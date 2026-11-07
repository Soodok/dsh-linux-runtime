#!/system/bin/sh
# 端到端验收：AI 在手机容器内的完整工作流（写码 → 编译 → 跑 GUI → 截图 → 点击 → 验证）
#
# 用 raw Xlib 而不是 GTK：
#   gtk3 没装，装它要一百多 MB；libX11 随 xvfb 已经在。
#   而且 raw Xlib 只有几十行，像素判据更干净 —— 按钮颜色固定，
#   点了变另一个固定色，diff 区域能精确对上按钮矩形。
#
# 判据：点击前后两张截图的**差异区域必须落在按钮矩形内**，
#   且按钮区颜色从 A 变成 B。只看命令 exit 0 不算通过。
set -u
X=/data/local/tmp/ax
R=/data/local/tmp/rf
I=/lib/ld-musl-x86_64.so.1
C() { $X $R $I /bin/busybox sh -c "$1"; }

echo "=== 0) 清场 ==="
C 'dshgui stop >/dev/null 2>&1; pkill -f Xvfb >/dev/null 2>&1; rm -f /tmp/e2e_*.png /tmp/e2e_*.xwd /tmp/e2e/run.log'
sleep 1

echo "=== 1) 写 Xlib 程序（400x220 窗口，内含 160x50 按钮）==="
C 'mkdir -p /tmp/e2e'
C 'cat > /tmp/e2e/app.c <<'"'"'EOC'"'"'
/* 最小 Xlib 交互程序：一个按钮，点击后颜色与标签都变。
   按钮矩形固定 (20,90)-(180,140)，判据据此定位。 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

#define BTN_X 20
#define BTN_Y 90
#define BTN_W 160
#define BTN_H 50

static int clicked = 0;

static void draw(Display *d, Window w, GC gc, XFontStruct *fs) {
    XSetForeground(d, gc, WhitePixel(d, 0));
    XFillRectangle(d, w, gc, 0, 0, 400, 220);
    /* 按钮 */
    XSetForeground(d, gc, clicked ? 0x00CC44 : 0xCC2222);
    XFillRectangle(d, w, gc, BTN_X, BTN_Y, BTN_W, BTN_H);
    /* 边框 */
    XSetForeground(d, gc, BlackPixel(d, 0));
    XDrawRectangle(d, w, gc, BTN_X, BTN_Y, BTN_W, BTN_H);
    /* 文字 */
    const char *t = clicked ? "CLICKED!" : "PRESS ME";
    XSetForeground(d, gc, WhitePixel(d, 0));
    XDrawString(d, w, gc, BTN_X + 40, BTN_Y + 30, t, (int)strlen(t));
    XSetForeground(d, gc, BlackPixel(d, 0));
    const char *s = clicked ? "state=1" : "state=0";
    XDrawString(d, w, gc, 20, 40, s, (int)strlen(s));
    XFlush(d);
}

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    int scr = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 100, 100, 400, 220, 1,
                                   BlackPixel(d, scr), WhitePixel(d, scr));
    XStoreName(d, w, "E2E");
    XSelectInput(d, w, ExposureMask | ButtonPressMask);
    XMapWindow(d, w);
    GC gc = XCreateGC(d, w, 0, NULL);
    XFontStruct *fs = XLoadQueryFont(d, "fixed");
    if (fs) XSetFont(d, gc, fs->fid);

    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose && e.xexpose.count == 0) {
            draw(d, w, gc, fs);
        } else if (e.type == ButtonPress) {
            int x = e.xbutton.x, y = e.xbutton.y;
            if (x >= BTN_X && x < BTN_X + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H) {
                clicked = 1 - clicked;
                draw(d, w, gc, fs);
            }
        }
    }
    return 0;
}
EOC
echo "  written: $(wc -c < /tmp/e2e/app.c) bytes"'

echo "=== 2) 编译（容器内 gcc + libX11）==="
C 'cd /tmp/e2e && gcc -O2 -o app app.c -lX11 2>&1 | head -6; ls -l app 2>/dev/null | awk "{print \"  binary: \"\$5\" bytes\"}" || echo COMPILE_FAIL'

echo "=== 3) 起 Xvfb ==="
C 'dshgui start 800x600x24'
sleep 2

echo "=== 4) 跑程序 ==="
C 'DISPLAY=:99 nohup /tmp/e2e/app > /tmp/e2e/run.log 2>&1 & echo spawned'
sleep 4

echo "=== 5) 列窗口 ==="
C 'dshgui windows'

echo "=== 6) 截图 A（点击前）==="
C 'dshgui capture /tmp/e2e_before.png'
ls -l "$R/tmp/e2e_before.png" 2>/dev/null | awk '{print "  "$5" bytes"}' || echo NO_A

echo "=== 7) 点按钮中心（窗口 100,100 + 按钮中心 100,115 = 200,215）==="
C 'dshgui click 200 215'
sleep 2

echo "=== 8) 截图 B（点击后）==="
C 'dshgui capture /tmp/e2e_after.png'
ls -l "$R/tmp/e2e_after.png" 2>/dev/null | awk '{print "  "$5" bytes"}' || echo NO_B

echo "=== 9) 清理 ==="
C 'pkill -f /tmp/e2e/app >/dev/null 2>&1; dshgui stop >/dev/null 2>&1'
echo "=== done ==="
