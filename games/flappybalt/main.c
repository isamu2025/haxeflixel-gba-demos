/*
 * Flappybalt - GBA Native Port
 * Based on HaxeFlixel Flappybalt demo by Daniel Linssen / HaxeFlixel team
 * Optimized for Game Boy Advance bare-metal ARM7TDMI
 */

#include "gba.h"
#include "assets.h"

/* Fixed-point arithmetic (16.16) */
#define FP_SHIFT        16
#define TO_FP(x)        ((int32_t)((x) * (1 << FP_SHIFT)))
#define TO_FP_INT(x)    ((int32_t)((x) << FP_SHIFT))
#define FROM_FP(x)      ((int32_t)((x) >> FP_SHIFT))

/* Display Dimensions */
#define GBA_SCREEN_W    240
#define GBA_SCREEN_H    160

/* Playfield Geometry (Arcade Fit Mode: 160x160 centered) */
#define PLAYFIELD_X     40
#define PLAYFIELD_W     160
#define PLAYFIELD_H     160
#define ARENA_LEFT      (PLAYFIELD_X + 5)   /* 45 */
#define ARENA_RIGHT     (PLAYFIELD_X + 147) /* 187 */
#define ARENA_TOP       16
#define ARENA_BOTTOM    144

/* Physics Constants (Comfort-tuned for GBA 60FPS) */
#define GRAVITY         TO_FP(250.0f)
#define JUMP_VELOCITY   TO_FP(-155.0f)
#define HORIZ_VELOCITY  TO_FP(62.0f)
#define PADDLE_SPEED    TO_FP(135.0f)
#define BOUNCE_DAMP_NUM 7
#define BOUNCE_DAMP_DEN 10

/* Game Modes */
typedef enum {
    MODE_ARCADE_FIT = 0,    /* 160x160 full view on 240x160 screen */
    MODE_CLASSIC_SCROLL     /* 160x240 original world with camera tracking */
} GameMode;

/* Entity Structures */
typedef struct {
    int32_t x, y;
    int32_t vx, vy;
    int32_t ay;
    uint8_t started;
    uint8_t alive;
    uint8_t flip_x;
    uint8_t anim_frame;     /* 0, 1, 2 */
    uint8_t anim_timer;
    uint8_t anim_playing;
} Player;

typedef struct {
    int32_t x, y;
    int32_t target_y;
    int32_t vy;
    uint8_t active;
    uint8_t facing_left;
} Paddle;

typedef struct {
    int32_t x, y;
    int32_t vx, vy;
    int32_t ay;
    int16_t life;
    uint8_t active;
} Feather;

#define MAX_FEATHERS 10

/* Game State */
static Player player;
static Paddle paddle_left;
static Paddle paddle_right;
static Feather feathers[MAX_FEATHERS];

static uint16_t score = 0;
static uint16_t high_score = 0;
static GameMode current_mode = MODE_ARCADE_FIT;

static uint8_t bounce_flash_l = 0;
static uint8_t bounce_flash_r = 0;
static uint8_t flash_timer = 0;
static uint8_t shake_timer = 0;
static uint8_t death_delay = 0;
static uint32_t frame_counter = 0;
static int32_t camera_y = 0;

/* SRAM Save/Load */
#define SRAM ((volatile uint8_t*)0x0E000000)

static uint16_t load_saved_high_score(void) {
    if (SRAM[0] == 'F' && SRAM[1] == 'B') {
        return (uint16_t)(SRAM[2] | (SRAM[3] << 8));
    }
    return 0;
}

static void save_saved_high_score(uint16_t hs) {
    SRAM[0] = 'F';
    SRAM[1] = 'B';
    SRAM[2] = (uint8_t)(hs & 0xFF);
    SRAM[3] = (uint8_t)((hs >> 8) & 0xFF);
}

/* Pseudo-Random Generator (LCG) */
static uint32_t rng_state = 0x9A3B5C7D;

static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int rnd_range(int min_val, int max_val) {
    if (max_val <= min_val) return min_val;
    return min_val + (int)(rnd() % (max_val - min_val + 1));
}

/* Sound Functions */
static void sound_init(void) {
    REG_SOUNDCNT_X = 0x0080; /* Sound master enable */
    REG_SOUNDCNT_L = 0x0077; /* Max output volume */
    REG_SOUNDCNT_H = 0x0002; /* Enable PSG channels */
}

/* Flap Jump Chirp (Channel 1 upward frequency sweep) */
static void snd_flap(void) {
    REG_SOUND1CNT_L = 0x0014;
    REG_SOUND1CNT_H = 0xF180;
    REG_SOUND1CNT_X = 0x86B0;
}

/* Wall Bounce Bell / Score (Channel 2 metallic ding) */
static void snd_bounce(void) {
    REG_SOUND2CNT_L = 0xF180;
    REG_SOUND2CNT_H = 0x87C0;
}

/* Crash / Explosion Noise (Channel 4 white noise) */
static void snd_crash(void) {
    REG_SOUND4CNT_L = 0xF021;
    REG_SOUND4CNT_H = 0x8050;
}

/* Text Drawing Helper on BG0 */
static void draw_string(int col, int row, const char* str, uint8_t pal) {
    volatile uint16_t* bg0_map = SCREENBLOCK(28);
    int c = col;
    while (*str) {
        if (c < 32 && row < 32) {
            bg0_map[row * 32 + c] = (uint16_t)(*str) | ((pal & 0xF) << 12);
        }
        c++;
        str++;
    }
}

static void draw_number(int col, int row, int val, int digits, uint8_t pal) {
    char buf[12];
    int pos = digits;
    buf[pos] = '\0';
    if (val == 0) {
        for (int i = 0; i < digits - 1; i++) buf[i] = ' ';
        buf[digits - 1] = '0';
    } else {
        while (pos > 0) {
            pos--;
            if (val > 0) {
                buf[pos] = '0' + (val % 10);
                val /= 10;
            } else {
                buf[pos] = ' ';
            }
        }
    }
    draw_string(col, row, buf, pal);
}

/* Build BG0: Arcade Cabinet Side Bezels & HUD Layout */
static void init_bg0_bezels(void) {
    volatile uint16_t* bg0_map = SCREENBLOCK(28);
    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++) {
            bg0_map[r * 32 + c] = 0; /* Transparent center */
        }
    }

    /* Left Bezel: col 0..4 */
    for (int r = 0; r < 20; r++) {
        for (int c = 0; c < 4; c++) {
            bg0_map[r * 32 + c] = (128) | (1 << 12); /* Tile 128: dark fill, pal 1 */
        }
        bg0_map[r * 32 + 4] = (129) | (1 << 12);     /* Tile 129: vertical divider line */
    }

    /* Right Bezel: col 25..29 */
    for (int r = 0; r < 20; r++) {
        bg0_map[r * 32 + 25] = (130) | (1 << 12);    /* Tile 130: vertical divider line */
        for (int c = 26; c < 30; c++) {
            bg0_map[r * 32 + c] = (128) | (1 << 12); /* Tile 128: dark fill, pal 1 */
        }
    }

    /* Left Bezel Text (Cols 0..3) */
    draw_string(1, 1, "GBA", 3);
    draw_string(0, 2, "FLAP", 3);

    draw_string(1, 5, "HI", 1);
    draw_number(0, 6, high_score, 4, 1);

    draw_string(0, 9, "SCOR", 1);
    draw_number(0, 10, 0, 4, 1);

    draw_string(0, 15, "====", 1);
    draw_string(0, 18, "PORT", 1);

    /* Right Bezel Text (Cols 26..29) */
    draw_string(26, 1, "BALT", 3);
    draw_string(26, 2, "====", 3);

    draw_string(26, 5, "[A]", 1);
    draw_string(26, 6, "FLAP", 2);

    draw_string(26, 9, "SEL", 1);
    draw_string(26, 10, "MODE", 2);

    draw_string(26, 13, "STA", 1);
    draw_string(26, 14, "RST", 2);

    draw_string(26, 18, "60FS", 1);
}


/* Build BG1: City Skyline, Top Spikes, Bottom Spikes, Bounce Bars */
static void init_bg1_playfield(void) {
    volatile uint16_t* bg1_map = SCREENBLOCK(29);

    /* Clear all */
    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++) {
            bg1_map[r * 32 + c] = 0;
        }
    }

    if (current_mode == MODE_ARCADE_FIT) {
        /* Arcade Fit (160x160): rows 0..19, cols 5..24 */
        /* Row 0, 1: Top Spikes (VFLIP = 0x0800) */
        for (int c = 0; c < 20; c++) {
            bg1_map[0 * 32 + (5 + c)] = (spike_map[1][c]) | 0x0800;
            bg1_map[1 * 32 + (5 + c)] = (spike_map[0][c]) | 0x0800;
        }

        /* Rows 2..17: City Skyline + Bounce Bars */
        for (int r = 2; r < 18; r++) {
            int bg_row = r + 6; /* Center city silhouette (rows 8..23) */
            for (int c = 0; c < 20; c++) {
                if (c == 0) {
                    bg1_map[r * 32 + (5 + c)] = BOUNCE_BAR_L_TILE;
                } else if (c == 19) {
                    bg1_map[r * 32 + (5 + c)] = BOUNCE_BAR_R_TILE;
                } else {
                    bg1_map[r * 32 + (5 + c)] = bg_city_map[bg_row][c];
                }
            }
        }

        /* Row 18, 19: Bottom Spikes */
        for (int c = 0; c < 20; c++) {
            bg1_map[18 * 32 + (5 + c)] = spike_map[0][c];
            bg1_map[19 * 32 + (5 + c)] = spike_map[1][c];
        }
    } else {
        /* Classic Scroll (160x240): rows 0..29, cols 5..24 */
        /* Row 0, 1: Top Spikes */
        for (int c = 0; c < 20; c++) {
            bg1_map[0 * 32 + (5 + c)] = (spike_map[1][c]) | 0x0800;
            bg1_map[1 * 32 + (5 + c)] = (spike_map[0][c]) | 0x0800;
        }

        /* Rows 2..27: Full 240px City Skyline + Bounce Bars */
        for (int r = 2; r < 28; r++) {
            for (int c = 0; c < 20; c++) {
                if (c == 0) {
                    bg1_map[r * 32 + (5 + c)] = BOUNCE_BAR_L_TILE;
                } else if (c == 19) {
                    bg1_map[r * 32 + (5 + c)] = BOUNCE_BAR_R_TILE;
                } else {
                    bg1_map[r * 32 + (5 + c)] = bg_city_map[r][c];
                }
            }
        }

        /* Row 28, 29: Bottom Spikes */
        for (int c = 0; c < 20; c++) {
            bg1_map[28 * 32 + (5 + c)] = spike_map[0][c];
            bg1_map[29 * 32 + (5 + c)] = spike_map[1][c];
        }
    }
}

/* Update Bounce Bars Flash Animation in BG1 */
static void update_bg1_bounce_bars(void) {
    volatile uint16_t* bg1_map = SCREENBLOCK(29);
    uint16_t l_tile = (bounce_flash_l > 0) ? BOUNCE_FLASH_L_TILE : BOUNCE_BAR_L_TILE;
    uint16_t r_tile = (bounce_flash_r > 0) ? BOUNCE_FLASH_R_TILE : BOUNCE_BAR_R_TILE;

    int max_r = (current_mode == MODE_ARCADE_FIT) ? 18 : 28;
    for (int r = 2; r < max_r; r++) {
        bg1_map[r * 32 + 5] = l_tile;
        bg1_map[r * 32 + 24] = r_tile;
    }
}

/* Reset Game State */
static void reset_game(void) {
    player.x = TO_FP_INT(PLAYFIELD_X + (PLAYFIELD_W - 8) / 2); /* 116 */
    if (current_mode == MODE_ARCADE_FIT) {
        player.y = TO_FP_INT((PLAYFIELD_H - 8) / 2);           /* 76 */
    } else {
        player.y = TO_FP_INT((240 - 8) / 2);                  /* 116 */
    }
    player.vx = 0;
    player.vy = 0;
    player.ay = 0;
    player.started = 0;
    player.alive = 1;
    player.flip_x = 0;
    player.anim_frame = 2; /* Idle glide frame */
    player.anim_timer = 0;
    player.anim_playing = 0;

    paddle_left.x = TO_FP_INT(PLAYFIELD_X + 5);
    paddle_left.y = TO_FP_INT(300); /* Offscreen */
    paddle_left.target_y = TO_FP_INT(300);
    paddle_left.vy = 0;
    paddle_left.active = 0;
    paddle_left.facing_left = 0;

    paddle_right.x = TO_FP_INT(PLAYFIELD_X + 139);
    paddle_right.y = TO_FP_INT(300); /* Offscreen */
    paddle_right.target_y = TO_FP_INT(300);
    paddle_right.vy = 0;
    paddle_right.active = 0;
    paddle_right.facing_left = 1;

    for (int i = 0; i < MAX_FEATHERS; i++) {
        feathers[i].active = 0;
    }

    score = 0;
    bounce_flash_l = 0;
    bounce_flash_r = 0;
    flash_timer = 0;
    shake_timer = 0;
    death_delay = 0;
    camera_y = 0;

    draw_number(0, 10, 0, 4, 1);
    update_bg1_bounce_bars();
}

/* Flap Action */
static void player_flap(void) {
    if (!player.alive) return;

    if (!player.started) {
        player.started = 1;
        player.ay = GRAVITY;
        player.vx = HORIZ_VELOCITY;
        /* Erase "PRESS A" text */
        draw_string(9, 11, "       ", 2);
    }

    player.vy = JUMP_VELOCITY;
    player.anim_playing = 1;
    player.anim_frame = 0;
    player.anim_timer = 0;

    snd_flap();
}

/* Death Trigger */
static void player_die(void) {
    if (!player.alive) return;
    player.alive = 0;
    death_delay = 45; /* 0.75s death sequence */
    flash_timer = 20; /* 20 frames white flash */
    shake_timer = 15; /* 15 frames screen shake */

    snd_crash();

    /* Spawn Feathers */
    int px = FROM_FP(player.x);
    int py = FROM_FP(player.y);
    for (int i = 0; i < MAX_FEATHERS; i++) {
        feathers[i].active = 1;
        feathers[i].x = TO_FP_INT(px + (rnd() % 4));
        feathers[i].y = TO_FP_INT(py + (rnd() % 4));
        feathers[i].vx = TO_FP((float)rnd_range(-35, 35));
        feathers[i].vy = TO_FP((float)rnd_range(-40, 10));
        feathers[i].ay = TO_FP(140.0f);
        feathers[i].life = rnd_range(25, 45);
    }

    /* Check High Score */
    if (score > high_score) {
        high_score = score;
        save_saved_high_score(high_score);
        draw_number(0, 6, high_score, 4, 1);
    }
}

/* Paddle Target Randomizer */
static void randomize_paddle(Paddle* p) {
    p->active = 1;
    int min_y = ARENA_TOP;
    int max_y = (current_mode == MODE_ARCADE_FIT) ? (ARENA_BOTTOM - 48) : (224 - 48);
    int target = rnd_range(min_y, max_y);
    p->target_y = TO_FP_INT(target);

    if (p->y > TO_FP_INT(240)) {
        /* Spawn from below bottom */
        p->y = TO_FP_INT((current_mode == MODE_ARCADE_FIT) ? ARENA_BOTTOM : 224);
    }

    if (p->target_y < p->y) {
        p->vy = -PADDLE_SPEED;
    } else {
        p->vy = PADDLE_SPEED;
    }
}

/* Update Game Logic (60 FPS tick) */
static void update_game(uint16_t keys_pressed, uint16_t keys_just_down) {
    frame_counter++;

    /* SELECT: Toggle Mode */
    if (keys_just_down & KEY_SELECT) {
        current_mode = (current_mode == MODE_ARCADE_FIT) ? MODE_CLASSIC_SCROLL : MODE_ARCADE_FIT;
        init_bg1_playfield();
        reset_game();
        return;
    }

    /* START: Reset */
    if (keys_just_down & KEY_START) {
        reset_game();
        return;
    }

    /* Button A / Up / B: Flap */
    if (keys_just_down & (KEY_A | KEY_B | KEY_UP)) {
        if (player.alive) {
            player_flap();
        }
    }

    /* Blinking "PRESS A" when waiting to start */
    if (!player.started && player.alive) {
        if ((frame_counter % 60) < 35) {
            draw_string(9, 11, "PRESS A", 2);
        } else {
            draw_string(9, 11, "       ", 2);
        }
    }

    /* Update Player Physics */
    if (player.alive && player.started) {
        player.vy += player.ay / 60;
        player.x += player.vx / 60;
        player.y += player.vy / 60;

        /* Wall Bounce: Left */
        if (player.x <= TO_FP_INT(ARENA_LEFT)) {
            player.x = TO_FP_INT(ARENA_LEFT);
            if (player.vx < 0) {
                player.vx = -player.vx;
                player.vy = (player.vy * BOUNCE_DAMP_NUM) / BOUNCE_DAMP_DEN;
                player.flip_x = 0;
                score++;
                draw_number(0, 10, score, 4, 1);
                bounce_flash_l = 8;
                update_bg1_bounce_bars();
                randomize_paddle(&paddle_right);
                snd_bounce();
            }
        }

        /* Wall Bounce: Right */
        if (player.x >= TO_FP_INT(ARENA_RIGHT)) {
            player.x = TO_FP_INT(ARENA_RIGHT);
            if (player.vx > 0) {
                player.vx = -player.vx;
                player.vy = (player.vy * BOUNCE_DAMP_NUM) / BOUNCE_DAMP_DEN;
                player.flip_x = 1;
                score++;
                draw_number(0, 10, score, 4, 1);
                bounce_flash_r = 8;
                update_bg1_bounce_bars();
                randomize_paddle(&paddle_left);
                snd_bounce();
            }
        }

        /* Spikes Collision: Top & Bottom */
        int py = FROM_FP(player.y);
        int death_bottom = (current_mode == MODE_ARCADE_FIT) ? (ARENA_BOTTOM - 8) : (224 - 8);
        if (py <= (ARENA_TOP - 1) || py >= death_bottom) {
            player_die();
        }

        /* Paddle Collisions */
        int px = FROM_FP(player.x);
        /* Dove Hitbox: [px+2, py+2, 4x4] */
        int db_l = px + 2;
        int db_r = px + 6;
        int db_t = py + 2;
        int db_b = py + 6;

        /* Left Paddle Hitbox */
        if (paddle_left.active) {
            int ply = FROM_FP(paddle_left.y);
            int pl_l = PLAYFIELD_X + 5;
            int pl_r = PLAYFIELD_X + 13;
            int pl_t = ply + 4;
            int pl_b = ply + 44;
            if (db_r >= pl_l && db_l <= pl_r && db_b >= pl_t && db_t <= pl_b) {
                player_die();
            }
        }

        /* Right Paddle Hitbox */
        if (paddle_right.active) {
            int pry = FROM_FP(paddle_right.y);
            int pr_l = PLAYFIELD_X + 147;
            int pr_r = PLAYFIELD_X + 155;
            int pr_t = pry + 4;
            int pr_b = pry + 44;
            if (db_r >= pr_l && db_l <= pr_r && db_b >= pr_t && db_t <= pr_b) {
                player_die();
            }
        }
    }

    /* Update Player Flap Animation */
    if (player.anim_playing) {
        player.anim_timer++;
        if (player.anim_timer >= 4) { /* 15 FPS animation cycle */
            player.anim_timer = 0;
            if (player.anim_frame == 0) player.anim_frame = 1;
            else if (player.anim_frame == 1) player.anim_frame = 2;
            else {
                player.anim_frame = 2;
                player.anim_playing = 0;
            }
        }
    }

    /* Update Paddles */
    if (paddle_left.active && paddle_left.vy != 0) {
        paddle_left.y += paddle_left.vy / 60;
        if ((paddle_left.vy < 0 && paddle_left.y <= paddle_left.target_y) ||
            (paddle_left.vy > 0 && paddle_left.y >= paddle_left.target_y)) {
            paddle_left.y = paddle_left.target_y;
            paddle_left.vy = 0;
        }
    }

    if (paddle_right.active && paddle_right.vy != 0) {
        paddle_right.y += paddle_right.vy / 60;
        if ((paddle_right.vy < 0 && paddle_right.y <= paddle_right.target_y) ||
            (paddle_right.vy > 0 && paddle_right.y >= paddle_right.target_y)) {
            paddle_right.y = paddle_right.target_y;
            paddle_right.vy = 0;
        }
    }

    /* Update Bounce Flash Timers */
    if (bounce_flash_l > 0) {
        bounce_flash_l--;
        if (bounce_flash_l == 0) update_bg1_bounce_bars();
    }
    if (bounce_flash_r > 0) {
        bounce_flash_r--;
        if (bounce_flash_r == 0) update_bg1_bounce_bars();
    }

    /* Update Feathers */
    for (int i = 0; i < MAX_FEATHERS; i++) {
        if (feathers[i].active) {
            feathers[i].vy += feathers[i].ay / 60;
            feathers[i].x += feathers[i].vx / 60;
            feathers[i].y += feathers[i].vy / 60;
            feathers[i].life--;
            if (feathers[i].life <= 0) {
                feathers[i].active = 0;
            }
        }
    }

    /* Death Delay & Respawn */
    if (!player.alive) {
        if (death_delay > 0) {
            death_delay--;
            if (death_delay == 0) {
                reset_game();
            }
        }
    }

    /* Camera Tracking in Classic Scroll Mode */
    if (current_mode == MODE_CLASSIC_SCROLL) {
        int py = FROM_FP(player.y);
        int target_cam_y = py - 76;
        if (target_cam_y < 0) target_cam_y = 0;
        if (target_cam_y > 80) target_cam_y = 80;
        camera_y = target_cam_y;
    } else {
        camera_y = 0;
    }

    /* Screen Shake & Flash Timers */
    if (shake_timer > 0) shake_timer--;
    if (flash_timer > 0) flash_timer--;
}

/* Render to OAM and Video Registers */
static void render_game(void) {
    /* 1. Screen Scroll / Shake */
    int shake_y = 0;
    if (shake_timer > 0) {
        shake_y = (rnd() % 5) - 2;
    }
    REG_BG1VOFS = (uint16_t)(camera_y + shake_y);
    REG_BG1HOFS = 0;
    REG_BG0VOFS = 0;
    REG_BG0HOFS = 0;

    /* 2. White Screen Flash via Hardware Blending */
    if (flash_timer > 0) {
        /* Blend all layers with white backdrop */
        REG_BLDCNT = 0x00BF; /* Blend 1st target: BG0, BG1, OBJ to White */
        uint16_t evy = (flash_timer * 16) / 20;
        REG_BLDY = evy;
    } else {
        REG_BLDCNT = 0;
        REG_BLDY = 0;
    }

    /* 3. OAM Sprites */
    volatile OBJ_ATTR* oam = MEM_OAM;

    /* Sprite 0: Dove (Player) */
    if (player.alive) {
        int px = FROM_FP(player.x);
        int py = FROM_FP(player.y) - camera_y;

        uint16_t a0 = (py & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
        uint16_t a1 = (px & ATTR1_X_MASK) | ATTR1_SIZE_8;
        if (player.flip_x) a1 |= ATTR1_HFLIP;
        uint16_t a2 = (DOVE_SPRITE_TILE + player.anim_frame) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(0);

        oam[0].attr0 = a0;
        oam[0].attr1 = a1;
        oam[0].attr2 = a2;
    } else {
        oam[0].attr0 = ATTR0_HIDE;
    }

    /* Sprite 1 & 2: Left Paddle (16x48 = 16x32 + 16x16) */
    if (paddle_left.active) {
        int lx = FROM_FP(paddle_left.x);
        int ly = FROM_FP(paddle_left.y) - camera_y;

        if (ly >= -48 && ly < GBA_SCREEN_H) {
            /* Upper 16x32 */
            oam[1].attr0 = (ly & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_TALL;
            oam[1].attr1 = (lx & ATTR1_X_MASK) | ATTR1_SIZE_32;
            oam[1].attr2 = (PADDLE_SPRITE_TILE) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(1);

            /* Lower 16x16 */
            oam[2].attr0 = ((ly + 32) & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
            oam[2].attr1 = (lx & ATTR1_X_MASK) | ATTR1_SIZE_16;
            oam[2].attr2 = (PADDLE_SPRITE_TILE + 8) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(1);
        } else {
            oam[1].attr0 = ATTR0_HIDE;
            oam[2].attr0 = ATTR0_HIDE;
        }
    } else {
        oam[1].attr0 = ATTR0_HIDE;
        oam[2].attr0 = ATTR0_HIDE;
    }

    /* Sprite 3 & 4: Right Paddle (16x48, HFLIP) */
    if (paddle_right.active) {
        int rx = FROM_FP(paddle_right.x);
        int ry = FROM_FP(paddle_right.y) - camera_y;

        if (ry >= -48 && ry < GBA_SCREEN_H) {
            /* Upper 16x32 */
            oam[3].attr0 = (ry & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_TALL;
            oam[3].attr1 = (rx & ATTR1_X_MASK) | ATTR1_SIZE_32 | ATTR1_HFLIP;
            oam[3].attr2 = (PADDLE_SPRITE_TILE) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(1);

            /* Lower 16x16 */
            oam[4].attr0 = ((ry + 32) & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
            oam[4].attr1 = (rx & ATTR1_X_MASK) | ATTR1_SIZE_16 | ATTR1_HFLIP;
            oam[4].attr2 = (PADDLE_SPRITE_TILE + 8) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(1);
        } else {
            oam[3].attr0 = ATTR0_HIDE;
            oam[4].attr0 = ATTR0_HIDE;
        }
    } else {
        oam[3].attr0 = ATTR0_HIDE;
        oam[4].attr0 = ATTR0_HIDE;
    }

    /* Sprites 5..14: Feathers */
    for (int i = 0; i < MAX_FEATHERS; i++) {
        if (feathers[i].active) {
            int fx = FROM_FP(feathers[i].x);
            int fy = FROM_FP(feathers[i].y) - camera_y;

            if (fx >= 0 && fx < GBA_SCREEN_W && fy >= 0 && fy < GBA_SCREEN_H) {
                oam[5 + i].attr0 = (fy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
                oam[5 + i].attr1 = (fx & ATTR1_X_MASK) | ATTR1_SIZE_8;
                oam[5 + i].attr2 = (FEATHER_SPRITE_TILE) | ATTR2_PRIORITY(0) | ATTR2_PALETTE(2);
            } else {
                oam[5 + i].attr0 = ATTR0_HIDE;
            }
        } else {
            oam[5 + i].attr0 = ATTR0_HIDE;
        }
    }
}

/* Main Entry Point */
int main(void) {
    /* 1. Setup Video Registers: Mode 0, BG0, BG1, OBJ, 1D OBJ mapping */
    REG_DISPCNT = 0x1340; /* Mode 0, OBJ Enable, BG0 Enable, BG1 Enable, 1D Mapping */
    
    /* BG0: Priority 0 (Topmost HUD/Bezels), Charblock 0, Screenblock 28 */
    REG_BG0CNT = 0x1C00; /* Charblock 0 (bits 2-3 = 0), Screenblock 28 (bits 8-12 = 28) */

    /* BG1: Priority 1 (City & Spikes), Charblock 1, Screenblock 29 */
    REG_BG1CNT = 0x1D05; /* Priority 1, Charblock 1 (bit 2 = 1), Screenblock 29 (bits 8-12 = 29) */

    /* 2. Load Palettes */
    for (int i = 0; i < 64; i++) {
        MEM_PAL_BG[i] = bg_palette[i];
        MEM_PAL_OBJ[i] = obj_palette[i];
    }

    /* 3. Load Charblock 0: Font Tiles (0..127) + Bezel Tiles (128..130) */
    volatile uint16_t* cb0 = CHARBLOCK(0);
    const uint16_t* font16 = (const uint16_t*)font_tiles;
    for (int i = 0; i < (FONT_TILES_COUNT * 32) / 2; i++) {
        cb0[i] = font16[i];
    }
    const uint16_t* bezel16 = (const uint16_t*)bezel_tiles;
    int cb0_offset = (FONT_TILES_COUNT * 32) / 2;
    for (int i = 0; i < (BEZEL_TILES_COUNT * 32) / 2; i++) {
        cb0[cb0_offset + i] = bezel16[i];
    }

    /* 4. Load Charblock 1: BG Tiles (City, Spikes, Bounce Bars) */
    volatile uint16_t* cb1 = CHARBLOCK(1);
    const uint16_t* bg16 = (const uint16_t*)bg_tiles;
    for (int i = 0; i < (BG_TILES_COUNT * 32) / 2; i++) {
        cb1[i] = bg16[i];
    }

    /* 5. Load Charblock 4: OBJ Sprite Tiles (Dove, Feathers, Paddles) */
    volatile uint16_t* cb4 = CHARBLOCK(4);
    const uint16_t* obj16 = (const uint16_t*)obj_tiles;
    for (int i = 0; i < (OBJ_TILES_COUNT * 32) / 2; i++) {
        cb4[i] = obj16[i];
    }

    /* 6. Hide all 128 OAM Sprites */
    for (int i = 0; i < 128; i++) {
        MEM_OAM[i].attr0 = ATTR0_HIDE;
        MEM_OAM[i].attr1 = 0;
        MEM_OAM[i].attr2 = 0;
        MEM_OAM[i].fill = 0;
    }

    /* 7. Sound System Init */
    sound_init();

    /* 8. Load High Score from SRAM */
    high_score = load_saved_high_score();

    /* 9. Build Screenblocks */
    init_bg0_bezels();
    init_bg1_playfield();

    /* 10. Reset Game Entities */
    reset_game();

    /* Key Input Tracker */
    uint16_t prev_keys = 0x03FF;

    /* Main Game Loop (Rock-solid 60 FPS) */
    while (1) {
        uint16_t raw_keys = REG_KEYINPUT & 0x03FF;
        uint16_t keys_pressed = ~raw_keys & 0x03FF;
        uint16_t keys_just_down = (~raw_keys & prev_keys) & 0x03FF;
        prev_keys = raw_keys;

        update_game(keys_pressed, keys_just_down);

        vsync();

        render_game();
    }

    return 0;
}
