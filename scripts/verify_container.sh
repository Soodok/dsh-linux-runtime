#!/system/bin/sh
# verify_container.sh —— 在 root 域一次性验证容器的三条路径语义 + AF_UNIX。
#
# 用法：adb push 到设备后
#   adb shell "sh /data/local/tmp/verify_container.sh /data/local/tmp/dshtest"
#
# 前置：$T/dsh_alpine、$T/rootfs（已解包且绝对软链已改相对）、$T/dsh、$T/eng、$T/home/w
set -u
T="${1:-/data/local/tmp/dshtest}"
R="$T/dsh_alpine"
F="$T/rootfs"
I="/lib/ld-musl-x86_64.so.1"
M="-m /dsh:$T/dsh -m /eng:$T/eng -m /ws:$T/home/w"
fail=0

echo "=== 1) rootfs 基本可用性 ==="
out=$($R $M $F $I /bin/sh -c 'cat /etc/alpine-release' 2>&1)
case "$out" in
  3.*) echo "  OK  alpine-release=$out" ;;
  *)   echo "  FAIL $out"; fail=$((fail+1)) ;;
esac

echo "=== 2) 路径语义（realpath / inotify / getcwd）==="
$R $M $F $I /bin/sh -c '/path_probe' 2>&1 | tail -3
$R $M $F $I /bin/sh -c '/path_probe' 2>&1 | grep -q PATH_PROBE_OK || fail=$((fail+1))

echo "=== 3) AF_UNIX bind/connect ==="
$R $M $F $I /bin/sh -c 'mkdir -p /tmp/wl; /unix_socket_probe /tmp /dsh /eng /ws /tmp/wl' 2>&1 | tail -3
$R $M $F $I /bin/sh -c 'mkdir -p /tmp/wl; /unix_socket_probe /tmp /dsh /eng /ws /tmp/wl' 2>&1 | grep -q UNIX_SOCKET_OK || fail=$((fail+1))

echo
[ $fail -eq 0 ] && echo "RESULT: VERIFY_CONTAINER_OK" || echo "RESULT: VERIFY_CONTAINER_FAIL ($fail)"
exit $fail
