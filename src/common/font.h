/* Runtime font baking with stb_truetype (crisp at any window scale). */
#ifndef FONT_H
#define FONT_H
#include <stdint.h>
#include "stb_truetype.h"
typedef struct {
    uint8_t *atlas; int aw, ah;          /* 8-bit coverage */
    stbtt_packedchar ascii[95];
    stbtt_packedchar cloud; int has_cloud;   /* U+2601 from the fallback font */
    float px, ascent, descent;
} Font;
typedef void (*FontQuadFn)(void *ud, float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1);
int   font_bake(Font *f, const void *ttf, const void *fallback, float px);
float font_width(const Font *f, const char *utf8);
void  font_draw(const Font *f, const char *utf8, float x, float baseline, FontQuadFn fn, void *ud);
void  font_free(Font *f);
#endif
