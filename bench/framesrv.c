/*
 * framesrv.c —— 容器侧帧服务：用宿主 GPU 持续渲染，写进共享文件供宿主贴图
 *
 * 这是「免 root 开窗口」的完整可行方案：
 *   App 侧起一个容器进程跑本程序 → 它用宿主 GPU 渲染 → glReadPixels 直接
 *   落进双方 mmap 的同一个普通文件 → App 侧 mmap 同一文件即得 RGBA 像素，
 *   交给 SurfaceView/ImageView 贴图。零拷贝、无需 Binder、无需 root。
 *
 * ★★为什么不能用 memfd 做跨进程共享（实测踩过）：
 *   /proc/<pid>/fd/<n> 只有 root 能 open。同一个 uid 下的 run-as 进程读它会
 *   被 SELinux 拦成 Permission denied（实测：root 读 921600 字节正常，
 *   run-as 读报 Permission denied）。memfd 只能在**同进程内**用。
 *   跨进程必须落在双方都能 mmap 的**普通文件**上。
 *
 * ★文件必须放在 App 可写目录（rootfs 之外）：
 *   /data/data/<pkg>/files/dshframe.bin
 *   放 rootfs 里宿主看不到（容器 path 翻译只对容器内生效）。
 *
 * 输出约定（首行，供宿主解析）：
 *   DSH_FRAME pid=<pid> w=<w> h=<h> fmt=rgba8888 path=<共享文件> renderer=<GL_RENDERER>
 *
 * 用法：/framesrv [w] [h] [frames] [间隔ms] [共享文件路径]
 *   frames=0 表示一直跑到被信号终止。
 *
 * 实测（模拟器 x86_64 / SwiftShader / 640x360 / 16ms 间隔）：稳定 32.8 fps。
 */
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>

static void P(const char *fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (n > 0) write(1, b, (size_t)n);
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1e6;
}
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint; typedef unsigned int EGLenum, EGLBoolean;
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NONE 0x3038
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

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    int W = (argc > 1) ? atoi(argv[1]) : 1280;
    int H = (argc > 2) ? atoi(argv[2]) : 720;
    int frames = (argc > 3) ? atoi(argv[3]) : 0;
    int interval = (argc > 4) ? atoi(argv[4]) : 16;
    const char *sharepath = (argc > 5) ? argv[5]
                                       : "/data/data/app.dsh.mobile/files/dshframe.bin";
    if (W < 16) W = 1280;
    if (H < 16) H = 720;
    size_t sz = (size_t)W * H * 4;

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);

    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) { P("FAIL dlopen libEGL\n"); return 1; }
    EGLDisplay (*gd)(void*) = dlsym(egl,"eglGetDisplay");
    EGLBoolean (*ei)(EGLDisplay,EGLint*,EGLint*) = dlsym(egl,"eglInitialize");
    EGLBoolean (*cc)(EGLDisplay,const EGLint*,EGLConfig*,EGLint,EGLint*) = dlsym(egl,"eglChooseConfig");
    EGLSurface (*cp)(EGLDisplay,EGLConfig,const EGLint*) = dlsym(egl,"eglCreatePbufferSurface");
    EGLBoolean (*ba)(EGLenum) = dlsym(egl,"eglBindAPI");
    EGLContext (*cx)(EGLDisplay,EGLConfig,EGLContext,const EGLint*) = dlsym(egl,"eglCreateContext");
    EGLBoolean (*mc)(EGLDisplay,EGLSurface,EGLSurface,EGLContext) = dlsym(egl,"eglMakeCurrent");
    EGLDisplay d = gd(EGL_DEFAULT_DISPLAY);
    EGLint a=0,b=0; ei(d,&a,&b);
    EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                 EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig cf; EGLint n=0; cc(d,ca,&cf,1,&n);
    EGLint pb[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE};
    EGLSurface s = cp(d,cf,pb);
    ba(EGL_OPENGL_ES_API);
    EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext c = cx(d,cf,EGL_NO_CONTEXT,xa);
    if(!mc(d,s,s,c)){P("FAIL current\n");return 2;}

    void *gl=dlopen("libGLESv2.so",RTLD_NOW);
    void (*vp)(int,int,int,int)=dlsym(gl,"glViewport");
    void (*cc2)(float,float,float,float)=dlsym(gl,"glClearColor");
    void (*cl)(unsigned)=dlsym(gl,"glClear");
    void (*fi)(void)=dlsym(gl,"glFinish");
    void (*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(gl,"glReadPixels");
    const unsigned char*(*gs)(unsigned)=dlsym(gl,"glGetString");
    vp(0,0,W,H);

    int fd = open(sharepath, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { P("FAIL open(%s) errno=%d\n", sharepath, errno); return 3; }
    if (ftruncate(fd,(off_t)sz)!=0) { P("FAIL ftruncate\n"); return 4; }
    unsigned char *px = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (px==MAP_FAILED) { P("FAIL mmap errno=%d\n", errno); return 5; }

    P("DSH_FRAME pid=%ld w=%d h=%d fmt=rgba8888 path=%s renderer=%s\n",
      (long)getpid(), W, H, sharepath, (const char*)gs(0x1F01));
    fsync(1);

    int i = 0;
    double t0 = now_ms();
    while (!g_stop && (frames <= 0 || i < frames)) {
        float t = (float)(i % 120) / 120.0f;
        cc2(t, 1.0f - t, 0.5f, 1.0f);
        cl(0x4000);
        fi();
        rp(0,0,W,H,0x1908,0x1401,px);
        i++;
        if (frames <= 0 && (i % 120) == 0) {
            double el = now_ms() - t0;
            P("progress frames=%d %.1f fps\n", i, i*1000.0/el);
            fsync(1);
        }
        if (interval > 0) usleep((useconds_t)interval * 1000);
    }
    double el = now_ms() - t0;
    P("done frames=%d elapsed=%.0fms fps=%.1f\n", i, el, i*1000.0/el);
    return 0;
}
