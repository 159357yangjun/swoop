#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对比度门：配对**从 QSS 块派生**，不硬编码。

为什么重写成这样（2026-09-30，用户实跑抓出）：上一版把 `button_text #18191c on #4c8dff`
这类配对**写死在表里**。主按钮换成「深填充 + 白字」之后，表里两行已经不属于主按钮了，
但那对值在盘上仍然存在（输入框选中块用的就是 `#18191c` + `#4c8dff`）⇒
「配对不存在就红」拦不住，工具继续报 OK。硬编码表一定会随 QSS 漂移，**所以让 QSS 自己说话**：
一个块里同时声明了 `color` 与 `background(-color)`（或 `selection-color` 与
`selection-background-color`），那一对就是真实的文字/底配对，配对名 = 该块的选择器。

判据（可执行、不发明）：
  GATE-A  每个「同块自配对」的文本对比度 ≥ 4.5；`:disabled` 块按 WCAG 豁免，但**逐条打印**，
          不让它静默消失。
  GATE-B  REQUIRED 里点名的块必须在 QSS 中存在，且必须真的产出它声称的那对值；
          找不到块 / 值变了 = 红。（用户要的「建模表每条要找得到自己的块」，只是表现在由文件生成）
  GATE-C  夹具：被否决的填充 #2f6fe0 对具名面 #2a3a55 必须仍 < 3.0。
  INFO    面集（容器选择器的 background）照旧打印，供人核集合有没有漂。

用法：python tools/contrast_matrix.py
退出码：0 全达标；1 有配对不达标 / REQUIRED 缺失或漂移 / 夹具退化；2 读不到文件。
"""
import re
import sys

QSS = {"dark": "resources/qss/dark.qss", "light": "resources/qss/light.qss"}
BLOCK = re.compile(r"([^{}]+)\{([^{}]*)\}", re.S)
HEX = r"#[0-9a-fA-F]{6}"
COLOR = re.compile(r"(?<![\w-])color\s*:\s*(%s)" % HEX)
BG = re.compile(r"(?<![\w-])background(?:-color)?\s*:\s*(%s)" % HEX)
SEL_FG = re.compile(r"selection-color\s*:\s*(%s)" % HEX)
SEL_BG = re.compile(r"selection-background-color\s*:\s*(%s)" % HEX)
BORDER = re.compile(r"(?<![\w-])border[\w-]*\s*:\s*[^;]*?(%s)" % HEX)
TEXT_MIN, GRAPHIC_MIN = 4.5, 3.0
# GATE-B：点名的块。判据是「这个选择器的块在不在、它自己声明的是不是这对值」。
REQUIRED = {
    "dark":  [("QToolButton#primaryToolBtn", "#ffffff", "#2f6fe0"),
              ("QPushButton:default", "#ffffff", "#2f6fe0")],
    "light": [("QToolButton#primaryToolBtn", "#ffffff", "#2563eb"),
              ("QPushButton:default", "#ffffff", "#2563eb")],
}
FIXTURE = ("#2f6fe0", "#2a3a55")      # 填充, 具名面
CONTAINER = re.compile(r"QMainWindow|QDialog|\bQWidget\b|QGroupBox|::pane|QScrollArea|"
                       r"QTableView|QTreeView|QListView|QTableWidget|item:selected|QMenu\b|QToolTip")
NOT_SURFACE = re.compile(r":hover|:pressed|:checked|:default|QProgressBar|QScrollBar|QToolButton|"
                         r"QPushButton|QCheckBox|QRadioButton|QLineEdit|QComboBox|QSpinBox|QTabBar|indicator")


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


def clean_sel(sel):
    return re.sub(r"/\*.*?\*/", "", sel, flags=re.S).strip().replace("\n", " ")


def pairs_of(txt):
    """派生 (选择器, 文字色, 底色)：**只有同一个块里声明的**才算一对。"""
    out = []
    for sel, body in BLOCK.findall(txt):
        s = clean_sel(sel)
        if not s:
            continue
        fg, bg = COLOR.findall(body), BG.findall(body)
        if fg and bg:
            out.append((s, fg[0].lower(), bg[0].lower()))
        sf, sb = SEL_FG.findall(body), SEL_BG.findall(body)
        if sf and sb:
            out.append((s + " [selection]", sf[0].lower(), sb[0].lower()))
    return out


def surfaces(txt):
    cont, borders = [], []
    for sel, body in BLOCK.findall(txt):
        s = clean_sel(sel)
        if CONTAINER.search(s) and not NOT_SURFACE.search(s):
            cont += [v.lower() for v in BG.findall(body)]
        borders += [v.lower() for v in BORDER.findall(body)]
    return [p for p in dict.fromkeys(cont) if p not in borders]


def main(argv):
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    fails = []
    for theme, path in QSS.items():
        try:
            txt = open(path, encoding="utf-8").read()
        except OSError as e:
            sys.stderr.write("读不了 %s：%s\n" % (path, e))
            return 2
        pairs = pairs_of(txt)
        print("[%s] %s 派生出同块自配对 %d 条（配对名 = 该块选择器）" % (theme, path, len(pairs)))
        for r, s, f, b in sorted((ratio(f, b), s, f, b) for s, f, b in pairs):
            exempt = ":disabled" in s
            print("    %5.2f:1  %-9s on %-9s  %-14s %s"
                  % (r, f, b, "豁免(disabled)" if exempt else ("OK" if r >= TEXT_MIN else "FAIL"), s[:60]))
            if r < TEXT_MIN and not exempt:
                fails.append("%s: %s on %s = %.2f < 4.5  块=%s" % (theme, f, b, r, s[:52]))
        for sel, fg, bg in REQUIRED[theme]:
            got = [(f, b) for s, f, b in pairs if sel in s and not s.endswith("[selection]")]
            if not got:
                fails.append("%s: REQUIRED 块在 QSS 里找不到 —— %s" % (theme, sel))
                print("    ⚠ REQUIRED 缺失：%s" % sel)
            elif not any(f == fg and b == bg for f, b in got):
                fails.append("%s: %s 块里的实际配对不是 %s on %s，而是 %s" % (theme, sel, fg, bg, got))
                print("    ⚠ REQUIRED 漂移：%s → %s" % (sel, got))
            else:
                print("    REQUIRED 命中：%s = %s on %s（%.2f:1）" % (sel, fg, bg, ratio(fg, bg)))
        panels = surfaces(txt)
        fx = ratio(FIXTURE[0], FIXTURE[1])
        print("    面 %d 种：%s" % (len(panels), " ".join(panels)))
        print("    [夹具] %s on %s = %.2f:1（必须 <%.1f）" % (FIXTURE[0], FIXTURE[1], fx, GRAPHIC_MIN))
        if fx >= GRAPHIC_MIN:
            fails.append("夹具退化：%s on %s = %.2f 不再 <3.0" % (FIXTURE[0], FIXTURE[1], fx))
        if theme == "dark" and FIXTURE[1] not in panels:
            # 夹具面是暗色的选中行底，只在 dark 一侧核（第一版在 light 也查，
            # 于是每轮都报一条假失败 —— 集合没坏，是断言走错了主题）
            fails.append("夹具所在的面 %s 不在 dark 面集里（集合漂了）" % FIXTURE[1])
    print()
    if fails:
        print("对比度门：FAIL（%d 条）" % len(fails))
        for f in fails:
            print("   " + f)
        return 1
    print("对比度门：通过（配对由 QSS 块派生，没有会过期的硬编码表）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
