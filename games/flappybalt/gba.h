#ifndef GBA_H
#define GBA_H

#include <stdint.h>
#include <stdbool.h>

/* Video Registers */
#define REG_DISPCNT     (*(volatile uint16_t*)0x04000000)
#define REG_DISPSTAT    (*(volatile uint16_t*)0x04000004)
#define REG_VCOUNT      (*(volatile uint16_t*)0x04000006)
#define REG_BG0CNT      (*(volatile uint16_t*)0x04000008)
#define REG_BG1CNT      (*(volatile uint16_t*)0x0400000A)
#define REG_BG0HOFS     (*(volatile uint16_t*)0x04000010)
#define REG_BG0VOFS     (*(volatile uint16_t*)0x04000012)
#define REG_BG1HOFS     (*(volatile uint16_t*)0x04000014)
#define REG_BG1VOFS     (*(volatile uint16_t*)0x04000016)

/* Input Register */
#define REG_KEYINPUT    (*(volatile uint16_t*)0x04000130)

/* Blend Registers */
#define REG_BLDCNT      (*(volatile uint16_t*)0x04000050)
#define REG_BLDALPHA    (*(volatile uint16_t*)0x04000052)
#define REG_BLDY        (*(volatile uint16_t*)0x04000054)

/* Sound Registers */
#define REG_SOUND1CNT_L (*(volatile uint16_t*)0x04000060)
#define REG_SOUND1CNT_H (*(volatile uint16_t*)0x04000062)
#define REG_SOUND1CNT_X (*(volatile uint16_t*)0x04000064)
#define REG_SOUND2CNT_L (*(volatile uint16_t*)0x04000068)
#define REG_SOUND2CNT_H (*(volatile uint16_t*)0x0400006C)
#define REG_SOUND4CNT_L (*(volatile uint16_t*)0x04000078)
#define REG_SOUND4CNT_H (*(volatile uint16_t*)0x0400007C)
#define REG_SOUNDCNT_L  (*(volatile uint16_t*)0x04000080)
#define REG_SOUNDCNT_H  (*(volatile uint16_t*)0x04000082)
#define REG_SOUNDCNT_X  (*(volatile uint16_t*)0x04000084)

/* Memory Map */
#define MEM_PAL_BG      ((volatile uint16_t*)0x05000000)
#define MEM_PAL_OBJ     ((volatile uint16_t*)0x05000200)
#define MEM_VRAM        ((volatile uint16_t*)0x06000000)
#define MEM_OAM         ((volatile OBJ_ATTR*)0x07000000)

/* Helper macros for VRAM blocks */
#define CHARBLOCK(n)    ((volatile uint16_t*)(0x06000000 + ((n) * 0x4000)))
#define SCREENBLOCK(n)  ((volatile uint16_t*)(0x06000000 + ((n) * 0x800)))

/* Color Helper */
#define RGB15(r,g,b)    ((uint16_t)(((r) & 0x1F) | (((g) & 0x1F) << 5) | (((b) & 0x1F) << 10)))

/* Keys */
#define KEY_A           (1 << 0)
#define KEY_B           (1 << 1)
#define KEY_SELECT      (1 << 2)
#define KEY_START       (1 << 3)
#define KEY_RIGHT       (1 << 4)
#define KEY_LEFT        (1 << 5)
#define KEY_UP          (1 << 6)
#define KEY_DOWN        (1 << 7)
#define KEY_R           (1 << 8)
#define KEY_L           (1 << 9)

/* Sprite OAM Structure */
typedef struct {
    uint16_t attr0;
    uint16_t attr1;
    uint16_t attr2;
    int16_t  fill;
} __attribute__((packed, aligned(4))) OBJ_ATTR;

/* ATTR0 Flags */
#define ATTR0_Y_MASK      0x00FF
#define ATTR0_REGULAR     (0 << 8)
#define ATTR0_AFFINE      (1 << 8)
#define ATTR0_HIDE        (2 << 8)
#define ATTR0_DOUBLE      (3 << 8)
#define ATTR0_MODE_NORMAL (0 << 10)
#define ATTR0_4BPP        (0 << 13)
#define ATTR0_8BPP        (1 << 13)
#define ATTR0_SQUARE      (0 << 14)
#define ATTR0_WIDE        (1 << 14)
#define ATTR0_TALL        (2 << 14)

/* ATTR1 Flags */
#define ATTR1_X_MASK      0x01FF
#define ATTR1_HFLIP       (1 << 12)
#define ATTR1_VFLIP       (1 << 13)
#define ATTR1_SIZE_8      (0 << 14)
#define ATTR1_SIZE_16     (1 << 14)
#define ATTR1_SIZE_32     (2 << 14)
#define ATTR1_SIZE_64     (3 << 14)

/* ATTR2 Flags */
#define ATTR2_TILE_MASK   0x03FF
#define ATTR2_PRIORITY(n) (((n) & 3) << 10)
#define ATTR2_PALETTE(n)  (((n) & 0xF) << 12)

/* VBlank synchronizer */
static inline void vsync(void) {
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

#endif /* GBA_H */
