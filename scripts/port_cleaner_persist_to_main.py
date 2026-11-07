#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把「cleaner 常驻 + 传递请求方环境」补丁移植到主仓版本的 dsh_alpine.c。

## 为什么需要单独的移植脚本

两个仓的 dsh_alpine.c 已双向分叉：
- 主仓有 SYS_lchown / SYS_utimes / SYS_mknod（aarch64 老式 syscall）
- 镜像仓有 cleaner 的一系列修复

直接 cp 会回退主仓的 aarch64 成果（实测探针立刻 7 条红）。所以只搬
cleaner 这一段。

## 本脚本打的两个补丁（对应镜像仓 13a6b93）

1. cleaner_loop 从「accept 一次就退出」改成 `for(;;)` 常驻
2. cleaner 请求协议追加「请求方当前环境」段，接收侧优先用它

用法：python scripts/port_cleaner_persist_to_main.py <path/to/dsh_alpine.c>
"""
import io
import re
import sys


def patch_loop(src):
    """cleaner_loop: accept 一次 → for(;;) 常驻。"""
    old_head = """static void cleaner_loop(long lfd, int socklen) {
    static char buf[8192];
    struct ldr_sockaddr_un sa;
    {
        unsigned int sl = (unsigned int)socklen;"""
    new_head = """static void cleaner_loop(long lfd, int socklen) {
    static char buf[8192];
    struct ldr_sockaddr_un sa;
    // ★常驻循环，而不是「一次一进程」。
    //
    // 旧实现只 accept 一次就 exit_group，理由是「常驻会持有借来的 stdio，
    // 让 adb shell 永不返回」。那个观察没错，但解法错了：
    //   - adb shell 等的是它 fork 的 sh（我们的 guest），cleaner 是 loader
    //     fork 的兄弟进程，不阻塞任何东西。
    //   - 泄漏由 PR_SET_PDEATHSIG 兜住（父进程死，内核立刻 SIGKILL）。
    //   - stdio 必须保留 —— 孙进程继承 cleaner 的 fd 1/2，那是 bionic 程序
    //     输出回终端的唯一路径，关掉就只剩 RC=0、输出全丢。
    //
    // 一次性模型的致命后果：一条命令里出现**第二个** bionic 程序时，第一个
    // 请求已把 cleaner 消费掉，第二个必然 cleaner_exec FAIL / not found。
    // 实测 sh -c 'export X=1; /eprobe; /framesrv ...' —— eprobe 成功、
    // framesrv 立刻 not found。
    for (;;) {
        unsigned int sl = (unsigned int)socklen;"""
    if old_head not in src:
        return src, False
    src = src.replace(old_head, new_head, 1)

    old_tail = """        raw6(SYS_close /*close*/, cfd, 0, 0, 0, 0, 0);
        // ★必须关掉本次借来的 stdio 副本：cleaner 是常驻进程，若一直持有
        //   这两个 fd，adb shell 的管道永远不关，命令永不返回（实测超时丢输出）。
    }
    raw6(SYS_exit_group /*exit_group*/, 0, 0, 0, 0, 0, 0);   // 服务完一次即退出
}"""
    new_tail = """        raw6(SYS_close /*close*/, cfd, 0, 0, 0, 0, 0);
    }
    // 循环永不返回；lfd 在 cleaner_start 里 fork 前创建，
    // PDEATHSIG 保证父进程死亡时内核直接回收本进程。
}"""
    if old_tail not in src:
        return src, False
    src = src.replace(old_tail, new_tail, 1)
    return src, True


def patch_env(src):
    """cleaner 协议：argv 之后追加请求方当前环境。"""
    # 1) 签名
    old = "static int cleaner_exec(const char *hostpath, char **gargv) {"
    new = "static int cleaner_exec(const char *hostpath, char **gargv, char **genv_cur) {"
    if old not in src:
        return src, False
    src = src.replace(old, new, 1)

    # 2) 发送侧：argv 终止符之后追加环境
    old_send = """    msg[n++] = 0;
    // cleaner 是**一次性进程**（服务一次就 exit），因此它继承的 stdout/stderr"""
    new_send = """    msg[n++] = 0;
    // ★argv 终止后再追加**当前**环境变量，再以空串收尾。
    //
    // 原实现只用 g_envp_for_cleaner —— 那是 cleaner 在**首次启动时** fork
    // 那一刻固化的 envp（ldr_entry 里 `g_envp_for_cleaner = envp`）。之后用户
    // export 的任何变量都进不去，实测：
    //   musl_child=[hello_from_export]      <= musl 程序看得到
    //   bionic_child=[]                     <= bionic（走 cleaner）看不到
    // 后果：LD_LIBRARY_PATH / HOME / PATH 对 bionic 程序全部失效，表现为
    //   CANNOT LINK EXECUTABLE ...: library "libz.so.1" not found
    // —— 明明 LD_LIBRARY_PATH 里就有那个库。
    //
    // 协议：path\\0argv[1..]\\0\\0 ENV[1..]\\0\\0（环境段在 argv 终止符之后）
    if (genv_cur) {
        for (int i = 0; genv_cur[i] && n < (long)sizeof(msg) - 300; i++) {
            for (const char *q = genv_cur[i]; *q && n < (long)sizeof(msg) - 2; q++) msg[n++] = *q;
            msg[n++] = 0;
        }
    }
    msg[n++] = 0;
    // cleaner 是**一次性进程**（服务一次就 exit），因此它继承的 stdout/stderr"""
    if old_send not in src:
        return src, False
    src = src.replace(old_send, new_send, 1)

    # 3) 接收侧：解析环境段
    old_recv = """        char *av[64]; int ac = 0;
        av[ac++] = path;
        while (p < buf + n && ac < 63) {
            if (!*p) break;
            av[ac++] = p;
            while (*p) p++;
            p++;
        }
        av[ac] = 0;"""
    new_recv = """        char *av[64]; int ac = 0;
        av[ac++] = path;
        while (p < buf + n && ac < 63) {
            if (!*p) break;
            av[ac++] = p;
            while (*p) p++;
            p++;
        }
        av[ac] = 0;
        // ★argv 之后是**请求方当前环境**（见 cleaner_exec 的协议说明）。
        //   以前这里用 g_envp_for_cleaner —— 首次启动那一刻的快照，之后
        //   export 的变量全丢（实测 bionic_child=[] 而 musl_child=[hello]）。
        char *ev[128]; int ec = 0;
        if (p < buf + n) {
            p++;   // 跳过 argv 终止符
            while (p < buf + n && ec < 127) {
                if (!*p) break;
                ev[ec++] = p;
                while (*p) p++;
                p++;
            }
        }
        ev[ec] = 0;
        // 请求方没带环境（老协议/空环境）就退回启动快照，保持兼容
        char **use_env = (ec > 0) ? ev : g_envp_for_cleaner;"""
    if old_recv not in src:
        return src, False
    src = src.replace(old_recv, new_recv, 1)

    # 4) 两处 execve 改用 use_env
    n = src.count("raw6(SYS_execve /*execve*/, (long)path, (long)av, (long)g_envp_for_cleaner, 0, 0, 0);")
    if n != 2:
        return src, False
    src = src.replace(
        "raw6(SYS_execve /*execve*/, (long)path, (long)av, (long)g_envp_for_cleaner, 0, 0, 0);",
        "raw6(SYS_execve /*execve*/, (long)path, (long)av, (long)use_env, 0, 0, 0);")

    # 5) 调用点
    old_call = "            int rc2 = cleaner_exec(hp, gargv);"
    new_call = "            int rc2 = cleaner_exec(hp, gargv, genvp);"
    if old_call not in src:
        return src, False
    src = src.replace(old_call, new_call, 1)
    return src, True


def main(argv):
    if len(argv) < 2:
        print("usage: port_cleaner_persist_to_main.py <path/to/dsh_alpine.c>")
        return 2
    p = argv[1]
    with io.open(p, encoding="utf-8") as f:
        s = f.read()

    if "char **use_env" in s and "for (;;) {\n        unsigned int sl" in s:
        print("already ported")
        return 0

    s, ok1 = patch_loop(s)
    s, ok2 = patch_env(s)
    if not (ok1 and ok2):
        print("MISS: loop=%s env=%s" % (ok1, ok2))
        return 2

    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)
    print("ported cleaner persist+env into %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
