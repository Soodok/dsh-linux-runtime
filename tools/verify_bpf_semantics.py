#!/usr/bin/env python3
"""BPF 过滤器语义自检。

用法: python verify_bpf_semantics.py tools/bpf_tree.inc

模拟内核 cBPF 逐条执行生成出来的过滤器，验证：
  - 13 个 dirfd 类 syscall：arg0==AT_FDCWD 时 TRAP，其他 fd 时 ALLOW
  - 11 个纯 TRAP 类 syscall：必须 TRAP
  - clone：只有 vfork 形状（x1==0 且 flags 含 CLONE_VFORK）才 TRAP
  - 未登记的 syscall：必须 ALLOW
  - loader 自己的裸 svc 桩（高 32 位 == LOADER_BASE_HI）：必须 ALLOW

这个脚本的存在意义：cBPF 的跳转偏移是手算的，一旦算错就是
「静默放行一切」或「静默拦死一切」，编译期完全看不出来。
历史上错过两次（BPF_JA 偏移写在错误字段、忘了重载 nr），
都是靠这个脚本在推送设备之前抓到的。
"""
import io
import re
import sys

NR = {
 "connect":203,
    "getcwd": 17, "mknodat": 33, "mkdirat": 34, "unlinkat": 35, "symlinkat": 36,
    "linkat": 37, "renameat": 38, "statfs": 43, "fstatfs": 44, "faccessat": 48,
    "chdir": 49, "fchmodat": 53, "fchownat": 54, "openat": 56, "readlinkat": 78,
    "newfstatat": 79, "utimensat": 88, "rt_sigaction": 134, "rt_sigprocmask": 135,
    "clone": 220, "execve": 221, "renameat2": 276, "execveat": 281, "statx": 291,
    "openat2": 437, "faccessat2": 439,
}
DIRFD = {
    "openat", "newfstatat", "faccessat", "readlinkat", "statx", "unlinkat",
    "mkdirat", "fchmodat", "fchownat", "utimensat", "mknodat", "openat2",
    "faccessat2",
}
UNLISTED = [172, 64, 63, 222, 98, 29, 139, 167, 0, 1, 999, 500, 113]
LOADER_BASE_HI = 0x20
AUDIT_ARCH_AARCH64 = 0xC00000B7
AT_FDCWD = 0xFFFFFF9C
CLONE_VFORK = 0x4000


def parse(path):
    txt = io.open(path, encoding="utf-8").read()
    return [l.strip().rstrip(",").strip() for l in txt.splitlines() if l.strip()]


def simulate(ins, nr, arg0=5, arg1_lo=0, arg1_hi=0, ip=(0x1111, 0x5678)):
    """逐条模拟。返回 TRAP / ALLOW / KILL / LOOP / FELL_OFF。"""
    i = 0
    acc = 0
    guard = 0
    while i < len(ins):
        guard += 1
        if guard > 10000:
            return "LOOP"
        line = ins[i]
        if "BPF_RET" in line:
            if "KILL_PROCESS" in line:
                return "KILL"
            return "TRAP" if "TRAP" in line else "ALLOW"
        if "BPF_JA" in line:
            off = int(line.rsplit(",", 1)[1].rstrip(")"))
            i = i + 1 + off
            continue
        m = re.search(r"BPF_(JGT|JEQ|JGE)\|BPF_K, (.+?), (\d+), (\d+)\)", line)
        if not m:
            # LD / ALU 类
            if "BPF_LD" in line:
                if "arch" in line:
                    acc = AUDIT_ARCH_AARCH64
                elif "instruction_pointer" in line:
                    acc = ip[1] if line.rstrip(")").endswith("+ 4") else ip[0]
                elif "args" in line:
                    if "+ 8" in line:
                        acc = arg1_lo
                    elif "+ 12" in line:
                        acc = arg1_hi
                    else:
                        acc = arg0
                else:
                    acc = nr
            elif "BPF_ALU" in line and "0x4000" in line:
                acc = acc & CLONE_VFORK
            i += 1
            continue
        op, kk, jt, jf = m.group(1), m.group(2).strip(), int(m.group(3)), int(m.group(4))
        if kk.startswith("SYS_"):
            v = NR[kk[4:]]
        elif kk == "LOADER_BASE_HI":
            v = LOADER_BASE_HI
        elif kk == "AUDIT_ARCH_AARCH64":
            v = AUDIT_ARCH_AARCH64
        elif kk.startswith("0x"):
            v = int(kk, 16)
        else:
            v = int(kk)
        a = acc & 0xFFFFFFFF
        cand = v & 0xFFFFFFFF
        if op == "JGT":
            go = a > cand
        elif op == "JGE":
            go = a >= cand
        else:
            go = a == cand
        i = i + 1 + (jt if go else jf)
    return "FELL_OFF"


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: verify_bpf_semantics.py <bpf_tree.inc>")
    ins = parse(sys.argv[1])
    fails = []

    for nm, v in sorted(NR.items(), key=lambda x: x[1]):
        if nm in DIRFD:
            r1 = simulate(ins, v, AT_FDCWD)
            r2 = simulate(ins, v, 0x10)
            if r1 != "TRAP":
                fails.append("%s nr=%d arg0=AT_FDCWD -> %s (want TRAP)" % (nm, v, r1))
            if r2 != "ALLOW":
                fails.append("%s nr=%d arg0=fd -> %s (want ALLOW)" % (nm, v, r2))
        elif nm == "clone":
            # vfork 形状：child_stack(x1)==0 且 flags 含 CLONE_VFORK -> TRAP
            a = simulate(ins, v, 0x4111, 0, 0)
            if a != "TRAP":
                fails.append("clone vfork-shape(x1=0, VM|VFORK) -> %s (want TRAP)" % a)
            # posix_spawn 形状：x1!=0 -> ALLOW（绝不能拦，否则 gcc/pip/npm 全挂）
            b = simulate(ins, v, 0x4111, 0x1234, 0)
            if b != "ALLOW":
                fails.append("clone posix_spawn-shape(x1!=0) -> %s (want ALLOW)" % b)
            # x1 高 32 位非 0 也算自定义栈
            c = simulate(ins, v, 0x4111, 0, 0x1)
            if c != "ALLOW":
                fails.append("clone x1-hi!=0 -> %s (want ALLOW)" % c)
            # 普通 fork：无 CLONE_VFORK -> ALLOW
            d = simulate(ins, v, 0x0011, 0, 0)
            if d != "ALLOW":
                fails.append("clone plain-fork -> %s (want ALLOW)" % d)
        else:
            r = simulate(ins, v, 0)
            if r != "TRAP":
                fails.append("%s nr=%d -> %s (want TRAP)" % (nm, v, r))

    for t in UNLISTED:
        if t in NR.values():
            continue
        r = simulate(ins, t, 0)
        if r != "ALLOW":
            fails.append("unlisted nr=%d -> %s (want ALLOW)" % (t, r))

    # loader 自己的裸 svc 桩必须被放行
    r = simulate(ins, 56, AT_FDCWD, 0, 0, ip=(0x1111, LOADER_BASE_HI))
    if r != "ALLOW":
        fails.append("stub-ip bypass -> %s (want ALLOW)" % r)

    print("指令数: %d" % len(ins))
    print("语义错误: %d" % len(fails))
    for f in fails:
        print("  " + f)
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
