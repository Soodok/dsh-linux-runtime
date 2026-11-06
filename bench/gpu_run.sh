#!/usr/bin/env bash
# 在容器内验证硬件 GPU 通路，并量化帧回读成本。
#
# 前置：dsh_alpine 已部署到设备的 linux/runner/，rootfs 为 Alpine。
# 用法：./gpu_run.sh <adb-设备> [包名]
#
# 为什么必须经 `sh -c`：dsh_alpine 对「guest 本身是 bionic ELF」的直调会先走
# musl interp，报 `__libc_init: symbol not found`。只有容器内发起一次 execve
# （由 sh 发起）才会命中 bionic 直通分支、转交 filter-free 的 cleaner 代跑。
set -euo pipefail
DEV="${1:?用法: $0 <adb设备> [包名]}"
PKG="${2:-app.dsh.mobile}"
ADB="${ADB:-adb}"
R="/data/data/$PKG/files/linux"

run() { $ADB -s "$DEV" shell "run-as $PKG $R/runner/dsh_alpine $R/distros/alpine /lib/ld-musl-x86_64.so.1 /bin/sh -c '$1'"; }

$ADB -s "$DEV" shell "cp /data/local/tmp/gpu_probe $R/distros/alpine/gpu_probe 2>/dev/null; chmod 755 $R/distros/alpine/gpu_probe" || true
echo "===== 1. GPU 可用性 ====="
run "/gpu_probe 60" || true
echo
echo "===== 2. 帧泵成本 ====="
run "/frame_pump /tmp/frame.raw 60" || true
