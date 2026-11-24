#!/usr/bin/env python3
"""Convert a PNG image to a 1-bit C array include file for ST7305 bitmap."""
import sys
from PIL import Image


def image_to_1bit(src_path, dst_path, width, height, var_name="s_sponsor_img",
                threshold=128, invert=False):
    """Convert image to 1-bit MSB-left packed rows.
    
    默认用简单阈值二值化(非抖动), 适合二维码/线条图, 保持边缘锐利。
    极性: ST7305 的 st7305_draw_bitmap_1bit 中 bit=1 画黑像素, 故此处
    源图黑(暗) -> bit=1 -> 屏上黑; 源图白(亮) -> bit=0 -> 屏上白(清屏底色)。
    invert=True 时翻转极性(黑变白/白变黑), 用于某些反相显示的屏。
    """
    img = Image.open(src_path)
    # 先转灰度, 再做无抖动阈值二值化
    img = img.convert("L")
    # 保持宽高比缩放到目标内接尺寸(长边填满)
    img.thumbnail((width, height), Image.Resampling.LANCZOS)
    # 居中粘贴到目标尺寸的白色画布(留出黑边/白边保持原图比例)
    canvas = Image.new("L", (width, height), 255)
    ox = (width - img.width) // 2
    oy = (height - img.height) // 2
    canvas.paste(img, (ox, oy))
    img = canvas

    # ST7305 极性: 源图黑(暗, <threshold) 应输出 bit=1(屏画黑)
    #   pixel<threshold -> 1, pixel>=threshold -> 0
    # invert=True 时反过来。
    bytes_per_row = (width + 7) // 8
    data = []
    pixels = img.load()
    for y in range(height):
        row = []
        for x in range(width):
            dark = pixels[x, y] < threshold
            bit = (not dark) if invert else dark
            row.append(1 if bit else 0)

        # Pack bits, MSB on the left (bit=1 -> black pixel -> ST7305 draw pixel)
        for b in range(bytes_per_row):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < width and row[x]:
                    byte |= (1 << (7 - bit))
            data.append(byte)

    # Write .inc file
    total_bytes = len(data)
    lines = [
        f"/* Auto-generated from {src_path} */",
        f"#define SPONSOR_IMG_W {width}",
        f"#define SPONSOR_IMG_H {height}",
        f"#define SPONSOR_IMG_BYTES {total_bytes}",
        "",
        f"static const uint8_t {var_name}[{total_bytes}] = {{",
    ]
    # 12 bytes per line
    for i in range(0, total_bytes, 12):
        vals = [f"0x{b:02X}" for b in data[i:i + 12]]
        lines.append("    " + ", ".join(vals) + ",")
    lines[-1] = lines[-1].rstrip(",")
    lines.append("};")
    lines.append("")

    with open(dst_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"Wrote {dst_path}: {width}x{height}, {total_bytes} bytes")


if __name__ == "__main__":
    # 参数: <input.png> <output.inc> [width] [height] [--invert]
    args = sys.argv[1:]
    invert = "--invert" in args
    args = [a for a in args if a != "--invert"]
    if len(args) < 2:
        print("Usage: png_to_1bit_inc.py <input.png> <output.inc> [width] [height] [--invert]")
        sys.exit(1)
    src = args[0]
    dst = args[1]
    w = int(args[2]) if len(args) > 2 else 300
    h = int(args[3]) if len(args) > 3 else 300
    image_to_1bit(src, dst, w, h, invert=invert)
