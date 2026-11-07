#!/bin/sh
# bridge_probe.sh —— 在**容器内**验证宿主 Android 设备能力链路
#
# 为什么需要这个探针：Linux 模式的「设备能力」不是技术细节，而是我们相对
# tawcroot 的核心差异 —— TAWC 是「装一个 Linux 用它」，我们是「Linux 里的
# agent 能直接操作这台手机」。但这条链路跨了三个域：
#     容器（musl）→ 宿主回环 TCP → App 的 AgentBridge → Android 无障碍服务
# 任何一环断掉，症状都只是「某个工具报 403/超时」，看不出断在哪。
#
# 用法（在容器内跑，DSH_BRIDGE_TOKEN 由引擎环境提供）：
#   sh /bridge_probe.sh
#
# 期望输出：RESULT: BRIDGE_OK，且四项各自 ok
set -u
PORT="${DSH_BRIDGE_PORT:-3083}"
TOKEN="${DSH_BRIDGE_TOKEN:-}"
BASE="http://127.0.0.1:$PORT"
fails=0

kv() { printf '%s=%s\n' "$1" "$2"; }
bad() { kv "$1" FAIL; fails=$((fails + 1)); }

# 没有 wget 就用 busybox 的；两边都确认存在
WGET="$(command -v wget 2>/dev/null || command -v busybox 2>/dev/null || true)"
[ -n "$WGET" ] || { echo "RESULT: BRIDGE_FAIL no-wget"; exit 2; }
case "$WGET" in *busybox) WGET="busybox wget" ;; esac

post() {  # post <path> <json>
    $WGET -qO- --timeout=8 --header="X-DSH-Token: $TOKEN" --post-data="$2" "$BASE$1" 2>&1
}

# ---- 1) 无 token 必须 403（证明鉴权真的在起作用，不是裸奔）----
r=$($WGET -qO- --timeout=5 --post-data="" "$BASE/idle" 2>&1)
case "$r" in
    *403*) kv no_token_403 ok ;;
    *)     bad no_token_403 ;;
esac

# ---- 2) 带 token 必须可达（/idle 返回 json）----
r=$(post /idle '{}')
case "$r" in
    *'"ok":true'*) kv auth_ok ok ;;
    *)             bad auth_ok ;;
esac

# ---- 3) 无障碍服务可用（/idle 会给出当前前台包名）----
case "$r" in
    *'"pkg":'*) kv a11y_ok ok ;;
    *)          bad a11y_ok ;;
esac

# ---- 4) 真的能操作设备：启动一个系统 app 并核对回执 ----
r=$(post /launch '{"pkg":"com.android.settings"}')
case "$r" in
    *'"ok":true'*'launched'*) kv launch_ok ok ;;
    *)                        bad launch_ok ;;
esac

if [ "$fails" -eq 0 ]; then
    echo "RESULT: BRIDGE_OK"
    exit 0
fi
echo "RESULT: BRIDGE_FAIL ($fails)"
exit 1
