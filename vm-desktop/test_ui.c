#include <stdio.h>
#include <string.h>

#include "ui_focus.h"
#include "ui_hit.h"
#include "ui_text_edit.h"

static int g_checks;
static int g_fails;

static void check(int cond, const char *msg) {
    g_checks++;
    if (!cond) {
        g_fails++;
        printf("  FAIL: %s\n", msg);
    }
}

static void check_int(int got, int want, const char *msg) {
    g_checks++;
    if (got != want) {
        g_fails++;
        printf("  FAIL: %s (got %d, want %d)\n", msg, got, want);
    }
}

static void check_str(const char *got, const char *want, const char *msg) {
    g_checks++;
    if (strcmp(got ? got : "", want ? want : "") != 0) {
        g_fails++;
        printf("  FAIL: %s (got '%s', want '%s')\n", msg, got ? got : "", want ? want : "");
    }
}

static void test_text_edit(void) {
    char buf[8];
    VmUiTextEdit ed;

    printf("text edit\n");
    snprintf(buf, sizeof(buf), "ab");
    VmUiTextEdit_Init(&ed, buf, sizeof(buf), 2);
    check_int((int)ed.caret, 2, "init: caret at end");
    check(VmUiTextEdit_Insert(&ed, "c", 1) == 1, "insert: consumed");
    check_str(buf, "abc", "insert: appended");
    check(VmUiTextEdit_Key(&ed, "Backspace", 0, 0) == 1, "backspace: consumed");
    check_str(buf, "ab", "backspace: removed last");
    check(VmUiTextEdit_Key(&ed, "a", 1, 0) == 1, "ctrl+a: consumed");
    check(VmUiTextEdit_HasSelection(&ed), "ctrl+a: selects all");
    VmUiTextEdit_Insert(&ed, "x", 1);
    check_str(buf, "x", "insert replaces selection");

    /* UTF-8 一个字符 3 字节。 */
    snprintf(buf, sizeof(buf), "a");
    VmUiTextEdit_Init(&ed, buf, sizeof(buf), 1);
    VmUiTextEdit_Insert(&ed, "\xE4\xB8\xAD", 3);
    check_int((int)ed.len, 4, "utf8: 1 ascii + 3-byte char");
    VmUiTextEdit_Key(&ed, "Backspace", 0, 0);
    check_str(buf, "a", "utf8: backspace removes whole char");

    /* 容量：cap=4 只能再放 2 字节。 */
    {
        char small[4];
        VmUiTextEdit se;
        snprintf(small, sizeof(small), "a");
        VmUiTextEdit_Init(&se, small, sizeof(small), 1);
        VmUiTextEdit_Insert(&se, "xyz", 3);
        /* cap=4 留 NUL 后只剩 2 字节空间：写入 "xy"，多出的 "z" 被丢弃。 */
        check_int((int)se.len, 3, "capacity: shrinks to available bytes");
        check_str(small, "axy", "capacity: stops at cap");
    }
}

static void test_focus(void) {
    VmUiFocus f;
    printf("focus\n");
    VmUiFocus_Reset(&f);
    VmUiFocus_Add(&f, 10);
    VmUiFocus_Add(&f, 20);
    VmUiFocus_Add(&f, 30);
    check_int(VmUiFocus_Add(&f, 20), 0, "add: duplicate rejected");
    check_int(VmUiFocus_Cycle(&f, 1), 10, "cycle: first from none");
    check_int(VmUiFocus_Cycle(&f, 1), 20, "cycle: forward");
    check_int(VmUiFocus_Cycle(&f, -1), 10, "cycle: backward");
    check_int(VmUiFocus_Cycle(&f, -1), 30, "cycle: wraps backward");
    VmUiFocus_Set(&f, 30);
    check_int(VmUiFocus_Get(&f), 30, "set: current");
    VmUiFocus_Remove(&f, 30);
    check_int(VmUiFocus_Get(&f), -1, "remove: clears current");
    check_int(VmUiFocus_Cycle(&f, 1), 10, "remove: cycle skips removed");
    VmUiFocus_Set(&f, 999);
    check_int(VmUiFocus_Get(&f), -1, "set: unknown id ignored");
}

static void test_hit(void) {
    VmUiHitRect rects[3];
    printf("hit\n");
    rects[0] = (VmUiHitRect){1, 0, 0, 100, 100, 1};
    rects[1] = (VmUiHitRect){2, 10, 10, 50, 50, 1};
    rects[2] = (VmUiHitRect){3, 20, 20, 10, 10, 0};
    check_int(VmUiHit_Test(rects, 3, 5, 5), 1, "hit: only base");
    check_int(VmUiHit_Test(rects, 3, 15, 15), 2, "hit: top-most wins");
    check_int(VmUiHit_Test(rects, 3, 25, 25), 2, "hit: disabled skipped");
    check_int(VmUiHit_Test(rects, 3, 200, 200), -1, "hit: outside");
    check_int(VmUiHit_Test(NULL, 0, 0, 0), -1, "hit: null table");
}

int main(int argc, char **argv) {
    int probe = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) probe = 1;
    }
    test_text_edit();
    test_focus();
    test_hit();
    if (!probe) printf("\n");
    printf("test_ui: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
