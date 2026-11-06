#!/usr/bin/env bash
# 编译三个 bionic 侧探针（gpu_probe / frame_pump / ipc_probe / window_probe）。
#
# 用法：./build_probes.sh <ndk根目录> [架构]
#   架构：x86_64 | aarch64（默认两个都编）
#
# 产物落在 ./out/<arch>/ 下，用 push.sh 推到设备。
set -euo pipefail
NDK="${1:?用法: $0 <ndk根目录> [x86_64|aarch64]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64 ;;
  Darwin)               HOST=darwin-x86_64 ;;
  *)                    HOST=linux-x86_64 ;;
esac
BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
ARCHS="${2:-x86_64 aarch64}"
for ARCH in $ARCHS; do
  CC="$BIN/$ARCH-linux-android28-clang"
  [ -x "$CC" ] || { echo "缺少编译器 $CC" >&2; exit 1; }
  OUT="$HERE/out/$ARCH"
  mkdir -p "$OUT"
  for src in gpu_probe frame_pump ipc_probe window_probe framesrv; do
    # window_probe / ipc_probe 用裸 syscall 建 memfd，不需要额外库；
    # gpu_probe / frame_pump 走 dlopen，只链 libdl。
    "$CC" -O2 -o "$OUT/$src" "$HERE/$src.c" -ldl
    echo "  $ARCH/$src  $(stat -c%s "$OUT/$src") B"
  done
  # dsh_gl 库探针：验证容器内**任意** GL 程序都能吃到 AHB 收益
  # （framesrv 好不等于库好，这块是给第三方程序用的）
  "$CC" -O2 -fPIC -o "$OUT/dsh_gl_pixmode"     "$HERE/../lib/pixmode_probe.c" "$HERE/../lib/dsh_gl.c" -ldl
  echo "  $ARCH/dsh_gl_pixmode  $(stat -c%s "$OUT/dsh_gl_pixmode") B"
done
# wlwin（Wayland 窗口客户端）是 **musl** 程序，必须在容器内用 gcc 编译，
# 不能用 NDK 交叉编译（NDK 只有 bionic）。编译脚本见 scripts/build_wlwin.sh。
echo ""
echo "注意：以下三个需在容器内编译（musl），不在本脚本的 NDK 产物里："
echo "  bench/wlwin.c  → scripts/build_wlwin.sh   （开窗口，xdg-shell）"
echo "  bench/wlshot.c → scripts/build_wlshot.sh  （抓帧，wlr-screencopy）"
echo "  bench/unixsock_guest.c → 容器内 gcc -O2 -o unixsock_guest unixsock_guest.c"
