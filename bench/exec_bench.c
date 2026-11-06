/*
 * exec_bench.c —— 容器内 exec 开销基准（musl 侧 guest 可执行文件）
 *
 * 为什么需要这个数：冷启优化里「插件加载」是大头，但容器本身每启动一个
 * 进程都要付 dsh_alpine 的 seccomp 拦截成本。只有把这个底噪量出来，才能
 * 判断「启动 N 个进程」这条路的可行边界。
 *
 * 本探针在**容器内**跑（musl guest），用 clock_gettime 打点，fork+exec 一个
 * 平凡程序 N 次，报告每次的 wall 时间（含 fork + execve + 等 seccomp 拦截
 * + 装载 + 退出）。
 *
 * 编译（musl 静态，放到 rootfs 里）：
 *   x86_64-linux-musl-gcc -static -O2 -o exec_bench exec_bench.c
 * 或在容器的 Alpine 里：apk add gcc musl-dev && gcc -static -O2 ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 50;
    const char *prog = (argc > 2) ? argv[2] : "/bin/true";
    if (n < 1) n = 50;

    /* 预热 5 次，避开首次装载的页缓存缺失 */
    for (int i = 0; i < 5; i++) {
        pid_t p = fork();
        if (p == 0) { execl(prog, prog, (char *)0); _exit(127); }
        int st; waitpid(p, &st, 0);
    }

    double t0 = now_ms();
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) { execl(prog, prog, (char *)0); _exit(127); }
        int st; waitpid(p, &st, 0);
    }
    double t1 = now_ms();
    printf("prog=%s  n=%d\n", prog, n);
    printf("fork+exec+wait : %.3f ms/次  (合计 %.2f ms)\n", (t1 - t0) / n, t1 - t0);
    return 0;
}
