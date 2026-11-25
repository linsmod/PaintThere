#include "ui_text_edit.h"

#include <string.h>

size_t VmUi_Utf8Prev(const char *s, size_t pos) {
    if (!s || pos == 0) return 0;
    size_t i = pos - 1;
    /* 续字节形如 10xxxxxx，一路退到首字节 */
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--;
    return i;
}

size_t VmUi_Utf8Next(const char *s, size_t len, size_t pos) {
    if (!s || pos >= len) return len;
    size_t i = pos + 1;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    return i;
}

static int is_continuation(const char *s, size_t pos) {
    return ((unsigned char)s[pos] & 0xC0) == 0x80;
}

void VmUiTextEdit_Init(VmUiTextEdit *ed, char *buf, size_t cap, size_t len) {
    if (!ed) return;
    ed->buf = buf;
    ed->cap = cap;
    ed->len = 0;
    ed->caret = 0;
    ed->anchor = 0;
    if (!buf || cap == 0) return;

    if (len >= cap) len = cap - 1;
    /* 截断点若落在多字节字符中间，回退到该字符首字节 */
    while (len > 0 && len < cap && is_continuation(buf, len)) len--;
    ed->len = len;
    ed->caret = len;
    ed->anchor = len;
}

int VmUiTextEdit_HasSelection(const VmUiTextEdit *ed) {
    return ed && ed->anchor != ed->caret;
}

void VmUiTextEdit_Selection(const VmUiTextEdit *ed, size_t *start, size_t *end) {
    size_t a = ed ? ed->anchor : 0;
    size_t b = ed ? ed->caret : 0;
    if (a > b) {
        size_t t = a;
        a = b;
        b = t;
    }
    if (start) *start = a;
    if (end) *end = b;
}

void VmUiTextEdit_SetCaret(VmUiTextEdit *ed, size_t pos) {
    if (!ed || !ed->buf) return;
    if (pos > ed->len) pos = ed->len;
    while (pos > 0 && pos < ed->len && is_continuation(ed->buf, pos)) pos--;
    ed->caret = pos;
    ed->anchor = pos;
}

static void delete_range(VmUiTextEdit *ed, size_t a, size_t b) {
    if (a > b) {
        size_t t = a;
        a = b;
        b = t;
    }
    if (b > ed->len) b = ed->len;
    if (a >= b) return;
    memmove(ed->buf + a, ed->buf + b, ed->len - b);
    ed->len -= b - a;
    ed->buf[ed->len] = '\0';
    ed->caret = a;
    ed->anchor = a;
}

int VmUiTextEdit_DeleteSelection(VmUiTextEdit *ed) {
    size_t a, b;
    if (!ed || !VmUiTextEdit_HasSelection(ed)) return 0;
    VmUiTextEdit_Selection(ed, &a, &b);
    delete_range(ed, a, b);
    return 1;
}

int VmUiTextEdit_Insert(VmUiTextEdit *ed, const char *utf8, size_t bytes) {
    size_t room, tail;
    if (!ed || !ed->buf || !utf8 || bytes == 0) return 0;
    VmUiTextEdit_DeleteSelection(ed);

    /* 留一个字节给 NUL；放不下整个字符时按字符边界收缩，一个都放不下就不写。 */
    room = (ed->cap > ed->len + 1) ? ed->cap - ed->len - 1 : 0;
    while (bytes > room) {
        size_t shrunk = VmUi_Utf8Prev(utf8, bytes);
        if (shrunk == bytes) break;
        bytes = shrunk;
    }
    if (bytes == 0) return 1;

    tail = ed->len - ed->caret;
    memmove(ed->buf + ed->caret + bytes, ed->buf + ed->caret, tail);
    memcpy(ed->buf + ed->caret, utf8, bytes);
    ed->len += bytes;
    ed->buf[ed->len] = '\0';
    ed->caret += bytes;
    ed->anchor = ed->caret;
    return 1;
}

/* 把 caret 移到 pos（不折叠选区，供 Shift+方向键扩展）。 */
static void move_caret_extend(VmUiTextEdit *ed, size_t pos) {
    if (pos > ed->len) pos = ed->len;
    while (pos > 0 && pos < ed->len && is_continuation(ed->buf, pos)) pos--;
    ed->caret = pos;
}

static int key_is(const char *key, const char *name) {
    return strcmp(key, name) == 0;
}

int VmUiTextEdit_Key(VmUiTextEdit *ed, const char *key, int ctrl, int shift) {
    size_t start, end;
    int hasSel;
    if (!ed || !ed->buf || !key) return 0;
    hasSel = VmUiTextEdit_HasSelection(ed);
    VmUiTextEdit_Selection(ed, &start, &end);

    if (key_is(key, "Backspace")) {
        if (hasSel) {
            delete_range(ed, start, end);
        } else if (ed->caret > 0) {
            size_t prev = VmUi_Utf8Prev(ed->buf, ed->caret);
            delete_range(ed, prev, ed->caret);
        }
        return 1;
    }

    if (key_is(key, "Delete")) {
        if (hasSel) {
            delete_range(ed, start, end);
        } else if (ed->caret < ed->len) {
            size_t next = VmUi_Utf8Next(ed->buf, ed->len, ed->caret);
            delete_range(ed, ed->caret, next);
        }
        return 1;
    }

    if (key_is(key, "ArrowLeft")) {
        if (hasSel && !shift) {
            ed->caret = ed->anchor = start;
        } else {
            move_caret_extend(ed, VmUi_Utf8Prev(ed->buf, ed->caret));
            if (!shift) ed->anchor = ed->caret;
        }
        return 1;
    }

    if (key_is(key, "ArrowRight")) {
        if (hasSel && !shift) {
            ed->caret = ed->anchor = end;
        } else {
            move_caret_extend(ed, VmUi_Utf8Next(ed->buf, ed->len, ed->caret));
            if (!shift) ed->anchor = ed->caret;
        }
        return 1;
    }

    if (key_is(key, "Home")) {
        ed->caret = 0;
        if (!shift) ed->anchor = 0;
        return 1;
    }

    if (key_is(key, "End")) {
        ed->caret = ed->len;
        if (!shift) ed->anchor = ed->len;
        return 1;
    }

    if (ctrl && (key_is(key, "a") || key_is(key, "A"))) {
        ed->anchor = 0;
        ed->caret = ed->len;
        return 1;
    }

    /* 上下键、分页、Tab、Enter、Escape 交给焦点链或应用。 */
    if (key_is(key, "ArrowUp") || key_is(key, "ArrowDown") ||
        key_is(key, "PageUp") || key_is(key, "PageDown") ||
        key_is(key, "Tab") || key_is(key, "Enter") || key_is(key, "Escape")) {
        return 0;
    }

    /* Shift 已经体现在 key 本身（Shift+a 得到 "A"），只有 Ctrl 组合留给上层。 */
    if (ctrl) return 0;

    /* 可打印字符：单字节 ASCII，或多字节 UTF-8（首字节 >= 0x80）。 */
    if (key[0] != '\0' && ((unsigned char)key[0] >= 0x20 && (unsigned char)key[0] < 0x80)) {
        if (key[1] == '\0') return VmUiTextEdit_Insert(ed, key, 1);
        return 0;
    }
    if (key[0] != '\0' && (unsigned char)key[0] >= 0x80) {
        return VmUiTextEdit_Insert(ed, key, strlen(key));
    }
    return 0;
}
