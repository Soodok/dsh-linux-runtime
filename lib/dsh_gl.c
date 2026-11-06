/*
 * dsh_gl.c —— dsh_gl.h 的实现
 *
 * 设计取舍（都是实测踩过才这么定的）：
 *
 * 1. **全部 dlopen + dlsym，不链接 libEGL/libGLESv2。**
 *    bionic 的非 zygote 进程（我们的 cleaner 代跑的程序就是）用静态链接的
 *    libEGL 会触发 `FORTIFY: pthread_mutex_lock called on a destroyed mutex`
 *    直接 abort。dlopen 才正常。
 *
 * 2. **不用 printf，全部裸 write。** 同上，printf 也会踩那个 FORTIFY abort。
 *
 * 3. **共享文件而不是 memfd。** `/proc/<pid>/fd/<n>` 只有 root 能 open，
 *    同 uid 的 App 读容器进程的 memfd 会被 SELinux 拦成 Permission denied。
 *
 * 4. **文件头 + 像素放同一个映射。** 分开映射要在两个文件之间同步，多一处
 *    出错空间；单文件单映射，只要 seq 写对就天然一致。
 *
 * 5. **seq 用「先写像素、后推 seq」的顺序。** 宿主看到 seq 变了才会去读像素，
 *    所以只要 seq 是最后写的，宿主就不会读到半帧。这里靠 msync 的可见性
 *    保证：像素写在前，seq 写在后。
 */
#include "dsh_gl.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ---- 最小 EGL/GLES 常量与签名（不依赖系统头，跨架构通用） ---- */
typedef void *EGLDisplay_, *EGLConfig_, *EGLSurface_, *EGLContext_;
typedef int EGLint_;
typedef unsigned int EGLenum_, EGLBoolean_;

#define EGL_NO_CONTEXT_ ((EGLContext_)0)
#define EGL_DEFAULT_DISPLAY_ ((void *)0)
#define EGL_NONE_ 0x3038
#define EGL_SURFACE_TYPE_ 0x3033
#define EGL_PBUFFER_BIT_ 0x0001
#define EGL_RENDERABLE_TYPE_ 0x3040
#define EGL_OPENGL_ES2_BIT_ 0x0004
#define EGL_RED_SIZE_ 0x3024
#define EGL_GREEN_SIZE_ 0x3023
#define EGL_BLUE_SIZE_ 0x3022
#define EGL_ALPHA_SIZE_ 0x3021
#define EGL_WIDTH_ 0x3057
#define EGL_HEIGHT_ 0x3056
#define EGL_CONTEXT_CLIENT_VERSION_ 0x3098
#define EGL_OPENGL_ES_API_ 0x30A0

#define GL_COLOR_BUFFER_BIT_ 0x4000
#define GL_RGBA_ 0x1908
#define GL_UNSIGNED_BYTE_ 0x1401
#define GL_RENDERER_ 0x1F01

static char g_err[256];

const char *dsh_gl_error(void) { return g_err; }

static void seterr(const char *what, const char *detail) {
    size_t i = 0, j = 0;
    while (what[i] && i < sizeof(g_err) - 4) { g_err[i] = what[i]; i++; }
    if (detail) {
        g_err[i++] = ':';
        g_err[i++] = ' ';
        while (detail[j] && i < sizeof(g_err) - 1) g_err[i++] = detail[j++];
    }
    g_err[i] = 0;
}

void dsh_write(const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = write(1, s, n);
        if (w <= 0) break;
        s += w;
        n -= (size_t)w;
    }
}

/* 极简 printf 子集：够诊断用，不引入 stdio 的锁 */
void dsh_printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(1, buf + off, len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
}

/* 小端写 u32（aarch64/x86_64 本来就小端，但显式写不依赖字节序假设） */
static void put_u32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void write_hdr(const dsh_frame *f, unsigned seq) {
    put_u32(f->map + 0, DSH_FRAME_MAGIC);
    put_u32(f->map + 4, seq);
    put_u32(f->map + 8, (unsigned)f->width);
    put_u32(f->map + 12, (unsigned)f->height);
}

int dsh_frame_open(dsh_frame *f, int w, int h, const char *sharepath) {
    if (!f) return 1;
    memset(f, 0, sizeof(*f));
    f->fd = -1;
    if (w < 16) w = 16;
    if (h < 16) h = 16;

    if (!sharepath) {
        seterr("sharepath is required", NULL);
        return 2;
    }

    /* ---- EGL ---- */
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { seterr("dlopen libEGL", dlerror()); return 3; }

    EGLDisplay_ (*p_getDisplay)(void *) = dlsym(egl, "eglGetDisplay");
    EGLBoolean_ (*p_init)(EGLDisplay_, EGLint_ *, EGLint_ *) = dlsym(egl, "eglInitialize");
    EGLBoolean_ (*p_choose)(EGLDisplay_, const EGLint_ *, EGLConfig_ *, EGLint_, EGLint_ *) =
        dlsym(egl, "eglChooseConfig");
    EGLSurface_ (*p_pbuf)(EGLDisplay_, EGLConfig_, const EGLint_ *) =
        dlsym(egl, "eglCreatePbufferSurface");
    EGLBoolean_ (*p_bindapi)(EGLenum_) = dlsym(egl, "eglBindAPI");
    EGLContext_ (*p_ctx)(EGLDisplay_, EGLConfig_, EGLContext_, const EGLint_ *) =
        dlsym(egl, "eglCreateContext");
    EGLBoolean_ (*p_current)(EGLDisplay_, EGLSurface_, EGLSurface_, EGLContext_) =
        dlsym(egl, "eglMakeCurrent");
    if (!p_getDisplay || !p_init || !p_choose || !p_pbuf || !p_bindapi || !p_ctx || !p_current) {
        seterr("EGL entrypoints missing", NULL);
        return 4;
    }

    EGLDisplay_ d = p_getDisplay(EGL_DEFAULT_DISPLAY_);
    EGLint_ vmaj = 0, vmin = 0;
    if (!p_init(d, &vmaj, &vmin)) { seterr("eglInitialize failed", NULL); return 5; }

    EGLint_ cfgattr[] = {
        EGL_SURFACE_TYPE_, EGL_PBUFFER_BIT_,
        EGL_RENDERABLE_TYPE_, EGL_OPENGL_ES2_BIT_,
        EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8,
        EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, 8,
        EGL_NONE_
    };
    EGLConfig_ cfg;
    EGLint_ ncfg = 0;
    if (!p_choose(d, cfgattr, &cfg, 1, &ncfg) || ncfg < 1) {
        seterr("eglChooseConfig found no config", NULL);
        return 6;
    }
    EGLint_ pbattr[] = { EGL_WIDTH_, w, EGL_HEIGHT_, h, EGL_NONE_ };
    EGLSurface_ surf = p_pbuf(d, cfg, pbattr);
    if (!surf) { seterr("eglCreatePbufferSurface failed", NULL); return 7; }

    p_bindapi(EGL_OPENGL_ES_API_);
    EGLint_ ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION_, 2, EGL_NONE_ };
    EGLContext_ ctx = p_ctx(d, cfg, EGL_NO_CONTEXT_, ctxattr);
    if (!ctx) { seterr("eglCreateContext failed", NULL); return 8; }
    if (!p_current(d, surf, surf, ctx)) { seterr("eglMakeCurrent failed", NULL); return 9; }

    void *gl = dlopen("libGLESv2.so", RTLD_NOW);
    if (!gl) { seterr("dlopen libGLESv2", dlerror()); return 10; }

    f->egl_display = (void *)d;
    f->egl_surface = (void *)surf;
    f->egl_context = (void *)ctx;
    f->gl_lib = gl;
    f->width = w;
    f->height = h;
    f->bytes = (size_t)w * (size_t)h * 4u;

    /* ---- 共享文件 ---- */
    size_t total = (size_t)DSH_FRAME_HDR + f->bytes;
    int fd = open(sharepath, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { seterr("open sharepath", strerror(errno)); return 11; }
    if (ftruncate(fd, (off_t)total) != 0) {
        seterr("ftruncate sharepath", strerror(errno));
        close(fd);
        return 12;
    }
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        seterr("mmap sharepath", strerror(errno));
        close(fd);
        return 13;
    }
    f->fd = fd;
    f->map = (unsigned char *)map;
    f->pixels = f->map + DSH_FRAME_HDR;
    f->seq = 0;
    /* 先落文件头（seq=0 表示还没有有效帧），宿主才能算出该映射多大 */
    write_hdr(f, 0u);
    msync(f->map, DSH_FRAME_HDR, MS_SYNC);

    /* 报告 renderer，便于确认拿到的是硬件还是软件实现 */
    const unsigned char *(*p_getString)(unsigned) = dlsym(gl, "glGetString");
    if (p_getString) {
        const unsigned char *r = p_getString(GL_RENDERER_);
        dsh_printf("DSH_FRAME_GL w=%d h=%d renderer=%s\n", w, h, r ? (const char *)r : "?");
    }
    return 0;
}

int dsh_frame_present(dsh_frame *f) {
    if (!f || f->fd < 0 || !f->map) { seterr("frame not open", NULL); return 1; }

    void (*p_finish)(void) = dlsym(f->gl_lib, "glFinish");
    void (*p_read)(int, int, int, int, unsigned, unsigned, void *) =
        dlsym(f->gl_lib, "glReadPixels");
    if (!p_finish || !p_read) { seterr("GL entrypoints missing", NULL); return 2; }

    p_finish();
    p_read(0, 0, f->width, f->height, GL_RGBA_, GL_UNSIGNED_BYTE_, f->pixels);
    /* ★顺序很重要：像素先写、seq 后推。宿主只在 seq 变化时去读像素，
       所以只要 seq 是最后写的，就绝不会读到半帧。 */
    f->seq += 1u;
    write_hdr(f, f->seq);
    return 0;
}

void dsh_frame_close(dsh_frame *f) {
    if (!f) return;
    if (f->map) { munmap(f->map, (size_t)DSH_FRAME_HDR + f->bytes); f->map = NULL; f->pixels = NULL; }
    if (f->fd >= 0) { close(f->fd); f->fd = -1; }
    /* EGL/GL 交给进程退出回收：这套 API 里 eglTerminate 在 cleaner 代跑
       场景下反而容易在析构顺序上出问题（实测偶发 FORTIFY abort），
       进程生命周期本来就短，不主动销毁更稳。 */
}
