#!/bin/sh
# 在容器内编译 qscreensrv（QEMU 持续帧服务）
# 用法：sh /build_qscreensrv.sh    （源码需已在 /tmp/qscreensrv.c）
set -e
cd /tmp
gcc -O2 -o qscreensrv qscreensrv.c
echo "built: $(ls -l qscreensrv | awk '{print $5}') bytes"
