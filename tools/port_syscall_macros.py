#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 src/dsh_alpine.c 里裸写的 aarch64 syscall 号换成 <sys/syscall.h> 的 SYS_* 宏。

为什么必须做：
  作者全部按 aarch64 的编号硬编码（openat=56、write=64、clone=220…）。
  这些数字在 x86_64 上是**完全不同的 syscall**（56=clone、64=semget、220=clone 之外的
  别的东西），移植后必然出现 uselib/semget 之类的怪错。

做法：
  逐个把 `raw6(56 /*SYS_openat*/` 与 `raw6(56` 这种形态统一成 `raw6(SYS_openat`。
  注释里已经写清了语义，所以映射取自注释本身，不靠猜。

安全措施：
  - 只替换 `raw6(<num>` 这种紧邻形态，不碰其它数字。
  - 若同一数字在不同位置有冲突注释，脚本会报错退出（人工裁决）。
"""
import io
import re
import sys

SRC = 'src/dsh_alpine.c'

# aarch64 编号 -> <sys/syscall.h> 宏名（取自源码注释，已逐条核对语义）
MAP = {
    56:  'SYS_openat',
    57:  'SYS_close',
    63:  'SYS_read',
    64:  'SYS_write',
    94:  'SYS_exit_group',
    132: 'SYS_sigaltstack',
    134: 'SYS_rt_sigaction',
    167: 'SYS_prctl',
    172: 'SYS_getpid',
    173: 'SYS_getppid',
    174: 'SYS_getuid',
    198: 'SYS_socket',
    200: 'SYS_bind',
    201: 'SYS_listen',
    203: 'SYS_connect',
    220: 'SYS_clone',
    221: 'SYS_execve',
    242: 'SYS_accept4',
    260: 'SYS_wait4',
    277: 'SYS_seccomp',
}


def main():
    s = io.open(SRC, encoding='utf-8').read()

    # 找出所有 raw6(<digits> 出现，检查注释一致性
    conflicts = []
    for m in re.finditer(r'raw6\((\d+)(?: /\*([^*]*)\*/)?', s):
        num = int(m.group(1))
        note = (m.group(2) or '').strip()
        if num not in MAP:
            conflicts.append((num, note, 'UNKNOWN'))
            continue
        want = MAP[num]
        if note and note.upper().startswith('SYS_'):
            if note.upper() != want.upper():
                conflicts.append((num, note, want))
        elif note and note.lower() not in want.lower():
            # 例如 /*getuid*/ vs SYS_getuid —— 允许子串包含
            if note.lower().lstrip('sys_') not in want.lower():
                conflicts.append((num, note, want))
    if conflicts:
        print('冲突（需人工裁决）:')
        for c in conflicts[:20]:
            print('  num=%s note=%r want=%s' % c)
        sys.exit(1)

    # 替换：raw6(56 /*...*/  -> raw6(SYS_openat /*...*/   ；raw6(56, -> raw6(SYS_openat,
    def repl(m):
        num = int(m.group(1))
        rest = m.group(2) or ''
        return 'raw6(' + MAP[num] + rest

    pat = re.compile(r'raw6\((\d+)((?: /\*[^*]*\*/)?)')
    out, n = pat.subn(repl, s)
    io.open(SRC, 'w', encoding='utf-8', newline='\n').write(out)
    print('replaced %d call sites' % n)


if __name__ == '__main__':
    main()
