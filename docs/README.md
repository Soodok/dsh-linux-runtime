# 文档索引

| 文件 | 内容 |
|---|---|
| [mechanism.md](./mechanism.md) | 实现机制：seccomp、SIGSYS handler、自我重执行、ELF loader、DNS 改写、bionic 桥 |
| [pitfalls.md](./pitfalls.md) | 26 个踩坑记录，每条含「症状 → 原因 → 修法」 |
| [benchmark.md](./benchmark.md) | 测量纪律、基准代码、全部原始数据、复现步骤 |

## 给第一次读这个项目的人

1. 先读 README 的「工作原理（概要）」一节，看懂数据流
2. 再读 `mechanism.md` 的 1~4 节（seccomp / handler / re-exec / loader）
3. 动手改之前读 `mechanism.md` 的三条「铁律」与 `pitfalls.md` 的第 5、6、15 条
4. 要改性能就读 `benchmark.md` 开头的测量纪律 —— 这个项目踩过的最大坑
   不是实现，是**测错了方向**

## 给要加新能力的人

新增一个要被翻译的 syscall，按这个顺序：

```
tools/gen_bpf_tree.py 的 NR 表
  → python tools/gen_bpf_tree.py > tools/bpf_tree.inc
  → src/dsh_alpine.c 里整体替换 f[] 数组
  → src/dsh_alpine.c 的 sigsys_handler 加 case
  → python tools/verify_bpf_semantics.py（必须 0 错误）
  → ./build/build.sh（必须仍 ET_EXEC）
  → 真机验（别只看编译通过）
```

**最容易漏的一步**：`bpf_tree.inc` 生成后忘了替换源码里的数组，
于是新 case 写了但永远不会被 trap —— 表现是「代码明明有、就是不生效」。
