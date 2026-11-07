#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""修掉 C 源码里被真实换行截断的字符串字面量。

起因：用 bash heredoc 生成 C 代码时，`\\n` 会被 shell 解释成真换行，
于是 C 字符串在源码里断成两行，gcc 报 `missing terminating " character`。

这个脚本扫全文件，凡是「行内引号数为奇数的行」与「下一行以引号开头或
本行缺引号」的相邻两行，判断为被截断的字符串并合并（中间插入 \\n）。

用法：python scripts/fix_c_broken_strings.py <file.c> [...]
"""
import io
import sys

BS = chr(92)


def fix(path):
    with io.open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")

    out = []
    i = 0
    n = len(lines)
    fixed = 0

    while i < n:
        ln = lines[i]
        # 只在看起来是 C 代码行时处理（含 fprintf/printf/P/puts 等或纯字符串续行）
        if ln.count('"') % 2 == 1:
            # 找下一个含引号的行
            j = i + 1
            while j < n and '"' not in lines[j]:
                j += 1
            if j < n and j - i <= 2:
                # 把中间的行（若有）一起拼进来，并补 \n
                merged = ln + BS + "n" + lines[j]
                out.append(merged)
                fixed += 1
                i = j + 1
                continue
        out.append(ln)
        i += 1

    if fixed:
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(out))
    print("%s: fixed %d" % (path, fixed))
    return fixed


def main(argv):
    if len(argv) < 2:
        print("usage: fix_c_broken_strings.py <file.c> [...]")
        return 2
    total = 0
    for p in argv[1:]:
        total += fix(p)
    return 0 if total >= 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
