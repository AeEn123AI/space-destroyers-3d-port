/* PNG loading + mip generation (alpha-weighted box filter on bled base levels). */
#ifndef IMAGE_H
#define IMAGE_H
#include <stdint.h>
typedef struct { int w, h; uint8_t *px; } Image;   /* RGBA8, non-premultiplied */
int  image_load(const char *name, Image *out);     /* via plat_read_file */
void image_half(const Image *src, Image *dst);     /* next mip level (dst->px malloc'd) */
void image_free(Image *im);
#endif
