/*
 * dsh_gl.h —— 在 DSH 免 root 容器里用宿主 GPU 渲染、把画面送回 Android App
 *
 * 用法（三步）：
 *
 *     dsh_frame f;
 *     if (dsh_frame_open(&f, 1280, 720, NULL) != 0) return 1;
 *     while (running) {
 *         glClearColor(...); glClear(...);   // 你自己的 GL 代码
 *         dsh_frame_present(&f);             // 渲染完成 → 回读 → 写共享文件
 *     }
 *     dsh_frame_close(&f);
 *
 * 它会做的事：
 *   1. dlopen libEGL / libGLESv2（**不能静态链接**，否则 bionic 非 zygote 进程
 *      会 FORTIFY abort，见 pitfalls #31）
 *   2. 建 EGL pbuffer surface + GLES2 上下文
 *   3. 打开共享文件、写好文件头、mmap 好像素区
 *   4. 每次 present：glFinish → glReadPixels 直接写进 mmap → 推进 seq
 *
 * 共享文件格式（App 侧 LinuxFrameSurface.kt 按这个解析，改这里必须同步改那边）：
 *   offset 0   u32 magic = 0x4D415246（小端落盘即 "FRAM"）
 *   offset 4   u32 seq     每帧 +1，宿主靠它判「有没有新帧」
 *   offset 8   u32 w
 *   offset 12  u32 h
 *   offset 16  起为像素，RGBA8888，w*h*4 字节
 *
 * 注意：
 *   - 这是 bionic 程序（由 cleaner 代跑）。**不能用 printf**，输出请用
 *     dsh_write()（裸 write）或先 printf 后立刻 fflush —— 但更稳的是裸 write。
 *   - 容器里必须经 `sh -c "/yourprog"` 启动，让容器内再发一次 execve 才会
 *     命中 guest_is_bionic() 走 cleaner。直接 `dsh_alpine ... /yourprog`
 *     会按 musl 装载，报 `__libc_init: symbol not found`。
 */
#ifndef DSH_GL_H
#define DSH_GL_H

#include <stddef.h>
#include <sys/mman.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 共享文件头长度，与 App 侧 LinuxFrameSurface.HEADER_BYTES 必须一致 */
#define DSH_FRAME_HDR 16u
#define DSH_FRAME_MAGIC 0x4D415246u

typedef struct dsh_frame {
    int      width;
    int      height;
    int      fd;                    /* 共享文件 fd（-1 = 未打开） */
    unsigned char *map;             /* 整个映射区起点（= 文件头） */
    unsigned char *pixels;          /* 像素区起点（= map + DSH_FRAME_HDR） */
    size_t   bytes;                 /* 像素字节数 = w*h*4 */
    unsigned seq;                   /* 已 present 的帧数 */

    /* 取像素方式 */
    int      pixmode;               /* 0 = glReadPixels，1 = AHardwareBuffer */
    unsigned ahb_stride;            /* AHB 的实际行跨度（可能 > w*4，需按行拷） */

    /* 不透明：EGL/GL 句柄与函数指针。用户不要碰。 */
    void    *egl_display;
    void    *egl_surface;
    void    *egl_context;
    void    *gl_lib;
    void    *ahb;                   /* AHardwareBuffer*（pixmode=1 时有效） */
    void    *(*sym)(void *lib, const char *name);
} dsh_frame;

/*
 * 打开帧通道并初始化 GL（等价于 dsh_frame_open_ex(..., DSH_PIX_DEFAULT)）。
 *
 * @param w,h      画面尺寸（像素）。小于 16 会被夹到 16。
 * @param sharepath 共享文件路径；NULL 用默认
 *                  "/data/data/<pkg>/files/dshframe.bin" —— 但包名因设备而异，
 *                  建议由调用方传入（App 启动时会告诉容器真实路径）。
 * @return 0 成功；非 0 失败（错误原因用 dsh_gl_error() 取）
 */
int dsh_frame_open(dsh_frame *f, int w, int h, const char *sharepath);

/* 取像素方式 */
#define DSH_PIX_DEFAULT  0   /* 同 DSH_PIX_RPIX */
#define DSH_PIX_RPIX     0   /* glReadPixels 逐行回读（最稳，所有设备都能用） */
#define DSH_PIX_AHB      1   /* 渲染进 AHardwareBuffer 再 lock（快，但依赖 gralloc） */
#define DSH_PIX_AUTO     2   /* 有 AHB 就用，没有自动退回 rpix */

/*
 * 带取像素方式的重载。
 *
 * 为什么要这个：产线实测（模拟器 x86_64 / SwiftShader，各 60 帧）
 *   1280x720   rpix 210.4 fps -> ahb 286.0 fps   (1.36x)
 *   1920x1080  rpix  80.8 fps -> ahb 121.4 fps   (1.50x)
 * 像素逐字节一致。收益集中在「GPU→CPU 取像素」那一段。
 *
 * ★不管传什么 mode，**AHB 不可用时都会安静退回 rpix**，并用
 *   dsh_frame_pixmode_name() 报出实际用的方式。调用方不需要自己探测
 *   gralloc 支持度 —— 真机与模拟器行为不同（模拟器 createFromHandle 恒
 *   -22，见 pitfalls #41），让库来兜这件事。
 *
 * @return 0 成功（**含 AHB 探测失败后退回 rpix 的情况**）
 */
int dsh_frame_open_ex(dsh_frame *f, int w, int h, const char *sharepath, int pixmode);

/* 实际生效的取像素方式：返回 "ahb" 或 "rpix"（静态串） */
const char *dsh_frame_pixmode_name(const dsh_frame *f);

/*
 * 提交一帧：glFinish → 取像素（rpix 或 AHB lock）→ 写进共享文件 → 推进 seq。
 * 调用前请把要显示的图画在默认 framebuffer 上。
 * @return 0 成功
 */
int dsh_frame_present(dsh_frame *f);

/* 关闭并释放（会 munmap / close / eglTerminate） */
void dsh_frame_close(dsh_frame *f);

/* 最近一次失败的可读原因（静态缓冲，不要 free） */
const char *dsh_gl_error(void);

/* 裸写 stdout（bionic 非 zygote 进程不能用 printf，见 pitfalls #31） */
void dsh_write(const char *s);

/* 带参数的版本，支持 %d %u %s %x %ld，够诊断用 */
void dsh_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* DSH_GL_H */
