/*
 * ahb_probe.c —— AHardwareBuffer + EGL 零拷贝硬件合成可行性探针（bionic 侧）
 *
 * 为什么需要：目前容器里的 GPU 路径是「渲染完 glReadPixels 回读 -> 写共享文件
 * -> App mmap 贴图」，瓶颈全在 glReadPixels（1080p 每帧 5.6ms，占 83%）。
 * AHardwareBuffer（AHB）+ EGL_ANDROID_get_native_client_buffer 能做到
 * GPU 直接写进共享缓冲、App 侧当纹理贴，一次 CPU 回读都不需要。
 * 这是 TAWC 用 libhybris 做的事里最关键的一环。
 *
 * 本探针验三件事：
 *   1) AHardwareBuffer_allocate 能不能成功（GPU 可渲染 + CPU 可读的格式）
 *   2) AHB 能不能经 eglGetNativeClientBufferANDROID 变成 EGLClientBuffer
 *   3) 渲染后 CPU lock 能不能读到正确像素（证明这块内存真的被 GPU 写了）
 * 不验 App 侧贴图（那需要 Kotlin 侧 HardwareRenderer / SurfaceTexture）。
 *
 * 关键实现约束（都是踩过的）：
 *   - 必须 dlopen + dlsym，不能静态链接 libEGL / libandroid
 *     （静态链接在 bionic 非 zygote 进程里触发 FORTIFY abort）
 *   - 输出必须用裸 write(1,...)，printf 同样会 abort
 *   - 必须经 sh -c '/ahb_probe' 触发，让容器内再发 execve 命中 bionic 直通
 *
 * 用法：/ahb_probe [width] [height]
 * 期望：RESULT: AHB_OK 或 RESULT: AHB_FAIL <原因>
 */
#include <dlfcn.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void P(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) write(1, b, (size_t)n);
}

/* AHardwareBuffer_Desc —— 照 AOSP hardware_buffer.h，字段顺序不能改 */
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

#define AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM 1
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN    (1ULL << 1)
#define AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE (1ULL << 8)
#define AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT  (1ULL << 9)

typedef int  (*AHB_allocate_t)(const AHBDesc *, void **);
typedef void (*AHB_describe_t)(const void *, AHBDesc *);
typedef void (*AHB_release_t)(void *);
/* ★AHardwareBuffer_lock 是 5 个参数：
 *   (buffer, usage, fence, const ARect* rect, void** outAddr)
 *   少声明 rect 时调用方按 4 参压栈，被调方从错误的寄存器/栈位取 rect，
 *   实测返回 -22(EINVAL)。这个错很隐蔽：编译不报错，只是永远锁不上。 */
typedef int  (*AHB_lock_t)(void *, unsigned long long, int, const void *, void **);
typedef int  (*AHB_unlock_t)(void *, int);

int main(int argc, char **argv) {
    int W = (argc > 1) ? atoi(argv[1]) : 640;
    int H = (argc > 2) ? atoi(argv[2]) : 360;

    /* ---- 1) AHardwareBuffer ---- */
    void *nw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!nw) nw = dlopen("libandroid.so", RTLD_NOW);
    if (!nw) {
        P("FAIL dlopen libnativewindow/libandroid: %s\n", dlerror());
        P("RESULT: AHB_FAIL dlopen\n");
        return 1;
    }
    P("nativewindow ok\n");

    AHB_allocate_t ahb_alloc = (AHB_allocate_t)dlsym(nw, "AHardwareBuffer_allocate");
    AHB_describe_t ahb_desc  = (AHB_describe_t)dlsym(nw, "AHardwareBuffer_describe");
    AHB_release_t  ahb_rel   = (AHB_release_t)dlsym(nw, "AHardwareBuffer_release");
    AHB_lock_t     ahb_lock  = (AHB_lock_t)dlsym(nw, "AHardwareBuffer_lock");
    AHB_unlock_t   ahb_unlk  = (AHB_unlock_t)dlsym(nw, "AHardwareBuffer_unlock");
    if (!ahb_alloc || !ahb_desc || !ahb_rel) {
        P("FAIL dlsym AHB core (alloc=%p desc=%p rel=%p)\n",
          (void *)ahb_alloc, (void *)ahb_desc, (void *)ahb_rel);
        P("RESULT: AHB_FAIL dlsym\n");
        return 2;
    }
    P("AHB symbols ok (lock=%p unlock=%p)\n", (void *)ahb_lock, (void *)ahb_unlk);

    /* ---- 2) 分配 AHB ---- */
    AHBDesc d;
    memset(&d, 0, sizeof d);
    d.width  = (unsigned)W;
    d.height = (unsigned)H;
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage  = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;

    void *ahb = 0;
    int rc = ahb_alloc(&d, &ahb);
    if (rc != 0 || !ahb) {
        P("FAIL AHardwareBuffer_allocate rc=%d ahb=%p\n", rc, ahb);
        P("RESULT: AHB_FAIL alloc\n");
        return 3;
    }
    P("AHB allocated %dx%d ahb=%p\n", W, H, ahb);

    AHBDesc got;
    memset(&got, 0, sizeof got);
    ahb_desc(ahb, &got);
    P("AHB describe: %ux%u fmt=0x%x usage=0x%llx stride=%u\n",
      got.width, got.height, got.format, got.usage, got.stride);

    /* ---- 3) EGL 初始化 ---- */
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) {
        P("FAIL dlopen libEGL: %s\n", dlerror());
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL egl_dlopen\n");
        return 4;
    }

    typedef void *(*getDisplay_t)(void *);
    typedef int   (*initialize_t)(void *, int *, int *);
    typedef const char *(*queryString_t)(void *, int);
    typedef int   (*bindAPI_t)(unsigned);
    typedef void *(*createContext_t)(void *, void *, void *, const int *);
    typedef int   (*makeCurrent_t)(void *, void *, void *, void *);
    typedef int   (*getError_t)(void);

    getDisplay_t    eglGetDisplay     = (getDisplay_t)dlsym(egl, "eglGetDisplay");
    initialize_t    eglInitialize     = (initialize_t)dlsym(egl, "eglInitialize");
    queryString_t   eglQueryString    = (queryString_t)dlsym(egl, "eglQueryString");
    getError_t      eglGetError       = (getError_t)dlsym(egl, "eglGetError");
    bindAPI_t       eglBindAPI        = (bindAPI_t)dlsym(egl, "eglBindAPI");
    createContext_t eglCreateContext  = (createContext_t)dlsym(egl, "eglCreateContext");
    makeCurrent_t   eglMakeCurrent    = (makeCurrent_t)dlsym(egl, "eglMakeCurrent");

    void *dp = eglGetDisplay((void *)0);
    int maj = 0, min = 0;
    if (!dp || !eglInitialize(dp, &maj, &min)) {
        P("FAIL eglInitialize display=%p err=0x%x\n",
          dp, eglGetError ? eglGetError() : 0);
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL egl_init\n");
        return 5;
    }
    P("EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dp, 0x3053));

    const char *ext = eglQueryString(dp, 0x3055);   /* EGL_EXTENSIONS */
    int has_ncb = (ext && strstr(ext, "EGL_ANDROID_get_native_client_buffer")) ? 1 : 0;
    P("EGL_ANDROID_get_native_client_buffer: %s\n", has_ncb ? "present" : "MISSING");

    /* ---- 4) GLES ---- */
    void *gl = dlopen("libGLESv2.so", RTLD_NOW);
    if (!gl) {
        P("FAIL dlopen libGLESv2\n");
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL gles\n");
        return 6;
    }
    typedef void (*viewport_t)(int, int, int, int);
    typedef void (*clearColor_t)(float, float, float, float);
    typedef void (*clear_t)(unsigned);
    typedef void (*finish_t)(void);
    typedef const unsigned char *(*getString_t)(unsigned);

    viewport_t   glViewport   = (viewport_t)dlsym(gl, "glViewport");
    clearColor_t glClearColor = (clearColor_t)dlsym(gl, "glClearColor");
    clear_t      glClear      = (clear_t)dlsym(gl, "glClear");
    finish_t     glFinish     = (finish_t)dlsym(gl, "glFinish");
    getString_t  glGetString  = (getString_t)dlsym(gl, "glGetString");

    eglBindAPI(0x30A0);   /* EGL_OPENGL_ES_API */
    /* ★必须先用 eglChooseConfig 拿 config，否则拿 NULL config 建 ctx 报
     *   EGL_BAD_CONFIG(0x3005)。这是第一次跑踩到的（见 pitfalls #40）。
     *
     * ★但 eglMakeCurrent 时**不能**带 config 相关的 surface —— surfaceless
     *   context 要求 EGL_NO_SURFACE，且 makeCurrent 的 read/draw 传 config
     *   会报 EGL_BAD_MATCH(0x3009)。所以 ctx 用 cfg 建、makeCurrent 传 NO_SURFACE。 */
    typedef int (*chooseConfig_t)(void *, const int *, void **, int, int *);
    chooseConfig_t eglChooseConfig = (chooseConfig_t)dlsym(egl, "eglChooseConfig");
    void *cfg = (void *)0;
    if (eglChooseConfig) {
        int cfgAttr[] = {
            0x3024 /*EGL_RED_SIZE*/,   8,
            0x3023 /*EGL_GREEN_SIZE*/, 8,
            0x3022 /*EGL_BLUE_SIZE*/,  8,
            0x3021 /*EGL_ALPHA_SIZE*/, 8,
            0x3040 /*EGL_SURFACE_TYPE*/, 0x0004 /*EGL_PBUFFER_BIT*/,
            0x3038, 0
        };
        int n = 0;
        if (eglChooseConfig(dp, cfgAttr, &cfg, 1, &n) && n > 0) {
            P("EGLConfig chosen n=%d cfg=%p\n", n, cfg);
        } else {
            P("eglChooseConfig failed err=0x%x, fall back to n=0\n",
              eglGetError ? eglGetError() : 0);
            cfg = (void *)0;
        }
    }

    int ctxAttr[] = { 0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, 2, 0x3038, 0 };
    void *ctx = eglCreateContext(dp, cfg, (void *)0, ctxAttr);
    if (!ctx) {
        P("FAIL eglCreateContext err=0x%x\n", eglGetError ? eglGetError() : 0);
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL ctx\n");
        return 7;
    }

    /* ★makeCurrent 必须绑一个**真实 surface**（pbuffer），不能 surfaceless。
     *   实测 surfaceless（read/draw 全 EGL_NO_SURFACE）报 EGL_BAD_MATCH(0x3009)，
     *   说明该 EGL 实现没开 EGL_KHR_surfaceless_context。
     *   framesrv.c 一直是 pbuffer 路线，所以它从没碰到这个问题。 */
    typedef void *(*createPbuffer_t)(void *, void *, const int *);
    typedef int   (*destroySurface_t)(void *, void *);
    createPbuffer_t eglCreatePbufferSurface =
        (createPbuffer_t)dlsym(egl, "eglCreatePbufferSurface");
    destroySurface_t eglDestroySurface = (destroySurface_t)dlsym(egl, "eglDestroySurface");
    void *surf = (void *)0;
    if (eglCreatePbufferSurface && cfg) {
        int pbAttr[] = { 0x3057 /*EGL_WIDTH*/, W, 0x3056 /*EGL_HEIGHT*/, H, 0x3038, 0 };
        surf = eglCreatePbufferSurface(dp, cfg, pbAttr);
        P("pbuffer surface = %p (err=0x%x)\n", surf, eglGetError ? eglGetError() : 0);
    }
    if (!eglMakeCurrent(dp, surf, surf, ctx)) {
        P("FAIL eglMakeCurrent err=0x%x surf=%p\n",
          eglGetError ? eglGetError() : 0, surf);
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL makecurrent\n");
        return 8;
    }
    P("GL_RENDERER=%s\n", (const char *)glGetString(0x1F01));
    P("GL_VERSION=%s\n", (const char *)glGetString(0x1F02));

    /* ---- 5) 关键：AHB -> EGLClientBuffer ---- */
    typedef void *(*getNativeClientBuffer_t)(const void *);
    getNativeClientBuffer_t eglGetNativeClientBufferANDROID =
        (getNativeClientBuffer_t)dlsym(egl, "eglGetNativeClientBufferANDROID");

    if (!eglGetNativeClientBufferANDROID) {
        P("FAIL eglGetNativeClientBufferANDROID symbol missing\n");
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL ncb_sym\n");
        return 9;
    }
    void *cbuf = eglGetNativeClientBufferANDROID(ahb);
    if (!cbuf) {
        P("FAIL eglGetNativeClientBufferANDROID returned NULL err=0x%x\n",
          eglGetError ? eglGetError() : 0);
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL ncb_null\n");
        return 10;
    }
    P("EGLClientBuffer from AHB = %p\n", cbuf);

    /* ---- 6) 关键第二步：AHB -> EGLImage -> GL texture -> FBO attachment ----
     *
     *  只把 AHB 变成 EGLClientBuffer 是不够的：渲染仍然落在当前绑定的
     *  pbuffer surface 上，AHB 那块内存一个字节都不会被写（实测 CPU lock
     *  读回 center RGBA = 0,0,0,0，这就是「以为通了其实没通」的典型）。
     *  必须把 AHB 包成 EGLImage、附到 GL texture、再挂成 FBO 的
     *  GL_COLOR_ATTACHMENT0，之后的绘制才真的写进 AHB。
     */
    typedef void *(*createImage_t)(void *, void *, unsigned, int *, const int *);
    typedef int   (*destroyImage_t)(void *, void *);
    typedef void  (*glBindTexture_t)(unsigned, unsigned);
    typedef void  (*glTexParameteri_t)(unsigned, unsigned, int);
    typedef void  (*glEGLImageTargetTexture2DOES_t)(unsigned, void *);
    typedef void  (*glGenTextures_t)(int, unsigned *);
    typedef void  (*glGenFramebuffers_t)(int, unsigned *);
    typedef void  (*glBindFramebuffer_t)(unsigned, unsigned);
    typedef void  (*glFramebufferTexture2D_t)(unsigned, unsigned, unsigned, unsigned, int);
    typedef unsigned (*glCheckFramebufferStatus_t)(unsigned);

    createImage_t  eglCreateImageKHR  = (createImage_t)dlsym(egl, "eglCreateImageKHR");
    destroyImage_t eglDestroyImageKHR = (destroyImage_t)dlsym(egl, "eglDestroyImageKHR");
    glGenTextures_t glGenTextures = (glGenTextures_t)dlsym(gl, "glGenTextures");
    glBindTexture_t glBindTexture = (glBindTexture_t)dlsym(gl, "glBindTexture");
    glTexParameteri_t glTexParameteri = (glTexParameteri_t)dlsym(gl, "glTexParameteri");
    glEGLImageTargetTexture2DOES_t glEGLImageTargetTexture2DOES =
        (glEGLImageTargetTexture2DOES_t)dlsym(gl, "glEGLImageTargetTexture2DOES");
    glGenFramebuffers_t glGenFramebuffers = (glGenFramebuffers_t)dlsym(gl, "glGenFramebuffers");
    glBindFramebuffer_t glBindFramebuffer = (glBindFramebuffer_t)dlsym(gl, "glBindFramebuffer");
    glFramebufferTexture2D_t glFramebufferTexture2D =
        (glFramebufferTexture2D_t)dlsym(gl, "glFramebufferTexture2D");
    glCheckFramebufferStatus_t glCheckFramebufferStatus =
        (glCheckFramebufferStatus_t)dlsym(gl, "glCheckFramebufferStatus");

    if (!eglCreateImageKHR) {
        P("FAIL eglCreateImageKHR symbol missing\n");
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL image_sym\n");
        return 11;
    }
    if (!glEGLImageTargetTexture2DOES) {
        P("FAIL glEGLImageTargetTexture2DOES symbol missing\n");
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL glimg_sym\n");
        return 12;
    }

    /* EGL_IMAGE_PRESERVED_KHR=0x30D2, EGL_NONE=0x3038
     * target 必须用 EGL_NATIVE_BUFFER_ANDROID=0x3140（不是 EGL_GL_TEXTURE_2D） */
    int imgAttr[] = { 0x30D2, 1, 0x3038, 0 };
    void *img = eglCreateImageKHR(dp, (void *)0, 0x3140, cbuf, imgAttr);
    if (!img) {
        P("FAIL eglCreateImageKHR err=0x%x\n", eglGetError ? eglGetError() : 0);
        ahb_rel(ahb);
        P("RESULT: AHB_FAIL image_create\n");
        return 13;
    }
    P("EGLImage from AHB = %p\n", img);

    unsigned tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(0x0DE1 /*GL_TEXTURE_2D*/, tex);
    glTexParameteri(0x0DE1, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, 0x2601 /*LINEAR*/);
    glTexParameteri(0x0DE1, 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2601);
    glEGLImageTargetTexture2DOES(0x0DE1, img);
    P("texture=%u bound to EGLImage\n", tex);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, fbo);
    glFramebufferTexture2D(0x8D40, 0x8CE0 /*GL_COLOR_ATTACHMENT0*/, 0x0DE1, tex, 0);
    unsigned fbs = glCheckFramebufferStatus ? glCheckFramebufferStatus(0x8D40) : 0;
    P("FBO=%u status=0x%x (0x8CD5=COMPLETE)\n", fbo, fbs);

    /* ---- 7) 渲染进 AHB，再从 CPU 读回中心像素 ---- */
    glViewport(0, 0, W, H);
    glClearColor(1.0f, 0.35f, 0.1f, 1.0f);
    glClear(0x4000 /*GL_COLOR_BUFFER_BIT*/);
    glFinish();
    P("cleared into AHB\n");

    if (ahb_lock && ahb_unlk) {
        void *addr = 0;
        /* ★lock 的 usage 必须与申请时的 CPU_READ_* 一致，否则 EINVAL(-22)。
         *   常量：NEVER=0, RARELY=1<<0(0x1), OFTEN=1<<1(0x2)。
         *   本探针申请的是 OFTEN，所以这里必须传 0x2 —— 传 0x1 实测 rc=-22。 */
        int lrc = ahb_lock(ahb, 0x2 /*AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN*/, -1, (const void *)0, &addr);
        if (lrc == 0 && addr) {
            unsigned char *px = (unsigned char *)addr;
            unsigned st = got.stride ? got.stride : (unsigned)(W * 4);
            unsigned off = (unsigned)(H / 2) * st + (unsigned)(W / 2) * 4;
            P("center RGBA = %u,%u,%u,%u\n",
              px[off], px[off + 1], px[off + 2], px[off + 3]);
            ahb_unlk(ahb, 0);
        } else {
            P("lock failed rc=%d addr=%p\n", lrc, addr);
        }
    } else {
        P("lock/unlock symbols missing, skip CPU readback\n");
    }

    /* ---- 8) 量化：渲染 N 帧进 AHB，测每帧耗时 ----
     *
     *  对照基准（benchmark 第六节，1080p）：
     *    render+finish  1.125 ms
     *    glReadPixels   5.630 ms   <- 占 83% 的瓶颈
     *    write          0.014 ms
     *  本路径没有 glReadPixels 那一步（GPU 直接写 AHB，CPU 只 lock 指针）。
     */
    {
        int N = 100;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++) {
            glClearColor((float)(i % 10) / 10.0f, 0.35f, 0.1f, 1.0f);
            glClear(0x4000);
        }
        glFinish();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0
                     + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) / (double)N;
        P("render into AHB: %.3f ms/frame (%d frames)\n", ms, N);

        /* lock 本身的开销（App 只在需要时锁，不是每帧） */
        struct timespec l0, l1;
        void *a2 = 0;
        clock_gettime(CLOCK_MONOTONIC, &l0);
        int lr2 = ahb_lock ? ahb_lock(ahb, 0x2, -1, (const void *)0, &a2) : -1;
        clock_gettime(CLOCK_MONOTONIC, &l1);
        if (lr2 == 0 && a2) {
            double lms = ((double)(l1.tv_sec - l0.tv_sec) * 1000.0
                          + (double)(l1.tv_nsec - l0.tv_nsec) / 1e6);
            P("AHB lock: %.3f ms\n", lms);
            if (ahb_unlk) ahb_unlk(ahb, 0);
        }
    }

    if (eglDestroyImageKHR && img) eglDestroyImageKHR(dp, img);
    ahb_rel(ahb);
    P("RESULT: AHB_OK\n");
    return 0;
}
