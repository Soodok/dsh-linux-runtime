#!/system/bin/sh
# qscreensrv 端到端验证（生产脚本版本，从临时脚本整理而来）
#
# 三条已踩实的教训：
#  1) 旧进程没杀掉 → 往已被 `>` 截断的同一日志按旧偏移续写
#     → NUL 空洞、seq 从旧值续起，像"服务正常"实为两次运行混合。
#     必须先按 PID 精确清场并确认 leftover=0。
#  2) busybox od 不支持 -j 跳偏移（静默返回空）→ 读头用 dd。
#  3) 判据必须是 seq 增量（服务真的在出帧），
#     不能是"文件存在"或"日志有 STAT"——那两样在单帧模式下同样成立。
#
# 用法：sh qsrv_verify.sh [rootfs] [runner] [fps]
set -u
R="${1:-/data/local/tmp/rf}"
X="${2:-/data/local/tmp/ax}"
FPS="${3:-10}"
I=/lib/ld-musl-x86_64.so.1
Q="$X $R $I /usr/bin/qemu-system-x86_64"
B="$X $R $I /bin/busybox"
# ★端口必须随机：固定端口在连续验证时会撞 TIME_WAIT，
#   QEMU 报 "Failed to find an available port: Address in use"，
#   而脚本会把它伪装成"共享文件没生成"（qemu 日志才看得出真因）。
PORT=$(( 20000 + ($$ % 20000) ))
SHARE="$R/tmp/qframe.bin"
LOG="$R/tmp/qsrv_verify.log"

cleanup() {
  for p in $(ps -A -o PID,NAME 2>/dev/null \
             | grep -E "qscreensrv|qemu-system-x86_64" | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
  done
}

cleanup
sleep 3
LEFT=$(ps -A -o NAME 2>/dev/null | grep -cE "qscreensrv|qemu-system-x86_64")
echo "leftover=$LEFT"
[ "$LEFT" != "0" ] && { echo "RESULT: QSRV_VERIFY_FAILED dirty"; exit 1; }
rm -f "$SHARE" "$LOG" "$R/tmp/_qs.ppm"

cd "$R/tmp" || exit 1
setsid $Q -m 768 -kernel /boot/vmlinuz-lts -initrd /tmp/q.gz \
  -append "console=tty0 rdinit=/init" \
  -vga std -display none \
  -monitor "tcp:127.0.0.1:$PORT,server,nowait" \
  -accel tcg -L /usr/share/qemu > "$R/tmp/qq_verify.log" 2>&1 &

n=0
while [ $n -lt 25 ]; do
  sleep 2
  $B netstat -ltn 2>/dev/null | grep -q ":$PORT" && break
  n=$((n+1))
done
echo "monitor ready"
sleep 50

setsid $X $R $I /tmp/qscreensrv /tmp/qframe.bin 127.0.0.1 $PORT "$FPS" \
  > "$LOG" 2>&1 &

n=0
while [ $n -lt 25 ]; do
  sleep 2
  [ -s "$SHARE" ] && break
  n=$((n+1))
done

if [ ! -s "$SHARE" ]; then
  echo "RESULT: QSRV_VERIFY_FAILED no share file"
  dd if="$LOG" bs=1 count=300 2>/dev/null
  cleanup
  exit 1
fi

echo "--- header ---"
dd if="$SHARE" bs=1 count=16 2>/dev/null | $B od -A d -t u4

readseq() { dd if="$SHARE" bs=1 skip=4 count=4 2>/dev/null | $B od -A n -t u4 | tr -d ' \n'; }
S1=$(readseq)
sleep 3
S2=$(readseq)
DELTA=$((S2 - S1))
echo "seq $S1 -> $S2 (delta=$DELTA, 期望≈$((FPS*3)))"

cleanup
if [ "$DELTA" -ge $((FPS * 3 / 2)) ]; then
  echo "RESULT: QSRV_VERIFY_OK seq_delta=$DELTA"
else
  echo "RESULT: QSRV_VERIFY_STALLED seq_delta=$DELTA"
fi
