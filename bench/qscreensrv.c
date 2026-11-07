/*
 * qscreensrv.c —— QEMU 虚拟机的持续帧服务 + 输入注入（容器内 musl 程序）
 *
 * 为什么需要它：
 *   「手机里跑模拟器」只做到「能启动」没有意义，必须是**看得见 + 点得动**。
 *   QEMU 的 -display vnc 在 guest 有 fb0 之后仍报 desktop 0x0（QEMU 10 + -vga std
 *   的已知现象），所以走 monitor 通道：
 *     screendump  →  9ms/帧（实测，111fps）
 *     pmemsave    → 31ms/帧，固定 16MB（是 screendump 的 3.4 倍，不用）
 *
 * 它做的事（单进程，一条 monitor 长连接到底）：
 *   1. 连 QEMU monitor TCP
 *   2. 循环：发 screendump → 读 PPM(P6) → 转 RGBA → 写共享文件 → seq++
 *   3. 从 stdin 读命令，转发给 monitor 实现输入注入（sendkey / mouse_move 等）
 *
 * 共享文件格式与 dsh_gl 完全一致，App 侧 LinuxFrameSurface.kt 直接复用：
 *   offset 0   u32 magic = 0x4D415246
 *   offset 4   u32 seq
 *   offset 8   u32 w
 *   offset 12  u32 h
 *   offset 16  RGBA8888
 *
 * ★为什么不用 printf 打日志：容器里经 sh -c 启动时 stdout 可能被 QEMU 串口
 *   占用，用裸 write(2,...) 到 stderr 更可靠。
 *
 * 用法：
 *   qscreensrv <sharepath> <monitor_host> <monitor_port> [fps] [--input-stdin]
 *   例：qscreensrv /tmp/qemu_frame.bin 127.0.0.1 4460 10 --input-stdin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <sys/select.h>

#define MAGIC 0x4D415246u
#define HDR   16u
#define PPM_TMP "/tmp/_qs.ppm"

static void say(const char *s) { write(2, s, strlen(s)); }

static void sayf(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) write(2, b, (size_t)n);
}
static int tcp_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

/* 等 monitor 回 (qemu) 提示符 —— 确认命令执行完，否则读到的是上一次的残留 */
static int drain_until_prompt(int fd) {
    char buf[4096];
    int guard = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) return -1;
        for (ssize_t i = 0; i + 5 <= n; i++) {
            if (memcmp(buf + i, "(qemu)", 6) == 0) return 0;
        }
        if (++guard > 200) return -1;   /* 防死等 */
    }
}

static void put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* 读 PPM(P6) 头部，拿到宽高与像素起点偏移
 *
 * ★踩过：maxval 读完后**无条件只跳 1 字节**是错的。若 raster 前是 `\r\n`
 *   （合法且常见），会只跳过 `\r`，把 `\n` 留在像素数据开头 → 整帧 RGB
 *   错位 1 字节，表现为色偏/撕裂，且**完全不报错**。
 *   必须按「一个空白分隔符」处理：`\r\n` 算一个，其它单字节算一个。
 */
static int ppm_header(const unsigned char *b, size_t len,
                      unsigned *w, unsigned *h, size_t *off) {
    size_t i = 0;
    int field = 0;
    unsigned vals[3] = {0, 0, 0};
    if (len < 2 || b[0] != 'P' || b[1] != '6') return -1;
    i = 2;
    while (i < len && field < 3) {
        /* 字段之间允许任意空白与注释 */
        while (i < len && (b[i] == ' ' || b[i] == '\n' || b[i] == '\r' || b[i] == '\t')) i++;
        if (i < len && b[i] == '#') { while (i < len && b[i] != '\n') i++; continue; }
        unsigned v = 0;
        int got = 0;
        while (i < len && b[i] >= '0' && b[i] <= '9') {
            /* 防溢出：PPM 的宽高/最大值都在 32 位内，超了就是坏文件 */
            if (v > 100000000u) return -1;
            v = v * 10 + (unsigned)(b[i] - '0'); i++; got = 1;
        }
        if (!got) return -1;
        vals[field++] = v;
    }
    if (field < 3) return -1;
    /* P6 的 maxval 必须 ≤255 —— 否则像素是 2 字节/通道，按 3 字节解析必错 */
    if (vals[2] == 0 || vals[2] > 255) return -1;
    /* maxval 与 raster 之间**恰好一个**空白分隔符；\r\n 视为一个 */
    if (i >= len) return -1;
    if (b[i] == '\r' && i + 1 < len && b[i + 1] == '\n') i += 2;
    else i += 1;
    *w = vals[0]; *h = vals[1];
    *off = i;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        say("usage: qscreensrv <sharepath> <host> <port> [fps] [--input-stdin]\n");
        return 1;
    }
    const char *share = argv[1];
    const char *host  = argv[2];
    int port = atoi(argv[3]);
    /* ★踩过：原实现把 argv[4] 无条件 atoi 当 fps，
     *   `qscreensrv <share> <host> <port> --input-stdin` 会得到 fps=1，
     *   静默变成 1fps（用户以为只是开了输入）。必须按首字符区分数字与标志。 */
    int fps = 10;
    int do_input = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--input-stdin") == 0) { do_input = 1; continue; }
        if (argv[i][0] >= '0' && argv[i][0] <= '9') {
            int v = atoi(argv[i]);
            if (v > 0) fps = v;
            continue;
        }
        sayf("WARN unknown arg: %s\n", argv[i]);
    }
    if (fps < 1) fps = 1;
    if (fps > 30) fps = 30;

    int mon = tcp_connect(host, port);
    if (mon < 0) { sayf("FAIL connect monitor %s:%d errno=%d\n", host, port, errno); return 1; }
    sayf("MONITOR_OK %s:%d\n", host, port);
    drain_until_prompt(mon);   /* 吃掉 banner */

    /* 预分配共享文件：用 pmemsave 之外的方式先探一次尺寸 —— 抓一帧就知道 */
    write(mon, "screendump " PPM_TMP "\n", strlen("screendump " PPM_TMP "\n"));
    if (drain_until_prompt(mon) != 0) { say("FAIL first screendump\n"); close(mon); return 1; }

    int pf = open(PPM_TMP, O_RDONLY);
    if (pf < 0) { sayf("FAIL open ppm errno=%d\n", errno); close(mon); return 1; }
    struct stat st;
    fstat(pf, &st);
    size_t plen = (size_t)st.st_size;
    unsigned char *pbuf = (unsigned char *)malloc(plen);
    if (!pbuf || read(pf, pbuf, plen) != (ssize_t)plen) { say("FAIL read ppm\n"); close(pf); close(mon); return 1; }
    close(pf);

    unsigned W = 0, H = 0;
    size_t poff = 0;
    if (ppm_header(pbuf, plen, &W, &H, &poff) != 0) { say("FAIL ppm header\n"); return 1; }
    free(pbuf);
    sayf("CAPTURE %ux%u ppm=%zuB\n", W, H, plen);

    size_t pixbytes = (size_t)W * H * 4;
    size_t total = HDR + pixbytes;

    int fd = open(share, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { sayf("FAIL open share errno=%d\n", errno); close(mon); return 1; }
    if (ftruncate(fd, (off_t)total) != 0) { sayf("FAIL ftruncate errno=%d\n", errno); return 1; }
    unsigned char *map = (unsigned char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { sayf("FAIL mmap errno=%d\n", errno); return 1; }
    unsigned char *pix = map + HDR;

    /* 文件头先写好，seq 从 0 起 */
    put32(map + 0, MAGIC);
    put32(map + 4, 0);
    put32(map + 8, W);
    put32(map + 12, H);

    say("RESULT: QSCREENSRV_STARTED\n");

    unsigned seq = 0;
    long frame_ms = 1000L / fps;
    time_t last_report = time(NULL);
    unsigned frames_since = 0;

    for (;;) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        write(mon, "screendump " PPM_TMP "\n", strlen("screendump " PPM_TMP "\n"));
        if (drain_until_prompt(mon) != 0) {
            /* monitor 断了 —— 重连，不退出（持续服务不能用单次失败判死）
             * ★踩过：connect 成功但 drain banner 失败时必须 close 再重试，
             *   否则旧 fd 泄漏，且后续会把 banner 当命令回执 → 协议失步，
             *   表现为随机超时 / 误判 monitor lost（静默降质）。 */
            say("WARN monitor lost, reconnecting\n");
            close(mon);
            mon = -1;
            for (int i = 0; i < 30; i++) {
                sleep(1);
                int fd2 = tcp_connect(host, port);
                if (fd2 < 0) continue;
                if (drain_until_prompt(fd2) == 0) {
                    mon = fd2;
                    say("MONITOR_RECONNECTED\n");
                    break;
                }
                close(fd2);   /* 连上了但没拿到提示符 —— 丢弃重试 */
            }
            if (mon < 0) { say("FAIL reconnect give up\n"); break; }
            continue;
        }

        int f2 = open(PPM_TMP, O_RDONLY);
        if (f2 < 0) { sayf("WARN open ppm errno=%d\n", errno); usleep(50000); continue; }
        struct stat st2;
        fstat(f2, &st2);
        size_t n2 = (size_t)st2.st_size;
        if (n2 == 0) { close(f2); usleep(50000); continue; }
        unsigned char *b2 = (unsigned char *)malloc(n2);
        if (!b2) { close(f2); break; }
        if (read(f2, b2, n2) != (ssize_t)n2) { free(b2); close(f2); continue; }
        close(f2);

        /* ★每帧都要重解析头 —— guest 可能换分辨率（mode set / 启动图形栈）。
         *   只用首帧的 W/H/poff 会在换模式后静默产出错位或截断的帧，
         *   共享文件头还继续宣称旧尺寸，App 侧完全看不出来。 */
        unsigned w2 = 0, h2 = 0;
        size_t poff2 = 0;
        if (ppm_header(b2, n2, &w2, &h2, &poff2) != 0) {
            free(b2);
            say("WARN ppm header bad, skip frame\n");
            usleep(50000);
            continue;
        }
        if (w2 != W || h2 != H || poff2 != poff) {
            /* 尺寸或头长度变了 → 重建共享文件，否则像素区按旧步长解释必错 */
            sayf("REBUILD resolution %ux%u -> %ux%u\n", W, H, w2, h2);
            munmap(map, total);
            close(fd);
            W = w2; H = h2; poff = poff2;
            pixbytes = (size_t)W * H * 4;
            total = HDR + pixbytes;
            fd = open(share, O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { sayf("FAIL reopen share errno=%d\n", errno); free(b2); return 1; }
            if (ftruncate(fd, (off_t)total) != 0) { sayf("FAIL retrunc errno=%d\n", errno); free(b2); return 1; }
            map = (unsigned char *)mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) { sayf("FAIL remmap errno=%d\n", errno); free(b2); return 1; }
            pix = map + HDR;
            put32(map + 0, MAGIC);
            put32(map + 8, W);
            put32(map + 12, H);
            /* seq 保持单调递增，不能归零（宿主靠它判新帧，归零会当成旧帧丢弃） */
            put32(map + 4, seq);
        }

        /* PPM 是 RGB 三元组；共享文件要 RGBA，alpha 必须显式置 255。
           ★这条踩过：XRGB8888 的第 4 字节不是 alpha，抄过去会让 App 全透明。 */
        size_t need = (size_t)W * H * 3;
        if (n2 - poff >= need) {
            const unsigned char *s = b2 + poff;
            for (size_t i = 0, o = 0; i < need; i += 3, o += 4) {
                pix[o + 0] = s[i + 0];
                pix[o + 1] = s[i + 1];
                pix[o + 2] = s[i + 2];
                pix[o + 3] = 255;
            }
            seq++;
            /* seq 最后写：它是宿主判「有新帧」的唯一依据 */
            put32(map + 4, seq);
        }
        free(b2);

        frames_since++;
        time_t now = time(NULL);
        if (now - last_report >= 2) {
            struct timespec tr;
            clock_gettime(CLOCK_MONOTONIC, &tr);
            double ms = (tr.tv_sec - t0.tv_sec) * 1000.0 + (tr.tv_nsec - t0.tv_nsec) / 1e6;
            sayf("STAT seq=%u fps=%u last=%.1fms\n", seq, frames_since / 2, ms);
            frames_since = 0;
            last_report = now;
        }

        struct timespec t2;
        clock_gettime(CLOCK_MONOTONIC, &t2);
        long used = (t2.tv_sec - t0.tv_sec) * 1000L +
                    (t2.tv_nsec - t0.tv_nsec) / 1000000L;
        long wait = frame_ms - used;
        if (wait > 0) usleep((useconds_t)(wait * 1000));

        /* 输入注入：非阻塞检查 stdin，读到一行就原样转给 monitor */
        if (do_input) {
            char ib[256];
            struct timeval tv = {0, 0};
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(0, &rs);
            if (select(1, &rs, NULL, NULL, &tv) > 0) {
                ssize_t n = read(0, ib, sizeof ib - 1);
                if (n > 0) {
                    ib[n] = 0;
                    write(mon, ib, (size_t)n);
                    if (ib[n - 1] != '\n') write(mon, "\n", 1);
                    drain_until_prompt(mon);
                }
            }
        }
    }

    munmap(map, total);
    close(fd);
    close(mon);
    say("RESULT: QSCREENSRV_EXIT\n");
    return 0;
}
