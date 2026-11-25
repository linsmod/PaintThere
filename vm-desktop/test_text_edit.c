/* 文本编辑状态机断言：不依赖 Clay / WM / 网络，纯逻辑，毫秒级。
   Build: vm-desktop/CMakeLists.txt 的 test_text_edit 目标
   Run:   test_text_edit.exe [--probe]
*/

#include <stdio.h>
#include <string.h>

#include "ui_text_edit.h"

static int g_failures;
static int g_checks;
static int g_probe;

static void section(const char *name) {
    if (!g_probe) printf("-- %s\n", name);
}

static void expect(int cond, const char *what) {
    g_checks++;
    if (cond) {
        if (!g_probe) printf("PASS  %s\n", what);
    } else {
        g_failures++;
        printf("FAIL  %s\n", what);
    }
}

static void expect_int(int actual, int expected, const char *what) {
    g_checks++;
    if (actual == expected) {
        if (!g_probe) printf("PASS  %s (=%d)\n", what, actual);
    } else {
        g_failures++;
        printf("FAIL  %s (expected %d, got %d)\n", what, expected, actual);
    }
}

static void expect_str(const char *actual, const char *expected, const char *what) {
    g_checks++;
    if (strcmp(actual, expected) == 0) {
        if (!g_probe) printf("PASS  %s (=\"%s\")\n", what, actual);
    } else {
        g_failures++;
        printf("FAIL  %s (expected \"%s\", got \"%s\")\n", what, expected, actual);
    }
}

static VmUiTextEdit make(char *buf, size_t cap, const char *text) {
    size_t n = text ? strlen(text) : 0;
    memset(buf, 0, cap);
    if (n >= cap) n = cap - 1;
    memcpy(buf, text, n);
    buf[n] = '\0';
    VmUiTextEdit ed;
    VmUiTextEdit_Init(&ed, buf, cap, n);
    return ed;
}

static void test_ascii(void) {
    section("ascii 基础编辑");
    char buf[64];
    VmUiTextEdit ed = make(buf, sizeof(buf), "");
    expect_int((int)ed.caret, 0, "initial caret at 0");
    expect_int((int)ed.len, 0, "initial len 0");

    expect_int(VmUiTextEdit_Key(&ed, "h", 0, 0), 1, "'h' consumed");
    expect_int(VmUiTextEdit_Key(&ed, "i", 0, 0), 1, "'i' consumed");
    expect_str(buf, "hi", "inserted \"hi\"");
    expect_int((int)ed.caret, 2, "caret after two chars");

    VmUiTextEdit_Key(&ed, "Backspace", 0, 0);
    expect_str(buf, "h", "backspace removes last char");
    expect_int((int)ed.caret, 1, "caret after backspace");

    VmUiTextEdit_Key(&ed, "Home", 0, 0);
    expect_int((int)ed.caret, 0, "home moves to start");
    VmUiTextEdit_Key(&ed, "Delete", 0, 0);
    expect_str(buf, "", "delete removes char at caret");
    expect_int((int)ed.caret, 0, "caret stays at start after delete");
    expect_int((int)ed.len, 0, "len 0 after delete");

    /* 中间插入 */
    VmUiTextEdit_Key(&ed, "x", 0, 0);
    VmUiTextEdit_Key(&ed, "y", 0, 0);
    VmUiTextEdit_Key(&ed, "Home", 0, 0);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 0);
    VmUiTextEdit_Key(&ed, "z", 0, 0);
    expect_str(buf, "xzy", "insert at caret position");
    expect_int((int)ed.caret, 2, "caret after mid insert");
    VmUiTextEdit_Key(&ed, "End", 0, 0);
    expect_int((int)ed.caret, 3, "end moves to len");
}

static void test_utf8(void) {
    section("utf8 多字节");
    char buf[64];
    VmUiTextEdit ed = make(buf, sizeof(buf), "");

    VmUiTextEdit_Key(&ed, "跨", 0, 0);
    expect_int((int)ed.len, 3, "one CJK char is 3 bytes");
    expect_int((int)ed.caret, 3, "caret after CJK char");
    VmUiTextEdit_Key(&ed, "Backspace", 0, 0);
    expect_int((int)ed.len, 0, "backspace removes whole CJK char");
    expect_int((int)ed.caret, 0, "caret back to 0");

    VmUiTextEdit_Key(&ed, "跨", 0, 0);
    VmUiTextEdit_Key(&ed, "端", 0, 0);
    expect_int((int)ed.len, 6, "two CJK chars 6 bytes");
    VmUiTextEdit_Key(&ed, "ArrowLeft", 0, 0);
    expect_int((int)ed.caret, 3, "left lands on char boundary");
    VmUiTextEdit_Key(&ed, "ArrowLeft", 0, 0);
    expect_int((int)ed.caret, 0, "left again to 0");
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 0);
    expect_int((int)ed.caret, 3, "right lands on boundary");
    VmUiTextEdit_Key(&ed, "Delete", 0, 0);
    expect_str(buf, "跨", "delete removes whole CJK char");
    expect_int((int)ed.len, 3, "len after CJK delete");
}

static void test_selection(void) {
    section("选区");
    char buf[64];
    VmUiTextEdit ed = make(buf, sizeof(buf), "abcd");
    expect_int((int)ed.caret, 4, "init caret at end");
    expect_int(VmUiTextEdit_HasSelection(&ed), 0, "no selection initially");

    VmUiTextEdit_Key(&ed, "Home", 0, 0);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 1);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 1);
    size_t a, b;
    VmUiTextEdit_Selection(&ed, &a, &b);
    expect_int((int)a, 0, "selection start 0");
    expect_int((int)b, 2, "selection end 2");
    expect_int(VmUiTextEdit_HasSelection(&ed), 1, "selection active");

    VmUiTextEdit_Key(&ed, "X", 0, 0);
    expect_str(buf, "Xcd", "typing replaces selection");
    expect_int((int)ed.caret, 1, "caret after replacement");
    expect_int(VmUiTextEdit_HasSelection(&ed), 0, "selection collapsed after typing");

    /* 无 Shift 的方向键折叠到选区边界 */
    ed = make(buf, sizeof(buf), "abcd");
    VmUiTextEdit_Key(&ed, "Home", 0, 0);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 1);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 1);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 0);
    expect_int((int)ed.caret, 2, "right without shift collapses to end");
    expect_int(VmUiTextEdit_HasSelection(&ed), 0, "no selection after collapse");

    /* 有选区时 Backspace 只删选区 */
    ed = make(buf, sizeof(buf), "abcd");
    VmUiTextEdit_Key(&ed, "Home", 0, 0);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 1);
    VmUiTextEdit_Key(&ed, "Backspace", 0, 0);
    expect_str(buf, "bcd", "backspace deletes selection only");

    /* 跨多字节字符的选区删除 */
    ed = make(buf, sizeof(buf), "中文");
    VmUiTextEdit_Key(&ed, "Home", 0, 0);
    VmUiTextEdit_Key(&ed, "ArrowRight", 0, 1);
    VmUiTextEdit_DeleteSelection(&ed);
    expect_str(buf, "文", "selection delete across one CJK char");

    /* Ctrl+A 全选 */
    ed = make(buf, sizeof(buf), "abc");
    expect_int(VmUiTextEdit_Key(&ed, "a", 1, 0), 1, "ctrl+a consumed");
    VmUiTextEdit_Selection(&ed, &a, &b);
    expect_int((int)a, 0, "select all start 0");
    expect_int((int)b, 3, "select all end 3");
}

static void test_ignored_keys(void) {
    section("未消费的键");
    char buf[32];
    VmUiTextEdit ed = make(buf, sizeof(buf), "abc");
    const char *keys[] = {"Tab", "Enter", "Escape", "ArrowUp", "ArrowDown", "PageUp", "PageDown"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        expect_int(VmUiTextEdit_Key(&ed, keys[i], 0, 0), 0, keys[i]);
    }
    expect_int(VmUiTextEdit_Key(&ed, "F1", 0, 0), 0, "F1 not inserted");
    expect_int(VmUiTextEdit_Key(&ed, "x", 1, 0), 0, "ctrl+x left to clipboard");
    expect_str(buf, "abc", "buffer unchanged by ignored keys");
}

static void test_shift_letter(void) {
    section("shift 字母");
    char buf[16];
    VmUiTextEdit ed = make(buf, sizeof(buf), "");
    expect_int(VmUiTextEdit_Key(&ed, "A", 0, 1), 1, "shift+A consumed as text");
    expect_str(buf, "A", "uppercase inserted via shift state");
}

static void test_capacity(void) {
    section("容量与截断");
    char buf[8];
    VmUiTextEdit ed = make(buf, sizeof(buf), "");
    const char *s = "abcdefgh";
    for (size_t i = 0; i < 8; i++) {
        char one[2] = {s[i], '\0'};
        VmUiTextEdit_Key(&ed, one, 0, 0);
    }
    expect_int((int)ed.len, 7, "buffer capped at cap-1");
    expect_str(buf, "abcdefg", "last char dropped when full");

    ed = make(buf, sizeof(buf), "跨端");
    expect_int((int)ed.len, 6, "two CJK chars fill 6 of 8 bytes");
    expect_int(VmUiTextEdit_Key(&ed, "输", 0, 0), 1, "key consumed even when full");
    expect_int((int)ed.len, 6, "no partial UTF-8 written");
    expect_str(buf, "跨端", "buffer unchanged when no room for full char");
}

static void test_boundaries(void) {
    section("边界与非法序列");
    char buf[16];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "跨端", 6);
    VmUiTextEdit ed;
    VmUiTextEdit_Init(&ed, buf, sizeof(buf), 4);   /* 4 落在「端」的续字节 */
    expect_int((int)ed.len, 3, "init truncates to char boundary");
    expect_int((int)ed.caret, 3, "init caret at truncated len");

    ed = make(buf, sizeof(buf), "跨端");
    VmUiTextEdit_SetCaret(&ed, 4);
    expect_int((int)ed.caret, 3, "set caret snaps back to boundary");
    VmUiTextEdit_SetCaret(&ed, 99);
    expect_int((int)ed.caret, 6, "set caret clamps to len");

    char bad[4] = {(char)0xFF, 'A', '\0', '\0'};
    expect_int((int)VmUi_Utf8Prev(bad, 2), 1, "utf8 prev over invalid lead");
    expect_int((int)VmUi_Utf8Next(bad, 2, 0), 1, "utf8 next over invalid lead");
    expect_int((int)VmUi_Utf8Prev(bad, 0), 0, "utf8 prev at 0");
    expect_int((int)VmUi_Utf8Next(bad, 2, 2), 2, "utf8 next at end");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) g_probe = 1;
    }
    test_ascii();
    test_utf8();
    test_selection();
    test_ignored_keys();
    test_shift_letter();
    test_capacity();
    test_boundaries();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
