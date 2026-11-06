#!/bin/sh
# build_wlshot.sh —— 在 **容器内** 编译 wlr-screencopy 抓帧器
#
# 为什么必须在容器内编：NDK 只有 bionic，而 wayland-client 是 musl 库。
# 这个程序要在 guest 里跑，抓 guest 的 compositor 画面。
#
# 前置（容器内执行一次）：
#   apk add --no-interactive sway wlr-protocols wayland-dev wayland-protocols gcc musl-dev
#
# 用法（把 bench/wlshot.c 放进 rootfs 的 /wlsrc 后）：
#   /bin/sh /build_wlshot.sh /wlsrc
set -e
SRC="${1:-/wlsrc}"
cd "$SRC"

SCANNER=/usr/bin/wayland-scanner
[ -x "$SCANNER" ] || { echo "缺少 wayland-scanner（apk add wayland-dev）" >&2; exit 1; }
XML=/usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
[ -f "$XML" ] || { echo "缺少 $XML（apk add wlr-protocols）" >&2; exit 1; }

$SCANNER client-header "$XML" wlr-screencopy-client-protocol.h
$SCANNER private-code  "$XML" wlr-screencopy-protocol.c

gcc -O2 -Wall -o wlshot wlshot.c wlr-screencopy-protocol.c -lwayland-client
echo "built: $SRC/wlshot $(stat -c%s wlshot) B"
