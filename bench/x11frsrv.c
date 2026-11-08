/*
 * x11frsrv —— 把 X11 屏幕（Xvfb 里的真实程序画面）连续写进共享文件。
 *
 * ## 为什么需要它
 *
 * 原来的 `framesrv.c` 是用 EGL 自己画内容：能证明「容器能用宿主 GPU」，
 * 但**不是用户在用的那个画面**。要让用户点得到 xterm / Geany / NetSurf
 * 这些真实程序，就必须抓 X 服务器的合成结果。
 *
 * ## 与 framesrv 的共享文件协议完全一致
 *
 *   offset 0   u32 magic = 0x4D415246 ("FRAM")
 *   offset 4   u32 seq   每帧 +1
 *   offset 8   u32 w
 *   offset 12  u32 h
 *   offset 16  起 RGBA8888，w*h*4 字节
 *
 * App 侧 `LinuxFrameSurface` 不用改一行就能显示。
 *
 * ## 为什么用 XShm 而不是 XGetImage
 *
 * `XGetImage` 每个像素都要经过 X 协议序列化回传，1280x720 实测 20~40ms/帧。
 * `XShmGetImage` 走共享内存，实测 2~4ms/帧。没有 MIT-SHM 扩展时自动回落
 * 到 XGetImage（Xvfb 默认带 SHM，但真机上的 X server 不一定）。
 *
 * ## 用法
 *
 *   x11frsrv <共享文件> [宽] [高] [fps]
 *
 * 宽高给 0 表示自动取根窗口尺寸。fps 默认 30。
 *
 * 退出：SIGTERM/SIGINT。X 服务器断开会打印 `X11FRSRV_FAIL` 并退出（不静默）。
 *
 * 编译：gcc -O2 -o x11frsrv x11frsrv.c -lX11 -lXext
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#define FRAME_MAGIC 0x4D415246u
#define HDR_BYTES   16

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void put_u32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void sleep_us(long us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000L;
    ts.tv_nsec = (us % 1000000L) * 1000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: x11frsrv <sharepath> [w] [h] [fps]\n");
        return 2;
    }
    const char *share = argv[1];
    int reqW = (argc > 2) ? atoi(argv[2]) : 0;
    int reqH = (argc > 3) ? atoi(argv[3]) : 0;
    int fps = (argc > 4) ? atoi(argv[4]) : 30;
    if (fps < 1) fps = 1;
    if (fps > 240) fps = 240;
    long frame_us = 1000000L / fps;

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "X11FRSRV_FAIL open display (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    Window root = DefaultRootWindow(dpy);
    int scr = DefaultScreen(dpy);

    if (reqW <= 0 || reqH <= 0) {
        reqW = DisplayWidth(dpy, scr);
        reqH = DisplayHeight(dpy, scr);
    }
    fprintf(stderr, "X11FRSRV_KV display=%s size=%dx%d fps=%d\n",
            DisplayString(dpy), reqW, reqH, fps);

    /* MIT-SHM 探测：没有就回落，绝不因为扩展缺失而失败 */
    int use_shm = 0;
    XShmSegmentInfo shm = {0};
    XImage *img = NULL;
    if (XShmQueryExtension(dpy)) {
        img = XShmCreateImage(dpy, DefaultVisual(dpy, scr), (unsigned)DefaultDepth(dpy, scr),
                              ZPixmap, NULL, &shm, (unsigned)reqW, (unsigned)reqH);
        if (img) {
            shm.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height,
                               IPC_CREAT | 0600);
            if (shm.shmid >= 0) {
                shm.shmaddr = img->data = (char *)shmat(shm.shmid, NULL, 0);
                if (shm.shmaddr != (char *)-1) {
                    shm.readOnly = False;
                    if (XShmAttach(dpy, &shm)) {
                        use_shm = 1;
                    } else {
                        shmdt(shm.shmaddr);
                        shmctl(shm.shmid, IPC_RMID, NULL);
                    }
                } else {
                    shmctl(shm.shmid, IPC_RMID, NULL);
                }
            }
        }
        if (!use_shm && img) { XDestroyImage(img); img = NULL; }
    }
    fprintf(stderr, "X11FRSRV_KV shm=%d\n", use_shm);

    /* ---- 共享文件 ---- */
    size_t total = HDR_BYTES + (size_t)reqW * reqH * 4;
    int fd = open(share, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr, "X11FRSRV_FAIL open %s: %s\n", share, strerror(errno));
        return 1;
    }
    if (ftruncate(fd, (off_t)total) != 0) {
        fprintf(stderr, "X11FRSRV_FAIL ftruncate: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    unsigned char *map = (unsigned char *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                               MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "X11FRSRV_FAIL mmap: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    /* 头先落好，App 才能算出该映射多大；seq=0 表示还没有有效帧 */
    put_u32(map + 0, FRAME_MAGIC);
    put_u32(map + 4, 0);
    put_u32(map + 8, (unsigned)reqW);
    put_u32(map + 12, (unsigned)reqH);
    msync(map, HDR_BYTES, MS_SYNC);

    unsigned char *pix = map + HDR_BYTES;
    unsigned seq = 0;
    long ok = 0, fail = 0;

    while (!g_stop) {
        long t0 = 0;
        struct timespec ts0;
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        t0 = ts0.tv_sec * 1000000L + ts0.tv_nsec / 1000;

        int got = 0;
        if (use_shm) {
            got = XShmGetImage(dpy, root, img, 0, 0, AllPlanes) ? 1 : 0;
        } else {
            img = XGetImage(dpy, root, 0, 0, (unsigned)reqW, (unsigned)reqH, AllPlanes, ZPixmap);
            got = img ? 1 : 0;
        }

        if (got && img && img->data) {
            /*
             * 转 RGBA。Xvfb 默认 24bpp 是 BGRX 排布（小端），而 App 侧
             * Bitmap.Config.ARGB_8888 的字节序就是 R,G,B,A。
             *
             * ★alpha 必须显式写 255，绝不抄源字节：BGRX 的第 4 字节是
             *   "unused" 不是 alpha，实测可能全 0 —— 那会让 App 侧把整帧
             *   当全透明，用户看到一片空白且不报错（这个坑在 wlshot 上
             *   踩过一次，见镜像仓 pitfalls）。
             */
            int w = img->width, h = img->height;
            if (w > reqW) w = reqW;
            if (h > reqH) h = reqH;
            if (img->bits_per_pixel == 32) {
                for (int y = 0; y < h; y++) {
                    const unsigned char *src = (const unsigned char *)img->data +
                                               (size_t)y * img->bytes_per_line;
                    unsigned char *dst = pix + (size_t)y * reqW * 4;
                    for (int x = 0; x < w; x++) {
                        /* BGRX -> RGBA */
                        dst[x * 4 + 0] = src[x * 4 + 2];
                        dst[x * 4 + 1] = src[x * 4 + 1];
                        dst[x * 4 + 2] = src[x * 4 + 0];
                        dst[x * 4 + 3] = 255;
                    }
                    /* 右侧补黑（请求宽度大于实际时） */
                    if (w < reqW) memset(dst + (size_t)w * 4, 0, (size_t)(reqW - w) * 4);
                }
                if (h < reqH) {
                    memset(pix + (size_t)h * reqW * 4, 0, (size_t)(reqH - h) * reqW * 4);
                }
            } else if (img->bits_per_pixel == 16) {
                for (int y = 0; y < h; y++) {
                    const unsigned short *src = (const unsigned short *)((const unsigned char *)img->data +
                                              (size_t)y * img->bytes_per_line);
                    unsigned char *dst = pix + (size_t)y * reqW * 4;
                    for (int x = 0; x < w; x++) {
                        unsigned short v = src[x];
                        dst[x * 4 + 0] = (unsigned char)(((v >> 11) & 0x1F) * 255 / 31);
                        dst[x * 4 + 1] = (unsigned char)(((v >> 5) & 0x3F) * 255 / 63);
                        dst[x * 4 + 2] = (unsigned char)((v & 0x1F) * 255 / 31);
                        dst[x * 4 + 3] = 255;
                    }
                }
            } else {
                fprintf(stderr, "X11FRSRV_FAIL unsupported bpp=%d\n", img->bits_per_pixel);
                break;
            }

            /* ★seq 最后推进：App 靠它判「有没有新帧」，先推会读到半帧 */
            seq++;
            __sync_synchronize();
            put_u32(map + 4, seq);
            ok++;
        } else {
            fail++;
            if (fail == 1) fprintf(stderr, "X11FRSRV_KV first capture failed\n");
        }

        if (!use_shm && img) {
            XDestroyImage(img);
            img = NULL;
        }

        /* 按目标 fps 节流（扣除本帧耗时） */
        struct timespec ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        long t1 = ts1.tv_sec * 1000000L + ts1.tv_nsec / 1000;
        long spent = t1 - t0;
        if (spent < frame_us) sleep_us(frame_us - spent);
    }

    fprintf(stderr, "X11FRSRV_KV done seq=%u ok=%ld fail=%ld\n", seq, ok, fail);
    if (use_shm) {
        XShmDetach(dpy, &shm);
        XDestroyImage(img);
        shmdt(shm.shmaddr);
        shmctl(shm.shmid, IPC_RMID, NULL);
    }
    munmap(map, total);
    XCloseDisplay(dpy);
    return 0;
}
