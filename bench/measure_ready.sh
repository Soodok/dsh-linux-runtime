#!/system/bin/sh
# measure_ready.sh —— 在设备上跑一轮「web 就绪」测量并只输出 readyat 那一行
#
# 为什么要在设备上跑脚本：adb shell 的每次调用都有 ~200-400ms 的往返开销，
# 而且引擎起来后不会退出（常驻服务），本地循环会一直挂着。
# 放到设备上跑 + 自超时，才能稳定拿到数字。
#
# 用法（在设备上）：
#   sh measure_ready.sh <port> <timeout_sec>
set -u
B=/data/data/app.dsh.mobile/files
E=$B/engine
N=$E/bin/node
PORT_=$1
TMO=${2:-25}

export LD_LIBRARY_PATH=$E/lib
export TMPDIR=$B/dsh-home/tmp
export HOME=$B/dsh-home
export DSH_HOME=$B/dsh-home
export NODE_COMPILE_CACHE=$B/dsh-home/.node-compile-cache
export PORT=$PORT_

# 起引擎，输出写文件，后台跑
cd $B || exit 1
$N --expose-internals \
   --require $E/node-compile-cache-flusher.cjs \
   --require $E/readyat.cjs \
   $E/lib/node_modules/@deepseek-ai/dsh/lib/bin.js web --no-open \
   > $B/run_$PORT_.log 2>&1 &
PID=$!

# 轮询等就绪（最多 TMO 秒）
i=0
while [ $i -lt $((TMO * 10)) ]; do
  if grep -q 'dsh web:' $B/run_$PORT_.log 2>/dev/null; then break; fi
  if ! kill -0 $PID 2>/dev/null; then break; fi
  sleep 0.1
  i=$((i + 1))
done

# 不管成没成，杀掉
kill -9 $PID 2>/dev/null
sleep 0.3

echo "=== port $PORT_ ==="
grep -E 'readyat|dsh web:' $B/run_$PORT_.log 2>/dev/null | head -3
if ! grep -q 'dsh web:' $B/run_$PORT_.log 2>/dev/null; then
  echo "NOT READY; last lines:"
  tail -6 $B/run_$PORT_.log 2>/dev/null
fi
rm -f $B/run_$PORT_.log
