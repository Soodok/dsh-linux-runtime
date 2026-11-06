/*
 * frame_pump.c —— 验证「容器渲染 → 回读 → 交给 App 显示」这条窗口路径的成本
 *
 * 免 root 时容器侧建不了 SurfaceFlinger layer（那需要 app Binder 权限），
 * 所以唯一可行的窗口方案是：容器内 bionic 程序离屏渲染 → glReadPixels 回读
 * → 通过共享内存/文件/fd 交给 App 的 SurfaceView 贴图。
 *
 * 本探针测的就是这条链路里**容器侧**那一段的耗时：
 *   渲染 + glFinish + glReadPixels + 写出一帧（不含 App 侧贴图）。
 *
 * 关键：glReadPixels 是同步回读，通常比渲染本身贵一个数量级。
 * 实测它决定「这条路径能不能到 30/60 fps」。
 */
#include <dlfcn.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>

static void P(const char *fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (n > 0) write(1, b, (size_t)n);
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

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static int run(int W, int H, int frames, const char *outpath) {
    void *egl = dlopen("libEGL.so", RTLD_NOW);
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
    if(!mc(d,s,s,c)){P("current FAIL\n");return 1;}

    void *gl=dlopen("libGLESv2.so",RTLD_NOW);
    void (*vp)(int,int,int,int)=dlsym(gl,"glViewport");
    void (*cc2)(float,float,float,float)=dlsym(gl,"glClearColor");
    void (*cl)(unsigned)=dlsym(gl,"glClear");
    void (*fi)(void)=dlsym(gl,"glFinish");
    void (*rp)(int,int,int,int,unsigned,unsigned,void*)=dlsym(gl,"glReadPixels");
    const unsigned char*(*gs)(unsigned)=dlsym(gl,"glGetString");
    vp(0,0,W,H);

    size_t sz = (size_t)W*H*4;
    unsigned char *px = malloc(sz);
    int fd = open(outpath, O_WRONLY|O_CREAT|O_TRUNC, 0644);

    // 预热
    for(int i=0;i<5;i++){cc2(0.1f,0.2f,0.3f,1);cl(0x4000);fi();rp(0,0,W,H,0x1908,0x1401,px);}

    double tRender=0, tRead=0, tWrite=0;
    double t0 = now_ms();
    for(int i=0;i<frames;i++){
        double a0=now_ms();
        cc2((float)(i%10)/10.0f,0.3f,0.5f,1);
        cl(0x4000);
        fi();
        double a1=now_ms();
        rp(0,0,W,H,0x1908,0x1401,px);
        double a2=now_ms();
        if(fd>=0 && i==frames-1 && lseek(fd,0,SEEK_SET)>=0) write(fd,px,sz);
        double a3=now_ms();
        tRender+=a1-a0; tRead+=a2-a1; tWrite+=a3-a2;
    }
    double total = now_ms()-t0;
    if(fd>=0) close(fd);
    free(px);

    P("  %dx%d  frames=%d\n", W,H,frames);
    P("  render+finish : %7.3f ms/frame\n", tRender/frames);
    P("  glReadPixels  : %7.3f ms/frame\n", tRead/frames);
    P("  write(单帧)   : %7.3f ms\n", tWrite/frames);
    P("  ---- 合计     : %7.3f ms/frame  → %.1f fps\n", total/frames, frames*1000.0/total);
    return 0;
}

int main(int argc, char **argv) {
    const char *out = (argc>1)?argv[1]:"/tmp/frame.raw";
    int frames = (argc>2)?atoi(argv[2]):60;
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    EGLDisplay (*gd)(void*) = dlsym(egl,"eglGetDisplay");
    EGLBoolean (*ei)(EGLDisplay,EGLint*,EGLint*) = dlsym(egl,"eglInitialize");
    EGLDisplay d = gd(EGL_DEFAULT_DISPLAY); EGLint a=0,b=0; ei(d,&a,&b);
    void *gl=dlopen("libGLESv2.so",RTLD_NOW);
    const unsigned char*(*gs)(unsigned)=dlsym(gl,"glGetString");
    P("GL_RENDERER=%s\n",(const char*)gs(0x1F01));
    P("=== 帧泵成本分解 ===\n");
    run(1280, 720, frames, out);
    run(1920,1080, frames, out);
    P("RESULT: PUMP_OK\n");
    return 0;
}
