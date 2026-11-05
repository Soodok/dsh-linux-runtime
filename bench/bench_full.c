// bench_full.c —— 统一基准程序，程序内 clock_gettime 自计时
// 输出 key=value 固定格式，便于脚本解析对比
//   编译： gcc -O2 -o bench_full bench_full.c
//   用法： ./bench_full [rounds]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 300;

    // ---------- 1. 单次 exec（fork + execve 绝对路径 + waitpid） ----------
    {
        double t0 = now_ms();
        int fail = 0;
        for (int i = 0; i < rounds; i++) {
            pid_t p = fork();
            if (p == 0) { execl("/bin/true", "true", (char *)0); _exit(127); }
            int st = 0; waitpid(p, &st, 0);
            if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) fail++;
        }
        double t1 = now_ms();
        printf("exec_ms=%.4f\n", (t1 - t0) / rounds);
        printf("exec_fail=%d\n", fail);
    }

    // ---------- 2. 文件 IO（open/read/close 同一文件） ----------
    {
        int n = rounds * 20;
        char buf[512];
        double t0 = now_ms();
        int fail = 0;
        for (int i = 0; i < n; i++) {
            int fd = open("/etc/alpine-release", O_RDONLY);
            if (fd < 0) { fail++; continue; }
            if (read(fd, buf, sizeof(buf)) < 0) fail++;
            close(fd);
        }
        double t1 = now_ms();
        printf("io_ms=%.4f\n", (t1 - t0) / n);
        printf("io_fail=%d\n", fail);
    }

    // ---------- 3. stat 系列 ----------
    {
        int n = rounds * 20;
        double t0 = now_ms();
        int fail = 0;
        for (int i = 0; i < n; i++) {
            struct stat st;
            if (stat("/etc/passwd", &st) != 0) fail++;
        }
        double t1 = now_ms();
        printf("stat_ms=%.4f\n", (t1 - t0) / n);
        printf("stat_fail=%d\n", fail);
    }

    // ---------- 4. 纯 syscall（getpid，不经路径翻译） ----------
    {
        int n = rounds * 200;
        volatile pid_t sink = 0;
        double t0 = now_ms();
        for (int i = 0; i < n; i++) sink = getpid();
        double t1 = now_ms();
        printf("getpid_ms=%.5f\n", (t1 - t0) / n);
        (void)sink;
    }

    // ---------- 5. 纯 CPU（零 syscall） ----------
    {
        int n = rounds * 2000;
        volatile double acc = 0;
        double t0 = now_ms();
        for (int i = 0; i < n; i++) acc += 1.0 / (double)(i % 97 + 1);
        double t1 = now_ms();
        printf("cpu_ms=%.4f\n", (t1 - t0) / n * 1000.0);
        (void)acc;
    }

    // ---------- 6. 目录遍历 ----------
    {
        int n = rounds * 5;
        double t0 = now_ms();
        int entries = 0;
        for (int i = 0; i < n; i++) {
            DIR *d = opendir("/usr/bin");
            if (!d) continue;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) entries++;
            closedir(d);
        }
        double t1 = now_ms();
        printf("dir_ms=%.4f\n", (t1 - t0) / n);
        printf("dir_entries=%d\n", entries);
    }

    // ---------- 7. 写文件 ----------
    {
        int n = rounds * 5;
        char buf[256];
        memset(buf, 'A', sizeof(buf));
        double t0 = now_ms();
        int fail = 0;
        for (int i = 0; i < n; i++) {
            int fd = open("/tmp/_bench_w.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { fail++; continue; }
            if (write(fd, buf, sizeof(buf)) < 0) fail++;
            close(fd);
        }
        double t1 = now_ms();
        printf("write_ms=%.4f\n", (t1 - t0) / n);
        printf("write_fail=%d\n", fail);
    }

    return 0;
}
