#include <sys/stat.h>
/* Space Destroyers 3D - Nintendo 3DS backend (citro2d/citro3d, ndsp).
 * Top screen: the game with per-object stereoscopic depth. Bottom screen: HUD / touch menu. */
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "game.h"
#include "audio.h"
#include "image.h"
#include "font.h"

#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240
#define DS_SCALE (240.0f / 360.0f)
/* px per eye at full slider. World depth is -1..1, so max disparity is +-11 px (~0.56 deg on a
   New 3DS XL at 30 cm, inside the usual 1 deg comfort budget). Menus/HUD sit on the screen plane. */
#define MAX_EYE_SHIFT 5.5f

/* ------------------------------------------------------------------ files / locks */
void *plat_read_file(const char *name, long *size) {
    char path[256];
    snprintf(path, sizeof path, "romfs:/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *d = malloc(n + 1);
    if (d) { fread(d, 1, n, f); ((char *)d)[n] = 0; }
    fclose(f);
    *size = n;
    return d;
}
static LightLock audio_lock;
void plat_audio_lock(void) { LightLock_Lock(&audio_lock); }
void plat_audio_unlock(void) { LightLock_Unlock(&audio_lock); }

/* ------------------------------------------------------------------ textures */
typedef struct { C3D_Tex tex; Tex3DS_SubTexture sub; C2D_Image img; int ok; } DsTex;
static DsTex ctex[CO_COUNT], cmask[CO_COUNT], chue50;

static inline u32 morton8(u32 x, u32 y) {
    x = (x | (x << 2)) & 0x33; x = (x | (x << 1)) & 0x55;
    y = (y | (y << 2)) & 0x33; y = (y | (y << 1)) & 0x55;
    return x | (y << 1);
}
/* linear RGBA8 (top row first) -> PICA tiled ABGR; memory row 0 is sampled at v=1 (image top) */
static void swizzle(const Image *im, u32 *dst) {
    int w = im->w, h = im->h;
    for (int y = 0; y < h; y++) {
        int ty = y;
        for (int x = 0; x < w; x++) {
            const u8 *p = im->px + ((size_t)y * w + x) * 4;
            u32 off = (((ty >> 3) * (w >> 3) + (x >> 3)) << 6) + morton8(x & 7, ty & 7);
            dst[off] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
        }
    }
}
static int make_tex(DsTex *t, const char *file, int cw, int ch) {
    Image im;
    if (!image_load(file, &im)) return 0;
    int levels = 0;
    while ((im.w >> (levels + 1)) >= 8 && (im.h >> (levels + 1)) >= 8) levels++;
    C3D_TexInitParams p = { (u16)im.w, (u16)im.h, (u8)levels, GPU_RGBA8, GPU_TEX_2D, false };
    if (!C3D_TexInitWithParams(&t->tex, NULL, p)) { image_free(&im); return 0; }
    Image cur = im;
    for (int l = 0; l <= levels; l++) {
        u32 sz; void *dst = C3D_Tex2DGetImagePtr(&t->tex, l, &sz);
        swizzle(&cur, dst);
        if (l < levels) { Image nx; image_half(&cur, &nx); if (cur.px != im.px) image_free(&cur); cur = nx; }
    }
    if (cur.px != im.px) image_free(&cur);
    C3D_TexFlush(&t->tex);
    C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetFilterMipmap(&t->tex, GPU_LINEAR);
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    t->sub.width = (u16)cw; t->sub.height = (u16)ch;
    t->sub.left = 0; t->sub.top = 1.0f;
    t->sub.right = (float)cw / im.w; t->sub.bottom = 1.0f - (float)ch / im.h;
    t->img.tex = &t->tex; t->img.subtex = &t->sub;
    t->ok = 1;
    image_free(&im);
    return 1;
}
static void load_textures(void) {
    char name[96];
    for (int i = 0; i < CO_COUNT; i++) {
        const CostumeInfo *ci = &costume_info[i];
        snprintf(name, sizeof name, ci->hues ? "%s_h0.png" : "%s.png", ci->file);
        make_tex(&ctex[i], name, ci->ds_w, ci->ds_h);
        if (ci->layered) { snprintf(name, sizeof name, "%s_mask.png", ci->file); make_tex(&cmask[i], name, ci->ds_w, ci->ds_h); }
        if (ci->hues) { snprintf(name, sizeof name, "%s_h50.png", ci->file); make_tex(&chue50, name, ci->ds_w, ci->ds_h); }
    }
}

/* ------------------------------------------------------------------ fonts (stb_truetype -> RGBA8 white atlas) */
typedef struct { Font f; C3D_Tex tex; int ok; } DsFont;
static DsFont fnt_label, fnt_pixel, fnt_big;
static void make_font(DsFont *df, const void *ttf, const void *fallback, float px) {
    if (!font_bake(&df->f, ttf, fallback, px)) return;
    Image im; im.w = df->f.aw; im.h = df->f.ah; im.px = malloc((size_t)im.w * im.h * 4);
    for (int i = 0; i < im.w * im.h; i++) { im.px[i * 4] = im.px[i * 4 + 1] = im.px[i * 4 + 2] = 255; im.px[i * 4 + 3] = df->f.atlas[i]; }
    C3D_TexInitParams p = { (u16)im.w, (u16)im.h, 0, GPU_RGBA8, GPU_TEX_2D, false };
    if (C3D_TexInitWithParams(&df->tex, NULL, p)) {
        u32 sz; swizzle(&im, C3D_Tex2DGetImagePtr(&df->tex, 0, &sz));
        C3D_TexFlush(&df->tex);
        C3D_TexSetFilter(&df->tex, GPU_LINEAR, GPU_LINEAR);
        df->ok = 1;
    }
    free(im.px);
}
typedef struct { DsFont *df; u32 col; } TxtCtx;
static void ds_text_quad(void *ud, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
    TxtCtx *c = ud;
    /* real-hw GPU hang guard: skip NaN/degenerate glyph quads */
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) || x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
    Tex3DS_SubTexture sub = { (u16)(x1 - x0 + 0.5f), (u16)(y1 - y0 + 0.5f), s0, 1.0f - t0, s1, 1.0f - t1 };
    C2D_Image img = { &c->df->tex, &sub };
    C2D_ImageTint tint; C2D_PlainImageTint(&tint, c->col, 1.0f);
    C2D_DrawImageAt(img, x0, y0, 0.5f, &tint, (x1 - x0) / sub.width, (y1 - y0) / sub.height);
}
static float text_w(DsFont *df, const char *s) { return df->ok ? font_width(&df->f, s) : 0; }
static void text(DsFont *df, const char *s, float x, float y_top, u32 col) {
    if (!df->ok) return;
    TxtCtx c = { df, col };
    font_draw(&df->f, s, floorf(x), floorf(y_top + df->f.ascent + 0.5f), ds_text_quad, &c);
}
static void text_c(DsFont *df, const char *s, float cx, float y, u32 col) { text(df, s, cx - text_w(df, s) / 2, y, col); }

/* ------------------------------------------------------------------ drawing helpers */
static void rrect(float x, float y, float w, float h, float r, u32 col) {
    C2D_DrawRectSolid(x + r, y, 0.5f, w - 2 * r, h, col);
    C2D_DrawRectSolid(x, y + r, 0.5f, r, h - 2 * r, col);
    C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2 * r, col);
    C2D_DrawCircleSolid(x + r, y + r, 0.5f, r, col);
    C2D_DrawCircleSolid(x + w - r, y + r, 0.5f, r, col);
    C2D_DrawCircleSolid(x + r, y + h - r, 0.5f, r, col);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.5f, r, col);
}
static u32 hsv_col(float h, float s, float v, float a) {
    h = (h - floorf(h)) * 6.0f;
    int i = (int)h; float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
                     case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
    return C2D_Color32f(r, g, b, a);
}

/* costume at screen pixel position (x,y = rotation centre), scale = px per costume px */
static void draw_tex_px(DsTex *t, const CostumeInfo *ci, float x, float y, float scale, u32 tint_col) {
    if (!t->ok) return;
    float left = x + (ci->ds_ox - ci->rcx) * scale, top = y + (ci->ds_oy - ci->rcy) * scale;
    float s = scale / ci->ds_scale;
    if (!isfinite(left) || !isfinite(top) || !isfinite(s) || s <= 0 || s > 64) return;
    /* off the top screen (objects are no longer fenced to the stage) */
    if (left > TOP_W + 16 || top > TOP_H || left + t->sub.width * s < -16 || top + t->sub.height * s < 0) return;
    C2D_ImageTint tint; C2D_PlainImageTint(&tint, tint_col, 1.0f);
    C2D_DrawImageAt(t->img, left, top, 0.5f, &tint, s, s);
}
static void draw_costume_px(int co, float x, float y, float scale, float alpha, float hue) {
    const CostumeInfo *ci = &costume_info[co];
    if (ci->layered) {
        /* Scratch colour effect on 2-colour (white + yellow) art, exact: white base tinted
           HSV(d, .09, 1) (min-saturation rule), yellow mask tinted HSV(h_yellow + d, 1, 1) */
        float d = hue / 200.0f;
        u32 base = hue != 0 ? hsv_col(d, 0.09f, 1, alpha) : C2D_Color32f(1, 1, 1, alpha);
        draw_tex_px(&ctex[co], ci, x, y, scale, base);
        draw_tex_px(&cmask[co], ci, x, y, scale, hsv_col(0.15294f + d, 1, 1, alpha));
    } else if (ci->hues && hue >= 25) {
        draw_tex_px(&chue50, ci, x, y, scale, C2D_Color32f(1, 1, 1, alpha));
    } else draw_tex_px(&ctex[co], ci, x, y, scale, C2D_Color32f(1, 1, 1, alpha));
}

/* ------------------------------------------------------------------ top screen */
static DrawItem items[MAX_DRAW];
static int nitems;
/* "3D colour shimmer" option (off by default): in stereo the right eye draws the hue-cycling
   projectiles (your bullets, the ammo pellets) half a colour turn away from the left eye */
static int shimmer;
#define SHIMMER_HUE 100.0f   /* Scratch colour effect units, 200 = one turn */
static void draw_top(float eye_shift, float hue_off) {
    const GameView *v = game_view();
    C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, TOP_H, C2D_Color32f(v->bg_r, v->bg_g, v->bg_b, 1));
    for (int i = 0; i < nitems; i++) {
        DrawItem *d = &items[i];
        if (d->flags & DIF_PC_ONLY) continue;
        if (d->kind == DI_FILL) {
            C2D_DrawRectSolid(0, 0, 0.5f, TOP_W, TOP_H, C2D_Color32f(0, 0, 0, d->alpha));
            continue;
        }
        float px = TOP_W / 2 + d->x * DS_SCALE + d->depth * eye_shift;
        float py = TOP_H / 2 - d->y * DS_SCALE;
        float hue = d->hue;
        if (d->costume == CO_BULLET || d->costume == CO_POWERUP) hue += hue_off;
        draw_costume_px(d->costume, px, py, d->size * DS_SCALE, d->alpha, hue);
    }
}

/* ------------------------------------------------------------------ bottom screen */
#define COL_BG      C2D_Color32(0x1d, 0x1c, 0x29, 0xff)
#define COL_YELLOW  C2D_Color32(0xff, 0xe6, 0x00, 0xff)
#define COL_MONBG   C2D_Color32(0xe6, 0xf0, 0xff, 0xff)
#define COL_MONBRD  C2D_Color32(0xb8, 0xbf, 0xcc, 0xff)
#define COL_LABEL   C2D_Color32(0x57, 0x5e, 0x75, 0xff)
#define COL_ORANGE  C2D_Color32(0xff, 0x8c, 0x1a, 0xff)
#define COL_WHITE   C2D_Color32(0xff, 0xff, 0xff, 0xff)
#define COL_BLACK   C2D_Color32(0x00, 0x00, 0x00, 0xff)

typedef struct { float x, y, w, h; } Rect;
static int in_rect(Rect r, int tx, int ty) { return tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h; }

static void monitor(float x, float y, const char *label, const char *value, float minw) {
    float lw = text_w(&fnt_label, label), vw = text_w(&fnt_label, value);
    float valw = fmaxf(36, vw + 8);
    float w = fmaxf(minw, 4 + 4 + lw + 6 + valw + 4), h = 22;
    rrect(x, y, w, h, 4, COL_MONBRD);
    rrect(x + 1, y + 1, w - 2, h - 2, 3, COL_MONBG);
    text(&fnt_label, label, x + 7, y + 4, COL_LABEL);
    float vx = x + w - 4 - valw;
    rrect(vx, y + 3, valw, h - 6, 4, COL_ORANGE);
    text(&fnt_label, value, vx + (valw - vw) / 2, y + 4, COL_WHITE);
}

static void button_box(Rect r, const char *label, int highlighted, u32 fill) {
    rrect(r.x - 1, r.y - 1, r.w + 2, r.h + 2, 3, highlighted ? COL_YELLOW : COL_BLACK);
    C2D_DrawRectSolid(r.x + 1, r.y + 1, 0.5f, r.w - 2, r.h - 2, fill);
    text_c(&fnt_pixel, label, r.x + r.w / 2, r.y + r.h / 2 - fnt_pixel.f.px * 0.62f, COL_BLACK);
}

static Rect r_btn[BTN_COUNT], r_slider, r_pause, r_resume, r_quit, r_shim_title, r_shim_pause;
static int slider_drag;

static void layout_bottom(void) {
    r_btn[BTN_WHATSNEW] = (Rect){ 6, 92, 98, 50 };
    r_btn[BTN_PLAY] = (Rect){ 111, 86, 98, 62 };
    r_btn[BTN_SELECTMODE] = (Rect){ 216, 92, 98, 50 };
    r_btn[BTN_TOGGLEMUSIC] = (Rect){ 216, 176, 98, 56 };
    r_slider = (Rect){ 8, 186, 196, 40 };
    r_pause = (Rect){ 8, 190, 96, 42 };
    r_resume = (Rect){ 30, 120, 120, 44 };
    r_quit = (Rect){ 170, 120, 120, 44 };
    r_shim_title = (Rect){ 8, 153, 196, 27 };
    r_shim_pause = (Rect){ 62, 86, 196, 27 };
}

static void draw_slider(Rect r) {
    char v[16]; snprintf(v, sizeof v, "%d", game_view()->stars_slider);
    rrect(r.x, r.y, r.w, r.h, 4, COL_MONBRD);
    rrect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, COL_MONBG);
    text(&fnt_label, "Number of stars", r.x + 6, r.y + 3, COL_LABEL);
    float vw = text_w(&fnt_label, v), valw = fmaxf(30, vw + 8);
    rrect(r.x + r.w - 5 - valw, r.y + 3, valw, 16, 4, COL_ORANGE);
    text(&fnt_label, v, r.x + r.w - 5 - valw + (valw - vw) / 2, r.y + 3, COL_WHITE);
    float sx0 = r.x + 10, sx1 = r.x + r.w - 10, sy = r.y + 30, t = game_view()->stars_slider / 300.0f;
    rrect(sx0, sy - 2, sx1 - sx0, 4, 2, C2D_Color32(0xc6, 0xcb, 0xd6, 0xff));
    rrect(sx0, sy - 2, (sx1 - sx0) * t + 2, 4, 2, C2D_Color32(0x4c, 0x97, 0xff, 0xff));
    C2D_DrawCircleSolid(sx0 + (sx1 - sx0) * t, sy, 0.5f, 6, COL_WHITE);
    C2D_DrawCircleSolid(sx0 + (sx1 - sx0) * t, sy, 0.5f, 5, C2D_Color32(0x4c, 0x97, 0xff, 0xff));
}

/* on/off setting box, same style as the slider monitor */
static void draw_toggle(Rect r, const char *label, int on) {
    const char *v = on ? "On" : "Off";
    float ty = floorf(r.y + (r.h - 16) / 2);
    rrect(r.x, r.y, r.w, r.h, 4, COL_MONBRD);
    rrect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, 3, COL_MONBG);
    text(&fnt_label, label, r.x + 6, ty, COL_LABEL);
    float vw = text_w(&fnt_label, v), valw = fmaxf(36, vw + 8);
    rrect(r.x + r.w - 5 - valw, ty, valw, 16, 4, on ? COL_ORANGE : C2D_Color32(0x9a, 0xa0, 0xae, 0xff));
    text(&fnt_label, v, r.x + r.w - 5 - valw + (valw - vw) / 2, ty, COL_WHITE);
}

static const char *btn_label(int co) {
    switch (co) {
    case CO_PLAY: return "Play"; case CO_BACK: return "Back"; case CO_SELECTMODE: return "Select mode";
    case CO_HARDMODE: return "Hard mode"; case CO_WHATSNEW: return "What's new?"; case CO_NORMALMODE: return "Normal mode";
    default: return "Toggle Music";
    }
}

static void draw_bottom(void) {
    const GameView *v = game_view();
    Monitor *m = game_monitors();
    C2D_DrawRectSolid(0, 0, 0.5f, BOT_W, BOT_H, COL_BG);
    if (v->state == GS_TITLE || v->state == GS_STARTING) {
        text_c(&fnt_big, "Space Destroyers 3D", BOT_W / 2, 6, COL_YELLOW);
        if (v->changelog_open) {
            text_c(&fnt_pixel, "What's new is on the top screen.", BOT_W / 2, 80, COL_YELLOW);
            text_c(&fnt_pixel, "Tap or press any button to close", BOT_W / 2, 110, COL_YELLOW);
            return;
        }
        text_c(&fnt_pixel, v->mode_selection ? "Choose a mode, then Back" : "D-Pad + A or touch.  START: play",
               BOT_W / 2, 40, C2D_Color32(0xff, 0xe6, 0x00, 0xb0));
        ButtonView bv[BTN_COUNT]; game_buttons(bv);
        for (int b = 0; b < BTN_COUNT; b++) {
            /* the real button costumes, at bottom-screen size */
            const CostumeInfo *ci = &costume_info[bv[b].costume];
            Rect r = r_btn[b];
            float sc = (b == BTN_PLAY ? 0.8f : 0.74f) * (bv[b].hovered ? 1.05f : 1.0f);
            int sel = v->mode_selection && ((bv[b].costume == CO_HARDMODE && v->hard_mode) || (bv[b].costume == CO_NORMALMODE && !v->hard_mode));
            float w = ci->w * sc, h = ci->h * sc, x0 = r.x + r.w / 2 - w / 2, y0 = r.y + r.h / 2 - h / 2;
            if (bv[b].hovered) rrect(x0 - 3, y0 - 3, w + 6, h + 6, 4, COL_YELLOW);
            draw_costume_px(bv[b].costume, x0 + ci->rcx * sc, y0 + ci->rcy * sc, sc, 1.0f,
                            b == BTN_TOGGLEMUSIC ? v->player_music * 50.0f : 0.0f);
            if (sel) {
                const CostumeInfo *si = &costume_info[CO_SELECTED];
                float ss = 0.7f;
                draw_costume_px(CO_SELECTED, r.x + r.w / 2 - si->w * ss / 2 + si->rcx * ss, r.y + r.h + 4 + si->rcy * ss, ss, 1.0f, 0);
            }
        }
        if (!v->mode_selection) { draw_slider(r_slider); draw_toggle(r_shim_title, "3D colour shimmer", shimmer); }
        else {
            text(&fnt_pixel, v->hard_mode ? "Hard: planets every frame," : "Normal: the original mode,", 8, 178, COL_YELLOW);
            text(&fnt_pixel, v->hard_mode ? "full speed, no stars." : "best to get started.", 8, 196, COL_YELLOW);
        }
        return;
    }
    if (v->state == GS_EASTER) {
        text_c(&fnt_big, "???", BOT_W / 2, 90, COL_YELLOW);
        text_c(&fnt_pixel, "SELECT: back to title", BOT_W / 2, 200, COL_YELLOW);
        return;
    }
    if (v->paused) {
        text_c(&fnt_big, "Game Paused", BOT_W / 2, 30, COL_YELLOW);
        text_c(&fnt_pixel, "START to resume, SELECT to quit", BOT_W / 2, 66, COL_YELLOW);
        button_box(r_resume, "Resume", 0, COL_WHITE);
        button_box(r_quit, "Quit to title", 0, COL_WHITE);
        draw_toggle(r_shim_pause, "3D colour shimmer", shimmer);
        draw_slider((Rect){ 62, 186, 196, 40 });
        return;
    }
    /* in-game HUD (Scratch variable monitors) */
    monitor(6, 8, "Score:", m[MON_SCORE].value, 0);
    monitor(170, 8, "Ammo:", m[MON_AMMO].value, 144);
    monitor(6, 36, "Timer:", m[MON_TIMER].value, 0);
    monitor(170, 36, "Ship speed:", m[MON_SPEED].value, 144);
    monitor(6, 70, m[MON_WR_SCORE].label, m[MON_WR_SCORE].value, 0);
    monitor(6, 98, m[MON_WR_TIMER].label, m[MON_WR_TIMER].value, 0);
    if (v->state == GS_PLAYING) {
        text_c(&fnt_pixel, "Drag to steer, tap to shoot", BOT_W / 2 + 50, 150, C2D_Color32(0xff, 0xe6, 0x00, 0x90));
        button_box(r_pause, "Pause", 0, COL_WHITE);
    } else text_c(&fnt_big, "Game over", BOT_W / 2, 150, COL_YELLOW);
    char fps[48]; snprintf(fps, sizeof fps, "FPS: %s", m[MON_FPS].value);
    text(&fnt_label, fps, BOT_W - text_w(&fnt_label, fps) - 8, BOT_H - 22, C2D_Color32(0x80, 0x80, 0x90, 0xff));
}

/* ------------------------------------------------------------------ audio thread (ndsp) */
#define AUD_FRAMES 1024
#define AUD_BUFS 4
static ndspWaveBuf wbuf[AUD_BUFS];
static int16_t *aud_data;
static LightEvent aud_event;
static volatile int aud_quit;
static int have_ndsp;
static void ndsp_cb(void *d) { (void)d; LightEvent_Signal(&aud_event); }
static void audio_thread(void *arg) {
    (void)arg;
    while (!aud_quit) {
        for (int i = 0; i < AUD_BUFS; i++) {
            if (wbuf[i].status == NDSP_WBUF_DONE || wbuf[i].status == NDSP_WBUF_FREE) {
                audio_mix(wbuf[i].data_pcm16, AUD_FRAMES);
                DSP_FlushDataCache(wbuf[i].data_pcm16, AUD_FRAMES * 4);
                ndspChnWaveBufAdd(0, &wbuf[i]);
            }
        }
        LightEvent_Wait(&aud_event);
    }
}

/* ------------------------------------------------------------------ records */
#define SAVE_DIR "sdmc:/3ds/space-destroyers-3d"
static void load_records(double *t, double *s, int *pm) {
    *t = 687.19; *s = 1348; *pm = 1;
    FILE *f = fopen(SAVE_DIR "/records.txt", "r");
    if (!f) return;
    double a, b; int c, d;
    if (fscanf(f, "%lf %lf %d", &a, &b, &c) == 3) {
        if (a > *t) *t = a;
        if (b > *s) *s = b;
        *pm = c;
        if (fscanf(f, "%d", &d) == 1) shimmer = d == 1;   /* field added later: missing = off */
    }
    fclose(f);
}
static void save_records(void) {
    const GameView *v = game_view();
    mkdir("sdmc:/3ds", 0777); mkdir(SAVE_DIR, 0777);
    FILE *f = fopen(SAVE_DIR "/records.txt", "w");
    if (!f) return;
    fprintf(f, "%.3f %.0f %d %d\n", v->wr_timer, v->wr_score, v->player_music, shimmer);
    fclose(f);
}

static volatile int suspended;
static aptHookCookie apt_cookie;
static void apt_hook(APT_HookType h, void *p) { (void)p; if (h == APTHOOK_ONSUSPEND || h == APTHOOK_ONSLEEP) suspended = 1; }

/* ------------------------------------------------------------------ main */
int main(void) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    romfsInit();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 4);
    C2D_Prepare();
    C2D_SetTintMode(C2D_TintMult);
    C3D_RenderTarget *top_l = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *top_r = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    aptHook(&apt_cookie, apt_hook, NULL);

    load_textures();
    long sz;
    void *ttf_bold = plat_read_file("LiberationSans-Bold.ttf", &sz);
    void *ttf_pixel = plat_read_file("Grand9K-Pixel.ttf", &sz);
    void *ttf_dv = plat_read_file("DejaVuSans.ttf", &sz);
    if (ttf_bold) make_font(&fnt_label, ttf_bold, ttf_dv, 13.5f);
    if (ttf_pixel) { make_font(&fnt_pixel, ttf_pixel, NULL, 16.0f); make_font(&fnt_big, ttf_pixel, NULL, 24.0f);
        C3D_TexSetFilter(&fnt_pixel.tex, GPU_NEAREST, GPU_NEAREST); C3D_TexSetFilter(&fnt_big.tex, GPU_NEAREST, GPU_NEAREST); }
    layout_bottom();

    /* audio: one stereo channel fed by the shared software mixer on a worker thread */
    LightLock_Init(&audio_lock);
    int have_audio = audio_init();
    Thread th = NULL;
    if (have_audio && R_SUCCEEDED(ndspInit())) {
        have_ndsp = 1;
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, AUDIO_RATE);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = { 1, 1 };
        ndspChnSetMix(0, mix);
        aud_data = linearAlloc(AUD_FRAMES * 4 * AUD_BUFS);
        memset(aud_data, 0, AUD_FRAMES * 4 * AUD_BUFS);
        memset(wbuf, 0, sizeof wbuf);
        for (int i = 0; i < AUD_BUFS; i++) { wbuf[i].data_pcm16 = aud_data + i * AUD_FRAMES * 2; wbuf[i].nsamples = AUD_FRAMES; }
        LightEvent_Init(&aud_event, RESET_ONESHOT);
        ndspSetCallback(ndsp_cb, NULL);
        bool n3ds = false; APT_CheckNew3DS(&n3ds);
        if (!n3ds) APT_SetAppCpuTimeLimit(30);
        s32 prio = 0x30; svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        int core = n3ds ? 2 : 1;
        th = threadCreate(audio_thread, NULL, 32 * 1024, prio - 1, core, false);
        if (!th) { core = -2; th = threadCreate(audio_thread, NULL, 32 * 1024, prio + 1, -2, false); }
    }

    double wr_t, wr_s; int pm;
    load_records(&wr_t, &wr_s, &pm);
    game_init((uint32_t)svcGetSystemTick(), wr_t, wr_s, pm);

    u64 last = svcGetSystemTick();
    int nav_boot = 1;
    int touch_was = 0, touch_btn = -1, touch_on_ui = 0, touch_shim = 0, last_tx = 0, last_ty = 0;
    double autotest_t = 0;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kd = hidKeysDown(), kh = hidKeysHeld();
        touchPosition tp; hidTouchRead(&tp);
        int touching = (kh & KEY_TOUCH) != 0;
        circlePosition cp; hidCircleRead(&cp);
        const GameView *v = game_view();
        GameInput in; memset(&in, 0, sizeof in);
        int on_title = v->state == GS_TITLE || v->state == GS_STARTING;

        if ((kd & KEY_SELECT) && v->state == GS_TITLE) break;      /* exit to the Home Menu / hbmenu */

        in.left = !!(kh & KEY_DLEFT); in.right = !!(kh & KEY_DRIGHT);
        in.up = !!(kh & KEY_DUP); in.down = !!(kh & KEY_DDOWN);
        float cx = cp.dx / 150.0f, cy = cp.dy / 150.0f, mag = sqrtf(cx * cx + cy * cy);
        if (mag > 0.15f) { float s = fminf(1.0f, (mag - 0.15f) / 0.7f) / mag; in.ax = cx * s; in.ay = cy * s; }
        in.speed_up = !!(kh & (KEY_R | KEY_ZR)); in.speed_down = !!(kh & (KEY_L | KEY_ZL));
        in.any_key = (kh & ~KEY_TOUCH) != 0 || touching;
        in.egg_up = in.up; in.egg_down = in.down; in.egg_left = in.left; in.egg_right = in.right;
        in.egg_b = !!(kh & KEY_B); in.egg_a = !!(kh & KEY_A);
        in.egg_up_pressed = !!(kd & KEY_DUP);
        in.pause_pressed = !!(kd & KEY_START) && !on_title;
        in.quit_pressed = !!(kd & KEY_SELECT);
        in.back_pressed = !!(kd & KEY_B);
        in.music_key_pressed = !!(kd & KEY_X);
        in.nav_left = !!(kd & (KEY_DLEFT | KEY_CPAD_LEFT)); in.nav_right = !!(kd & (KEY_DRIGHT | KEY_CPAD_RIGHT));
        in.nav_up = !!(kd & (KEY_DUP | KEY_CPAD_UP)); in.nav_down = !!(kd & (KEY_DDOWN | KEY_CPAD_DOWN));
        if (nav_boot && on_title) { in.nav_up = 1; nav_boot = 0; }   /* highlight Play from the start */
        if (on_title) {
            in.confirm_pressed = !!(kd & KEY_A);
            in.space = !!(kh & KEY_START);
        } else in.space = !!(kh & KEY_A);
        in.mouse_hover_ok = 0;

        /* touch */
        /* the touch position reads (0,0) once released: keep the last touched point */
        if (touching) { last_tx = tp.px; last_ty = tp.py; }
        int tx = last_tx, ty = last_ty;
        if (touching && !touch_was) {
            touch_on_ui = 0; touch_btn = -1; touch_shim = 0;
            if (v->state == GS_TITLE && !v->changelog_open) {
                for (int b = 0; b < BTN_COUNT; b++) if (in_rect(r_btn[b], tx, ty)) { touch_btn = b; touch_on_ui = 1; }
                if (!v->mode_selection && in_rect(r_slider, tx, ty)) { slider_drag = 1; touch_on_ui = 1; }
                if (!v->mode_selection && in_rect(r_shim_title, tx, ty)) { touch_shim = 1; touch_on_ui = 1; }
            } else if (v->state == GS_PLAYING && !v->paused && in_rect(r_pause, tx, ty)) { game_toggle_pause(); touch_on_ui = 1; }
            else if (v->state == GS_PLAYING && v->paused) {
                touch_on_ui = 1;
                if (in_rect(r_resume, tx, ty)) game_toggle_pause();
                else if (in_rect(r_quit, tx, ty)) game_quit_to_title();
                else if (in_rect((Rect){ 62, 186, 196, 40 }, tx, ty)) slider_drag = 2;
                else if (in_rect(r_shim_pause, tx, ty)) touch_shim = 2;
            }
        }
        if (touching && slider_drag) {
            Rect r = slider_drag == 1 ? r_slider : (Rect){ 62, 186, 196, 40 };
            game_set_stars_slider((int)lrintf((tx - (r.x + 10)) / (r.w - 20) * 300.0f));
        }
        if (!touching && touch_was) {
            if (touch_btn >= 0 && in_rect(r_btn[touch_btn], tx, ty)) game_press_button(touch_btn);
            if (touch_shim && in_rect(touch_shim == 1 ? r_shim_title : r_shim_pause, tx, ty) &&
                (touch_shim == 1 ? v->state == GS_TITLE && !v->mode_selection && !v->changelog_open : v->state == GS_PLAYING && v->paused)) {
                shimmer = !shimmer; save_records();
            }
            touch_btn = -1; slider_drag = 0; touch_shim = 0;
        }
        if (!touching) touch_on_ui = 0;
        touch_was = touching;
        /* keep reporting the last touched point after release: game.c's tap test compares the
           release position with the press position, so zeros here broke tap-to-shoot everywhere
           except right at the screen centre */
        in.mouse_down = touching && !touch_on_ui;
        in.mouse_x = (tx - BOT_W / 2) * 1.5f;
        in.mouse_y = (BOT_H / 2 - ty) * 1.5f;
        if (suspended) { suspended = 0; if (v->state == GS_PLAYING && !v->paused) game_toggle_pause(); }

#ifdef AUTOTEST
        /* screenshot build: start a game automatically after 5 s */
        autotest_t += 1.0 / 60;
        if (autotest_t > 5.0 && autotest_t < 5.1 && on_title) { in.space = 1; in.confirm_pressed = 0; }
        if (autotest_t > 9.0 && autotest_t < 9.05) in.space = 1;
#else
        (void)autotest_t;
#endif
        u64 now = svcGetSystemTick();
        double dt = (double)(now - last) / SYSCLOCK_ARM11;
        last = now;
        game_update(dt, &in);
        if (game_view()->records_dirty) { save_records(); ((GameView *)game_view())->records_dirty = 0; }

        nitems = game_draw_list(items, MAX_DRAW);
        float slider = osGet3DSliderState();
        int stereo = slider > 0.01f;
        gfxSet3D(stereo);
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top_l, C2D_Color32(0, 0, 0, 255));
        C2D_SceneBegin(top_l);
        draw_top(stereo ? -MAX_EYE_SHIFT * slider : 0, 0);
        if (stereo) {
            C2D_TargetClear(top_r, C2D_Color32(0, 0, 0, 255));
            C2D_SceneBegin(top_r);
            draw_top(MAX_EYE_SHIFT * slider, shimmer ? SHIMMER_HUE : 0);
        }
        C2D_TargetClear(bot, COL_BG);
        C2D_SceneBegin(bot);
        draw_bottom();
        C3D_FrameEnd(0);
    }
    save_records();
    if (have_ndsp) {
        aud_quit = 1; LightEvent_Signal(&aud_event);
        if (th) { threadJoin(th, U64_MAX); threadFree(th); }
        ndspExit();
    }
    aptUnhook(&apt_cookie);
    C2D_Fini(); C3D_Fini();
    romfsExit();
    gfxExit();
    return 0;
}
