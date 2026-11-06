/*
 * wlshot.c —— 用 wlr-screencopy 抓 compositor 画面，**直接写进 dsh_gl 共享文件**
 *
 * 这个程序的定位：把「容器里能开窗口」补完到「窗口画面能进 App」。
 *
 * 之前的两条路各有缺口：
 *   - weston_capture_v1 有授权模型（只授予 screen-share 拉起的 RDP 客户端），
 *     grim 在 weston 下报 unauthorized（pitfalls #36）
 *   - lib/dsh_gl 走的是 EGL + AHB/glReadPixels，只对**自己渲染**的程序有效，
 *     拿不到别人窗口的画面
 *
 * wlr-screencopy 是 wlroots 系（sway）的标准合成抓取协议，无授权限制。
 * 抓到的 ARGB8888 直接拷进共享文件，App 侧 LinuxFrameSurface 零改动就能显示。
 *
 * 两种模式：
 *   单帧（默认）：抓一帧写文件就退出 —— 适合验收与一次性快照
 *   持续（--loop N）：按间隔反复抓，seq 递增 —— 适合当「桌面镜像」用
 *
 * 用法：
 *   WAYLAND_DISPLAY=wayland-1 ./wlshot <sharepath> [timeout_ms] [--loop [fps]]
 *
 * 输出契约（单帧）：
 *   WLSHOT_KV output=<w>x<h>
 *   WLSHOT_KV format=0x<fmt>
 *   WLSHOT_KV copied=1
 *   WLSHOT_KV bytes=<n>
 *   RESULT: WLSHOT_OK | WLSHOT_FAIL <stage>
 *
 * 输出契约（持续）：
 *   WLSHOT_KV mode=loop fps=<n>
 *   WLSHOT_KV frames=<k> elapsed_ms=<t>
 *   RESULT: WLSHOT_LOOP_OK | WLSHOT_FAIL <stage>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <wayland-client.h>
#include "wlr-screencopy-client-protocol.h"

#define DSH_FRAME_HDR   16u
#define DSH_FRAME_MAGIC 0x4D415246u

static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *scm;
static struct wl_output **outputs;
static int n_outputs, cap_outputs;

static int done = 0;
static int failed = 0;

static void put_u32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ---- screencopy 回调 ---- */
static int  f_w = 0, f_h = 0;
static unsigned f_format = 0;
static int  f_ready = 0;

static void ev_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                      uint32_t w, uint32_t h, uint32_t stride) {
    (void)d; (void)f;
    f_format = format; f_w = (int)w; f_h = (int)h;
}

static void ev_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {
    (void)d; (void)f; (void)flags;
}

static void ev_ready(void *d, struct zwlr_screencopy_frame_v1 *f,
                     uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    (void)d; (void)f; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    f_ready = 1; done = 1;
}

static void ev_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
    (void)d; (void)f;
    failed = 1; done = 1;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    ev_buffer, ev_flags, ev_ready, ev_failed,
    /* v3 起还有 linux_dmabuf / buffer_done，绑定 v1 就不会收到 */
};

/* ---- registry ---- */
static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)d;
    if (strcmp(iface, "wl_shm") == 0) {
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, "zwlr_screencopy_manager_v1") == 0) {
        scm = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface,
                               ver < 1 ? ver : 1);
    } else if (strcmp(iface, "wl_output") == 0) {
        if (n_outputs == cap_outputs) {
            cap_outputs = cap_outputs ? cap_outputs * 2 : 4;
            outputs = realloc(outputs, sizeof(struct wl_output *) * (size_t)cap_outputs);
        }
        outputs[n_outputs++] = wl_registry_bind(r, name, &wl_output_interface, 1);
    }
}
static const struct wl_registry_listener reg_listener = { reg_global, NULL };

int main(int argc, char **argv) {
    const char *share = (argc > 1) ? argv[1] : NULL;
    int timeout_ms = (argc > 2) ? atoi(argv[2]) : 3000;

    if (!share) {
        fprintf(stderr, "usage: %s <sharepath> [timeout_ms]\n", argv[0]);
        printf("RESULT: WLSHOT_FAIL usage\n");
        return 2;
    }

    struct wl_display *disp = wl_display_connect(NULL);
    if (!disp) {
        fprintf(stderr, "FAIL wl_display_connect (WAYLAND_DISPLAY=%s)\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
        printf("RESULT: WLSHOT_FAIL connect\n");
        return 3;
    }

    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);

    if (!shm || !scm || n_outputs == 0) {
        fprintf(stderr, "FAIL missing globals shm=%d scm=%d outputs=%d\n",
                shm != NULL, scm != NULL, n_outputs);
        printf("RESULT: WLSHOT_FAIL globals\n");
        return 4;
    }

    /* 第一次只抓 buffer 事件拿尺寸与格式，再建 shm pool 抓真帧。
       screencopy 要求：收到 buffer 事件后才能提交 buffer。 */
    struct zwlr_screencopy_frame_v1 *fr = zwlr_screencopy_manager_v1_capture_output(
        scm, 0, outputs[0]);
    zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, NULL);

    int spin;
    for (spin = 0; spin < timeout_ms / 10 && f_w == 0 && !failed; spin++) {
        if (wl_display_roundtrip(disp) < 0) break;
    }
    if (failed || f_w <= 0) {
        fprintf(stderr, "FAIL no buffer event (w=%d failed=%d)\n", f_w, failed);
        printf("RESULT: WLSHOT_FAIL buffer_event\n");
        return 5;
    }
    printf("WLSHOT_KV output=%dx%d\n", f_w, f_h);
    printf("WLSHOT_KV format=0x%x\n", f_format);

    /* 输出尺寸固定下来：flush 模式尺寸变了没法原地换 buffer，直接退出比
       静默画出错位画面好（App 侧 resize 走新的映射路径）。 */
    unsigned out_w = (unsigned)f_w, out_h = (unsigned)f_h;

    /* 建共享文件（含 dsh_gl 头，格式与 LinuxFrameSurface.kt 一致） */
    size_t stride = (size_t)f_w * 4u;
    size_t pixbytes = stride * (size_t)f_h;
    size_t total = (size_t)DSH_FRAME_HDR + pixbytes;

    int fd = open(share, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        fprintf(stderr, "FAIL open %s: %s\n", share, strerror(errno));
        printf("RESULT: WLSHOT_FAIL open\n");
        return 6;
    }
    if (ftruncate(fd, (off_t)total) != 0) {
        fprintf(stderr, "FAIL ftruncate: %s\n", strerror(errno));
        close(fd);
        printf("RESULT: WLSHOT_FAIL ftruncate\n");
        return 7;
    }
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "FAIL mmap: %s\n", strerror(errno));
        close(fd);
        printf("RESULT: WLSHOT_FAIL mmap\n");
        return 8;
    }
    unsigned char *hdr = (unsigned char *)map;
    unsigned char *pix = hdr + DSH_FRAME_HDR;

    /* 单独建一块 shm pool 给 screencopy（不能用同一块：协议要求
       wl_shm buffer 由 compositor 写，我们的文件不能被它直接写）。 */
    char tmppath[] = "/dev/shm/wlshot-XXXXXX";
    int tfd = mkstemp(tmppath);
    if (tfd < 0) {
        fprintf(stderr, "FAIL mkstemp: %s\n", strerror(errno));
        printf("RESULT: WLSHOT_FAIL mkstemp\n");
        return 9;
    }
    unlink(tmppath);
    if (ftruncate(tfd, (off_t)pixbytes) != 0) {
        fprintf(stderr, "FAIL tmp ftruncate: %s\n", strerror(errno));
        close(tfd);
        printf("RESULT: WLSHOT_FAIL tmp\n");
        return 10;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, tfd, (int32_t)pixbytes);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, f_w, f_h,
                                                      (int32_t)stride, f_format);
    wl_shm_pool_destroy(pool);

    /* ---- 抓帧（单帧或持续）----
     *
     * --loop 模式下反复抓同一 output 写进同一共享文件、seq 递增。
     * App 侧 LinuxFrameSurface 的脏判据就是 seq 变化，所以 wlshot 这样
     * 就能当「容器桌面镜像」用，而不是只能拍一张快照。
     *
     * ★每帧都要新建 zwlr_screencopy_frame_v1 —— 协议规定一个 frame 对象
     *   只能 copy 一次；但 wl_buffer 可以复用（ready 之后才允许下次 copy）。
     */
    int loop_mode = (argc > 3 && strcmp(argv[3], "--loop") == 0);
    int loop_fps = 0;
    if (loop_mode) {
        loop_fps = (argc > 4) ? atoi(argv[4]) : 2;
        if (loop_fps < 1) loop_fps = 1;
        if (loop_fps > 60) loop_fps = 60;
    }

    unsigned seq = 0;
    int iter = 0;
    int frames_ok = 0;

    for (;; iter++) {
        if (iter > 0) {
            f_w = 0; f_h = 0; f_ready = 0; failed = 0; done = 0;
            fr = zwlr_screencopy_manager_v1_capture_output(scm, 0, outputs[0]);
            zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, NULL);
            for (spin = 0; spin < timeout_ms / 10 && f_w == 0 && !failed; spin++) {
                if (wl_display_roundtrip(disp) < 0) break;
            }
            if (failed || f_w <= 0) {
                fprintf(stderr, "FAIL loop frame %d: no buffer event\n", iter);
                break;
            }
            if (f_w != (int)out_w || f_h != (int)out_h) {
                fprintf(stderr, "FAIL loop frame %d: size changed %dx%d -> %dx%d\n",
                        iter, out_w, out_h, f_w, f_h);
                break;
            }
        }

        done = 0; f_ready = 0; failed = 0;
        zwlr_screencopy_frame_v1_copy(fr, buf);

        for (spin = 0; spin < timeout_ms / 10 && !done; spin++) {
            if (wl_display_roundtrip(disp) < 0) break;
        }
        if (failed || !f_ready) {
            fprintf(stderr, "FAIL frame %d: copy failed=%d ready=%d\n",
                    iter, failed, f_ready);
            break;
        }

        /* 从 tmp 映射读到共享文件。XRGB8888 小端字节序是 B,G,R,X */
        void *src = mmap(NULL, pixbytes, PROT_READ, MAP_SHARED, tfd, 0);
        if (src == MAP_FAILED) {
            fprintf(stderr, "FAIL frame %d: src mmap: %s\n", iter, strerror(errno));
            break;
        }
        {
            const unsigned char *s = (const unsigned char *)src;
            size_t i;
            /*
             * ★alpha 必须显式写 255，不能从 XRGB8888 的第 4 字节抄。
             *
             * wlr-screencopy 在 headless/pixman 下给的格式是
             * WL_SHM_FORMAT_XRGB8888 (0x1)，第 4 字节是 **X（未定义）**。
             * 实测同一个窗口内 alpha 全是 0、窗口外是 255 —— 完全是垃圾。
             *
             * 而 App 侧 LinuxFrameSurface 用 Bitmap.Config.ARGB_8888 +
             * copyPixelsFromBuffer，会**当真读 alpha**：alpha=0 的区域被当成
             * 全透明，用户在 App 里看到的是一片空白 —— 数据其实是对的，
             * 但画面消失，且不报任何错。
             * 这个坑用「共享文件字节数正确 + magic 正确」查不出来，
             * 必须逐像素看 alpha 才会发现（pitfalls #45）。
             */
            for (i = 0; i + 3 < pixbytes; i += 4) {
                pix[i + 0] = s[i + 2];  /* R */
                pix[i + 1] = s[i + 1];  /* G */
                pix[i + 2] = s[i + 0];  /* B */
                pix[i + 3] = 255;       /* A —— 不抄 s[i+3] */
            }
        }
        munmap(src, pixbytes);

        seq++;
        frames_ok++;

        /* 头最后写：seq 是「有新帧」的唯一判据，必须最后落 */
        put_u32(hdr + 0, DSH_FRAME_MAGIC);
        put_u32(hdr + 4, seq);
        put_u32(hdr + 8, out_w);
        put_u32(hdr + 12, out_h);

        zwlr_screencopy_frame_v1_destroy(fr);
        fr = NULL;

        if (!loop_mode) break;
        if (loop_fps > 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000000L / loop_fps;
            while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
        }
    }

    printf("WLSHOT_KV copied=%d\n", frames_ok);
    printf("WLSHOT_KV seq=%u\n", seq);
    printf("WLSHOT_KV bytes=%zu\n", pixbytes);

    if (fr) zwlr_screencopy_frame_v1_destroy(fr);
    munmap(map, total);
    close(fd);
    close(tfd);
    wl_buffer_destroy(buf);
    wl_display_disconnect(disp);

    if (frames_ok == 0) {
        printf("RESULT: WLSHOT_FAIL no_frames\n");
        return 11;
    }
    printf("RESULT: WLSHOT_OK\n");
    return 0;
}
