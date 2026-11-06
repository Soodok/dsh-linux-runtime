#!/usr/bin/env bash
# dsh_alpine 编译脚本。
#
# 用法：./build.sh /path/to/ndk [输出路径] [架构]
#   架构：aarch64（默认）| x86_64
#
# 为什么参数这么硬：本程序的 BPF 白名单靠比对
# seccomp_data.instruction_pointer 识别自己写的裸 svc 桩，所以**地址必须每次
# 运行都相同** —— 因此必须 ET_EXEC + 固定基址 + 无 PIE。任何一条漏掉，
# 过滤器都会把自己 trap 掉，表现为启动即挂、无任何输出。
set -euo pipefail

NDK="${1:?用法: $0 <ndk根目录> [输出路径] [架构]}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
# 第三个参数是架构（允许把输出路径省略，只给架构）
ARCH="aarch64"
if [ "${3:-}" != "" ]; then
    ARCH="$3"
elif [ "${2:-}" = "x86_64" ] || [ "${2:-}" = "aarch64" ]; then
    ARCH="$2"
fi
# 第二个参数是输出路径；若它恰好等于架构名，则视为「省略了输出路径」
OUT_ARG="${2:-}"
if [ "$OUT_ARG" = "aarch64" ] || [ "$OUT_ARG" = "x86_64" ]; then
    [ -z "${3:-}" ] && ARCH="$OUT_ARG"
    OUT_ARG=""
fi
if [ "$ARCH" = "x86_64" ]; then
    OUT="${OUT_ARG:-$ROOT/build/libdsh_alpine-x86_64.so}"
    ENTRY_HINT="0x20000"
else
    OUT="${OUT_ARG:-$ROOT/build/libdsh_alpine.so}"
    ENTRY_HINT="0x2000026"
fi

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64 ;;
  Darwin)               HOST=darwin-x86_64 ;;
  *)                    HOST=linux-x86_64 ;;
esac

BIN="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
# Windows 的 NDK 里两份都在（裸名是真编译器，.cmd 是包装）；裸名优先，
# 因为从 bash 里直接调 .cmd 反而多一层 shell 解析、且某些 MSYS 版本会丢参数。
for c in "$ARCH-linux-android28-clang" "$ARCH-linux-android28-clang.cmd"; do
    if [ -x "$BIN/$c" ]; then CC="$BIN/$c"; break; fi
done
test -n "${CC:-}" || { echo "错误：$BIN 下找不到 $ARCH-linux-android28-clang" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"

# x86_64 必须加 -mcmodel=large：默认小代码模型下 --image-base=0x2000000000
# 会报 `relocation R_X86_64_32 out of range`（medium 也不行，只有 large 过）。
EXTRA=""
[ "$ARCH" = "x86_64" ] && EXTRA="-mcmodel=large"

# shellcheck disable=SC2086
"$CC" \
  -O1 -ffreestanding -fno-builtin -fno-pie -fno-PIE \
  -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector \
  -DPATH_MAX=4096 $EXTRA \
  -static -nostdlib -nostartfiles -nodefaultlibs -no-pie \
  -Wl,-e,_start \
  -Wl,--image-base=0x2000000000 \
  -Wl,-z,max-page-size=0x20000 \
  -Wl,-s \
  -o "$OUT" "$ROOT/src/dsh_alpine.c"

READELF="$(dirname "$CC")/llvm-readelf"
[ -x "$READELF.exe" ] && READELF="$READELF.exe"
[ -x "$READELF" ] || READELF="$(dirname "$CC")/llvm-readelf.exe"

echo "产物: $OUT ($(wc -c < "$OUT") 字节)  架构: $ARCH"
"$READELF" -h "$OUT" | grep -E 'Type|Entry point' || true
echo ""
echo "预期：Type: EXEC、Entry point address: $ENTRY_HINT..."
