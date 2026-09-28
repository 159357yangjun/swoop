#!/usr/bin/env python3
"""定时下载对话框「日期框接缝」测量工具（QSS 二分定位用）。

背景：QSS 只要给 QDateTimeEdit 声明 border，字段内右侧就会多出一条 1px 深色竖线 +
底边右端一段 16px 深色横线，拼成直角 L。它是 base style 给「自旋按钮区」画的凸起浮雕边
（亮边在左上，白底看不见；暗边落在右下），跟 QSS 圆角边框不同源。这个脚本用来量化它，
改 QSS 前后各跑一次就知道有没有真的消掉。

思路：不靠固定坐标，先用 IDM_DBG_SCHED 打出 QDateTimeEdit / QGroupBox 的几何，再在
字段右内侧、下内侧找深色像素行程 —— 换样式表导致布局位移也能正确测量。

用法:
  python tools/seam_probe.py "<标签>" "<QSS>" [--replace]
  python tools/seam_probe.py --batch "<标签>::<QSS>" "<标签2>::<QSS2>" ... [--replace]

输出里关注两个数：
  竖线(右内侧最深一列)  —— 接缝在时约为字段内高，修掉后应降到 0/个位数
  横线(底边最长一截)    —— 接缝在时约 16px，修掉后应降到 0/个位数
注意日历图标本身也是深色像素，所以脚本把右侧 8 列的计数逐列打出来，便于分辨哪列才是接缝。
"""
import os
import re
import shutil
import subprocess
import sys

from PIL import Image

EXE = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "..", "build", "ui_snapshot.exe"))
SHOTDIR = os.path.abspath(os.path.join(os.path.dirname(EXE), "seam-probe-shots"))
TASKS = os.path.join(os.environ["LOCALAPPDATA"], "IDM Next", "tasks.json")

DARK_MARGIN = 30     # 比字段底色暗这么多才算「接缝像素」——绝对值在暗色主题下会失效
SHOT = "03b_schedule_%s.png"     # %s = 主题（light / dark，由 IDM_PROBE_THEME 控制）


def measure(tag, qss, replace=False, style=None):
    """跑一次走查、量一次，返回 (geometry, 右侧各列暗像素数, 底边最长暗行程)。"""
    theme = os.environ.get("IDM_PROBE_THEME", "light")
    os.makedirs(SHOTDIR, exist_ok=True)
    for f in os.listdir(SHOTDIR):
        p = os.path.join(SHOTDIR, f)
        # ⚠️ 目录必须走 rmtree：ui_snapshot 在 portable 模式下会在截图目录里建一个
        # _data 数据目录，而 os.remove 对目录在 Windows 上抛的是 PermissionError
        # （不是 IsADirectoryError）—— 于是「连跑两次」第二次必崩，暗色基线永远测不了。
        # 基线工具必须能重复跑，这里连它一并清掉（ui_snapshot 每次都会重建它）。
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
        else:
            os.remove(p)

    # 走查工具会往真实数据目录写 tasks.json，先备份、跑完还原（绝不污染用户任务）
    backup = None
    if os.path.exists(TASKS):
        with open(TASKS, "rb") as fh:
            backup = fh.read()
    try:
        out = _run_once(qss, replace, style, theme)
    finally:
        if backup is not None:
            with open(TASKS, "wb") as fh:
                fh.write(backup)

    shot = os.path.join(SHOTDIR, SHOT % theme)
    if not os.path.exists(shot):
        print(f"{tag:34s} 未产出截图")
        return
    im = Image.open(shot).convert("L")
    px = im.load()
    W, H = im.size

    geom = _grep(out, r"QDateTimeEdit obj=\[\] geo=\((-?\d+),(-?\d+) (\d+)x(\d+)\)")
    off = _grep(out, r"QGroupBox obj=\[\] geo=\((-?\d+),(-?\d+) ")
    if geom is None or off is None:
        print(f"{tag:34s} 尺寸={W}x{H} 没抓到几何（QDateTimeEdit / QGroupBox）")
        return
    gx, gy, gw, gh = geom
    x0, y0 = gx + off[0], gy + off[1]

    # 底色取字段内部出现最多的亮度值（文字/图标像素是少数派，不影响众数）。
    # 用相对判定而不是绝对阈值：暗色主题下底色本身就比 170 还暗。
    # 扫描带也排除最外面那圈边框（QSS 边框色和底色本来就有差），只看边框内侧。
    from collections import Counter
    inner = [px[x, y] for y in range(y0 + 4, y0 + gh - 4) for x in range(x0 + 4, x0 + gw - 4)]
    bg = Counter(inner).most_common(1)[0][0] if inner else 255

    def off(x, y):
        return 0 <= x < W and 0 <= y < H and abs(px[x, y] - bg) > DARK_MARGIN

    cols = [(x, sum(1 for y in range(y0 + 1, y0 + gh - 1) if off(x, y)))
            for x in range(x0 + gw - 6, x0 + gw - 1)]
    rows = [(y, sum(1 for x in range(x0 + 1, x0 + gw - 1) if off(x, y)))
            for y in range(y0 + gh - 6, y0 + gh - 1)]
    best_v = max(cols, key=lambda t: t[1])
    best_h = max(rows, key=lambda t: t[1])

    verdict = "接缝在" if (best_v[1] > gh * 0.5 and best_h[1] >= 12) else "已清掉"
    print(f"{tag:34s} 字段 {gw}x{gh} @({x0},{y0}) 底色={bg:3d}  "
          f"竖线 x={best_v[0]} {best_v[1]:3d}/{gh}  横线 y={best_h[0]} {best_h[1]:3d}/{gw}  [{verdict}]")
    print(f"{'':34s}   右侧逐列(x:异色像素数): "
          + " ".join(f"{x}:{n}" for x, n in cols))
    print(f"{'':34s}   底边逐行(y:异色像素数): "
          + " ".join(f"{y}:{n}" for y, n in rows))
    return verdict


def _grep(text, pattern):
    m = re.search(pattern, text)
    return tuple(int(g) for g in m.groups()) if m else None


def _run_once(qss, replace=False, style=None, theme="light"):
    env = dict(os.environ)
    env.update({
        "QT_QPA_PLATFORM": "offscreen",
        "QT_QPA_FONTDIR": "C:/Windows/Fonts",
        "IDM_SHOT_DIR": SHOTDIR,
        "IDM_THEME": theme,
        "IDM_DBG_SCHED": "1",
        "IDM_EXTRA_QSS": qss,
    })
    if replace:
        env["IDM_QSS_REPLACE"] = "1"
    if style:
        env["QT_STYLE_OVERRIDE"] = style
    # ⚠️ 必须带 CREATE_NO_WINDOW：ui_snapshot 是控制台子系统程序（CMake 里没设
    #    WIN32_EXECUTABLE），不加这个标志的话每跑一次都会弹一个黑窗口 —— 批量量测
    #    几十次就是几十个窗口闪屏，用户会以为 AI 挂了常驻命令行没关。
    p = subprocess.run([EXE], env=env, capture_output=True, text=True, errors="replace",
                       creationflags=subprocess.CREATE_NO_WINDOW)
    return p.stdout + p.stderr


if __name__ == "__main__":
    argv = sys.argv[1:]
    replace = "--replace" in argv
    argv = [a for a in argv if a != "--replace"]
    if not argv:
        print(__doc__)
        sys.exit(2)
    if argv[0] == "--batch":
        for item in argv[1:]:
            label, _, qss = item.partition("::")
            measure(label, qss, replace)
    else:
        style = os.environ.get("IDM_STYLE")
        measure(argv[0], argv[1] if len(argv) > 1 else "", replace, style)
