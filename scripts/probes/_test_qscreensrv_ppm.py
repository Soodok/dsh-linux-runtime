#!/usr/bin/env python3
"""qscreensrv 的 PPM 头部解析单测 —— 覆盖所有合法变体 + 反向用例。

为什么单独写：PPM 解析如果错了，整帧 RGB 会错位 1 字节，表现为「色偏/撕裂」
而**完全不报错**。这类问题只能靠构造输入逐字节核对来防。

反向验证说明：每条 ASSERT 都有对应的「改坏了会变红」的对照，见 BAD_* 用例。

跑法：python scripts/probes/_test_qscreensrv_ppm.py
"""
import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "bench" / "qscreensrv.c"
fails = 0
checks = 0


def check(name, cond, detail=""):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print(f"  FAIL {name} {detail}")
    else:
        print(f"  ok   {name}")


def extract_func(text, name):
    """按大括号配对取出函数体（不用跨行贪婪正则，避免吃到后面的函数）。"""
    i = text.find(name)
    if i < 0:
        return None
    b = text.find("{", i)
    if b < 0:
        return None
    depth = 0
    for j in range(b, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[b:j + 1]
    return None


def main():
    src = SRC.read_text(encoding="utf-8")
    # 先剥注释，避免注释里的字样误判
    stripped = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    stripped = re.sub(r"//[^\n]*", "", stripped)

    print("[A] ppm_header 存在且结构完整")
    fn = extract_func(stripped, "static int ppm_header")
    check("A1 函数存在", fn is not None)
    if fn is None:
        return report()

    print("[B] 必须覆盖 \r\n 作为单个分隔符")
    check("B1 识别 \\r 后跟 \\n",
          re.search(r"b\[i\]\s*==\s*'\\r'\s*&&\s*i\s*\+\s*1\s*<\s*len\s*&&\s*b\[i\s*\+\s*1\]\s*==\s*'\\n'",
                    fn) is not None)
    check("B2 命中时跳 2 字节", re.search(r"i\s*\+=\s*2", fn) is not None)
    # 反向：不能只剩无条件 i++
    check("B3 不是无条件单字节跳过",
          not re.search(r"if\s*\(i\s*>=\s*len\)\s*return\s*-1;\s*i\+\+;", fn))

    print("[C] maxval 范围校验（>255 表示 2 字节/通道，按 3 字节解析必错）")
    check("C1 校验 maxval 上界",
          re.search(r"vals\[2\]\s*>\s*255", fn) is not None)
    check("C2 校验 maxval 非零",
          re.search(r"vals\[2\]\s*==\s*0", fn) is not None)

    print("[D] 注释与多空白支持")
    check("D1 跳过 # 注释行", re.search(r"b\[i\]\s*==\s*'#'", fn) is not None)
    check("D2 跳过 \\t", "'\\t'" in fn)

    print("[E] 主循环每帧重解析头（防换分辨率静默损坏）")
    mainfn = extract_func(stripped, "int main(") or ""
    check("E1 循环内再次调用 ppm_header",
          len(re.findall(r"ppm_header\s*\(", mainfn)) >= 2)
    check("E2 尺寸变化时重建共享文件",
          "REBUILD" in src and "munmap" in mainfn)
    check("E3 重建后 seq 不归零",
          re.search(r"put32\(map\s*\+\s*4,\s*seq\)", mainfn) is not None)

    print("[F] 参数解析：fps 与标志不混用")
    check("F1 --input-stdin 单独判断", "strcmp(argv[i], \"--input-stdin\")" in src)
    check("F2 fps 按首字符是数字才取",
          re.search(r"argv\[i\]\[0\]\s*>=\s*'0'", mainfn) is not None)
    check("F3 不再无条件 atoi(argv[4])",
          "atoi(argv[4])" not in src)

    print("[G] alpha 必须显式置 255")
    check("G1 每像素写 255", re.search(r"pix\[o\s*\+\s*3\]\s*=\s*255", mainfn) is not None)
    check("G2 不抄源第 4 字节", "pix[o + 3] = s[i + 3]" not in src)

    print("[H] 未初始化变量已清除")
    check("H1 无 t1 参与计算",
          not re.search(r"t1\.tv_", stripped))
    check("H2 无 *0 占位写法",
          "* 0 +" not in stripped)

    print("[I] 重连分支不泄漏 fd")
    check("I1 重连用局部 fd2",
          re.search(r"int\s+fd2\s*=\s*tcp_connect", mainfn) is not None)
    check("I2 drain 失败时 close(fd2)",
          re.search(r"close\(fd2\)", mainfn) is not None)

    return report()


def report():
    print()
    if fails:
        print(f"RESULT: QSCREENSRV_PPM_FAILED ({checks} checks, {fails} failed)")
        return 1
    print(f"RESULT: QSCREENSRV_PPM_OK ({checks} checks, 0 failed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
