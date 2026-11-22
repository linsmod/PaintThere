#include "vm_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/* 本文件是 native 侧唯一定义 STB_TRUETYPE_IMPLEMENTATION 的编译单元。
   stb_font_wasm.c 只进 emcc 构建，两者不进同一二进制。 */

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    int loaded;
} VmFontSlot;

static VmFontSlot g_fonts[VM_FONT_MAX_SLOTS];

static unsigned char *vm_font_read_file(const char *path, long *outSize) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    unsigned char *data = (unsigned char *)malloc((size_t)n);
    if (!data) { fclose(f); return NULL; }
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(data); return NULL; }
    *outSize = n;
    return data;
}

int VmFont_LoadFile(const char *ttfPath, int fontId) {
    if (!ttfPath || fontId < 0 || fontId >= VM_FONT_MAX_SLOTS) return -1;
    long size = 0;
    unsigned char *data = vm_font_read_file(ttfPath, &size);
    if (!data) return -2;

    VmFontSlot *slot = &g_fonts[fontId];
    if (!stbtt_InitFont(&slot->info, data, 0)) {
        free(data);
        return -3;
    }
    free(slot->data);
    slot->data = data;
    slot->loaded = 1;
    return 0;
}

int VmFont_IsLoaded(int fontId) {
    if (fontId < 0 || fontId >= VM_FONT_MAX_SLOTS) return 0;
    return g_fonts[fontId].loaded;
}

void VmFont_Unload(int fontId) {
    if (fontId < 0 || fontId >= VM_FONT_MAX_SLOTS) return;
    free(g_fonts[fontId].data);
    g_fonts[fontId].data = NULL;
    g_fonts[fontId].loaded = 0;
}

/* 与 stb_font_wasm.c 的 utf8_decode 完全一致（含非法序列的退化处理）。 */
static int vm_font_utf8_decode(const char *s, int *size) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *size = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *size = 2; return ((c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) { *size = 3; return ((c & 0x0F) << 12) | (((unsigned char)s[1] & 0x3F) << 6) | ((unsigned char)s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0) { *size = 4; return ((c & 0x07) << 18) | (((unsigned char)s[1] & 0x3F) << 12) | (((unsigned char)s[2] & 0x3F) << 6) | ((unsigned char)s[3] & 0x3F); }
    *size = 1;
    return c;
}

static int vm_font_glyph(const VmFontSlot *slot, int codepoint) {
    int g = stbtt_FindGlyphIndex(&slot->info, codepoint);
    return g > 0 ? g : 0;
}

/* 与 stb_font_wasm.c 的 rasterizeText 首段一致：scale / baseline / lineStep。 */
static void vm_font_scale(const VmFontSlot *slot, int px,
                          float *scale, int *baseline, float *lineStep) {
    *scale = stbtt_ScaleForPixelHeight(&slot->info, (float)px);
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&slot->info, &ascent, &descent, &lineGap);
    *baseline = (int)((float)ascent * (*scale));
    float step = (float)(ascent - descent + lineGap) * (*scale);
    if (step <= 0) step = (float)px;
    *lineStep = step;
}

VmFontMetrics VmFont_Measure(int fontId, int px, const char *utf8, int len) {
    VmFontMetrics m = {0, 0, 0, 0, 1};
    if (fontId < 0 || fontId >= VM_FONT_MAX_SLOTS || !utf8 || px <= 0) return m;
    VmFontSlot *slot = &g_fonts[fontId];
    if (!slot->loaded) return m;

    int cap = len < 0 ? (int)strlen(utf8) : len;
    float scale = 0, lineStep = 0;
    int baseline = 0;
    vm_font_scale(slot, px, &scale, &baseline, &lineStep);
    m.baseline = baseline;
    m.lineStep = (int)lineStep;
    m.height = (int)lineStep;

    if (cap <= 0) return m;

    float cursorX = 0;
    int width = 0, lines = 1, prev = 0, pos = 0;
    while (pos < cap) {
        int sz = 0;
        int cp = vm_font_utf8_decode(utf8 + pos, &sz);
        if (sz <= 0) break;
        pos += sz;
        if (cp == '\n') {
            if (cursorX > (float)width) width = (int)cursorX;
            cursorX = 0;
            prev = 0;
            lines++;
            continue;
        }
        int g = vm_font_glyph(slot, cp);
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&slot->info, g, &advance, &lsb);
        if (prev) cursorX += stbtt_GetGlyphKernAdvance(&slot->info, prev, g) * scale;
        cursorX += (float)advance * scale;
        prev = g;
    }
    if (cursorX > (float)width) width = (int)cursorX;

    m.width = width;
    m.lines = lines;
    m.height = (int)(lineStep * (float)lines);
    return m;
}

int VmFont_Width(int fontId, int px, const char *utf8, int len) {
    return VmFont_Measure(fontId, px, utf8, len).width;
}

int VmFont_LineHeight(int fontId, int px) {
    return VmFont_Measure(fontId, px, "", 0).lineStep;
}

int VmFont_Baseline(int fontId, int px) {
    return VmFont_Measure(fontId, px, "", 0).baseline;
}

int VmFont_Wrap(int fontId, int px, const char *utf8, int len, int maxWidth,
                int *lineStarts, int cap) {
    if (lineStarts && cap > 0) lineStarts[0] = 0;
    if (fontId < 0 || fontId >= VM_FONT_MAX_SLOTS || !utf8 || px <= 0) return 1;
    VmFontSlot *slot = &g_fonts[fontId];
    if (!slot->loaded) return 1;

    int total = 0;
    int capBytes = len < 0 ? (int)strlen(utf8) : len;
    float scale = 0, lineStep = 0;
    int baseline = 0;
    vm_font_scale(slot, px, &scale, &baseline, &lineStep);
    (void)lineStep;
    (void)baseline;

    int pos = 0, lineStart = 0, prev = 0;
    float x = 0;
    int lastBreak = -1;       /* 可断行位置（空格之后）的字节偏移 */
    int hasContent = 0;
    total = 1;
    while (pos < capBytes) {
        int sz = 0;
        int cp = vm_font_utf8_decode(utf8 + pos, &sz);
        if (sz <= 0) break;
        int charStart = pos;
        pos += sz;

        if (cp == '\n') {
            if (total < cap && lineStarts) lineStarts[total] = pos;
            total++;
            lineStart = pos;
            x = 0;
            prev = 0;
            lastBreak = -1;
            hasContent = 0;
            continue;
        }

        int g = vm_font_glyph(slot, cp);
        int advance = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&slot->info, g, &advance, &lsb);
        float adv = (float)advance * scale;
        if (prev) adv += stbtt_GetGlyphKernAdvance(&slot->info, prev, g) * scale;

        if (maxWidth > 0 && hasContent && x + adv > (float)maxWidth) {
            int brk = (lastBreak > lineStart) ? lastBreak : charStart;
            if (total < cap && lineStarts) lineStarts[total] = brk;
            total++;
            lineStart = brk;
            pos = brk;
            x = 0;
            prev = 0;
            lastBreak = -1;
            hasContent = 0;
            continue;
        }

        x += adv;
        prev = g;
        hasContent = 1;
        if (cp == ' ') lastBreak = pos;
    }
    return total;
}
