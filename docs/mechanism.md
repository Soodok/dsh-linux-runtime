# 实现机制

本文说明 `dsh_alpine` 如何在**非 root** 的 Android App 沙箱里运行完整 Linux 用户态。
所有结论都来自真机实测（OPPO PLZ110 / Android 16 / KernelSU 可 root 但全程用 uid 2000）。

## 0. 为什么需要自己写

Android 上跑 Linux 用户态的现成方案都有硬伤：

- **chroot** 需要 `CAP_SYS_ADMIN`，非 root 拿不到
- **user namespace** 是最干净的免 root 路线，但绝大多数安卓内核
  `# CONFIG_USER_NS is not set`（本机实测确认）
- **PID namespace** 同样常缺（本机 `# CONFIG_PID_NS is not set`）
- **PRoot** 用 ptrace 拦系统调用，免 root，但每次 syscall 都要两次上下文切换 ——
  实测文件 IO 慢 20~35 倍
- **tawcroot** 用 seccomp 做到接近原生，但整体 GPL-3.0，无法并入非 GPL 项目

剩下的路只有一条：**seccomp BPF + SIGSYS handler + 手写 ELF loader**。

## 1. seccomp 过滤器

`seccomp(SECCOMP_SET_MODE_FILTER, 0, &prog)` 把一个 cBPF 程序挂到进程上。
之后每条 syscall 都先过这个程序，返回值决定行为：

| 返回值 | 含义 |
|---|---|
| `SECCOMP_RET_ALLOW` | 放行 |
| `SECCOMP_RET_TRAP` | 发 `SIGSYS` 给进程，handler 可改寄存器后重试 |
| `SECCOMP_RET_KILL_PROCESS` | 直接杀 |

我们用 `TRAP`：不认识的路径交给内核原样处理，需要翻译的拦下来自己改参数。

### 二叉搜索树而非线性扫描

初版是 25 条 `TRAP_PAIR` + 12 条 dirfd 检查展开成 **108 条指令线性扫描**，
每条 syscall 都要全扫一遍 —— 实测未拦截的 `getpid` 被拖到 **0.7~1.4 µs**
（原生 0.002 µs），这是当时最大的固定开销。

改成按 syscall 号排序的二叉搜索树后最多 **13 条指令**，与表大小无关，
`getpid` 降到 **0.27 µs**。

三个必须记住的坑：

1. **cBPF 只允许向前跳** —— 公共尾块（ALLOW/TRAP）必须排在树之后
2. **`BPF_JA` 的偏移在 `k` 字段** —— 写成 `BPF_JUMP(BPF_JMP|BPF_JA, 0, 0, off)`
   实际是 `k=0`/`jf=off`，**不会跳转**（症状：Alpine 报 `Bad system call`）
3. **`static const` 数组初始化器必须是编译期常量** —— 原来用变量做 stub IP
   白名单报 `initializer element is not a compile-time constant`；且数组变大后
   编译器会生成 `.rodata → 栈` 的 `memcpy`，freestanding 下报
   `undefined symbol: memcpy`，**必须 `static`**

## 2. SIGSYS handler

handler 拿到 `ucontext`，可以读写被拦 syscall 的参数与返回值：

```c
case SYS_openat: {
    const char *p = (const char *)a1;   // 路径
    if (p[0] == '/') {
        char rel[PATH_MAX];
        make_rel(rel, p);               // /etc/foo → ./etc/foo
        r[0] = raw6(SYS_openat, g_rootfs_fd, (long)rel, a2, a3, a4, a5);
        return;
    }
    ...
}
```

### 铁律一：handler 内绝不能调 bionic libc

guest 的 musl `ld` 启动后会改写 `TPIDR_EL0` 接管 TLS。此后 handler 里任何
`snprintf` / `memcpy` / `malloc` / `errno` 调用都会 **SEGV**。

症状极具误导性：`echo` / `uname` / `pwd` 正常，`cat` / `ls` / `id` 全段错误 ——
看起来像 "rootfs 缺东西"，实际是 handler 自己崩了。

修法：handler 全程只用裸 `svc` 桩 + 手写字节循环。

### 铁律二：`instruction_pointer` 是 svc 地址 **+4**

aarch64 上 `seccomp_data.instruction_pointer` 报的是 `svc` 指令**之后**的地址。
白名单要写 `&svc_insn + 4`，否则自己的裸 svc 桩也会被拦，形成无限循环
（实测：`rc=124` 超时且无任何输出）。

### 铁律三：`execve` 后 handler 被重置

过滤器跨 `execve` 存活，但 **SIGSYS 处置被内核重置为默认**。
所以新镜像必须自己重装 handler —— 这正是「自我重执行」方案的动机。

## 3. 自我重执行（self-reexec）

最初用 `fork + wait4` 实现 execve：handler 里 fork 子进程、子进程加载新镜像、
父进程等它结束。功能正常但 `fork` 会复制整个 mm，代价 364 µs 以上。

改用 `execveat(self_fd, "", argv, envp, AT_EMPTY_PATH)` 让**内核**真 exec 自己：

- 内核建全新 mm，所以 `CLONE_VM` 的 posix_spawn 子进程也能安全脱离父进程
- PID / PPID / cwd / umask / rlimit 全部保持
- 新 `_start` 重装 handler、重装 altstack、重开 rootfs fd，然后手动装载 guest

**必须 freestanding 且固定基址**，否则新镜像的地址会变，IP 白名单失效。

## 4. 手写 ELF loader

不调 `ld.so`（那是宿主的，且要先 chroot）。自己读 ELF：

1. 校验 `ELFCLASS64` / `EM_AARCH64`
2. 扫 `PT_LOAD` 求 `[lo, hi)`，整段 anon **RW** 预留（段间空隙=零页）
3. 逐段 `mmap(MAP_FIXED)` 覆盖文件部分（v8 起不再 memcpy）
4. `ET_EXEC` **原位加载**（`GHI = 0`）；`ET_DYN` 用 `0x5000000000`
5. 手工合成栈：argv → envp → auxv（**注意 16 字节对齐**）
6. `mov sp, ...; br entry`

### 坑：`ET_EXEC` 必须原位

Alpine 的 `gcc` / `cc1` / `binutils` 是 `ET_EXEC`（Entry 0x40472c），
busybox 是 `ET_DYN`（PIE）。一律加 `0x5000000000` 基址会让 ET_EXEC 的
内部绝对地址错位，取指即崩。

### 坑：入口 sp 必须 16 字节对齐

AArch64 ABI 要求。栈构造在 auxv 后压 `(envp NULL) + ec + (argv NULL) + ac + argc`
= `3+ec+ac` 槽，只有 `ec+ac` 为奇数才对。

glibc 的 `ld.so` 入口第一条 `stp x29,x30,[sp,#-16]!` 直接触发
`SIGBUS / BUS_ADRALN`（`si_addr` 恰好等于 `sp`）；**musl 不要求 16 字节成对
存储，所以 Alpine 一直正常、把问题盖住了**。

补位必须压在 auxv **之前**，绝不能插在 `envp NULL` 与 auxv 之间 ——
glibc 从 envp 往后扫 auxv，插中间会被读成 `AT_NULL`，报
`-c: cannot open shared object file`。

### 坑：文件映射不能越过 EOF

Ubuntu 的 `ld.so` 用 `p_align = 0x10000`，4KB 页下映射窗口会超出文件末尾，
尾部无后备页 → 清零 BSS 时 `SIGBUS`。修法：先 `lseek` 取文件长，
`avail >= file_len` 才走文件映射，否则 `pread64` 只拷真实字节。

**取文件长度一律用 lseek，不要手写 `struct stat` 字段偏移** ——
在 aarch64/bionic 上写错会直接 SIGSEGV。

## 5. DNS 双向端口改写

musl 的 resolver 把 DNS 端口**硬编码成 53**：`nameserver 127.0.0.1:15353`
里的端口部分被整段忽略。而 53 是特权端口 App uid 绑不上，Android 又按 uid
拦掉发往外部 53 的包 —— 于是容器内 `apk` / `wget` / `pip` 全部 `bad address`。

解法是在 loader 里做**双向**改写：

```
出程 sendto / sendmsg   127.0.0.1:53        →  127.0.0.1:<代理端口>
回程 recvfrom / recvmsg 来源 <代理端口>      →  改回 53
```

**回程不能省**。musl 的 `__res_msend_rc` 会校验应答来源端口必须等于它发出的
目标端口，只改出程时症状极具误导性：`sendto` 返回成功、代理日志也打出
`udp replied 144B`，但 `getaddrinfo` 仍返回 `EAI_AGAIN`。

端口来自环境变量 `DSH_DNS_PORT`。**必须在 `ldr_entry` 入口直接扫 `envp`**，
不能在 handler 里读 `g_env_store` —— handler 可能在它填好之前就被调用
（启动早期的 stat/open 就会 trap），那一次读到 0 并被永久缓存。

## 6. 宿主路径透传

`/proc` `/sys` `/dev` 前缀命中时**不翻译**，原样交给内核。这样 `free` / `df` /
`ps` / `nproc` 都能用（等价 `proot -b`，但不需要它）。

bind 映射（`-m guest:host`）与 `chdir` / `getcwd` 必须优先级一致，
否则 `cd /tmp` 会进 rootfs 的 `/tmp` 而 `cat /tmp/x` 走 bind，同一路径两种视图。

## 7. bionic 进程级桥（硬件 GPU）

musl **装不了 bionic 库**（`dlopen` 报 `__register_atfork: symbol not found`），
所以桥必须是进程级：在装载任何 guest 之前 fork 一个**不带 filter** 的 cleaner
进程，需要跑 bionic 程序时把 argv 交给它代跑。

关键点：cleaner 必须在**第一次 `install_filter()` 之前** fork ——
filter 装上不可移除，fork 必然继承。

## 参考

- seccomp: `man 2 seccomp`，内核 `Documentation/userspace-api/seccomp_filter.rst`
- AArch64 ELF ABI: "Procedure Call Standard for the Arm 64-bit Architecture"
- musl resolver: `src/network/res_msend.c` 的 `__res_msend_rc`
