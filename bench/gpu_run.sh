#!/usr/bin/env bash
# 在设备上跑容器内 GPU 探针，并把结果拉回来。
#
# 用法：./gpu_run.sh <adb设备> [包名]
# 前置：build_probes.sh 已编译；dsh_alpine 已部署到 linux/runner/
#
# 为什么必须经 `sh -c`：dsh_alpine 对「guest 本身是 bionic ELF」的直调会先按
# musl 装载，报 `__libc_init: symbol not found`。只有容器内再发起一次 execve
# （由 sh 发起）才命中 bionic 直通分支、转交 filter-free 的 cleaner 代跑。
#
# 为什么模拟器上要用 run-as：App 进程由 zygote fork，自带一层 app policy
# seccomp filter（Seccomp=2），我们的 TRAP 压不过它的 KILL。run-as 起的进程
# Seccomp=0，容器才能跑。详见 docs/pitfalls.md 第 31 条。
set -euo pipefail
DEV="${1:?用法: $0 <adb设备> [包名]}"
PKG="${2:-app.dsh.mobile}"
ADB="${ADB:-adb}"
ARCH="${ARCH:-x86_64}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/out/$ARCH"
R="/data/data/$PKG/files/linux"
D="/data/data/$PKG/files"

[ -d "$SRC" ] || { echo "未找到 $SRC，先跑 ./build_probes.sh" >&2; exit 1; }

for p in gpu_probe frame_pump ipc_probe window_probe; do
  "$ADB" -s "$DEV" push "$(cygpath -w "$SRC/$p" 2>/dev/null || echo "$SRC/$p")" "/data/local/tmp/$p" >/dev/null
  "$ADB" -s "$DEV" shell "cp /data/local/tmp/$p $R/distros/alpine/$p && chmod 755 $R/distros/alpine/$p"
done

run() { "$ADB" -s "$DEV" shell "run-as $PKG $R/runner/dsh_alpine $R/distros/alpine /lib/ld-musl-x86_64.so.1 /bin/sh -c '$1'"; }

echo "===== 1. GPU 可用性 + 1080p 基准 ====="
run "/gpu_probe 60" || true
echo
echo "===== 2. 帧泵成本分解（720p / 1080p）====="
run "/frame_pump /tmp/frame.raw 60" || true
echo
echo "===== 3. 共享内存通路 ====="
run "/ipc_probe 60" || true
echo
echo "===== 4. 端到端窗口路径 ====="
run "/window_probe 30 $D/window.ppm" || true
# adb 是原生 Windows 程序，MSYS 路径要转成 Windows 形式
_dst="$HERE/_window.ppm"
command -v cygpath >/dev/null 2>&1 && _dst="$(cygpath -w "$HERE/_window.ppm")"
"$ADB" -s "$DEV" pull "$D/window.ppm" "$_dst" 2>/dev/null && echo "帧已拉回 bench/_window.ppm"
