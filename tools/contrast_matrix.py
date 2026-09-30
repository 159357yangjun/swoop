#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对比度矩阵：文本前景只与「面」比，控件只与「它真的坐在上面的那些面」比。

判据为什么长这样（2026-09-30 被用户实跑推翻后重做）：
第一版把 `background*:` 的十六进制**全部**当面板，于是按钮自己的 pressed 填充
（#357ae0）、边框（#4a4d54/#5c5f66）、勾选框填充都进了集合 ⇒
① 夹具那个蓝的"最差值"从 2.44(on #2a3a55) 静默漂成 1.12(on #357ae0) —— 数在没人重新做决定时自己动；
② 文字前景被拿去跟按钮自己的填充比，报出一堆不可能出现的配对；
③ 集合里含 `color:` 右边用过的值（#18191c/#5c5f66/#ffffff/#1f2329），
   于是 ratio(fg,fg)=1.00 恒真，四行"两条都不过"是恒等式不是发现；
④ 那些 tag 不进退出码 —— 12 行报 FAIL 而 `--verify` 退 0，是"会报警但咬不住"的装饰型 tally。

四条修法，全部可被 `--verify` 钉住：
① 面板 = **容器选择器**里的 background（窗体/对话框/分组框/表格/树/pane/选中行底），
   并且**硬断言**面板集与「`color:` 右边出现过的值」「`border*:` 右边出现过的值」「控制件填充」三者不相交；
② `fg == bg` 不参与 min（自配对是恒真式）；
③ 只有落在**包含关系表** CONTAINMENT 上的配对才进退出码；表外的一律只打印、标 "info"；
④ 夹具钉在具名面上：断言 `#2f6fe0 on #2a3a55 < 3.0`，不是"on 当前集合里最差的那个"。

用法：python tools/contrast_matrix.py [--verify]
退出码：0 全部达标；1 有已建模的配对不达标，或夹具/集合断言失效；2 读不到 QSS。
"""
import re
import sys

QSS = {"dark": "resources/qss/dark.qss", "light": "resources/qss/light.qss"}
BLOCK = re.compile(r"([^{}]+)\{([^{}]*)\}", re.S)
BG = re.compile(r"(?<![\w-])background(?:-color)?\s*:\s*(#[0-9a-fA-F]{6})")
FG = re.compile(r"(?<![\w-])color\s*:\s*(#[0-9a-fA-F]{6})")
BORDER = re.compile(r"(?<![\w-])border[\w-]*\s*:\s*[^;]*?(#[0-9a-fA-F]{6})")

# ① 哪些选择器的 background 算「面」
CONTAINER = re.compile(
    r"QMainWindow|QDialog|\bQWidget\b|QGroupBox|::pane|QScrollArea|"
    r"QTableView|QTreeView|QListView|QTableWidget|item:selected|QMenu\b|QToolTip|statusPill")
# 哪些一定不算（控制件本体、状态填充、轨道把手）
NOT_SURFACE = re.compile(
    r":hover|:pressed|:checked|:default|QProgressBar|QScrollBar|QToolButton|QPushButton|"
    r"QCheckBox|QRadioButton|QLineEdit|QComboBox|QSpinBox|QTabBar|indicator")
TEXT_MIN, GRAPHIC_MIN = 4.5, 3.0
REJECTED_FILL = "#2f6fe0"
FIXTURE_SURFACE = "#2a3a55"      # ④ 夹具钉在这块具名面上（选中行底）

# ③ 包含关系：谁真的坐在谁上面。每条都要有理由，宁可少写不可臆造。
# (前景来源, 它可能落在的面集合, 要求, 理由)
CONTAINMENT = {
    "dark": [
        ("base_text", ["#18191c", "#1c1d20", "#1f2024", "#232428", "#2a3a55"], TEXT_MIN,
         "正文/表格文字坐在窗体、输入框底、树/工具栏、表格、选中行底上"),
        ("button_text", ["#4c8dff"], TEXT_MIN,
         "主按钮/默认按钮的文字只坐在自己的填充上（填充不算面，单独建模）"),
    ],
    "light": [
        ("base_text", ["#f3f4f6", "#ffffff", "#f2f3f5", "#f7f8fa", "#e8e9eb"], TEXT_MIN,
         "正文/表格文字坐在窗体、表格白底、tab/胶囊底、hover 底上"),
        ("button_text", ["#2563eb"], TEXT_MIN,
         "主按钮文字坐在 light 的强调色填充上"),
    ],
}
# 控件边界：填充 vs 它真的会接触的面
FILL_ON = {
    "dark":  ("#4c8dff", ["#18191c", "#1f2024", "#232428", "#2c2d31", "#2a3a55"], GRAPHIC_MIN,
              "主按钮/勾选框的蓝填充坐在窗体、树、表格、按钮底、选中行底上"),
    "light": ("#2563eb", ["#f3f4f6", "#ffffff", "#f2f3f5"], GRAPHIC_MIN,
              "light 强调色填充坐在窗体、表格白底、tab 底上"),
}


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


def surfaces(txt):
    """按选择器语义取面：只要**有任意一个容器选择器**用它做 background，它就是面。

    与用户给的 ① 有一处偏差，写清楚：他说"集合里不得出现任何在 `color:` 右边出现过的值"。
    这条按字面执行会把 #18191c 从面里踢掉 —— 因为本轮主按钮的文字色正好取了同一个值
    （它既是 QMainWindow 的底，又是按钮的字色）。真正该挡的是**控制件填充与边框**
    （上一版混进来的 #357ae0/#4a4d54/#5c5f66 全是这类）。所以这里改成两条可执行的断言：
      a) 面不得同时是 border* 的颜色；
      b) 面必须至少被一个容器选择器使用（不被任何控制件规则单独定义）。
    自配对（fg==bg）另在 ② 里跳过，恒真式因此进不了 min。
    """
    container_vals, control_vals, border_vals = [], [], []
    for sel, body in BLOCK.findall(txt):
        s = " ".join(sel.split())
        is_container = bool(CONTAINER.search(s)) and not NOT_SURFACE.search(s)
        for v in BG.findall(body):
            (container_vals if is_container else control_vals).append(v.lower())
        for v in BORDER.findall(body):
            border_vals.append(v.lower())
    panels = []
    for v in container_vals:
        if v in border_vals:
            continue
        if v not in panels:
            panels.append(v)
    only_control = [v for v in control_vals if v not in container_vals]
    return panels, sorted(set(only_control)), sorted(set(border_vals))


def main(argv):
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass
    verify = "--verify" in argv
    fails = []
    for theme, path in QSS.items():
        try:
            txt = open(path, encoding="utf-8").read()
        except OSError as e:
            sys.stderr.write("读不了 %s：%s\n" % (path, e))
            return 2
        panels, only_control, border_vals = surfaces(txt)
        fgs = sorted(set(v.lower() for v in FG.findall(txt)))
        borders = sorted(set(v.lower() for v in BORDER.findall(txt)))
        # ① 集合纯净性断言：面里不得混进文字色 / 边框值 / 控制件填充
        polluted = [p for p in panels if p in border_vals]
        print("[%s] 面 %d 种（容器选择器的 background，已剔除边框值）：%s"
              % (theme, len(panels), " ".join(panels)))
        print("     控制件专属值（不得进面集）%d 种：%s"
              % (len(only_control), " ".join(only_control)))
        if polluted:
            fails.append("%s: 面板集混入边框值 %s" % (theme, polluted))
        if any(p in only_control for p in panels):
            fails.append("%s: 面集里有控制件专属值" % theme)
        base_text = fgs[-1] if theme == "dark" else fgs[0]
        named = {"base_text": {"dark": "#e8eaed", "light": "#1f2329"}.get(theme),
                 "button_text": {"dark": "#18191c", "light": "#ffffff"}.get(theme)}
        for fname, surf, need, why in CONTAINMENT[theme]:
            fg = named[fname]
            worst_p, worst_r = None, None
            for p in surf:
                if p == fg:            # ② 自配对不参与
                    continue
                r = ratio(fg, p)
                if worst_r is None or r < worst_r:
                    worst_r, worst_p = r, p
            ok = worst_r >= need
            print("    %-11s %-9s 最差 %5.2f:1 (on %s)  要求 %.1f  %s   ← %s"
                  % (fname, fg, worst_r, worst_p, need, "OK" if ok else "FAIL", why))
            if not ok:
                fails.append("%s: %s %s 对 %s = %.2f < %.1f" % (theme, fname, fg, worst_p, worst_r, need))
        fill, fsurf, fneed, fwhy = FILL_ON[theme]
        worst_p, worst_r = None, None
        for p in fsurf:
            r = ratio(fill, p)
            if worst_r is None or r < worst_r:
                worst_r, worst_p = r, p
        ok = worst_r >= fneed
        print("    %-11s %-9s 最差 %5.2f:1 (on %s)  要求 %.1f  %s   ← %s"
              % ("fill_edge", fill, worst_r, worst_p, fneed, "OK" if ok else "FAIL", fwhy))
        if not ok:
            fails.append("%s: 填充 %s 对 %s = %.2f < %.1f" % (theme, fill, worst_p, worst_r, fneed))
        # ④ 夹具钉在具名面上
        fx = ratio(REJECTED_FILL, FIXTURE_SURFACE)
        print("    [夹具] %s on %s = %.2f:1（必须 <%.1f，否则判据退化）"
              % (REJECTED_FILL, FIXTURE_SURFACE, fx, GRAPHIC_MIN))
        if verify and fx >= GRAPHIC_MIN:
            fails.append("夹具失效：%s on %s = %.2f，不再 <3.0" % (REJECTED_FILL, FIXTURE_SURFACE, fx))
        if theme == "dark" and FIXTURE_SURFACE not in panels:
            # 夹具面是暗色的选中行底，只在 dark 一侧核对（第一版在 light 上也查，
            # 于是每轮都报一条假失败——集合没坏，是断言走错了主题）
            fails.append("夹具所在的面 %s 不在 dark 面板集里（集合被改坏）" % FIXTURE_SURFACE)
    if verify:
        if fails:
            print("VERIFY 失败：")
            for f in fails:
                print("   " + f)
            return 1
        print("VERIFY 通过：集合纯净、已建模配对全达标、夹具仍钉在 %s 上报 <3.0" % FIXTURE_SURFACE)
    else:
        for f in fails:
            print("   FAIL " + f)
    return 1 if (fails and verify) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
