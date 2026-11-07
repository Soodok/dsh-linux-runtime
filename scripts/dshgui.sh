#!/bin/sh
# dshgui —— 容器内 GUI 环境的统一入口（给 AI 用的 API）
#
# 设计意图：AI 在容器里编译完软件后，需要「看得见 + 点得动」。
# 这个脚本把 Xvfb / 窗口管理 / 抓屏 / 输入注入收成几个幂等子命令，
# AI 只要记 dshgui capture / click / type 三件套即可完成功能实测。
#
# 用法：
#   dshgui start [WxHxD]      起 Xvfb（默认 800x600x24，display :99）
#   dshgui stop               关掉 Xvfb 与所有 X 客户端
#   dshgui run <cmd...>       在 :99 上后台跑一个 GUI 程序
#   dshgui capture [out.png]  抓整屏 → PNG（默认 /tmp/dshgui.png）
#   dshgui windows            列出可见窗口（含 pid / 几何）
#   dshgui focus <wid>        强制把输入焦点给该窗口
#   dshgui click <x> <y> [n]  鼠标移到 (x,y) 并点左键 n 次（默认 1）
#   dshgui type <text>        注入文本
#   dshgui key <keysym>       注入按键（如 Return / ctrl+c）
#
# 环境变量：
#   DSH_DISPLAY  默认 :99
#   DSH_OUTDIR   抓屏目录（容器内路径），默认 /tmp
#
# 退出码：0 成功；1 用法错误；2 GUI 未启动；3 操作失败

set -u

DISPLAY_ID="${DSH_DISPLAY:-:99}"
OUTDIR="${DSH_OUTDIR:-/tmp}"
SIZE_DEFAULT="800x600x24"

have_bin() { command -v "$1" >/dev/null 2>&1; }

need_gui() {
    if ! have_bin xdotool; then
        echo "dshgui: 缺少 xdotool（apk add xdotool）" >&2
        exit 2
    fi
    if ! DISPLAY="$DISPLAY_ID" xdotool getdisplaygeometry >/dev/null 2>&1; then
        echo "dshgui: $DISPLAY_ID 上没有 X 服务器（先跑 dshgui start）" >&2
        exit 2
    fi
}

cmd_start() {
    size="${1:-$SIZE_DEFAULT}"
    if DISPLAY="$DISPLAY_ID" xdotool getdisplaygeometry >/dev/null 2>&1; then
        echo "already-running $DISPLAY_ID"
        DISPLAY="$DISPLAY_ID" xdotool getdisplaygeometry
        return 0
    fi
    have_bin Xvfb || { echo "dshgui: 缺少 Xvfb（apk add xvfb）" >&2; exit 2; }
    # -nolock：Android 的 SELinux 禁止在 app_data_file 上建 hardlink，
    # X 服务器默认会把 /tmp/.X<n>-lock 做 link(2)，实测必失败
    # （Fatal server error: Linking lock file in place failed: Permission denied）。
    Xvfb "$DISPLAY_ID" -screen 0 "$size" -nolock >/tmp/dshgui-xvfb.log 2>&1 &
    # 等 socket 出现，最多 10 秒
    i=0
    while [ $i -lt 100 ]; do
        if DISPLAY="$DISPLAY_ID" xdotool getdisplaygeometry >/dev/null 2>&1; then
            echo "started $DISPLAY_ID $size"
            DISPLAY="$DISPLAY_ID" xdotool getdisplaygeometry
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    echo "dshgui: Xvfb 起不来，日志：" >&2
    tail -20 /tmp/dshgui-xvfb.log >&2 2>/dev/null
    exit 3
}

cmd_stop() {
    pkill -f "Xvfb $DISPLAY_ID" 2>/dev/null
    pkill -f xterm 2>/dev/null
    pkill -f xeyes 2>/dev/null
    rm -f /tmp/.X11-unix/X"${DISPLAY_ID#:}" 2>/dev/null
    echo "stopped $DISPLAY_ID"
}

cmd_run() {
    [ $# -gt 0 ] || { echo "dshgui run: 需要命令" >&2; exit 1; }
    need_gui
    DISPLAY="$DISPLAY_ID" "$@" >/tmp/dshgui-run.log 2>&1 &
    echo "spawned pid=$! log=/tmp/dshgui-run.log"
}

cmd_capture() {
    out="${1:-$OUTDIR/dshgui.png}"
    need_gui
    have_bin xwd || { echo "dshgui: 缺少 xwd（apk add xwd）" >&2; exit 2; }
    xwd_file="${out%.png}.xwd"
    DISPLAY="$DISPLAY_ID" xwd -root -out "$xwd_file" 2>/dev/null || {
        echo "dshgui: xwd 抓屏失败" >&2; exit 3; }
    if have_bin magick; then
        magick "$xwd_file" "$out" 2>/dev/null
    elif have_bin convert; then
        convert "$xwd_file" "$out" 2>/dev/null
    else
        # 没有 imagemagick 就保留 xwd（调用方自己转）
        echo "dshgui: 无 imagemagick，已保留 $xwd_file"
        rm -f "$out"
        echo "$xwd_file"
        return 0
    fi
    rm -f "$xwd_file"
    if [ -s "$out" ]; then
        echo "captured $out $(wc -c <"$out") bytes"
    else
        echo "dshgui: 抓屏产物为空" >&2; exit 3
    fi
}

cmd_windows() {
    need_gui
    for w in $(DISPLAY="$DISPLAY_ID" xdotool search --onlyvisible --name . 2>/dev/null); do
        name=$(DISPLAY="$DISPLAY_ID" xdotool getwindowname "$w" 2>/dev/null)
        pid=$(DISPLAY="$DISPLAY_ID" xdotool getwindowpid "$w" 2>/dev/null)
        geo=$(DISPLAY="$DISPLAY_ID" xdotool getwindowgeometry --shell "$w" 2>/dev/null |
              tr '\n' ' ')
        echo "WID=$w pid=${pid:-?} name=[$name] $geo"
    done
}

cmd_focus() {
    [ $# -ge 1 ] || { echo "dshgui focus: 需要窗口 id" >&2; exit 1; }
    need_gui
    # Xvfb 没有窗口管理器，_NET_ACTIVE_WINDOW 不存在，windowactivate 会报错，
    # 只有 windowfocus 能真正把键盘焦点挪过去（实测不 focus 的话 type 全被丢弃）。
    DISPLAY="$DISPLAY_ID" xdotool windowfocus --sync "$1" 2>/dev/null || {
        echo "dshgui: focus $1 失败" >&2; exit 3; }
    echo "focused $1 (now=$(
        DISPLAY="$DISPLAY_ID" xdotool getwindowfocus 2>/dev/null))"
}

cmd_click() {
    [ $# -ge 2 ] || { echo "dshgui click: 需要 x y [次数]" >&2; exit 1; }
    need_gui
    n="${3:-1}"
    DISPLAY="$DISPLAY_ID" xdotool mousemove "$1" "$2" 2>/dev/null || exit 3
    # 点窗口主体，否则无 WM 环境下事件可能投给根窗口
    i=0
    while [ $i -lt "$n" ]; do
        DISPLAY="$DISPLAY_ID" xdotool click 1 2>/dev/null || exit 3
        i=$((i + 1))
    done
    echo "clicked $1,$2 x$n"
}

cmd_type() {
    [ $# -ge 1 ] || { echo "dshgui type: 需要文本" >&2; exit 1; }
    need_gui
    # --clearmodifiers：避免上一次注入残留的修饰键污染
    DISPLAY="$DISPLAY_ID" xdotool type --clearmodifiers --delay 40 "$*" 2>/dev/null || exit 3
    echo "typed ${#*} chars"
}

cmd_key() {
    [ $# -ge 1 ] || { echo "dshgui key: 需要键名" >&2; exit 1; }
    need_gui
    DISPLAY="$DISPLAY_ID" xdotool key --clearmodifiers "$*" 2>/dev/null || exit 3
    echo "key $*"
}

case "${1:-}" in
    start)   shift; cmd_start "$@" ;;
    stop)    shift; cmd_stop "$@" ;;
    run)     shift; cmd_run "$@" ;;
    capture) shift; cmd_capture "$@" ;;
    windows) shift; cmd_windows "$@" ;;
    focus)   shift; cmd_focus "$@" ;;
    click)   shift; cmd_click "$@" ;;
    type)    shift; cmd_type "$@" ;;
    key)     shift; cmd_key "$@" ;;
    *)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
