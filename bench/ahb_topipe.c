/*
 * ahb_topipe.c —— AHB 渲染 + lock + memcpy 写共享文件（不依赖 createFromHandle）
 *
 * 背景：
 *   完整零拷贝（AHB 直接交给 App 的 SurfaceTexture）需要 AHardwareBuffer_createFromHandle，
 *   但模拟器上实测该方法一律返回 -22(EINVAL)，根因是 gralloc allocator 没进 VINTF manifest：
 *       servicemanager: Could not find android.hardware.graphics.allocator.IAllocator/default
 *                       in the VINTF manifest.
 *       Gralloc4: mapper 4.x is not supported
 *   同进程自测也失败、三种 desc（原始 / describe 回读 / 全零）× 两种 method（CLONE /
 *   REFERENCE）全部 -22 → 是环境级拒绝，不是用法问题。
 *
 * 所以退一步：AHB 仍然用来**避免 glReadPixels**，但产物靠 lock 拿指针后 memcpy
 * 到共享文件（App 侧仍是原来的 mmap 路径，零改动）。
 *
 *   旧：render 1.125 + glReadPixels 5.630 + write 0.014 = 6.777 ms/帧
 *   新：render 0.341（AHB） + lock/memcpy 到文件 = ? ms/帧
 *
 * 用法：
 *   /ahb_topipe <sharepath> [W] [H] [N]
 * 期望：RESULT: AHB_TOPIPE_OK
 */
#include <dlfcn.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static void P(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) write(1, b, (size_t)n);
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

typedef struct {
    unsigned int width, height, layers, format;
    unsigned long long usage;
    unsigned int stride, rfu0;
    unsigned long long rfu1;
} AHBDesc;

#define FMT_RGBA8888 1
#define USAGE_CPU_READ_OFTEN   (1ULL << 1)
#define USAGE_CPU_WRITE_OFTEN  (1ULL << 3)
#define USAGE_GPU_SAMPLED      (1ULL << 8)
#define USAGE_GPU_COLOR_OUT    (1ULL << 9)

typedef int  (*AHB_allocate_t)(const AHBDesc *, void **);
typedef void (*AHB_describe_t)(const void *, AHBDesc *);
typedef void (*AHB_release_t)(void *);
typedef int  (*AHB_lock_t)(void *, unsigned long long, int, const void *, void **);
typedef int  (*AHB_unlock_t)(void *, int);

int main(int argc, char **argv) {
    const char *share = (argc > 1) ? argv[1] : "/tmp/ahbframe.bin";
    int W = (argc > 2) ? atoi(argv[2]) : 1280;
    int H = (argc > 3) ? atoi(argv[3]) : 720;
    int N = (argc > 4) ? atoi(argv[4]) : 60;

    /* ---- 共享文件（与 App 的契约：16 字节头 + RGBA）---- */
    unsigned char *sharemem = MAP_FAILED;
    int sfd = -1;
    size_t framebytes = (size_t)W * (size_t)H * 4;
    size_t total = 16 + framebytes;
    {
        size_t page = 4096;
        size_t rounded = (total + page - 1) / page * page;
        sfd = open(share, O_RDWR | O_CREAT, 0666);
        if (sfd < 0) { P("FAIL open %s\n", share); P("RESULT: AHB_TOPIPE_FAIL open\n"); return 1; }
        if (ftruncate(sfd, (off_t)rounded) != 0) {
            P("FAIL ftruncate\n"); P("RESULT: AHB_TOPIPE_FAIL trunc\n"); return 1;
        }
        sharemem = (unsigned char *)mmap((void *)0, rounded, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, sfd, 0);
        if (sharemem == MAP_FAILED) {
            P("FAIL mmap\n"); P("RESULT: AHB_TOPIPE_FAIL mmap\n"); return 1;
        }
        /* magic 'FRAM' little-endian, seq, w, h */
        sharemem[0] = 0x46; sharemem[1] = 0x52; sharemem[2] = 0x41; sharemem[3] = 0x4D;
        unsigned int *hdr = (unsigned int *)sharemem;
        hdr[2] = (unsigned)W;
        hdr[3] = (unsigned)H;
        P("share file %s size=%zu (header 16 + frame %zu)\n", share, rounded, framebytes);
    }

    /* ---- AHB ---- */
    void *nw = dlopen("libnativewindow.so", RTLD_NOW);
    if (!nw) nw = dlopen("libandroid.so", RTLD_NOW);
    if (!nw) { P("FAIL dlopen nw: %s\n", dlerror()); P("RESULT: AHB_TOPIPE_FAIL nw\n"); return 2; }
    AHB_allocate_t p_alloc = (AHB_allocate_t)dlsym(nw, "AHardwareBuffer_allocate");
    AHB_describe_t p_describe = (AHB_describe_t)dlsym(nw, "AHardwareBuffer_describe");
    AHB_release_t  p_release  = (AHB_release_t)dlsym(nw, "AHardwareBuffer_release");
    AHB_lock_t     p_lock     = (AHB_lock_t)dlsym(nw, "AHardwareBuffer_lock");
    AHB_unlock_t   p_unlock   = (AHB_unlock_t)dlsym(nw, "AHardwareBuffer_unlock");
    if (!p_alloc || !p_lock) { P("FAIL dlsym ahb\n"); P("RESULT: AHB_TOPIPE_FAIL dlsym\n"); return 2; }

    AHBDesc d;
    memset(&d, 0, sizeof d);
    d.width = (unsigned)W; d.height = (unsigned)H; d.layers = 1;
    d.format = FMT_RGBA8888;
    d.usage = USAGE_GPU_SAMPLED | USAGE_GPU_COLOR_OUT | USAGE_CPU_READ_OFTEN;
    void *ahb = 0;
    if (p_alloc(&d, &ahb) != 0 || !ahb) {
        P("FAIL allocate\n"); P("RESULT: AHB_TOPIPE_FAIL alloc\n"); return 3;
    }
    AHBDesc real; memset(&real, 0, sizeof real);
    if (p_describe) p_describe(ahb, &real);
    P("AHB %ux%u stride=%u usage=0x%llx\n", real.width, real.height, real.stride, real.usage);

    /* ---- EGL + FBO ---- */
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    void *gl  = dlopen("libGLESv2.so", RTLD_NOW);
    if (!egl || !gl) { P("FAIL dlopen egl/gl\n"); P("RESULT: AHB_TOPIPE_FAIL egl\n"); return 4; }

    typedef void *(*getDisplay_t)(void *);
    typedef int (*init_t)(void *, int *, int *);
    typedef int (*bindAPI_t)(unsigned);
    typedef int (*chooseConfig_t)(void *, const int *, void **, int, int *);
    typedef void *(*createContext_t)(void *, void *, void *, const int *);
    typedef int (*makeCurrent_t)(void *, void *, void *, void *);
    typedef void *(*createPbuffer_t)(void *, void *, const int *);
    typedef void *(*getNCB_t)(const void *);
    typedef void *(*createImage_t)(void *, void *, unsigned, int *, const int *);
    typedef void (*glGen_t)(int, unsigned *);
    typedef void (*glBindTex_t)(unsigned, unsigned);
    typedef void (*glTexParam_t)(unsigned, unsigned, int);
    typedef void (*glImgTex_t)(unsigned, void *);
    typedef void (*glBindFbo_t)(unsigned, unsigned);
    typedef void (*glFboTex_t)(unsigned, unsigned, unsigned, unsigned, int);
    typedef unsigned (*glCheckFbo_t)(unsigned);
    typedef void (*viewport_t)(int, int, int, int);
    typedef void (*clearColor_t)(float, float, float, float);
    typedef void (*clear_t)(unsigned);
    typedef void (*finish_t)(void);

    void *dp = ((getDisplay_t)dlsym(egl, "eglGetDisplay"))((void *)0);
    int maj = 0, min = 0;
    ((init_t)dlsym(egl, "eglInitialize"))(dp, &maj, &min);
    ((bindAPI_t)dlsym(egl, "eglBindAPI"))(0x30A0);

    int cfgAttr[] = { 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 0x0004, 0x3038, 0 };
    void *cfg = (void *)0; int ncfg = 0;
    ((chooseConfig_t)dlsym(egl, "eglChooseConfig"))(dp, cfgAttr, &cfg, 1, &ncfg);
    if (!cfg) { P("FAIL chooseConfig\n"); P("RESULT: AHB_TOPIPE_FAIL cfg\n"); return 5; }

    int ctxAttr[] = { 0x3098, 2, 0x3038, 0 };
    void *ctx = ((createContext_t)dlsym(egl, "eglCreateContext"))(dp, cfg, (void *)0, ctxAttr);
    int pbAttr[] = { 0x3057, W, 0x3056, H, 0x3038, 0 };
    void *surf = ((createPbuffer_t)dlsym(egl, "eglCreatePbufferSurface"))(dp, cfg, pbAttr);
    if (!((makeCurrent_t)dlsym(egl, "eglMakeCurrent"))(dp, surf, surf, ctx)) {
        P("FAIL makeCurrent\n"); P("RESULT: AHB_TOPIPE_FAIL makecurrent\n"); return 6;
    }

    void *cbuf = ((getNCB_t)dlsym(egl, "eglGetNativeClientBufferANDROID"))(ahb);
    int imgAttr[] = { 0x30D2, 1, 0x3038, 0 };
    void *img = cbuf
        ? ((createImage_t)dlsym(egl, "eglCreateImageKHR"))(dp, (void *)0, 0x3140, cbuf, imgAttr)
        : (void *)0;
    if (!img) { P("FAIL createImage\n"); P("RESULT: AHB_TOPIPE_FAIL image\n"); return 7; }

    unsigned tex = 0, fbo = 0;
    ((glGen_t)dlsym(gl, "glGenTextures"))(1, &tex);
    ((glBindTex_t)dlsym(gl, "glBindTexture"))(0x0DE1, tex);
    ((glTexParam_t)dlsym(gl, "glTexParameteri"))(0x0DE1, 0x2801, 0x2601);
    ((glImgTex_t)dlsym(gl, "glEGLImageTargetTexture2DOES"))(0x0DE1, img);
    ((glGen_t)dlsym(gl, "glGenFramebuffers"))(1, &fbo);
    ((glBindFbo_t)dlsym(gl, "glBindFramebuffer"))(0x8D40, fbo);
    ((glFboTex_t)dlsym(gl, "glFramebufferTexture2D"))(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
    unsigned fbs = ((glCheckFbo_t)dlsym(gl, "glCheckFramebufferStatus"))(0x8D40);
    P("FBO status=0x%x (0x8CD5=COMPLETE)\n", fbs);
    if (fbs != 0x8CD5) { P("RESULT: AHB_TOPIPE_FAIL fbo\n"); return 8; }

    /* ---- 同进程 A/B 对照 ----
     *
     * ★这一步是必须的：如果只测 AHB 路径，会得到「1080p 7.095 ms」，然后拿它跟
     *   上一轮记录的「glReadPixels 5.630 ms」比 —— 但那是两个不同探针、不同批次的
     *   数字，不可比。必须在**同一个二进制、同一轮循环、同一 EGL 上下文**下把两条
     *   路径都跑一遍，否则很容易得出「AHB 更慢 5%」或「AHB 快 20 倍」这种反向结论
     *   （我这轮已经犯过错：拿「只渲染 0.341ms」比「渲染+回读 6.777ms」，算出
     *   19.9 倍，实际是拿苹果比橘子）。
     *
     *   A 路径：render into AHB -> AHB_lock -> memcpy 到共享文件 -> unlock
     *   B 路径：render 到 fbo -> glReadPixels -> memcpy 到共享文件
     */
    /* 为了 B 路径，把 AHB 从 FBO 上摘掉，换成一个普通 texture，这样
       glReadPixels 读的是普通 FBO 而不是 AHB（否则 AHB 的 lock 与
       glReadPixels 会互相干扰）。 */
    /* glReadPixels 是 7 个参数：(x, y, w, h, format, type, void* pixels) */
    typedef void (*glReadPixels_t)(int, int, int, int, unsigned, unsigned, void *);
    glReadPixels_t p_readpixels = (glReadPixels_t)dlsym(gl, "glReadPixels");

    static unsigned char *rp_buf = 0;
    rp_buf = (unsigned char *)malloc(framebytes);
    if (!rp_buf) { P("FAIL malloc rp_buf\n"); P("RESULT: AHB_TOPIPE_FAIL malloc\n"); return 10; }

    /* 建一个普通 texture surface 给 B 路径 */
    unsigned tex2 = 0, fbo2 = 0;
    typedef void (*glTexImage2D_t)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
    glTexImage2D_t p_teximage = (glTexImage2D_t)dlsym(gl, "glTexImage2D");
    ((glGen_t)dlsym(gl, "glGenTextures"))(1, &tex2);
    ((glBindTex_t)dlsym(gl, "glBindTexture"))(0x0DE1, tex2);
    ((glTexParam_t)dlsym(gl, "glTexParameteri"))(0x0DE1, 0x2801, 0x2601);
    ((glTexParam_t)dlsym(gl, "glTexParameteri"))(0x0DE1, 0x2800, 0x2601);
    if (p_teximage) p_teximage(0x0DE1, 0, 0x1908 /*GL_RGBA*/, W, H, 0, 0x1908, 0x1401 /*UNSIGNED_BYTE*/, 0);
    ((glGen_t)dlsym(gl, "glGenFramebuffers"))(1, &fbo2);
    ((glBindFbo_t)dlsym(gl, "glBindFramebuffer"))(0x8D40, fbo2);
    ((glFboTex_t)dlsym(gl, "glFramebufferTexture2D"))(0x8D40, 0x8CE0, 0x0DE1, tex2, 0);
    unsigned fbs2 = ((glCheckFbo_t)dlsym(gl, "glCheckFramebufferStatus"))(0x8D40);
    P("plain FBO status=0x%x (0x8CD5=COMPLETE)\n", fbs2);

    double a_render = 0, a_lock = 0, a_copy = 0, a_total = 0;
    double b_render = 0, b_read = 0, b_copy = 0, b_total = 0;
    int a_ok = 0, b_ok = 0;

    /* ---- A 路径：AHB ---- */
    for (int i = 0; i < N; i++) {
        double t0 = now_ms();
        ((viewport_t)dlsym(gl, "glViewport"))(0, 0, W, H);
        ((clearColor_t)dlsym(gl, "glClearColor"))(1.0f, 0.35f, 0.1f, 1.0f);
        ((clear_t)dlsym(gl, "glClear"))(0x4000);
        ((finish_t)dlsym(gl, "glFinish"))();
        double t1 = now_ms();

        void *addr = 0;
        int lr = p_lock(ahb, 0x2 /*CPU_READ_OFTEN*/, -1, (const void *)0, &addr);
        double t2 = now_ms();
        if (lr != 0 || !addr) { P("A lock failed rc=%d\n", lr); break; }

        unsigned st = real.stride ? real.stride : (unsigned)(W * 4);
        unsigned char *dst = sharemem + 16;
        unsigned char *src = (unsigned char *)addr;
        if (st == (unsigned)(W * 4)) {
            memcpy(dst, src, framebytes);
        } else {
            for (int y = 0; y < H; y++)
                memcpy(dst + (size_t)y * W * 4, src + (size_t)y * st, (size_t)W * 4);
        }
        double t3 = now_ms();
        p_unlock(ahb, 0);
        double t4 = now_ms();

        a_render += (t1 - t0); a_lock += (t2 - t1);
        a_copy += (t3 - t2);   a_total += (t4 - t0);
        a_ok++;
    }

    /* ---- B 路径：普通 FBO + glReadPixels ---- */
    if (fbs2 == 0x8CD5 && p_readpixels) {
        for (int i = 0; i < N; i++) {
            double t0 = now_ms();
            ((viewport_t)dlsym(gl, "glViewport"))(0, 0, W, H);
            ((clearColor_t)dlsym(gl, "glClearColor"))(1.0f, 0.35f, 0.1f, 1.0f);
            ((clear_t)dlsym(gl, "glClear"))(0x4000);
            ((finish_t)dlsym(gl, "glFinish"))();
            double t1 = now_ms();

            p_readpixels(0, 0, W, H, 0x1908 /*GL_RGBA*/, 0x1401 /*UNSIGNED_BYTE*/, rp_buf);
            ((finish_t)dlsym(gl, "glFinish"))();
            double t2 = now_ms();

            memcpy(sharemem + 16, rp_buf, framebytes);
            double t3 = now_ms();

            b_render += (t1 - t0); b_read += (t2 - t1);
            b_copy += (t3 - t2);   b_total += (t3 - t0);
            b_ok++;
        }
    }

    if (a_ok > 0) {
        double f = (double)a_ok;
        P("\n[A] AHB  render %.3f + lock %.3f + copy %.3f = %.3f ms/frame (%.1f fps)\n",
          a_render / f, a_lock / f, a_copy / f, a_total / f, 1000.0 * f / a_total);
    }
    if (b_ok > 0) {
        double f = (double)b_ok;
        P("[B] RPIX render %.3f + read %.3f + copy %.3f = %.3f ms/frame (%.1f fps)\n",
          b_render / f, b_read / f, b_copy / f, b_total / f, 1000.0 * f / b_total);
    }
    if (a_ok > 0 && b_ok > 0) {
        double fa = a_total / (double)a_ok, fb = b_total / (double)b_ok;
        P(">>> A/B ratio = %.3fx  (%s)\n", fb / fa,
          fa < fb ? "AHB FASTER" : "AHB SLOWER");
    }

    int ok = a_ok;

    if (ok == 0) { P("RESULT: AHB_TOPIPE_FAIL noframe\n"); return 11; }

    /* 中心像素自检 */
    {
        unsigned char *px = sharemem + 16;
        unsigned off = (unsigned)(H / 2) * (unsigned)(W * 4) + (unsigned)(W / 2) * 4;
        P("center RGBA = %u,%u,%u,%u\n", px[off], px[off + 1], px[off + 2], px[off + 3]);
        if (!(px[off] > 200 && px[off + 1] > 60 && px[off + 1] < 120)) {
            P("RESULT: AHB_TOPIPE_FAIL pixel\n"); return 9;
        }
    }

    P("RESULT: AHB_TOPIPE_OK\n");
    return 0;
}
