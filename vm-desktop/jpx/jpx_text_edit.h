#ifndef JPX_TEXT_EDIT_H
#define JPX_TEXT_EDIT_H

/* 单行文本编辑状态机：只管 UTF-8 字节缓冲上的 caret / 选区 / 编辑操作。
   不依赖 Clay、WM、剪贴板，可独立断言。 */

#include <stddef.h>

typedef struct {
    char *buf;        /* 调用方持有，长度上限 cap（含结尾 NUL） */
    size_t cap;
    size_t len;       /* 当前字节长度，不含 NUL */
    size_t caret;     /* 字节偏移，恒落在 UTF-8 字符边界上 */
    size_t anchor;    /* 选区锚点；== caret 表示没有选区 */
} JpxTextEdit;

/* 初始化并接管已有文本；caret 落在末尾，长度按字符边界校正。 */
void Jpx__TextEditInit(JpxTextEdit *ed, char *buf, size_t cap, size_t len);

/* 设置 caret（钳到 [0,len] 并回退到字符边界）；同时折叠选区。 */
void Jpx__TextEditSetCaret(JpxTextEdit *ed, size_t pos);

int Jpx__TextEditHasSelection(const JpxTextEdit *ed);
/* 选区按 [start,end) 返回且保证 start <= end；无选区时两者都等于 caret。 */
void Jpx__TextEditSelection(const JpxTextEdit *ed, size_t *start, size_t *end);

/* 在 caret 处插入（先删除选区）。返回 1 表示按键被消费（容量不足时也消费，但不写入）。 */
int Jpx__TextEditInsert(JpxTextEdit *ed, const char *utf8, size_t bytes);

/* 处理一次按键。消费返回 1；未消费（Tab/Enter/上下键/纯修饰等）返回 0。 */
int Jpx__TextEditKey(JpxTextEdit *ed, const char *key, int ctrl, int shift);

/* 删除当前选区（无选区时不动）。返回 1 表示有删除。 */
int Jpx__TextEditDeleteSelection(JpxTextEdit *ed);

/* UTF-8 字节边界游标；对非法序列也不越界。 */
size_t Jpx__Utf8Prev(const char *s, size_t pos);
size_t Jpx__Utf8Next(const char *s, size_t len, size_t pos);

#endif
