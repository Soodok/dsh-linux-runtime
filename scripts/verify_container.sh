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

echo "=== 4) dsh_gl 取像素三路径（rpix / ahb / auto）==="
# 这里同时验证「库这一层」——产线 framesrv 好不等于库好，容器内任意
# GL 程序都要能吃到 AHB 收益，才谈得上「图形栈补齐」。
for m in rpix ahb auto; do
  o=$($R $M $F $I /bin/sh -c "/dsh_gl_pixmode $T/gl_$m.bin $m 640 360 30" 2>&1)
  echo "$o" | grep -q DSH_GL_PIXMODE_OK || { echo "  FAIL mode=$m"; fail=$((fail+1)); continue; }
  pm=$(echo "$o" | sed -n 's/.*pixmode=\([a-z]*\) req=.*//p')
  fps=$(echo "$o" | sed -n 's/.*fps=\([0-9.]*\).*//p')
  echo "  OK  req=$m actual=$pm fps=$fps"
  # req=rpix 时实现必须是 rpix（否则说明退回逻辑写坏了，把不该走的走了）
  [ "$m" = "rpix" ] && [ "$pm" != "rpix" ] && { echo "  FAIL req=rpix but actual=$pm"; fail=$((fail+1)); }
done
# 三模式写出的像素必须逐字节一致 —— 否则「快」是拿画面对不上换来的
if [ -f "$T/gl_rpix.bin" ] && [ -f "$T/gl_ahb.bin" ]; then
  cmp -s "$T/gl_rpix.bin" "$T/gl_ahb.bin" && echo "  OK  rpix/ahb pixels IDENTICAL"     || { echo "  FAIL rpix/ahb pixels differ"; fail=$((fail+1)); }
fi

echo
[ $fail -eq 0 ] && echo "RESULT: VERIFY_CONTAINER_OK" || echo "RESULT: VERIFY_CONTAINER_FAIL ($fail)"
exit $fail
