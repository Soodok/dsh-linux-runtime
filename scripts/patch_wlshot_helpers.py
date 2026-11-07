#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""补上 wlshot.c 重连循环需要的三个辅助函数。

patch_wlshot_loop.py 已经调用 reconnect() / fw_reinit() / backoff_ms()，
但没定义。这里补上实现。
"""
import io
import sys

PATH = "bench/wlshot.c"

HELPERS = '''
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
        fprintf(stderr, "FAIL reinit open %s: %s\\n", share, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)tot) != 0) {
        fprintf(stderr, "FAIL reinit ftruncate: %s\\n", strerror(errno));
        close(fd);
        return -1;
    }
    void *m = mmap(NULL, tot, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        fprintf(stderr, "FAIL reinit mmap: %s\\n", strerror(errno));
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

'''

ANCHOR = "int main(int argc, char **argv) {"


def main():
    with io.open(PATH, encoding="utf-8") as f:
        s = f.read()
    if "static void backoff_ms(int consec)" in s:
        print("already added")
        return 0
    if ANCHOR not in s:
        print("MISS anchor")
        return 2
    s = s.replace(ANCHOR, HELPERS.lstrip("\n") + ANCHOR, 1)
    io.open(PATH, "w", encoding="utf-8", newline="\n").write(s)
    print("added helpers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
