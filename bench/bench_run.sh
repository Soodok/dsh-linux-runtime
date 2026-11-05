#!/bin/sh
# bench_run.sh —— 在 guest 内跑基准并输出 key=value
# 由宿主侧调用：dsh-alpine <rootfs> ld-musl busybox sh /tmp/bench_run.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cd /tmp || exit 1

if [ ! -x /tmp/bench_full ]; then
  echo "BUILD_FAIL"
  exit 1
fi

echo "---- bench start ----"
/tmp/bench_full "${1:-300}"
echo "---- bench end ----"
