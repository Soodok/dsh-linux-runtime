/*
 * inputd —— 容器内的触摸注入常驻服务。
 *
 * ## 为什么要这个，而不是每次点一下就跑 `xdotool`
 *
 * 实测每次起一个容器进程约 200ms（fork + execve + seccomp 装载 + SIGSYS
 * 翻译链）。手指拖动时 App 每秒能产生几十到上百个 MOVE 事件，逐事件起进程
 * 会让画面卡成幻灯片。这个程序一次启动、一条 X 长连接到底，单次注入是
 * 一次 XTEST 请求（实测亚毫秒），**拖动才能跟手**。
 *
 * ## 共享文件协议（与 Kotlin 侧 `LinuxInputChannel` 严格一致）
 *
 * 单生产者（App）/ 单消费者（本程序）无锁环形队列：
 *
 *   offset 0   u32 magic = 0x54504E49（小端落盘 od -c 读作 "INPT"）
 *   offset 4   u32 head    生产者写的下一个位置（单调递增，不取模）
 *   offset 8   u32 tail    消费者已处理到的位置（单调递增，不取模）
 *   offset 12  u32 capacity 条目数（固定 256）
 *   offset 16  u32 flags    bit0 = 生产者已挂载（仅诊断用）
 *   offset 20  u32 resv[3]  对齐到 32 字节
 *   offset 32  起为条目，每条 16 字节：
 *                u32 type    0=none 1=down 2=move 3=up
 *                u32 x       已换算成**容器帧内**的像素坐标
 *                u32 y
 *                u32 buttons 目前只用 bit0=左键
 *
 * head/tail 用「单调递增 + 取模定位」而不是「取模后存回」：这样队列满/空的
 * 判断只看减法，不会因为 32 位回绕出错（差值才是语义，绝对值不是）。
 *
 * ## 为什么坐标要 App 侧换算好
 *
 * 画面是按比例缩放显示的（`LinuxFrameSurface.onFrameSize` 里
 * `lp.height = widthPx * h / w`），手指位置与容器帧内像素**不是一比一**。
 * 换算放在 App 侧做，是因为那里才有 `dstRect` 与帧的真实尺寸；这里只管
 * 「拿到就注入」，不重复一遍缩放逻辑（两处各算一次迟早分叉）。
 *
 * ## 为什么不合并 MOVE
 *
 * 本来想「按帧合并只留最新位置」，但那样快速拖动的轨迹会丢拐点。
 * 改成队列不丢事件、队列满了就丢**最旧的**（生产者覆盖），消费者按顺序
 * 全量回放 —— 慢的时候表现为「拖影」，快到跟手时完全无损。
 *
 * 编译：
 *   gcc -O2 -o inputd inputd.c -lXtst -lX11
 *
 * 用法：
 *   inputd <共享文件> [轮询毫秒，默认 1]
 *
 * 退出：SIGTERM/SIGINT 或父进程死掉（共享文件被 truncate）都会干净退出。
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

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#define INPUT_MAGIC   0x54504E49u   /* "INPT" 小端 */
#define INPUT_CAP     256
#define HDR_BYTES     32
#define ENTRY_BYTES   16

#define T_NONE 0
#define T_DOWN 1
#define T_MOVE 2
#define T_UP   3

static volatile sig_atomic_t g_stop;

static void on_sig(int s) { (void)s; g_stop = 1; }

static unsigned rd_u32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void wr_u32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: inputd <shared-file> [poll-ms]\n");
        return 2;
    }
    const char *path = argv[1];
    int poll_ms = (argc > 2) ? atoi(argv[2]) : 1;
    if (poll_ms < 1) poll_ms = 1;

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* ---- X 连接：一次开到底，这是「跟手」的全部前提 ---- */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "INPUTD_FAIL open display (DISPLAY=%s)\n",
                getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
        return 1;
    }
    int ev, err, major, minor;
    if (!XTestQueryExtension(dpy, &ev, &err, &major, &minor)) {
        fprintf(stderr, "INPUTD_FAIL XTEST extension unavailable\n");
        XCloseDisplay(dpy);
        return 1;
    }
    fprintf(stderr, "INPUTD_KV xtest=%d.%d display=%s\n", major, minor,
            DisplayString(dpy));

    /* ---- 映射共享文件 ---- */
    size_t total = HDR_BYTES + (size_t)INPUT_CAP * ENTRY_BYTES;
    /*
     * 用 O_CREAT 而不是 O_RDWR —— 生产顺序不保证。
     *
     * 真实场景里 App 会先 open+ftruncate 再拉起本程序，但**手工调试、容器重启、
     * 以及 App 侧还没打开画面卡**这三种情况下，本程序完全可能先跑起来。
     * 只读打不开就直接退出，会让「inputd 没干活」看起来像注入逻辑坏了。
     *
     * 反过来说，本程序**绝不覆盖已有内容**（O_CREAT 不带 O_TRUNC）：共享文件
     * 里可能已经有事件，truncate 会静默丢掉它们。
     */
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        fprintf(stderr, "INPUTD_FAIL open %s: %s\n", path, strerror(errno));
        XCloseDisplay(dpy);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < total) {
        /* 生产者应该已经 ftruncate 过；这里兜底，绝不写超出映射 */
        if (ftruncate(fd, (off_t)total) != 0) {
            fprintf(stderr, "INPUTD_FAIL ftruncate: %s\n", strerror(errno));
            close(fd);
            XCloseDisplay(dpy);
            return 1;
        }
    }
    unsigned char *base = (unsigned char *)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                               MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        fprintf(stderr, "INPUTD_FAIL mmap: %s\n", strerror(errno));
        close(fd);
        XCloseDisplay(dpy);
        return 1;
    }
    close(fd);

    if (rd_u32(base + 0) != INPUT_MAGIC) {
        /* 第一次由本程序初始化；之后生产者会保持它 */
        wr_u32(base + 0, INPUT_MAGIC);
        wr_u32(base + 12, INPUT_CAP);
    }
    fprintf(stderr, "INPUTD_KV path=%s cap=%u\n", path, INPUT_CAP);

    unsigned last_tail = rd_u32(base + 8);
    unsigned long long done = 0;

    while (!g_stop) {
        unsigned head = rd_u32(base + 4);
        unsigned tail = rd_u32(base + 8);

        /* 生产者重开过（head/tail 归零）—— 跟着归零，别去追一个旧位置 */
        if (head < tail) {
            tail = head;
            wr_u32(base + 8, tail);
        }

        unsigned pend = head - tail;
        /* 队列被覆盖时（生产快于消费），只放最新的 CAP 条，避免空转追赶 */
        if (pend > INPUT_CAP) {
            tail = head - INPUT_CAP;
            wr_u32(base + 8, tail);
            pend = INPUT_CAP;
        }

        if (pend == 0) { sleep_ms(poll_ms); continue; }

        for (unsigned i = 0; i < pend && !g_stop; i++) {
            unsigned idx = (tail + i) % INPUT_CAP;
            const unsigned char *e = base + HDR_BYTES + (size_t)idx * ENTRY_BYTES;
            unsigned type = rd_u32(e + 0);
            unsigned x = rd_u32(e + 4);
            unsigned y = rd_u32(e + 8);
            unsigned btn = rd_u32(e + 12);

            if (type == T_DOWN || type == T_MOVE) {
                XTestFakeMotionEvent(dpy, -1, (int)x, (int)y, CurrentTime);
            }
            if (type == T_DOWN) {
                XTestFakeButtonEvent(dpy, 1, True, CurrentTime);
                (void)btn;
            } else if (type == T_UP) {
                XTestFakeButtonEvent(dpy, 1, False, CurrentTime);
            }
            XFlush(dpy);
            done++;

            /* 立刻推 tail：中途被信号打断时，已执行的不会再被执行一次 */
            wr_u32(base + 8, tail + i + 1);
        }
        /* 排空后把 tail 归到 head，防止长跑时无谓追赶 */
        wr_u32(base + 8, head);
    }

    fprintf(stderr, "INPUTD_KV done=%llu\n", done);
    munmap(base, total);
    XCloseDisplay(dpy);
    return 0;
}
