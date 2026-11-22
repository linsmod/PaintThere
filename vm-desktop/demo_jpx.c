/* JPX 等价示例：与 demo_wm 同样的两个窗口，但应用代码只 include jpx.h。
   构建：python tools/build_vm.py → demo_jpx.exe
   运行：demo_jpx.exe 9000 ../assets */

#include "jpx/jpx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JpxApp *g_app;

typedef struct {
    int clicks;
    char label[64];
    char text[128];
    size_t textLen;
} WindowAState;

static WindowAState g_a;

static int on_key(int winId, const char *key, const char *code, int isDown,
                  int ctrl, int shift, int alt, void *user) {
    (void)winId;
    (void)code;
    (void)ctrl;
    (void)shift;
    (void)alt;
    if (!isDown) return 0;
    WindowAState *state = (WindowAState *)user;
    if (strcmp(key, "Backspace") == 0) {
        if (state->textLen > 0) state->text[--state->textLen] = '\0';
    } else if (strlen(key) == 1 && (unsigned char)key[0] >= 0x20 &&
               state->textLen + 2 < sizeof(state->text)) {
        state->text[state->textLen++] = key[0];
        state->text[state->textLen] = '\0';
    } else {
        return 0;
    }
    Jpx_Invalidate(g_app);
    return 1;
}

static void on_close(int winId, void *user) {
    (void)user;
    Jpx_WindowClose(g_app, winId);
}

static void layout_a(int win, void *user) {
    WindowAState *state = (WindowAState *)user;

    Jpx_Label(win, "Type below; focus this client with a click");
    Jpx_TextInput(win, "input", state->text, sizeof(state->text), &state->textLen);

    Jpx_RowBegin(win);
    int clicked = Jpx_Button(win, "btn", "Click me");
    Jpx_RowEnd(win);
    if (clicked) state->clicks++;

    snprintf(state->label, sizeof(state->label), "Clicked: %d", state->clicks);
    Jpx_Label(win, state->label);
}

static void layout_b(int win, void *user) {
    (void)user;
    Jpx_Label(win, "Window B");
    Jpx_PanelBegin(win);
    Jpx_Label(win, "A panel drawn from theme tokens");
    Jpx_Separator(win);
    Jpx_Label(win, "Second line");
    Jpx_PanelEnd(win);
}

int main(int argc, char **argv) {
    static char fontPath[512];
    JpxOptions opt = {0};
    opt.width = 800;
    opt.height = 600;
    opt.tileW = 100;
    opt.tileH = 100;
    opt.port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9000;
    opt.webRoot = argc > 2 ? argv[2] : "../assets";
    snprintf(fontPath, sizeof(fontPath), "%s/font.ttf", opt.webRoot);
    opt.fontPath = fontPath;
    opt.logLevel = 2;

    g_app = Jpx_Create(&opt);
    if (!g_app) {
        fprintf(stderr, "demo_jpx: Jpx_Create failed\n");
        return 1;
    }

    /* 与 demo_wm 的按钮视觉对齐：hover 用橙色。 */
    JpxTheme theme = *Jpx_Theme(g_app);
    theme.hoverColor = "rgba(240,140,40,1)";
    Jpx_SetTheme(g_app, &theme);

    JpxWindowDesc a = {0};
    a.title = "Window A";
    a.x = 60;
    a.y = 60;
    a.w = 360;
    a.h = 260;
    a.layout = layout_a;
    a.user = &g_a;
    a.onClose = on_close;
    a.onKey = on_key;

    JpxWindowDesc b = {0};
    b.title = "Window B";
    b.x = 300;
    b.y = 200;
    b.w = 360;
    b.h = 260;
    b.layout = layout_b;
    b.onClose = on_close;

    if (Jpx_WindowOpen(g_app, &a) == 0 || Jpx_WindowOpen(g_app, &b) == 0) {
        fprintf(stderr, "demo_jpx: window open failed\n");
        Jpx_Destroy(g_app);
        return 1;
    }

    int rc = Jpx_Run(g_app);
    Jpx_Destroy(g_app);
    return rc;
}
