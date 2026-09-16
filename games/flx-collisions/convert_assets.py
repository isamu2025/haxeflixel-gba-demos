import os
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

def map_color_to_idx(rgba, is_sprite=False):
    # rgba: tuple of (r, g, b, a)
    if rgba[3] < 128:
        return 0
    r, g, b = rgba[:3]
    # Check if backdrop color
    if not is_sprite and r > 165 and g > 180 and b > 210:
        return 0
    if r < 10 and g < 10 and b < 10:
        return 5 # Black
    if r > 150 and g > 175:
        return 4 # Highlight soft blue
    if r > 100:
        return 3 # Light blue
    if r > 55:
        return 2 # Medium blue
    return 1     # Dark blue

def generate():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    assets_dir = os.path.join(script_dir, "assets")

    # 1. Font -> 4bpp tiles
    font_raw = parse_font8x8(os.path.join(assets_dir, "font8x8_basic.h"))
    font_tiles = []
    for char_idx, rows in enumerate(font_raw):
        tbytes = []
        for y in range(8):
            row_byte = rows[y]
            for x in range(0, 8, 2):
                p0 = 1 if (row_byte & (1 << x)) else 0
                p1 = 1 if (row_byte & (1 << (x + 1))) else 0
                tbytes.append((p1 << 4) | p0)
        font_tiles.append(tbytes)

    # 2. Map & autotiling
    map_img = Image.open(os.path.join(assets_dir, "map.png"))
    w, h = map_img.size # 20, 15
    w2, h2 = w * 2, h * 2 # 40, 30
    grid = [[0]*w2 for _ in range(h2)]
    for y in range(h):
        for x in range(w):
            val = map_img.getpixel((x, y))
            if val > 0:
                grid[y*2][x*2] = 1
                grid[y*2][x*2+1] = 1
                grid[y*2+1][x*2] = 1
                grid[y*2+1][x*2+1] = 1

    tile_map = [[0]*w2 for _ in range(h2)]
    for y in range(h2):
        for x in range(w2):
            if grid[y][x] == 0:
                continue
            up = 1 if (y == 0 or grid[y-1][x] > 0) else 0
            rt = 2 if (x == w2-1 or grid[y][x+1] > 0) else 0
            dn = 4 if (y == h2-1 or grid[y+1][x] > 0) else 0
            lt = 8 if (x == 0 or grid[y][x-1] > 0) else 0
            val = up + rt + dn + lt
            if val == 15:
                # interior corners
                if x > 0 and y < h2-1 and grid[y+1][x-1] == 0:
                    val = 1
                elif x > 0 and y > 0 and grid[y-1][x-1] == 0:
                    val = 2
                elif x < w2-1 and y > 0 and grid[y-1][x+1] == 0:
                    val = 3
                elif x < w2-1 and y < h2-1 and grid[y+1][x+1] == 0:
                    val = 4
            tile_map[y][x] = val

    # 3. Tiles.png -> 16 8x8 tiles (4bpp)
    tiles_img = Image.open(os.path.join(assets_dir, "tiles.png")).convert("RGBA")
    bg_tiles = []
    # Tile 0: empty
    bg_tiles.append([0]*32)
    # Tiles 1..15 from tiles.png
    for t in range(1, 16):
        sub = tiles_img.crop((t*8, 0, (t+1)*8, 8))
        tbytes = []
        for y in range(8):
            for x in range(0, 8, 2):
                p0 = map_color_to_idx(sub.getpixel((x, y)), is_sprite=False)
                p1 = map_color_to_idx(sub.getpixel((x+1, y)), is_sprite=False)
                tbytes.append((p1 << 4) | p0)
        bg_tiles.append(tbytes)

    # 4. Palettes
    # Background Palettes
    bg_pal0 = [
        rgb_to_gba(172, 188, 215), # 0: Backdrop #acbcd7
        rgb_to_gba(35, 62, 88),    # 1: Dark blue #233e58
        rgb_to_gba(73, 99, 122),   # 2: Medium blue #49637a
        rgb_to_gba(119, 142, 161), # 3: Light blue #778ea1
        rgb_to_gba(164, 189, 215), # 4: Soft highlight #a3bcd6
        rgb_to_gba(0, 0, 0),       # 5: Black
    ] + [0]*10

    # Subpalette 1: White text
    bg_pal1 = [0, rgb_to_gba(255, 255, 255)] + [0]*14
    # Subpalette 2: Yellow text
    bg_pal2 = [0, rgb_to_gba(255, 255, 0)] + [0]*14
    # Subpalette 3: Cyan text
    bg_pal3 = [0, rgb_to_gba(0, 255, 255)] + [0]*14

    # OBJ Palettes
    obj_pal0 = [
        0,                         # 0: Transparent
        rgb_to_gba(35, 62, 88),    # 1: Dark blue
        rgb_to_gba(73, 99, 122),   # 2: Medium blue
        rgb_to_gba(119, 142, 161), # 3: Light blue
        rgb_to_gba(164, 189, 215), # 4: Highlight
        rgb_to_gba(255, 255, 255), # 5: White
    ] + [0]*10

    # 5. Sprites -> 4bpp tiles in Charblock 4
    obj_tiles = []

    # Helper to convert an image rectangle into 8x8 4bpp tiles in 1D mapping
    def add_sprite_tiles(im, width_tiles, height_tiles):
        # In 1D OBJ mapping, tiles are ordered row by row:
        # tile (0, 0), tile (1, 0), ... tile (width_tiles-1, height_tiles-1)
        for ty in range(height_tiles):
            for tx in range(width_tiles):
                tbytes = []
                for y in range(8):
                    for x in range(0, 8, 2):
                        px0 = tx * 8 + x
                        py0 = ty * 8 + y
                        px1 = tx * 8 + x + 1
                        py1 = ty * 8 + y
                        c0 = im.getpixel((px0, py0)) if px0 < im.width and py0 < im.height else (0, 0, 0, 0)
                        c1 = im.getpixel((px1, py1)) if px1 < im.width and py1 < im.height else (0, 0, 0, 0)
                        p0 = map_color_to_idx(c0, is_sprite=True)
                        p1 = map_color_to_idx(c1, is_sprite=True)
                        tbytes.append((p1 << 4) | p0)
                obj_tiles.append(tbytes)

    # (a) Player: 5 frames of 14x14 -> put in 16x16 (2x2 tiles)
    player_img = Image.open(os.path.join(assets_dir, "player.png")).convert("RGBA")
    for f in range(5):
        sub_p = player_img.crop((f * 14, 0, (f + 1) * 14, 14))
        frame16 = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        frame16.paste(sub_p, (1, 1))
        add_sprite_tiles(frame16, 2, 2) # 4 tiles per frame * 5 frames = 20 tiles (0..19)

    # (b) Elevator: 50x12 -> put in 64x32 (8x4 tiles, matching GBA ATTR0_WIDE + ATTR1_SIZE_64)
    elev_img = Image.open(os.path.join(assets_dir, "elevator.png")).convert("RGBA")
    elev64 = Image.new("RGBA", (64, 32), (0, 0, 0, 0))
    elev64.paste(elev_img, (7, 2))
    add_sprite_tiles(elev64, 8, 4) # 32 tiles (20..51)

    # (c) Pusher: 8x16 -> put in 16x16 (2x2 tiles)
    push_img = Image.open(os.path.join(assets_dir, "pusher.png")).convert("RGBA")
    push16 = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    push16.paste(push_img, (4, 0))
    add_sprite_tiles(push16, 2, 2) # 4 tiles (52..55)

    # (d) Crate: 10x10 -> put in 16x16 (2x2 tiles)
    crate_img = Image.open(os.path.join(assets_dir, "crate.png")).convert("RGBA")
    crate16 = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    crate16.paste(crate_img, (3, 3))
    add_sprite_tiles(crate16, 2, 2) # 4 tiles (56..59)

    # (e) Gibs: 4 particles of 6x6 -> put in 8x8 (1x1 tile)
    gibs_img = Image.open(os.path.join(assets_dir, "gibs.png")).convert("RGBA")
    for g in range(4):
        sub_g = gibs_img.crop((g * 6, 0, (g + 1) * 6, 6))
        gib8 = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
        gib8.paste(sub_g, (1, 1))
        add_sprite_tiles(gib8, 1, 1) # 4 tiles (60..63)

    # Write assets.h
    with open(os.path.join(script_dir, "assets.h"), "w", encoding="utf-8") as f:
        f.write("#ifndef ASSETS_H\n#define ASSETS_H\n\n#include <stdint.h>\n\n")

        # BG Palettes
        f.write("static const uint16_t bg_palette[64] = {\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal0) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal1) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal2) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal3) + "\n};\n\n")

        # OBJ Palettes
        f.write("static const uint16_t obj_palette[16] = {\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal0) + "\n};\n\n")

        # Font Tiles
        f.write(f"static const uint8_t font_tiles[{len(font_tiles) * 32}] = {{\n")
        for i, tb in enumerate(font_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(font_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # BG Tiles
        f.write(f"static const uint8_t bg_tiles[{len(bg_tiles) * 32}] = {{\n")
        for i, tb in enumerate(bg_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(bg_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # OBJ Tiles
        f.write(f"static const uint8_t obj_tiles[{len(obj_tiles) * 32}] = {{\n")
        for i, tb in enumerate(obj_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(obj_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # Baked Tilemap 40 x 30
        f.write("#define MAP_COLS 40\n#define MAP_ROWS 30\n\n")
        f.write(f"static const uint8_t level_autotiles[MAP_ROWS][MAP_COLS] = {{\n")
        for r in range(h2):
            f.write("    {" + ", ".join(f"{v:2d}" for v in tile_map[r]) + "}" + ("," if r < h2-1 else "") + "\n")
        f.write("};\n\n")

        # Solid Grid 40 x 30
        f.write(f"static const uint8_t level_collision[MAP_ROWS][MAP_COLS] = {{\n")
        for r in range(h2):
            f.write("    {" + ", ".join(f"{v}" for v in grid[r]) + "}" + ("," if r < h2-1 else "") + "\n")
        f.write("};\n\n")

        f.write("#endif /* ASSETS_H */\n")

    print(f"Generated assets.h successfully! Font: {len(font_tiles)}, BG tiles: {len(bg_tiles)}, OBJ tiles: {len(obj_tiles)}")

if __name__ == "__main__":
    generate()
