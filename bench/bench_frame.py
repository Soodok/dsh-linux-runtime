#!/usr/bin/env python3
"""容器画面端到端帧率基准（多分辨率 × 多轮取中位）

用法：python bench_frame.py [串口]
前提：设备上已部署 rootfs、runner、framesrv（App 装过一次即可）。

测的是完整链路：容器渲染 → glReadPixels → 共享文件 → 宿主读回。
每一轮都独立起帧服务、跑固定帧数、读回共享文件核对尺寸，再取中位数。
"""
import os, statistics, subprocess, sys, tempfile, time

ADB = os.environ.get("ADB", r"/d/AndroidWatch_ADB_ToolBox/adb/adb.exe")
DEV = sys.argv[1] if len(sys.argv) > 1 else "emulator-5554"
PKG = "app.dsh.mobile"
BASE = "/data/data/%s/files" % PKG
RUNNER = BASE + "/linux/runner/dsh_alpine"
ROOTFS = BASE + "/linux/distros/alpine"
SHARE = BASE + "/dshframe.bin"

def sh(cmd, timeout=180):
    r = subprocess.run([ADB, "-s", DEV, "shell", cmd], capture_output=True, timeout=timeout)
    return r.stdout.decode("utf-8", "replace")

def run_one(w, h, frames=60, interval=0):
    sh("rm -f " + SHARE)
    cmd = ("run-as %s %s %s /lib/ld-musl-x86_64.so.1 /bin/sh -c "
           "'/framesrv %d %d %d %d %s'") % (PKG, RUNNER, ROOTFS, w, h, frames, interval, SHARE)
    out = sh(cmd, timeout=300)
    fps = None
    for line in out.splitlines():
        if line.startswith("done frames="):
            parts = dict(p.split("=") for p in line.split()[1:] if "=" in p)
            fps = float(parts.get("fps", 0))
    size = sh("stat -c %%s %s 2>/dev/null" % SHARE).strip()
    return fps, size

def main():
    res = {}
    for (w, h) in [(640, 360), (1280, 720), (1920, 1080)]:
        vals, sizes = [], set()
        for rnd in range(3):
            fps, size = run_one(w, h)
            if fps:
                vals.append(fps)
            sizes.add(size)
            time.sleep(1)
        if vals:
            res["%dx%d" % (w, h)] = (statistics.median(vals), vals, sizes)
    print("\n分辨率      中位 fps   各轮             共享文件")
    for k, (med, vals, sizes) in res.items():
        print("%-11s %7.1f   %-16s %s" % (k, med, "/".join("%.1f" % v for v in vals), ",".join(sorted(sizes))))
    print("\n注：interval=0 表示不限速，测的是管线上限。")

if __name__ == "__main__":
    main()
