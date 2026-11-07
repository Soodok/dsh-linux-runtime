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

/* ---- 重连与退避（--loop 容错用）----
 *
 * 这几个函数只服务 --loop 的恢复路径，单帧模式走不到。
 *
 * 为什么需要：compositor 会重启（用户 `pkill sway` 再起、或崩溃后被拉起），
 * 那时 wl_display_roundtrip 返回 -1，老实现直接 break 退出 → 帧服务永久停，
 * 而 App 侧只看到画面静止。重连能让它在 compositor 回来后自己接上。
 */

/* 毫秒退避：1,2,4,8...最多 2000ms。不做精确调度，只为别把 CPU 打满。 */
static void backoff_ms(int consec) {
    int ms = 100 << (consec > 5 ? 5 : (consec > 0 ? consec - 1 : 0));
    if (ms > 2000) ms = 2000;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
}

/* 重建共享文件（尺寸变化时用）。
 *
 * 布局：[16B header][pixels]。header 存 magic/seq/w/h，seq 最后写。
 * 重建后 seq 会从头来过也没关系 —— App 侧只比对「变了没有」。
 * 返回 0 成功。
 */
static int fw_reinit(int *pfd, void **pmap, unsigned char **phdr, size_t *ptotal,
                     size_t *ppixbytes, int w, int h,
                     unsigned *pout_w, unsigned *pout_h, const char *share) {
    size_t pix = (size_t)w * (size_t)h * 4u;
    size_t tot = DSH_FRAME_HDR + pix;

    if (*pmap) { munmap(*pmap, *ptotal); *pmap = NULL; }
    if (*pfd >= 0) { close(*pfd); *pfd = -1; }

    int fd = open(share, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        fprintf(stderr, "FAIL reinit open %s: %s\n", share, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)tot) != 0) {
        fprintf(stderr, "FAIL reinit ftruncate: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    void *m = mmap(NULL, tot, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        fprintf(stderr, "FAIL reinit mmap: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    *pfd = fd;
    *pmap = m;
    *phdr = (unsigned char *)m;
    *ptotal = tot;
    *ppixbytes = pix;
    *pout_w = (unsigned)w;
    *pout_h = (unsigned)h;

    /* magic 先写、seq 清零 —— 下一帧成功后会写真实 seq */
    put_u32(*phdr + 0, DSH_FRAME_MAGIC);
    put_u32(*phdr + 4, 0u);
    put_u32(*phdr + 8, (unsigned)w);
    put_u32(*phdr + 12, (unsigned)h);
    return 0;
}

/* 重连 display 并重新绑 global。
 *
 * 注意 outputs 数组是 realloc 来的，重连时先释放旧指针再清零重生，
 * 否则会泄漏并让 outputs[0] 指向已销毁的 wl_output。
 * 返回 0 成功。
 */
static int reconnect(struct wl_display **pdisp, struct wl_shm **pshm,
                     struct zwlr_screencopy_manager_v1 **pscm,
                     struct wl_output ***poutputs, int *pn_outputs) {
    if (*pdisp) { wl_display_disconnect(*pdisp); *pdisp = NULL; }

    /* 先清全局，registry 回调会重新填 */
    *pshm = NULL;
    *pscm = NULL;
    if (*poutputs) { free(*poutputs); *poutputs = NULL; }
    *pn_outputs = 0;
    cap_outputs = 0;

    struct wl_display *d = wl_display_connect(NULL);
    if (!d) return -1;
    struct wl_registry *reg = wl_display_get_registry(d);
    if (!reg) { wl_display_disconnect(d); return -1; }
    wl_registry_add_listener(reg, &reg_listener, NULL);
    if (wl_display_roundtrip(d) < 0) {
        wl_display_disconnect(d);
        return -1;
    }

    *pdisp = d;
    *pshm = shm;
    *pscm = scm;
    *poutputs = outputs;
    *pn_outputs = n_outputs;

    if (!*pshm || !*pscm || *pn_outputs <= 0) {
        wl_display_disconnect(d);
        *pdisp = NULL;
        return -1;
    }
    return 0;
}

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
    int consec_fail = 0;
    const int MAX_CONSEC = 30;   /* 连续失败 30 帧（约 15s@2fps）才放弃 */

    for (;; iter++) {
        if (iter > 0) {
            f_w = 0; f_h = 0; f_ready = 0; failed = 0; done = 0;
            fr = zwlr_screencopy_manager_v1_capture_output(scm, 0, outputs[0]);
            zwlr_screencopy_frame_v1_add_listener(fr, &frame_listener, NULL);
            for (spin = 0; spin < timeout_ms / 10 && f_w == 0 && !failed; spin++) {
                if (wl_display_roundtrip(disp) < 0) { failed = 2; break; }
            }

            /* ★出错不退出，退避后继续。
             *
             * 原来的实现这里 `break` 直接跳出去 —— compositor 重启一次，
             * 帧服务就永久停了，而 App 侧只是「画面不动」，看不到任何错误。
             * 单帧失败是可恢复的：重建 frame 对象重来即可。 */
            if (failed || f_w <= 0) {
                consec_fail++;
                if (consec_fail == 1 || consec_fail % MAX_CONSEC == 0) {
                    fprintf(stderr,
                            "WARN loop frame %d: no buffer event (failed=%d, consec=%d)\n",
                            iter, failed, consec_fail);
                }
                if (fr) { zwlr_screencopy_frame_v1_destroy(fr); fr = NULL; }
                /* failed==2 表示 roundtrip 返回 <0，连接大概率断了 → 重建。
                 *
                 * ★重连失败时绝不能直接 continue：reconnect() 失败会把
                 *   scm/outputs 清成 NULL，下一轮开头 capture_output(NULL,...)
                 *   会段错误，进程无日志消失（我第一版就这么死的）。
                 *   所以这里在内层循环里重试，直到连上或耗尽次数。 */
                while (failed == 2) {
                    if (consec_fail >= MAX_CONSEC) {
                        fprintf(stderr,
                                "WARN loop frame %d: giving up after %d failures\n",
                                iter, consec_fail);
                        goto giveup;
                    }
                    if (reconnect(&disp, &shm, &scm, &outputs, &n_outputs) == 0) {
                        fprintf(stderr, "WARN loop frame %d: reconnected\n", iter);
                        /* ★重连后 buf 属于旧 display 的 shm，已失效。
                         *   把 out_w 清零，逼下面的「尺寸变化」分支重建
                         *   wl_buffer；否则尺寸相同时不重建 → 悬垂指针。 */
                        out_w = 0; out_h = 0;
                        /* buf 也属于旧 display 的 shm，同样不能再用。置 NULL
                         * 让下面的重建分支跳过 destroy，只 create 新的。 */
                        buf = NULL;
                        break;
                    }
                    consec_fail++;
                    backoff_ms(consec_fail);
                }
                if (consec_fail >= MAX_CONSEC) goto giveup;
                backoff_ms(consec_fail);
                continue;
            }

            /* 尺寸变化：共享文件按新尺寸重建，不退出。
             * App 侧读 header 的 w/h，会自动跟着变。 */
            if (f_w != (int)out_w || f_h != (int)out_h) {
                fprintf(stderr, "WARN loop frame %d: size changed %dx%d -> %dx%d\n",
                        iter, out_w, out_h, f_w, f_h);
                if (fw_reinit(&fd, &map, &hdr, &total, &pixbytes,
                              f_w, f_h, &out_w, &out_h, share) != 0) {
                    fprintf(stderr, "WARN loop frame %d: reinit share failed\n", iter);
                    consec_fail++;
                    if (consec_fail >= MAX_CONSEC) goto giveup;
                    backoff_ms(consec_fail);
                    continue;
                }
                /* shm buffer 也要按新尺寸重建 */
                if (ftruncate(tfd, (off_t)pixbytes) != 0) {
                    fprintf(stderr, "WARN loop frame %d: tmp ftruncate: %s\n",
                            iter, strerror(errno));
                    consec_fail++;
                    if (consec_fail >= MAX_CONSEC) goto giveup;
                    backoff_ms(consec_fail);
                    continue;
                }
                pool = wl_shm_create_pool(shm, tfd, (int32_t)pixbytes);
                if (buf) { wl_buffer_destroy(buf); buf = NULL; }
                buf = wl_shm_pool_create_buffer(pool, 0, f_w, f_h,
                                                (int32_t)stride, f_format);
                wl_shm_pool_destroy(pool);
            }
        }

        done = 0; f_ready = 0; failed = 0;
        zwlr_screencopy_frame_v1_copy(fr, buf);

        for (spin = 0; spin < timeout_ms / 10 && !done; spin++) {
            if (wl_display_roundtrip(disp) < 0) break;
        }
        if (failed || !f_ready) {
            /* 同样是可恢复错误：copy 超时/被取消，下一帧重来即可。 */
            consec_fail++;
            if (consec_fail == 1 || consec_fail % MAX_CONSEC == 0) {
                fprintf(stderr,
                        "WARN frame %d: copy failed=%d ready=%d (consec=%d)\n",
                        iter, failed, f_ready, consec_fail);
            }
            if (fr) { zwlr_screencopy_frame_v1_destroy(fr); fr = NULL; }
            if (consec_fail >= MAX_CONSEC) goto giveup;
            backoff_ms(consec_fail);
            continue;
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
        consec_fail = 0;   /* 成功一帧就清零 */

        if (!loop_mode) break;
        if (loop_fps > 0) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000000L / loop_fps;
            while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
        }
    }

giveup:
    if (frames_ok == 0 || consec_fail > 0) {
        fprintf(stderr, "WARN final: frames_ok=%d consec_fail=%d\n",
                frames_ok, consec_fail);
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
