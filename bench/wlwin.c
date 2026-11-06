/*
 * wlwin.c —— 最小的 Wayland 顶层窗口客户端，证明「容器里真的能开窗口」
 *
 * 为什么不用现成的：
 *   - foot 需要字体（Alpine 容器没 monospace）和 wl_seat（headless 后端不提供）
 *   - weston-simple-shm 在 Alpine 没有对应包（weston-demo 不存在）
 * 所以自己写一个只依赖 wayland-client + xdg-shell 的最小客户端：
 *   连 compositor -> wl_shm 造 buffer -> xdg_toplevel 设标题 -> attach -> commit
 * 能拿到 xdg_surface.configure 并成功 commit，就证明窗口链路真的通
 * （configure 是 compositor 对「你要开一个顶层窗口」的正式回应，不是本地日志）。
 *
 * 用法：
 *   WAYLAND_DISPLAY=wl-t1 ./wlwin          # 挂 3 秒
 *   WAYLAND_DISPLAY=wl-t1 ./wlwin 30       # 挂 30 秒
 *
 * 输出契约（脚本按这些行断言）：
 *   WLWIN_KV compositor=<version>
 *   WLWIN_KV xdg_wm_base=<version>
 *   WLWIN_KV shm=<version>
 *   WLWIN_KV configure=<w>x<h>
 *   WLWIN_KV committed=<n>
 *   RESULT: WLWIN_OK
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "presentation-time-client-protocol.h"

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;
static struct wp_presentation *presentation;
static unsigned v_compositor = 0, v_wm_base = 0, v_shm = 0, v_pres = 0;
static int presented_count = 0;
static unsigned long long presented_ns = 0;
static int frame_count = 0;

static int have_configure = 0;
static int cfg_w = 0, cfg_h = 0;

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = { wm_base_ping };

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(s, serial);
    have_configure = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = { xdg_surface_configure };

static void toplevel_configure(void *d, struct xdg_toplevel *t,
                               int32_t w, int32_t h, struct wl_array *states) {
    (void)d; (void)t; (void)states;
    cfg_w = w; cfg_h = h;
}
static void toplevel_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
/* xdg_toplevel v4 起新增这些事件；绑定版本 5 就必须都提供 listener，
   否则 libwayland 在收到未知 opcode 时直接 abort（实测 rc=134）。 */
static void toplevel_configure_bounds(void *d, struct xdg_toplevel *t,
                                      int32_t w, int32_t h) {
    (void)d; (void)t; (void)w; (void)h;
}
static void toplevel_wm_capabilities(void *d, struct xdg_toplevel *t, struct wl_array *caps) {
    (void)d; (void)t; (void)caps;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

/* wp_presentation 的 presented 事件是「这一帧真的被显示到输出上了」的
   权威证据 —— 不同于 surface.commit（那只是客户端提交），它由 compositor
   在完成合成并送显后发出。有它才能说「窗口真的上了屏幕」。 */
static void pres_clock_id(void *d, struct wp_presentation *p, uint32_t clk) {
    (void)d; (void)p; (void)clk;
}
static void pres_presented(void *d, struct wp_presentation *p, uint32_t tv_sec_hi,
                           uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh,
                           uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
    (void)d; (void)p; (void)refresh; (void)seq_hi; (void)seq_lo; (void)flags;
    presented_count++;
    presented_ns = ((unsigned long long)tv_sec_hi << 32 | tv_sec_lo) * 1000000000ULL
                   + (unsigned long long)tv_nsec;
}
static const struct wp_presentation_listener pres_listener = {
    pres_clock_id, pres_presented
};

/* wl_surface_frame 的回调：compositor 在准备开始下一帧绘制时触发。
   比 presented 更基础，所有后端（含 headless）都会给。 */
static void frame_done(void *d, struct wl_callback *cb, uint32_t t) {
    (void)d; (void)t;
    frame_count++;
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void registry_global(void *d, struct wl_registry *r, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)d;
    if (strcmp(iface, "wl_compositor") == 0) {
        v_compositor = version < 4 ? version : 4;
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, v_compositor);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        v_wm_base = version < 5 ? version : 5;
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, v_wm_base);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(iface, "wl_shm") == 0) {
        v_shm = version < 1 ? version : 1;
        shm = wl_registry_bind(r, name, &wl_shm_interface, v_shm);
    } else if (strcmp(iface, "wp_presentation") == 0) {
        v_pres = version < 1 ? version : 1;
        presentation = wl_registry_bind(r, name, &wp_presentation_interface, v_pres);
        wp_presentation_add_listener(presentation, &pres_listener, NULL);
    }
}
static const struct wl_registry_listener registry_listener = { registry_global, NULL };

/* 造一块 shm buffer，画横向橙→蓝渐变，让截图里有可辨认内容 */
static struct wl_buffer *make_buffer(int w, int h, void **out_data, size_t *out_size) {
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    char path[] = "/tmp/wlwin-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return NULL; }
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) { perror("ftruncate"); close(fd); return NULL; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                                      WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    unsigned char *p = (unsigned char *)data;
    int y, x;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char *q = p + (size_t)y * stride + (size_t)x * 4;
            int t = (w > 1) ? (x * 255 / (w - 1)) : 0;
            /* XRGB8888 小端字节序：B, G, R, X */
            q[0] = (unsigned char)(255 - t);   /* B */
            q[1] = 89;                          /* G */
            q[2] = (unsigned char)t;            /* R */
            q[3] = 0;
        }
    }
    *out_data = data;
    *out_size = size;
    return buf;
}

int main(int argc, char **argv) {
    int hold = (argc > 1) ? atoi(argv[1]) : 3;
    int w = (argc > 2) ? atoi(argv[2]) : 320;
    int h = (argc > 3) ? atoi(argv[3]) : 200;
    if (hold < 1) hold = 1;

    struct wl_display *disp = wl_display_connect(NULL);
    if (!disp) {
        fprintf(stderr, "FAIL wl_display_connect (WAYLAND_DISPLAY=%s)\n",
                getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
        printf("RESULT: WLWIN_FAIL connect\n");
        return 2;
    }

    struct wl_registry *reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(disp);
    wl_display_roundtrip(disp);

    if (!compositor || !wm_base || !shm) {
        fprintf(stderr, "FAIL missing globals compositor=%d wm_base=%d shm=%d\n",
                compositor != NULL, wm_base != NULL, shm != NULL);
        printf("RESULT: WLWIN_FAIL globals\n");
        return 3;
    }
    printf("WLWIN_KV compositor=%u\n", v_compositor);
    printf("WLWIN_KV xdg_wm_base=%u\n", v_wm_base);
    printf("WLWIN_KV shm=%u\n", v_shm);

    struct wl_surface *surf = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
    struct xdg_toplevel *tl = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(tl, &toplevel_listener, NULL);
    xdg_toplevel_set_title(tl, "dsh-container-window");
    xdg_toplevel_set_app_id(tl, "org.dsh.wlwin");

    /* 先 commit 一次空 surface，让 compositor 发 configure */
    wl_surface_commit(surf);
    wl_display_roundtrip(disp);

    int i;
    for (i = 0; i < 50 && !have_configure; i++) {
        if (wl_display_dispatch(disp) < 0) break;
    }
    if (!have_configure) {
        fprintf(stderr, "FAIL no xdg_surface.configure\n");
        printf("RESULT: WLWIN_FAIL configure\n");
        return 4;
    }
    printf("WLWIN_KV configure=%dx%d\n", cfg_w, cfg_h);

    void *data = NULL;
    size_t size = 0;
    struct wl_buffer *buf = make_buffer(w, h, &data, &size);
    if (!buf) {
        printf("RESULT: WLWIN_FAIL buffer\n");
        return 5;
    }
    /* 挂 frame callback：compositor 完成这一帧时回调 */
    {
        struct wl_callback *cb = wl_surface_frame(surf);
        wl_callback_add_listener(cb, &frame_listener, NULL);
    }

    /* ★必须挂 feedback，否则 compositor 不会为这次 commit 发 presented 事件 */
    if (presentation) {
        struct wp_presentation_feedback *fb = wp_presentation_feedback(presentation, surf);
        (void)fb;
        wl_display_flush(disp);
    }

    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage_buffer(surf, 0, 0, w, h);
    wl_surface_commit(surf);
    wl_display_flush(disp);
    printf("WLWIN_KV committed=1 size=%zux%zu\n", (size_t)w, (size_t)h);

    /* ★等 presented。这是「窗口真的上了屏幕」的唯一权威证据：
       surface.commit 只说明客户端提交了 buffer，presented 由 compositor
       在完成合成并送显后发出。headless+pixman 下也会发。 */
    {
        int spin;
        for (spin = 0; spin < 300 && frame_count == 0 && presented_count == 0; spin++) {
            if (wl_display_dispatch_pending(disp) < 0) break;
            {
                struct pollfd pfd;
                pfd.fd = wl_display_get_fd(disp);
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 10) > 0) {
                    if (wl_display_dispatch(disp) < 0) break;
                } else {
                    wl_display_flush(disp);
                }
            }
        }
    }
    printf("WLWIN_KV frame_callback=%d\n", frame_count);
    printf("WLWIN_KV presented=%d\n", presented_count);
    if (presented_count > 0) printf("WLWIN_KV presented_ns=%llu\n", presented_ns);

    /* 挂一会儿，让 compositor 有机会合成这一帧 */
    struct timespec ts = { hold, 0 };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) { }
    wl_display_flush(disp);

    munmap(data, size);
    wl_buffer_destroy(buf);
    xdg_toplevel_destroy(tl);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surf);
    wl_display_disconnect(disp);

    printf("RESULT: WLWIN_OK\n");
    return 0;
}
