#include "font.h"
#include <stdlib.h>
#include <string.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int font_bake(Font *f, const void *ttf, const void *fallback, float px) {
    memset(f, 0, sizeof *f);
    f->px = px;
    int side = 128;
    while (side * side < px * px * 140) side *= 2;
    for (;;) {
        f->aw = f->ah = side;
        f->atlas = calloc((size_t)side * side, 1);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, f->atlas, side, side, 0, 1, NULL);
        stbtt_PackSetOversampling(&pc, px < 40 ? 2 : 1, 1);
        int ok = stbtt_PackFontRange(&pc, ttf, 0, px, 32, 95, f->ascii);
        if (ok && fallback) { f->has_cloud = stbtt_PackFontRange(&pc, fallback, 0, px, 0x2601, 1, &f->cloud); ok = f->has_cloud; }
        stbtt_PackEnd(&pc);
        if (ok) break;
        free(f->atlas); side *= 2;
        if (side > 4096) return 0;
    }
    stbtt_fontinfo info;
    stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0));
    int a, d, g;
    stbtt_GetFontVMetrics(&info, &a, &d, &g);
    float s = stbtt_ScaleForPixelHeight(&info, px);
    f->ascent = a * s; f->descent = d * s;
    return 1;
}

static const stbtt_packedchar *glyph(const Font *f, const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned cp = s[0];
    if (cp >= 0xe0 && s[1] && s[2]) { cp = ((cp & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f); *p += 3; }
    else if (cp >= 0xc0 && s[1]) { cp = ((cp & 0x1f) << 6) | (s[1] & 0x3f); *p += 2; }
    else *p += 1;
    if (cp == 0x2601) return f->has_cloud ? &f->cloud : NULL;
    if (cp >= 32 && cp < 127) return &f->ascii[cp - 32];
    return NULL;
}

float font_width(const Font *f, const char *t) {
    float w = 0;
    while (*t) { const stbtt_packedchar *g = glyph(f, &t); if (g) w += g->xadvance; }
    return w;
}

void font_draw(const Font *f, const char *t, float x, float base, FontQuadFn fn, void *ud) {
    while (*t) {
        const stbtt_packedchar *g = glyph(f, &t);
        if (!g) continue;
        if (g->x1 > g->x0)
            fn(ud, x + g->xoff, base + g->yoff, x + g->xoff2, base + g->yoff2,
               g->x0 / (float)f->aw, g->y0 / (float)f->ah, g->x1 / (float)f->aw, g->y1 / (float)f->ah);
        x += g->xadvance;
    }
}

void font_free(Font *f) { free(f->atlas); f->atlas = NULL; }
