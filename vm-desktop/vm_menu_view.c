#include "vm_menu_view.h"

#include <stdio.h>
#include <string.h>

#include "vm_font.h"

#define MENU_PAD_X 14
#define MENU_PAD_Y 4
#define MENU_BORDER 1
#define MENU_MIN_W 80
#define MENU_FALLBACK_LINE 14

static int line_h(const VmTheme *t) {
    int h = VmFont_LineHeight(t->fontId, t->fontSize);
    if (h > 0) return h;
    return t->fontSize > 0 ? t->fontSize : MENU_FALLBACK_LINE;
}

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

void VmMenuView_Layout(VmMenuView *v, const VmTheme *theme,
                       const VmMenuItem *items, int count,
                       int anchorX, int anchorY, int canvasW, int canvasH) {
    int line, itemH, sepH, maxW = 0, y;
    if (!v || !theme || !items || count <= 0) return;
    if (count > VM_MENU_VIEW_MAX_ITEMS) count = VM_MENU_VIEW_MAX_ITEMS;

    memset(v, 0, sizeof(*v));
    v->theme = theme;
    v->count = count;
    for (int i = 0; i < count; i++) v->items[i] = items[i];

    line = line_h(theme);
    itemH = line + 2 * MENU_PAD_Y;
    sepH = MENU_PAD_Y * 2 + 1;
    y = MENU_BORDER + 2;
    for (int i = 0; i < count; i++) {
        v->itemY[i] = y;
        if (!v->items[i].text) {
            v->itemH[i] = sepH;
        } else {
            int tw = VmFont_Width(theme->fontId, theme->fontSize, v->items[i].text, -1);
            v->itemH[i] = itemH;
            if (tw > maxW) maxW = tw;
        }
        y += v->itemH[i];
    }
    v->h = y + MENU_BORDER + 2;
    v->w = maxW + 2 * MENU_PAD_X + 2 * MENU_BORDER;
    if (v->w < MENU_MIN_W) v->w = MENU_MIN_W;

    v->x = anchorX;
    v->y = anchorY;
    if (v->x + v->w > canvasW) v->x = canvasW - v->w;
    if (v->y + v->h > canvasH) v->y = canvasH - v->h;
    if (v->x < 0) v->x = 0;
    if (v->y < 0) v->y = 0;
    v->hover = -1;
}

void VmMenuView_Paint(const VmMenuView *v, VmCmdBuffer *out) {
    const VmTheme *t;
    int line;
    if (!v || !out || !v->theme || v->count <= 0) return;
    t = v->theme;
    line = line_h(t);

    rect(out, "menu_bg", 0, 0, v->w, v->h,
         t->bgColor ? t->bgColor : "#ffffff", 0.0f);
    {
        VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_BORDER, "menu_border");
        if (c) {
            VmRect r = {0, 0, (float)v->w, (float)v->h};
            c->rect = r;
            c->args.drawBorder.rect = r;
            VmDisplay_CopyStr(c->args.drawBorder.paint, sizeof(c->args.drawBorder.paint),
                              t->borderColor ? t->borderColor : "#888888");
            for (int i = 0; i < 4; i++) c->args.drawBorder.widths[i] = MENU_BORDER;
        }
    }

    for (int i = 0; i < v->count; i++) {
        const int ih = v->itemH[i];
        const int ay = v->itemY[i];
        char id[24];
        if (!v->items[i].text) {
            snprintf(id, sizeof(id), "menu_sep_%d", i);
            rect(out, id, MENU_PAD_X / 2, ay + ih / 2, v->w - MENU_PAD_X, 1,
                 t->borderColor ? t->borderColor : "#888888", 0.0f);
            continue;
        }
        if (i == v->hover) {
            snprintf(id, sizeof(id), "menu_hi_%d", i);
            rect(out, id, MENU_BORDER, ay, v->w - 2 * MENU_BORDER, ih,
                 t->selectedColor ? t->selectedColor : t->accentColor, 0.0f);
        }
        {
            const char *color;
            if (!v->items[i].enabled)
                color = (t->disabledColor && *t->disabledColor) ? t->disabledColor : t->fgColor;
            else if (i == v->hover)
                color = t->accentTextColor ? t->accentTextColor : t->fgColor;
            else
                color = t->fgColor ? t->fgColor : "#000000";

            VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_TEXT, "menu_txt");
            if (c) {
                char tid[24];
                snprintf(tid, sizeof(tid), "menu_txt_%d", i);
                VmDisplay_CopyStr(c->id, sizeof(c->id), tid);
                VmRect box = {(float)(MENU_BORDER + MENU_PAD_X),
                              (float)(ay + (ih - line) / 2),
                              (float)(v->w - MENU_BORDER - MENU_PAD_X), (float)line};
                c->rect = box;
                c->args.drawText.rect = box;
                VmDisplay_CopyStr(c->args.drawText.text, sizeof(c->args.drawText.text),
                                  v->items[i].text);
                c->args.drawText.fontId = (uint16_t)t->fontId;
                c->args.drawText.fontSize = (uint16_t)t->fontSize;
                VmDisplay_CopyStr(c->args.drawText.color, sizeof(c->args.drawText.color),
                                  color);
            }
        }
    }
}

int VmMenuView_Hit(const VmMenuView *v, int cx, int cy) {
    int ly;
    if (!v || v->count <= 0) return -1;
    if (cx < v->x || cx >= v->x + v->w) return -1;
    if (cy < v->y || cy >= v->y + v->h) return -1;
    ly = cy - v->y;
    for (int i = 0; i < v->count; i++) {
        if (ly >= v->itemY[i] && ly < v->itemY[i] + v->itemH[i]) {
            if (!v->items[i].text || !v->items[i].enabled) return -1;
            return i;
        }
    }
    return -1;
}

int VmMenuView_SetHover(VmMenuView *v, int cx, int cy) {
    int h;
    if (!v) return 0;
    h = VmMenuView_Hit(v, cx, cy);
    if (h == v->hover) return 0;
    v->hover = h;
    return 1;
}
