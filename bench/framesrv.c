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
 * ★共享文件布局（宿主侧 LinuxFrameSurface.kt 按这个解析，改这里必须同步改那边）：
 *   offset 0   u32 magic = 0x4D524146 ("FRAM")
 *   offset 4   u32 seq   每写一帧 +1，宿主靠它判「有没有新帧」
 *   offset 8   u32 w
 *   offset 12  u32 h
 *   offset 16  起为像素，RGBA8888，w*h*4 字节
 *   全小端。
 *
 * ★文件必须放在 App 可写目录（rootfs 之外）：
 *   /data/data/<pkg>/files/dshframe.bin
 *   放 rootfs 里宿主看不到（容器 path 翻译只对容器内生效）。
 *
 * 输出约定（首行，供宿主解析）：
 *   DSH_FRAME pid=<pid> w=<w> h=<h> fmt=rgba8888 path=<共享文件> renderer=<GL_RENDERER>
 *
 * 用法：/framesrv [w] [h] [frames] [间隔ms] [共享文件路径] [mode] [取像素方式]
 *   mode = clear（默认，纯色渐变清屏）| tri（清屏 + 旋转三角形）
 *   tri 模式用于证明 GL 管线是完整的（着色器编译、VBO、draw call 都走通），
 *   而不仅仅是 glClear 能出像素。
 *   frames=0 表示一直跑到被信号终止。
 *
 *   取像素方式（第 7 个参数）：
 *     rpix（默认）—— glReadPixels 逐行回读
 *     ahb          —— 渲染进 AHardwareBuffer 再 lock（同进程 A/B 实测快 1.72 倍，
 *                     见 benchmark 第 14 节；AHB 不可用时自动退回 rpix 并打印原因）
 *     auto         —— 有 AHB 就用 AHB，没有就 rpix
 *
 * 实测（模拟器 x86_64 / SwiftShader / 640x360 / 16ms 间隔）：稳定 32.8 fps。
 * AHB 路径 1080p 端到端 6.344 ms/帧 vs glReadPixels 10.930 ms/帧（1.72 倍）。
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

#define HDR_BYTES 16u
#define FRAME_MAGIC 0x4D415246u  /* 小端落盘后 od -c 读出即 "FRAM" */

/* AHardwareBuffer 描述体（AOSP 定义，字段顺序不能改） */
typedef struct {
    unsigned int width, height, layers, format;
    unsigned long long usage;
    unsigned int stride, rfu0;
    unsigned long long rfu1;
} AHBDesc;

#define AHB_FMT_RGBA8888        1
#define AHB_USAGE_CPU_READ      (1ULL << 1)   /* CPU_READ_OFTEN */
#define AHB_USAGE_CPU_WRITE     (1ULL << 3)   /* CPU_WRITE_OFTEN */
#define AHB_USAGE_GPU_SAMPLED   (1ULL << 8)
#define AHB_USAGE_GPU_COLOR_OUT (1ULL << 9)

static void put_u32(unsigned char *p, unsigned v) {
    p[0]=(unsigned char)(v & 0xFF); p[1]=(unsigned char)((v>>8) & 0xFF);
    p[2]=(unsigned char)((v>>16) & 0xFF); p[3]=(unsigned char)((v>>24) & 0xFF);
}
static void write_hdr(unsigned char *base, unsigned seq, int w, int h) {
    put_u32(base+0, FRAME_MAGIC);
    put_u32(base+4, seq);
    put_u32(base+8, (unsigned)w);
    put_u32(base+12, (unsigned)h);
}

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    int W = (argc > 1) ? atoi(argv[1]) : 1280;
    int H = (argc > 2) ? atoi(argv[2]) : 720;
    int frames = (argc > 3) ? atoi(argv[3]) : 0;
    int interval = (argc > 4) ? atoi(argv[4]) : 16;
    const char *sharepath = (argc > 5) ? argv[5]
                                       : "/data/data/app.dsh.mobile/files/dshframe.bin";
    int want_tri = (argc > 6 && strcmp(argv[6], "tri") == 0);
    /* 第 7 个参数选取像素方式：rpix（默认）/ ahb / auto */
    const char *pixmode = (argc > 7) ? argv[7] : "rpix";
    if (W < 16) W = 1280;
    if (H < 16) H = 720;
    /* 文件 = 16 字节头 + 整帧像素 */
    size_t pxsz = (size_t)W * H * 4;
    size_t sz = (size_t)HDR_BYTES + pxsz;

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

    /* 先把文件头落好，宿主才能算出该映射多大。seq=0 表示还没有有效帧。 */
    unsigned char *hdr = px;
    unsigned char *pixels = px + HDR_BYTES;
    write_hdr(hdr, 0u, W, H);
    msync(px, HDR_BYTES, MS_SYNC);

    P("DSH_FRAME pid=%ld w=%d h=%d fmt=rgba8888 hdr=%u path=%s renderer=%s\n",
      (long)getpid(), W, H, HDR_BYTES, sharepath, (const char*)gs(0x1F01));
    fsync(1);

    /* ---- 可选的三角形渲染：证明管线完整 ---- */
    unsigned prog = 0, vbo = 0;
    unsigned (*gp2)(void) = dlsym(gl, "glCreateProgram");
    int (*gs2)(unsigned) = dlsym(gl, "glCreateShader");
    void (*gsh)(unsigned, int, const char* const*, const int*) = dlsym(gl, "glShaderSource");
    void (*gco)(unsigned) = dlsym(gl, "glCompileShader");
    void (*gat)(unsigned, unsigned) = dlsym(gl, "glAttachShader");
    void (*glk)(unsigned) = dlsym(gl, "glLinkProgram");
    void (*gup)(unsigned) = dlsym(gl, "glUseProgram");
    void (*gu1)(int, float) = dlsym(gl, "glUniform1f");
    int (*gul)(unsigned, const char*) = dlsym(gl, "glGetUniformLocation");
    unsigned (*gcs)(unsigned) = dlsym(gl, "glCreateShader");
    void (*ggb)(int, unsigned*) = dlsym(gl, "glGenBuffers");
    void (*ggsi)(unsigned, unsigned, int*) = dlsym(gl, "glGetShaderiv");
    void (*ggpi)(unsigned, unsigned, int*) = dlsym(gl, "glGetProgramiv");
    void (*ggil)(unsigned, int, int*, char*) = dlsym(gl, "glGetShaderInfoLog");
    void (*gbd)(int, unsigned) = dlsym(gl, "glBindBuffer");
    void (*gbu)(int, long, const void*, unsigned) = dlsym(gl, "glBufferData");
    void (*gva)(unsigned, int, unsigned, unsigned char, int, const void*) = dlsym(gl, "glVertexAttribPointer");
    void (*gea)(unsigned) = dlsym(gl, "glEnableVertexAttribArray");
    void (*gdr)(int, int, unsigned) = dlsym(gl, "glDrawArrays");

    if (want_tri && gp2) {
        /* 用连续字符串字面量拼接，避免行内 \n 转义被工具链吃掉 */
        const char *vs =
            "attribute vec2 p;\n"
            "uniform float a;\n"
            "void main(){\n"
            "  float c=cos(a),s=sin(a);\n"
            "  vec2 q=vec2(p.x*c-p.y*s, p.x*s+p.y*c);\n"
            "  gl_Position=vec4(q,0.0,1.0);\n"
            "}\n";
        const char *fs =
            "precision mediump float;\n"
            "void main(){ gl_FragColor=vec4(1.0,0.95,0.2,1.0); }\n";
        unsigned v = gcs(0x8B31);
        { const int L=(int)strlen(vs); gsh(v,1,&vs,&L); } gco(v);
        unsigned f2 = gcs(0x8B30);
        { const int L=(int)strlen(fs); gsh(f2,1,&fs,&L); } gco(f2);
        /* 着色器编译必须查状态：失败时 prog 仍在但画不出东西，
           不查会误判成「管线没问题、只是没画」 */
        int ok=0;
        ggsi(v, 0x8B81, &ok);
        if (!ok) { P("FAIL vs compile"); }
        ggsi(f2, 0x8B81, &ok);
        if (!ok) { P("FAIL fs compile"); }
        prog = gp2();
        gat(prog,v); gat(prog,f2); glk(prog);
        ggpi(prog, 0x8B82, &ok);
        if (!ok) { P("FAIL program link"); prog = 0; }
        /* 等边三角形，边长 0.6，中心在原点 */
        static const float verts[6] = { 0.0f, 0.6f,  -0.52f, -0.3f,  0.52f, -0.3f };
        /* ★必须先 glGenBuffers 拿到名字。vbo 初值 0 是「默认缓冲」，
           对它 glBufferData 是 INVALID_OPERATION，顶点数据根本不会上传，
           表现就是「程序链接成功但画面只有清屏色」。 */
        if (ggb) ggb(1, &vbo);
        gbd(0x8892, vbo);
        gbu(0x8892, (long)sizeof(verts), verts, 0x88E4);
        P("tri mode: prog=0x%x vbo=%u", prog, vbo);
    }

    int i = 0;
    double t0 = now_ms();

    /* ---- 可选取像素方式：AHB（渲染直写）vs glReadPixels ----
     *
     * 为什么要给这个选项：同进程 A/B 实测（benchmark 第 14 节）AHB 路径
     * 1080p 端到端 6.344 ms/帧 vs glReadPixels 10.930 ms/帧，快 1.72 倍。
     * 收益集中在「GPU→CPU 取像素」那一段（3.954 vs 8.379 ms，2.1 倍）。
     *
     * ★AHB 不可用时必须**安静地退回 rpix**，不能让帧服务整体失败：
     *   真机上 AHardwareBuffer_createFromHandle 与 EGLImage 绑定的支持度
     *   与模拟器不同（模拟器 gralloc allocator 未进 VINTF，见 pitfalls #41），
     *   而帧服务是产线依赖的，退化成 rpix 只是慢一点，完全不能崩。
     */
    int use_ahb = 0;
    int ahb_fail_streak = 0;   /* AHB lock 连续失败计数，达 3 次彻底切 rpix */
    void *ahb = 0;
    AHBDesc ahbreal;
    memset(&ahbreal, 0, sizeof ahbreal);
    int (*ahb_lock)(void *, unsigned long long, int, const void *, void **) = 0;
    int (*ahb_unlock)(void *, int) = 0;

    if (strcmp(pixmode, "ahb") == 0 || strcmp(pixmode, "auto") == 0) {
        void *nw = dlopen("libnativewindow.so", RTLD_NOW);
        if (!nw) nw = dlopen("libandroid.so", RTLD_NOW);
        int (*ahb_alloc)(const AHBDesc *, void **) =
            nw ? dlsym(nw, "AHardwareBuffer_allocate") : 0;
        void (*ahb_describe)(const void *, AHBDesc *) =
            nw ? dlsym(nw, "AHardwareBuffer_describe") : 0;
        void (*ahb_release)(void *) = nw ? dlsym(nw, "AHardwareBuffer_release") : 0;
        ahb_lock = nw ? dlsym(nw, "AHardwareBuffer_lock") : 0;
        ahb_unlock = nw ? dlsym(nw, "AHardwareBuffer_unlock") : 0;
        void *(*getNCB)(const void *) =
            dlsym(egl, "eglGetNativeClientBufferANDROID");
        void *(*createImage)(void *, void *, unsigned, int *, const int *) =
            dlsym(egl, "eglCreateImageKHR");
        void (*glGenTex)(int, unsigned *) = dlsym(gl, "glGenTextures");
        void (*glBindTex)(unsigned, unsigned) = dlsym(gl, "glBindTexture");
        void (*glTexParam)(unsigned, unsigned, int) = dlsym(gl, "glTexParameteri");
        void (*glImgTex)(unsigned, void *) = dlsym(gl, "glEGLImageTargetTexture2DOES");
        void (*glGenFbo)(int, unsigned *) = dlsym(gl, "glGenFramebuffers");
        void (*glBindFbo)(unsigned, unsigned) = dlsym(gl, "glBindFramebuffer");
        void (*glFboTex)(unsigned, unsigned, unsigned, unsigned, int) =
            dlsym(gl, "glFramebufferTexture2D");
        unsigned (*glCheckFbo)(unsigned) = dlsym(gl, "glCheckFramebufferStatus");

        const char *why = 0;
        if (!ahb_alloc || !ahb_lock || !ahb_unlock) why = "no AHB symbols";
        else if (!getNCB) why = "no eglGetNativeClientBufferANDROID";
        else if (!createImage) why = "no eglCreateImageKHR";
        else if (!glImgTex) why = "no glEGLImageTargetTexture2DOES";
        else if (!glGenFbo || !glFboTex || !glCheckFbo) why = "no FBO symbols";

        if (!why) {
            AHBDesc want;
            memset(&want, 0, sizeof want);
            want.width = (unsigned)W; want.height = (unsigned)H; want.layers = 1;
            want.format = AHB_FMT_RGBA8888;
            want.usage = AHB_USAGE_GPU_SAMPLED | AHB_USAGE_GPU_COLOR_OUT |
                         AHB_USAGE_CPU_READ | AHB_USAGE_CPU_WRITE;
            if (ahb_alloc(&want, &ahb) != 0 || !ahb) why = "AHardwareBuffer_allocate failed";
        }
        if (!why) {
            if (ahb_describe) ahb_describe(ahb, &ahbreal);
            void *cbuf = getNCB(ahb);
            int imgAttr[] = { 0x30D2 /*EGL_IMAGE_PRESERVED_KHR*/, 1, EGL_NONE, 0 };
            void *img = cbuf ? createImage(d, EGL_NO_CONTEXT,
                                           0x3140 /*EGL_NATIVE_BUFFER_ANDROID*/,
                                           cbuf, imgAttr) : 0;
            if (!img) why = "eglCreateImageKHR failed";
            else {
                unsigned tex = 0, fbo = 0;
                glGenTex(1, &tex);
                glBindTex(0x0DE1, tex);
                glTexParam(0x0DE1, 0x2801, 0x2601);
                glTexParam(0x0DE1, 0x2800, 0x2601);
                glImgTex(0x0DE1, img);
                glGenFbo(1, &fbo);
                glBindFbo(0x8D40, fbo);
                glFboTex(0x8D40, 0x8CE0, 0x0DE1, tex, 0);
                if (glCheckFbo(0x8D40) != 0x8CD5) why = "FBO incomplete";
                else use_ahb = 1;
            }
        }
        if (use_ahb) {
            P("pixmode=ahb stride=%u (AHB 直写，省 glReadPixels)\n", ahbreal.stride);
        } else {
            P("pixmode=rpix (fallback: %s)\n", why ? why : "unknown");
        }
        fsync(1);
    }

    while (!g_stop && (frames <= 0 || i < frames)) {
        float t = (float)(i % 120) / 120.0f;
        cc2(t * 0.55f, (1.0f - t) * 0.55f, 0.32f, 1.0f);
        cl(0x4000);
        if (prog) {
            gup(prog);
            gbd(0x8892, vbo);
            gea(0);
            gva(0, 2, 0x1406, 0, 0, 0);
            gu1(gul(prog, "a"), (float)i * 0.05f);
            gdr(0x0004, 0, 3);
        }
        fi();
        if (use_ahb) {
            /* AHB 路径：GPU 已经直接写进 AHB，这里 lock 拿指针再拷进共享文件。
             * AHB lock 内部也做 GPU→CPU 同步，但它比 glReadPixels 那套逐行拷
             * 高效（实测 3.954 vs 8.379 ms @1080p），所以整体仍快 1.72 倍。
             * stride 可能 > W*4（对齐），必须按行拷。 */
            void *addr = 0;
            int lock_rc = ahb_lock(ahb, 0x2 /*CPU_READ_OFTEN*/, -1, (const void *)0, &addr);
            if (lock_rc == 0 && addr) {
                unsigned st = ahbreal.stride ? ahbreal.stride : (unsigned)(W * 4);
                unsigned char *src = (unsigned char *)addr;
                if (st == (unsigned)(W * 4)) {
                    memcpy(pixels, src, pxsz);
                } else {
                    for (int y = 0; y < H; y++)
                        memcpy(pixels + (size_t)y * (size_t)W * 4,
                               src + (size_t)y * st, (size_t)W * 4);
                }
                ahb_unlock(ahb, 0);
                ahb_fail_streak = 0;
            } else {
                /* ★AHB lock 失败必须退回 glReadPixels，不能什么都不做。
                 *
                 * 原来的实现：lock 失败时整个 if 块被跳过，pixels 保持**上一帧**
                 * 的内容，但下面 write_hdr 照旧把 seq 加一。宿主侧判据是 seq
                 * 变化 → 认为有新帧 → 显示旧内容。画面永久静止，不报任何错。
                 *
                 * 这是 2026-11-07 修 wlshot --loop 时发现的同类静默失败：
                 * 「保持最后成功状态并继续宣称成功」比直接失败更难查。
                 *
                 * 退避策略：连续失败 3 帧后彻底切回 rpix（AHB 可能已损坏，
                 * 每帧都试一次 lock 只是白白浪费 CPU），并把原因打出来。 */
                ahb_fail_streak++;
                if (ahb_fail_streak == 1) {
                    P("warn: AHB lock failed (rc=%d addr=%p), falling back to glReadPixels\n",
                      lock_rc, addr);
                }
                rp(0,0,W,H,0x1908,0x1401,pixels);
                if (ahb_fail_streak >= 3 && use_ahb) {
                    P("warn: AHB lock failed %d times, switching pixmode to rpix for good\n",
                      ahb_fail_streak);
                    use_ahb = 0;
                }
            }
        } else {
            rp(0,0,W,H,0x1908,0x1401,pixels);
        }
        i++;
        /* 先更新尺寸/魔数再推 seq —— 宿主若在中间读到，seq 还是旧值，
           不会把半帧当成新帧显示。 */
        write_hdr(hdr, (unsigned)i, W, H);
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
