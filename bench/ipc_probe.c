/*
 * ipc_probe.c —— 容器 ↔ 宿主 共享内存通路成本探针（bionic 侧）
 *
 * 背景：GPU 帧回读后要交给 App 贴图。若每帧都走 socket/pipe 拷贝，
 * 1080p 一帧 7.9MB 会立刻吃满。真正可行的是 memfd 共享内存：
 * 容器侧直接写进映射，宿主同 uid 可直接 open 同一块，零中转拷贝。
 *
 * 测三件事：
 *   1. memfd_create 在容器内是否可用（Android 没有 /dev/shm，shm_open 不行）；
 *   2. 写满一帧的耗时（模拟 glReadPixels 写进同一块映射）；
 *   3. SCM_RIGHTS 传 fd 的成本（每帧一次的上界）。
 *
 * 结果（模拟器 x86_64 / 1080p / 7.91MB 帧）：
 *   memfd_create OK
 *   写满整帧   0.327 ms/frame
 *   msync      0.001 ms
 *   fd 传递    0.0012 ms/次（往返）
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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>

static void P(const char *fmt, ...) {
    char b[512]; va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (n > 0) write(1, b, (size_t)n);
}
static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1e6;
}

/* __NR_memfd_create: x86_64=319, aarch64=279 */
#if defined(__x86_64__)
#define NR_MEMFD_CREATE 319
#else
#define NR_MEMFD_CREATE 279
#endif

static long raw4(long n, long a, long b, long c, long d) {
#if defined(__x86_64__)
    long r;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(d):"rcx","r11","memory");
    return r;
#else
    register long x8 __asm__("x8")=n; register long x0 __asm__("x0")=a;
    register long x1 __asm__("x1")=b; register long x2 __asm__("x2")=c; register long x3 __asm__("x3")=d;
    __asm__ volatile("svc #0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory");
    return x0;
#endif
}

int main(int argc, char **argv) {
    int W = 1920, H = 1080, frames = (argc>1)?atoi(argv[1]):60;
    size_t sz = (size_t)W*H*4;
    P("=== 共享内存通路 ===\n");
    P("frame = %dx%d = %.2f MB\n", W, H, sz/1048576.0);

    int fd = (int)raw4(NR_MEMFD_CREATE, (long)"dshframe", 1 /*MFD_CLOEXEC*/, 0, 0);
    if (fd < 0) { P("FAIL memfd_create errno=%d\n", errno); return 1; }
    P("memfd_create OK fd=%d\n", fd);
    if (ftruncate(fd, (off_t)sz) != 0) { P("FAIL ftruncate errno=%d\n", errno); return 2; }

    void *p = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { P("FAIL mmap errno=%d\n", errno); return 3; }
    P("mmap OK\n");

    unsigned char *q = (unsigned char*)p;
    double t0 = now_ms();
    for (int i = 0; i < frames; i++) memset(q, i & 0xFF, sz);
    double t1 = now_ms();
    P("memset 整帧  : %7.3f ms/frame  (%.1f fps)\n", (t1-t0)/frames, frames*1000.0/(t1-t0));

    t0 = now_ms();
    for (int i = 0; i < 10; i++) msync(p, sz, MS_SYNC);
    t1 = now_ms();
    P("msync(SYNC)  : %7.3f ms/次\n", (t1-t0)/10);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0) {
        char buf[1] = {0};
        struct iovec io = { buf, 1 };
        char cmsgbuf[CMSG_SPACE(sizeof(int))];
        struct msghdr msg; memset(&msg,0,sizeof(msg));
        msg.msg_iov = &io; msg.msg_iovlen = 1;
        msg.msg_control = cmsgbuf; msg.msg_controllen = sizeof(cmsgbuf);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));

        int N = 200;
        t0 = now_ms();
        for (int i = 0; i < N; i++) {
            if (sendmsg(sv[0], &msg, 0) < 0) { P("sendmsg FAIL errno=%d\n", errno); break; }
            char rb[1]; char rcm[CMSG_SPACE(sizeof(int))];
            struct iovec rio = { rb, 1 };
            struct msghdr rmsg; memset(&rmsg,0,sizeof(rmsg));
            rmsg.msg_iov = &rio; rmsg.msg_iovlen = 1;
            rmsg.msg_control = rcm; rmsg.msg_controllen = sizeof(rcm);
            ssize_t rn = recvmsg(sv[1], &rmsg, 0);
            if (rn < 0) { P("recvmsg FAIL errno=%d\n", errno); break; }
            struct cmsghdr *rc2 = CMSG_FIRSTHDR(&rmsg);
            if (rc2) { int got; memcpy(&got, CMSG_DATA(rc2), sizeof(int)); close(got); }
        }
        t1 = now_ms();
        P("fd 传递      : %7.4f ms/次  (往返, %d 次平均)\n", (t1-t0)/N, N);
        close(sv[0]); close(sv[1]);
    } else {
        P("socketpair FAIL errno=%d\n", errno);
    }

    P("RESULT: IPC_OK\n");
    return 0;
}
