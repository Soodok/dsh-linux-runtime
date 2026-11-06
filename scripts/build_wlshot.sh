#!/bin/sh
# build_wlshot.sh —— 在 **容器内** 编译 wlr-screencopy 抓帧探针
#
# 为什么必须在容器内编：wlr-screencopy 是 wlroots 的 Wayland 协议，
# 客户端库是 musl 的 libwayland-client；NDK 只有 bionic 工具链。
#
# 前置（容器内执行一次）：
#   apk add --no-interactive wlr-protocols wayland-dev gcc musl-dev
#   apk add --no-interactive sway grim          # 抓帧需要有 wlroots 系 compositor
#
# 用法（把 bench/wlshot.c 放进 rootfs 后）：
#   /bin/sh /build_wlshot.sh /wlsrc
set -e
SRC="${1:-/wlsrc}"
cd "$SRC"

SCANNER=/usr/bin/wayland-scanner
[ -x "$SCANNER" ] || { echo "缺少 wayland-scanner（apk add wayland-dev）" >&2; exit 1; }

PROTO=/usr/share/wlr-protocols/unstable/wlr-screencopy-unstable-v1.xml
[ -f "$PROTO" ] || { echo "缺少 $PROTO（apk add wlr-protocols）" >&2; exit 1; }

$SCANNER client-header "$PROTO" wlr-screencopy-client-protocol.h
$SCANNER private-code  "$PROTO" wlr-screencopy-protocol.c

gcc -O2 -o wlshot wlshot.c wlr-screencopy-protocol.c -lwayland-client
echo "built: $SRC/wlshot $(stat -c%s wlshot) B"
