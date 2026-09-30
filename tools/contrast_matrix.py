#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对比度矩阵：每个前景色对它**可能出现的所有底色**取最差值。

为什么不是"一行一个比值"：2026-09-30 我把 dark 主按钮的填充换成深一档蓝，
只报了「对窗体底 #18191c = 3.74 ≥3」就交了；而面板实际有七种底，
同一个蓝对 `#2a3a55`（选中行底）只有 **2.44** —— 白字清楚了，按钮边界看不清了。
一行一个比值的表**允许这种挑底色报**，所以判据本身要改。

底色集合从 QSS 枚举生成，不手写：`background` / `background-color` 的十六进制值全部收进来。

夹具（永久）：`--verify` 必须报出那个被否决的蓝 `#2f6fe0` 的最差值 < 3.0，
否则说明本工具退化成了"只看最暗一块"，直接 exit 1。
用法：python tools/contrast_matrix.py [--verify]
"""
import re
import sys

QSS = {"dark": "resources/qss/dark.qss", "light": "resources/qss/light.qss"}
BLOCK = re.compile(r"([^{}]+)\{([^{}]*)\}", re.S)
BG = re.compile(r"(?<![\w-])background(?:-color)?\s*:\s*(#[0-9a-fA-F]{6})")
FG = re.compile(r"(?<![\w-])color\s*:\s*(#[0-9a-fA-F]{6})")
TEXT_MIN, GRAPHIC_MIN = 4.5, 3.0
REJECTED_FILL = "2f6fe0"          # 永久夹具：它必须被报成不达标


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def lum(h):
    r, g, b = [v / 255.0 for v in rgb(h)]
    f = lambda c: c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b)


def ratio(a, b):
    x, y = lum(a), lum(b)
    hi, lo = max(x, y), min(x, y)
    return (hi + 0.05) / (lo + 0.05)


def panels(txt):
    seen = []
    for v in BG.findall(txt):
        v = v.lower()
        if v not in seen:
            seen.append(v)
    return seen


def main(argv):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    verify = "--verify" in argv
    bad = []
    for theme, path in QSS.items():
        try:
            txt = open(path, encoding="utf-8").read()
        except OSError as e:
            sys.stderr.write("读不了 %s：%s\n" % (path, e))
            return 2
        ps = panels(txt)
        fgs = sorted(set(v.lower() for v in FG.findall(txt)))
        print("[%s] 底色 %d 种（枚举自 QSS）：%s" % (theme, len(ps), " ".join(ps)))
        print("       前景 %d 种，逐个对它可能出现的所有底色取最差：" % len(fgs))
        rows = []
        for fg in fgs:
            worst_p, worst_r = min(((p, ratio(fg, p)) for p in ps), key=lambda kv: kv[1])
            rows.append((worst_r, fg, worst_p))
        for worst_r, fg, worst_p in sorted(rows):
            tag = "OK" if worst_r >= TEXT_MIN else ("图形线过/文本不过" if worst_r >= GRAPHIC_MIN else "两条都不过")
            print("    %-9s 最差 %5.2f:1 (on %s)  %s" % (fg, worst_r, worst_p, tag))
            # 只打印、不判失败：「所有底色」是**上界**，其中很多配对不可能出现
            # （例如新改的按钮文字 #18191c 只会落在强调色填充上，不会落在 #18191c 窗体上）。
            # 要按「可能出现」判，需要先有控件→表面的包含关系模型，本工具还没建。
            pass
        # 永久夹具：被否决的那个填充色**不依赖它还在不在文件里**，每次都拿当前面板集合算一遍。
        # 它必须仍然 <3.0 —— 一旦这条报成达标，说明底色集合缩水了（比如又只剩最暗那块）。
        rej = "#" + REJECTED_FILL
        worst_p, worst_r = min(((p, ratio(rej, p)) for p in ps), key=lambda kv: kv[1])
        print("    [夹具] 被否决的填充 %s 对最差面板 = %.2f:1（要求 <3.0 才算判据没退化，on %s）"
              % (rej, worst_r, worst_p))
        if verify and worst_r >= GRAPHIC_MIN:
            bad.append("夹具失效：%s 的最差值被算成 %.2f（应 <3.0），底色集合只剩 %d 种"
                       % (rej, worst_r, len(ps)))
        # 注：这里**不**判「填充色 vs 所有底色」。WCAG 1.4.11 的对象是「控件对它所在的那块表面」，
        # 不是任意两块面板之间 —— 相邻底色差 1.01:1 是刻意的层次，拿 3.0 去卡会造出一堆假缺陷
        # （第一版就报了 16 条 light 的「边界不达标」，全是相邻表面）。
        # 要判按钮边界，得先有「哪个控件落在哪些表面上」的包含关系模型，本工具没建，
        # 所以只对**文本前景**取所有底色的最差值（保守上界），其余留给人看。
    if verify:
        if bad:
            print("VERIFY 失败：")
            for b in bad:
                print("   " + b)
            return 1
        print("VERIFY 通过：夹具 #%s 的最差值仍 <3.0，判据没退化" % REJECTED_FILL)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
