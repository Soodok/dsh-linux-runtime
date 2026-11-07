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
# —— 虚拟机（QEMU）系列：把「手机里跑模拟器」也纳进同一个入口 ——
#   dshgui vm-start [--iso X] [--mem 768] [--vga std]
#                             启动 QEMU 虚拟机（默认 linux-lts + bochs DRM）
#   dshgui vm-stop            关掉虚拟机
#   dshgui vm-status          是否在跑 + monitor 端口 + 当前分辨率
#   dshgui vm-capture [out]   抓虚拟机整屏 → PNG（默认 /tmp/dshvm.png）
#   dshgui vm-key <k...>      注入按键（如 vm-key h e l l o）
#   dshgui vm-click <x> <y>   注入鼠标点击
#   dshgui vm-type <text>     注入文本（逐字符转 sendkey）
#
# ※ 虚拟机与 X11 是两套独立环境：vm-* 不需要 dshgui start，可同时使用。
#
# 环境变量：
#   DSH_DISPLAY  默认 :99
#   DSH_OUTDIR   抓屏目录（容器内路径），默认 /tmp
#   DSH_VM_MON   虚拟机 monitor 首选端口（默认 4492，占用则顺延）
#
# 退出码：0 成功；1 用法错误；2 GUI 未启动；3 操作失败；4 虚拟机未启动

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

# ============================================================
# 虚拟机（QEMU）系列
#
# 为什么用 monitor 而不是 VNC 取画面：
#   VNC 在 guest 尚未 drmModeSetCrtc 时报 desktop 0x0，只写出 16 字节帧头
#   （实测多次，QEMU 10 + -vga std）。monitor 的 screendump 直接读
#   framebuffer，9ms/帧，比 pmemsave 快 3.4 倍、体积小 5 倍。
#
# 为什么 screendump 的文件参数不能带 "/"：
#   HMP 把路径里的 / 当**除号**解析，报 invalid char 错误。
#   必须用相对名，靠 QEMU 进程的 cwd（这里是 /tmp）定位。
# ============================================================

VM_STATE=/tmp/dshgui-vm.state          # 内容 "pid:port"
VM_SHOT=dshgui-vm-shot.ppm             # screendump 中转（相对名！别改成绝对路径）

vm_load() {
    [ -f "$VM_STATE" ] || return 4
    VM_PID=$(cut -d: -f1 "$VM_STATE" 2>/dev/null)
    VM_PORT=$(cut -d: -f2 "$VM_STATE" 2>/dev/null)
    [ -n "${VM_PID:-}" ] && [ -n "${VM_PORT:-}" ] || return 4
    kill -0 "$VM_PID" 2>/dev/null || return 4
    return 0
}

vm_port_free() {
    # 固定端口连续验证会撞 TIME_WAIT，QEMU 报
    # "Failed to find an available port: Address in use"，
    # 而外层脚本会把它伪装成"共享文件没生成"。必须逐端口探测。
    if have_bin netstat; then
        netstat -ltn 2>/dev/null | grep -q ":$1 " && return 1
    fi
    return 0
}

cmd_vm_start() {
    mem=768; vga=std; iso=""
    while [ $# -gt 0 ]; do
        case "$1" in
            --mem) mem="$2"; shift 2 ;;
            --vga) vga="$2"; shift 2 ;;
            --iso) iso="$2"; shift 2 ;;
            *) echo "dshgui vm-start: 未知参数 $1" >&2; exit 1 ;;
        esac
    done
    if vm_load; then
        echo "already-running pid=$VM_PID port=$VM_PORT"
        return 0
    fi
    have_bin qemu-system-x86_64 || {
        echo "dshgui: 缺少 qemu（apk add qemu-system-x86_64）" >&2; exit 2; }

    port=""
    for cand in "${DSH_VM_MON:-4492}" 4493 4494 4495 4496 4497 4498 4499 4500 4501 4502; do
        if vm_port_free "$cand"; then port="$cand"; break; fi
    done
    [ -n "$port" ] || { echo "dshgui: 找不到空闲 monitor 端口" >&2; exit 3; }

    cd /tmp || exit 3
    if [ -n "$iso" ]; then
 # ★必须带 usb-ehci + usb-tablet：
 #   默认 QEMU 不给 guest 鼠标设备，
 #   guest 里 /proc/bus/input/devices 只有键盘。
 #   是 usb-ehci 不是 qemu-xhci —— 后者在
 #   TCG+SeaBIOS 下内核枚举不到。
        setsid qemu-system-x86_64 -m "$mem" -vga "$vga" -display none \
            -monitor "tcp:127.0.0.1:$port,server,nowait" \
            -device usb-ehci -device usb-tablet,id=tab0 \
            -accel tcg -cdrom "$iso" >/tmp/dshgui-vm.log 2>&1 &
    else
        [ -f /tmp/q.gz ] || {
            echo "dshgui: 缺 /tmp/q.gz（先跑 qemu_boot.sh 生成 initrd，或用 --iso）" >&2
            exit 3; }
        setsid qemu-system-x86_64 -m "$mem" -kernel /boot/vmlinuz-lts \
            -initrd /tmp/q.gz -append "console=tty0 rdinit=/init" \
            -vga "$vga" -display none \
            -monitor "tcp:127.0.0.1:$port,server,nowait" \
            -device usb-ehci -device usb-tablet,id=tab0 \
            -accel tcg -L /usr/share/qemu >/tmp/dshgui-vm.log 2>&1 &
    fi
    vpid=$!
    echo "$vpid:$port" > "$VM_STATE"

    i=0
    while [ $i -lt 50 ]; do
        if have_bin netstat && netstat -ltn 2>/dev/null | grep -q ":$port "; then
            echo "started pid=$vpid monitor=127.0.0.1:$port"
            return 0
        fi
        sleep 0.2
        i=$((i+1))
    done
    if kill -0 "$vpid" 2>/dev/null; then
        echo "started pid=$vpid monitor=127.0.0.1:$port (netstat 不可用，端口未确认)"
        return 0
    fi
    echo "dshgui: 虚拟机起不来，日志：" >&2
    tail -20 /tmp/dshgui-vm.log >&2 2>/dev/null
    rm -f "$VM_STATE"
    exit 3
}

cmd_vm_stop() {
    if vm_load; then
        # ★不要用 pkill -f qemu-system：它会匹配到调用它的脚本自身
        #   （命令行里含该串），实测把整个 .sh 一起杀掉，表现为"脚本无输出"。
        kill -9 "$VM_PID" 2>/dev/null
    fi
    rm -f "$VM_STATE"
    echo "stopped"
}

cmd_vm_status() {
    if ! vm_load; then
        echo "not-running"
        exit 4
    fi
    echo "running pid=$VM_PID monitor=127.0.0.1:$VM_PORT"
    ( cd /tmp && printf 'screendump %s\n' "$VM_SHOT" | \
      nc 127.0.0.1 "$VM_PORT" >/dev/null 2>&1 )
    if [ -s "/tmp/$VM_SHOT" ]; then
        # PPM 头是 3 行（"P6" / "宽 高" / "maxval"），
        # 只取前两行会漏掉分辨率，显示成 "P6 720 400" 这种半截信息。
        head -3 "/tmp/$VM_SHOT" | tr '\n' ' '
        echo ""
    fi
}

cmd_vm_capture() {
    out="${1:-$OUTDIR/dshvm.png}"
    vm_load || { echo "dshgui: 虚拟机未启动（先跑 dshgui vm-start）" >&2; exit 4; }
    have_bin nc || { echo "dshgui: 缺少 nc（apk add netcat-openbsd）" >&2; exit 2; }

    ( cd /tmp && printf 'screendump %s\n' "$VM_SHOT" | \
      nc 127.0.0.1 "$VM_PORT" >/dev/null 2>&1 )
    [ -s "/tmp/$VM_SHOT" ] || { echo "dshgui: screendump 无产物" >&2; exit 3; }

    if have_bin magick; then
        magick "/tmp/$VM_SHOT" "$out" 2>/dev/null
    elif have_bin convert; then
        convert "/tmp/$VM_SHOT" "$out" 2>/dev/null
    else
        echo "dshgui: 无 imagemagick，已保留 /tmp/$VM_SHOT"
        echo "/tmp/$VM_SHOT"
        return 0
    fi
    if [ -s "$out" ]; then
        echo "captured $out $(wc -c <"$out") bytes"
    else
        echo "dshgui: 转换失败，PPM 保留在 /tmp/$VM_SHOT" >&2
        exit 3
    fi
}

cmd_vm_key() {
    [ $# -ge 1 ] || { echo "dshgui vm-key: 需要至少一个键名" >&2; exit 1; }
    vm_load || { echo "dshgui: 虚拟机未启动" >&2; exit 4; }
    # 一条长连接连发所有 sendkey：每次起 nc 要 207ms，连发只要 9ms/条
    {
        for k in "$@"; do echo "sendkey $k"; done
        sleep 1
    } | nc 127.0.0.1 "$VM_PORT" >/dev/null 2>&1
    echo "sent keys: $*"
}

cmd_vm_type() {
    [ $# -ge 1 ] || { echo "dshgui vm-type: 需要文本" >&2; exit 1; }
    vm_load || { echo "dshgui: 虚拟机未启动" >&2; exit 4; }
    # HMP sendkey 用小写键名；空格是 spc；大写靠 shift- 前缀
    {
        printf '%s' "$*" | while IFS= read -r -n1 ch; do
            case "$ch" in
                " ") echo "sendkey spc" ;;
                "-") echo "sendkey minus" ;;
                ".") echo "sendkey dot" ;;
                "/") echo "sendkey slash" ;;
                "") : ;;
                [A-Z]) lc=$(echo "$ch" | tr 'A-Z' 'a-z'); echo "sendkey shift-$lc" ;;
                *) echo "sendkey $ch" ;;
            esac
        done
        sleep 1
    } | nc 127.0.0.1 "$VM_PORT" >/dev/null 2>&1
    echo "typed ${#*} chars"
}

cmd_vm_click() {
    [ $# -ge 2 ] || { echo "dshgui vm-click: 需要 x y" >&2; exit 1; }
    vm_load || { echo "dshgui: 虚拟机未启动" >&2; exit 4; }
    # 鼠标链路已实测通，判据是 guest 侧读 /dev/input/mice 的 PS/2 包：
    #   单次左键点击 → 恰好 2 个包 [0x09 按下][0x08 松开]，bit3 同步位恒 1。
    #
    # 三个前提（缺任一都读不到事件，全部踩过）：
    #   1. QEMU 带 -device usb-ehci -device usb-tablet（见 vm-start）
    #   2. guest 里 insmod ehci-hcd/ehci-pci/hid/usbhid/hid-generic/mousedev/evdev
    #      这套内核里它们全是 =m，不加载等于主板没有 USB 控制器
    #   3. 读的是 /dev/input/mice（PS/2 协议），不是 /dev/input/event1 ——
    #      evdev 路径在这套配置下不产事件，这是之前一直"读不到"的真因
    #
    # HMP 的 mouse_move 是相对位移。接了 usb-tablet（绝对坐标设备）后
    # QEMU 会把相对位移换算成绝对位置，所以下面「先回左上角再相对移动」可用。
    {
        echo "mouse_move -10000 -10000"
        sleep 0.2
        echo "mouse_move $1 $2"
        sleep 0.2
        echo "mouse_button 1"
        sleep 0.1
        echo "mouse_button 0"
        sleep 0.5
    } | nc 127.0.0.1 "$VM_PORT" >/dev/null 2>&1
    echo "clicked $1,$2"
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
    vm-start)   shift; cmd_vm_start "$@" ;;
    vm-stop)    shift; cmd_vm_stop "$@" ;;
    vm-status)  shift; cmd_vm_status "$@" ;;
    vm-capture) shift; cmd_vm_capture "$@" ;;
    vm-key)     shift; cmd_vm_key "$@" ;;
    vm-type)    shift; cmd_vm_type "$@" ;;
    vm-click)   shift; cmd_vm_click "$@" ;;
    *)
        sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
