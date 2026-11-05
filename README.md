# DSH Linux Runtime

**专有软件 / Proprietary software** — 许可条款见 [LICENSE](./LICENSE)，非开源。
**请勿**假设你可以像 MIT/Apache 项目那样复制、修改或再分发本仓库的任何部分。

在 Android 上**免 root** 运行完整 Linux 用户态（glibc / musl 均可）的容器运行器。
不走 ptrace、不需要 user namespace、不需要 chroot、不需要解锁 BL 或刷 Magisk。

---

## 这是什么

`dsh_alpine` 是 [DSH Mobile](https://github.com/Soodok/Deepseek-Harness-Local-Android)
里 Linux 模式使用的运行器。它被编进 APK，把 Alpine 或 Ubuntu 的 rootfs 挂到
一个普通 App 的私有目录里，然后让里面的 `node`、`gcc`、`python`、`nginx`
等程序**以原本的 uid 正常运行** —— 没有 `su`，没有系统改动。

它不是模拟器，也不是虚拟机：**用宿主内核**（`uname -r` 显示设备内核版本），
所以性能接近原生。

## 为什么不用现有方案

| 方案 | 免 root | 性能 | 本项目的差异 |
|---|---|---|---|
| chroot | ❌ 需 CAP_SYS_ADMIN | 原生 | 要 root |
| PRoot | ✅（ptrace 拦系统调用） | IO 慢 20~30 倍 | 每次 syscall 都要 ptrace 往返 |
| tawcroot | ✅（seccomp） | 接近原生 | GPL-3.0，无法并入 MIT/专有项目 |
| QEMU user | ✅ | 慢 5~20 倍 | 指令级翻译 |
| **dsh_alpine** | ✅（seccomp） | **接近原生** | 专有，无 GPL 传染 |

## 实测数据

同一台设备（OPPO PLZ110 / Android 16 / 骁龙 8 Elite）、同一 Alpine 3.22 rootfs、
程序内 `clock_gettime(CLOCK_MONOTONIC)` 自计时：

| 指标 | 原生 chroot | dsh_alpine | PRoot |
|---|---|---|---|
| `exec`（fork+exec+wait） | 0.65~0.79 ms | 0.97~1.26 ms | 6.28~8.69 ms |
| `getpid` ×6000 | — | 0.27 µs | 0.39 µs |
| `stat` ×600 | 1.9~2.4 µs | 2.7~3.6 µs | 3.2~4.4 µs |
| `open+read+close` ×300 | — | 11.6 µs | 7.1 µs |
| 文件 IO（2 万次） | 62 ms | 116~164 ms | **4104~4920 ms** |
| CPU 运算基准 | 101~110 ms | 124~133 ms | 105~133 ms |

真实负载：

- SQLite 3.54.1（271131 行）`-O2` 编译：**38.6s** vs PRoot 49.2s，产物 sha256 完全一致
- 30 个 `.c` 文件 `gcc -O2 -c` + 链接：**1.27~1.40s** vs PRoot 2.50~4.23s
- git 40 次 commit：**1.17s** vs PRoot 3.20s
- 硬件 GPU（Adreno 840，经 bionic 进程级桥）：**1.76 ms/帧**，软件渲染 30.54 ms/帧（**快 35×**）

内存：容器层自身约 **1.8 MB**；容器内跑 DSH 引擎 `VmRSS ≈ 171 MB`，
同引擎原生模式 `≈ 235 MB`（差异来自 musl node 的精简 ICU，不是容器开销）。

## 工作原理（概要）

```
App (bionic)  ──fork/exec──▶  dsh_alpine (freestanding, 固定基址)
                                    │
                                    ├─ 装 seccomp BPF 过滤器（按 syscall 号二叉搜索树）
                                    ├─ 装 SIGSYS handler（裸 svc 桩，不碰 libc）
                                    │
                  ┌─────────────────┴──────────────────┐
                  │  被拦的 syscall 在 handler 里翻译：  │
                  │   /etc/foo  →  rootfs/etc/foo       │
                  │   127.0.0.1:53 → 代理端口（双向）    │
                  └─────────────────┬──────────────────┘
                                    │
                                    ▼
                            手写 ELF loader
                     读 PT_LOAD / 建栈 / 合成 auxv
                                    │
                                    ▼
                          guest（musl 或 glibc）
                             用宿主内核运行
```

完整机制、21 个踩坑记录与复现步骤见 `docs/`。

## 目录

```
src/dsh_alpine.c          运行器全部实现（单文件，约 2500 行，freestanding 无 libc）
tools/gen_bpf_tree.py     生成 seccomp BPF 二叉搜索树（改 syscall 表后重跑）
tools/bpf_tree.inc        生成产物，被源码内嵌
tools/verify_bpf_semantics.py  模拟内核 cBPF 逐条执行，校验树语义
build/build.sh            编译脚本（需要 NDK r28+）
docs/                     机制文档、踩坑记录、性能测量方法
```

## 编译

需要 NDK r28 或更新（r26b 有 TLS underaligned 链接 bug）。

```bash
NDK=/path/to/ndk/28.2.13676358
./build/build.sh "$NDK"
```

产物 `build/libdsh_alpine.so`，校验：

```bash
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf -h build/libdsh_alpine.so
# 期望：Type: EXEC   Entry point address: 0x2000026...
```

**六条硬约束**（缺一即运行时崩，详见 `src/dsh_alpine.c` 头部注释）：

1. freestanding 静态，**不链接 bionic libc** —— handler 内绝不能调 libc
2. 固定基址 `0x2000000000`、`ET_EXEC`、无 PIE —— BPF 白名单靠比对指令指针
3. `-ffreestanding -fno-builtin` —— 阻止编译器把字节循环优化成 `memcpy`
4. `-Wl,-z,max-page-size=0x20000` —— Android 15+ 的 16KB 页内核要求
5. 关掉 fortify 与 stack-protector —— 会引用 libc 符号导致链接失败
6. 仅 aarch64 —— 用 `register x0` 与 AArch64 ucontext 布局

## 已知限制

- **仅 aarch64**（实现是架构专属的，x86_64 编不过）
- **需要 Android 10+**（依赖 seccomp 与 `O_PATH` 等）
- `CONFIG_SYSVIPC` 未编译的内核上跑不了 PostgreSQL / MariaDB 服务端
  （`shmget` 无解，PRoot 与 tawcroot 同样做不到）
- 不是安全沙箱：单进程设计，guest 里的恶意代码可以逃逸到宿主视角
- 容器内后台常驻服务（nginx/redis）需从宿主侧拉起

## 许可

**Proprietary. All rights reserved.** 详见 [LICENSE](./LICENSE)。

允许：阅读、学习、在 issue/PR 中引用讨论、随官方 App 一起编译运行、复现性能数据。
禁止：复制、修改、再分发、并入其他产品、做竞品、去除声明。

商用或其它授权请联系：<https://github.com/Soodok/Deepseek-Harness-Local-Android/issues>

## 与 DSH Mobile 的关系

本仓库是 [DSH Mobile](https://github.com/Soodok/Deepseek-Harness-Local-Android)
的子项目，持有该运行器的**专有实现**。DSH Mobile 主仓继续保持 MIT；
`dsh_alpine.c` 单独使用本仓库的专有许可（主仓内的副本同样受此约束）。
