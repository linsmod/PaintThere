/* Measure text with mystbfont_c (stb_truetype) for cross-end consistency test.
   Build: gcc -std=c11 -I<mystbfont-dir> font_probe.c <mystbfont-dir>/stb_font_cache.c -lm -o font_probe
   Run:   ./font_probe <font.ttf> <fontSize> <text> */
#include "stb_font_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *data = (unsigned char *)malloc((size_t)n);
    size_t got = fread(data, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(data); return NULL; }
    *size = got;
    return data;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: font_probe <font.ttf> <fontSize> <text>\n");
        return 1;
    }
    size_t fontDataSize = 0;
    unsigned char *fontData = read_file(argv[1], &fontDataSize);
    if (!fontData) { fprintf(stderr, "cannot read font\n"); return 1; }

    stb_font_cache_t *cache = stb_font_cache_create(NULL, NULL);
    stb_font_cache_set_face_size(cache, atoi(argv[2]));

    stb_font_memory_t mem = {0};
    if (stb_font_memory_alloc(&mem, fontDataSize) != 0) { fprintf(stderr, "oom\n"); return 1; }
    memcpy(mem.data, fontData, fontDataSize);
    if (stb_font_load_managed(cache, &mem, 0) != 0) { fprintf(stderr, "font load failed\n"); return 1; }

    int width = 0, height = 0;
    stb_font_get_text_size(cache, argv[3], -1, &width, &height);
    printf("%d %d\n", width, height);

    stb_font_cache_destroy(cache);
    free(fontData);
    return 0;
}
