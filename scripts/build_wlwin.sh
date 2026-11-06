#!/bin/sh
# build_wlwin.sh —— 在 **容器内** 编译 Wayland 窗口探针
#
# 为什么必须在容器内编：NDK 只有 bionic 工具链，而 wayland-client 是 musl 库。
# 这个探针要验证的是「容器 guest 能不能开窗口」，所以必须是 musl 程序。
#
# 前置（容器内执行一次）：
#   apk add --no-interactive wayland-dev wayland-protocols gcc musl-dev
#
# 用法（把 bench/wlwin.c 放进 rootfs 后）：
#   /bin/sh /build_wlwin.sh /wlsrc
set -e
SRC="${1:-/wlsrc}"
cd "$SRC"

SCANNER=/usr/bin/wayland-scanner
[ -x "$SCANNER" ] || { echo "缺少 wayland-scanner（apk add wayland-dev）" >&2; exit 1; }

# xdg-shell：建顶层窗口必需
PROTO=/usr/share/wayland-protocols
$SCANNER client-header "$PROTO/stable/xdg-shell/xdg-shell.xml" xdg-shell-client-protocol.h
$SCANNER private-code  "$PROTO/stable/xdg-shell/xdg-shell.xml" xdg-shell-protocol.c

# presentation-time：拿「这一帧真的送显了」的权威事件
$SCANNER client-header "$PROTO/stable/presentation-time/presentation-time.xml" \
    presentation-time-client-protocol.h
$SCANNER private-code  "$PROTO/stable/presentation-time/presentation-time.xml" \
    presentation-time-protocol.c

gcc -O2 -o wlwin wlwin.c xdg-shell-protocol.c presentation-time-protocol.c -lwayland-client
echo "built: $SRC/wlwin $(stat -c%s wlwin) B"
