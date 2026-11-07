#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""wlshot --loop 出错必须能自恢复，不能静默退出。

## 为什么需要这个探针

`--loop` 模式原本每帧出任何错都 `break` 退出：

    if (failed || f_w <= 0) {
        fprintf(stderr, "FAIL loop frame %d: no buffer event\\n", iter);
        break;                       // ← 进程直接结束
    }

后果链条：
  compositor 重启 / 输出尺寸变化 / 某帧超时
    → 帧服务进程永久退出
    → 共享文件 seq 不再递增
    → App 侧 LinuxFrameSurface 的脏判据（seq 变化）永远不成立
    → **画面永久静止，且没有任何报错**

用户看到的是「画面卡住了」，不是「服务挂了」。这与项目里反复踩过的
「静默失败」是同一类问题。

## 本探针守什么

A. 单帧错误必须是 `continue`，不能是 `break`
B. 连接断开（roundtrip < 0）必须触发重连
C. 重连失败时不能直接 continue（scm 会被清成 NULL → 段错误）
D. 重连成功后必须让 wl_buffer 重建（旧 buffer 属于旧 display）
E. 必须有连续失败上限，不能无限重试把日志刷爆
F. 成功一帧后必须清零 consec_fail

## 反向验证

把任一处 `continue` 改回 `break`，A 段立刻变红。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, "bench", "wlshot.c")

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

    print("[A] 单帧错误不退出")
    # 定位 --loop 的错误处理块：从 if (failed || f_w <= 0) 起，截到
    # 下一个 if (f_w != (int)out_w ...)（尺寸变化分支）之前。
    #
    # ★锚点必须用**代码**不能用注释：本函数的 src 已经过 strip_comments()，
    #   拿 `/* 尺寸变化` 当锚点永远匹配不到（我踩过一次）。
    m = re.search(r'if \(failed \|\| f_w <= 0\) \{(.*?)if \(f_w != \(int\)out_w', src, re.S)
    ok(m is not None, "A0 --loop 错误块可定位")
    body = m.group(1) if m else ""

    # ★A1 必须把 while(reconnect) 块剥掉再判 break —— 那个 while 里有一个
    #   合法的 `break`（跳出 while 而非 for）。我第一版写成
    #   `"break;" not in body or "while" in body`，于是把整块改成裸 break 后
    #   A1 仍然绿（因为 body 里确实还有 while 字样）→ 假通过。
    # ★A1 统计 break 数量，上限 2。两个都是合法的：
    #   ① `if (wl_display_roundtrip(disp) < 0) { failed = 2; break; }`
    #      —— 跳出内层 spin 的 for，不是跳出主 for
    #   ② `while (failed == 2) { ... break; }`
    #      —— 跳出 reconnect 的 while
    #   多出第 3 个就说明错误块里有裸 break（会永久停机）。
    #
    #   我第一版写成 `"break;" not in body or "while" in body`，于是把整块
    #   改成裸 break 后 A1 仍然绿 → 假通过。改成计数才真变红。
    n_break = body.count("break;")
    ok(n_break <= 2,
       "A1 错误块 break 数 ≤2（spin 的 for + reconnect 的 while），实际 %d 个" % n_break)
    ok("continue;" in body, "A2 错误块以 continue 收尾")

    print("[B] 断连必须重连")
    ok("reconnect(" in src, "B1 存在 reconnect() 调用")
    ok(re.search(r"failed\s*=\s*2", src) is not None,
       "B2 roundtrip<0 时标记 failed=2（与普通超时区分）")

    print("[C] 重连失败不得直接 continue（防 NULL 解引用）")
    # reconnect 失败分支必须在 while 循环里重试，不能落到外层 continue
    ok(re.search(r"while \(failed == 2\)", src) is not None,
       "C1 重连放在 while 循环里重试，而非 if 一次后就 continue")

    print("[D] 重连后重建 buffer")
    ok(re.search(r"out_w = 0; out_h = 0;", src) is not None,
       "D1 重连后把 out_w 清零，逼尺寸分支重建")
    ok(re.search(r"if \(buf\) \{ wl_buffer_destroy\(buf\); buf = NULL; \}", src) is not None,
       "D2 重建前判空（旧 display 的 buf 已失效）")

    print("[E] 连续失败上限")
    ok(re.search(r"MAX_CONSEC", src) is not None, "E1 定义了连续失败上限")
    ok(re.search(r"consec_fail >= MAX_CONSEC", src) is not None,
       "E2 达到上限才放弃")

    print("[F] 成功清零")
    ok(re.search(r"consec_fail = 0;", src) is not None,
       "F1 成功一帧后清零 consec_fail")

    print("")
    if failed:
        print("FAILED (%d/%d)" % (failed, checks))
        return 1
    print("All passed (%d checks)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
