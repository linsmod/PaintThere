#ifndef VM_UI_TEXT_EDIT_H
#define VM_UI_TEXT_EDIT_H

/* 单行文本编辑状态机：只管 UTF-8 字节缓冲上的 caret / 选区 / 编辑操作。
   不依赖 Clay / WM / 剪贴板，可独立断言。
   从 jpx_text_edit 迁移（M7.3）：接口去掉 Jpx 前缀，供任何 app 复用。 */

#include <stddef.h>

typedef struct {
    char *buf;        /* 调用方持有，容量 cap（含结尾 NUL） */
    size_t cap;
    size_t len;       /* 当前字节长度，不含 NUL */
    size_t caret;     /* 字节偏移，恒落在 UTF-8 字符边界上 */
    size_t anchor;    /* 选区锚点；== caret 表示没有选区 */
} VmUiTextEdit;

void   VmUiTextEdit_Init(VmUiTextEdit *ed, char *buf, size_t cap, size_t len);
void   VmUiTextEdit_SetCaret(VmUiTextEdit *ed, size_t pos);
int    VmUiTextEdit_HasSelection(const VmUiTextEdit *ed);
void   VmUiTextEdit_Selection(const VmUiTextEdit *ed, size_t *start, size_t *end);

/* 在 caret 处插入（先删除选区）。返回 1 表示按键被消费（容量不足时也消费，但不写入）。 */
int    VmUiTextEdit_Insert(VmUiTextEdit *ed, const char *utf8, size_t bytes);

/* 处理一次按键。消费返回 1；未消费（Tab/Enter/方向键/纯修饰等）返回 0。 */
int    VmUiTextEdit_Key(VmUiTextEdit *ed, const char *key, int ctrl, int shift);

int    VmUiTextEdit_DeleteSelection(VmUiTextEdit *ed);

size_t VmUi_Utf8Prev(const char *s, size_t pos);
size_t VmUi_Utf8Next(const char *s, size_t len, size_t pos);

#endif
