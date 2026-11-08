#!/usr/bin/env python3
"""容器内真实图形界面软件 —— dshgui demo 组的静态契约探针。

为什么需要这个探针：
    「容器能跑 GUI」这件事过去用 xterm/xcalc 证明，但那类是最小 X11 客户端，
    只能说明 X 协议通。真正的桌面应用（NetSurf / Geany）要额外依赖
    字体栈、图标主题、GTK 主题、文件选择器、会话管理 —— 这些才是
    「手机端工作站」成立的门槛。本探针把这条链路的入口固定下来。

覆盖：
    A. help 里 demo / demo-list 有说明（否则 AI 不知道有这个入口）
    B. demo-list 与 demo 两个函数存在且被 case 分派
    C. DEMO_KNOWN 列出的软件与分支实现一一对应（不许列了却不能跑）
    D. netsurf / geany 分支各自准备了前置文件（否则起来是白屏）
    E. 未安装时给可操作的提示（apk add X），且退出码 3
    F. Xvfb 未起时自动拉起（不许要求用户先手动 start）
    G. 启动后调 cmd_windows（否则调用方不知道窗口 ID）

反向验证：把 E 的退出码改 0、或删掉 F 的自动拉起，本探针必须变红。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TARGET = os.path.join(ROOT, "scripts", "dshgui.sh")

checks = []
def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))

def main():
    if not os.path.isfile(TARGET):
        print("FAIL: 找不到 %s" % TARGET)
        return 2
    src = open(TARGET, encoding="utf-8").read()
    # 剥注释：只在「非注释行」里找实现，避免把帮助文本当代码
    code = "\n".join(l for l in src.splitlines() if not l.lstrip().startswith("#"))

    # ---- A. 帮助里有入口 ----
    check("A1 帮助含 demo 说明", re.search(r"#\s*dshgui demo\b", src) is not None)
    check("A2 帮助含 demo-list 说明", re.search(r"#\s*dshgui demo-list\b", src) is not None)

    # ---- B. 函数与分派 ----
    check("B1 cmd_demo_list 已定义", "cmd_demo_list()" in code)
    check("B2 cmd_demo 已定义", "cmd_demo()" in code)
    check("B3 case 分派 demo-list", re.search(r"^\s*demo-list\)", code, re.M) is not None)
    check("B4 case 分派 demo", re.search(r"^\s*demo\)", code, re.M) is not None)

    # ---- C. DEMO_KNOWN 与分支一致 ----
    m = re.search(r'^DEMO_KNOWN="([^"]+)"', code, re.M)
    check("C1 DEMO_KNOWN 存在", m is not None)
    known = m.group(1).split() if m else []
    check("C2 DEMO_KNOWN 至少含 netsurf/geany",
          {"netsurf", "geany"} <= set(known), "实际=%s" % known)

    # 每个 known 要么有专属分支，要么落到通用 *) 分支
    demo_fn = ""
    mm = re.search(r"cmd_demo\(\)\s*\{(.*?)\n\}", code, re.S)
    if mm:
        demo_fn = mm.group(1)
    has_generic = re.search(r"^\s*\*\)", demo_fn, re.M) is not None
    for name in known:
        specific = re.search(r"^\s*%s\)" % re.escape(name), demo_fn, re.M) is not None
        check("C3 %s 有启动路径" % name, specific or has_generic,
              "专属分支=%s 通用分支=%s" % (specific, has_generic))

    # ---- D. 各分支的前置文件 ----
    check("D1 netsurf 准备了 index.html", "/tmp/site/index.html" in demo_fn)
    check("D2 geany 准备了 demo.c", "/tmp/demo.c" in demo_fn)
    # 前置文件必须是「没有才创建」，不能每次覆盖用户改过的内容
    check("D3 前置文件条件创建（不覆盖）",
          demo_fn.count("[ -f ") >= 2)

    # ---- E. 未安装提示与退出码 ----
    check("E1 未安装时提示 apk add", re.search(r"apk add \$name|apk add \{?\$name", demo_fn) is not None
          or "apk add" in demo_fn)
    check("E2 未安装返回非零", re.search(r"command -v \"\$name\".*?return\s+[1-9]", demo_fn, re.S) is not None)

    # ---- F. Xvfb 自动拉起 ----
    check("F1 未起 Xvfb 时自动 cmd_start", re.search(r"xdpyinfo.*?cmd_start", demo_fn, re.S) is not None)

    # ---- G. 启动后报告窗口 ----
    check("G1 启动后调 cmd_windows", "cmd_windows" in demo_fn)

    # ---- H. 通用分支也走 DISPLAY ----
    check("H1 通用分支带 DISPLAY", re.search(r"nohup \"\$name\"", demo_fn) is not None
          and re.search(r'DISPLAY="\$DISPLAY_ID"\s+nohup "\$name"', demo_fn) is not None)

    npass = sum(1 for _, ok, _ in checks if ok)
    nfail = len(checks) - npass
    for name, ok, detail in checks:
        print(("  OK   " if ok else "  FAIL ") + name + (("  <" + detail + ">") if detail and not ok else ""))
    print("\n%d checks, %d failed" % (len(checks), nfail))
    return 0 if nfail == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
