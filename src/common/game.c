/* Space Destroyers 3D - shared game logic.
 *
 * Port of the Scratch project. Continuous motion uses f = dt*30 (the project's "Game speed"
 * normalisation); script timing (waits, spawners, thread order) is emulated on a 30 Hz tick, which
 * is how the original behaves on scratch.mit.edu. See NOTES.md for the distilled behaviour spec.
 */
#include "game.h"
#include "audio.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TICK (1.0 / 30.0)
#define DIST 300.0f
/* no clone limit: the object list grows as needed (Scratch's 300 cap caused stars to stop spawning in hard mode) */
#define UI_RESERVE 64         /* draw-list slots kept free for menus/HUD after world objects */
#define DEG (3.14159265358979 / 180.0)

/* Stars sprite costume numbers (Scratch order) */
enum { SC_STAR01 = 1, SC_STAR1, SC_STAR001, SC_EARTH, SC_PLANET2, SC_POWERUP, SC_BULLET };
static const int sc_to_co[8] = { 0, CO_STAR01, CO_STAR1, CO_STAR001, CO_EARTH, CO_PLANET2, CO_POWERUP, CO_BULLET };

typedef struct { float x, y, z, hue, age; int c; } Obj;

static struct {
    uint32_t rng;
    int state, first_title;
    double t;                 /* sensing timer */
    double tick_acc;
    /* camera / ship */
    float X, Y;
    double speed;
    int stars_n;
    /* objects */
    Obj *obj;                 /* points at g_objs (kept outside G so memset in game_init does not leak it) */
    int nobj;
    int shared_costume;       /* the Stars sprite's own costume, shared by all spawner threads */
    float bx, by, bz;         /* the one global Bullet X/Y/Z */
    /* threads (tick counters) */
    int star_wait, star_phase;
    int pu_wait;
    int pl_wait, pl_phase, pl_left;
    int shoot_wait;
    /* game vars */
    double score, ammo;
    double timer;
    int paused, hard, player_music, music_flag;
    int shoot_flag, control_mouse, dots_shown, pause_btn_shown;
    float target_x, target_y, old_mx, old_my, old_tx, old_ty;
    int was_down, shoot_key, hard_ticks, first_title_music;
    double wr_timer, wr_score;
    int wr_t_beaten, wr_s_beaten;
    double wr_t_next, wr_s_next;
    double popup_t; int popup_active; /* WR popup */
    double game_time;          /* time since Start game */
    /* title */
    int mode_sel, desc_hard, changelog, changelog_phase;
    float title_ghost, overlay_ghost;
    double title_fade_t;       /* fade-in progress (s) */
    double start_t;            /* Play game fade-out progress */
    float btn_y[BTN_COUNT], btn_size[BTN_COUNT], btn_ghost[BTN_COUNT];
    float desc_y, sel_x, sel_size, sel_ghost, chg_ghost;
    int sel_alive;
    int hovered;               /* button index hovered, -1 */
    int nav_index, nav_active;
    int space_released;
    int mouse_was_down_title;
    float bg_effect; int bg_scripts;
    /* stop/lose/easter */
    double seq_t; int seq_phase;
    /* misc scripts */
    int egg_phase, cheat_phase, cheat_on; int cheat_ticks;
    int key1_phase, key1_was;
    double key1_t;
    float vol_stage, vol_thumb;
    double fps_acc; int fps_frames; float fps;
    GameView view;
    Monitor mon[MON_COUNT];
} G;

static const float btn_x[BTN_COUNT] = { -135, 0, 135, 180 };
static const float btn_y0[BTN_COUNT] = { -15, -15, -15, -120 };

/* ---------------------------------------------------------------- helpers */
static uint32_t rnd_u32(void) {
    uint32_t x = G.rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; G.rng = x; return x;
}
static double rnd01(void) { return (rnd_u32() >> 8) * (1.0 / 16777216.0); }
static int rnd_int(int a, int b) { return a + (int)floor(rnd01() * (b - a + 1)); }
static float ease(float cur, float target, float div, float f) {
    /* Scratch: v += ((target - v) / div) / gamespeed per frame -> exponential approach */
    return target + (cur - target) * powf(1.0f - 1.0f / div, f);
}
static int wait_ticks(double d) { int n = (int)ceil(d * 30.0 - 1e-6); return n < 1 ? 1 : n; }

void fmt_number(double v, char *out, int n) {
    if (fabs(v - floor(v + 0.5)) < 1e-9 && fabs(v) < 1e15) { snprintf(out, n, "%.0f", v); return; }
    snprintf(out, n, "%.6f", v);
    char *p = out + strlen(out) - 1;
    while (p > out && *p == '0') *p-- = 0;
    if (p > out && *p == '.') *p = 0;
}

static void costume_size(int co, float *w, float *h) { *w = costume_info[co].w; *h = costume_info[co].h; }
/* world objects: only Scratch's minimum size; its max-size limit (sprite <= 1.5x stage) is
   deliberately not replicated so near planets keep growing as they leave the screen */
static float min_size(int co, float size) {
    float w, h; costume_size(co, &w, &h);
    float mn = fminf(1.0f, fmaxf(5.0f / w, 5.0f / h));
    return size < mn ? mn : size;
}

/* ---------------------------------------------------------------- objects */
static Obj *g_objs; static int g_obj_cap;
static void add_object(float x, float y, float z) {
    if (G.nobj >= g_obj_cap) {
        int cap = g_obj_cap ? g_obj_cap * 2 : 512;
        Obj *p = realloc(g_objs, (size_t)cap * sizeof *p);
        if (!p) return;                     /* out of memory: skip the spawn rather than crash */
        g_objs = p; g_obj_cap = cap;
    }
    G.obj = g_objs;
    Obj *o = &G.obj[G.nobj++];
    o->x = x; o->y = y; o->z = z; o->hue = 0; o->age = 0; o->c = G.shared_costume;
}
static void del_object(int i) { G.obj[i] = G.obj[--G.nobj]; }
static void clear_objects(void) { G.nobj = 0; }

static void broadcast_lose(void);

static void update_objects(float f, float dt) {
    for (int i = 0; i < G.nobj; i++) {
        Obj *o = &G.obj[i];
        o->age += dt;
        if (o->z < 100) { del_object(i--); continue; }
        if (o->c == SC_POWERUP) {
            o->hue += f;
            o->z -= G.speed * f;
            if (G.X + 250 > o->x && G.X - 250 < o->x && G.Y + 250 > o->y && G.Y - 250 < o->y && o->z < 250) {
                if (G.state == GS_PLAYING) { G.ammo += 1; G.score += 2; audio_play(SND_POP, GRP_SPRITE); }
                del_object(i--); continue;
            }
        } else if (o->c == SC_BULLET) {
            o->hue += 10 * f;
            if (o->z > 4000) { G.bx = 0; G.by = 0; G.bz = -250; del_object(i--); continue; }
            double t = G.t;
            o->x = G.X + (float)(sin(t * 6000 * DEG) * sin(t * 30 * DEG) * 20);
            o->y = G.Y + (float)(cos(t * 6000 * DEG) * cos(t * 30 * DEG) * 20);
            o->z += 60 * f;
            G.bx = o->x; G.by = o->y; G.bz = o->z;
        } else {
            o->z -= G.speed * f;
            if (o->c > 3) {
                if (G.X + 250 > o->x && G.X - 250 < o->x && G.Y + 250 > o->y && G.Y - 250 < o->y && o->z < 250) {
                    broadcast_lose();
                    return;
                }
                /* original bug kept: the second Z test compares Bullet Z with itself (always true) */
                if (G.bx + 275 > o->x && G.bx - 275 < o->x && G.by + 275 > o->y && G.by - 275 < o->y && G.bz + 275 > o->z) {
                    G.score += 1;
                    audio_play(SND_HIT, GRP_SPRITE);
                    del_object(i--); continue;
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- monitors */
static void mon_set(int m, double v) { fmt_number(v, G.mon[m].value, sizeof G.mon[m].value); }
/* single-precision values: round to 7 significant digits first so 60.2f shows as "60.2" like Scratch */
static void mon_setf(int m, float v) { char b[32]; snprintf(b, sizeof b, "%.7g", v); mon_set(m, atof(b)); }
static void show_hud(int on) {
    G.mon[MON_SPEED].visible = G.mon[MON_AMMO].visible = G.mon[MON_WR_SCORE].visible = G.mon[MON_SCORE].visible = on;
    G.mon[MON_TIMER].visible = G.mon[MON_WR_TIMER].visible = on;
}

/* ---------------------------------------------------------------- flow */
static void title_broadcast(void) {
    G.state = GS_TITLE;
    G.music_flag = 1;
    G.speed = 60;
    G.paused = 0;
    G.mode_sel = 0; G.changelog = 0; G.sel_alive = 0;
    G.score = 0; G.timer = 0;
    show_hud(0);
    G.mon[MON_STARS].visible = 1;
    G.title_ghost = 100; G.overlay_ghost = 50; G.title_fade_t = 0;
    for (int i = 0; i < BTN_COUNT; i++) { G.btn_y[i] = btn_y0[i]; G.btn_size[i] = 1; G.btn_ghost[i] = 100; }
    G.desc_y = 0;
    G.space_released = 0;
    G.mouse_was_down_title = 1;
    G.hovered = -1;
    G.bg_effect = 0; G.bg_scripts = G.first_title ? 2 : 1; G.first_title = 0;
    /* Stars threads restart in script order; shared costume reset to a star (see NOTES.md) */
    if (G.shared_costume > SC_STAR001) G.shared_costume = SC_STAR1;
    G.star_wait = 0; G.star_phase = 0; G.pu_wait = 0;
    G.popup_active = 0;
    G.view.records_dirty = 1;
}

static void stop_music_broadcast(void) {
    G.music_flag = 0;
    audio_stop_all();
    clear_objects();
    /* "You beat world record" sprite: update records */
    if (G.timer > G.wr_timer) { G.wr_timer = G.timer; }
    if (G.score > G.wr_score) { G.wr_score = G.score; }
    G.view.records_dirty = 1;
    G.popup_active = 0;
}

static void broadcast_lose(void) {
    audio_play(SND_POP_STAGE, GRP_STAGE);
    stop_music_broadcast();
    /* "Pop" was stopped by stop-all in Scratch too, but it is started first and Scratch's stop
       happens in the next block; replay so the crash is audible like in the original */
    audio_play(SND_POP_STAGE, GRP_STAGE);
    G.state = GS_LOSE; G.seq_t = 0; G.seq_phase = 0;
}

static void start_game(void) {
    G.state = GS_PLAYING;
    G.ammo = 5;
    show_hud(1);
    G.mon[MON_STARS].visible = 0;
    G.shoot_flag = 0;
    G.timer = 0;
    G.game_time = 0;
    G.bx = 0; G.by = 0; G.bz = -250;
    G.target_x = 0; G.target_y = 0;
    G.pl_wait = 0; G.pl_phase = 0; G.pl_left = 0;
    G.shoot_wait = 0; G.hard_ticks = 0;
    G.wr_t_beaten = G.wr_s_beaten = 0;
    G.mon[MON_WR_TIMER].visible = G.mon[MON_TIMER].visible = 1;
}

void game_quit_to_title(void) {
    if (G.state == GS_PLAYING || G.state == GS_LOSE || G.state == GS_EASTER) {
        stop_music_broadcast();
        title_broadcast();
    }
}

void game_toggle_pause(void) {
    if (G.state != GS_PLAYING) return;
    G.paused = !G.paused;
    if (G.paused) { show_hud(0); G.mon[MON_STARS].visible = 1; }
    else { show_hud(1); G.mon[MON_STARS].visible = 0; G.pause_btn_shown = 0; }
    audio_play(SND_POP, GRP_SPRITE);
}

int game_pause_button_visible(void) { return G.state == GS_PLAYING && G.pause_btn_shown && !G.paused; }
void game_set_stars_slider(int v) { G.stars_n = v < 0 ? 0 : (v > 300 ? 300 : v); }

/* ---------------------------------------------------------------- title buttons */
static int btn_costume(int b) {
    switch (b) {
    case BTN_WHATSNEW: return G.mode_sel ? CO_NORMALMODE : CO_WHATSNEW;
    case BTN_PLAY: return G.mode_sel ? CO_BACK : CO_PLAY;
    case BTN_SELECTMODE: return G.mode_sel ? CO_HARDMODE : CO_SELECTMODE;
    default: return CO_TOGGLEMUSIC;
    }
}
static int btn_hit(int b, float mx, float my) {
    int co = btn_costume(b);
    const CostumeInfo *ci = &costume_info[co];
    float s = G.btn_size[b];
    float x0 = btn_x[b] - ci->rcx * s, x1 = x0 + ci->w * s;
    float y1 = G.btn_y[b] + ci->rcy * s, y0 = y1 - ci->h * s;
    return mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
}

void game_press_button(int b) {
    if (G.state != GS_TITLE || G.changelog) return;
    int co = btn_costume(b);
    audio_play(SND_POP, GRP_SPRITE);
    if (co == CO_WHATSNEW) { G.changelog = 1; G.changelog_phase = 0; G.chg_ghost = 100; G.mon[MON_STARS].visible = 0; }
    else if (co == CO_PLAY) { G.state = GS_STARTING; G.start_t = 0; G.mon[MON_STARS].visible = 0; }
    else if (co == CO_SELECTMODE) {
        G.mode_sel = 1; G.mon[MON_STARS].visible = 0;
        G.sel_alive = 1; G.sel_x = 0; G.sel_size = G.btn_size[BTN_SELECTMODE]; G.sel_ghost = 100;
    }
    else if (co == CO_NORMALMODE) G.hard = 0;
    else if (co == CO_HARDMODE) G.hard = 1;
    else if (co == CO_BACK) { G.mode_sel = 0; G.sel_alive = 0; G.mon[MON_STARS].visible = 1; }
    else if (co == CO_TOGGLEMUSIC) {
        if (G.player_music) { G.player_music = 0; audio_stop_all(); }
        else G.player_music = 1;
    }
}

static void nav_move(const GameInput *in) {
    int moved = in->nav_left || in->nav_right || in->nav_up || in->nav_down;
    if (!moved) return;
    if (!G.nav_active) { G.nav_active = 1; G.nav_index = BTN_PLAY; return; }
    int i = G.nav_index;
    if (in->nav_left) i = (i == BTN_TOGGLEMUSIC) ? BTN_SELECTMODE : (i > 0 ? i - 1 : 0);
    if (in->nav_right) i = (i == BTN_TOGGLEMUSIC) ? BTN_TOGGLEMUSIC : (i < BTN_SELECTMODE ? i + 1 : BTN_TOGGLEMUSIC);
    if (in->nav_down) i = BTN_TOGGLEMUSIC;
    if (in->nav_up && i == BTN_TOGGLEMUSIC) i = BTN_SELECTMODE;
    G.nav_index = i;
}

static void update_title(double dt, float f, const GameInput *in) {
    /* fades (Title sprite + clones: ghost -2 per tick for 50 ticks) */
    G.title_fade_t += dt;
    float fade = 100.0f - (float)(G.title_fade_t * 30.0);  /* -2 ghost per (wait .01 + loop) = 2 ticks */
    if (fade < 0) fade = 0;
    if (G.state == GS_TITLE) {
        G.title_ghost = fade;
        for (int i = 0; i < BTN_COUNT; i++) G.btn_ghost[i] = fade;
    }
    if (G.sel_alive && G.sel_ghost > 0) G.sel_ghost = fmaxf(0, G.sel_ghost - 30.0f * (float)dt);
    if (G.changelog && G.chg_ghost > 0) G.chg_ghost = fmaxf(0, G.chg_ghost - 30.0f * (float)dt);

    /* pointer / nav hover */
    float cx = 0, cy = 0;
    int interactive = (G.state == GS_TITLE) && !G.changelog;
    if (in->mouse_moved && in->mouse_hover_ok) G.nav_active = 0;
    if (G.state == GS_TITLE) nav_move(in);
    G.hovered = -1;
    if (G.nav_active) {
        G.hovered = G.nav_index;
        cx = btn_x[G.nav_index]; cy = G.btn_y[G.nav_index];
    } else if (in->mouse_hover_ok) {
        cx = in->mouse_x; cy = in->mouse_y;
        for (int b = 0; b < BTN_COUNT; b++) if (btn_hit(b, cx, cy)) G.hovered = b;
    }
    for (int b = 0; b < BTN_COUNT; b++) {
        float target = (b == G.hovered && G.state == GS_TITLE) ? 1.2f : 1.0f;
        G.btn_size[b] = ease(G.btn_size[b], target, 5, f);
        if (b != BTN_TOGGLEMUSIC) G.btn_y[b] = ease(G.btn_y[b], G.mode_sel ? 45.0f : -15.0f, 3, f);
    }
    if (G.hovered >= 0 && G.state == GS_TITLE) {
        int co = btn_costume(G.hovered);
        if (co == CO_NORMALMODE) G.desc_hard = 0;
        if (co == CO_HARDMODE) G.desc_hard = 1;
    }
    G.desc_y = ease(G.desc_y, G.mode_sel ? -40.0f : 0.0f, 3, f);
    if (G.sel_alive) G.sel_x = ease(G.sel_x, G.hard ? 135.0f : -135.0f, 3, f);

    if (G.state == GS_TITLE) {
        /* camera follows the cursor */
        G.X = ease(G.X, cx, 10, f);
        G.Y = ease(G.Y, cy, 10, f);
        /* clicks */
        if (interactive && in->mouse_hover_ok && in->mouse_down && !G.mouse_was_down_title && G.hovered >= 0 && !G.nav_active)
            game_press_button(G.hovered);
        else if (interactive && in->confirm_pressed && G.nav_active)
            game_press_button(G.nav_index);
        else if (interactive && in->back_pressed && G.mode_sel)
            game_press_button(BTN_PLAY);   /* Back */
        G.mouse_was_down_title = in->mouse_down;
        /* changelog: wait release, press, release */
        if (G.changelog) {
            int anyk = in->any_key || in->mouse_down;
            if (G.changelog_phase == 0 && !anyk) G.changelog_phase = 1;
            else if (G.changelog_phase == 1 && anyk) G.changelog_phase = 2;
            else if (G.changelog_phase == 2 && !anyk) {
                G.changelog = 0; G.dots_shown = 1;
                if (!G.mode_sel) G.mon[MON_STARS].visible = 1;
            }
        }
        /* space starts the game (after it was released once) */
        if (!in->space) G.space_released = 1;
        if (G.space_released && in->space && !G.changelog) { G.state = GS_STARTING; G.start_t = 0; G.mon[MON_STARS].visible = 0; }
        G.mon[MON_STARS].visible = !G.mode_sel && !G.changelog;
    } else { /* GS_STARTING: everything fades out over 1 s while the camera recentres */
        G.start_t += dt;
        float add = (float)(dt * 100.0);
        G.title_ghost += add; G.overlay_ghost += add;
        for (int i = 0; i < BTN_COUNT; i++) G.btn_ghost[i] += add;
        G.sel_ghost += add; G.chg_ghost += add;
        G.X = ease(G.X, 0, 10, f);
        G.Y = ease(G.Y, 0, 10, f);
        if (G.start_t >= 1.0) { G.changelog = 0; G.sel_alive = 0; G.mode_sel = 0; start_game(); }
    }
}

/* ---------------------------------------------------------------- ticks (Scratch threads) */
static void tick_threads(void) {
    int spawn = (G.state == GS_TITLE || G.state == GS_STARTING || G.state == GS_PLAYING) && !G.paused;
    if (spawn) {
        /* 1. star spawner */
        if (G.star_wait > 0) {
            if (--G.star_wait == 0) { if (rnd_int(1, 10) == 10) G.shared_costume = SC_STAR001; G.star_phase = 1; }
        } else if (G.star_phase == 1 || G.star_phase == 0) {
            add_object(rnd_int(-1000, 1000) + G.X, rnd_int(-1000, 1000) + G.Y, 8000);
            add_object(rnd_int(-1000, 1000) + G.X, rnd_int(-1000, 1000) + G.Y, 8000);
            G.shared_costume = rnd_int(1, 2);
            double w = 0.25 - ((G.stars_n * (G.speed / 60.0)) / 150.0) * 0.125;
            G.star_wait = wait_ticks(w);
        }
        /* 3. power ups */
        if (G.pu_wait > 0) G.pu_wait--;
        else {
            G.shared_costume = SC_POWERUP;
            add_object(rnd_int(-1000, 1000) + G.X, rnd_int(-1000, 1000) + G.Y, 8000);
            G.pu_wait = wait_ticks(1.0);
        }
    }
    if (G.state == GS_PLAYING && !G.paused) {
        /* 4. planets */
        if (!G.hard) {
            if (G.pl_phase == 0) { G.pl_wait = wait_ticks(rnd_int(0, 1)); G.pl_phase = 1; }
            else if (G.pl_phase == 1) {
                if (--G.pl_wait == 0) { G.pl_left = rnd_int(1, 5); G.pl_phase = 2; }
            }
            if (G.pl_phase == 2) {
                if (G.pl_left > 0) {
                    G.shared_costume = rnd_int(4, 5);
                    add_object(rnd_int(-1000, 1000) + G.X, rnd_int(-1000, 1000) + G.Y, G.speed * 100);
                    G.pl_left--;
                } else G.pl_phase = 0;           /* repeat exits + forever yields this tick */
            }
        } else {
            if (G.hard_ticks++ > 0) {
                G.shared_costume = rnd_int(4, 5);
                add_object(rnd_int(-1000, 1000) + G.X, rnd_int(-1000, 1000) + G.Y, G.speed * 100);
                G.speed = 80;
                G.stars_n = 0;
            }
        }
        /* 5. shooting */
        if (G.shoot_wait > 0) G.shoot_wait--;
        else if ((G.shoot_key || G.shoot_flag) && G.ammo > 0) {
            G.shoot_flag = 0;
            G.shared_costume = SC_BULLET;
            audio_play(SND_CRUNCH, GRP_SPRITE);
            G.ammo -= 1;
            add_object(G.X, G.Y, 100);
            G.shoot_wait = wait_ticks(1.0);
        }
        /* cheat: Original timer -= 1 every second */
        if (G.cheat_on && ++G.cheat_ticks >= 31) { G.cheat_ticks = 0; G.timer += 1; }
    }
}

/* ---------------------------------------------------------------- misc scripts */
static void misc_scripts(double dt, const GameInput *in) {
    /* key 1 toggles music (Stage script: first press always turns it off) */
    if (G.key1_phase == 1) { G.key1_t -= dt; if (G.key1_t <= 0) { audio_stop_all(); G.key1_phase = 2; } }
    if (in->music_key_pressed) {
        if (G.key1_phase == 0) { G.player_music = 0; G.key1_phase = 1; G.key1_t = 0.05; }
        else if (G.key1_phase == 2) { G.player_music = 1; G.key1_phase = 0; }
    }
    /* easter egg: Up, then Down, Left, Right, B, A held in turn */
    switch (G.egg_phase) {
    case 0: if (in->egg_up_pressed && G.state != GS_EASTER) G.egg_phase = 1; break;
    case 1: if (in->egg_down) G.egg_phase = 2; break;
    case 2: if (in->egg_left) G.egg_phase = 3; break;
    case 3: if (in->egg_right) G.egg_phase = 4; break;
    case 4: if (in->egg_b) G.egg_phase = 5; break;
    case 5:
        if (in->egg_a) {
            G.egg_phase = 0;
            stop_music_broadcast();
            G.paused = 0;
            G.state = GS_EASTER; G.seq_t = 0; G.seq_phase = 0;
        }
        break;
    }
    /* q w e r t y: timer cheat */
    switch (G.cheat_phase) {
    case 0: if (in->cheat_q_pressed) G.cheat_phase = 1; break;
    case 1: if (in->cheat_w) G.cheat_phase = 2; break;
    case 2: if (in->cheat_e) G.cheat_phase = 3; break;
    case 3: if (in->cheat_r) G.cheat_phase = 4; break;
    case 4: if (in->cheat_t) G.cheat_phase = 5; break;
    case 5: if (in->cheat_y) { G.cheat_phase = 6; G.cheat_on = 1; } break;
    }
}

static void update_playing(double dt, float f, const GameInput *in) {
    G.game_time += dt;
    if (in->pause_pressed) game_toggle_pause();
    if (in->quit_pressed) { game_quit_to_title(); return; }

    /* pointer: pause button / pause panel clicks, else the Cursor sprite's touch steering */
    int press = in->mouse_down && !G.was_down;
    int eaten = 0;
    if (press && in->mouse_hover_ok) {
        if (G.paused) { game_toggle_pause(); eaten = 1; }
        else if (game_pause_button_visible() && in->mouse_x >= -240.5f && in->mouse_x <= -133.5f &&
                 in->mouse_y >= -152.5f && in->mouse_y <= -95.5f) { game_toggle_pause(); eaten = 1; }
    }
    if (press && !eaten && !G.paused) {
        G.control_mouse = 1; G.dots_shown = 1; G.pause_btn_shown = 1;
        G.old_mx = in->mouse_x; G.old_my = in->mouse_y;
        G.old_tx = G.target_x; G.old_ty = G.target_y;
        G.was_down = 2;               /* dragging */
    }
    if (G.was_down == 2) {
        if (in->mouse_down) {
            G.target_x = G.old_tx + (in->mouse_x - G.old_mx) * 3;
            G.target_y = G.old_ty + (in->mouse_y - G.old_my) * 3;
        } else {
            float dx = in->mouse_x - G.old_mx, dy = in->mouse_y - G.old_my;
            if (dx < 10 && dx > -10 && dy < 10 && dy > -10) G.shoot_flag = 1;
        }
    }
    if (!(G.was_down == 2 && in->mouse_down)) G.was_down = in->mouse_down ? 1 : 0;
    if (G.was_down == 1 && press && !eaten && !G.paused) G.was_down = 2;
    float ax = in->ax, ay = in->ay;
    int movekey = in->left || in->right || in->up || in->down || fabsf(ax) > 0.3f || fabsf(ay) > 0.3f;
    if (movekey && G.was_down != 2) {
        G.control_mouse = 0; G.target_x = G.X; G.target_y = G.Y; G.dots_shown = 0; G.pause_btn_shown = 0;
    }
    G.shoot_key = in->space;

    if (!G.paused) {
        /* Sprite3: ship control */
        if (!G.control_mouse) {
            float mx = (float)(in->right - in->left) + ax, my = (float)(in->up - in->down) + ay;
            mx = mx > 1 ? 1 : (mx < -1 ? -1 : mx);
            my = my > 1 ? 1 : (my < -1 ? -1 : my);
            G.X += mx * 10 * f; G.Y += my * 10 * f;
        } else {
            float vx = (G.target_x - G.X) / 10, vy = (G.target_y - G.Y) / 10;
            vx = vx > 10 ? 10 : (vx < -10 ? -10 : vx);
            vy = vy > 10 ? 10 : (vy < -10 ? -10 : vy);
            G.X += vx * f; G.Y += vy * f;
        }
        G.speed += round((double)(in->speed_up - in->speed_down) * 10 * f) / 50.0;
        if (G.speed > 80) G.speed = 80;
        if (G.speed < 5) G.speed = 5;
        if (G.music_flag) G.timer += dt;
    }
    /* world record popups (You beat world record sprite) */
    if (G.game_time >= 1.0) {
        if (!G.wr_t_beaten && G.timer > G.wr_timer) {
            G.wr_t_beaten = 1; G.wr_timer = G.timer; G.wr_t_next = 5;
            G.popup_active = 1; G.popup_t = 0; audio_play(SND_POP, GRP_SPRITE); G.view.records_dirty = 1;
        }
        if (!G.wr_s_beaten && G.score > G.wr_score) {
            G.wr_s_beaten = 1; G.wr_score = G.score; G.wr_s_next = 5;
            G.popup_active = 1; G.popup_t = 0; audio_play(SND_POP, GRP_SPRITE); G.view.records_dirty = 1;
        }
    }
    if (G.popup_active) { G.popup_t += dt; if (G.popup_t >= 10.0) G.popup_active = 0; }
    if (G.wr_t_beaten && !G.popup_active) {
        G.wr_t_next -= dt; if (G.wr_t_next <= 0) { G.wr_timer = G.timer; G.wr_t_next = 5; G.view.records_dirty = 1; }
    }
    if (G.wr_s_beaten && !G.popup_active) {
        G.wr_s_next -= dt; if (G.wr_s_next <= 0) { G.wr_score = G.score; G.wr_s_next = 5; G.view.records_dirty = 1; }
    }
}

void game_update(double dt, const GameInput *in) {
    if (dt > 0.1) dt = 0.1;
    if (dt < 0) dt = 0;
    float f = (float)(dt * 30.0);
    G.t += dt;
    G.fps_acc += dt; G.fps_frames++;
    if (G.fps_acc >= 0.25) { G.fps = (float)(round(G.fps_frames / G.fps_acc * 10) / 10); G.fps_acc = 0; G.fps_frames = 0; }

    misc_scripts(dt, in);

    switch (G.state) {
    case GS_TITLE: case GS_STARTING:
        update_title(dt, f, in);
        G.bg_effect += (float)(dt * 15.0 * G.bg_scripts);
        break;
    case GS_PLAYING:
        update_playing(dt, f, in);
        break;
    case GS_LOSE:
        G.seq_t += dt;
        if (in->quit_pressed) { title_broadcast(); break; }
        if (G.seq_phase == 0 && G.seq_t >= 1.0) {
            if (G.player_music) { audio_play(SND_LOSE, GRP_STAGE); G.seq_phase = 1; G.seq_t = 0; }
            else title_broadcast();
        } else if (G.seq_phase == 1 && G.seq_t >= 3.0) title_broadcast();
        break;
    case GS_EASTER:
        G.seq_t += dt;
        if (G.seq_phase == 0 && G.seq_t >= 1.0) { audio_play(SND_EASTER, GRP_STAGE); G.seq_phase = 1; G.seq_t = 0; }
        else if (G.seq_phase == 1 && G.seq_t > 0.2 && !audio_sound_playing(SND_EASTER)) { G.seq_phase = 2; G.seq_t = 0; }
        else if (G.seq_phase == 2 && G.seq_t >= 1.0) title_broadcast();
        if (in->quit_pressed && G.seq_phase >= 1) { audio_stop_all(); title_broadcast(); }
        break;
    }
    if (G.state != GS_TITLE && G.state != GS_STARTING) G.bg_effect = 0;

    /* Scratch threads at 30 Hz */
    G.tick_acc += dt;
    while (G.tick_acc >= TICK) { G.tick_acc -= TICK; tick_threads(); }

    /* Render Frame: clones move (only while the render loop runs and not paused) */
    if ((G.state == GS_TITLE || G.state == GS_STARTING || G.state == GS_PLAYING) && !G.paused)
        update_objects(f, (float)dt);

    /* music (Stage forever loop) + crossfade between the two mixes */
    if (G.music_flag && G.player_music && !audio_music_playing() && G.state != GS_EASTER) {
        audio_music_start(G.first_title_music);
        G.first_title_music = 0;
    }
    int alt = G.mode_sel || G.paused;
    G.vol_stage = ease(G.vol_stage, alt ? 0.0f : 100.0f, 10, f);
    G.vol_thumb = ease(G.vol_thumb, alt ? 100.0f : 0.0f, 10, f);
    audio_set_group_volume(GRP_STAGE, G.vol_stage / 100.0f);
    audio_set_group_volume(GRP_THUMB, G.vol_thumb / 100.0f);

    /* monitors */
    mon_set(MON_SPEED, G.speed);
    mon_set(MON_WR_TIMER, G.wr_timer);
    mon_set(MON_TIMER, floor(G.timer * 1000.0 + 1e-6) / 1000.0);
    mon_set(MON_SCORE, G.score);
    mon_set(MON_WR_SCORE, G.wr_score);
    mon_set(MON_AMMO, G.ammo);
    mon_setf(MON_FPS, G.fps);
    mon_set(MON_STARS, G.stars_n);
    if (G.state == GS_EASTER || G.state == GS_LOSE) G.mon[MON_STARS].visible = 0;

    /* view */
    GameView *v = &G.view;
    v->state = G.state; v->paused = G.paused; v->mode_selection = G.mode_sel; v->changelog_open = G.changelog;
    v->hard_mode = G.hard; v->player_music = G.player_music; v->control_mouse = G.control_mouse;
    v->score = G.score; v->ammo = G.ammo; v->speed = G.speed; v->timer = G.timer;
    v->wr_timer = G.wr_timer; v->wr_score = G.wr_score; v->stars_slider = G.stars_n; v->fps = G.fps;
    v->nav_index = G.nav_active ? G.nav_index : -1;
    if (G.bg_effect > 0) {
        /* Scratch colour effect on the black backdrop: HSV(effect/200, 1, 0.055) */
        float h = fmodf(G.bg_effect / 200.0f, 1.0f) * 6.0f, vv = 0.055f;
        int i = (int)h; float fr = h - i, q = vv * (1 - fr), t = vv * fr;
        float r = 0, g = 0, b = 0;
        switch (i % 6) { case 0: r = vv; g = t; break; case 1: r = q; g = vv; break; case 2: g = vv; b = t; break;
                         case 3: g = q; b = vv; break; case 4: r = t; b = vv; break; default: r = vv; b = q; break; }
        v->bg_r = r; v->bg_g = g; v->bg_b = b;
    } else v->bg_r = v->bg_g = v->bg_b = 0;
}

/* ---------------------------------------------------------------- drawing */
static int cmp_dist(const void *a, const void *b) {
    float da = *(const float *)a, db = *(const float *)b;
    return da < db ? 1 : (da > db ? -1 : 0);
}

static DrawItem *push(DrawItem *out, int *n, int max, int co, float x, float y, float size, float alpha) {
    if (*n >= max || alpha <= 0.001f) return NULL;
    DrawItem *d = &out[(*n)++];
    memset(d, 0, sizeof *d);
    d->kind = DI_COSTUME; d->costume = (int16_t)co; d->x = x; d->y = y; d->size = size;
    d->alpha = alpha > 1 ? 1 : alpha;
    return d;
}
static float ghost_alpha(float ghost) { return 1.0f - (ghost < 0 ? 0 : (ghost > 100 ? 100 : ghost)) / 100.0f; }

int game_draw_list(DrawItem *out, int max) {
    int n = 0;
    /* world objects, far to near */
    static float (*order)[2]; static int order_cap;
    if (G.nobj > order_cap) {
        float (*p)[2] = realloc(order, (size_t)G.nobj * sizeof *p);
        if (!p) return 0;
        order = p; order_cap = G.nobj;
    }
    int no = 0;
    for (int i = 0; i < G.nobj; i++) {
        Obj *o = &G.obj[i];
        float rx = o->x - G.X, ry = o->y - G.Y, rz = o->z;
        if (rz <= 30) continue;
        order[no][0] = rx * rx + ry * ry + rz * rz;
        order[no][1] = (float)i;
        no++;
    }
    if (no > 1) qsort(order, no, sizeof order[0], cmp_dist);
    int title_ui = G.state == GS_TITLE || G.state == GS_STARTING || (G.state == GS_EASTER && G.title_ghost < 100 && 0);
    int ui_front = title_ui || (G.state == GS_PLAYING && G.paused);
    for (int k = 0; k < no; k++) {
        Obj *o = &G.obj[(int)order[k][1]];
        float rx = o->x - G.X, ry = o->y - G.Y, rz = o->z;
        /* Scratch's sprite fencing (the "0.1" costume at 15429% trick) is deliberately not
           replicated: objects move off the screen edges freely */
        float sx = rx * (DIST / rz), sy = ry * (DIST / rz);
        int co = sc_to_co[o->c];
        float size = min_size(co, DIST / rz);
        /* cull only when far outside any viewport (3DS top is +-300 x +-180, wide PC windows more) */
        const CostumeInfo *ci = &costume_info[co];
        if (sx + (ci->w - ci->rcx) * size < -1000 || sx - ci->rcx * size > 1000 ||
            sy + ci->rcy * size < -600 || sy - (ci->h - ci->rcy) * size > 600) continue;
        float alpha = (o->age * 30.0f + 1.0f) / 10.0f;
        DrawItem *d = push(out, &n, max - UI_RESERVE, co, sx, sy, size, alpha);
        if (!d) continue;
        d->hue = o->hue;
        float dep = 1.0f - DIST / rz;
        if (dep < 0) dep *= 0.5f;
        /* menus sit on the screen plane: keep the world behind them so nothing pops out under them */
        if (ui_front && dep < 0) dep = 0;
        d->depth = dep < -1 ? -1 : (dep > 1 ? 1 : dep);
    }
    if (title_ui) {
        if (n < max) {
            DrawItem *d = &out[n++]; memset(d, 0, sizeof *d);
            d->kind = DI_FILL; d->rgba = 0x000000ffu; d->alpha = ghost_alpha(G.overlay_ghost);
        }
        push(out, &n, max, CO_TITLE, 0, 0, 1, ghost_alpha(G.title_ghost));
        for (int b = 0; b < BTN_COUNT; b++) {
            DrawItem *d = push(out, &n, max, btn_costume(b), btn_x[b], G.btn_y[b], G.btn_size[b], ghost_alpha(G.btn_ghost[b]));
            if (d && b == BTN_TOGGLEMUSIC) d->hue = G.player_music * 50.0f;
        }
        int dco = G.mode_sel ? (G.desc_hard ? CO_DESC_HARD : CO_DESC_NORMAL) : CO_DESCRIPTION;
        push(out, &n, max, dco, 0, G.desc_y, 1, ghost_alpha(G.btn_ghost[0]));
        if (G.sel_alive) push(out, &n, max, CO_SELECTED, G.sel_x, -5, G.sel_size, ghost_alpha(G.sel_ghost));
        if (G.changelog) push(out, &n, max, CO_CHANGELOG, 0, 0, 1, ghost_alpha(G.chg_ghost));
    }
    if (G.popup_active) {
        int steps = (int)(G.popup_t / 0.1);
        push(out, &n, max, CO_WR, 0, 100 + steps * 0.5f, 1, 1.0f - steps / 100.0f);
    }
    if ((G.state == GS_PLAYING || G.state == GS_LOSE) && G.dots_shown) {
        static const int ids[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 10 };
        for (int i = 0; i < 9; i++) {
            float x = ((G.target_x - G.X) / 10) * ids[i] / 5, y = ((G.target_y - G.Y) / 10) * ids[i] / 5;
            push(out, &n, max, CO_DOT, x, y, 0.1f, 0.1f);
        }
    }
    if (G.state == GS_PLAYING && G.paused) push(out, &n, max, CO_PAUSE_PANEL, 0, 0, 1, 1);
    if (game_pause_button_visible()) {
        DrawItem *d = push(out, &n, max, CO_PAUSE_BUTTON, 0, 0, 1, 1);
        if (d) d->flags = DIF_PC_ONLY;
    }
    return n;
}

const GameView *game_view(void) { return &G.view; }
Monitor *game_monitors(void) { return G.mon; }

void game_buttons(ButtonView out[BTN_COUNT]) {
    for (int b = 0; b < BTN_COUNT; b++) {
        out[b].costume = btn_costume(b);
        out[b].x = btn_x[b]; out[b].y = G.btn_y[b]; out[b].size = G.btn_size[b];
        out[b].alpha = ghost_alpha(G.btn_ghost[b]);
        out[b].hovered = b == G.hovered;
        out[b].visible = G.state == GS_TITLE;
    }
}

void game_init(uint32_t seed, double wr_timer, double wr_score, int player_music) {
    memset(&G, 0, sizeof G);
    G.obj = g_objs;
    G.rng = seed ? seed : 0x12345678u;
    G.wr_timer = wr_timer; G.wr_score = wr_score;
    G.player_music = player_music;
    G.stars_n = 250;
    G.shared_costume = SC_STAR1;
    G.desc_hard = 1;              /* saved value of "Mode description" in the sb3 */
    G.dots_shown = 1;
    G.vol_stage = 100; G.vol_thumb = 100;
    G.first_title = 1;
    G.first_title_music = 1;
    G.nav_index = BTN_PLAY;
    static const char *labels[MON_COUNT] = { "Ship speed:", "\xe2\x98\x81 World record timer:", "Timer:", "Score:",
        "\xe2\x98\x81 World record score:", "Ammo:", "FPS:", "Number of stars (turn down if you're laggy)" };
    static const float pos[MON_COUNT][2] = { {5, 5}, {288, 0}, {376, 34}, {201, 317}, {156, 336}, {0, 332}, {385, 330}, {79, 236} };
    for (int i = 0; i < MON_COUNT; i++) { G.mon[i].label = labels[i]; G.mon[i].x = pos[i][0]; G.mon[i].y = pos[i][1]; }
    G.mon[MON_FPS].visible = 1;
    title_broadcast();
    G.view.records_dirty = 0;
}
