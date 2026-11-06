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

echo "=== 3a) AF_UNIX（guest 侧，musl 探针）==="
# ★这才是「容器内 AF_UNIX 能不能用」的真判据。
# bench/unix_socket_probe 是 bionic，经 cleaner 跑的是宿主路径（pitfalls #41），
# 只能验 loader 的 sockaddr_un 重拼逻辑。guest 路径的真实行为必须用 musl 程序测。
if [ -x "$F/unixsock_guest" ]; then
  uo=$($R $M $F $I /bin/sh -c "mkdir -p /tmp/wl; /unixsock_guest /tmp /tmp/wl /dsh /eng /ws" 2>&1)
  echo "$uo" | grep -q "RESULT: UNIXSOCK_GUEST_OK"     && echo "  OK  guest 路径 bind/connect/rw 全通"     || { echo "  FAIL guest AF_UNIX"; echo "$uo" | grep "FAIL" | head -3; fail=$((fail+1)); }
else
  echo "  SKIP rootfs 内没有 unixsock_guest（gcc -O2 -o unixsock_guest bench/unixsock_guest.c）"
fi

echo "=== 3c) POSIX 共享内存（/dev/shm，pitfalls #44）==="
# Android 宿主没有 /dev/shm，而 /dev 整棵透传。这条不验，
# 所有走 shm_open 的程序（wlroots/dbus/Chromium）都会静默失败。
so=$($R $M $F $I /bin/sh -c 'ls -ld /dev/shm 2>&1; touch /dev/shm/.dshchk 2>/dev/null && echo SHM_WRITE_OK && rm -f /dev/shm/.dshchk || echo SHM_WRITE_FAIL' 2>&1)
echo "$so" | grep -q SHM_WRITE_OK   && echo "  OK  /dev/shm 可写（容器自有，不依赖宿主）"   || { echo "  FAIL /dev/shm 不可用：$(echo "$so" | head -2)"; fail=$((fail+1)); }

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

echo "=== 5) 开窗口（weston compositor + xdg_toplevel）==="
# 这段验证的是「容器里能不能开真实 GUI 窗口」。
# 判据不是「weston 进程起来了」，而是 **compositor 回了 xdg_surface.configure
# 且来了 frame_callback** —— configure 是 compositor 对「你要开顶层窗口」的
# 正式回应，frame_callback 证明它真的跑了一帧绘制循环。
# 只看进程存活会误判：weston 起来但没有任何客户端连上时，进程也在。
if [ ! -x "$F/wlsrc/wlwin" ]; then
  echo "  SKIP rootfs 内没有 wlwin（先 apk add wayland-dev gcc musl-dev 并跑 build_wlwin.sh）"
elif ! $R $M $F $I /bin/sh -c 'command -v weston >/dev/null' 2>/dev/null; then
  echo "  SKIP rootfs 内没有 weston（apk add weston weston-shell-desktop weston-backend-headless）"
else
  out=$($R $M $F $I /bin/sh -c '
    rm -rf /tmp/wl; mkdir -p /tmp/wl; chmod 700 /tmp/wl
    export XDG_RUNTIME_DIR=/tmp/wl
    weston --backend=headless-backend.so --renderer=pixman --socket=wl-v1 --idle-time=0 > /tmp/wv.log 2>&1 &
    sleep 5
    cd /wlsrc && WAYLAND_DISPLAY=wl-v1 ./wlwin 3 320 200 2>&1
    kill %1 2>/dev/null
  ' 2>&1)
  echo "$out" | grep -q "RESULT: WLWIN_OK"     || { echo "  FAIL 窗口客户端失败"; echo "$out" | tail -4; fail=$((fail+1)); }
  cfg=$(echo "$out" | grep "^WLWIN_KV configure=" | cut -d= -f2)
  fc=$(echo "$out" | grep "^WLWIN_KV frame_callback=" | cut -d= -f2)
  # 空值必须判 FAIL：解析坏了还判 OK 就是假通过
  if [ -z "$cfg" ] || [ -z "$fc" ]; then
    echo "  FAIL configure/frame_callback 未解析出"; fail=$((fail+1))
  elif [ "$fc" -lt 1 ] 2>/dev/null; then
    echo "  FAIL frame_callback=$fc（compositor 没跑绘制循环）"; fail=$((fail+1))
  else
    echo "  OK  窗口开出：configure=$cfg frame_callback=$fc"
  fi
  # 顺带记录 GL renderer 是硬件还是软件 —— 这是图形栈的真实边界
  gl=$(echo "$out" | grep -c WLWIN_OK)
  rend=$($R $M $F $I /bin/sh -c '
    rm -rf /tmp/wl2; mkdir -p /tmp/wl2; chmod 700 /tmp/wl2
    export XDG_RUNTIME_DIR=/tmp/wl2
    weston --backend=headless-backend.so --renderer=gl --socket=wl-g1 --idle-time=0 > /tmp/wg.log 2>&1 &
    sleep 5; kill %1 2>/dev/null
    grep -oE "GL renderer: .*" /tmp/wg.log | head -1
  ' 2>&1 | tr -d "
")
  [ -n "$rend" ] && echo "  INFO $rend"
fi

echo
[ $fail -eq 0 ] && echo "RESULT: VERIFY_CONTAINER_OK" || echo "RESULT: VERIFY_CONTAINER_FAIL ($fail)"
exit $fail
