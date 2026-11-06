/* path_probe —— 一次性验证「路径语义」三件套是否正常。
 *
 * 为什么需要它：realpath / fs.watch / getcwd 三条路径各自踩过不同的坑，
 * 但症状都表现为「某个插件起不来」，离根因极远（详见 docs/pitfalls.md #34）。
 * 这个探针把三条一次性跑完，出问题时能立刻分辨是哪一条断了。
 *
 * 编译（放进 rootfs 或 bind 目录，不能放 /data/local/tmp）：
 *   aarch64-linux-android28-clang -O1 -static -o path_probe bench/path_probe.c
 * 运行（必须经 sh -c，直调会按 musl 装载）：
 *   dsh_alpine -m /dsh:<home> <rootfs> /lib/ld-musl-*.so.1 /bin/sh -c '/path_probe'
 *
 * 期望输出三行全 OK。任何一行 FAIL 都意味着对应 syscall 的路径翻译缺失：
 *   realpath FAIL  -> readlinkat(/proc/self/fd/N) 没做 host_to_guest 反查
 *   inotify  FAIL  -> inotify_add_watch 没进 BPF 树 / handler 没做 make_host
 *   chdir    FAIL  -> getcwd 的 bind 反查 + rootfs 前缀裁剪没生效
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/inotify.h>

static int fails;

static void check_realpath(const char *p) {
    char buf[PATH_MAX];
    char *r = realpath(p, buf);
    printf("realpath(%s) = %s\n", p, r ? r : "(FAIL)");
    if (!r) fails++;
}

static void check_inotify(const char *p) {
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) { printf("inotify_init1 FAIL %d\n", errno); fails++; return; }
    int wd = inotify_add_watch(fd, p, IN_CREATE | IN_MODIFY);
    printf("inotify_add_watch(%s) = %s (wd=%d)\n", p, wd >= 0 ? "OK" : "FAIL", wd);
    if (wd < 0) { printf("    errno=%d (%s)\n", errno, strerror(errno)); fails++; }
    else inotify_rm_watch(fd, wd);
    close(fd);
}

static void check_getcwd(const char *dir) {
    if (chdir(dir) != 0) {
        printf("chdir(%s) FAIL %d (%s)\n", dir, errno, strerror(errno));
        fails++; return;
    }
    char buf[PATH_MAX];
    char *r = getcwd(buf, sizeof buf);
    printf("chdir(%s) -> getcwd = %s\n", dir, r ? r : "(FAIL)");
    /* 关键断言：cwd 绝不能泄漏宿主路径 */
    if (!r) { fails++; return; }
    if (strncmp(r, "/data/", 6) == 0 || strncmp(r, "/storage/", 9) == 0) {
        printf("    ^ LEAK: getcwd 返回宿主路径，guest 看不到这个目录\n");
        fails++;
    }
}

int main(void) {
    const char *dirs[] = { "/", "/tmp", "/dsh", "/eng", "/ws", NULL };
    printf("--- realpath ---\n");
    for (int i = 0; dirs[i]; i++) check_realpath(dirs[i]);
    printf("--- inotify ---\n");
    for (int i = 0; dirs[i]; i++) check_inotify(dirs[i]);
    printf("--- getcwd ---\n");
    for (int i = 0; dirs[i]; i++) check_getcwd(dirs[i]);
    printf("\n%s (fails=%d)\n", fails ? "RESULT: PATH_PROBE_FAIL" : "RESULT: PATH_PROBE_OK", fails);
    return fails ? 1 : 0;
}
