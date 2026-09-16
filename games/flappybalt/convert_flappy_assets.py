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

def convert_all(assets_dir, font_path, out_header_path):
    print("Converting assets from:", assets_dir)
    
    # Palettes
    # BG Bank 0: City & Spikes
    bg_pal0 = [
        rgb_to_gba(100, 106, 125), # 0: Backdrop #646a7d
        rgb_to_gba(100, 106, 125), # 1: Sky #646a7d
        rgb_to_gba(134, 134, 150), # 2: Mid skyline #868696
        rgb_to_gba(176, 176, 191), # 3: Light buildings #b0b0bf
        rgb_to_gba(255, 255, 255), # 4: White #ffffff
        rgb_to_gba(53, 53, 61),    # 5: Dark shadow #35353d
        rgb_to_gba(77, 77, 89),    # 6: Spike body #4d4d59
        rgb_to_gba(0, 0, 0),       # 7: Black
        rgb_to_gba(28, 30, 36),    # 8: Bezel dark #1c1e24
        rgb_to_gba(45, 48, 58),    # 9: Bezel line #2d303a
        0, 0, 0, 0, 0, 0
    ]
    
    # BG Bank 1: Bezel Arcade HUD
    bg_pal1 = [
        0,                         # 0: Transparent
        rgb_to_gba(28, 30, 36),    # 1: Bezel dark BG
        rgb_to_gba(45, 48, 58),    # 2: Bezel border line
        rgb_to_gba(134, 134, 150), # 3: Grey label text
        rgb_to_gba(255, 255, 255), # 4: White text/highlight
        rgb_to_gba(224, 144, 48),  # 5: Retro orange
        rgb_to_gba(64, 192, 96),   # 6: Green
        rgb_to_gba(48, 160, 255),  # 7: Cyan
        0, 0, 0, 0, 0, 0, 0, 0
    ]

    # BG Bank 2: White Text
    bg_pal2 = [
        0,
        rgb_to_gba(255, 255, 255), # 1: White font
        rgb_to_gba(53, 53, 61),    # 2: Shadow
    ] + [0]*13

    # BG Bank 3: Gold/Yellow Text
    bg_pal3 = [
        0,
        rgb_to_gba(255, 220, 60),  # 1: Gold font
        rgb_to_gba(180, 140, 20),  # 2: Dark gold
    ] + [0]*13

    # OBJ Palettes:
    # Bank 0: Dove (White, light grey, dark eye)
    obj_pal0 = [
        0,                         # 0: Transparent
        rgb_to_gba(255, 255, 255), # 1: White
        rgb_to_gba(176, 176, 191), # 2: Light grey
        rgb_to_gba(53, 53, 61),    # 3: Dark beak/eye
    ] + [0]*12

    # Bank 1: Spikes & Paddle
    obj_pal1 = [
        0,                         # 0: Transparent
        rgb_to_gba(53, 53, 61),    # 1: Dark outline #35353d
        rgb_to_gba(77, 77, 89),    # 2: Spike body #4d4d59
        rgb_to_gba(134, 134, 150), # 3: Mid tone #868696
        rgb_to_gba(176, 176, 191), # 4: Light tooth #b0b0bf
        rgb_to_gba(255, 255, 255), # 5: White tip #ffffff
        rgb_to_gba(100, 106, 125)  # 6: Sky match
    ] + [0]*9

    # Bank 2: Feathers
    obj_pal2 = [
        0,                         # 0: Transparent
        rgb_to_gba(255, 255, 255), # 1: White
        rgb_to_gba(176, 176, 191), # 2: Light grey
    ] + [0]*13

    # Bank 3: Bounce bar flash
    obj_pal3 = [
        0,
        rgb_to_gba(255, 255, 255), # 1: White
        rgb_to_gba(224, 144, 48),  # 2: Orange spark
    ] + [0]*13

    # 1. Font tiles (128 tiles)
    font_raw = parse_font8x8(font_path)
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

    # 2. Bezel & UI helper tiles (placed after font tiles in Charblock 0)
    # Tile 128: Solid Bezel Dark BG (color 1 in pal1)
    # Tile 129: Vertical border line left (color 2 at x=7, color 1 elsewhere)
    # Tile 130: Vertical border line right (color 2 at x=0, color 1 elsewhere)
    # Tile 131: Diagonal hatch / pattern tile
    bezel_tiles = []
    # Solid dark
    bezel_tiles.append([0x11]*32)
    # Left vertical divider
    t_vdiv_l = []
    for y in range(8):
        for x in range(0, 8, 2):
            p0 = 2 if x == 6 else (2 if x == 7 else 1)
            p1 = 2 if (x + 1) == 7 else 1
            t_vdiv_l.append((p1 << 4) | p0)
    bezel_tiles.append(t_vdiv_l)
    # Right vertical divider
    t_vdiv_r = []
    for y in range(8):
        for x in range(0, 8, 2):
            p0 = 2 if x == 0 else 1
            p1 = 1
            t_vdiv_r.append((p1 << 4) | p0)
    bezel_tiles.append(t_vdiv_r)

    # 3. BG City Skyline (bg.png: 160x240 -> 20x30 tiles)
    img_bg = Image.open(os.path.join(assets_dir, "bg.png")).convert("RGB")
    def bg_color_idx(rgb):
        r, g, b = rgb
        if r > 150 and g > 150 and b > 160:
            return 3 # #b0b0bf light
        if r > 115 and g > 115 and b > 130:
            return 2 # #868696 mid
        return 1     # #646a7d dark base / sky

    bg_tiles = []
    bg_tile_map = {} # bytes -> tile_index (offset by 0 in Charblock 1)
    bg_layout = []   # 30 rows of 20 tile IDs

    w_bg, h_bg = img_bg.size
    for ty in range(h_bg // 8):
        row_ids = []
        for tx in range(w_bg // 8):
            tbytes = []
            for y in range(8):
                for x in range(0, 8, 2):
                    c0 = bg_color_idx(img_bg.getpixel((tx*8 + x, ty*8 + y)))
                    c1 = bg_color_idx(img_bg.getpixel((tx*8 + x + 1, ty*8 + y)))
                    tbytes.append((c1 << 4) | c0)
            tb_tuple = bytes(tbytes)
            if tb_tuple not in bg_tile_map:
                bg_tile_map[tb_tuple] = len(bg_tiles)
                bg_tiles.append(tbytes)
            row_ids.append(bg_tile_map[tb_tuple])
        bg_layout.append(row_ids)

    # 4. Spike.png (160x16 -> 20x2 tiles)
    img_spike = Image.open(os.path.join(assets_dir, "spike.png")).convert("RGBA")
    def spike_color_idx(rgba):
        if rgba[3] < 128:
            return 0 # transparent / sky
        r, g, b, _ = rgba
        if r > 240 and g > 240 and b > 240:
            return 4 # white
        if r > 150 and g > 150 and b > 160:
            return 3 # #b0b0bf light tooth
        if r > 115 and g > 115 and b > 130:
            return 2 # #868696 mid
        if r < 60 and g < 60 and b < 70:
            return 5 # #35353d dark outline
        return 6     # #4d4d59 body

    spike_tiles = []
    spike_tile_map = {}
    spike_layout = [] # 2 rows of 20 tile IDs

    w_sp, h_sp = img_spike.size
    spike_base_idx = len(bg_tiles)
    for ty in range(h_sp // 8):
        row_ids = []
        for tx in range(w_sp // 8):
            tbytes = []
            for y in range(8):
                for x in range(0, 8, 2):
                    c0 = spike_color_idx(img_spike.getpixel((tx*8 + x, ty*8 + y)))
                    c1 = spike_color_idx(img_spike.getpixel((tx*8 + x + 1, ty*8 + y)))
                    tbytes.append((c1 << 4) | c0)
            tb_tuple = bytes(tbytes)
            if tb_tuple not in spike_tile_map:
                spike_tile_map[tb_tuple] = spike_base_idx + len(spike_tiles)
                spike_tiles.append(tbytes)
            row_ids.append(spike_tile_map[tb_tuple])
        spike_layout.append(row_ids)

    # 5. Bounce bars (Left & Right)
    # Left bounce bar: dark line at x=0, grey fill x=1..3, light line at x=4, sky x=5..7
    # Tile index = spike_base_idx + len(spike_tiles)
    bounce_bar_tile_idx = spike_base_idx + len(spike_tiles)
    t_bounce_l = []
    for y in range(8):
        for x in range(0, 8, 2):
            def p_col(cx):
                if cx == 0: return 5 # dark line #35353d
                if 1 <= cx <= 3: return 2 # mid grey #868696
                if cx == 4: return 3 # light line #b0b0bf
                return 0 # sky/transparent
            p0 = p_col(x)
            p1 = p_col(x + 1)
            t_bounce_l.append((p1 << 4) | p0)
    
    # Right bounce bar: sky x=0..2, light line x=3, grey fill x=4..6, dark line x=7
    t_bounce_r = []
    for y in range(8):
        for x in range(0, 8, 2):
            def p_col(cx):
                if cx == 3: return 3 # light line
                if 4 <= cx <= 6: return 2 # mid grey
                if cx == 7: return 5 # dark line
                return 0 # sky
            p0 = p_col(x)
            p1 = p_col(x + 1)
            t_bounce_r.append((p1 << 4) | p0)

    # Flash bounce bar tiles (white)
    t_bounce_flash_l = []
    for y in range(8):
        for x in range(0, 8, 2):
            def p_col(cx):
                if cx == 0: return 4 # white
                if 1 <= cx <= 4: return 4 # white
                return 0
            p0 = p_col(x)
            p1 = p_col(x + 1)
            t_bounce_flash_l.append((p1 << 4) | p0)

    t_bounce_flash_r = []
    for y in range(8):
        for x in range(0, 8, 2):
            def p_col(cx):
                if 3 <= cx <= 7: return 4 # white
                return 0
            p0 = p_col(x)
            p1 = p_col(x + 1)
            t_bounce_flash_r.append((p1 << 4) | p0)

    bg_extra_tiles = [t_bounce_l, t_bounce_r, t_bounce_flash_l, t_bounce_flash_r]

    # Combine all Charblock 1 BG tiles:
    # 0 .. len(bg_tiles)-1: bg tiles
    # spike_base_idx .. spike_base_idx + len(spike_tiles)-1: spike tiles
    # then bounce bar tiles
    all_bg_tiles = bg_tiles + spike_tiles + bg_extra_tiles
    print(f"Total BG Tiles in Charblock 1: {len(all_bg_tiles)} (BG: {len(bg_tiles)}, Spikes: {len(spike_tiles)}, Extra: {len(bg_extra_tiles)})")

    # 6. OBJ Sprites:
    # Dove (24x8 -> 3 frames of 8x8)
    img_dove = Image.open(os.path.join(assets_dir, "dove.png")).convert("RGBA")
    obj_tiles = []
    # 3 frames
    for f in range(3):
        tbytes = []
        for y in range(8):
            for x in range(0, 8, 2):
                def d_col(dx, dy):
                    pix = img_dove.getpixel((f*8 + dx, dy))
                    if pix[3] < 128: return 0
                    if pix[0] > 200 and pix[1] > 200 and pix[2] > 200: return 1 # white body
                    return 2
                p0 = d_col(x, y)
                p1 = d_col(x + 1, y)
                tbytes.append((p1 << 4) | p0)
        obj_tiles.append(tbytes)

    # Feather (4x2 -> put in 8x8 tile)
    img_feather = Image.open(os.path.join(assets_dir, "feather.png")).convert("RGBA")
    t_feather = []
    for y in range(8):
        for x in range(0, 8, 2):
            def f_col(fx, fy):
                # center 4x2 in 8x8 (x: 2..5, y: 3..4)
                if 2 <= fx <= 5 and 3 <= fy <= 4:
                    pix = img_feather.getpixel((fx - 2, fy - 3))
                    if pix[3] > 128: return 1 # white
                return 0
            p0 = f_col(x, y)
            p1 = f_col(x + 1, y)
            t_feather.append((p1 << 4) | p0)
    obj_tiles.append(t_feather)
    feather_tile_idx = 3

    # Paddle (9x91) -> Extract 16x48 (2 tiles wide x 6 tiles high = 12 tiles)
    # Upper 16x32: 8 tiles (in 1D mapping: TL, TR, row2L, row2R, row3L, row3R, row4L, row4R)
    # Lower 16x16: 4 tiles (row5L, row5R, row6L, row6R)
    img_paddle = Image.open(os.path.join(assets_dir, "paddle.png")).convert("RGBA")
    def paddle_col_idx(rgba):
        if rgba[3] < 128: return 0
        r, g, b, _ = rgba
        if r > 240 and g > 240 and b > 240: return 5 # white tip
        if r > 150 and g > 150 and b > 160: return 4 # light tooth
        if r > 115 and g > 115 and b > 130: return 3 # mid tone
        if r < 60 and g < 60 and b < 70: return 1   # dark outline
        return 2 # body

    paddle_start_idx = len(obj_tiles) # 4
    # 6 rows of 8px high = 48px
    # Take rows 20..68 from paddle.png (rich teeth pattern)
    for r in range(6):
        base_y = 20 + r * 8
        # Left 8x8 tile (x: 0..7)
        t_l = []
        for y in range(8):
            for x in range(0, 8, 2):
                p0 = paddle_col_idx(img_paddle.getpixel((x, base_y + y)))
                p1 = paddle_col_idx(img_paddle.getpixel((x + 1, base_y + y)))
                t_l.append((p1 << 4) | p0)
        obj_tiles.append(t_l)
        
        # Right 8x8 tile (x: 8..15, paddle has width 9 so x=8 is last column, 9..15 is 0)
        t_r = []
        for y in range(8):
            for x in range(0, 8, 2):
                def pr_col(cx):
                    if cx == 0 and (base_y + y) < img_paddle.height:
                        return paddle_col_idx(img_paddle.getpixel((8, base_y + y)))
                    return 0
                p0 = pr_col(x)
                p1 = pr_col(x + 1)
                t_r.append((p1 << 4) | p0)
        obj_tiles.append(t_r)

    print(f"Total OBJ Tiles: {len(obj_tiles)} (Dove: 3, Feather: 1, Paddle 16x48: 12)")

    # Write assets.h
    with open(out_header_path, "w", encoding="utf-8") as f:
        f.write("#ifndef FLAPPY_ASSETS_H\n#define FLAPPY_ASSETS_H\n\n#include <stdint.h>\n\n")

        # Constants
        f.write(f"#define FONT_TILES_COUNT       {len(font_tiles)}\n")
        f.write(f"#define BEZEL_TILES_COUNT      {len(bezel_tiles)}\n")
        f.write(f"#define BG_TILES_COUNT         {len(all_bg_tiles)}\n")
        f.write(f"#define OBJ_TILES_COUNT        {len(obj_tiles)}\n\n")
        f.write(f"#define SPIKE_BASE_TILE        {spike_base_idx}\n")
        f.write(f"#define BOUNCE_BAR_L_TILE      {bounce_bar_tile_idx}\n")
        f.write(f"#define BOUNCE_BAR_R_TILE      {bounce_bar_tile_idx + 1}\n")
        f.write(f"#define BOUNCE_FLASH_L_TILE    {bounce_bar_tile_idx + 2}\n")
        f.write(f"#define BOUNCE_FLASH_R_TILE    {bounce_bar_tile_idx + 3}\n\n")
        f.write(f"#define DOVE_SPRITE_TILE       0\n")
        f.write(f"#define FEATHER_SPRITE_TILE    {feather_tile_idx}\n")
        f.write(f"#define PADDLE_SPRITE_TILE     {paddle_start_idx}\n\n")

        # Palettes
        f.write("static const uint16_t bg_palette[64] = {\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal0) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal1) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal2) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in bg_pal3) + "\n};\n\n")

        f.write("static const uint16_t obj_palette[64] = {\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal0) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal1) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal2) + ",\n    ")
        f.write(", ".join(f"0x{c:04X}" for c in obj_pal3) + "\n};\n\n")

        # Font Tiles
        f.write(f"static const uint8_t font_tiles[{len(font_tiles) * 32}] = {{\n")
        for i, tb in enumerate(font_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(font_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # Bezel Tiles
        f.write(f"static const uint8_t bezel_tiles[{len(bezel_tiles) * 32}] = {{\n")
        for i, tb in enumerate(bezel_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(bezel_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # BG Tiles
        f.write(f"static const uint8_t bg_tiles[{len(all_bg_tiles) * 32}] = {{\n")
        for i, tb in enumerate(all_bg_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(all_bg_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # OBJ Tiles
        f.write(f"static const uint8_t obj_tiles[{len(obj_tiles) * 32}] = {{\n")
        for i, tb in enumerate(obj_tiles):
            f.write("    " + ", ".join(f"0x{b:02X}" for b in tb) + ("," if i < len(obj_tiles)-1 else "") + "\n")
        f.write("};\n\n")

        # BG Map (30 rows of 20 tiles)
        f.write("static const uint16_t bg_city_map[30][20] = {\n")
        for r in range(30):
            f.write("    {" + ", ".join(f"{t}" for t in bg_layout[r]) + "}" + ("," if r < 29 else "") + "\n")
        f.write("};\n\n")

        # Spike Map (2 rows of 20 tiles)
        f.write("static const uint16_t spike_map[2][20] = {\n")
        for r in range(2):
            f.write("    {" + ", ".join(f"{t}" for t in spike_layout[r]) + "}" + ("," if r < 1 else "") + "\n")
        f.write("};\n\n")

        f.write("#endif /* FLAPPY_ASSETS_H */\n")

    print(f"Successfully generated {out_header_path}!")

if __name__ == "__main__":
    import sys
    script_dir = os.path.dirname(os.path.abspath(__file__))
    assets_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(script_dir, "assets")
    fpath = sys.argv[2] if len(sys.argv) > 2 else os.path.join(assets_dir, "font8x8_basic.h")
    out_h = sys.argv[3] if len(sys.argv) > 3 else os.path.join(script_dir, "assets.h")
    convert_all(assets_dir, fpath, out_h)

