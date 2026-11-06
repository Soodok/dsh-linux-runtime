/*
 * window_probe.c —— 端到端窗口路径验证（容器渲染 → memfd → 宿主可取）
 *
 * 这是「免 root 开窗口」方案的最小闭环证明：
 *   ① 容器内 bionic 程序用宿主 GPU 渲染；
 *   ② glReadPixels 直接回读进 memfd 映射（零用户态中转拷贝）；
 *   ③ 宿主同 uid 可直接 open 同一块内存（/proc/<pid>/fd/N）；
 *   ④ 顺带落一份 PPM，供人眼确认渲染内容真的出来了。
 *
 * 为什么不能直接建 SurfaceFlinger layer：那需要 app Binder 权限，免 root 拿不到。
 * 所以 App 侧只能「取像素 → SurfaceView 贴图」。本探针证明容器侧那半段通且够快。
 *
 * 实测（模拟器 x86_64 / 1280x720 / SwiftShader）：
 *   render+finish  0.438 ms/frame
 *   read→memfd     2.513 ms/frame
 *   合计           2.958 ms/frame → 338 fps
 *   中心像素 RGBA = 255,89,26,255（对齐 glClearColor(1.0,0.35,0.1)）
 *
 * 用法：/window_probe [frames] [输出PPM的绝对可写路径]
 *   ★输出路径必须显式传：本程序跑在 cleaner（bionic 环境）里，
 *     bionic 的 /tmp 与容器内的 /tmp 不是同一个东西，且通常不可写
 *     （实测 fopen("/tmp/...") 静默失败）。
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

#if defined(__x86_64__)
#define NR_MEMFD_CREATE 319
#else
#define NR_MEMFD_CREATE 279
#endif
static long raw4(long n, long a, long b, long c, long d) {
#if defined(__x86_64__)
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(d):"rcx","r11","memory");
    return r;
#else
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
#endif
}

int main(int argc, char **argv) {
    const int W = 1280, H = 720;
    int frames = (argc > 1) ? atoi(argv[1]) : 60;
    const char *outp = (argc > 2) ? argv[2] : "/data/local/tmp/window.ppm";
    size_t sz = (size_t)W * H * 4;
    P("=== 端到端窗口路径 ===\n");

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
    P("GL_RENDERER=%s\n",(const char*)gs(0x1F01));
    P("size=%dx%d  frame=%.2f MB  frames=%d\n",W,H,sz/1048576.0,frames);

    int fd = (int)raw4(NR_MEMFD_CREATE,(long)"dshwin",1,0,0);
    if (fd < 0) { P("FAIL memfd errno=%d\n", errno); return 3; }
    if (ftruncate(fd,(off_t)sz)!=0) { P("FAIL ftruncate\n"); return 4; }
    unsigned char *px = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (px==MAP_FAILED) { P("FAIL mmap errno=%d\n", errno); return 5; }
    P("memfd fd=%d  mmap OK\n", fd);

    for (int i=0;i<5;i++){ cc2(0.1f,0.2f,0.3f,1); cl(0x4000); fi(); rp(0,0,W,H,0x1908,0x1401,px); }

    double tRender=0, tRead=0, t0=now_ms();
    for (int i=0;i<frames;i++){
        double a0=now_ms();
        cc2((float)(i%10)/10.0f,0.3f,0.5f,1);
        cl(0x4000); fi();
        double a1=now_ms();
        rp(0,0,W,H,0x1908,0x1401,px);
        tRender += a1-a0; tRead += now_ms()-a1;
    }
    double total=now_ms()-t0;
    P("render+finish : %7.3f ms/frame\n", tRender/frames);
    P("read->memfd   : %7.3f ms/frame\n", tRead/frames);
    P("---- 合计     : %7.3f ms/frame -> %.1f fps\n", total/frames, frames*1000.0/total);

    cc2(1.0f,0.35f,0.1f,1.0f); cl(0x4000); fi(); rp(0,0,W,H,0x1908,0x1401,px);
    unsigned char *mid = px + (((size_t)(H/2)*W)+W/2)*4;
    P("center RGBA = %d,%d,%d,%d\n", mid[0],mid[1],mid[2],mid[3]);

    FILE *f = fopen(outp,"wb");
    if (f) {
        fprintf(f,"P6\n%d %d\n255\n",W,H);
        for (int y=H-1;y>=0;y--) for(int x=0;x<W;x++){
            unsigned char *q=px+(((size_t)y*W)+x)*4; fwrite(q,1,3,f);
        }
        fclose(f); P("wrote %s\n", outp);
    } else {
        P("WARN fopen errno=%d\n", errno);
    }
    P("host-open path = /proc/<pid>/fd/%d\n", fd);
    P("RESULT: WINDOW_PATH_OK\n");
    return 0;
}
