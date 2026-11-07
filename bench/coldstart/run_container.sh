#!/system/bin/sh
# run_container.sh —— 在 **root 域用容器** 跑引擎并测冷启
#
# 与 run.sh 的区别：run.sh 走原生 bionic node（engine/bin/node, v24.18.0），
# 这个走容器里的 musl node（/usr/bin/node, v22.23.4）。同一份引擎代码。
#
# 为什么必须分开测：此前所有冷启数据（1.9s / 3.4s / 缓存省 1112ms）全部来自
# 原生路径，容器路径**从未与原生做过同设备对比**。而「Linux 模式」才是产品
# 差异化的核心，它的冷启代价必须先量出来，否则优化方向可能是错的。
#
# 用法：
#   adb shell "sh /data/local/tmp/run_container.sh"
#   端口用 --port 19998（原生用 19999，App 用 3080）。
#   ★不能用 PORT 环境变量：`dsh web` 只认 --port 参数，实测 PORT 设了
#   19998 仍监听 3080（pitfalls 已记过一次，这里再踩一次）。
#
# 每轮前清场：pkill -9 -f 'dsh/lib/bin.js'; pkill -9 -f dsh_alpine; sleep 2
set -u
E=/data/data/app.dsh.mobile/files/engine
H=/data/data/app.dsh.mobile/files/dsh-home
RUNNER=/data/data/app.dsh.mobile/files/linux/runner/dsh_alpine
R=/data/data/app.dsh.mobile/files/linux/distros/alpine
I=/lib/ld-musl-x86_64.so.1

exec "$RUNNER" \
  -m "/eng:$E" \
  -m "/dsh:$H" \
  -m /data/data:/data/data \
  "$R" "$I" /bin/sh -c '
export HOME=/dsh DSH_HOME=/dsh TMPDIR=/dsh/tmp
mkdir -p "$TMPDIR"
export NODE_COMPILE_CACHE=/dsh/.node-compile-cache
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export TZ=Asia/Shanghai
cd /eng
exec node --expose-internals \
  --require /eng/node-compile-cache-flusher.cjs \
  --require /probes/readyat.cjs \
  /eng/lib/node_modules/@deepseek-ai/dsh/lib/bin.js web \
  --patch /eng/android-overlay.yml --port 19998 --no-open
'
