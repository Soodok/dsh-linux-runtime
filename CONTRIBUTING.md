# 贡献须知

本仓库是**专有软件**。提交贡献前请确认你接受以下条款（详见 [LICENSE](./LICENSE)）：

## 1. 版权转让

提交 patch / PR 即表示你同意将贡献的**全部版权**转让给版权持有人，
并确认你有权这么做（例如：不是从雇主处带出的代码、不是从别处复制）。

## 2. 禁止 GPL 传染

本仓库是专有许可。**任何 GPL / AGPL / LGPL 代码都不得提交**，
包括但不限于：

- TAWC / tawcroot（GPL-3.0）
- PRoot（GPL-2.0）
- QEMU（GPL-2.0）
- 任何标注 "or later" 的双许可项目（除非明确选择非 GPL 的那一侧）

一旦引入，整个 `src/dsh_alpine.c` 会被 GPL-3.0 覆盖，专有条款失效。
提交前请注明代码来源；来自文档而非源码的可以参考，但要在 PR 里说明。

## 3. 改动运行器必须注意

`src/dsh_alpine.c` 有六条硬约束（见 README），任何一条被破坏都会导致
运行时崩溃且**报错极不直观**。改完请：

```bash
./build/build.sh "$NDK"
$NDK/toolchains/llvm/prebuilt/*/bin/llvm-readelf -h build/libdsh_alpine.so
# 必须仍是 Type: EXEC，Entry point 0x2000026...
```

## 4. 改 BPF 表必须重新生成

`tools/bpf_tree.inc` 是 `tools/gen_bpf_tree.py` 的产物，内嵌在源码里。
新增要拦的 syscall 时：

1. 改 `tools/gen_bpf_tree.py` 的 `NR` 表
2. `python tools/gen_bpf_tree.py > tools/bpf_tree.inc`
3. 把源码里 `f[]` 数组整体替换为新内容（注意 `LOADER_BASE_HI` 的跳转偏移
   由脚本算好，不要手改）
4. `python tools/verify_bpf_semantics.py` 必须报 0 错误

## 5. 提交信息

中文或英文均可，但请写**为什么**而非只写「改了什么」。这个项目的每个改动
背后通常都有一个真机踩坑，那条信息比代码本身更有价值。
