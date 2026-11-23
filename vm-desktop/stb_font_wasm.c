/* Minimal wasm binding over stb_truetype for cross-end font consistency.
   Exported ABI (all pointers are offsets into wasm linear memory):
     int  loadFont(int fontId, const unsigned char* ttf, int len)
     int  measureText(int fontId, int fontSize, const char* utf8, int len)
     int  rasterizeText(int fontId, int fontSize, const char* utf8, int len,
                        unsigned char* outRgba, int outCap, int* outW, int* outH)
     void* alloc(int size)
     void  freeMem(void* ptr)

   rasterizeText writes top-down RGBA, white glyph with alpha from the font.
   Returns 0 on success, negative on failure.
*/
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STB_FONT_WASM_MAX_FONTS 8

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    int loaded;
} FontSlot;

static FontSlot g_fonts[STB_FONT_WASM_MAX_FONTS];

int alloc_dummy(void); /* avoid unused warning on some linkers */

void *alloc(int size) { return malloc((size_t)size); }
void freeMem(void *ptr) { free(ptr); }

int loadFont(int fontId, const unsigned char *ttf, int len) {
    if (fontId < 0 || fontId >= STB_FONT_WASM_MAX_FONTS || !ttf || len <= 0) return -1;
    FontSlot *slot = &g_fonts[fontId];
    free(slot->data);
    slot->data = NULL;
    slot->loaded = 0;
    unsigned char *copy = (unsigned char *)malloc((size_t)len);
    if (!copy) return -2;
    memcpy(copy, ttf, (size_t)len);
    /* TTC（字体集合，如 simsun.ttc）取第 0 个字体；普通 TTF 返回 0。
       与 guest 侧 vm_font.c 保持一致，两端都能直接吃系统 TTC。 */
    int offset = stbtt_GetFontOffsetForIndex(copy, 0);
    if (offset < 0) offset = 0;
    if (!stbtt_InitFont(&slot->info, copy, offset)) {
        free(copy);
        return -3;
    }
    slot->data = copy;
    slot->loaded = 1;
    return 0;
}

static int utf8_decode(const char *s, int *size) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *size = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *size = 2; return ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) { *size = 3; return ((c & 0x0F) << 12) | ((unsigned char)s[1] & 0x3F) << 6 | ((unsigned char)s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0) { *size = 4; return ((c & 0x07) << 18) | ((unsigned char)s[1] & 0x3F) << 12 | ((unsigned char)s[2] & 0x3F) << 6 | ((unsigned char)s[3] & 0x3F); }
    *size = 1;
    return c;
}

static int glyph_index(FontSlot *slot, int codepoint) {
    return stbtt_FindGlyphIndex(&slot->info, codepoint);
}

int measureText(int fontId, int fontSize, const char *utf8, int len) {
    if (fontId < 0 || fontId >= STB_FONT_WASM_MAX_FONTS) return 0;
    FontSlot *slot = &g_fonts[fontId];
    if (!slot->loaded || !utf8 || len <= 0 || fontSize <= 0) return 0;

    float scale = stbtt_ScaleForPixelHeight(&slot->info, (float)fontSize);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&slot->info, &ascent, &descent, &lineGap);
    int lineHeight = (int)((ascent - descent + lineGap) * scale);
    if (lineHeight <= 0) lineHeight = fontSize;

    int x = 0, maxX = 0, pos = 0;
    int prev = 0;
    while (pos < len) {
        int sz = 0;
        int cp = utf8_decode(utf8 + pos, &sz);
        if (sz <= 0) break;
        pos += sz;
        if (cp == '\n') { x = 0; prev = 0; continue; }
        int g = glyph_index(slot, cp);
        if (g <= 0) g = 0;
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&slot->info, g, &advance, &lsb);
        if (prev) {
            int kern = stbtt_GetGlyphKernAdvance(&slot->info, prev, g);
            x += (int)(kern * scale);
        }
        x += (int)(advance * scale);
        if (x > maxX) maxX = x;
        prev = g;
    }
    (void)lineHeight;
    return maxX;
}

int rasterizeText(int fontId, int fontSize, const char *utf8, int len,
                  unsigned char *outRgba, int outCap, int *outW, int *outH) {
    if (fontId < 0 || fontId >= STB_FONT_WASM_MAX_FONTS) return -1;
    FontSlot *slot = &g_fonts[fontId];
    if (!slot->loaded || !utf8 || len <= 0 || fontSize <= 0 || !outRgba || !outW || !outH) return -2;

    float scale = stbtt_ScaleForPixelHeight(&slot->info, (float)fontSize);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&slot->info, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scale);
    float lineStep = (float)(ascent - descent + lineGap) * scale;
    if (lineStep <= 0) lineStep = (float)fontSize;

    /* First pass: total width and height. */
    int width = 0, pos = 0, prev = 0;
    float cursorX = 0;
    int lines = 1;
    while (pos < len) {
        int sz = 0;
        int cp = utf8_decode(utf8 + pos, &sz);
        if (sz <= 0) break;
        pos += sz;
        if (cp == '\n') {
            if (cursorX > width) width = (int)cursorX;
            cursorX = 0;
            prev = 0;
            lines++;
            continue;
        }
        int g = glyph_index(slot, cp);
        if (g <= 0) g = 0;
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&slot->info, g, &advance, &lsb);
        if (prev) cursorX += stbtt_GetGlyphKernAdvance(&slot->info, prev, g) * scale;
        cursorX += advance * scale;
        prev = g;
    }
    if (cursorX > width) width = (int)cursorX;
    int height = (int)(lineStep * lines);
    if (width <= 0 || height <= 0) return -3;
    if (width * height * 4 > outCap) return -4;

    memset(outRgba, 0, (size_t)width * height * 4);

    /* Second pass: blit glyph alpha into RGBA (white glyph, alpha from font). */
    pos = 0;
    prev = 0;
    int line = 0;
    cursorX = 0;
    while (pos < len) {
        int sz = 0;
        int cp = utf8_decode(utf8 + pos, &sz);
        if (sz <= 0) break;
        pos += sz;
        if (cp == '\n') {
            cursorX = 0;
            prev = 0;
            line++;
            continue;
        }
        int g = glyph_index(slot, cp);
        if (g <= 0) g = 0;
        int advance, lsb;
        stbtt_GetGlyphHMetrics(&slot->info, g, &advance, &lsb);
        if (prev) cursorX += stbtt_GetGlyphKernAdvance(&slot->info, prev, g) * scale;

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&slot->info, g, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *bitmap = (unsigned char *)malloc((size_t)gw * gh);
            if (bitmap) {
                stbtt_MakeGlyphBitmap(&slot->info, bitmap, gw, gh, gw, scale, scale, g);
                int dstX = (int)cursorX + x0;
                int dstY = baseline + line * (int)lineStep + y0;
                for (int gy = 0; gy < gh; gy++) {
                    int py = dstY + gy;
                    if (py < 0 || py >= height) continue;
                    for (int gx = 0; gx < gw; gx++) {
                        int px = dstX + gx;
                        if (px < 0 || px >= width) continue;
                        unsigned char a = bitmap[gy * gw + gx];
                        if (!a) continue;
                        unsigned char *dst = outRgba + ((size_t)py * width + px) * 4;
                        dst[0] = 255;
                        dst[1] = 255;
                        dst[2] = 255;
                        dst[3] = a;
                    }
                }
                free(bitmap);
            }
        }
        cursorX += advance * scale;
        prev = g;
    }

    *outW = width;
    *outH = height;
    return 0;
}
