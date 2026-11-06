import sys

NR = {
 "getcwd":17,"mknodat":33,"mkdirat":34,"unlinkat":35,"symlinkat":36,"linkat":37,
 "renameat":38,"statfs":43,"fstatfs":44,"faccessat":48,"chdir":49,"fchmodat":53,
 "fchownat":54,"openat":56,"readlinkat":78,"newfstatat":79,"utimensat":88,
 "rt_sigaction":134,"rt_sigprocmask":135,"clone":220,"execve":221,"renameat2":276,
 "execveat":281,"statx":291,"sendto":206,"sendmsg":211,"recvfrom":207,"recvmsg":212,
 "connect":203,"openat2":437,"faccessat2":439,
}
DIRFD = {"openat","newfstatat","faccessat","readlinkat","statx","unlinkat","mkdirat",
         "fchmodat","fchownat","utimensat","mknodat","openat2","faccessat2"}
names = sorted(NR, key=lambda n: NR[n])

L = []
def label(n): L.append(('label', n))
def stmt(t):  L.append(('stmt', t))
def jmp(op,k,t,f): L.append(('jmp',(op,k,t,f)))
def ja(t): L.append(('ja', t))

def build(lo,hi):
    if lo > hi:
        ja('ALLOW'); return
    mid = (lo+hi)//2
    n = names[mid]
    jmp('JGT', n, 'R_'+n, 'NX_'+n)
    label('NX_'+n)
    jmp('JEQ', n, 'ACT_'+n, 'LEFT_'+n)
    label('ACT_'+n)
    if n in DIRFD:
        stmt('BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, args) + 0)')
        jmp('JEQ','0xFFFFFF9C','TRAP','ALLOW')
    elif n == 'clone':
        ja('CLONE_CHK')
    else:
        ja('TRAP')
    label('R_'+n);    build(mid+1, hi)
    label('LEFT_'+n); build(lo, mid-1)

build(0, len(names)-1)

label('CLONE_CHK')
stmt('BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, args) + 8)')
jmp('JEQ','0','C1','ALLOW')
label('C1')
stmt('BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, args) + 12)')
jmp('JEQ','0','C2','ALLOW')
label('C2')
stmt('BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, args) + 0)')
stmt('BPF_STMT(BPF_ALU|BPF_AND|BPF_K, 0x4000)')
jmp('JEQ','0','ALLOW','TRAP')

label('ALLOW'); stmt('BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW)')
label('TRAP');  stmt('BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_TRAP)')

idx = {}
for k,p in L:
    if k=='label': idx[p] = 0

PRO = [
 ('stmt','BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, arch))'),
 ('jmp',('JEQ','AUDIT_ARCH','ARCH_OK','KILLP')),
 ('label','KILLP'),
 ('stmt','BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_KILL_PROCESS)'),
 ('label','ARCH_OK'),
 ('stmt','BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, instruction_pointer) + 4)'),
 ('jmp',('JEQ','BASEHI','ALLOW','NOTSTUB')),
 ('label','NOTSTUB'),
 ('stmt','BPF_STMT(BPF_LD|BPF_W|BPF_ABS, (unsigned)offsetof(struct seccomp_data, nr))'),
]
full = PRO + L

# 两遍：先定标签位置，再算偏移
pos = {}
body = []
for k,p in full:
    if k=='label': pos[p] = len(body)
    else: body.append((k,p))

lines = []
for i,(k,p) in enumerate(body):
    if k=='stmt':
        lines.append('        ' + p + ',')
    elif k=='ja':
        off = pos[p] - i - 1
        lines.append('        BPF_STMT(BPF_JMP|BPF_JA, %d),' % off)
    else:
        op,kk,t,f = p
        if kk == 'AUDIT_ARCH': kx = 'AUDIT_ARCH_AARCH64'
        elif kk == 'BASEHI':   kx = 'LOADER_BASE_HI'
        elif kk in ('0','0xFFFFFF9C','0x4000'): kx = kk
        else: kx = 'SYS_' + kk
        jt = pos[t] - i - 1
        jf = pos[f] - i - 1
        lines.append('        BPF_JUMP(BPF_JMP|BPF_%s|BPF_K, %s, %d, %d),' % (op,kx,jt,jf))
print('\n'.join(lines))
sys.stderr.write('INSTR=%d\n' % len(body))
