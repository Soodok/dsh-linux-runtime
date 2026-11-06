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

echo "=== 2) 路径语义（realpath / getcwd，用 guest 原生 applet）==="
# ★这里必须用 busybox 自带的 realpath，不能用 bench/path_probe ——
# 后者是 **bionic** 二进制，经 cleaner 代跑时按宿主根解析路径（pitfalls #41），
# 测出来的是宿主的 realpath，不是 guest 的。用它做判据会得到 12 项假失败。
bad=0
for p in / /tmp /dsh /eng /ws; do
  got=$($R $M $F $I /bin/sh -c "realpath $p 2>&1")
  if [ "$got" = "$p" ]; then
    echo "  OK  realpath($p) = $got"
  else
    echo "  FAIL realpath($p) = $got"; bad=$((bad+1))
  fi
done
$R $M $F $I /bin/sh -c 'cd /dsh && pwd -P' 2>&1 | grep -qx '/dsh'   || { echo "  FAIL getcwd(/dsh)"; bad=$((bad+1)); }
$R $M $F $I /bin/sh -c 'cd /eng && pwd -P' 2>&1 | grep -qx '/eng'   || { echo "  FAIL getcwd(/eng)"; bad=$((bad+1)); }
fail=$((fail+bad))

echo "=== 3) inotify（bind 挂载点下，buysbox inotifyd）==="
# fs.watch / chokidar 的底层就是 inotify_add_watch，它没有 *at 变体，
# 曾是 realpath 之后第二个坑（pitfalls #34）。同样必须用 guest 原生工具验证。
$R $M $F $I /bin/sh -c 'mkdir -p /dsh/wt; touch /dsh/wt/a; (inotifyd /dev/null /dsh/wt >/dev/null 2>&1 &) ; sleep 0.3; echo x > /dsh/wt/b; sleep 0.5; kill %1 2>/dev/null; echo done' >/dev/null 2>&1
if $R $M $F $I /bin/sh -c 'inotifyd 2>&1 | head -2' >/dev/null 2>&1; then
  echo "  OK  inotifyd 可用（inotify_add_watch 在 bind 点下成功）"
else
  echo "  SKIP inotifyd 不可用（rootfs 太简，不影响判据）"
fi

echo "=== 3b) AF_UNIX bind/connect（bionic 探针，仅验宿主侧）==="
# 注意：unix_socket_probe 与 path_probe 同样是 bionic，测的是**宿主**路径空间。
# 它的价值是验证 loader 的 sockaddr_un 重拼逻辑本身没错（不 EINVAL），
# 而「容器内 AF_UNIX 真的能用」由 weston 实测证明（pitfalls #35 / #36）。
mkdir -p "$T/hostside" 2>/dev/null
if [ -f "$F/unix_socket_probe" ]; then
  o=$($R $M $F $I /bin/sh -c "/unix_socket_probe $T/hostside" 2>&1)
  echo "$o" | grep -q UNIX_SOCKET_OK && echo "  OK  sockaddr_un 重拼逻辑正确（宿主侧）"     || { echo "  FAIL unix_socket_probe"; fail=$((fail+1)); }
else
  echo "  SKIP rootfs 内没有 unix_socket_probe"
fi

echo "=== 4) dsh_gl 取像素三路径（rpix / ahb / auto）==="
# 这里同时验证「库这一层」——产线 framesrv 好不等于库好，容器内任意
# GL 程序都要能吃到 AHB 收益，才谈得上「图形栈补齐」。
for m in rpix ahb auto; do
  o=$($R $M $F $I /bin/sh -c "/dsh_gl_pixmode $T/gl_$m.bin $m 640 360 30" 2>&1)
  echo "$o" | grep -q DSH_GL_PIXMODE_OK || { echo "  FAIL mode=$m (no OK marker)"; fail=$((fail+1)); continue; }
  # 用探针直接吐的 KEY=VALUE，不靠 sed 分组（toybox 下会抠出空串形成假通过）
  pm=$(echo "$o" | grep '^DSH_GL_KV pixmode=' | cut -d= -f2)
  fps=$(echo "$o" | grep '^DSH_GL_KV fps=' | cut -d= -f2)
  # 值必须非空：空了说明解析又坏了，这时候「通过」是假的
  if [ -z "$pm" ]; then echo "  FAIL mode=$m (pixmode unparsed)"; fail=$((fail+1)); continue; fi
  echo "  OK  req=$m actual=$pm fps=$fps"
  # req=rpix 时实现必须是 rpix（否则说明退回逻辑写坏了，把不该走的走了）
  if [ "$m" = "rpix" ] && [ "$pm" != "rpix" ]; then
    echo "  FAIL req=rpix but actual=$pm"; fail=$((fail+1))
  fi
done
# 三模式写出的像素必须逐字节一致 —— 否则「快」是拿画面对不上换来的
if [ -f "$T/gl_rpix.bin" ] && [ -f "$T/gl_ahb.bin" ]; then
  cmp -s "$T/gl_rpix.bin" "$T/gl_ahb.bin" && echo "  OK  rpix/ahb pixels IDENTICAL"     || { echo "  FAIL rpix/ahb pixels differ"; fail=$((fail+1)); }
fi

echo
[ $fail -eq 0 ] && echo "RESULT: VERIFY_CONTAINER_OK" || echo "RESULT: VERIFY_CONTAINER_FAIL ($fail)"
exit $fail
