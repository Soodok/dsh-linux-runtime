#!/usr/bin/env bash
# dsh_alpine 编译脚本。
#
# 用法：./build.sh /path/to/ndk [输出路径]
#
# 为什么参数这么硬：本程序的 BPF 白名单靠比对
# seccomp_data.instruction_pointer 识别自己写的裸 svc 桩，所以**地址必须每次
# 运行都相同** —— 因此必须 ET_EXEC + 固定基址 + 无 PIE。任何一条漏掉，
# 过滤器都会把自己 trap 掉，表现为启动即挂、无任何输出。
set -euo pipefail

NDK="${1:?用法: $0 <ndk根目录> [输出路径]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
OUT="${2:-$ROOT/build/libdsh_alpine.so}"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64 ;;
  Darwin)               HOST=darwin-x86_64 ;;
  *)                    HOST=linux-x86_64 ;;
esac

BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
# Windows 的 NDK 里两份都在（裸名是真编译器，.cmd 是包装）；裸名优先，
# 因为从 bash 里直接调 .cmd 反而多一层 shell 解析、且某些 MSYS 版本会丢参数。
for c in aarch64-linux-android28-clang aarch64-linux-android28-clang.cmd; do
    if [ -x "$BIN/$c" ]; then CC="$BIN/$c"; break; fi
done
test -n "${CC:-}" || { echo "错误：$BIN 下找不到 aarch64-linux-android28-clang" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"

"$CC" \
  -O1 -ffreestanding -fno-builtin -fno-pie -fno-PIE \
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector \
  -DPATH_MAX=4096 \
  -static -nostdlib -nostartfiles -nodefaultlibs -no-pie \
  -Wl,-e,_start \
  -Wl,--image-base=0x2000000000 \
  -Wl,-z,max-page-size=0x20000 \
  -Wl,-s \
  -o "$OUT" "$ROOT/src/dsh_alpine.c"

READELF="$(dirname "$CC")/llvm-readelf"
[ -x "$READELF.exe" ] && READELF="$READELF.exe"
[ -x "$READELF" ] || READELF="$(dirname "$CC")/llvm-readelf.exe"

echo "产物: $OUT ($(wc -c < "$OUT") 字节)"
"$READELF" -h "$OUT" | grep -E 'Type|Entry point' || true
echo ""
echo "预期：Type: EXEC、Entry point address: 0x2000026..."
