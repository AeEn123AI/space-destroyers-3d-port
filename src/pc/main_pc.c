/* Space Destroyers 3D - PC backend (SDL2 + OpenGL 2.1). */
#include <SDL.h>
#include <SDL_opengl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "audio.h"
#include "image.h"
#include "font.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/* ------------------------------------------------------------------ GL entry points */
#define GLFUNCS \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1FPROC, glUniform1f) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) X(PFNGLUNIFORM1IPROC, glUniform1i) \
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)
#define X(t, n) static t p_##n;
GLFUNCS
#undef X

static const char *VS =
    "#version 120\n"
    "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col;\n"
    "uniform vec2 u_screen; varying vec2 v_uv; varying vec4 v_col;\n"
    "void main(){ v_uv=a_uv; v_col=a_col; gl_Position=vec4(a_pos.x/u_screen.x*2.0-1.0, 1.0-a_pos.y/u_screen.y*2.0, 0.0, 1.0); }\n";
/* Scratch's colour effect: unpremultiplied RGB -> HSV, min lightness/saturation rules, hue rotate */
static const char *FS =
    "#version 120\n"
    "uniform sampler2D u_tex; uniform float u_hue; uniform float u_mode;\n"
    "varying vec2 v_uv; varying vec4 v_col;\n"
    "vec3 rgb2hsv(vec3 c){ vec4 K=vec4(0.0,-1.0/3.0,2.0/3.0,-1.0); vec4 p=c.g<c.b?vec4(c.bg,K.wz):vec4(c.gb,K.xy);\n"
    "  vec4 q=c.r<p.x?vec4(p.xyw,c.r):vec4(c.r,p.yzx); float d=q.x-min(q.w,q.y); float e=1.0e-10;\n"
    "  return vec3(abs(q.z+(q.w-q.y)/(6.0*d+e)), d/(q.x+e), q.x); }\n"
    "vec3 hsv2rgb(vec3 c){ vec4 K=vec4(1.0,2.0/3.0,1.0/3.0,3.0); vec3 p=abs(fract(c.xxx+K.xyz)*6.0-K.www);\n"
    "  return c.z*mix(K.xxx, clamp(p-K.xxx,0.0,1.0), c.y); }\n"
    "void main(){ vec4 t;\n"
    "  if (u_mode < 0.5) t = texture2D(u_tex, v_uv);\n"
    "  else if (u_mode < 1.5) t = vec4(1.0,1.0,1.0, texture2D(u_tex, v_uv).a);\n"
    "  else t = vec4(1.0);\n"
    "  if (u_hue != 0.0) { vec3 hsv = rgb2hsv(t.rgb);\n"
    "    if (hsv.z < 0.055) hsv = vec3(0.0,1.0,0.055); else if (hsv.y < 0.09) hsv = vec3(0.0,0.09,hsv.z);\n"
    "    hsv.x = mod(hsv.x + u_hue, 1.0); t.rgb = hsv2rgb(hsv); }\n"
    "  gl_FragColor = t * v_col; }\n";

static GLuint prog;
static GLint u_screen, u_hue, u_mode;

/* ------------------------------------------------------------------ batching */
typedef struct { float x, y, u, v, r, g, b, a; } Vtx;
#define MAXV 6 * 2048
static Vtx vbuf[MAXV];
static int nv;
static GLuint cur_tex; static float cur_hue, cur_mode;

static void flush(void) {
    if (!nv) return;
    glBindTexture(GL_TEXTURE_2D, cur_tex);
    p_glUniform1f(u_hue, cur_hue);
    p_glUniform1f(u_mode, cur_mode);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vbuf[0].x);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vbuf[0].u);
    p_glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vbuf[0].r);
    glDrawArrays(GL_TRIANGLES, 0, nv);
    nv = 0;
}
static void quad(GLuint tex, float mode, float hue, float x0, float y0, float x1, float y1,
                 float u0, float v0, float u1, float v1, float r, float g, float b, float a) {
    if (tex != cur_tex || hue != cur_hue || mode != cur_mode || nv + 6 > MAXV) { flush(); cur_tex = tex; cur_hue = hue; cur_mode = mode; }
    Vtx q[4] = { { x0, y0, u0, v0, r, g, b, a }, { x1, y0, u1, v0, r, g, b, a }, { x1, y1, u1, v1, r, g, b, a }, { x0, y1, u0, v1, r, g, b, a } };
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (int i = 0; i < 6; i++) vbuf[nv++] = q[idx[i]];
}

/* ------------------------------------------------------------------ assets */
static char asset_dir[1024];
void *plat_read_file(const char *name, long *size) {
    char path[1200];
    snprintf(path, sizeof path, "%s%s", asset_dir, name);
    SDL_RWops *rw = SDL_RWFromFile(path, "rb");
    if (!rw) { fprintf(stderr, "missing asset %s\n", path); return NULL; }
    Sint64 n = SDL_RWsize(rw);
    void *d = malloc((size_t)n + 1);
    SDL_RWread(rw, d, 1, (size_t)n);
    SDL_RWclose(rw);
    ((char *)d)[n] = 0;
    *size = (long)n;
    return d;
}
static SDL_mutex *audio_mutex;
void plat_audio_lock(void) { SDL_LockMutex(audio_mutex); }
void plat_audio_unlock(void) { SDL_UnlockMutex(audio_mutex); }

static GLuint tex[CO_COUNT];
static int tex_w[CO_COUNT], tex_h[CO_COUNT];
static GLuint white_tex, disc_tex;

static GLuint upload_mipmapped(Image *im) {
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    Image cur = *im; int lvl = 0;
    for (;;) {
        glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, cur.w, cur.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, cur.px);
        if (cur.w == 1 && cur.h == 1) break;
        Image nx; image_half(&cur, &nx);
        if (cur.px != im->px) image_free(&cur);
        cur = nx; lvl++;
    }
    if (cur.px != im->px) image_free(&cur);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lvl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static int load_textures(void) {
    for (int i = 0; i < CO_COUNT; i++) {
        char name[96];
        snprintf(name, sizeof name, costume_info[i].hues ? "%s_h0.png" : "%s.png", costume_info[i].file);
        Image im;
        if (!image_load(name, &im)) { fprintf(stderr, "failed to load %s\n", name); return 0; }
        tex_w[i] = im.w; tex_h[i] = im.h;
        tex[i] = upload_mipmapped(&im);
        image_free(&im);
    }
    /* white texel + anti-aliased disc (for rounded monitor boxes) */
    uint8_t w4[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &white_tex); glBindTexture(GL_TEXTURE_2D, white_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, w4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    Image d; d.w = d.h = 128; d.px = malloc(128 * 128 * 4);
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) {
        float dx = x + 0.5f - 64, dy = y + 0.5f - 64, a = 64 - sqrtf(dx * dx + dy * dy) + 0.5f;
        a = a < 0 ? 0 : (a > 1 ? 1 : a);
        uint8_t *p = d.px + (y * 128 + x) * 4; p[0] = p[1] = p[2] = 255; p[3] = (uint8_t)(a * 255);
    }
    disc_tex = upload_mipmapped(&d); image_free(&d);
    return 1;
}

/* ------------------------------------------------------------------ view transform */
static int win_w = 1280, win_h = 720;
static float vscale;   /* window px per stage unit */
static void stage_to_px(float sx, float sy, float *px, float *py) { *px = win_w * 0.5f + sx * vscale; *py = win_h * 0.5f - sy * vscale; }

static void draw_costume(int co, float x, float y, float size, float alpha, float hue) {
    const CostumeInfo *ci = &costume_info[co];
    float s = size * vscale;
    float left = x + (ci->pc_ox - ci->rcx) * size, top = y + (ci->rcy - ci->pc_oy) * size;
    float px0, py0; stage_to_px(left, top, &px0, &py0);
    float w = tex_w[co] / ci->pc_scale * s, h = tex_h[co] / ci->pc_scale * s;
    if (px0 > win_w || py0 > win_h || px0 + w < 0 || py0 + h < 0) return;   /* off-window */
    float hs = hue / 200.0f; hs -= floorf(hs);
    quad(tex[co], 0, hs, px0, py0, px0 + w, py0 + h, 0, 0, 1, 1, 1, 1, 1, alpha);
}

/* ------------------------------------------------------------------ monitors (Scratch style) */
static void *ttf_regular, *ttf_bold, *ttf_dejavu;
static Font f_label, f_value;
static GLuint t_label, t_value;
static float font_scale = -1;

static GLuint upload_alpha(const Font *f) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, f->aw, f->ah, 0, GL_ALPHA, GL_UNSIGNED_BYTE, f->atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}
static void ensure_fonts(float s) {
    if (fabsf(s - font_scale) < 0.01f) return;
    if (font_scale > 0) { font_free(&f_label); font_free(&f_value); glDeleteTextures(1, &t_label); glDeleteTextures(1, &t_value); }
    font_scale = s;
    font_bake(&f_label, ttf_bold, ttf_dejavu, 12 * 1.15f * s);
    font_bake(&f_value, ttf_regular, NULL, 12 * 1.15f * s);
    t_label = upload_alpha(&f_label); t_value = upload_alpha(&f_value);
}
typedef struct { GLuint t; float r, g, b, a; } TextCtx;
static void text_quad(void *ud, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
    TextCtx *c = ud;
    quad(c->t, 1, 0, x0, y0, x1, y1, s0, t0, s1, t1, c->r, c->g, c->b, c->a);
}
static void rrect(float x0, float y0, float x1, float y1, float rad, uint32_t rgba) {
    float r = (rgba >> 24 & 255) / 255.f, g = (rgba >> 16 & 255) / 255.f, b = (rgba >> 8 & 255) / 255.f, a = (rgba & 255) / 255.f;
    if (rad * 2 > x1 - x0) rad = (x1 - x0) / 2;
    if (rad * 2 > y1 - y0) rad = (y1 - y0) / 2;
    float xs[4] = { x0, x0 + rad, x1 - rad, x1 }, ys[4] = { y0, y0 + rad, y1 - rad, y1 };
    float us[4] = { 0, 0.5f, 0.5f, 1 };
    for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) {
        if (i == 1 || j == 1) quad(white_tex, 0, 0, xs[i], ys[j], xs[i + 1], ys[j + 1], 0, 0, 1, 1, r, g, b, a);
        else quad(disc_tex, 0, 0, xs[i], ys[j], xs[i + 1], ys[j + 1], us[i], us[j], us[i + 1], us[j + 1], r, g, b, a);
    }
}
static float mon_x_px(float x, float w) {
    (void)w;
    if (x < 160) return x * vscale;
    if (x > 320) return win_w - (480 - x) * vscale;
    return win_w * 0.5f + (x - 240) * vscale;
}
static float mon_y_px(float y) {
    float stage_h = 360 * vscale;
    if (win_h - stage_h < 1) return y * vscale;
    if (y < 120) return y * vscale;
    if (y > 240) return win_h - (360 - y) * vscale;
    return win_h * 0.5f + (y - 180) * vscale;
}

/* slider drag state */
static int slider_drag;
static float slider_x0, slider_x1, slider_y0, slider_y1;

static void draw_monitors(void) {
    float s = vscale;
    ensure_fonts(s);
    Monitor *m = game_monitors();
    slider_x1 = -1;
    for (int i = 0; i < MON_COUNT; i++) {
        if (!m[i].visible) continue;
        float lw = font_width(&f_label, m[i].label), vw = font_width(&f_value, m[i].value);
        float pad = 3 * s, line = 12 * 1.15f * s * 1.2f, vmin = 40 * s;
        float valw = fmaxf(vmin, vw + 4 * s + 2 * s);
        float w = 2 * s + 2 * pad + 5 * s + lw + 5 * s + 5 * s + valw + 5 * s;
        float h = 2 * s + 2 * pad + line;
        int slider = i == MON_STARS;
        if (slider) h += 20 * s;
        float x = mon_x_px(m[i].x, w), y = mon_y_px(m[i].y);
        if (x + w > win_w) x = win_w - w;
        rrect(x, y, x + w, y + h, 4 * s, 0x00000026);
        rrect(x + s, y + s, x + w - s, y + h - s, 3 * s, 0xe6f0ffff);
        float base = y + s + pad + line * 0.5f + (f_label.ascent + f_label.descent) * 0.5f;
        TextCtx lc = { t_label, 0x57 / 255.f, 0x5e / 255.f, 0x75 / 255.f, 1 };
        font_draw(&f_label, m[i].label, x + s + pad + 5 * s, base, text_quad, &lc);
        float vx = x + s + pad + 5 * s + lw + 10 * s;
        rrect(vx, y + s + pad, vx + valw, y + s + pad + line, 4 * s, 0xff8c1aff);
        TextCtx vc = { t_value, 1, 1, 1, 1 };
        font_draw(&f_value, m[i].value, vx + (valw - vw) * 0.5f, base, text_quad, &vc);
        if (slider) {
            float sy = y + s + pad + line + 10 * s;
            float sx0 = x + s + pad + 8 * s, sx1 = x + w - s - pad - 8 * s;
            rrect(sx0, sy - 2 * s, sx1, sy + 2 * s, 2 * s, 0xc6cbd6ff);
            float t = game_view()->stars_slider / 300.0f;
            rrect(sx0, sy - 2 * s, sx0 + (sx1 - sx0) * t, sy + 2 * s, 2 * s, 0x4c97ffff);
            float kx = sx0 + (sx1 - sx0) * t;
            rrect(kx - 7 * s, sy - 7 * s, kx + 7 * s, sy + 7 * s, 7 * s, 0xffffffff);
            rrect(kx - 6 * s, sy - 6 * s, kx + 6 * s, sy + 6 * s, 6 * s, 0x4c97ffff);
            slider_x0 = sx0; slider_x1 = sx1; slider_y0 = sy - 10 * s; slider_y1 = sy + 10 * s;
        }
    }
}

/* ------------------------------------------------------------------ frame */
static DrawItem items[MAX_DRAW];
static void render(void) {
    glViewport(0, 0, win_w, win_h);
    const GameView *v = game_view();
    glClearColor(v->bg_r, v->bg_g, v->bg_b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    p_glUseProgram(prog);
    p_glUniform2f(u_screen, (float)win_w, (float)win_h);
    vscale = fminf(win_w / 480.0f, win_h / 360.0f);
    int n = game_draw_list(items, MAX_DRAW);
    for (int i = 0; i < n; i++) {
        DrawItem *d = &items[i];
        if (d->kind == DI_FILL) {
            float r = (d->rgba >> 24 & 255) / 255.f, g = (d->rgba >> 16 & 255) / 255.f, b = (d->rgba >> 8 & 255) / 255.f;
            quad(white_tex, 0, 0, 0, 0, (float)win_w, (float)win_h, 0, 0, 1, 1, r, g, b, d->alpha);
        } else draw_costume(d->costume, d->x, d->y, d->size, d->alpha, d->hue);
    }
    draw_monitors();
    flush();
}

/* ------------------------------------------------------------------ records */
static char save_path[1024];
static void load_records(double *t, double *s, int *pm) {
    *t = 687.19; *s = 1348; *pm = 1;
    FILE *f = fopen(save_path, "r");
    if (!f) return;
    double a, b; int c;
    if (fscanf(f, "%lf %lf %d", &a, &b, &c) == 3) { if (a > *t) *t = a; if (b > *s) *s = b; *pm = c; }
    fclose(f);
}
static void save_records(void) {
    const GameView *v = game_view();
    FILE *f = fopen(save_path, "w");
    if (!f) return;
    fprintf(f, "%.3f %.0f %d\n", v->wr_timer, v->wr_score, v->player_music);
    fclose(f);
}

/* ------------------------------------------------------------------ test script (headless testing) */
typedef struct { double t; char act[16]; char arg[256]; } ScriptStep;
static ScriptStep script[128];
static int nscript, iscript;
static int script_keys[SDL_NUM_SCANCODES];
static int script_mouse_down = -1; static float script_mx, script_my; static int script_mouse_set;
static void parse_script(const char *s) {
    while (s && *s && nscript < 128) {
        ScriptStep *st = &script[nscript];
        const char *e = strchr(s, ';'); size_t len = e ? (size_t)(e - s) : strlen(s);
        char buf[300]; if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, s, len); buf[len] = 0;
        char *c1 = strchr(buf, ':'); if (!c1) break;
        *c1 = 0; st->t = atof(buf);
        char *eq = strchr(c1 + 1, '=');
        if (eq) { *eq = 0; snprintf(st->arg, sizeof st->arg, "%s", eq + 1); }
        snprintf(st->act, sizeof st->act, "%s", c1 + 1);
        nscript++;
        s = e ? e + 1 : NULL;
    }
}
static int want_shot; static char shot_path[256]; static int want_quit;
static void run_script(double t) {
    while (iscript < nscript && script[iscript].t <= t) {
        ScriptStep *st = &script[iscript++];
        if (!strcmp(st->act, "down") || !strcmp(st->act, "up")) {
            SDL_Scancode sc = SDL_GetScancodeFromName(st->arg);
            script_keys[sc] = !strcmp(st->act, "down") ? 1 : 0;
            if (script_keys[sc]) { SDL_Event e; memset(&e, 0, sizeof e); e.type = SDL_KEYDOWN; e.key.keysym.scancode = sc; e.key.keysym.sym = SDL_GetKeyFromScancode(sc); SDL_PushEvent(&e); }
        } else if (!strcmp(st->act, "move")) { sscanf(st->arg, "%f,%f", &script_mx, &script_my); script_mouse_set = 1; }
        else if (!strcmp(st->act, "mdown")) script_mouse_down = 1;
        else if (!strcmp(st->act, "mup")) script_mouse_down = 0;
        else if (!strcmp(st->act, "shot")) { want_shot = 1; snprintf(shot_path, sizeof shot_path, "%s", st->arg); }
        else if (!strcmp(st->act, "size")) { int w, h; if (sscanf(st->arg, "%dx%d", &w, &h) == 2) { extern SDL_Window *g_win; SDL_SetWindowSize(g_win, w, h); } }
        else if (!strcmp(st->act, "quit")) want_quit = 1;
    }
}
static void screenshot(const char *path) {
    uint8_t *px = malloc((size_t)win_w * win_h * 4);
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < win_h / 2; y++)
        for (int x = 0; x < win_w * 4; x++) { uint8_t t = px[y * win_w * 4 + x]; px[y * win_w * 4 + x] = px[(win_h - 1 - y) * win_w * 4 + x]; px[(win_h - 1 - y) * win_w * 4 + x] = t; }
    for (int i = 0; i < win_w * win_h; i++) px[i * 4 + 3] = 255;
    stbi_write_png(path, win_w, win_h, 4, px, win_w * 4);
    free(px);
    fprintf(stderr, "screenshot %s\n", path);
}

/* ------------------------------------------------------------------ main */
SDL_Window *g_win;
static void audio_cb(void *ud, Uint8 *stream, int len) { (void)ud; audio_mix((int16_t *)stream, len / 4); }

static GLuint compile(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type);
    p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint ok; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader: %s\n", log); }
    return s;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
    }
    const char *ad = getenv("SD3D_ASSETS");
    if (ad) snprintf(asset_dir, sizeof asset_dir, "%s/", ad);
    else { char *bp = SDL_GetBasePath(); snprintf(asset_dir, sizeof asset_dir, "%sassets/", bp ? bp : "./"); SDL_free(bp); }
    char *pp = SDL_GetPrefPath("AeEn123", "Space Destroyers 3D");
    snprintf(save_path, sizeof save_path, "%srecords.txt", pp ? pp : "./");
    SDL_free(pp);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const char *ws = getenv("SD3D_WINDOW");
    if (ws) sscanf(ws, "%dx%d", &win_w, &win_h);
    g_win = SDL_CreateWindow("Space Destroyers 3D", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(g_win);
    if (!ctx) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
#define X(t, n) p_##n = (t)SDL_GL_GetProcAddress(#n); if (!p_##n) { fprintf(stderr, "missing GL %s\n", #n); return 1; }
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram)
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram)
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLUSEPROGRAMPROC, glUseProgram)
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) X(PFNGLUNIFORM1FPROC, glUniform1f)
    X(PFNGLUNIFORM2FPROC, glUniform2f) X(PFNGLUNIFORM1IPROC, glUniform1i)
    X(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation) X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)
#undef X
    int vsync = SDL_GL_SetSwapInterval(1) == 0;
    prog = p_glCreateProgram();
    p_glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    p_glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    p_glBindAttribLocation(prog, 0, "a_pos"); p_glBindAttribLocation(prog, 1, "a_uv"); p_glBindAttribLocation(prog, 2, "a_col");
    p_glLinkProgram(prog);
    p_glUseProgram(prog);
    u_screen = p_glGetUniformLocation(prog, "u_screen"); u_hue = p_glGetUniformLocation(prog, "u_hue"); u_mode = p_glGetUniformLocation(prog, "u_mode");
    p_glUniform1i(p_glGetUniformLocation(prog, "u_tex"), 0);
    p_glEnableVertexAttribArray(0); p_glEnableVertexAttribArray(1); p_glEnableVertexAttribArray(2);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    if (!load_textures()) return 1;
    long sz;
    ttf_regular = plat_read_file("LiberationSans-Regular.ttf", &sz);
    ttf_bold = plat_read_file("LiberationSans-Bold.ttf", &sz);
    ttf_dejavu = plat_read_file("DejaVuSans.ttf", &sz);
    if (!ttf_regular || !ttf_bold || !ttf_dejavu) return 1;

    audio_mutex = SDL_CreateMutex();
    int have_audio = audio_init();
    if (!have_audio) fprintf(stderr, "audio assets missing - running silent\n");
    SDL_AudioSpec want, got; memset(&want, 0, sizeof want);
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = audio_cb;
    SDL_AudioDeviceID dev = have_audio ? SDL_OpenAudioDevice(NULL, 0, &want, &got, 0) : 0;
    if (dev) SDL_PauseAudioDevice(dev, 0);
    else fprintf(stderr, "no audio device: %s\n", SDL_GetError());

    double wr_t, wr_s; int pm;
    load_records(&wr_t, &wr_s, &pm);
    const char *seed = getenv("SD3D_SEED");
    game_init(seed ? (uint32_t)strtoul(seed, NULL, 0) : (uint32_t)SDL_GetPerformanceCounter(), wr_t, wr_s, pm);
    if (getenv("SD3D_SCRIPT")) parse_script(getenv("SD3D_SCRIPT"));
    double fixed_dt = getenv("SD3D_FIXED_DT") ? atof(getenv("SD3D_FIXED_DT")) : 0;

    SDL_GameController *pad = NULL;
    Uint64 freq = SDL_GetPerformanceFrequency(), last = SDL_GetPerformanceCounter();
    double clock = 0;
    int running = 1, fullscreen = 0;
    float last_mx = 0, last_my = 0;
    int pad_prev[SDL_CONTROLLER_BUTTON_MAX] = { 0 };
    int title_ss_axis_prev = 0;
    while (running) {
        GameInput in; memset(&in, 0, sizeof in);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = 0; break;
            case SDL_CONTROLLERDEVICEADDED: if (!pad) pad = SDL_GameControllerOpen(e.cdevice.which); break;
            case SDL_CONTROLLERDEVICEREMOVED: if (pad) { SDL_GameControllerClose(pad); pad = NULL; } break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(g_win, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    break;
                case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER:
                    in.pause_pressed = 1; in.confirm_pressed = 1; break;
                case SDL_SCANCODE_P: in.pause_pressed = 1; break;
                case SDL_SCANCODE_1: in.music_key_pressed = 1; break;
                case SDL_SCANCODE_ESCAPE:
                    if (fullscreen) { fullscreen = 0; SDL_SetWindowFullscreen(g_win, 0); }
                    else { in.back_pressed = 1; in.quit_pressed = 1; }
                    break;
                case SDL_SCANCODE_UP: in.egg_up_pressed = 1; in.nav_up = 1; break;
                case SDL_SCANCODE_DOWN: in.nav_down = 1; break;
                case SDL_SCANCODE_LEFT: in.nav_left = 1; break;
                case SDL_SCANCODE_RIGHT: in.nav_right = 1; break;
                case SDL_SCANCODE_Q: in.cheat_q_pressed = 1; break;
                default: break;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (!pad) pad = SDL_GameControllerFromInstanceID(e.cbutton.which);
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_START: in.pause_pressed = 1; break;
                case SDL_CONTROLLER_BUTTON_BACK: in.quit_pressed = 1; break;
                case SDL_CONTROLLER_BUTTON_A: in.confirm_pressed = 1; break;
                case SDL_CONTROLLER_BUTTON_B: in.back_pressed = 1; break;
                case SDL_CONTROLLER_BUTTON_Y: in.music_key_pressed = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_UP: in.nav_up = 1; in.egg_up_pressed = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: in.nav_down = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: in.nav_left = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: in.nav_right = 1; break;
                default: break;
                }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) SDL_GL_GetDrawableSize(g_win, &win_w, &win_h);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT && slider_x1 > 0) {
                    int dw, dh; SDL_GetWindowSize(g_win, &dw, &dh);
                    float mx = e.button.x * (float)win_w / dw, my = e.button.y * (float)win_h / dh;
                    if (mx >= slider_x0 - 8 && mx <= slider_x1 + 8 && my >= slider_y0 && my <= slider_y1) slider_drag = 1;
                }
                break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) slider_drag = 0; break;
            }
        }
        Uint64 now = SDL_GetPerformanceCounter();
        double dt = (double)(now - last) / freq; last = now;
        if (fixed_dt > 0) dt = fixed_dt;
        clock += dt;
        run_script(clock);
        if (want_quit) running = 0;

        const Uint8 *k = SDL_GetKeyboardState(NULL);
#define KEY(sc) (k[sc] || script_keys[sc])
        in.left = KEY(SDL_SCANCODE_A) || KEY(SDL_SCANCODE_LEFT);
        in.right = KEY(SDL_SCANCODE_D) || KEY(SDL_SCANCODE_RIGHT);
        in.up = KEY(SDL_SCANCODE_W) || KEY(SDL_SCANCODE_UP);
        in.down = KEY(SDL_SCANCODE_S) || KEY(SDL_SCANCODE_DOWN);
        in.space = KEY(SDL_SCANCODE_SPACE);
        in.speed_up = KEY(SDL_SCANCODE_E); in.speed_down = KEY(SDL_SCANCODE_Q);
        in.egg_up = KEY(SDL_SCANCODE_UP); in.egg_down = KEY(SDL_SCANCODE_DOWN);
        in.egg_left = KEY(SDL_SCANCODE_LEFT); in.egg_right = KEY(SDL_SCANCODE_RIGHT);
        in.egg_b = KEY(SDL_SCANCODE_B); in.egg_a = KEY(SDL_SCANCODE_A);
        in.cheat_w = KEY(SDL_SCANCODE_W); in.cheat_e = KEY(SDL_SCANCODE_E); in.cheat_r = KEY(SDL_SCANCODE_R);
        in.cheat_t = KEY(SDL_SCANCODE_T); in.cheat_y = KEY(SDL_SCANCODE_Y);
        int numkeys; const Uint8 *ks = SDL_GetKeyboardState(&numkeys);
        for (int i = 0; i < numkeys; i++) if (ks[i] || script_keys[i]) { in.any_key = 1; break; }
        /* the title starts with Space; on the title the arrows navigate the menu instead of being ignored */
        if (pad) {
            float lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
            float ly = -SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
            float mag = sqrtf(lx * lx + ly * ly);
            if (mag > 0.2f) { float s = fminf(1.0f, (mag - 0.2f) / 0.65f) / mag; in.ax = lx * s; in.ay = ly * s; }
            int ax_dir = fabsf(lx) > fabsf(ly) ? (lx > 0.6f ? 1 : (lx < -0.6f ? 2 : 0)) : (ly > 0.6f ? 3 : (ly < -0.6f ? 4 : 0));
            if (ax_dir && ax_dir != title_ss_axis_prev) {
                if (ax_dir == 1) in.nav_right = 1; else if (ax_dir == 2) in.nav_left = 1;
                else if (ax_dir == 3) in.nav_up = 1; else in.nav_down = 1;
            }
            title_ss_axis_prev = ax_dir;
            int b[SDL_CONTROLLER_BUTTON_MAX];
            for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) b[i] = SDL_GameControllerGetButton(pad, i);
            in.left |= b[SDL_CONTROLLER_BUTTON_DPAD_LEFT]; in.right |= b[SDL_CONTROLLER_BUTTON_DPAD_RIGHT];
            in.up |= b[SDL_CONTROLLER_BUTTON_DPAD_UP]; in.down |= b[SDL_CONTROLLER_BUTTON_DPAD_DOWN];
            in.egg_up |= b[SDL_CONTROLLER_BUTTON_DPAD_UP]; in.egg_down |= b[SDL_CONTROLLER_BUTTON_DPAD_DOWN];
            in.egg_left |= b[SDL_CONTROLLER_BUTTON_DPAD_LEFT]; in.egg_right |= b[SDL_CONTROLLER_BUTTON_DPAD_RIGHT];
            in.egg_b |= b[SDL_CONTROLLER_BUTTON_B]; in.egg_a |= b[SDL_CONTROLLER_BUTTON_A];
            in.space |= b[SDL_CONTROLLER_BUTTON_A];
            in.speed_up |= b[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
            in.speed_down |= b[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] || SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
            for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++) { if (b[i]) in.any_key = 1; pad_prev[i] = b[i]; }
        }
        /* on the title, A/space confirm a highlighted button instead of starting directly */
        if (game_view()->state == GS_TITLE && game_view()->nav_index >= 0 && pad && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A))
            in.space = 0;

        int mx, my; Uint32 mb = SDL_GetMouseState(&mx, &my);
        int dw, dh; SDL_GetWindowSize(g_win, &dw, &dh);
        float fx = mx * (float)win_w / dw, fy = my * (float)win_h / dh;
        vscale = fminf(win_w / 480.0f, win_h / 360.0f);
        in.mouse_x = (fx - win_w * 0.5f) / vscale;
        in.mouse_y = (win_h * 0.5f - fy) / vscale;
        in.mouse_down = (mb & SDL_BUTTON_LMASK) != 0;
        if (script_mouse_set) { in.mouse_x = script_mx; in.mouse_y = script_my; }
        if (script_mouse_down >= 0) in.mouse_down = script_mouse_down;
        in.mouse_moved = fabsf(in.mouse_x - last_mx) > 0.5f || fabsf(in.mouse_y - last_my) > 0.5f;
        last_mx = in.mouse_x; last_my = in.mouse_y;
        in.mouse_hover_ok = 1;
        if (slider_drag) {
            if (!in.mouse_down) slider_drag = 0;
            else game_set_stars_slider((int)lrintf((fx - slider_x0) / (slider_x1 - slider_x0) * 300.0f));
            in.mouse_down = 0;
        }

        game_update(dt, &in);
        if (game_view()->records_dirty) { save_records(); ((GameView *)game_view())->records_dirty = 0; }
        render();
        if (want_shot) { want_shot = 0; screenshot(shot_path); }
        SDL_GL_SwapWindow(g_win);
        if (!vsync || fixed_dt > 0) {
            double spent = (double)(SDL_GetPerformanceCounter() - now) / freq;
            if (spent < 1.0 / 60 && fixed_dt <= 0) SDL_Delay((Uint32)((1.0 / 60 - spent) * 1000));
        }
    }
    save_records();
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
