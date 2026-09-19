#!/usr/bin/env python3
"""界面走查用：裁剪 + 放大截图的局部区域，方便逐像素检查一块小区域。

用法:
    python tools/img_crop.py <输入.png> <输出.png> x,y,w,h [缩放倍数] [还要拼在后面的图...]

    # 两块区域并排对比（左=改前，右=改后）
    python tools/img_crop.py before.png cmp.png 0,90,230,610 2 --right after.png

为什么需要: 整窗截图缩在聊天里看不清细节（徽标、圆角、1px 接缝），
而把 PNG 放大又需要图像库——装了 Pillow 的 venv 里跑这一个脚本就够，
不必为了看一眼界面去引依赖。

依赖: Pillow（托管 venv 里已有；无需在本机全局安装）
"""
import sys


def main() -> int:
    try:
        from PIL import Image
    except ImportError:
        print("[img_crop] 需要 Pillow：用托管 venv 的 python 跑本脚本", file=sys.stderr)
        return 2

    if len(sys.argv) < 4:
        print(__doc__)
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    try:
        x, y, w, h = (int(v) for v in sys.argv[3].split(","))
    except ValueError:
        print("[img_crop] 区域格式应为 x,y,w,h（整数）", file=sys.stderr)
        return 2
    scale = float(sys.argv[4]) if len(sys.argv) > 4 and not sys.argv[4].startswith("-") else 1.0

    right = None
    if "--right" in sys.argv:
        right = sys.argv[sys.argv.index("--right") + 1]
    below = None
    if "--below" in sys.argv:
        below = sys.argv[sys.argv.index("--below") + 1]

    def crop(path):
        im = Image.open(path).convert("RGB")
        box = (max(0, x), max(0, y), min(im.width, x + w), min(im.height, y + h))
        im = im.crop(box)
        if scale != 1.0:
            im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
        return im

    left = crop(src)
    if right:
        r = crop(right)
        canvas = Image.new("RGB", (left.width + r.width + 8, max(left.height, r.height)), (255, 0, 255))
        canvas.paste(left, (0, 0))
        canvas.paste(r, (left.width + 8, 0))
        canvas.save(dst)
        print(f"[img_crop] {dst}  ({canvas.width}x{canvas.height}) 左={src} 右={right}")
    elif below:
        b = crop(below)
        canvas = Image.new("RGB", (max(left.width, b.width), left.height + b.height + 8), (255, 0, 255))
        canvas.paste(left, (0, 0))
        canvas.paste(b, (0, left.height + 8))
        canvas.save(dst)
        print(f"[img_crop] {dst}  ({canvas.width}x{canvas.height}) 上={src} 下={below}")
    else:
        left.save(dst)
        print(f"[img_crop] {dst}  ({left.width}x{left.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
