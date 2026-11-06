/* unix_socket_probe —— 验证 AF_UNIX 的 bind/connect 路径翻译。
 *
 * 为什么需要它：bind 与 connect 是两个独立的缺失点，只修一个会出现
 * 「服务端能起、客户端连不上」这种更难查的症状（见 docs/pitfalls.md #35）。
 * 本探针在同一进程内自 bind 自 connect，一次跑完两层。
 *
 * 编译（放进 rootfs 或 bind 目录，不能放 /data/local/tmp）：
 *   x86_64-linux-android28-clang -O2 -static -o unix_socket_probe bench/unix_socket_probe.c
 * 运行（必须经 sh -c）：
 *   dsh_alpine -m /dsh:<home> <rootfs> /lib/ld-musl-*.so.1 /bin/sh -c '/unix_socket_probe /tmp'
 *
 * 期望 RESULT: UNIX_SOCKET_OK。任一 FAIL 即对应 syscall 的 sun_path 没翻译。
 */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>

static int fails;

static void try_dir(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/us-probe.sock", dir);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { printf("%-24s socket FAIL %s\n", dir, strerror(errno)); fails++; return; }

    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    unlink(path);

    if (bind(srv, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("%-24s bind FAIL %s\n", dir, strerror(errno)); fails++; close(srv); return;
    }
    if (listen(srv, 4) != 0) {
        printf("%-24s listen FAIL %s\n", dir, strerror(errno)); fails++; close(srv); return;
    }

    int cli = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(cli, (struct sockaddr *)&a, sizeof a) != 0) {
        printf("%-24s connect FAIL %s\n", dir, strerror(errno)); fails++;
        close(cli); close(srv); unlink(path); return;
    }

    int acc = accept(srv, NULL, NULL);
    if (acc < 0) {
        printf("%-24s accept FAIL %s\n", dir, strerror(errno)); fails++;
    } else {
        /* 真正传一个字节，确认连接可用而不只是握手成功 */
        char c = 'z';
        write(cli, &c, 1);
        char got = 0;
        if (read(acc, &got, 1) == 1 && got == 'z')
            printf("%-24s bind+connect+rw OK\n", dir);
        else
            printf("%-24s rw FAIL\n", dir), fails++;
        close(acc);
    }
    close(cli); close(srv); unlink(path);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: unix_socket_probe <dir>...\n"); return 2; }
    for (int i = 1; i < argc; i++) try_dir(argv[i]);
    printf("\n%s (fails=%d)\n",
           fails ? "RESULT: UNIX_SOCKET_FAIL" : "RESULT: UNIX_SOCKET_OK", fails);
    return fails ? 1 : 0;
}
