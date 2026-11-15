#include "image.h"
#include <stdlib.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

void *plat_read_file(const char *name, long *size);

int image_load(const char *name, Image *out) {
    long size;
    unsigned char *d = plat_read_file(name, &size);
    if (!d) return 0;
    int n;
    out->px = stbi_load_from_memory(d, (int)size, &out->w, &out->h, &n, 4);
    free(d);
    return out->px != NULL;
}

void image_half(const Image *s, Image *d) {
    d->w = s->w > 1 ? s->w / 2 : 1;
    d->h = s->h > 1 ? s->h / 2 : 1;
    d->px = malloc((size_t)d->w * d->h * 4);
    for (int y = 0; y < d->h; y++) {
        int y0 = y * 2 < s->h ? y * 2 : s->h - 1, y1 = y * 2 + 1 < s->h ? y * 2 + 1 : s->h - 1;
        for (int x = 0; x < d->w; x++) {
            int x0 = x * 2 < s->w ? x * 2 : s->w - 1, x1 = x * 2 + 1 < s->w ? x * 2 + 1 : s->w - 1;
            const uint8_t *p[4] = { s->px + ((size_t)y0 * s->w + x0) * 4, s->px + ((size_t)y0 * s->w + x1) * 4,
                                    s->px + ((size_t)y1 * s->w + x0) * 4, s->px + ((size_t)y1 * s->w + x1) * 4 };
            unsigned sa = p[0][3] + p[1][3] + p[2][3] + p[3][3];
            uint8_t *o = d->px + ((size_t)y * d->w + x) * 4;
            for (int c = 0; c < 3; c++) {
                if (sa) o[c] = (uint8_t)((p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] + p[3][c] * p[3][3] + sa / 2) / sa);
                else o[c] = (uint8_t)((p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4);  /* keeps the bleed */
            }
            o[3] = (uint8_t)((sa + 2) / 4);
        }
    }
}

void image_free(Image *im) { free(im->px); im->px = NULL; }
