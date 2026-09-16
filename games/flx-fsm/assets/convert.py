import sys
from PIL import Image

def rgb_to_gba(r, g, b):
    # 8-bit to 5-bit
    r5 = (r >> 3) & 0x1F
    g5 = (g >> 3) & 0x1F
    b5 = (b >> 3) & 0x1F
    return r5 | (g5 << 5) | (b5 << 10)

def convert_tiles():
    img = Image.open("/home/pi/dc-projects/flx-fsm/assets/tiles.png").convert("RGBA")
    # Solid block is at x=16..31, y=0..15
    # Colors:
    # 0: transparent (or purple bg)
    # 1: black (0,0,0)
    # 2: purple (88, 0, 248)
    # 3: white (255, 255, 255)
    palette = [
        rgb_to_gba(102, 17, 102), # 0: Backdrop purple
        rgb_to_gba(0, 0, 0),       # 1: Black
        rgb_to_gba(88, 0, 248),   # 2: Purple
        rgb_to_gba(255, 255, 255) # 3: White
    ]
    while len(palette) < 16:
        palette.append(0)

    def get_color_idx(rgba):
        if rgba[3] < 128:
            return 0
        r, g, b, _ = rgba
        if r > 200 and g > 200 and b > 200:
            return 3
        if b > 180 and r < 120:
            return 2
        return 1

    # Tile 0: empty 8x8 (32 bytes of 0)
    # Tile 1: Top-Left (x: 16..23, y: 0..7)
    # Tile 2: Top-Right (x: 24..31, y: 0..7)
    # Tile 3: Bottom-Left (x: 16..23, y: 8..15)
    # Tile 4: Bottom-Right (x: 24..31, y: 8..15)
    tiles_data = []

    # Tile 0
    tiles_data.append([0]*32)

    coords = [
        (16, 0), # TL
        (24, 0), # TR
        (16, 8), # BL
        (24, 8), # BR
    ]
    for ox, oy in coords:
        tbytes = []
        for y in range(8):
            for x in range(0, 8, 2):
                p0 = get_color_idx(img.getpixel((ox + x, oy + y)))
                p1 = get_color_idx(img.getpixel((ox + x + 1, oy + y)))
                tbytes.append((p1 << 4) | p0)
        tiles_data.append(tbytes)

    return palette, tiles_data

print("Converter test...")
pal, tiles = convert_tiles()
print("Tiles count:", len(tiles), "Palette:", [hex(c) for c in pal[:4]])
