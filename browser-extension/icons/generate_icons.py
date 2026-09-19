from PIL import Image, ImageDraw, ImageFont
import os

# 输出目录
out_dir = r'D:\visual studio\lianxi\MFC\idm-next\browser-extension\icons'
os.makedirs(out_dir, exist_ok=True)

# 颜色
bg_color = (30, 120, 220)       # 主蓝色
accent_color = (255, 255, 255)  # 白色箭头/文字
shadow_color = (20, 90, 170)

sizes = [16, 48, 128]

for size in sizes:
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    # 圆角矩形背景
    radius = max(size // 8, 2)
    draw.rounded_rectangle([0, 0, size-1, size-1], radius=radius, fill=bg_color, outline=shadow_color, width=max(1, size//32))
    
    # 下载箭头（向下）
    margin = size // 5
    shaft_width = max(size // 8, 2)
    head_size = size // 4
    
    cx = size // 2
    top_y = margin
    bottom_y = size - margin - head_size
    
    # 箭杆
    draw.line([(cx, top_y), (cx, bottom_y)], fill=accent_color, width=shaft_width)
    # 箭头头部
    draw.polygon([
        (cx - head_size, bottom_y - head_size // 2),
        (cx + head_size, bottom_y - head_size // 2),
        (cx, bottom_y + head_size // 2)
    ], fill=accent_color)
    
    img.save(os.path.join(out_dir, f'icon{size}.png'))
    print(f'Created icon{size}.png ({size}x{size})')

print('Done')
