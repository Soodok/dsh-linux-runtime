/*
 * ahb_xproc_probe.c —— AHB 跨进程零拷贝验证（bionic 侧）
 *
 * 上一轮（ahb_test.c）已证明：容器内可以把 AHB 挂成 FBO 让 GPU 直接渲染进去，
 * 1080p 0.341 ms/帧（比 glReadPixels 路径快 19.9 倍）。
 *
 * 但那还是**同进程内**。真产品要的是：容器进程渲染 → App 进程显示。
 * 所以必须验证 AHB 的 native_handle 能不能跨进程传：
 *
 *   进程 A（本程序 --producer）：
 *     AHardwareBuffer_allocate
 *     -> AHardwareBuffer_getNativeHandle(ahb, &handle)
 *     -> 把 handle->data[0]（是 dmabuf fd）dup 成可继承的 fd
 *     -> 用 SCM_RIGHTS 或直接让父进程继承，交给进程 B
 *
 *   进程 B（本程序 --consumer <fd>）：
 *     -> AHardwareBuffer_createFromHandle(handle, ...)
 *     -> AHardwareBuffer_lock 读像素，确认看到 A 渲染的内容
 *
 * Android 上 AHB 的 native_handle 通常只有 1 个 fd（gralloc dmabuf），
 * 少数实现有 2~3 个。本探针不假设几个，按 handle->numFds 逐个传。
 *
 * 用法（自测模式，一个进程内 fork）：
 *   /ahb_xproc_probe           # 自己 fork 一个子进程做 consumer
 *   /ahb_xproc_probe 1280 720  # 指定分辨率
 *
 * 期望：RESULT: AHB_XPROC_OK
 */
#include <dlfcn.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>

static void P(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) write(1, b, (size_t)n);
}

typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned int layers;
    unsigned int format;
    unsigned long long usage;
    unsigned int stride;
    unsigned int rfu0;
    unsigned long long rfu1;
} AHBDesc;

/* native_handle_t：AOSP 定义，字段顺序不能改 */
typedef struct {
    int version;          /* sizeof(native_handle_t) */
    int numFds;
    int numInts;
    int data[0];          /* 前 numFds 个是 fd，后 numInts 个是 int */
} NativeHandle;

#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN    (1ULL << 1)
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN   (1ULL << 3)
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT  (1ULL << 9)

typedef int  (*AHB_allocate_t)(const AHBDesc *, void **);
typedef void (*AHB_describe_t)(const void *, AHBDesc *);
typedef void (*AHB_release_t)(void *);
typedef int  (*AHB_lock_t)(void *, unsigned long long, int, const void *, void **);
typedef int  (*AHB_unlock_t)(void *, int);
typedef const NativeHandle *(*AHB_getNativeHandle_t)(const void *);
/* ★AHardwareBuffer_createFromHandle 是 **4 个参数**：
 *   (const AHardwareBuffer_Desc* desc, const native_handle_t* handle,
 *    int32_t method, AHardwareBuffer** outBuffer)
 *   漏掉 desc 时调用方按 3 参压栈，被调方从 handle 位置读 desc → -22(EINVAL)。
 *   与 AHardwareBuffer_lock 漏 rect 是同一类错（见 pitfalls #40 坑3）。 */
typedef int  (*AHB_createFromHandle_t)(const AHBDesc *, const NativeHandle *, int, void **);

/* 全局，供 fork 后子进程使用 */
static void *g_nw, *g_egl, *g_gl;
static AHB_createFromHandle_t p_createFromHandle;
static AHB_lock_t p_lock;
static AHB_unlock_t p_unlock;
static AHB_release_t p_release;
static AHB_describe_t p_describe;
static void *g_dp;                 /* EGLDisplay，fork 后可复用 */

/* 子进程：从 fd 重建 AHB，读中心像素 */
static int consumer(const NativeHandle *nh, int W, int H) {
    if (!p_createFromHandle) {
        P("[child] FAIL createFromHandle missing\n");
        return 20;
    }
    /* 用 alloca 造一个同布局的 handle 传给 createFromHandle。
     *
     * ★★这里踩过一个很隐蔽的坑：`native_handle_t.version` 的值是
     *   sizeof(native_handle_t) = **12**（只是结构体头，不含 data[]），
     *   **不是**整个 handle 的字节数。按 version 复制只拷 12 字节，
     *   numFds 个 fd 全部丢掉 → createFromHandle 返回 -22(EINVAL)。
     *   正确长度 = sizeof(NativeHandle) + (numFds + numInts) * sizeof(int)
     *   实测 version=12 / numFds=2 / numInts=21 → 12 + 23*4 = 104 字节。 */
    size_t hsz = sizeof(NativeHandle)
               + (size_t)(nh->numFds + nh->numInts) * sizeof(int);
    NativeHandle *copy = (NativeHandle *)alloca(hsz);
    memset(copy, 0, hsz);
    memcpy(copy, nh, hsz);
    P("[child] handle copy %u bytes (version=%d numFds=%d numInts=%d)\n",
      (unsigned)hsz, nh->version, nh->numFds, nh->numInts);

    void *ahb2 = 0;
    AHBDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.width  = (unsigned)W;
    desc.height = (unsigned)H;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                  AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                  AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    int rc = p_createFromHandle(&desc, copy,
                                1 /*AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE*/,
                                &ahb2);
    if (rc != 0 || !ahb2) {
        P("[child] FAIL createFromHandle rc=%d ahb=%p\n", rc, ahb2);
        return 21;
    }
    P("[child] AHB rebuilt from handle = %p\n", ahb2);

    AHBDesc d2;
    memset(&d2, 0, sizeof d2);
    if (p_describe) p_describe(ahb2, &d2);
    P("[child] describe %ux%u fmt=0x%x stride=%u\n", d2.width, d2.height, d2.format, d2.stride);

    if (p_lock && p_unlock) {
        void *addr = 0;
        int lr = p_lock(ahb2, 0x2 /*CPU_READ_OFTEN*/, -1, (const void *)0, &addr);
        if (lr == 0 && addr) {
            unsigned char *px = (unsigned char *)addr;
            unsigned st = d2.stride ? d2.stride : (unsigned)(W * 4);
            unsigned off = (unsigned)(H / 2) * st + (unsigned)(W / 2) * 4;
            P("[child] center RGBA = %u,%u,%u,%u\n",
              px[off], px[off + 1], px[off + 2], px[off + 3]);
            p_unlock(ahb2, 0);
            if (px[off] > 200 && px[off + 1] > 60 && px[off + 1] < 120) {
                P("[child] PIXEL MATCHES producer clear color\n");
                if (p_release) p_release(ahb2);
                return 0;
            }
            P("[child] PIXEL MISMATCH\n");
            if (p_release) p_release(ahb2);
            return 22;
        }
        P("[child] lock failed rc=%d\n", lr);
    }
    if (p_release) p_release(ahb2);
    return 23;
}

int main(int argc, char **argv) {
    int W = (argc > 1) ? atoi(argv[1]) : 640;
    int H = (argc > 2) ? atoi(argv[2]) : 360;

    /* ---- 载入符号 ---- */
    g_nw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!g_nw) g_nw = dlopen("libandroid.so", RTLD_NOW);
    if (!g_nw) { P("FAIL dlopen nativewindow: %s\n", dlerror()); P("RESULT: AHB_XPROC_FAIL nw\n"); return 1; }

    AHB_allocate_t p_alloc = (AHB_allocate_t)dlsym(g_nw, "AHardwareBuffer_allocate");
    p_describe  = (AHB_describe_t)dlsym(g_nw, "AHardwareBuffer_describe");
    p_release   = (AHB_release_t)dlsym(g_nw, "AHardwareBuffer_release");
    p_lock      = (AHB_lock_t)dlsym(g_nw, "AHardwareBuffer_lock");
    p_unlock    = (AHB_unlock_t)dlsym(g_nw, "AHardwareBuffer_unlock");
    AHB_getNativeHandle_t p_getHandle =
        (AHB_getNativeHandle_t)dlsym(g_nw, "AHardwareBuffer_getNativeHandle");
    p_createFromHandle = (AHB_createFromHandle_t)dlsym(g_nw, "AHardwareBuffer_createFromHandle");

    if (!p_alloc || !p_getHandle) {
        P("FAIL dlsym (alloc=%p getHandle=%p)\n", (void *)p_alloc, (void *)p_getHandle);
        P("RESULT: AHB_XPROC_FAIL dlsym\n"); return 2;
    }
    P("symbols ok: getNativeHandle=%p createFromHandle=%p\n",
      (void *)p_getHandle, (void *)p_createFromHandle);

    /* ---- producer: 分配 + 渲染 ---- */
    AHBDesc d;
    memset(&d, 0, sizeof d);
    d.width = (unsigned)W; d.height = (unsigned)H; d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
              AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
              AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    void *ahb = 0;
    int rc = p_alloc(&d, &ahb);
    if (rc != 0 || !ahb) { P("FAIL allocate rc=%d\n", rc); P("RESULT: AHB_XPROC_FAIL alloc\n"); return 3; }
    P("[parent] AHB allocated %dx%d\n", W, H);

    /* EGL 渲染进去（同 ahb_test 已验证的四步链路） */
    g_egl = dlopen("libEGL.so", RTLD_NOW);
    g_gl  = dlopen("libGLESv2.so", RTLD_NOW);
    if (!g_egl || !g_gl) { P("FAIL dlopen egl/gl\n"); P("RESULT: AHB_XPROC_FAIL egl\n"); return 4; }

    typedef void *(*getDisplay_t)(void *);
    typedef int (*init_t)(void *, int *, int *);
    typedef int (*bindAPI_t)(unsigned);
    typedef int (*chooseConfig_t)(void *, const int *, void **, int, int *);
    typedef void *(*createContext_t)(void *, void *, void *, const int *);
    typedef int (*makeCurrent_t)(void *, void *, void *, void *);
    typedef void *(*createPbuffer_t)(void *, void *, const int *);

    void *dp = ((getDisplay_t)dlsym(g_egl, "eglGetDisplay"))((void *)0);
    g_dp = dp;
    int maj = 0, min = 0;
    ((init_t)dlsym(g_egl, "eglInitialize"))(dp, &maj, &min);
    ((bindAPI_t)dlsym(g_egl, "eglBindAPI"))(0x30A0);

    int cfgAttr[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 0x0004, 0x3038, 0 };
    void *cfg = (void *)0; int n = 0;
    ((chooseConfig_t)dlsym(g_egl, "eglChooseConfig"))(dp, cfgAttr, &cfg, 1, &n);
    int ctxAttr[] = { 0x3098, 2, 0x3038, 0 };
    void *ctx = ((createContext_t)dlsym(g_egl, "eglCreateContext"))(dp, cfg, (void *)0, ctxAttr);
    int pbAttr[] = { 0x3057, W, 0x3056, H, 0x3038, 0 };
    void *surf = ((createPbuffer_t)dlsym(g_egl, "eglCreatePbufferSurface"))(dp, cfg, pbAttr);
    if (!((makeCurrent_t)dlsym(g_egl, "eglMakeCurrent"))(dp, surf, surf, ctx)) {
        P("FAIL makeCurrent\n"); P("RESULT: AHB_XPROC_FAIL makecurrent\n"); return 5;
    }

    typedef void *(*getNCB_t)(const void *);
    typedef void *(*createImage_t)(void *, void *, unsigned, int *, const int *);
    typedef void (*glGen_t)(int, unsigned *);
    typedef void (*glBindTex_t)(unsigned, unsigned);
    typedef void (*glTexParam_t)(unsigned, unsigned, int);
    typedef void (*glImgTex_t)(unsigned, void *);
    typedef void (*glBindFbo_t)(unsigned, unsigned);
    typedef void (*glFboTex_t)(unsigned, unsigned, unsigned, unsigned, int);
    typedef void (*viewport_t)(int, int, int, int);
    typedef void (*clearColor_t)(float, float, float, float);
    typedef void (*clear_t)(unsigned);
    typedef void (*finish_t)(void);

    void *cbuf = ((getNCB_t)dlsym(g_egl, "eglGetNativeClientBufferANDROID"))(ahb);
    if (!cbuf) { P("FAIL getNativeClientBuffer\n"); P("RESULT: AHB_XPROC_FAIL ncb\n"); return 6; }

    int imgAttr[] = { 0x30D2, 1, 0x3038, 0 };
    void *img = ((createImage_t)dlsym(g_egl, "eglCreateImageKHR"))(
        dp, (void *)0, 0x3140, cbuf, imgAttr);
    if (!img) { P("FAIL createImage\n"); P("RESULT: AHB_XPROC_FAIL image\n"); return 7; }

    unsigned tex = 0, fbo = 0;
    ((glGen_t)dlsym(g_gl, "glGenTextures"))(1, &tex);
    ((glBindTex_t)dlsym(g_gl, "glBindTexture"))(0x0DE1, tex);
    ((glTexParam_t)dlsym(g_gl, "glTexParameteri"))(0x0DE1, 0x2801, 0x2601);
    ((glImgTex_t)dlsym(g_gl, "glEGLImageTargetTexture2DOES"))(0x0DE1, img);
    ((glGen_t)dlsym(g_gl, "glGenFramebuffers"))(1, &fbo);
    ((glBindFbo_t)dlsym(g_gl, "glBindFramebuffer"))(0x8D40, fbo);
    ((glFboTex_t)dlsym(g_gl, "glFramebufferTexture2D"))(0x8D40, 0x8CE0, 0x0DE1, tex, 0);

    ((viewport_t)dlsym(g_gl, "glViewport"))(0, 0, W, H);
    ((clearColor_t)dlsym(g_gl, "glClearColor"))(1.0f, 0.35f, 0.1f, 1.0f);
    ((clear_t)dlsym(g_gl, "glClear"))(0x4000);
    ((finish_t)dlsym(g_gl, "glFinish"))();
    P("[parent] rendered into AHB\n");

    /* ---- 拿 native handle ---- */
    const NativeHandle *nh = p_getHandle(ahb);
    if (!nh) { P("FAIL getNativeHandle NULL\n"); P("RESULT: AHB_XPROC_FAIL nh\n"); return 8; }
    P("[parent] native_handle version=%d numFds=%d numInts=%d\n",
      nh->version, nh->numFds, nh->numInts);
    if (nh->numFds <= 0) {
        P("FAIL handle has no fds (纯 CPU 内存，跨进程无意义)\n");
        P("RESULT: AHB_XPROC_FAIL nofd\n"); return 9;
    }
    for (int i = 0; i < nh->numFds; i++) {
        P("[parent]   fd[%d] = %d\n", i, nh->data[i]);
    }

    /* ---- 先做同进程自测：隔离「handle 内容问题」与「跨进程继承问题」 ----
     *
     * 这一步很重要：如果同一进程里 createFromHandle 也失败，说明是 desc 或
     * handle 布局的问题；如果同进程成功、跨进程失败，才轮到 fd 继承/SELinux。
     * 不分这两层就会一直在错误的方向上调参（本轮已经踩过）。
     */
    {
        size_t hsz0 = sizeof(NativeHandle)
                    + (size_t)(nh->numFds + nh->numInts) * sizeof(int);
        NativeHandle *c0 = (NativeHandle *)alloca(hsz0);
        memcpy(c0, nh, hsz0);

        /* ★desc 必须用 describe() 回读的**实际** desc，不能用申请时那份：
         *   申请时 stride 我没填（=0），而 createFromHandle 会把 desc 与 handle
         *   里的实际布局比对，stride 不符即 EINVAL(-22)。 */
        AHBDesc real;
        memset(&real, 0, sizeof real);
        if (p_describe) p_describe(ahb, &real);
        P("[selfcheck] real describe: %ux%u fmt=0x%x usage=0x%llx stride=%u\n",
          real.width, real.height, real.format, real.usage, real.stride);

        void *self = 0;
        int src = p_createFromHandle(&real, c0, 1 /*CLONE*/, &self);
        P("[selfcheck] createFromHandle(real desc, CLONE) rc=%d ahb=%p\n", src, self);
        if (src != 0) {
            src = p_createFromHandle(&real, c0, 0 /*REFERENCE*/, &self);
            P("[selfcheck] createFromHandle(real desc, REFERENCE) rc=%d ahb=%p\n", src, self);
        }
        if (src != 0) {
            /* 再退一步：desc 全零，让底层自己从 handle 推断 */
            AHBDesc zero;
            memset(&zero, 0, sizeof zero);
            src = p_createFromHandle(&zero, c0, 1, &self);
            P("[selfcheck] createFromHandle(zero desc, CLONE) rc=%d ahb=%p\n", src, self);
        }
        if (src == 0 && self && p_release) p_release(self);
    }

    /* ---- fork 出 consumer，让它继承 fd ---- */
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：fd 已由 fork 继承 */
        int r = consumer(nh, W, H);
        _exit(r);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    P("[parent] child exit=%d\n", code);

    if (p_release) p_release(ahb);
    if (code == 0) { P("RESULT: AHB_XPROC_OK\n"); return 0; }
    P("RESULT: AHB_XPROC_FAIL child=%d\n", code);
    return 30 + (code < 0 ? 0 : (code % 10));
}
