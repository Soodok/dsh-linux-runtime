#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把 cleaner 探活修复移植到主仓版本的 dsh_alpine.c。

## 为什么不用「直接复制镜像仓文件」

两个仓的 dsh_alpine.c 已经**双向分叉**：

- 主仓（dsh-android/app/src/main/cpp/）有较新的 aarch64 老式 syscall
  修复：`SYS_lchown` / `SYS_utimes` / `SYS_mknod` 的 dispatch 与对应
  BPF 树项。镜像仓没有这三项 —— 直接覆盖会把主仓已修的 aarch64 功能
  回退掉（实测探针 _test_dsh_alpine_symlink_rel.py 立刻 7 条红）。
- 镜像仓有 cleaner 探活修复（本轮），主仓还是旧的 env 标记方式。

所以正确做法是**只移植 cleaner 这一段**，保留主仓其余内容。

## 本脚本做什么

1. 把 `g_cleaner_on = 0; if (envp) {...}` 整段替换成探活版本
2. 在 `g_cleaner_on = 0;` 之后插入 socket 探活 + 三件状态恢复

用法：python scripts/port_cleaner_probe_to_main.py <main_repo_cpp_path>
"""
import io
import re
import sys

OLD = """        g_cleaner_on = 0;
        if (envp) {
            for (int i = 0; envp[i]; i++) {
                const char *e = envp[i];
                if (e[0]=='D'&&e[1]=='S'&&e[2]=='H'&&e[3]=='_'&&e[4]=='C'&&e[5]=='L'&&e[6]=='E'&&e[7]=='A'&&e[8]=='N') { g_cleaner_on = 1; break; }
            }
        }"""

NEW = """        // ★不再依赖 DSH_CLEANER 环境变量恢复状态 —— 改为**探测 socket**。
        //
        // 原设计用 envp 里的 DSH_CLEANER=1 传递，但有三个理由让它不可靠：
        //   ① 标记要写进 g_env_store，而重执行那行 execveat 传的是 genvp，
        //      两块内存不同 —— 我改了两版都仍传不下去。
        //   ② guest 程序可能自己清空环境（env -i）。
        //   ③ 多一层依赖就多一个静默失败点。
        //
        // socket 名 = <prefix><uid>_<pid>，这两个值**跨 exec 都不变**（uid 不变，
        // PID 原地 exec 也不变），所以重执行后重新算一遍名字、connect 一次探活
        // 就能确定 cleaner 在不在。探活成功就把三件状态一起恢复。
        //
        // 实测症状（修好前，2026-11-07）：
        //   BIONIC: cleaner_exec FAIL
        //   sh: /framesrv: not found          (rc=127)
        // 而直接 dsh_alpine <rootfs> <interp> /prog（首次装载，不跨重执行）
        // 却能跑通 —— 那条 bug 掩盖了自己，所以 30 多轮没被发现。
        g_cleaner_on = 0;
        g_cleaner_namelen = 0;
        {
            char pname[112];
            int plen = cleaner_sockname(pname);
            struct ldr_sockaddr_un psa;
            ((unsigned short *)&psa)[0] = 1;
            ((char *)&psa)[2] = 0;
            for (int i = 0; i < plen; i++) ((char *)&psa)[3 + i] = pname[i];
            int palen = 3 + plen;
            long pfd = raw6(SYS_socket, 1, 1, 0, 0, 0, 0);
            if (pfd >= 0) {
                long pr = raw6(SYS_connect, pfd, (long)&psa, palen, 0, 0, 0);
                raw6(SYS_close, pfd, 0, 0, 0, 0, 0);
                if (pr == 0) {
                    g_cleaner_on = 1;
                    // ★g_cleaner_pid 必须一起恢复成哨兵值。cleaner_exec() 开头
                    //   是 `if (!g_cleaner_on || g_cleaner_pid <= 0) return -1;`
                    //   —— 重执行后它是 0，光把 g_cleaner_on 置 1 仍然返回 -1。
                    //   我第一版漏了这句，症状和没修一模一样（改了四轮才看出来）。
                    //   重执行后父进程不再是 cleaner 的父级，wait4 已经不适用，
                    //   用 1 作哨兵表示「靠 socket 通信，不靠 wait4」。
                    g_cleaner_pid = 1;
                    g_cleaner_namelen = plen;
                    g_cleaner_addrlen = palen;
                    for (int i = 0; i < plen; i++) g_cleaner_name[i] = pname[i];
                }
            }
            // envp 里的旧标记仅作兜底（探活失败时才看），不单独作为判据
            if (!g_cleaner_on && envp) {
                for (int i = 0; envp[i]; i++) {
                    const char *e = envp[i];
                    if (e[0]=='D'&&e[1]=='S'&&e[2]=='H'&&e[3]=='_'&&e[4]=='C'&&e[5]=='L'&&e[6]=='E'&&e[7]=='A'&&e[8]=='N') {
                        g_cleaner_namelen = cleaner_sockname(g_cleaner_name);
                        g_cleaner_addrlen = 3 + g_cleaner_namelen;
                        g_cleaner_on = 1;
                        break;
                    }
                }
            }
        }"""


def main(argv):
    if len(argv) < 2:
        print("usage: port_cleaner_probe_to_main.py <path/to/dsh_alpine.c>")
        return 2
    p = argv[1]
    with io.open(p, encoding="utf-8") as f:
        s = f.read()

    if "cleaner_sockname(pname)" in s:
        print("already ported")
        return 0

    if OLD not in s:
        print("MISS: 主仓版本的旧 cleaner 恢复块没找到")
        return 2

    s = s.replace(OLD, NEW, 1)
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)
    print("ported cleaner probe fix into %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
