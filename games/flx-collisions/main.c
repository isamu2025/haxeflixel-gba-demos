#include "gba.h"
#include "assets.h"

#define FP_SHIFT        16
#define TO_FP(x)        ((int32_t)((x) << FP_SHIFT))
#define FROM_FP(x)      ((int32_t)((x) >> FP_SHIFT))

typedef enum {
    DEMO_WAREHOUSE = 0,
    DEMO_BLOCKS,
    DEMO_PARTS_RAIN,
    DEMO_COUNT
} DemoMode;

/* RNG */
static uint32_t rng_state = 0x92D68CA1;
static uint32_t rnd(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}
static int rnd_range(int min_val, int max_val) {
    return min_val + (int)(rnd() % (max_val - min_val + 1));
}

/* Sound */
static void sound_init(void) {
    REG_SOUNDCNT_X = 0x0080;
    REG_SOUNDCNT_L = 0x0077;
    REG_SOUNDCNT_H = 0x0002;
}
static void snd_jump(void) {
    REG_SOUND1CNT_L = 0x0014;
    REG_SOUND1CNT_H = 0xF180;
    REG_SOUND1CNT_X = 0x8680;
}
static void snd_mode(void) {
    REG_SOUND2CNT_L = 0xF180;
    REG_SOUND2CNT_H = 0x87C0;
}
static void snd_bounce(void) {
    REG_SOUND4CNT_L = 0x2010;
    REG_SOUND4CNT_H = 0x8052;
}

/* Input state */
static uint16_t key_curr = 0;
static uint16_t key_prev = 0;
static uint16_t key_pressed = 0;

/* Shadow OAM */
static OBJ_ATTR shadow_oam[128];
static void flush_oam(void) {
    volatile uint32_t* dst = (volatile uint32_t*)MEM_OAM;
    const uint32_t* src = (const uint32_t*)shadow_oam;
    for (int i = 0; i < 128 * 2; i++) {
        dst[i] = src[i];
    }
}

/* HUD functions */
static void hud_clear(void) {
    volatile uint16_t* sb = SCREENBLOCK(31);
    for (int i = 0; i < 32 * 32; i++) sb[i] = 0;
}
static void hud_print(int x, int y, const char* str, uint8_t pal) {
    volatile uint16_t* sb = SCREENBLOCK(31);
    int i = 0;
    while (str[i] && (x + i) < 30) {
        sb[y * 32 + (x + i)] = ((uint8_t)str[i]) | (pal << 12);
        i++;
    }
}

/* Camera */
static int32_t cam_x = 0;
static int32_t cam_y = 0;

/* Demo 1: Warehouse Entities */
typedef struct {
    int32_t x, y;
    int32_t vx, vy;
    bool is_grounded;
    bool facing_left;
    uint8_t anim_frame;
    uint8_t anim_timer;
    uint8_t anim_step;
} Player;

typedef struct {
    int32_t x, y;
    int32_t vy;
    int dir;
} Elevator;

typedef struct {
    int32_t x, y;
    int32_t vx;
    int dir;
} Pusher;

typedef struct {
    int32_t x, y;
    int32_t vx, vy;
    bool is_grounded;
} Crate;

#define MAX_GIBS 32
typedef struct {
    int32_t x, y;
    int32_t vx, vy;
    uint8_t type;
    uint16_t life;
    bool active;
} Gib;

static Player player;
static Elevator elevator;
static Pusher pusher;
static Crate crates[5];
static Gib gibs[MAX_GIBS];
static uint16_t gib_spawn_timer = 0;
static DemoMode current_demo = DEMO_WAREHOUSE;

/* Demo 2 & 3 State */
static int32_t focus_x = 0, focus_y = 0;
static int32_t paddle_x = 0;

/* Runtime dynamic collision grid (40x30 tiles) */
static uint8_t dynamic_collision[MAP_ROWS][MAP_COLS];

/* Map solid check (8x8 tiles) */
static bool is_solid(int px, int py) {
    if (px < 0 || px >= MAP_COLS * 8) return true;
    if (py < 0 || py >= MAP_ROWS * 8) return true;
    int tx = px / 8;
    int ty = py / 8;
    return dynamic_collision[ty][tx] == 1;
}

/* Resolve crate horizontal overlap against solid tiles */
static void resolve_crate_solid_x(int i) {
    int cx = FROM_FP(crates[i].x);
    int cy = FROM_FP(crates[i].y);
    int cw = 10, ch = 10;

    int y_top = cy + 1;
    int y_bot = cy + ch - 2;
    int y_mid = cy + ch / 2;

    /* Check right edge */
    if (is_solid(cx + cw - 1, y_top) || is_solid(cx + cw - 1, y_bot) || is_solid(cx + cw - 1, y_mid)) {
        cx = ((cx + cw - 1) / 8) * 8 - cw;
        crates[i].x = TO_FP(cx);
        crates[i].vx = 0;
    }
    /* Check left edge */
    if (is_solid(cx, y_top) || is_solid(cx, y_bot) || is_solid(cx, y_mid)) {
        cx = (cx / 8 + 1) * 8;
        crates[i].x = TO_FP(cx);
        crates[i].vx = 0;
    }
}

/* Resolve crate vertical overlap against solid tiles */
static void resolve_crate_solid_y(int i) {
    int cx = FROM_FP(crates[i].x);
    int cy = FROM_FP(crates[i].y);
    int cw = 10, ch = 10;

    int x_left = cx + 1;
    int x_right = cx + cw - 2;
    int x_mid = cx + cw / 2;

    if (crates[i].vy >= 0) {
        /* Falling */
        if (is_solid(x_left, cy + ch - 1) || is_solid(x_right, cy + ch - 1) || is_solid(x_mid, cy + ch - 1)) {
            cy = ((cy + ch - 1) / 8) * 8 - ch;
            crates[i].y = TO_FP(cy);
            crates[i].vy = 0;
            crates[i].is_grounded = true;
        }
    } else {
        /* Rising (e.g. ceiling check) */
        if (is_solid(x_left, cy) || is_solid(x_right, cy) || is_solid(x_mid, cy)) {
            cy = (cy / 8 + 1) * 8;
            crates[i].y = TO_FP(cy);
            crates[i].vy = 0;
        }
    }
}

/* Resolve player horizontal overlap against solid tiles */
static void resolve_player_solid_x(void) {
    int p_ix = FROM_FP(player.x) + 4;
    int p_iy = FROM_FP(player.y) + 4;
    int p_w = 8, p_h = 10;

    int y_top = p_iy + 1;
    int y_bot = p_iy + p_h - 2;
    int y_mid = p_iy + p_h / 2;

    if (is_solid(p_ix + p_w - 1, y_top) || is_solid(p_ix + p_w - 1, y_bot) || is_solid(p_ix + p_w - 1, y_mid)) {
        p_ix = ((p_ix + p_w - 1) / 8) * 8 - p_w;
        player.x = TO_FP(p_ix - 4);
        player.vx = 0;
    }
    if (is_solid(p_ix, y_top) || is_solid(p_ix, y_bot) || is_solid(p_ix, y_mid)) {
        p_ix = (p_ix / 8 + 1) * 8;
        player.x = TO_FP(p_ix - 4);
        player.vx = 0;
    }
}

/* Initialize Background 1 with autotiles */
static void load_warehouse_tilemap(void) {
    volatile uint16_t* sb28 = SCREENBLOCK(28);
    volatile uint16_t* sb29 = SCREENBLOCK(29);

    for (int i = 0; i < 1024; i++) {
        sb28[i] = 0;
        sb29[i] = 0;
    }

    for (int r = 0; r < MAP_ROWS; r++) {
        for (int c = 0; c < MAP_COLS; c++) {
            uint16_t tile = level_autotiles[r][c];
            if (c < 32) sb28[r * 32 + c] = tile;
            else        sb29[r * 32 + (c - 32)] = tile;
        }
    }
}

static void spawn_gib(int sx, int sy, int vx_min, int vx_max, int vy_min, int vy_max) {
    for (int i = 0; i < MAX_GIBS; i++) {
        if (!gibs[i].active) {
            gibs[i].active = true;
            gibs[i].x = TO_FP(sx);
            gibs[i].y = TO_FP(sy);
            gibs[i].vx = TO_FP(rnd_range(vx_min, vx_max));
            gibs[i].vy = TO_FP(rnd_range(vy_min, vy_max));
            gibs[i].type = rnd() % 4;
            gibs[i].life = 400; /* ~6.6 seconds */
            break;
        }
    }
}

/* Setup Demo Modes */
static void init_demo(DemoMode mode) {
    current_demo = mode;
    snd_mode();
    hud_clear();

    /* Clear all gibs */
    for (int i = 0; i < MAX_GIBS; i++) gibs[i].active = false;
    gib_spawn_timer = 0;

    if (mode == DEMO_WAREHOUSE) {
        load_warehouse_tilemap();
        for (int r = 0; r < MAP_ROWS; r++) {
            for (int c = 0; c < MAP_COLS; c++) {
                dynamic_collision[r][c] = level_collision[r][c];
            }
        }
        /* Setup Player: starts at (32, 176) */
        player.x = TO_FP(32);
        player.y = TO_FP(176);
        player.vx = 0;
        player.vy = 0;
        player.is_grounded = false;
        player.facing_left = false;
        player.anim_frame = 0;
        player.anim_timer = 0;
        player.anim_step = 0;

        /* Setup Elevator: (208, 80), oscillates down to 192 */
        elevator.x = TO_FP(208);
        elevator.y = TO_FP(80);
        elevator.vy = TO_FP(40);
        elevator.dir = 1; /* 1 = down, -1 = up */

        /* Setup Pusher: (96, 208), oscillates right to 152 */
        pusher.x = TO_FP(96);
        pusher.y = TO_FP(208);
        pusher.vx = TO_FP(40);
        pusher.dir = 1;

        /* Setup 5 Crates */
        static const int crate_init[5][2] = {
            {64, 208}, {108, 176}, {140, 176}, {192, 208}, {272, 48}
        };
        for (int i = 0; i < 5; i++) {
            crates[i].x = TO_FP(crate_init[i][0]);
            crates[i].y = TO_FP(crate_init[i][1]);
            crates[i].vx = 0;
            crates[i].vy = 0;
            crates[i].is_grounded = false;
        }

        /* Clean non-obstructive HUD */
        hud_print(1, 1, "Warehouse", 2);
        hud_print(1, 19, "A:Jump  START:Next  SEL:Reset", 1);
    } else if (mode == DEMO_BLOCKS) {
        volatile uint16_t* sb28 = SCREENBLOCK(28);
        volatile uint16_t* sb29 = SCREENBLOCK(29);
        for (int i = 0; i < 1024; i++) {
            sb28[i] = 0;
            sb29[i] = 0;
        }
        for (int r = 0; r < MAP_ROWS; r++) {
            for (int c = 0; c < MAP_COLS; c++) {
                dynamic_collision[r][c] = 0;
            }
        }

        /* 1. Boundary solid walls around the 320x240 map */
        for (int c = 0; c < MAP_COLS; c++) {
            dynamic_collision[0][c] = 1;
            dynamic_collision[MAP_ROWS - 1][c] = 1;
            if (c < 32) {
                sb28[0 * 32 + c] = 15;
                sb28[(MAP_ROWS - 1) * 32 + c] = 15;
            } else {
                sb29[0 * 32 + (c - 32)] = 15;
                sb29[(MAP_ROWS - 1) * 32 + (c - 32)] = 15;
            }
        }
        for (int r = 0; r < MAP_ROWS; r++) {
            dynamic_collision[r][0] = 1;
            dynamic_collision[r][MAP_COLS - 1] = 1;
            sb28[r * 32 + 0] = 15;
            sb29[r * 32 + (MAP_COLS - 1 - 32)] = 15;
        }

        /* 2. Dense 16x16 rigid blocks (like HaxeFlixel 300 Blocks) */
        for (int i = 0; i < 110; i++) {
            int bx = rnd_range(1, 18) * 2; /* col 2, 4, ..., 36 */
            int by = rnd_range(1, 13) * 2; /* row 2, 4, ..., 26 */

            /* Keep center particle dispenser area clear (cols 18..22, rows 13..17) */
            if (bx >= 18 && bx <= 22 && by >= 12 && by <= 16) continue;

            for (int dr = 0; dr < 2; dr++) {
                for (int dc = 0; dc < 2; dc++) {
                    int r = by + dr;
                    int c = bx + dc;
                    dynamic_collision[r][c] = 1;
                    if (c < 32) sb28[r * 32 + c] = 15;
                    else        sb29[r * 32 + (c - 32)] = 15;
                }
            }
        }

        focus_x = TO_FP(160);
        focus_y = TO_FP(120);

        hud_print(1, 1, "300 Blocks Universe", 2);
        hud_print(1, 19, "D-PAD:Camera  START:Next", 1);
    } else if (mode == DEMO_PARTS_RAIN) {
        /* Clear background map and collisions */
        volatile uint16_t* sb28 = SCREENBLOCK(28);
        volatile uint16_t* sb29 = SCREENBLOCK(29);
        for (int i = 0; i < 1024; i++) {
            sb28[i] = 0;
            sb29[i] = 0;
        }
        for (int r = 0; r < MAP_ROWS; r++) {
            for (int c = 0; c < MAP_COLS; c++) {
                dynamic_collision[r][c] = 0;
            }
        }
        paddle_x = TO_FP(95);

        hud_print(1, 1, "Parts Rain", 2);
        hud_print(1, 19, "<- ->:Paddle  A:Boost  START:Next", 1);
    }
}

/* Update Demo 1: Warehouse */
static void update_warehouse(void) {
    /* 1. Elevator logic (moves smoothly between y=80 and y=192 at 40 px/s) */
    int32_t prev_ey = elevator.y;
    int32_t edy = (elevator.vy / 60) * elevator.dir;
    elevator.y += edy;

    if (elevator.y >= TO_FP(192)) {
        elevator.y = TO_FP(192);
        elevator.dir = -1;
    } else if (elevator.y <= TO_FP(80)) {
        elevator.y = TO_FP(80);
        elevator.dir = 1;
    }
    int32_t actual_edy = elevator.y - prev_ey;

    /* 2. Pusher logic (moves smoothly between x=96 and x=152 at 40 px/s) */
    int32_t pdx = (pusher.vx / 60) * pusher.dir;
    pusher.x += pdx;

    if (pusher.x >= TO_FP(152)) {
        pusher.x = TO_FP(152);
        pusher.dir = -1;
    } else if (pusher.x <= TO_FP(96)) {
        pusher.x = TO_FP(96);
        pusher.dir = 1;
    }

    /* 3. Player Movement & Physics */
    bool left = (key_curr & KEY_LEFT);
    bool right = (key_curr & KEY_RIGHT);
    bool jump = (key_pressed & (KEY_UP | KEY_A));

    /* Acceleration & Drag */
    if (left) {
        player.vx -= TO_FP(400) / 60;
        if (player.vx < -TO_FP(100)) player.vx = -TO_FP(100);
        player.facing_left = true;
    } else if (right) {
        player.vx += TO_FP(400) / 60;
        if (player.vx > TO_FP(100)) player.vx = TO_FP(100);
        player.facing_left = false;
    } else {
        /* Drag decelerates smoothly */
        if (player.vx > 0) {
            player.vx -= TO_FP(400) / 60;
            if (player.vx < 0) player.vx = 0;
        } else if (player.vx < 0) {
            player.vx += TO_FP(400) / 60;
            if (player.vx > 0) player.vx = 0;
        }
    }

    /* Gravity */
    player.vy += TO_FP(400) / 60;
    if (player.vy > TO_FP(400)) player.vy = TO_FP(400);

    /* Jump */
    if (jump && player.is_grounded) {
        player.vy = -TO_FP(204);
        snd_jump();
    }

    /* Resolve Player X against map */
    player.x += player.vx / 60;
    resolve_player_solid_x();

    /* Resolve Player Y against map */
    player.y += player.vy / 60;
    int p_ix = FROM_FP(player.x) + 4;
    int p_iy = FROM_FP(player.y) + 4;
    int p_w = 8, p_h = 10;
    player.is_grounded = false;

    if (player.vy >= 0) {
        if (is_solid(p_ix + 1, p_iy + p_h) || is_solid(p_ix + p_w - 2, p_iy + p_h)) {
            p_iy = ((p_iy + p_h) / 8) * 8 - p_h;
            player.y = TO_FP(p_iy - 4);
            player.vy = 0;
            player.is_grounded = true;
        }
    } else {
        if (is_solid(p_ix + 1, p_iy) || is_solid(p_ix + p_w - 2, p_iy)) {
            p_iy = (p_iy / 8 + 1) * 8;
            player.y = TO_FP(p_iy - 4);
            player.vy = 0;
        }
    }

    /* 4. Elevator collision with Player */
    int ex = FROM_FP(elevator.x);
    int ey = FROM_FP(elevator.y);
    int ew = 50;
    p_ix = FROM_FP(player.x) + 4;
    p_iy = FROM_FP(player.y) + 4;

    /* Stand on or carried by elevator */
    if (p_ix + p_w > ex && p_ix < ex + ew) {
        if (p_iy + p_h >= ey - 2 && p_iy + p_h <= ey + 8 && player.vy >= 0) {
            player.y = TO_FP(ey - p_h - 4);
            player.vy = 0;
            player.is_grounded = true;
            player.y += actual_edy;
        }
    }

    /* 5. Pusher collision with Player (with strict solid wall resolution) */
    int px = FROM_FP(pusher.x);
    int py = FROM_FP(pusher.y);
    int pw = 8, ph = 16;
    p_ix = FROM_FP(player.x) + 4;
    p_iy = FROM_FP(player.y) + 4;

    if (p_iy + p_h > py && p_iy < py + ph) {
        if (p_ix < px + pw && p_ix + p_w > px) {
            if (pusher.dir > 0) {
                player.x = TO_FP(px + pw - 4);
            } else {
                player.x = TO_FP(px - p_w - 4);
            }
            resolve_player_solid_x();
        }
    }

    /* 6. Crates Physics and Collisions */
    for (int i = 0; i < 5; i++) {
        /* Gravity & Drag */
        crates[i].vy += TO_FP(400) / 60;
        if (crates[i].vy > TO_FP(400)) crates[i].vy = TO_FP(400);

        if (crates[i].vx > 0) {
            crates[i].vx -= TO_FP(200) / 60;
            if (crates[i].vx < 0) crates[i].vx = 0;
        } else if (crates[i].vx < 0) {
            crates[i].vx += TO_FP(200) / 60;
            if (crates[i].vx > 0) crates[i].vx = 0;
        }

        /* Move X */
        crates[i].x += crates[i].vx / 60;
        resolve_crate_solid_x(i);

        /* Move Y */
        crates[i].y += crates[i].vy / 60;
        resolve_crate_solid_y(i);

        int cx = FROM_FP(crates[i].x);
        int cy = FROM_FP(crates[i].y);
        int cw = 10, ch = 10;

        /* Elevator vs Crate (ride elevator smoothly) */
        if (cx + cw > ex && cx < ex + ew) {
            if (cy + ch >= ey - 2 && cy + ch <= ey + 8 && crates[i].vy >= 0) {
                cy = ey - ch;
                crates[i].y = TO_FP(cy);
                crates[i].vy = 0;
                crates[i].is_grounded = true;
                crates[i].y += actual_edy;
            }
        }

        /* Pusher vs Crate (with strict solid wall resolution, preventing wall pass-through) */
        cx = FROM_FP(crates[i].x);
        cy = FROM_FP(crates[i].y);
        if (cy + ch > py && cy < py + ph) {
            if (cx < px + pw && cx + cw > px) {
                if (pusher.dir > 0) {
                    int target_cx = px + pw;
                    crates[i].x = TO_FP(target_cx);
                    resolve_crate_solid_x(i);
                    int resolved_cx = FROM_FP(crates[i].x);
                    if (resolved_cx < target_cx) {
                        crates[i].x = TO_FP(resolved_cx);
                    }
                } else {
                    int target_cx = px - cw;
                    crates[i].x = TO_FP(target_cx);
                    resolve_crate_solid_x(i);
                    int resolved_cx = FROM_FP(crates[i].x);
                    if (resolved_cx > target_cx) {
                        crates[i].x = TO_FP(resolved_cx);
                    }
                }
            }
        }

        /* Player vs Crate interaction (pushing and standing) */
        p_ix = FROM_FP(player.x) + 4;
        p_iy = FROM_FP(player.y) + 4;
        cx = FROM_FP(crates[i].x);
        cy = FROM_FP(crates[i].y);

        /* 1. Standing on crate: player bottom rests on crate top */
        if (p_ix + p_w > cx + 1 && p_ix < cx + cw - 1 &&
            p_iy + p_h >= cy && p_iy + p_h <= cy + 6 && player.vy >= 0) {
            player.y = TO_FP(cy - p_h - 4);
            player.vy = 0;
            player.is_grounded = true;
        }
        /* 2. Pushing crate from left to right */
        else if (player.vx > 0 && p_ix + p_w >= cx && p_ix < cx &&
                 p_iy + p_h > cy + 1 && p_iy < cy + ch - 1) {
            int target_cx = p_ix + p_w;
            crates[i].x = TO_FP(target_cx);
            crates[i].vx = player.vx;
            resolve_crate_solid_x(i);
            int resolved_cx = FROM_FP(crates[i].x);
            if (resolved_cx < target_cx) {
                player.x = TO_FP(resolved_cx - p_w - 4);
                player.vx = 0;
            }
        }
        /* 3. Pushing crate from right to left */
        else if (player.vx < 0 && p_ix <= cx + cw && p_ix + p_w > cx + cw &&
                 p_iy + p_h > cy + 1 && p_iy < cy + ch - 1) {
            int target_cx = p_ix - cw;
            crates[i].x = TO_FP(target_cx);
            crates[i].vx = player.vx;
            resolve_crate_solid_x(i);
            int resolved_cx = FROM_FP(crates[i].x);
            if (resolved_cx > target_cx) {
                player.x = TO_FP(resolved_cx + cw - 4);
                player.vx = 0;
            }
        }

        /* Crate vs other Crates (Stacking & Pushing) */
        for (int j = 0; j < 5; j++) {
            if (i == j) continue;
            int cx2 = FROM_FP(crates[j].x);
            int cy2 = FROM_FP(crates[j].y);
            int cw2 = 10, ch2 = 10;
            cx = FROM_FP(crates[i].x);
            cy = FROM_FP(crates[i].y);

            if (cx + cw > cx2 && cx < cx2 + cw2 && cy + ch > cy2 && cy < cy2 + ch2) {
                /* Crate i landing on top of Crate j */
                if (crates[i].vy >= 0 && cy + ch >= cy2 && cy + ch <= cy2 + 6 && cx + cw - 2 > cx2 && cx + 2 < cx2 + cw2) {
                    cy = cy2 - ch;
                    crates[i].y = TO_FP(cy);
                    crates[i].vy = 0;
                    crates[i].is_grounded = true;
                }
                /* Horizontal push */
                else if (cx < cx2) {
                    int target_j = cx + cw;
                    crates[j].x = TO_FP(target_j);
                    crates[j].vx = crates[i].vx;
                    resolve_crate_solid_x(j);
                    int actual_j = FROM_FP(crates[j].x);
                    if (cx + cw > actual_j) {
                        cx = actual_j - cw;
                        crates[i].x = TO_FP(cx);
                        crates[i].vx = 0;
                    }
                } else {
                    int target_j = cx - cw2;
                    crates[j].x = TO_FP(target_j);
                    crates[j].vx = crates[i].vx;
                    resolve_crate_solid_x(j);
                    int actual_j = FROM_FP(crates[j].x);
                    if (actual_j + cw2 > cx) {
                        cx = actual_j + cw2;
                        crates[i].x = TO_FP(cx);
                        crates[i].vx = 0;
                    }
                }
            }
        }
    }

    /* 7. Gibs Dispenser at (32, 40..75) */
    gib_spawn_timer++;
    if (gib_spawn_timer >= 6) {
        gib_spawn_timer = 0;
        /* Matches HaxeFlixel FlxEmitter: size (8, 40), velocity (140..260, -40..45) */
        spawn_gib(rnd_range(32, 40), rnd_range(40, 75), 140, 260, -40, 45);
    }

    /* Update Gibs with full 4-directional map & object collision */
    for (int i = 0; i < MAX_GIBS; i++) {
        if (!gibs[i].active) continue;
        gibs[i].life--;
        if (gibs[i].life == 0) {
            gibs[i].active = false;
            continue;
        }

        /* Gravity: 300 px/s^2 */
        gibs[i].vy += TO_FP(300) / 60;
        if (gibs[i].vy > TO_FP(350)) gibs[i].vy = TO_FP(350);

        /* Ground friction */
        if (gibs[i].vy == 0 && gibs[i].vx != 0) {
            if (gibs[i].vx > 0) {
                gibs[i].vx -= TO_FP(120) / 60;
                if (gibs[i].vx < 0) gibs[i].vx = 0;
            } else {
                gibs[i].vx += TO_FP(120) / 60;
                if (gibs[i].vx > 0) gibs[i].vx = 0;
            }
        }

        int gw = 6, gh = 6;

        /* 1. X Movement & Wall Collision */
        int32_t next_x = gibs[i].x + gibs[i].vx / 60;
        int gx = FROM_FP(next_x);
        int gy = FROM_FP(gibs[i].y);

        /* Inset Y by 1 pixel so horizontal surfaces are not detected as vertical walls */
        int y_top = gy + 1;
        int y_bot = gy + gh - 2;
        int y_mid = gy + gh / 2;

        if (gibs[i].vx > 0) {
            /* Moving Right: check leading right edge at gx + gw - 1 */
            if (is_solid(gx + gw - 1, y_top) || is_solid(gx + gw - 1, y_bot) || is_solid(gx + gw - 1, y_mid)) {
                gx = ((gx + gw - 1) / 8) * 8 - gw;
                next_x = TO_FP(gx);
                gibs[i].vx = -(gibs[i].vx * 3) / 10;
                snd_bounce();
            }
        } else if (gibs[i].vx < 0) {
            /* Moving Left: check leading left edge at gx */
            if (is_solid(gx, y_top) || is_solid(gx, y_bot) || is_solid(gx, y_mid)) {
                gx = (gx / 8 + 1) * 8;
                next_x = TO_FP(gx);
                gibs[i].vx = -(gibs[i].vx * 3) / 10;
                snd_bounce();
            }
        }
        gibs[i].x = next_x;

        /* 2. Y Movement & Floor / Ceiling Collision */
        int32_t next_y = gibs[i].y + gibs[i].vy / 60;
        gx = FROM_FP(gibs[i].x);
        gy = FROM_FP(next_y);

        /* Inset X by 1 pixel so vertical walls are not detected as floor/ceiling */
        int x_left = gx + 1;
        int x_right = gx + gw - 2;
        int x_mid = gx + gw / 2;

        if (gibs[i].vy > 0) {
            /* Moving Down: check bottom edge at gy + gh - 1 */
            if (is_solid(x_left, gy + gh - 1) || is_solid(x_right, gy + gh - 1) || is_solid(x_mid, gy + gh - 1)) {
                gy = ((gy + gh - 1) / 8) * 8 - gh;
                next_y = TO_FP(gy);
                gibs[i].vy = -(gibs[i].vy * 3) / 10;
                if (gibs[i].vy > -TO_FP(25)) gibs[i].vy = 0;
                snd_bounce();
            }
        } else if (gibs[i].vy < 0) {
            /* Moving Up: check top edge at gy */
            if (is_solid(x_left, gy) || is_solid(x_right, gy) || is_solid(x_mid, gy)) {
                gy = (gy / 8 + 1) * 8;
                next_y = TO_FP(gy);
                gibs[i].vy = -(gibs[i].vy * 3) / 10;
                snd_bounce();
            }
        }
        gibs[i].y = next_y;

        /* 3. Gib vs Elevator Platform */
        gx = FROM_FP(gibs[i].x);
        gy = FROM_FP(gibs[i].y);
        if (gx + gw > ex && gx < ex + ew) {
            if (gibs[i].vy >= 0 && gy + gh >= ey - 2 && gy + gh <= ey + 8) {
                gy = ey - gh;
                gibs[i].y = TO_FP(gy);
                gibs[i].vy = -(gibs[i].vy * 3) / 10;
                if (gibs[i].vy > -TO_FP(25)) gibs[i].vy = 0;
                gibs[i].y += actual_edy;
                snd_bounce();
            }
        }

        /* 4. Gib vs Crates */
        gx = FROM_FP(gibs[i].x);
        gy = FROM_FP(gibs[i].y);
        for (int c = 0; c < 5; c++) {
            int cx = FROM_FP(crates[c].x);
            int cy = FROM_FP(crates[c].y);
            int cw = 10, ch = 10;
            if (gx + gw > cx && gx < cx + cw && gy + gh > cy && gy < cy + ch) {
                if (gibs[i].vy > 0 && gy + gh <= cy + 5) {
                    gy = cy - gh;
                    gibs[i].y = TO_FP(gy);
                    gibs[i].vy = -(gibs[i].vy * 3) / 10;
                    if (gibs[i].vy > -TO_FP(25)) gibs[i].vy = 0;
                } else if (gibs[i].vx > 0) {
                    gx = cx - gw;
                    gibs[i].x = TO_FP(gx);
                    gibs[i].vx = -(gibs[i].vx * 3) / 10;
                } else if (gibs[i].vx < 0) {
                    gx = cx + cw;
                    gibs[i].x = TO_FP(gx);
                    gibs[i].vx = -(gibs[i].vx * 3) / 10;
                }
                snd_bounce();
            }
        }
    }

    /* 8. Player Animation */
    if (!player.is_grounded) {
        if (player.vy < 0) player.anim_frame = 4; /* jump */
        else player.anim_frame = 1; /* flail */
    } else {
        if (player.vx != 0) {
            player.anim_timer++;
            if (player.anim_timer >= 6) {
                player.anim_timer = 0;
                static const uint8_t walk_frames[4] = {1, 2, 3, 0};
                player.anim_step = (player.anim_step + 1) % 4;
                player.anim_frame = walk_frames[player.anim_step];
            }
        } else {
            player.anim_frame = 0; /* idle */
            player.anim_step = 0;
        }
    }

    /* 9. Camera follow */
    cam_x = FROM_FP(player.x) - 120 + 4;
    cam_y = FROM_FP(player.y) - 80 + 5;
    if (cam_x < 0) cam_x = 0;
    if (cam_x > 80) cam_x = 80;
    if (cam_y < 0) cam_y = 0;
    if (cam_y > 80) cam_y = 80;

    REG_BG1HOFS = cam_x;
    REG_BG1VOFS = cam_y;
}

/* Update Demo 2: 300 Blocks Universe */
static void update_blocks(void) {
    int spd = TO_FP(200) / 60;
    if (key_curr & KEY_LEFT)  focus_x -= spd;
    if (key_curr & KEY_RIGHT) focus_x += spd;
    if (key_curr & KEY_UP)    focus_y -= spd;
    if (key_curr & KEY_DOWN)  focus_y += spd;

    if (focus_x < TO_FP(120)) focus_x = TO_FP(120);
    if (focus_x > TO_FP(200)) focus_x = TO_FP(200);
    if (focus_y < TO_FP(80))  focus_y = TO_FP(80);
    if (focus_y > TO_FP(160)) focus_y = TO_FP(160);

    /* Spew gibs from center */
    gib_spawn_timer++;
    if (gib_spawn_timer >= 8) {
        gib_spawn_timer = 0;
        spawn_gib(160, 120, -120, 120, -120, 120);
    }

    bool any_bounce = false;
    const int gw = 6, gh = 6;

    for (int i = 0; i < MAX_GIBS; i++) {
        if (!gibs[i].active) continue;

        gibs[i].life--;
        if (gibs[i].life == 0) {
            gibs[i].active = false;
            continue;
        }

        /* 1. X Movement & Solid Obstacle Collision */
        int32_t next_x = gibs[i].x + gibs[i].vx / 60;
        int gx = FROM_FP(next_x);
        int gy = FROM_FP(gibs[i].y);

        int y_top = gy + 1;
        int y_bot = gy + gh - 2;
        int y_mid = gy + gh / 2;

        if (gibs[i].vx > 0) {
            /* Moving Right: check leading right edge */
            if (is_solid(gx + gw - 1, y_top) || is_solid(gx + gw - 1, y_bot) || is_solid(gx + gw - 1, y_mid)) {
                gx = ((gx + gw - 1) / 8) * 8 - gw;
                next_x = TO_FP(gx);
                gibs[i].vx = -gibs[i].vx;
                any_bounce = true;
            }
        } else if (gibs[i].vx < 0) {
            /* Moving Left: check leading left edge */
            if (is_solid(gx, y_top) || is_solid(gx, y_bot) || is_solid(gx, y_mid)) {
                gx = (gx / 8 + 1) * 8;
                next_x = TO_FP(gx);
                gibs[i].vx = -gibs[i].vx;
                any_bounce = true;
            }
        }
        gibs[i].x = next_x;

        /* 2. Y Movement & Solid Obstacle Collision */
        int32_t next_y = gibs[i].y + gibs[i].vy / 60;
        gx = FROM_FP(gibs[i].x);
        gy = FROM_FP(next_y);

        int x_left = gx + 1;
        int x_right = gx + gw - 2;
        int x_mid = gx + gw / 2;

        if (gibs[i].vy > 0) {
            /* Moving Down: check bottom edge */
            if (is_solid(x_left, gy + gh - 1) || is_solid(x_right, gy + gh - 1) || is_solid(x_mid, gy + gh - 1)) {
                gy = ((gy + gh - 1) / 8) * 8 - gh;
                next_y = TO_FP(gy);
                gibs[i].vy = -gibs[i].vy;
                any_bounce = true;
            }
        } else if (gibs[i].vy < 0) {
            /* Moving Up: check top edge */
            if (is_solid(x_left, gy) || is_solid(x_right, gy) || is_solid(x_mid, gy)) {
                gy = (gy / 8 + 1) * 8;
                next_y = TO_FP(gy);
                gibs[i].vy = -gibs[i].vy;
                any_bounce = true;
            }
        }
        gibs[i].y = next_y;

        /* 3. Outer boundary clamp & bounce */
        gx = FROM_FP(gibs[i].x);
        gy = FROM_FP(gibs[i].y);
        if (gx < 8) {
            gibs[i].x = TO_FP(8);
            if (gibs[i].vx < 0) gibs[i].vx = -gibs[i].vx;
            any_bounce = true;
        } else if (gx > 306) {
            gibs[i].x = TO_FP(306);
            if (gibs[i].vx > 0) gibs[i].vx = -gibs[i].vx;
            any_bounce = true;
        }
        if (gy < 8) {
            gibs[i].y = TO_FP(8);
            if (gibs[i].vy < 0) gibs[i].vy = -gibs[i].vy;
            any_bounce = true;
        } else if (gy > 226) {
            gibs[i].y = TO_FP(226);
            if (gibs[i].vy > 0) gibs[i].vy = -gibs[i].vy;
            any_bounce = true;
        }

        /* 4. Safety unstick: if somehow trapped inside solid */
        if (is_solid(gx + 1, gy + 1) && is_solid(gx + gw - 2, gy + gh - 2)) {
            gibs[i].active = false;
        }
    }

    static uint8_t bounce_snd_cd = 0;
    if (bounce_snd_cd > 0) bounce_snd_cd--;
    if (any_bounce && bounce_snd_cd == 0) {
        snd_bounce();
        bounce_snd_cd = 4;
    }

    cam_x = FROM_FP(focus_x) - 120;
    cam_y = FROM_FP(focus_y) - 80;
    REG_BG1HOFS = cam_x;
    REG_BG1VOFS = cam_y;
}

/* Update Demo 3: Parts Rain */
static void update_parts_rain(void) {
    int spd = (key_curr & (KEY_A | KEY_B)) ? (TO_FP(150) / 60) : (TO_FP(50) / 60);
    int32_t prev_paddle_x = paddle_x;

    if (key_curr & KEY_LEFT)  paddle_x -= spd;
    if (key_curr & KEY_RIGHT) paddle_x += spd;

    if (paddle_x < TO_FP(0)) paddle_x = TO_FP(0);
    if (paddle_x > TO_FP(240 - 50)) paddle_x = TO_FP(240 - 50);

    int32_t paddle_dx = paddle_x - prev_paddle_x;

    /* Pour gibs from top center directly over starting paddle position */
    gib_spawn_timer++;
    if (gib_spawn_timer >= 6) {
        gib_spawn_timer = 0;
        spawn_gib(rnd_range(95, 145), -8, -10, 10, 40, 75);
    }

    int px = FROM_FP(paddle_x);
    int py = 136;
    int pw = 50;
    bool any_caught = false;

    for (int i = 0; i < MAX_GIBS; i++) {
        if (!gibs[i].active) continue;

        gibs[i].life--;
        if (gibs[i].life == 0) {
            gibs[i].active = false;
            continue;
        }

        int32_t prev_y = gibs[i].y;

        /* Gravity */
        gibs[i].vy += TO_FP(200) / 60;
        if (gibs[i].vy > TO_FP(250)) gibs[i].vy = TO_FP(250);

        /* Ground friction if resting */
        if (gibs[i].vy == 0 && gibs[i].vx != 0) {
            if (gibs[i].vx > 0) {
                gibs[i].vx -= TO_FP(150) / 60;
                if (gibs[i].vx < 0) gibs[i].vx = 0;
            } else {
                gibs[i].vx += TO_FP(150) / 60;
                if (gibs[i].vx > 0) gibs[i].vx = 0;
            }
        }

        gibs[i].x += gibs[i].vx / 60;
        gibs[i].y += gibs[i].vy / 60;

        int gx = FROM_FP(gibs[i].x);
        int gy = FROM_FP(gibs[i].y);
        int prev_gy = FROM_FP(prev_y);

        /* Check landing on paddle or on top of already resting parts */
        bool caught = false;
        if (gibs[i].vy >= 0) {
            /* 1. Direct landing on paddle top surface */
            if (gx + 6 > px && gx < px + pw) {
                if ((prev_gy + 6 <= py + 2 && gy + 6 >= py) || (gy + 6 >= py - 1 && gy + 6 <= py + 8)) {
                    gy = py - 6;
                    gibs[i].y = TO_FP(gy);
                    gibs[i].vy = -(gibs[i].vy * 2) / 10;
                    if (gibs[i].vy > -TO_FP(25)) gibs[i].vy = 0;
                    gibs[i].x += paddle_dx;
                    caught = true;
                    any_caught = true;
                }
            }

            /* 2. Stacking / piling on another resting part */
            if (!caught) {
                for (int j = 0; j < MAX_GIBS; j++) {
                    if (j == i || !gibs[j].active) continue;
                    int jx = FROM_FP(gibs[j].x);
                    int jy = FROM_FP(gibs[j].y);
                    if (gibs[j].vy == 0 && jy >= py - 30 && jy <= py - 6 && jx + 6 > px - 4 && jx < px + pw + 4) {
                        if (gx + 5 > jx && gx < jx + 5) {
                            if ((prev_gy + 6 <= jy + 2 && gy + 6 >= jy) || (gy + 6 >= jy - 1 && gy + 6 <= jy + 6)) {
                                gy = jy - 6;
                                gibs[i].y = TO_FP(gy);
                                gibs[i].vy = -(gibs[i].vy * 2) / 10;
                                if (gibs[i].vy > -TO_FP(25)) gibs[i].vy = 0;
                                gibs[i].x += paddle_dx;
                                caught = true;
                                any_caught = true;
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* Fall off screen */
        if (gy > 165) gibs[i].active = false;
    }

    static uint8_t snd_cd = 0;
    if (snd_cd > 0) snd_cd--;
    if (any_caught && snd_cd == 0) {
        snd_bounce();
        snd_cd = 5;
    }

    cam_x = 0;
    cam_y = 0;
    REG_BG1HOFS = 0;
    REG_BG1VOFS = 0;
}

/* OAM Updates */
static void update_sprites(void) {
    if (current_demo == DEMO_WAREHOUSE) {
        /* Sprite 0: Player (16x16, 4bpp, Bank 0, tile: anim_frame * 4) */
        int psx = FROM_FP(player.x) - cam_x;
        int psy = FROM_FP(player.y) - cam_y;
        if (psx >= -16 && psx < 240 && psy >= -16 && psy < 160) {
            shadow_oam[0].attr0 = (psy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
            shadow_oam[0].attr1 = (psx & ATTR1_X_MASK) | ATTR1_SIZE_16 | (player.facing_left ? ATTR1_HFLIP : 0);
            shadow_oam[0].attr2 = (player.anim_frame * 4) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
        } else {
            shadow_oam[0].attr0 = ATTR0_HIDE;
        }

        /* Sprite 1: Elevator (64x32, 4bpp, Bank 0, tile 20) */
        int esx = FROM_FP(elevator.x) - 7 - cam_x;
        int esy = FROM_FP(elevator.y) - 2 - cam_y;
        if (esx >= -64 && esx < 240 && esy >= -32 && esy < 160) {
            shadow_oam[1].attr0 = (esy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_WIDE;
            shadow_oam[1].attr1 = (esx & ATTR1_X_MASK) | ATTR1_SIZE_64;
            shadow_oam[1].attr2 = 20 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
        } else {
            shadow_oam[1].attr0 = ATTR0_HIDE;
        }

        /* Sprite 2: Pusher (16x16, 4bpp, Bank 0, tile 52) */
        int pusx = FROM_FP(pusher.x) - 4 - cam_x;
        int pusy = FROM_FP(pusher.y) - cam_y;
        if (pusx >= -16 && pusx < 240 && pusy >= -16 && pusy < 160) {
            shadow_oam[2].attr0 = (pusy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
            shadow_oam[2].attr1 = (pusx & ATTR1_X_MASK) | ATTR1_SIZE_16;
            shadow_oam[2].attr2 = 52 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
        } else {
            shadow_oam[2].attr0 = ATTR0_HIDE;
        }

        /* Sprites 3..7: Crates (16x16, 4bpp, Bank 0, tile 56) */
        for (int i = 0; i < 5; i++) {
            int csx = FROM_FP(crates[i].x) - 3 - cam_x;
            int csy = FROM_FP(crates[i].y) - 3 - cam_y;
            if (csx >= -16 && csx < 240 && csy >= -16 && csy < 160) {
                shadow_oam[3 + i].attr0 = (csy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
                shadow_oam[3 + i].attr1 = (csx & ATTR1_X_MASK) | ATTR1_SIZE_16;
                shadow_oam[3 + i].attr2 = 56 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
            } else {
                shadow_oam[3 + i].attr0 = ATTR0_HIDE;
            }
        }

        /* Sprites 8..31: Gibs (8x8, 4bpp, Bank 0, tile 60+type) */
        for (int i = 0; i < MAX_GIBS; i++) {
            int spr_idx = 8 + i;
            if (gibs[i].active) {
                int gsx = FROM_FP(gibs[i].x) - 1 - cam_x;
                int gsy = FROM_FP(gibs[i].y) - 1 - cam_y;
                if (gsx >= -8 && gsx < 240 && gsy >= -8 && gsy < 160) {
                    shadow_oam[spr_idx].attr0 = (gsy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
                    shadow_oam[spr_idx].attr1 = (gsx & ATTR1_X_MASK) | ATTR1_SIZE_8;
                    shadow_oam[spr_idx].attr2 = (60 + gibs[i].type) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
                } else {
                    shadow_oam[spr_idx].attr0 = ATTR0_HIDE;
                }
            } else {
                shadow_oam[spr_idx].attr0 = ATTR0_HIDE;
            }
        }

        /* Hide others */
        for (int i = 8 + MAX_GIBS; i < 128; i++) shadow_oam[i].attr0 = ATTR0_HIDE;

    } else if (current_demo == DEMO_BLOCKS) {
        shadow_oam[0].attr0 = ATTR0_HIDE;
        shadow_oam[1].attr0 = ATTR0_HIDE;
        shadow_oam[2].attr0 = ATTR0_HIDE;
        for (int i = 0; i < 5; i++) shadow_oam[3 + i].attr0 = ATTR0_HIDE;

        /* Sprite 8: Focus cursor (8x8) */
        int fx = FROM_FP(focus_x) - 4 - cam_x;
        int fy = FROM_FP(focus_y) - 4 - cam_y;
        shadow_oam[8].attr0 = (fy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
        shadow_oam[8].attr1 = (fx & ATTR1_X_MASK) | ATTR1_SIZE_8;
        shadow_oam[8].attr2 = 63 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);

        /* Gibs */
        for (int i = 0; i < MAX_GIBS; i++) {
            int spr_idx = 9 + i;
            if (gibs[i].active) {
                int gsx = FROM_FP(gibs[i].x) - 1 - cam_x;
                int gsy = FROM_FP(gibs[i].y) - 1 - cam_y;
                if (gsx >= -8 && gsx < 240 && gsy >= -8 && gsy < 160) {
                    shadow_oam[spr_idx].attr0 = (gsy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
                    shadow_oam[spr_idx].attr1 = (gsx & ATTR1_X_MASK) | ATTR1_SIZE_8;
                    shadow_oam[spr_idx].attr2 = (60 + gibs[i].type) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
                } else {
                    shadow_oam[spr_idx].attr0 = ATTR0_HIDE;
                }
            } else {
                shadow_oam[spr_idx].attr0 = ATTR0_HIDE;
            }
        }
        for (int i = 9 + MAX_GIBS; i < 128; i++) shadow_oam[i].attr0 = ATTR0_HIDE;

    } else if (current_demo == DEMO_PARTS_RAIN) {
        /* Sprite 0: Paddle (50x12 visible inside 64x32 sprite, offset -7, -2) */
        int px = FROM_FP(paddle_x);
        int py = 136;
        shadow_oam[0].attr0 = ((py - 2) & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_WIDE;
        shadow_oam[0].attr1 = ((px - 7) & ATTR1_X_MASK) | ATTR1_SIZE_64;
        shadow_oam[0].attr2 = 20 | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);

        /* Gibs */
        for (int i = 0; i < MAX_GIBS; i++) {
            int spr_idx = 1 + i;
            if (gibs[i].active) {
                int gsx = FROM_FP(gibs[i].x) - 1;
                int gsy = FROM_FP(gibs[i].y) - 1;
                if (gsx >= -8 && gsx < 240 && gsy >= -8 && gsy < 160) {
                    shadow_oam[spr_idx].attr0 = (gsy & ATTR0_Y_MASK) | ATTR0_REGULAR | ATTR0_4BPP | ATTR0_SQUARE;
                    shadow_oam[spr_idx].attr1 = (gsx & ATTR1_X_MASK) | ATTR1_SIZE_8;
                    shadow_oam[spr_idx].attr2 = (60 + gibs[i].type) | ATTR2_PRIORITY(1) | ATTR2_PALETTE(0);
                } else {
                    shadow_oam[spr_idx].attr0 = ATTR0_HIDE;
                }
            } else {
                shadow_oam[spr_idx].attr0 = ATTR0_HIDE;
            }
        }
        for (int i = 1 + MAX_GIBS; i < 128; i++) shadow_oam[i].attr0 = ATTR0_HIDE;
    }
}

int main(void) {
    /* 1. Load Palettes */
    for (int i = 0; i < 64; i++) MEM_PAL_BG[i] = bg_palette[i];
    for (int i = 0; i < 16; i++) MEM_PAL_OBJ[i] = obj_palette[i];

    /* 2. Load Tiles into VRAM */
    volatile uint16_t* cb0 = CHARBLOCK(0);
    const uint16_t* f_src = (const uint16_t*)font_tiles;
    for (int i = 0; i < (int)(sizeof(font_tiles) / 2); i++) cb0[i] = f_src[i];

    volatile uint16_t* cb1 = CHARBLOCK(1);
    const uint16_t* bg_src = (const uint16_t*)bg_tiles;
    for (int i = 0; i < (int)(sizeof(bg_tiles) / 2); i++) cb1[i] = bg_src[i];

    volatile uint16_t* cb4 = (volatile uint16_t*)0x06010000;
    const uint16_t* obj_src = (const uint16_t*)obj_tiles;
    for (int i = 0; i < (int)(sizeof(obj_tiles) / 2); i++) cb4[i] = obj_src[i];

    /* 3. Configure Backgrounds */
    REG_BG0CNT = 0x0000 | (31 << 8) | (0 << 2) | 0;
    REG_BG1CNT = 0x4000 | (28 << 8) | (1 << 2) | 2;

    /* 4. Enable Display */
    REG_DISPCNT = 0x1000 | 0x0200 | 0x0100 | 0x0040 | 0x0000;

    /* 5. Sound & Demos */
    sound_init();
    init_demo(DEMO_WAREHOUSE);

    while (1) {
        key_prev = key_curr;
        key_curr = ~REG_KEYINPUT & 0x03FF;
        key_pressed = key_curr & ~key_prev;

        if (key_pressed & KEY_START) {
            DemoMode next = (current_demo + 1) % DEMO_COUNT;
            init_demo(next);
        }

        if (key_pressed & KEY_SELECT) {
            init_demo(current_demo);
        }

        if (current_demo == DEMO_WAREHOUSE) {
            update_warehouse();
        } else if (current_demo == DEMO_BLOCKS) {
            update_blocks();
        } else if (current_demo == DEMO_PARTS_RAIN) {
            update_parts_rain();
        }

        update_sprites();

        vsync();
        flush_oam();
    }

    return 0;
}

