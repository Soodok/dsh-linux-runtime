#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""bionic 直通（cleaner 桥）在自我重执行后必须仍可用。

## 背景：这条 bug 让产线上所有图形程序都跑不了

loader 的 bionic 直通依赖一个在 `install_filter()` **之前** fork 出来的
filter-free 子进程（cleaner）。父进程随后 `execveat(/proc/self/exe)` 自我
重执行，`.data` 段被重初始化 → `g_cleaner_on` / `g_cleaner_pid` /
`g_cleaner_namelen` 全部归零。

原设计靠环境标记 `DSH_CLEANER=1` 恢复，但：

1. 标记被写进 `g_env_store`（给 guest 用的那份），而重执行那行
   `execveat` 传的是 `genvp`（调用方原始 envp）——**两块内存不同**，
   标记永远传不到新 loader。
2. 即便标记传到了，`cleaner_exec()` 开头是
   `if (!g_cleaner_on || g_cleaner_pid <= 0) return -1;`
   而 `g_cleaner_pid` 也是 0 —— 只恢复 `g_cleaner_on` 仍然返回 -1。

实测症状（2026-11-07 前）：

```
BIONIC: cleaner_exec FAIL
sh: /framesrv: not found          (rc=127)
```

**为什么 30 多轮没被发现**：直接 `dsh_alpine <rootfs> <interp> /prog`
走的是首次装载，`g_cleaner_on` 还在同一进程里，bug 掩盖了自己。只有经
`sh -c '/prog'`（容器内二次 execve，即产线路径）才暴露。

## 修法

不依赖环境变量：socket 名 = `<prefix><uid>_<pid>`，而 uid 与 pid **跨
exec 都不变**。重执行后重新算一遍名字、connect 一次探活即可。探活成功
时三件事一起恢复：`g_cleaner_on=1` / `g_cleaner_pid=1`（哨兵）/ 名字与
addrlen。

## 本探针守什么

A. 重执行分支里有 socket 探活（不是只看环境变量）
B. 探活成功必须同时恢复 g_cleaner_on / g_cleaner_pid / g_cleaner_namelen
C. cleaner_exec 的入口守卫保持原样（两个条件都要）
D. 不许退回「只靠 DSH_CLEANER 环境变量」的旧做法

## 反向验证

把 `g_cleaner_pid = 1;` 删掉，B 段立刻变红。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, "src", "dsh_alpine.c")

checks = 0
failed = 0


def ok(cond, msg):
    global checks, failed
    checks += 1
    if cond:
        print("  ok   %s" % msg)
    else:
        failed += 1
        print("  FAIL %s" % msg)


def strip_comments(src):
    out = []
    i, n = 0, len(src)
    in_str = in_ch = False
    bs = chr(92)
    while i < n:
        c = src[i]
        if in_ch:
            if c == bs:
                out.append(src[i:i + 2])
                i += 2
                continue
            if c == "'":
                in_ch = False
            out.append(c)
            i += 1
            continue
        if in_str:
            if c == bs:
                out.append(src[i:i + 2])
                i += 2
                continue
            if c == '"':
                in_str = False
            out.append(c)
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "'":
            in_ch = True
            out.append(c)
            i += 1
            continue
        if src.startswith("//", i):
            j = src.find(chr(10), i)
            i = n if j < 0 else j
            continue
        if src.startswith("/*", i):
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def main():
    with open(SRC, encoding="utf-8") as f:
        src = strip_comments(f.read())

    print("[A] 重执行后必须探活 cleaner socket")
    ok("cleaner_sockname(pname)" in src,
       "A1 重执行分支重新计算 socket 名")
    ok(re.search(r"SYS_connect,\s*pfd", src) is not None,
       "A2 用 connect 探活（不是只看环境变量）")

    print("[B] 探活成功必须恢复三件状态")
    m = re.search(r"if \(pr == 0\) \{(.{0,600}?)\n                \}", src, re.S)
    ok(m is not None, "B0 探活成功分支可定位")
    body = m.group(1) if m else ""
    ok("g_cleaner_on = 1;" in body, "B1 恢复 g_cleaner_on")
    ok("g_cleaner_pid = 1;" in body,
       "B2 恢复 g_cleaner_pid（否则 cleaner_exec 仍 return -1）")
    ok("g_cleaner_namelen = plen;" in body, "B3 恢复 g_cleaner_namelen")

    print("[C] cleaner_exec 入口守卫不变")
    ok(re.search(r"if \(!g_cleaner_on \|\| g_cleaner_pid <= 0\) return -1;", src) is not None,
       "C1 两个条件都在（这正是 B2 必要的原因）")

    print("[E] cleaner 必须常驻（能连续服务多次请求）")
    ok(re.search(r"static void cleaner_loop[^}]*for \(;;\) \{", src, re.S) is not None,
       "E1 cleaner_loop 是 for(;;) 常驻循环，不是只 accept 一次")

    print("[F] 请求方必须把自己的当前环境发给 cleaner")
    ok("cleaner_exec(const char *hostpath, char **gargv, char **genv_cur)" in src,
       "F1 cleaner_exec 接收 genv_cur（当前环境）")
    ok(re.search(r"cleaner_exec\(hp, gargv, genvp\)", src) is not None,
       "F2 调用点传 genvp")
    ok(re.search(r"if \(genv_cur\) \{\s*for \(int i = 0; genv_cur\[i\]", src) is not None,
       "F3 发送侧把环境段追加进 msg")
    ok(re.search(r"char \*\*use_env = \(ec > 0\) \? ev : g_envp_for_cleaner;", src) is not None,
       "F4 接收侧优先用请求方环境，空则退回启动快照（兼容）")

    print("[D] 不许只靠环境变量")
    # 旧做法：重执行分支里 g_cleaner_on = 0 之后紧跟 envp 扫描，没有探活
    ok(not re.search(r"g_cleaner_on = 0;\s*if \(envp\)", src),
       "D1 不是「清 0 后直接扫 envp」的旧写法")

    print("")
    if failed:
        print("FAILED (%d/%d)" % (failed, checks))
        return 1
    print("All passed (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
