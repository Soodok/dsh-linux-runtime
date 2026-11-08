#!/system/bin/sh
# 一键重建 root 域容器环境（模拟器/真机 adb root 后可用）。
#
# 为什么需要它：`/data/local/tmp/{rf,ax}` 属于临时目录，模拟器被清盘、
# App 重装、adb root 之后经常整片消失（实测丢过一次 rootfs 一次 runner）。
# 每次丢了手工敲十几条命令既慢又容易漏步骤，所以把整条链路固化在这里。
#
# 前置：
#   - `adb root` 已生效（adbd 跑在 u:r:su:s0）
#   - 本机已有 `dsh-linux-runtime/build/libdsh_alpine-x86_64.so`
#   - 本机已有 `dsh-android/app/src/main/assets/linux/rootfs-bootstrap-x86_64.tgz`
#
# 用法：
#   ADB=<adb路径> bash scripts/setup_rootenv.sh <宿主资产目录> [设备目录]
#   例：ADB=/d/AndroidWatch_ADB_ToolBox/adb/adb.exe bash scripts/setup_rootenv.sh E:/tmp/rootenv
#
# ⚠️ 三个已踩实的坑（本脚本据此设计，勿"优化"掉）：
#  1. **不要用容器内的 tar 解包** —— 解包前 rootfs 是空的，`/lib/ld-musl-*`
#     还不存在，容器根本起不来（报 `LOADER: preload FAIL`）。必须宿主侧解。
#  2. **不要用容器内的 readlink 改软链** —— 同理，PATH 继承宿主 `/system/bin`，
#     容器内 `readlink`/`cat`/`uname` 全 not found。改软链在宿主侧做（toybox 齐全）。
#  3. **adb push 的源路径必须是 Windows 风格** —— adb 是原生 Windows 程序，
#     传 `/e/tmp/...` 会 `cannot stat`（MSYS 路径不在它的字典里）。
set -u
SRC=${1:?usage: setup_rootenv.sh <host-assets-dir> <device-tmp-dir>}
DST=${2:-/data/local/tmp}
ADB=${ADB:-adb}

# adb 是 Windows 原生程序，源路径必须转成 Windows 风格
winpath() {
  case "$1" in
    /?/*) printf '%s:/%s' "$(echo "$1" | cut -c2)" "$(echo "$1" | cut -c4-)" ;;
    *)    printf '%s' "$1" ;;
  esac
}

RF=$DST/rf
AX=$DST/ax
S=$(winpath "$SRC")

echo "=== 0) 清场 ==="
$ADB shell "pkill -f 'dsh_alpine.*$RF' 2>/dev/null; rm -rf $RF; mkdir -p $RF" || exit 1

echo "=== 1) 推资产 ==="
$ADB push "$S/_rf.tgz" "$DST/_rf.tgz" || exit 1
$ADB push "$S/ax" "$DST/ax" || exit 1
$ADB shell "chmod 755 $AX"

echo "=== 2) 宿主侧解包（容器内 tar 在这里还不可用）==="
$ADB shell "cd $RF && tar -xzf $DST/_rf.tgz && echo UNPACK-OK"
$ADB shell "ls -l $RF/lib/ld-musl-x86_64.so.1"

echo "=== 3) 宿主侧软链相对化（304 条左右）==="
# Alpine 官方 rootfs 的 applet 是 `cat -> /bin/busybox` 绝对链接，
# 而软链内容不经路径翻译（pitfalls #30），容器里会指向宿主根 → 全废。
# 只改真软链（[ -L ]），绝不碰 busybox 本体 —— 碰了就是把 busybox
# 改成指向自己的死链，症状与"没改"完全一样（这个坑踩过第二次）。
$ADB shell "n=0
for d in bin sbin usr/bin usr/sbin; do
  [ -d $RF/\$d ] || continue
  for f in $RF/\$d/*; do
    [ -L \"\$f\" ] || continue
    t=\$(readlink \"\$f\")
    case \"\$t\" in
      /bin/busybox|*/bin/busybox)
        case \$d in
          usr/bin|usr/sbin) ln -sf ../../bin/busybox \"\$f\";;
          sbin) ln -sf ../bin/busybox \"\$f\";;
          *) ln -sf busybox \"\$f\";;
        esac
        n=\$((n+1));;
    esac
  done
done
echo RELINKED=\$n"

echo "=== 4) 容器自检 ==="
$ADB shell "$AX $RF /lib/ld-musl-x86_64.so.1 /bin/sh -c '
cat /etc/alpine-release; uname -m
echo SETUP-OK'"

echo "=== 完成：X=$AX  R=$RF ==="
