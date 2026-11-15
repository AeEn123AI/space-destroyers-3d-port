/* Space Destroyers 3D - shared game logic (port of the Scratch project). */
#ifndef GAME_H
#define GAME_H

#include <stdint.h>
#include "costumes_gen.h"

#define STAGE_W 480
#define STAGE_H 360

/* ---- input (filled by the platform each frame) ---- */
typedef struct {
    /* held keys, Scratch semantics */
    int left, right, up, down;      /* WASD / arrows / D-pad (digital) */
    float ax, ay;                   /* analog stick, -1..1 (adds to digital, clamped) */
    int space;                      /* shoot / start */
    int speed_up, speed_down;       /* E / Q */
    int any_key;                    /* any key or button held (changelog close) */
    /* easter egg keys held: arrows (or D-pad) then B, A */
    int egg_up, egg_down, egg_left, egg_right, egg_b, egg_a;
    int egg_up_pressed;
    int cheat_q_pressed, cheat_w, cheat_e, cheat_r, cheat_t, cheat_y;
    /* edge-triggered actions */
    int pause_pressed;              /* Enter / P / START */
    int music_key_pressed;          /* key 1 */
    int back_pressed;               /* B / Esc: close changelog, leave mode select */
    int confirm_pressed;            /* A / Enter on menus: activate highlighted button */
    int quit_pressed;               /* SELECT / Esc in game: back to title */
    int nav_left, nav_right, nav_up, nav_down; /* menu navigation (edges) */
    /* pointer (mouse on PC, bottom-screen touch on 3DS for steering), stage coords */
    int mouse_down;
    float mouse_x, mouse_y;
    int mouse_moved;
    int mouse_hover_ok;             /* 0 on 3DS: pointer is not on the top screen, no hover */
} GameInput;

/* ---- draw list (back to front) ---- */
enum { DI_COSTUME, DI_FILL };
enum { DIF_PC_ONLY = 1 };
typedef struct {
    uint8_t kind, flags;
    int16_t costume;
    float x, y;        /* stage coords (centre of rotation) */
    float size;        /* 1.0 = 100% */
    float alpha;       /* 1 - ghost */
    float hue;         /* Scratch colour effect value (0..200 = one turn) */
    float depth;       /* stereo: -1 (pops out) .. 0 (screen) .. 1 (far) */
    uint32_t rgba;     /* DI_FILL colour */
} DrawItem;

#define MAX_DRAW 4096

/* ---- monitors / HUD ---- */
enum { MON_SPEED, MON_WR_TIMER, MON_TIMER, MON_SCORE, MON_WR_SCORE, MON_AMMO, MON_FPS, MON_STARS, MON_COUNT };
typedef struct {
    const char *label;
    float x, y;        /* Scratch stage position of the top-left corner */
    int visible;
    char value[32];
} Monitor;

/* title buttons (Title screen clones) */
enum { BTN_WHATSNEW, BTN_PLAY, BTN_SELECTMODE, BTN_TOGGLEMUSIC, BTN_COUNT };

typedef struct {
    int costume;       /* current costume (depends on mode selection) */
    float x, y, size, alpha;
    int hovered, visible;
} ButtonView;

enum { GS_TITLE, GS_STARTING, GS_PLAYING, GS_LOSE, GS_EASTER };

typedef struct {
    int state;
    int paused;
    int mode_selection, changelog_open, hard_mode;
    int player_music;
    int control_mouse;          /* mobile/mouse control mode in game */
    float score, ammo, speed;
    double timer;
    double wr_timer, wr_score;
    int stars_slider;           /* "Number of stars" 0..300 */
    float bg_r, bg_g, bg_b;     /* stage background colour */
    float fps;
    int nav_index;              /* highlighted title button (-1 = none) */
    int records_dirty;          /* set when records changed (platform saves) */
} GameView;

void game_init(uint32_t seed, double wr_timer, double wr_score, int player_music);
void game_update(double dt, const GameInput *in);
int  game_draw_list(DrawItem *out, int max);       /* returns count */
const GameView *game_view(void);
Monitor *game_monitors(void);
void game_buttons(ButtonView out[BTN_COUNT]);
/* actions for touch / direct UI (3DS bottom screen) */
void game_press_button(int btn);
void game_toggle_pause(void);
void game_set_stars_slider(int v);
int  game_pause_button_visible(void);
void game_quit_to_title(void);
void fmt_number(double v, char *out, int n);

#endif
