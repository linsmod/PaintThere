#ifndef VM_FONT_H
#define VM_FONT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* guest 侧文本度量，与前端 stb_truetype 光栅化（assets/stb_font_wasm.wasm）同源同算法。
   几何口径以 rasterizeText 的 outW / outH 为准：累加用 float，最后才截断，
   因为前端真正贴到画布上的就是这个位图尺寸。 */

#define VM_FONT_MAX_SLOTS 8

typedef struct {
    int width;    /* 与 rasterizeText 的 outW 相同；空串为 0 */
    int height;   /* 与 rasterizeText 的 outH 相同；空串按一行行高返回 */
    int baseline; /* 位图顶部到基线，垂直居中与 caret 定位用 */
    int lineStep; /* 行距（px，已截断） */
    int lines;    /* 行数，至少 1 */
} VmFontMetrics;

/* 0 成功；<0 失败（-1 参数错，-2 读文件失败，-3 字体解析失败）。 */
int VmFont_LoadFile(const char *ttfPath, int fontId);
int VmFont_IsLoaded(int fontId);
void VmFont_Unload(int fontId);

/* 未加载或空参数时返回零值结构；空串返回 width=0、height/lineStep 为一行行高。 */
VmFontMetrics VmFont_Measure(int fontId, int px, const char *utf8, int len);
int VmFont_Width(int fontId, int px, const char *utf8, int len);
int VmFont_LineHeight(int fontId, int px);
int VmFont_Baseline(int fontId, int px);

/* 贪心换行：maxWidth 内尽量按空格断行，无空格时按字符断行。
   lineStarts 写入每行起始的字节偏移（cap 个），返回值是实际行数（可能大于 cap）。
   空串按 1 行处理。 */
int VmFont_Wrap(int fontId, int px, const char *utf8, int len, int maxWidth,
                int *lineStarts, int cap);

#ifdef __cplusplus
}
#endif

#endif
