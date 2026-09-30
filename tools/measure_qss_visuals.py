#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""只读：从 resources/qss/*.qss 量出「节奏 / 圆角 / 字体 / 对比度」四组数字。
不写任何文件、不改任何东西；输出就是视觉基线表的数值来源。

为什么量 QSS 而不是量截图像素：间距/圆角/字号/颜色的**真源**是这两份样式表，
截图只能证明"渲染出来了"，证明不了"值是多少"。两边都要，这一步先拿真源。
"""
import re
import sys
import collections

FILES = ["resources/qss/light.qss", "resources/qss/dark.qss"]
PX = re.compile(r"(-?\d+(?:\.\d+)?)px")
RADIUS = re.compile(r"border(?:-(?:top|bottom)-(?:left|right))?-radius\s*:\s*([^;]+);")
FONT = re.compile(r"font-(family|size|weight)\s*:\s*([^;]+);")
SPACING = re.compile(r"(padding|margin)(?:-(?:top|right|bottom|left))?\s*:\s*([^;]+);")
BLOCK = re.compile(r"([^{}]+)\{([^{}]*)\}", re.S)
COLOR = re.compile(r"(?<![\w-])(#[0-9a-fA-F]{3,8}|\b(?:white|black)\b)")
PROP_COLOR = re.compile(r"(?<![\w-])(color|background-color|background)\s*:\s*([^;]+);")


def lum(rgb):
    def ch(c):
        c /= 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = rgb
    return 0.2126 * ch(r) + 0.7152 * ch(g) + 0.0722 * ch(b)


def hexrgb(h):
    h = h.strip().lstrip("#")
    if len(h) == 3:
        h = "".join(c * 2 for c in h)
    if len(h) == 8:      # #RRGGBBAA：Qt 支持，取 RGB，Alpha 单独报
        h = h[:6]
    if len(h) != 6:
        return None
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def ratio(fg, bg):
    a, b = lum(fg), lum(bg)
    hi, lo = max(a, b), min(a, b)
    return (hi + 0.05) / (lo + 0.05)


NAMED = {"white": (255, 255, 255), "black": (0, 0, 0)}


def parse(v):
    v = v.strip().lower()
    if v in NAMED:
        return NAMED[v]
    m = re.search(r"#[0-9a-fA-F]{3,8}", v)
    return hexrgb(m.group(0)) if m else None


def force_utf8():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def main():
    force_utf8()
    print("QSS 视觉量测（只读）")
    for f in FILES:
        try:
            txt = open(f, encoding="utf-8").read()
        except OSError as e:
            print("  读不了 %s：%s" % (f, e))
            continue
        theme = "dark" if "dark" in f else "light"
        spacing_vals = collections.Counter()
        for sel, body in BLOCK.findall(txt):
            for kind, val in SPACING.findall(body):
                for num in PX.findall(val):
                    spacing_vals[int(float(num))] += 1
        off = {v: c for v, c in spacing_vals.items() if v % 4 != 0}
        print("\n[%s] %s" % (theme, f))
        print("  ② 间距：用到的 px 值 = %s" % sorted(spacing_vals))
        print("     落在 4/8 阶梯上的 = %d 种 / 共 %d 种；**不在阶梯上**的 = %s（合计 %d 处）"
              % (len(spacing_vals) - len(off), len(spacing_vals),
                 sorted(off), sum(off.values())))
        rad = collections.Counter()
        for _, body in BLOCK.findall(txt):
            for val in RADIUS.findall(body):
                for num in PX.findall(val):
                    rad[int(float(num))] += 1
        print("  ② 圆角：值集合 = %s（每种出现次数 %s）→ 全页圆角种类数 %d（尺子要求 ≤2）"
              % (sorted(rad), dict(sorted(rad.items())), len(rad)))
        fam = collections.Counter()
        sz = collections.Counter()
        wt = collections.Counter()
        for _, body in BLOCK.findall(txt):
            for kind, val in FONT.findall(body):
                if kind == "family":
                    fam[val.strip()] += 1
                elif kind == "size":
                    sz[PX.findall(val)[0] if PX.findall(val) else val.strip()] += 1
                else:
                    wt[val.strip()] += 1
        print("  ③ font-family 声明 = %s" % dict(fam))
        print("     font-size 值 = %s；font-weight 值 = %s" % (dict(sorted(sz.items())), dict(wt)))

        # ⑥ 对比度：同一块里同时给了 color 与 background(-color) 的，按字面值算
        pairs = []
        for sel, body in BLOCK.findall(txt):
            fg = bg = None
            for prop, val in PROP_COLOR.findall(body):
                rgb = parse(val)
                if rgb is None:
                    continue
                if prop == "color":
                    fg = rgb
                else:
                    bg = rgb
            if fg and bg:
                pairs.append((sel.strip().replace("\n", " ")[:44], fg, bg, ratio(fg, bg)))
        pairs.sort(key=lambda p: p[3])
        print("  ⑥ 可算的「前景+同块背景」配对 %d 组，最差 6 组：" % len(pairs))
        for sel, fg, bg, r in pairs[:6]:
            verdict = "正文需≥4.5" if r < 4.5 else ("图标/控件需≥3" if r < 3 else "过")
            print("     %-44s fg=%s bg=%s 比值=%.2f  %s"
                  % (sel, "#" + "%02x%02x%02x" % fg, "#" + "%02x%02x%02x" % bg, r, verdict))
        print("     注：只覆盖「同一选择器里两个值都写死」的情况；继承来的背景色、"
              "自绘控件（进度条/徽章/图标）不在此列，得另外量。")


if __name__ == "__main__":
    sys.exit(main())
