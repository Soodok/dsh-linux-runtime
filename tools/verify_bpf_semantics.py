#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BPF 过滤器语义自检（支持 aarch64 / x86_64）。

用法: python verify_bpf_semantics.py <bpf_tree.inc> [aarch64|x86_64]

模拟内核 cBPF 逐条执行生成出来的过滤器，验证：
  - 13 个 dirfd 类 syscall：arg0==AT_FDCWD 时 TRAP，其他 fd 时 ALLOW
  - 其余登记 syscall：必须 TRAP
  - clone：只有 vfork 形状（x1 低/高 32 位都为 0 且 flags 含 CLONE_VFORK）才 TRAP
  - 未登记的 syscall：必须 ALLOW
  - loader 自己的裸 syscall 桩（ip 高 32 位 == LOADER_BASE_HI）：必须 ALLOW

这个脚本的存在意义：cBPF 的跳转偏移是手算的，一旦算错就是
「静默放行一切」或「静默拦死一切」，编译期完全看不出来。
历史上错过三次：
  1. BPF_JA 偏移写在错误字段
  2. 忘了重载 nr
  3. **移植到 x86_64 时整个 BPF 树还用 aarch64 的编号** —— 树本身合法、
     编译通过、seccomp() 返回 0，但一个 syscall 都匹配不上，表现是
     容器里 `ls /` 列出**宿主**根目录。这个脚本能抓到，真机不装调试器抓不到。

NR 表不再硬编码：从 gen_bpf_tree.py 读取（它从 NDK 头文件取权威编号）。
"""
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_bpf_tree as gen                                    # noqa: E402

AUDIT = {
    'aarch64': 0xC00000B7,
    'x86_64': 0xC000003E,
}
# 各架构的「绝不登记」候选，用来确认它们是 ALLOW
UNLISTED = {
    'aarch64': [172, 64, 63, 222, 98, 29, 139, 167, 0, 1, 999, 500, 113],
    # x86_64：getpid=39 write=1 read=0 getuid=102 prctl=157 socket=41
    #          这里故意选**不是**本方案登记的那些
    'x86_64': [39, 1, 0, 102, 157, 41, 62, 2, 999, 500, 113, 60, 228],
}
LOADER_BASE_HI = 0x20
AT_FDCWD = 0xFFFFFF9C
CLONE_VFORK = 0x4000


def parse(path):
    txt = io.open(path, encoding='utf-8').read()
    return [l.strip().rstrip(',').strip() for l in txt.splitlines() if l.strip()]


def simulate(ins, nr, arg0=5, arg1_lo=0, arg1_hi=0,
             ip=(0x1111, 0x5678), arch='aarch64'):
    """逐条模拟。返回 TRAP / ALLOW / KILL / LOOP / FELL_OFF。"""
    i = 0
    acc = 0
    guard = 0
    while i < len(ins):
        guard += 1
        if guard > 10000:
            return 'LOOP'
        line = ins[i]
        if 'BPF_RET' in line:
            if 'KILL_PROCESS' in line:
                return 'KILL'
            return 'TRAP' if 'TRAP' in line else 'ALLOW'
        if 'BPF_JA' in line:
            off = int(line.rsplit(',', 1)[1].rstrip(')'))
            i = i + 1 + off
            continue
        m = re.search(r'BPF_(JGT|JEQ|JGE)\|BPF_K, (.+?), (\d+), (\d+)\)', line)
        if not m:
            # LD / ALU 类
            if 'BPF_LD' in line:
                if 'arch' in line:
                    acc = AUDIT[arch]
                elif 'instruction_pointer' in line:
                    # +4 取的是「高 32 位」在**大端**下的解释；对任何架构，
                    # 只要两个半字都不等于 LOADER_BASE_HI，就不该命中白名单。
                    acc = ip[1]
                elif 'args' in line:
                    if '+ 8' in line:
                        acc = arg1_lo
                    elif '+ 12' in line:
                        acc = arg1_hi
                    else:
                        acc = arg0
                else:
                    acc = nr
            elif 'BPF_ALU' in line and '0x4000' in line:
                acc = acc & CLONE_VFORK
            i += 1
            continue
        op, kk = m.group(1), m.group(2).strip()
        jt, jf = int(m.group(3)), int(m.group(4))
        if kk.startswith('SYS_'):
            v = gen.NR_NAMES[kk[4:]]
        elif kk == 'LOADER_BASE_HI':
            v = LOADER_BASE_HI
        elif kk.startswith('AUDIT_ARCH_'):
            v = AUDIT[kk[len('AUDIT_ARCH_'):].lower()
                      .replace('aarch64', 'aarch64').replace('x86_64', 'x86_64')] \
                if kk[len('AUDIT_ARCH_'):].lower() in AUDIT else AUDIT[arch]
        elif kk.startswith('0x'):
            v = int(kk, 16)
        else:
            v = int(kk)
        a = acc & 0xFFFFFFFF
        cand = v & 0xFFFFFFFF
        if op == 'JGT':
            go = a > cand
        elif op == 'JGE':
            go = a >= cand
        else:
            go = a == cand
        i = i + 1 + (jt if go else jf)
    return 'FELL_OFF'


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: verify_bpf_semantics.py <bpf_tree.inc> [aarch64|x86_64]')
    path = sys.argv[1]
    arch = sys.argv[2] if len(sys.argv) > 2 else 'aarch64'
    if arch not in AUDIT:
        sys.exit('arch 必须是 %s' % list(AUDIT))

    # 从生成器取该架构的权威编号（它读 NDK 头文件）
    gen.load_numbers(arch, verbose=True)
    nr = gen.NR_NAMES

    ins = parse(path)
    fails = []

    for nm, v in sorted(nr.items(), key=lambda x: x[1]):
        if nm in gen.DIRFD:
            r1 = simulate(ins, v, AT_FDCWD, arch=arch)
            r2 = simulate(ins, v, 0x10, arch=arch)
            if r1 != 'TRAP':
                fails.append('%s nr=%d arg0=AT_FDCWD -> %s (want TRAP)' % (nm, v, r1))
            if r2 != 'ALLOW':
                fails.append('%s nr=%d arg0=fd -> %s (want ALLOW)' % (nm, v, r2))
        elif nm == 'clone':
            a = simulate(ins, v, 0x4111, 0, 0, arch=arch)
            if a != 'TRAP':
                fails.append('clone vfork-shape(x1=0, VM|VFORK) -> %s (want TRAP)' % a)
            b = simulate(ins, v, 0x4111, 0x1234, 0, arch=arch)
            if b != 'ALLOW':
                fails.append('clone posix_spawn-shape(x1!=0) -> %s (want ALLOW)' % b)
            c = simulate(ins, v, 0x4111, 0, 0x1, arch=arch)
            if c != 'ALLOW':
                fails.append('clone x1-hi!=0 -> %s (want ALLOW)' % c)
            d = simulate(ins, v, 0x0011, 0, 0, arch=arch)
            if d != 'ALLOW':
                fails.append('clone plain-fork -> %s (want ALLOW)' % d)
        else:
            r = simulate(ins, v, 0, arch=arch)
            if r != 'TRAP':
                fails.append('%s nr=%d -> %s (want TRAP)' % (nm, v, r))

    # 未登记 syscall 必须放行
    openat = nr['openat']
    for t in UNLISTED[arch]:
        if t in nr.values():
            continue
        r = simulate(ins, t, 0, arch=arch)
        if r != 'ALLOW':
            fails.append('unlisted nr=%d -> %s (want ALLOW)' % (t, r))

    # loader 自己的裸 syscall 桩必须被放行
    r = simulate(ins, openat, AT_FDCWD, 0, 0,
                 ip=(0x1111, LOADER_BASE_HI), arch=arch)
    if r != 'ALLOW':
        fails.append('stub-ip bypass -> %s (want ALLOW)' % r)

    # 反向：另一架构的 openat 编号在本树里必须 ALLOW（证明这棵树用的是本架构编号）。
    # ★注意：两个架构的编号集有**真实重叠**（例如 aarch64 的 openat=56
    #   恰好等于 x86_64 的 clone=56），所以只有当该编号**不在**本架构登记的
    #   集合里时，这个断言才成立。
    other = 'x86_64' if arch == 'aarch64' else 'aarch64'
    gen.load_numbers(other, verbose=False)
    cross = gen.NR_NAMES['openat']
    gen.load_numbers(arch, verbose=False)
    if cross != openat and cross not in nr.values():
        r = simulate(ins, cross, AT_FDCWD, 0, 0, arch=arch)
        if r != 'ALLOW':
            fails.append(
                'cross-arch check: %s 的 openat=%d 在本树里 -> %s (want ALLOW，'
                '否则说明这棵树用的是别的架构的编号)' % (other, cross, r))

    print('架构: %s  指令数: %d' % (arch, len(ins)))
    print('语义错误: %d' % len(fails))
    for f in fails:
        print('  ' + f)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
