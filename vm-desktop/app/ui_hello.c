#include "ui_hello.h"

#include <stdio.h>
#include <string.h>

#include "vm_font.h"

#define HELLO_FONT_ID 0
#define HELLO_FONT_SIZE 14
#define HELLO_PAD 16
#define HELLO_BTN_W 168
#define HELLO_BTN_H 36

static void rect(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                 const char *paint, float radius) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawRect.rect = r;
    VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint), paint);
    c->args.drawRect.cornerRadius = radius;
}

static void border(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                   int width, const char *color) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_BORDER, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawBorder.rect = r;
    VmDisplay_CopyStr(c->args.drawBorder.paint, sizeof(c->args.drawBorder.paint), color);
    for (int i = 0; i < 4; i++) c->args.drawBorder.widths[i] = width;
}

static void text(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                 const char *s, const char *color) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_TEXT, id);
    if (!c) return;
    VmRect box = {(float)x, (float)y, (float)w, (float)h};
    c->rect = box;
    c->args.drawText.rect = box;
    VmDisplay_CopyStr(c->args.drawText.text, sizeof(c->args.drawText.text), s);
    c->args.drawText.fontId = HELLO_FONT_ID;
    c->args.drawText.fontSize = HELLO_FONT_SIZE;
    VmDisplay_CopyStr(c->args.drawText.color, sizeof(c->args.drawText.color), color);
}

/* 从尾部截取到 maxW，保证最新输入与 caret 可见；不切断 UTF-8 字符。 */
static void fit_tail(const char *s, int maxW, char *out, size_t cap) {
    size_t n = strlen(s);
    size_t start = 0;
    while (start < n && VmFont_Width(HELLO_FONT_ID, HELLO_FONT_SIZE, s + start, -1) > maxW) {
        start++;
        while (start < n && ((unsigned char)s[start] & 0xC0) == 0x80) start++;
    }
    snprintf(out, cap, "%s", s + start);
}

void Hello_ButtonRect(const HelloState *s, int *x, int *y, int *w, int *h) {
    (void)s;
    *x = HELLO_PAD;
    *y = HELLO_PAD + 40;
    *w = HELLO_BTN_W;
    *h = HELLO_BTN_H;
}

void Hello_InputRect(const HelloState *s, int *x, int *y, int *w, int *h) {
    *x = HELLO_PAD;
    *y = HELLO_PAD + 96;
    *w = s->bodyW - HELLO_PAD * 2;
    *h = 30;
}

void Hello_Paint(const HelloState *s, VmCmdBuffer *out) {
    if (!s || !out) return;
    const int w = s->bodyW > 0 ? s->bodyW : 400;
    const int h = s->bodyH > 0 ? s->bodyH : 240;
    const int line = VmFont_LineHeight(HELLO_FONT_ID, HELLO_FONT_SIZE) > 0
                         ? VmFont_LineHeight(HELLO_FONT_ID, HELLO_FONT_SIZE)
                         : HELLO_FONT_SIZE;

    rect(out, "bg", 0, 0, w, h, "rgba(43,45,49,1)", 0.0f);
    text(out, "label", HELLO_PAD, HELLO_PAD - 4, w - HELLO_PAD * 2, line + 4,
         "vm_app / A1 (no Clay)", "rgba(220,224,230,1)");

    int bx, by, bw, bh;
    Hello_ButtonRect(s, &bx, &by, &bw, &bh);
    const char *paint = s->pressed ? "rgba(62,126,224,1)" : "rgba(48,96,180,1)";
    rect(out, "btn", bx, by, bw, bh, paint, 4.0f);
    char btnLabel[64];
    snprintf(btnLabel, sizeof(btnLabel), "Clicked: %d%s", s->clicks,
             s->pressed ? "  [on]" : "");
    text(out, "btn_label", bx + 12, by + (bh - line) / 2, bw - 24, line,
         btnLabel, "rgba(255,255,255,1)");

    int ix, iy, iw, ih;
    Hello_InputRect(s, &ix, &iy, &iw, &ih);
    rect(out, "input", ix, iy, iw, ih, "rgba(24,26,30,1)", 3.0f);
    border(out, "input_border", ix, iy, iw, ih, 1,
           s->focused ? "rgba(90,150,240,1)" : "rgba(90,96,104,1)");

    char shown[256];
    fit_tail(s->text, iw - 16, shown, sizeof(shown));
    const int textX = ix + 8;
    const int textY = iy + (ih - line) / 2;
    text(out, "input_text", textX, textY, iw - 16, line, shown, "rgba(235,238,242,1)");

    if (s->focused && s->caretOn) {
        int caretX = textX + VmFont_Width(HELLO_FONT_ID, HELLO_FONT_SIZE, shown, -1);
        if (caretX > ix + iw - 4) caretX = ix + iw - 4;
        rect(out, "caret", caretX, textY, 1, line, "rgba(235,238,242,1)", 0.0f);
    }

    text(out, "hint", HELLO_PAD, h - line - HELLO_PAD, w - HELLO_PAD * 2, line,
         "click the button / click the box and type", "rgba(150,156,164,1)");
}
