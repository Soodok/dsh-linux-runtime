#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""修掉 wlshot.c 里两处字符串字面量被真实换行截断的编译错误。

起因：我用 heredoc 生成 C 代码时，`\n` 被 shell 解释成了真换行，
于是 C 字符串在源码里断成两行，gcc 报 unterminated string。

这里按「行内容」定位并合并，不依赖行号（行号会随编辑漂移）。
"""
import io
import sys

PATH = "bench/wlshot.c"
BS = chr(92)  # backslash

def main():
    with io.open(PATH, encoding="utf-8") as f:
        lines = f.read().split("\n")

    out = []
    i = 0
    n = len(lines)
    fixed = 0

    while i < n:
        ln = lines[i]
        # 命中条件：行尾是 failures 或 reconnected（即 C 字符串被真换行截断）
        stripped = ln.rstrip()
        if stripped.endswith("failures") and stripped.count('"') % 2 == 1:
            nxt = lines[i + 1] if i + 1 < n else ""
            out.append(ln + BS + "n" + nxt)
            fixed += 1
            i += 2
            continue
        if stripped.endswith("reconnected") and stripped.count('"') % 2 == 1:
            nxt = lines[i + 1] if i + 1 < n else ""
            out.append(ln + BS + "n" + nxt)
            fixed += 1
            i += 2
            continue
        out.append(ln)
        i += 1

    with io.open(PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))

    print("fixed %d broken string literals" % fixed)
    return 0 if fixed == 2 else 1


if __name__ == "__main__":
    sys.exit(main())
