import re
from PIL import Image

def rgb_to_gba(r, g, b):
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    return r5 | (g5 << 5) | (b5 << 10)

def parse_font8x8(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    clean = re.sub(r"//.*", "", text)
    clean = re.sub(r"/\*.*?\*/", "", clean, flags=re.DOTALL)
    m = re.search(r"font8x8_basic\[128\]\[8\]\s*=\s*\{([^;]+)\};", clean, re.DOTALL)
    if not m:
        raise ValueError("Could not find font8x8_basic in " + path)
    blocks = re.findall(r"\{([^}]+)\}", m.group(1))
    font_bytes = []
    for b in blocks:
        vals = [int(x.strip(), 0) for x in b.split(",") if x.strip()]
        if len(vals) == 8:
            font_bytes.append(vals)
    return font_bytes

import os

def generate():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    assets_dir = os.path.join(script_dir, "assets")

    # 1. Font -> 4bpp tiles (Charblock 0)
    font_raw = parse_font8x8(os.path.join(assets_dir, "font8x8_basic.h"))
    font_tiles = []
    for char_idx, rows in enumerate(font_raw):
        tbytes = []
        for y in range(8):
            row_byte = rows[y]
            # font8x8: bit 0 is leftmost pixel x=0, bit 7 is rightmost pixel x=7
            for x in range(0, 8, 2):
                p0 = 1 if (row_byte & (1 << x)) else 0
                p1 = 1 if (row_byte & (1 << (x + 1))) else 0
                tbytes.append((p1 << 4) | p0)
        font_tiles.append(tbytes)

    # 2. Tiles.png -> 4bpp tiles (Charblock 1)
    img_tiles = Image.open(os.path.join(assets_dir, "tiles.png")).convert("RGBA")
    def tile_color_idx(rgba):
        if rgba[3] < 128:
            return 0
        r, g, b, _ = rgba
        if r > 200 and g > 200 and b > 200:
            return 3 # White
        if b > 180 and r < 120:
            return 2 # Purple
        return 1     # Black

    bg_tiles = []
    # Tile 0: empty
    bg_tiles.append([0]*32)
    # Solid block (16x16): 4 8x8 tiles (TL, TR, BL, BR)
    for ox, oy in [(16, 0), (24, 0), (16, 8), (24, 8)]:
        tbytes = []
        for y in range(8):
            for x in range(0, 8, 2):
                p0 = tile_color_idx(img_tiles.getpixel((ox + x, oy + y)))
                p1 = tile_color_idx(img_tiles.getpixel((ox + x + 1, oy + y)))
                tbytes.append((p1 << 4) | p0)
        bg_tiles.append(tbytes)

    # BG Palettes:
    bg_pal0 = [
        rgb_to_gba(102, 17, 102), # 0: Backdrop #661166
        rgb_to_gba(0, 0, 0),       # 1: Black
        rgb_to_gba(88, 0, 248),   # 2: Purple
        rgb_to_gba(255, 255, 255) # 3: White
    ] + [0]*12

    bg_pal1 = [
        0,                         # 0: Transparent
        rgb_to_gba(255, 255, 255), # 1: White text
    ] + [0]*14

    # Subpalette 2: Yellow text font (for State and highlights)
    bg_pal2 = [
        0,                         # 0: Transparent
        rgb_to_gba(255, 255, 0),   # 1: Yellow text
    ] + [0]*14

    # Subpalette 3: Cyan text font
    bg_pal3 = [
        0,                         # 0: Transparent
        rgb_to_gba(0, 255, 255),   # 1: Cyan text
    ] + [0]*14

    # 3. Slime.png -> 5 frames of 16x16 (20 tiles)
    img_slime = Image.open(os.path.join(assets_dir, "slime.png")).convert("RGBA")
    def slime_color_idx(rgba):
        if rgba[3] < 128:
            return 0
        r, g, b, _ = rgba
        if r < 50 and g < 50 and b < 50:
            return 1 # Black
        if r > 200 and g > 240 and b > 200:
            return 4 # White
        if r > 200 and g > 240 and b > 150:
            return 3 # Pale green
        return 2     # Lime green

    obj_tiles = []
    # 5 frames, 16x16 in 1D mapping (TL, TR, BL, BR)
    for frame in range(5):
        base_x = frame * 16
        for ox, oy in [(base_x, 0), (base_x + 8, 0), (base_x, 8), (base_x + 8, 8)]:
            tbytes = []
            for y in range(8):
                for x in range(0, 8, 2):
                    p0 = slime_color_idx(img_slime.getpixel((ox + x, oy + y)))
                    p1 = slime_color_idx(img_slime.getpixel((ox + x + 1, oy + y)))
                    tbytes.append((p1 << 4) | p0)
            obj_tiles.append(tbytes)

    # 4. Powerup.png -> 1 frame of 16x16 (4 tiles)
    img_pow = Image.open(os.path.join(assets_dir, "powerup.png")).convert("RGBA")
    def pow_color_idx(rgba):
        if rgba[3] < 128:
            return 0
        r, g, b, _ = rgba
        if r < 50 and g < 50 and b < 50:
            return 1 # Black
        if r > 240 and g > 240 and b > 240:
            return 4 # White
        if r > 240 and g > 240 and b > 150:
            return 3 # Light yellow
        return 2     # Yellow

    for ox, oy in [(0, 0), (8, 0), (0, 8), (8, 8)]:
        tbytes = []
        for y in range(8):
            for x in range(0, 8, 2):
                p0 = pow_color_idx(img_pow.getpixel((ox + x, oy + y)))
                p1 = pow_color_idx(img_pow.getpixel((ox + x + 1, oy + y)))
                tbytes.append((p1 << 4) | p0)
        obj_tiles.append(tbytes)

    obj_pal0 = [
        0,                         # 0: Transparent
        rgb_to_gba(0, 0, 0),       # 1: Black outline
        rgb_to_gba(167, 255, 7),   # 2: Lime green
        rgb_to_gba(229, 255, 179), # 3: Pale green
        rgb_to_gba(255, 255, 255)  # 4: White
    ] + [0]*11

    obj_pal1 = [
        0,                         # 0: Transparent
        rgb_to_gba(0, 0, 0),       # 1: Black outline
        rgb_to_gba(255, 255, 0),   # 2: Yellow
        rgb_to_gba(255, 255, 191), # 3: Light yellow
        rgb_to_gba(255, 255, 255)  # 4: White
    ] + [0]*11

    with open(os.path.join(script_dir, "assets.h"), "w", encoding="utf-8") as f:
        f.write("#ifndef ASSETS_H\n#define ASSETS_H\n\n#include <stdint.h>\n\n")

        f.write("static const uint16_t bg_palette[64] = {\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal0) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal1) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal2) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal3) + "\n};\n\n")

        f.write("static const uint16_t obj_palette[32] = {\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal0) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal1) + "\n};\n\n")

        f.write(f"static const uint8_t font_tiles[{len(font_tiles) * 32}] = {{\n")
        for i, tb in enumerate(font_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(font_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        f.write(f"static const uint8_t bg_tiles[{len(bg_tiles) * 32}] = {{\n")
        for i, tb in enumerate(bg_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(bg_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        f.write(f"static const uint8_t obj_tiles[{len(obj_tiles) * 32}] = {{\n")
        for i, tb in enumerate(obj_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(obj_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        f.write("#endif /* ASSETS_H */\n")

    print(f"Generated assets.h successfully! Font: {len(font_tiles)}, BG tiles: {len(bg_tiles)}, OBJ tiles: {len(obj_tiles)}")

if __name__ == "__main__":
    generate()
