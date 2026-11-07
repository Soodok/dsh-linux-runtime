#!/system/bin/sh
# 在 Android 手机上用 QEMU 跑起一台有**真实画面输出**的 x86_64 虚拟机。
#
# 为什么需要这套东西：
#   手机上没有 /dev/kvm、cpuinfo 里没有 vmx，QEMU 只能走 TCG 纯软件模拟。
#   更麻烦的是 guest 拿不到 /dev/fb0 —— 内核里 CONFIG_DRM_SIMPLEDRM=y 只吃
#   EFI GOP，而 SeaBIOS 不提供 EFI。唯一可行路径是 insmod bochs DRM 模块。
#
# 内核事实（Alpine linux-lts 的 /boot/config-6.12.115-0-lts）：
#   CONFIG_DRM=y                       内置
#   CONFIG_DRM_KMS_HELPER=y            内置
#   CONFIG_DRM_BOCHS=m                 模块 → 必须 insmod
#   CONFIG_DRM_SIMPLEDRM=y             内置，但 SeaBIOS 下绑不上
#   CONFIG_DRM_TTM=m / DRM_TTM_HELPER=m / DRM_VRAM_HELPER=m
#   CONFIG_FRAMEBUFFER_CONSOLE=y
#
# 模块加载顺序绝不能乱（错了就是 "Unknown symbol drm_gem_vram_*"）：
#   ttm → drm_ttm_helper → drm_vram_helper → bochs
#
# 用法：
#   sh qemu_boot.sh [rootfs] [runner] [serial|screen]
#     serial  只出串口日志（快，用于验证 guest 起没起）
#     screen  额外把真实屏幕 dump 成 PNG（慢，需等 45s 让 fbcon 切模式）
set -u

R="${1:-/data/local/tmp/rf}"          # rootfs（x86_64 minirootfs）
X="${2:-/data/local/tmp/ax}"          # dsh_alpine runner
MODE="${3:-serial}"
I=/lib/ld-musl-x86_64.so.1
Q="$X $R $I /usr/bin/qemu-system-x86_64"
B="$X $R $I /bin/busybox"
KVER=6.12.115-0-lts
MOD="$R/lib/modules/$KVER"
IR="$R/tmp/irq"

# ★路径规则（本项目踩过三次）：
#   shell 的 `>` 重定向由**宿主**执行 → 必须写宿主路径 $R/tmp/...
#   传给容器程序的参数由**容器**解析 → 必须写容器路径 /tmp/...
#   搞反的后果是「文件静默 0 字节」或 `can't create`，而命令本身返回 0。
#   QEMU 的 -serial file: / -monitor tcp 里给的文件名都按**容器**解析。

echo "=== 1) 精确清场（不要用 pkill -f qemu-system）==="
# pkill -f qemu-system 会连带杀掉「命令行里含 qemu-system 的脚本自身」，
# 实测把整个 .sh 一起杀了，表现为「脚本无输出、qemu 也没起来」。
for p in $(ps -A -o PID,NAME 2>/dev/null | grep -w "qemu-system-x86_64" | awk '{print $1}'); do
  kill -9 "$p" 2>/dev/null
done
sleep 2

echo "=== 2) 建 initrd 目录 ==="
rm -rf "$IR"
mkdir -p "$IR"/bin "$IR"/proc "$IR"/sys "$IR"/dev "$IR"/tmp "$IR"/etc
cp "$R/bin/busybox.static" "$IR/bin/busybox"
chmod 755 "$IR/bin/busybox"

echo "=== 3) 解压 4 个必需 .ko ==="
for m in ttm drm_ttm_helper drm_vram_helper bochs; do
  src=$(find "$MOD" -name "$m.ko.gz" 2>/dev/null | head -1)
  if [ -z "$src" ]; then echo "  MISS $m.ko.gz"; continue; fi
  rel=${src#"$R"}                    # 容器路径
  dst="$IR${rel%.gz}"                # 宿主目标路径
  mkdir -p "$(dirname "$dst")"
  # gzip 输入用容器路径，输出重定向用宿主路径
  $B gzip -d -c "$rel" > "$dst" 2>/dev/null
  echo "  $m $(wc -c < "$dst") bytes"
done

echo "=== 4) 写 init ==="
cat > "$IR/init" <<'EOS'
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
echo "=== GUEST BOOT ==="
for m in ttm drm_ttm_helper drm_vram_helper bochs; do
  p=$(find /lib/modules -name "$m.ko" 2>/dev/null | head -1)
  [ -z "$p" ] && { echo "MISS $m"; continue; }
  if insmod "$p" 2>/dev/null; then echo "OK   $m"; else echo "FAIL $m"; fi
done
echo "--- /proc/fb ---"; cat /proc/fb 2>/dev/null || echo NO_PROC_FB
echo "--- /dev/fb ---";  ls -l /dev/fb* 2>/dev/null || echo NO_DEV_FB
echo "=== DIAG END ==="
exec /bin/sh
EOS
chmod 755 "$IR/init"

echo "=== 5) 打包 initrd ==="
# cd 用容器路径，> 用宿主路径
$B sh -c 'cd /tmp/irq && find . -print | busybox cpio -o -H newc 2>/dev/null > /tmp/q.cpio'
$B gzip -9 -c /tmp/q.cpio > "$R/tmp/q.gz" 2>/dev/null
echo "  initrd: $(wc -c < "$R/tmp/q.gz") bytes"

cd "$R/tmp" || exit 1

if [ "$MODE" = "screen" ]; then
  # ---- 取屏幕模式：pmemsave 读 guest 物理 framebuffer ----
  # bochs DRM 把它映射在 0xfd000000（内核日志：「Framebuffer size 16384 kB @ 0xfd000000」）
  #
  # 两个坑：
  #  1) -monitor stdio + stdin 重定向在 setsid 后台进程里不生效，
  #     pmemsave 根本不执行。→ 用 TCP monitor。
  #  2) HMP 把 `/tmp/x` 的 `/` 当除号，报 invalid char。→ 用相对路径，
  #     靠 QEMU 进程 cwd 定位（cwd = 容器 /tmp = 宿主 $R/tmp）。
  rm -f "$R/tmp/fb.raw"
  PORT=4448
  setsid $Q -m 768 -kernel /boot/vmlinuz-lts -initrd /tmp/q.gz \
    -append "console=tty0 rdinit=/init" \
    -vga std -display none \
    -monitor "tcp:127.0.0.1:$PORT,server,nowait" \
    -accel tcg -L /usr/share/qemu \
    > "$R/tmp/qscreen.log" 2>&1 &
  n=0
  while [ $n -lt 20 ]; do
    sleep 2
    $B netstat -ltn 2>/dev/null | grep -q ":$PORT" && break
    n=$((n+1))
  done
  echo "=== 6) 等 guest 切 graphics 模式 ==="
  sleep 45
  printf 'pmemsave 0xfd000000 0x1000000 fb.raw\n' | $B nc 127.0.0.1 $PORT >/dev/null 2>&1
  sleep 3
  printf 'quit\n' | $B nc 127.0.0.1 $PORT >/dev/null 2>&1
  if [ -s "$R/tmp/fb.raw" ]; then
    echo "RESULT: QEMU_SCREEN_OK size=$(wc -c < "$R/tmp/fb.raw")"
  else
    echo "RESULT: QEMU_SCREEN_FAIL no framebuffer dump"
  fi
else
  # ---- 串口模式 ----
  rm -f "$R/tmp/ser.txt"
  setsid $Q -m 768 -kernel /boot/vmlinuz-lts -initrd /tmp/q.gz \
    -append "console=ttyS0 rdinit=/init" \
    -vga std -display none \
    -serial file:/tmp/ser.txt -monitor none \
    -accel tcg -L /usr/share/qemu \
    > "$R/tmp/qserial.log" 2>&1 &
  n=0
  while [ $n -lt 40 ]; do
    sleep 2
    grep -aq "DIAG END" "$R/tmp/ser.txt" 2>/dev/null && break
    n=$((n+1))
  done
  echo "=== 6) 串口输出 ==="
  grep -a -A10 "GUEST BOOT" "$R/tmp/ser.txt" 2>/dev/null | head -14
  if grep -aq "DIAG END" "$R/tmp/ser.txt" 2>/dev/null; then
    echo "RESULT: QEMU_SERIAL_OK"
  else
    echo "RESULT: QEMU_SERIAL_FAIL"
  fi
fi

for p in $(ps -A -o PID,NAME 2>/dev/null | grep -w "qemu-system-x86_64" | awk '{print $1}'); do
  kill -9 "$p" 2>/dev/null
done
