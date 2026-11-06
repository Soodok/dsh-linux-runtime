/*
 * gpu_probe.c —— 容器内硬件 GPU 可用性探针（Android bionic 侧）
 *
 * 用途：验证「免 root 的 musl 容器里，bionic 图形程序能否用上宿主 GPU」，
 * 并把结果量化为可对比的数字。
 *
 * 编译（NDK r28+，x86_64 与 arm64 都用对应 target）：
 *   CC=$NDK/toolchains/llvm/prebuilt/<host>/bin/<arch>-linux-android28-clang
 *   $CC -O2 -o gpu_probe gpu_probe.c -ldl
 *
 * 用法（在容器里）：
 *   dsh_alpine <rootfs> <interp> /bin/sh -c "/gpu_probe [frames]"
 *
 * 为什么要走 sh -c：dsh_alpine 对「guest 本身是 bionic ELF」的直接调用会
 * 先走 musl interp，报 `__libc_init: symbol not found`；只有经过一次
 * 容器内的 execve（sh 发起）才会命中 bionic 直通分支、转交 cleaner 代跑。
 *
 * 关键设计：全部用 dlopen + dlsym，且输出走裸 write()。
 *   静态链接 libEGL 会在 bionic 的非 zygote 进程里触发
 *   `FORTIFY: pthread_mutex_lock called on a destroyed mutex` 直接 abort；
 *   同理 printf 家族也走 stdio 锁，必须绕开。
 */
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static void P(const char *fmt, ...) {
    char b[512];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    if (n > 0) write(1, b, (size_t)n);
}

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint;
typedef unsigned int EGLenum, EGLBoolean;

#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NONE 0x3038
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_API 0x30A0

int main(int argc, char **argv) {
    const int W = 1920, H = 1080;
    int frames = (argc > 1) ? atoi(argv[1]) : 60;
    if (frames < 1) frames = 60;

    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { P("FAIL dlopen libEGL: %s\n", dlerror()); return 1; }
    EGLDisplay (*getDisplay)(void *) = dlsym(egl, "eglGetDisplay");
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *) = dlsym(egl, "eglInitialize");
    const char *(*queryString)(EGLDisplay, EGLint) = dlsym(egl, "eglQueryString");
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) =
        dlsym(egl, "eglChooseConfig");
    EGLSurface (*createPbuffer)(EGLDisplay, EGLConfig, const EGLint *) =
        dlsym(egl, "eglCreatePbufferSurface");
    EGLBoolean (*bindAPI)(EGLenum) = dlsym(egl, "eglBindAPI");
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) =
        dlsym(egl, "eglCreateContext");
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) =
        dlsym(egl, "eglMakeCurrent");

    EGLDisplay d = getDisplay(EGL_DEFAULT_DISPLAY);
    if (!d) { P("FAIL no EGL display\n"); return 2; }
    EGLint maj = 0, min = 0;
    if (!initialize(d, &maj, &min)) { P("FAIL eglInitialize\n"); return 3; }
    P("EGL %d.%d vendor=%s\n", maj, min, queryString(d, EGL_VENDOR));
    P("EGL version=%s\n", queryString(d, EGL_VERSION));

    EGLint cfga[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                     EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                     EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!chooseConfig(d, cfga, &cfg, 1, &n) || n < 1) { P("FAIL chooseConfig n=%d\n", n); return 4; }
    EGLint pb[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface s = createPbuffer(d, cfg, pb);
    bindAPI(EGL_OPENGL_ES_API);
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = createContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (!c) { P("FAIL createContext\n"); return 5; }
    if (!makeCurrent(d, s, s, c)) { P("FAIL makeCurrent\n"); return 6; }

    void *gl = dlopen("libGLESv2.so", RTLD_NOW);
    if (!gl) { P("FAIL dlopen libGLESv2\n"); return 7; }
    void (*viewport)(int, int, int, int) = dlsym(gl, "glViewport");
    void (*clearColor)(float, float, float, float) = dlsym(gl, "glClearColor");
    void (*clear)(unsigned) = dlsym(gl, "glClear");
    void (*finish)(void) = dlsym(gl, "glFinish");
    void (*readPixels)(int, int, int, int, unsigned, unsigned, void *) = dlsym(gl, "glReadPixels");
    const unsigned char *(*gstr)(unsigned) = dlsym(gl, "glGetString");
    if (!viewport || !clearColor || !clear || !finish || !readPixels || !gstr) {
        P("FAIL dlsym GLESv2\n"); return 8;
    }

    P("GL_RENDERER=%s\n", (const char *)gstr(0x1F01));
    P("GL_VERSION=%s\n", (const char *)gstr(0x1F02));
    P("size=%dx%d frames=%d\n", W, H, frames);

    viewport(0, 0, W, H);
    for (int i = 0; i < 10; i++) { clearColor(0.1f, 0.2f, 0.3f, 1.0f); clear(0x4000); finish(); }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < frames; i++) {
        clearColor((float)(i % 10) / 10.0f, 0.3f, 0.5f, 1.0f);
        clear(0x4000);
    }
    finish();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
                ((double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
    P("total=%.2f ms  per_frame=%.3f ms  fps=%.1f\n", ms, ms / frames, frames * 1000.0 / ms);

    /* 正确性：清成橙色后回读中心像素，验证不是空跑 */
    clearColor(1.0f, 0.35f, 0.1f, 1.0f);
    clear(0x4000);
    finish();
    unsigned char *px = malloc((size_t)W * H * 4);
    readPixels(0, 0, W, H, 0x1908, 0x1401, px);
    unsigned char *p = px + (((size_t)(H / 2) * W) + W / 2) * 4;
    P("center RGBA = %d,%d,%d,%d\n", p[0], p[1], p[2], p[3]);
    int ok = (p[0] > 240 && p[1] > 70 && p[1] < 110 && p[2] > 15 && p[2] < 40);
    free(px);
    P("RESULT: %s\n", ok ? "GPU_OK" : "RENDER_MISMATCH");
    return ok ? 0 : 9;
}
