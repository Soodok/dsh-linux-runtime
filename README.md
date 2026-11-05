# DSH Linux Runtime

> ### ⚠️ 专有软件 / PROPRIETARY — NOT OPEN SOURCE
>
> 本仓库**不是开源项目**。代码公开是为了**可审查、可讨论**，不构成任何许可授予，
> 也不构成对权利的放弃。
>
> **可以**：阅读研究 / issue·PR 中引用有限片段讨论 / 用未修改副本自行编译并个人非商业运行 /
> 运行官方构建 / 复现并发布基准数据（须注明来源）。
>
> **不可以**：复制镜像 / 修改改编 / 分发再分发 / 并入其他产品 / 作为服务提供 /
> 用作 AI 训练数据 / 开发竞品或提取设计重实现 / 去除声明 / 转让许可。
>
> **商业使用须另行授权**（含企业内部生产性使用）。
>
> 完整条款：[LICENSE](./LICENSE)（英文正本）· [中文译本](./LICENSE.zh-CN.md)。
> 授权咨询请开 issue。

<br>

在 Android 上**免 root** 运行完整 Linux 用户态（glibc / musl 均可）。
不走 ptrace、不需要 user namespace、不需要 chroot、不需要解锁 BL 或刷 Magisk。

用**宿主内核**运行（`uname -r` 显示设备内核），所以性能接近原生。

---

## 目录

- [这是什么](#这是什么)
- [为什么不用现有方案](#为什么不用现有方案)
- [性能](#性能)
- [工作原理](#工作原理)
- [编译](#编译)
- [已知限制](#已知限制)
- [许可](#许可)

---

## 这是什么

`dsh_alpine` 是一个**容器运行器**：把 Alpine 或 Ubuntu 的 rootfs 挂进普通 App 的
私有目录，然后让里面的 `node`、`gcc`、`python`、`nginx` 等程序以原本的 uid
正常运行 —— 没有 `su`，没有系统改动。

它是 [DSH Mobile](https://github.com/Soodok/Deepseek-Harness-Local-Android)
Linux 模式的执行层。

```bash
# 典型用法：在容器里编译并运行一个 C 程序
dsh_alpine --dsh-reexec-v1 \
  -m /eng:/data/data/<pkg>/files/engine \
  --rootfs  /data/data/<pkg>/files/linux/distros/alpine \
  --interp  /lib/ld-musl-aarch64.so.1 \
  --guest   /usr/bin/gcc  /usr/bin/gcc -O2 -o /tmp/a.out /tmp/a.c
```

已验证可用：`gcc` / `g++` / `make` / `cmake` / `clang` / `python3` / `node` /
`npm` / `pip` / `git` / `sqlite3` / `ffmpeg` / `ImageMagick` / `nginx` / `redis` /
`vim` / `jq` / `rg` / `htop` / `tmux` / `openssl`，以及 `apk` 联网装包。

---

## 为什么不用现有方案

| 方案 | 免 root | 性能 | 问题 |
|---|---|---|---|
| `chroot` | ❌ | 原生 | 需 `CAP_SYS_ADMIN` |
| user namespace | ✅ | 原生 | 多数安卓内核 `# CONFIG_USER_NS is not set` |
| PRoot | ✅ | **IO 慢 20~30 倍** | 每 syscall 两次 ptrace 上下文切换 |
| tawcroot | ✅ | 接近原生 | **GPL-3.0**，无法并入非 GPL 项目 |
| QEMU user | ✅ | 慢 5~20 倍 | 指令级翻译 |
| **dsh_alpine** | ✅ | **接近原生** | 专有许可，需授权才能复用 |

---

## 性能

测量条件：OPPO PLZ110 / Android 16 / 骁龙 8 Elite Gen 5（SM8850）/ 8 核（厂商限频约 55%，
绝对值偏保守；同一轮内各列对比有效）。同一 rootfs、同一设备、同一脚本、
**程序内 `clock_gettime(CLOCK_MONOTONIC)` 自计时**，`rounds=500`，3 轮取中位。

基准程序见 [`bench/bench_full.c`](./bench/bench_full.c)（7 维，输出 `key=value`）。

### 与 PRoot 的逐项对比

| 指标 | dsh_alpine | PRoot 5.1.107 | 倍率 |
|---|---:|---:|---:|
| `exec`（fork+exec+wait 一轮） | **1.46 ms** | 2.76 ms | **1.89×** |
| `io`（open+read+close） | **0.009 ms** | 0.19 ms | **21×** |
| `stat` | **0.0037 ms** | 0.10 ms | **27×** |
| `write` | **0.0089 ms** | 0.22 ms | **25×** |
| `dir`（readdir 2025 项） | **0.082 ms** | 0.56 ms | **6.8×** |
| `getpid` | 0.00023 ms | 0.00046 ms | 2× |
| `cpu`（纯运算） | 0.0045 ms | 0.0075~0.16 ms | 1.7~35× |

所有 `*_fail` 均为 0，`dir_entries` 双方一致（1012500 / 2025000）。

### 三方对照（含 root 基线）

启动耗时（绝对路径，rc=0，3 轮取最优）：

| 程序 | 真 chroot（root） | dsh_alpine（非 root） | PRoot（非 root） |
|---|---:|---:|---:|
| `python3 -c pass` | 18.1 ms | **17.1 ms** | 43.8 ms |
| `git --version` | 1.1 ms | 6.8 ms | 2.3 ms |
| `gcc --version` | 1.1 ms | 6.8 ms | 2.3 ms |
| `sqlite3` | 1.3 ms | 7.4 ms | 3.1 ms |
| `cmake --version` | 10.2 ms | 31.2 ms | 31.1 ms |
| `node -e 0` | 44.3 ms | 111.5 ms | 44.9 ms |

文件 IO / syscall（2 万次 open+read+close / 20 万次 getpid）：

| 组 | 文件 IO | syscall |
|---|---:|---:|
| 真 chroot（root） | 62 ms | 32 ms |
| **dsh_alpine（非 root）** | **116~164 ms** | **34~35 ms** |
| PRoot（非 root） | **4104~4920 ms** | 91~96 ms |

→ 文件 IO 比 PRoot **快约 30 倍**，syscall 快 2.7×，CPU 运算持平。

### 真实负载

| 负载 | dsh_alpine | PRoot | 倍率 |
|---|---:|---:|---:|
| SQLite 3.54.1 `-O2` 编译（27 万行） | **38.6 s** | 49.2 s | 1.27× |
| SQLite 全量链接 | **47.7 s** | 51.3 s | 1.08× |
| 30 个 `.c` 编译 + 链接 | **1.27~1.40 s** | 2.50~4.23 s | 1.8~3.3× |
| `git` 40 次 commit | **1.17 s** | 3.20 s | 2.7× |
| `tar` 打包 300 文件 | **0.05 s** | 0.23 s | 4.6× |
| `cp` 300 文件 | **0.65 s** | 1.80 s | 2.8× |

编译产物与 chroot 侧 **sha256 完全一致**（SQLite `-O0`→`7a6bace197c4`，
`-O2`→`515bba8fdb83`）—— 结果可复现。

### 内存

| 对象 | VmRSS |
|---|---:|
| 容器 loader 进程本身 | **1836 kB** |
| 宿主原生 `sh` | 5396 kB |
| 容器内 Python 启动 | 8172 kB（chroot 7224 kB，差 +948 kB） |

分配 200 MB 大内存时容器与 chroot 差 **+820 kB，无放大**。
`VmSize` 显示 2 GB+ 是固定基址的保留映射，**不是实占**。

> 多出来的 ≈1 MB 是 loader 自身（19 KB 二进制 + interp 映射 + 栈），
> 不是 ptrace 那种双进程 / 影子页表开销。

### 硬件 GPU（bionic 进程级桥）

同一程序同参数（10000 三角形 × 20 帧 @ 1024²）：

| 后端 | 每帧 |
|---|---:|
| Adreno 840（硬件） | **0.74~1.76 ms** |
| llvmpipe（软件） | 26.3~30.5 ms |

差 **35×**。musl 装不了 bionic 库（ABI 级不兼容），所以桥是**进程级**的：
在装载 guest 之前 fork 一个不带 seccomp filter 的 cleaner 进程代跑。

---

## 工作原理

```
App (bionic)
   │ fork + exec
   ▼
dsh_alpine  ─── freestanding、ET_EXEC、固定基址 0x2000000000
   │
   ├─ 装 seccomp BPF（按 syscall 号排序的二叉搜索树，最多 13 条指令）
   ├─ 装 SIGSYS handler（裸 svc 桩，全程不碰 libc）
   │
   │   被拦的 syscall 在 handler 里翻译：
   │     /etc/foo      →  rootfs/etc/foo
   │     127.0.0.1:53  →  DNS 代理端口（双向）
   │     /proc /sys /dev  →  原样透传宿主
   │
   ▼
手写 ELF loader（不调 ld.so）
   ├─ 扫 PT_LOAD，文件映射装载（ET_EXEC 原位 / ET_DYN 用 0x5000000000）
   ├─ 手工合成栈：argv → envp → auxv（16 字节对齐）
   └─ mov sp, ...; br entry
   │
   ▼
guest（musl 或 glibc）—— 用宿主内核跑
```

完整机制说明：[`docs/mechanism.md`](./docs/mechanism.md)

---

## 编译

需要 **NDK r28+**（r26b 有 TLS underaligned 链接 bug）。

```bash
./build/build.sh /path/to/ndk/28.2.13676358
# 产物 build/libdsh_alpine.so
```

校验：

```bash
$NDK/toolchains/llvm/prebuilt/*/bin/llvm-readelf -h build/libdsh_alpine.so
# 必须：Type: EXEC    Entry point address: 0x2000026...
```

### 六条硬约束

缺任何一条都会运行时崩，且**报错极不直观**（详见源码头部注释）：

| # | 约束 | 为什么 |
|---|---|---|
| 1 | freestanding 静态，**不链接 bionic libc** | handler 内调 libc 会 SEGV（guest 的 musl 接管了 TLS） |
| 2 | 固定基址 `0x2000000000`、`ET_EXEC`、无 PIE | BPF 白名单靠比对 `instruction_pointer` 识别自己的桩 |
| 3 | `-ffreestanding -fno-builtin` | 阻止编译器把字节循环优化成 `memcpy` 调用 |
| 4 | `-Wl,-z,max-page-size=0x20000` | Android 15+ 的 16 KB 页内核要求 |
| 5 | 关掉 fortify 与 stack-protector | 会引用 libc 符号导致链接失败 |
| 6 | 仅 aarch64 | 用 `register x0` 与 AArch64 ucontext 布局 |

### 改 BPF 表

```bash
python tools/gen_bpf_tree.py > tools/bpf_tree.inc      # 重新生成
python tools/verify_bpf_semantics.py                    # 必须 0 错误
# 然后把源码里的 f[] 数组整体替换为新内容（跳转偏移由脚本算好，不要手改）
```

---

## 目录

```
src/dsh_alpine.c              全部实现（2519 行，单文件 freestanding）
bench/bench_full.c            7 维基准程序（exec/io/stat/getpid/cpu/dir/write）
bench/bench_run.sh            guest 内跑基准的封装
tools/gen_bpf_tree.py         生成 seccomp BPF 二叉搜索树
tools/bpf_tree.inc            生成产物，被源码内嵌
tools/verify_bpf_semantics.py 模拟内核 cBPF 逐条执行，校验树语义
build/build.sh                编译脚本
docs/mechanism.md             实现机制
docs/pitfalls.md              26 个踩坑记录（症状 → 原因 → 修法）
docs/benchmark.md             测量纪律、原始数据、复现步骤
```

---

## 已知限制

| 限制 | 说明 |
|---|---|
| 仅 aarch64 | 实现是架构专属的，x86_64 编不过 |
| 需要 Android 10+ | 依赖 seccomp 与 `O_PATH` |
| 无 SysV IPC | `# CONFIG_SYSVIPC` 未编译的内核上跑不了 PostgreSQL / MariaDB 服务端（PRoot 与 tawcroot 同样做不到） |
| **不是安全沙箱** | 单进程设计，guest 里的恶意代码可以逃逸到宿主视角 |
| 后台常驻服务 | 容器内 `&` 不可靠，`nginx` / `redis` 需从宿主侧拉起 |
| 极小数据管道 | `echo x \| gzip \| zcat` 偶发失败（1/5~4/5），大文件往返正常 |

---

## 许可

**Proprietary. All rights reserved.** 完整条款见 [LICENSE](./LICENSE)（英文正本）
与 [LICENSE.zh-CN.md](./LICENSE.zh-CN.md)（中文参考译本）。

| | |
|---|---|
| **可以**（无需付费/无需签约） | 阅读研究 · issue/PR 中引用有限片段讨论 · 用未修改副本自行编译并个人非商业运行 · 运行官方构建 · 复现并发布基准数据（须注明来源与链接） |
| **不可以** | 复制/镜像/存储实质部分 · 修改改编（自用本地改动除外）· 分发/再分发/上架任何仓库 · 并入其他产品 · 作为服务向第三方提供 · **用作 AI 训练/评测数据** · 用于开发竞品或提取设计做独立重实现 · 去除声明 · 转让许可 |
| **须另行授权** | 一切**商业使用**（含企业内部生产性使用）；评估用途无需授权，但一旦投入商业使用即失效 |

**这不是开源项目**。仓库公开可读不等于放弃权利，也不等于授予开源许可 —— 详见
LICENSE 第 1 条（权利保留）与第 2 条（有限许可的封闭清单）。

**贡献**：提交 PR 即视为同意第 6 条 —— 版权转让 + 声明无第三方代码 +
不得引入任何 GPL/AGPL/LGPL 等互惠许可代码（详见 [CONTRIBUTING](./CONTRIBUTING.md)）。

**不含第三方代码**：本实现为从零手写，未使用 TAWC / PRoot / QEMU / LXC 的任何代码 ——
详见 [NOTICE](./NOTICE)。

**安全边界**：本软件**不是安全沙箱**（单进程设计，guest 可触及宿主级资源），
详见 LICENSE 第 11 条。

商业许可或超范围授权：<https://github.com/Soodok/Deepseek-Harness-Local-Android/issues>
