#include "gba.h"
#include "assets.h"

/* Level dimensions: 20 x 15 tiles (16x16 pixels each) = 320 x 240 */
#define MAP_WIDTH       20
#define MAP_HEIGHT      15
#define TILE_SIZE       16

/* Fixed point (16.16) helpers */
#define FP_SHIFT        16
#define TO_FP(x)        ((int32_t)((x) << FP_SHIFT))
#define FROM_FP(x)      ((int32_t)((x) >> FP_SHIFT))

/* Physics Constants (in pixels/sec scaled to 60fps) */
#define GRAVITY         TO_FP(600)
#define MAX_VEL_X       TO_FP(100)
#define MAX_VEL_Y       TO_FP(600)
#define ACCEL_X         TO_FP(300)
#define JUMP_IMPULSE    TO_FP(200)
#define SUPER_JUMP_IMP  TO_FP(300)

/* Map layout matching FlxFSM PlayState.hx */
static const uint8_t level_data[MAP_HEIGHT][MAP_WIDTH] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
    {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}
};

/* FlxFSM States */
typedef enum {
    STATE_IDLE = 0,
    STATE_JUMP,
    STATE_SUPER_JUMP,
    STATE_GROUND_POUND,
    STATE_GROUND_POUND_FINISH
} SlimeState;

static const char* state_names[] = {
    "Idle",
    "Jump",
    "SuperJump",
    "GroundPound",
    "GroundPoundFinish"
};

/* Slime Entity */
typedef struct {
    int32_t x, y;       /* Fixed point 16.16 */
    int32_t vx, vy;     /* Fixed point 16.16 */
    SlimeState state;
    bool facing_left;
    bool is_grounded;
    uint8_t anim_frame;
    uint16_t anim_timer;
    uint16_t state_timer;
    uint8_t anim_step;
} Slime;

static Slime slime;
static bool has_super_jump;
static bool has_ground_pound;
static bool super_jump_active;
static bool ground_pound_active;

static int32_t cam_x = 0;
static int32_t cam_y = 0;
static int16_t shake_timer = 0;
static uint16_t ground_pound_info_timer = 0;

static uint16_t key_curr = 0;
static uint16_t key_prev = 0;
static uint16_t key_pressed = 0;

/* Shadow OAM buffer */
static OBJ_ATTR shadow_oam[128];

/* Simple RNG for camera shake */
static uint32_t rng_seed = 1234567;
static uint32_t rand_next(void) {
    rng_seed = rng_seed * 1664525u + 1013904223u;
    return rng_seed;
}

/* Sound Functions */
static void sound_init(void) {
    REG_SOUNDCNT_X = 0x0080; /* Sound Master Enable */
    REG_SOUNDCNT_L = 0xFF77; /* Enable Sound 1, 2, 3, 4 to Left & Right at Max Volume 7 */
    REG_SOUNDCNT_H = 0x0002; /* 100% PSG volume ratio */
}

static void snd_jump(void) {
    REG_SOUND1CNT_L = 0x0015; /* Sweep Up, shift 5, time 1 */
    REG_SOUND1CNT_H = 0xF280; /* Volume 15, envelope decay step 2, 50% duty */
    REG_SOUND1CNT_X = 0x8680; /* Initial frequency ~1200Hz, trigger */
}

static void snd_super_jump(void) {
    REG_SOUND1CNT_L = 0x0013; /* Faster sweep Up, shift 3, time 1 */
    REG_SOUND1CNT_H = 0xF280; /* Volume 15, envelope decay step 2, 50% duty */
    REG_SOUND1CNT_X = 0x8740; /* Higher pitch ~1700Hz, trigger */
}

static void snd_ground_pound_start(void) {
    REG_SOUND1CNT_L = 0x0019; /* Sweep Down, shift 1, time 1 */
    REG_SOUND1CNT_H = 0xF180; /* Volume 15, 50% duty */
    REG_SOUND1CNT_X = 0x8700; /* High starting pitch, sweep downwards */
}

static void snd_powerup(void) {
    REG_SOUND2CNT_L = 0xF380; /* Volume 15, envelope decay step 3, 50% duty */
    REG_SOUND2CNT_H = 0x87C0; /* High chime ~1900Hz, trigger */
}

static void snd_ground_slam(void) {
    REG_SOUND4CNT_L = 0xF022; /* Volume 15, envelope decay step 2 */
    REG_SOUND4CNT_H = 0x8028; /* Heavy noise rumble / crash, trigger */
}

static void snd_walk_tick(void) {
    REG_SOUND4CNT_L = 0x5101; /* Soft pop volume 5, envelope decay */
    REG_SOUND4CNT_H = 0xC042; /* Short timed burst, trigger */
}


/* HUD System on BG0 */
static void hud_clear(void) {
    volatile uint16_t* hud_sb = SCREENBLOCK(31);
    for (int i = 0; i < 32 * 32; i++) {
        hud_sb[i] = 0; /* 0 is transparent character */
    }
}

static void hud_print(int x, int y, const char* str, uint8_t palette_bank) {
    volatile uint16_t* hud_sb = SCREENBLOCK(31);
    int i = 0;
    while (str[i] != '\0' && (x + i) < 30) {
        uint8_t c = (uint8_t)str[i];
        hud_sb[y * 32 + (x + i)] = c | (palette_bank << 12);
        i++;
    }
}

static void hud_clear_line(int y) {
    volatile uint16_t* hud_sb = SCREENBLOCK(31);
    for (int x = 0; x < 30; x++) {
        hud_sb[y * 32 + x] = 0;
    }
}

/* Background Map Setup on BG1 (64x32 tiles, 512x256 px) */
static void init_map(void) {
    volatile uint16_t* sb28 = SCREENBLOCK(28);
    volatile uint16_t* sb29 = SCREENBLOCK(29);

    for (int i = 0; i < 1024; i++) {
        sb28[i] = 0;
        sb29[i] = 0;
    }

    for (int ty = 0; ty < MAP_HEIGHT; ty++) {
        for (int tx = 0; tx < MAP_WIDTH; tx++) {
            uint8_t block = level_data[ty][tx];
            /* 16x16 tile is 2x2 of 8x8 tiles */
            int gx0 = tx * 2;
            int gx1 = tx * 2 + 1;
            int gy0 = ty * 2;
            int gy1 = ty * 2 + 1;

            uint16_t t_tl = 0, t_tr = 0, t_bl = 0, t_br = 0;
            if (block == 1) {
                t_tl = 1; /* TL solid */
                t_tr = 2; /* TR solid */
                t_bl = 3; /* BL solid */
                t_br = 4; /* BR solid */
            }

            /* Set TL */
            if (gx0 < 32) sb28[gy0 * 32 + gx0] = t_tl;
            else          sb29[gy0 * 32 + (gx0 - 32)] = t_tl;

            /* Set TR */
            if (gx1 < 32) sb28[gy0 * 32 + gx1] = t_tr;
            else          sb29[gy0 * 32 + (gx1 - 32)] = t_tr;

            /* Set BL */
            if (gx0 < 32) sb28[gy1 * 32 + gx0] = t_bl;
            else          sb29[gy1 * 32 + (gx0 - 32)] = t_bl;

            /* Set BR */
            if (gx1 < 32) sb28[gy1 * 32 + gx1] = t_br;
            else          sb29[gy1 * 32 + (gx1 - 32)] = t_br;
        }
    }
}

/* Reset Game State */
static void reset_game(void) {
    slime.x = TO_FP(192);
    slime.y = TO_FP(128);
    slime.vx = 0;
    slime.vy = 0;
    slime.state = STATE_IDLE;
    slime.facing_left = false;
    slime.is_grounded = false;
    slime.anim_frame = 0;
    slime.anim_timer = 0;
    slime.state_timer = 0;
    slime.anim_step = 0;

    has_super_jump = false;
    has_ground_pound = false;
    super_jump_active = true;
    ground_pound_active = true;

    shake_timer = 0;
    ground_pound_info_timer = 0;

    hud_clear();
    hud_print(1, 1, "LEFT & RIGHT to move", 1);
    hud_print(1, 2, "UP / (A) to jump", 1);
    hud_print(1, 4, "SELECT: Reset", 1);
}

/* Check Solid Collision at (px, py) in world coordinates */
static bool is_solid(int px, int py) {
    if (px < 0 || px >= MAP_WIDTH * TILE_SIZE) return true;
    if (py < 0 || py >= MAP_HEIGHT * TILE_SIZE) return true;
    int tx = px / TILE_SIZE;
    int ty = py / TILE_SIZE;
    return level_data[ty][tx] == 1;
}

/* Move Slime and resolve collision */
static void move_slime(void) {
    /* Horizontal movement */
    slime.x += slime.vx / 60;
    int cur_x = FROM_FP(slime.x);
    int cur_y = FROM_FP(slime.y);

    /* Hitbox: 16x16 pixels */
    if (slime.vx > 0) {
        if (is_solid(cur_x + 15, cur_y + 2) || is_solid(cur_x + 15, cur_y + 14)) {
            cur_x = ((cur_x + 15) / TILE_SIZE) * TILE_SIZE - 16;
            slime.x = TO_FP(cur_x);
            slime.vx = 0;
        }
    } else if (slime.vx < 0) {
        if (is_solid(cur_x, cur_y + 2) || is_solid(cur_x, cur_y + 14)) {
            cur_x = (cur_x / TILE_SIZE + 1) * TILE_SIZE;
            slime.x = TO_FP(cur_x);
            slime.vx = 0;
        }
    }

    /* Vertical movement */
    slime.y += slime.vy / 60;
    cur_x = FROM_FP(slime.x);
    cur_y = FROM_FP(slime.y);
    slime.is_grounded = false;

    if (slime.vy >= 0) {
        /* Moving downwards: check floor at foot corners */
        if (is_solid(cur_x + 2, cur_y + 16) || is_solid(cur_x + 13, cur_y + 16)) {
            cur_y = ((cur_y + 16) / TILE_SIZE) * TILE_SIZE - 16;
            slime.y = TO_FP(cur_y);
            slime.vy = 0;
            slime.is_grounded = true;
        }
    } else {
        /* Moving upwards: check ceiling */
        if (is_solid(cur_x + 2, cur_y) || is_solid(cur_x + 13, cur_y)) {
            cur_y = (cur_y / TILE_SIZE + 1) * TILE_SIZE;
            slime.y = TO_FP(cur_y);
            slime.vy = 0;
        }
    }
}

/* FSM Update */
static void update_slime(void) {
    bool move_left = (key_curr & KEY_LEFT);
    bool move_right = (key_curr & KEY_RIGHT);
    bool jump_pressed = (key_pressed & (KEY_UP | KEY_A));
    bool down_pressed = (key_pressed & KEY_DOWN);

    switch (slime.state) {
        case STATE_IDLE: {
            /* Gravity */
            slime.vy += GRAVITY / 60;
            if (slime.vy > MAX_VEL_Y) slime.vy = MAX_VEL_Y;

            if (move_left) {
                slime.facing_left = true;
                slime.vx -= ACCEL_X / 60;
                if (slime.vx < -MAX_VEL_X) slime.vx = -MAX_VEL_X;
            } else if (move_right) {
                slime.facing_left = false;
                slime.vx += ACCEL_X / 60;
                if (slime.vx > MAX_VEL_X) slime.vx = MAX_VEL_X;
            } else {
                slime.vx = (slime.vx * 9) / 10;
                if (slime.vx > -TO_FP(2) && slime.vx < TO_FP(2)) slime.vx = 0;
            }

            /* Animation: standing (3 fps = 20 frames) vs walking (12 fps = 5 frames) */
            slime.anim_timer++;
            if (move_left || move_right) {
                if (slime.anim_timer >= 5) {
                    slime.anim_timer = 0;
                    slime.anim_frame = (slime.anim_frame == 0) ? 1 : 0;
                    if (slime.anim_frame == 1) snd_walk_tick();
                }
            } else {
                if (slime.anim_timer >= 20) {
                    slime.anim_timer = 0;
                    slime.anim_frame = (slime.anim_frame == 0) ? 1 : 0;
                }
            }

            /* Transition to Jump */
            if (jump_pressed && slime.is_grounded) {
                if (has_super_jump) {
                    slime.state = STATE_SUPER_JUMP;
                    slime.vy = -SUPER_JUMP_IMP;
                    snd_super_jump();
                } else {
                    slime.state = STATE_JUMP;
                    slime.vy = -JUMP_IMPULSE;
                    snd_jump();
                }
                slime.anim_frame = 2; /* jumping frame */
            }
            break;
        }

        case STATE_JUMP:
        case STATE_SUPER_JUMP: {
            /* Gravity */
            slime.vy += GRAVITY / 60;
            if (slime.vy > MAX_VEL_Y) slime.vy = MAX_VEL_Y;

            /* Air control */
            if (move_left) {
                slime.facing_left = true;
                slime.vx -= ACCEL_X / 60;
                if (slime.vx < -MAX_VEL_X) slime.vx = -MAX_VEL_X;
            } else if (move_right) {
                slime.facing_left = false;
                slime.vx += ACCEL_X / 60;
                if (slime.vx > MAX_VEL_X) slime.vx = MAX_VEL_X;
            }

            slime.anim_frame = 2; /* jumping frame */

            /* Transition to GroundPound if unlocked */
            if (has_ground_pound && down_pressed && !slime.is_grounded) {
                slime.state = STATE_GROUND_POUND;
                slime.vx = 0;
                slime.vy = 0;
                slime.state_timer = 0;
                slime.anim_frame = 3; /* pound frame */
                snd_ground_pound_start();
                break;
            }


            /* Transition back to Idle on landing */
            if (slime.is_grounded && slime.vy >= 0) {
                slime.state = STATE_IDLE;
                slime.anim_frame = 0;
                slime.anim_timer = 0;
            }
            break;
        }

        case STATE_GROUND_POUND: {
            slime.anim_frame = 3; /* pound frame */
            slime.vx = 0;
            slime.state_timer++;

            /* Hover for 0.25s (15 frames) then plummet */
            if (slime.state_timer < 15) {
                slime.vy = 0;
            } else {
                slime.vy = GRAVITY; /* 600 px/s slam down */
            }

            if (slime.is_grounded) {
                slime.state = STATE_GROUND_POUND_FINISH;
                slime.anim_frame = 4; /* landing squash */
                slime.anim_step = 0;
                slime.anim_timer = 0;
                shake_timer = 15; /* 0.25s screen shake */
                snd_ground_slam();
            }
            break;
        }

        case STATE_GROUND_POUND_FINISH: {
            slime.vx = 0;
            slime.vy = 0;
            slime.anim_timer++;

            /* landing animation sequence: [4, 0, 1, 0] at 8 fps (7.5 ticks per step) */
            static const uint8_t landing_seq[4] = {4, 0, 1, 0};
            if (slime.anim_timer >= 7) {
                slime.anim_timer = 0;
                slime.anim_step++;
                if (slime.anim_step >= 4) {
                    /* Animation finished: transition to Idle */
                    slime.state = STATE_IDLE;
                    slime.anim_frame = 0;
                } else {
                    slime.anim_frame = landing_seq[slime.anim_step];
                }
            }
            break;
        }
    }

    move_slime();

    /* Check Powerup Collisions */
    int sx = FROM_FP(slime.x);
    int sy = FROM_FP(slime.y);

    /* Super Jump Powerup: (x=48, y=208) */
    if (super_jump_active) {
        if (sx + 14 >= 48 && sx <= 48 + 14 && sy + 14 >= 208 && sy <= 208 + 14) {
            super_jump_active = false;
            has_super_jump = true;
            snd_powerup();
        }
    }

    /* Ground Pound Powerup: (x=272, y=164) */
    if (ground_pound_active) {
        if (sx + 14 >= 272 && sx <= 272 + 14 && sy + 14 >= 164 && sy <= 164 + 14) {
            ground_pound_active = false;
            has_ground_pound = true;
            ground_pound_info_timer = 120; /* Flash HUD info for 2 seconds */
            snd_powerup();
        }
    }
}

/* Update Camera & Shake */
static void update_camera(void) {
    cam_x = FROM_FP(slime.x) - 120 + 8;
    cam_y = FROM_FP(slime.y) - 80 + 8;

    /* Clamp camera to level bounds (320x240 - 240x160 = max 80x80) */
    if (cam_x < 0) cam_x = 0;
    if (cam_x > 80) cam_x = 80;
    if (cam_y < 0) cam_y = 0;
    if (cam_y > 80) cam_y = 80;

    /* Apply screen shake */
    if (shake_timer > 0) {
        shake_timer--;
        int ox = (int)(rand_next() % 5) - 2;
        int oy = (int)(rand_next() % 5) - 2;
        cam_x += ox;
        cam_y += oy;
    }

    REG_BG1HOFS = cam_x;
    REG_BG1VOFS = cam_y;
}

/* Update Sprites in Shadow OAM */
static void update_sprites(void) {
    /* Sprite 0: Slime (16x16, 4bpp, Palette 0) */
    int scr_sx = FROM_FP(slime.x) - cam_x;
    int scr_sy = FROM_FP(slime.y) - cam_y;

    if (scr_sx >= -16 && scr_sx < 240 && scr_sy >= -16 && scr_sy < 160) {
        shadow_oam[0].attr0 = (scr_sy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
        shadow_oam[0].attr1 = (scr_sx & ATTR1_X_MASK) | ATTR1_SIZE_16 | (slime.facing_left ? ATTR1_HFLIP : 0);
        shadow_oam[0].attr2 = (slime.anim_frame * 4) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
    } else {
        shadow_oam[0].attr0 = ATTR0_HIDE;
    }

    /* Sprite 1: Super Jump Powerup (16x16, Tile 20, Palette 1) */
    if (super_jump_active) {
        int px = 48 - cam_x;
        int py = 208 - cam_y;
        if (px >= -16 && px < 240 && py >= -16 && py < 160) {
            shadow_oam[1].attr0 = (py & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
            shadow_oam[1].attr1 = (px & ATTR1_X_MASK) | ATTR1_SIZE_16;
            shadow_oam[1].attr2 = 20 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(1);
        } else {
            shadow_oam[1].attr0 = ATTR0_HIDE;
        }
    } else {
        shadow_oam[1].attr0 = ATTR0_HIDE;
    }

    /* Sprite 2: Ground Pound Powerup (16x16, Tile 20, Palette 1, VFLIP) */
    if (ground_pound_active) {
        int px = 272 - cam_x;
        int py = 164 - cam_y;
        if (px >= -16 && px < 240 && py >= -16 && py < 160) {
            shadow_oam[2].attr0 = (py & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
            shadow_oam[2].attr1 = (px & ATTR1_X_MASK) | ATTR1_SIZE_16 | ATTR1_VFLIP;
            shadow_oam[2].attr2 = 20 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(1);
        } else {
            shadow_oam[2].attr0 = ATTR0_HIDE;
        }
    } else {
        shadow_oam[2].attr0 = ATTR0_HIDE;
    }

    /* Hide unused sprites (3..127) */
    for (int i = 3; i < 128; i++) {
        shadow_oam[i].attr0 = ATTR0_HIDE;
    }
}

/* Copy shadow OAM to hardware OAM during VBlank */
static void flush_oam(void) {
    volatile uint32_t* dst = (volatile uint32_t*)MEM_OAM;
    const uint32_t* src = (const uint32_t*)shadow_oam;
    for (int i = 0; i < 128 * 2; i++) {
        dst[i] = src[i];
    }
}

/* Update HUD text display */
static void update_hud(void) {
    /* Line 3: Ground Pound Hint when unlocked */
    if (has_ground_pound) {
        uint8_t pal = 1;
        if (ground_pound_info_timer > 0) {
            ground_pound_info_timer--;
            /* Flashing yellow effect */
            pal = ((ground_pound_info_timer / 8) % 2 == 0) ? 2 : 1;
        }
        hud_print(1, 3, "DOWN in air: Ground-Pound", pal);
    } else {
        hud_clear_line(3);
    }

    /* Line 5: Current FSM State */
    const char* sname = state_names[slime.state];
    hud_clear_line(5);
    hud_print(1, 5, "State: ", 1);
    hud_print(8, 5, sname, 2);
}

int main(void) {
    /* 1. Load Palettes */
    for (int i = 0; i < 64; i++) {
        MEM_PAL_BG[i] = bg_palette[i];
    }
    for (int i = 0; i < 32; i++) {
        MEM_PAL_OBJ[i] = obj_palette[i];
    }

    /* 2. Load BG Tiles into VRAM */
    /* Charblock 0: Font Tiles */
    volatile uint16_t* cb0 = CHARBLOCK(0);
    const uint16_t* f_src = (const uint16_t*)font_tiles;
    for (int i = 0; i < (int)(sizeof(font_tiles) / 2); i++) {
        cb0[i] = f_src[i];
    }

    /* Charblock 1: Game Terrain Tiles */
    volatile uint16_t* cb1 = CHARBLOCK(1);
    const uint16_t* bg_src = (const uint16_t*)bg_tiles;
    for (int i = 0; i < (int)(sizeof(bg_tiles) / 2); i++) {
        cb1[i] = bg_src[i];
    }

    /* 3. Load OBJ Tiles into VRAM (Charblock 4 = 0x06010000) */
    volatile uint16_t* cb4 = (volatile uint16_t*)0x06010000;
    const uint16_t* obj_src = (const uint16_t*)obj_tiles;
    for (int i = 0; i < (int)(sizeof(obj_tiles) / 2); i++) {
        cb4[i] = obj_src[i];
    }

    /* 4. Configure Backgrounds */
    /* BG0: Text HUD, Charblock 0, Screenblock 31, 32x32, Priority 0 */
    REG_BG0CNT = 0x0000 | (31 << 8) | (0 << 2) | 0;

    /* BG1: Game Map, Charblock 1, Screenblock 28, 64x32 (size 1), Priority 2 */
    REG_BG1CNT = 0x4000 | (28 << 8) | (1 << 2) | 2;

    /* 5. Enable BG0, BG1, OBJ, 1D OBJ mapping, Mode 0 */
    REG_DISPCNT = 0x1000 | 0x0200 | 0x0100 | 0x0040 | 0x0000;

    /* 6. Initialize Sound */
    sound_init();

    /* 7. Initialize Map and Game State */
    init_map();
    reset_game();

    /* Main Loop */
    while (1) {
        /* Read input */
        key_prev = key_curr;
        key_curr = ~REG_KEYINPUT & 0x03FF;
        key_pressed = key_curr & ~key_prev;

        /* Reset button (SELECT or R) */
        if (key_pressed & (KEY_SELECT | KEY_R)) {
            reset_game();
        }

        /* Update logic */
        update_slime();
        update_camera();
        update_sprites();
        update_hud();

        /* Wait for VBlank */
        vsync();

        /* Flush shadow OAM to hardware */
        flush_oam();
    }

    return 0;
}
