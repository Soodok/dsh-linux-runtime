/*
 * unixsock_guest.c —— **musl** 版 AF_UNIX 探针，在容器 guest 内真实验证 bind/connect
 *
 * 为什么必须有这个（而 bench/unix_socket_probe.c 不够）：
 *   那个是 **bionic** 程序，经 cleaner 代跑时按宿主根解析路径（pitfalls #41），
 *   它验证的是「loader 的 sockaddr_un 重拼逻辑在宿主路径上没错」，
 *   完全没碰 guest 路径。而容器内真实程序（weston/sway/dbus）用的是 guest 路径。
 *
 * 实测暴露问题：sway headless 启动时
 *   [wlr] [wayland] bind() failed with error: No such file or directory
 * 说明 guest 路径下的 bind 仍有缺陷 —— 这个探针就是把它钉住的。
 *
 * 用法（容器内编译）：gcc -O2 -o unixsock_guest unixsock_guest.c
 *   ./unixsock_guest /tmp /tmp/wl /dsh /eng
 *
 * 输出契约：
 *   UNIXSOCK_KV path=<p> bind=OK|FAIL
 *   RESULT: UNIXSOCK_GUEST_OK | UNIXSOCK_GUEST_FAIL (n)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

static int test_path(const char *dir) {
    char path[256];
    struct sockaddr_un addr;
    int sfd, cfd, rc;
    char buf[8] = "x";

    snprintf(path, sizeof path, "%s/usock-%d.sock", dir, (int)getpid());

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        printf("UNIXSOCK_KV path=%s bind=FAIL socket errno=%d(%s)\n",
               dir, errno, strerror(errno));
        return 1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(sfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        printf("UNIXSOCK_KV path=%s bind=FAIL errno=%d(%s) sock=%s\n",
               dir, errno, strerror(errno), path);
        close(sfd);
        return 1;
    }
    if (listen(sfd, 4) < 0) {
        printf("UNIXSOCK_KV path=%s listen=FAIL errno=%d(%s)\n",
               dir, errno, strerror(errno));
        close(sfd); unlink(path);
        return 1;
    }

    /* 自连一次，确认 connect 侧也通 */
    cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd < 0) { close(sfd); unlink(path); return 1; }
    if (connect(cfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        printf("UNIXSOCK_KV path=%s bind=OK connect=FAIL errno=%d(%s)\n",
               dir, errno, strerror(errno));
        close(cfd); close(sfd); unlink(path);
        return 1;
    }

    /* 传一个字节 */
    struct sockaddr_un peer;
    socklen_t plen = sizeof peer;
    int afd = accept(sfd, (struct sockaddr *)&peer, &plen);
    if (afd < 0) {
        printf("UNIXSOCK_KV path=%s accept=FAIL errno=%d(%s)\n",
               dir, errno, strerror(errno));
        close(cfd); close(sfd); unlink(path);
        return 1;
    }
    rc = (int)write(cfd, buf, 1);
    if (rc != 1) {
        printf("UNIXSOCK_KV path=%s write=FAIL\n", dir);
        close(afd); close(cfd); close(sfd); unlink(path);
        return 1;
    }
    rc = (int)read(afd, buf, 1);
    close(afd); close(cfd); close(sfd); unlink(path);
    if (rc != 1) {
        printf("UNIXSOCK_KV path=%s read=FAIL\n", dir);
        return 1;
    }
    printf("UNIXSOCK_KV path=%s bind=OK connect=OK rw=OK\n", dir);
    return 0;
}

int main(int argc, char **argv) {
    int i, fails = 0;
    if (argc < 2) {
        printf("用法: %s <dir> [dir...]\n", argv[0]);
        return 2;
    }
    for (i = 1; i < argc; i++) fails += test_path(argv[i]);
    printf("RESULT: %s (%d)\n",
           fails == 0 ? "UNIXSOCK_GUEST_OK" : "UNIXSOCK_GUEST_FAIL", fails);
    return fails;
}
