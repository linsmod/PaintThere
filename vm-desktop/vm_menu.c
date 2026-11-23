#include "vm_menu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_font.h"

/* 几何兜底：主题没给或字体未加载时只保证画得出来，不代表任何主题观感。 */
#define MENU_FALLBACK_LINE 14
#define MENU_PAD_X 14          /* 文字左右内边距（留出勾选/图标 gutter） */
#define MENU_PAD_Y 4           /* 条目上下内边距 */
#define MENU_BORDER 1          /* 菜单外边框宽 */
#define MENU_MIN_W 80

#define MENU_MAX_ITEMS 32

struct MenuState {
    VmWm *wm;
    const VmTheme *theme;
    VmMenuItem items[MENU_MAX_ITEMS];
    int count;
    void (*onSelect)(int index, void *userData);
    void *userData;
    /* 每个条目的命中矩形（画布坐标）。分隔线也占一项，h 记分隔条总高。 */
    int itemY[MENU_MAX_ITEMS];
    int itemH[MENU_MAX_ITEMS];
    int width, height;
    int open;
};

static struct MenuState g_menu;

static int menu_line_height(const VmTheme *t) {
    int h = VmFont_LineHeight(t->fontId, t->fontSize);
    return h > 0 ? h : (t->fontSize > 0 ? t->fontSize : MENU_FALLBACK_LINE);
}

static int menu_text_color_dim(const VmTheme *t) {
    return t->disabledColor && *t->disabledColor;
}

/* 量宽排高：条目按行高 + 上下内边距，分隔线固定高度。返回菜单总宽。 */
static void menu_measure(struct MenuState *m, const VmTheme *t) {
    const int line = menu_line_height(t);
    const int itemH = line + 2 * MENU_PAD_Y;
    const int sepH = MENU_PAD_Y * 2 + 1;
    int maxW = 0;
    int y = MENU_BORDER + 2;
    for (int i = 0; i < m->count; i++) {
        m->itemY[i] = y;
        if (!m->items[i].text) {
            m->itemH[i] = sepH;
        } else {
            m->itemH[i] = itemH;
            int tw = VmFont_Width(t->fontId, t->fontSize, m->items[i].text, -1);
            if (tw > maxW) maxW = tw;
        }
        y += m->itemH[i];
    }
    m->height = y + MENU_BORDER + 2;
    m->width = (maxW > 0 ? maxW : 0) + 2 * MENU_PAD_X + MENU_BORDER * 2;
    if (m->width < MENU_MIN_W) m->width = MENU_MIN_W;
}

static void menu_free_window(struct MenuState *m) {
    if (m->wm) {
        VmWm_RemoveWindow(m->wm, VM_MENU_WINDOW_ID);
        VmWm_SetPopup(m->wm, 0, NULL, NULL);
        m->wm = NULL;
    }
    m->open = 0;
}

/* WM popup dismiss 入口：外点只关菜单，不通知 onSelect。 */
static void menu_dismiss(void *userData) {
    (void)userData;
    menu_free_window(&g_menu);
}

static int menu_hit_item(const struct MenuState *m, int x, int y) {
    const VmWindow *win = VmWm_FindWindow(m->wm, VM_MENU_WINDOW_ID);
    if (!win) return -1;
    if (x < win->x || x >= win->x + win->w) return -1;
    int ly = y - win->y;
    for (int i = 0; i < m->count; i++) {
        if (ly >= m->itemY[i] && ly < m->itemY[i] + m->itemH[i]) return i;
    }
    return -1;
}

static void menu_paint(VmCmdBuffer *out, int x, int y, int w, int h, void *userData) {
    (void)h;
    struct MenuState *m = (struct MenuState *)userData;
    if (!m || !m->theme) return;
    const VmTheme *t = m->theme;
    const int line = menu_line_height(t);

    /* 背景 + 外边框。菜单用纯色（bgColor/borderColor），不引入新主题 token。 */
    {
        VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, "menu_bg");
        if (c) {
            VmRect r = {(float)x, (float)y, (float)w, (float)m->height};
            c->rect = r;
            c->args.drawRect.rect = r;
            VmDisplay_CopyStr(c->args.drawRect.paint,
                              sizeof(c->args.drawRect.paint),
                              t->bgColor ? t->bgColor : "#ffffff");
        }
        c = VmCmdBuffer_Append(out, VM_CMD_DRAW_BORDER, "menu_border");
        if (c) {
            VmRect r = {(float)x, (float)y, (float)w, (float)m->height};
            c->rect = r;
            c->args.drawBorder.rect = r;
            VmDisplay_CopyStr(c->args.drawBorder.paint,
                              sizeof(c->args.drawBorder.paint),
                              t->borderColor ? t->borderColor : "#888888");
            for (int i = 0; i < 4; i++) c->args.drawBorder.widths[i] = MENU_BORDER;
        }
    }

    /* 悬停判定：WM 的全局指针在画布坐标，命中矩形也是画布坐标。 */
    int hover = -1;
    if (m->wm->pointerValid) {
        hover = menu_hit_item(m, m->wm->pointerX, m->wm->pointerY);
        if (hover >= 0 && (!m->items[hover].text || !m->items[hover].enabled)) hover = -1;
    }

    for (int i = 0; i < m->count; i++) {
        const int ih = m->itemH[i];
        const int ay = y + m->itemY[i];
        if (!m->items[i].text) {
            /* 分隔线：左右各缩进 PAD_X/2，1px 边框色。 */
            char id[24];
            snprintf(id, sizeof(id), "menu_sep_%d", i);
            VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
            if (c) {
                VmRect r = {(float)(x + MENU_PAD_X / 2),
                            (float)(ay + ih / 2), (float)(w - MENU_PAD_X), 1.0f};
                c->rect = r;
                c->args.drawRect.rect = r;
                VmDisplay_CopyStr(c->args.drawRect.paint,
                                  sizeof(c->args.drawRect.paint),
                                  t->borderColor ? t->borderColor : "#888888");
            }
            continue;
        }
        if (i == hover) {
            char id[24];
            snprintf(id, sizeof(id), "menu_hi_%d", i);
            VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
            if (c) {
                VmRect r = {(float)(x + MENU_BORDER), (float)ay,
                            (float)(w - 2 * MENU_BORDER), (float)ih};
                c->rect = r;
                c->args.drawRect.rect = r;
                VmDisplay_CopyStr(c->args.drawRect.paint,
                                  sizeof(c->args.drawRect.paint),
                                  t->selectedColor ? t->selectedColor
                                                   : t->accentColor);
            }
        }
        const char *color;
        if (!m->items[i].enabled) {
            color = menu_text_color_dim(t) ? t->disabledColor : t->fgColor;
        } else if (i == hover) {
            color = t->accentTextColor ? t->accentTextColor : t->fgColor;
        } else {
            color = t->fgColor ? t->fgColor : "#000000";
        }
        char id[24];
        snprintf(id, sizeof(id), "menu_txt_%d", i);
        VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_TEXT, id);
        if (c) {
            VmRect box = {(float)(x + MENU_BORDER + MENU_PAD_X),
                          (float)(ay + (ih - line) / 2),
                          (float)(w - MENU_BORDER - MENU_PAD_X), (float)line};
            c->rect = box;
            c->args.drawText.rect = box;
            VmDisplay_CopyStr(c->args.drawText.text,
                              sizeof(c->args.drawText.text), m->items[i].text);
            c->args.drawText.fontId = (uint16_t)t->fontId;
            c->args.drawText.fontSize = (uint16_t)t->fontSize;
            VmDisplay_CopyStr(c->args.drawText.color,
                              sizeof(c->args.drawText.color), color);
        }
    }
}

static int menu_click(int winId, int x, int y, int button, void *userData) {
    (void)winId;
    (void)button;   /* 左键选中，右键也当选中并关闭（与 XP 行为一致） */
    struct MenuState *m = (struct MenuState *)userData;
    if (!m || !m->open) return 0;
    int idx = menu_hit_item(m, x, y);
    if (idx < 0 || !m->items[idx].text || !m->items[idx].enabled) {
        /* 点在菜单内非条目处：消费掉并关闭，避免穿透。 */
        menu_free_window(m);
        return 1;
    }
    void (*onSelect)(int, void *) = m->onSelect;
    void *userDataSel = m->userData;
    menu_free_window(m);
    /* 先关菜单再回调：回调里开新窗口（设置窗）不受窗口上限挤压，
       也不会在菜单窗口还挂着时被WM再次 dismiss。 */
    if (onSelect) onSelect(idx, userDataSel);
    return 1;
}

int VmMenu_Open(VmWm *wm, const VmTheme *theme, int x, int y,
                const VmMenuItem *items, int count,
                void (*onSelect)(int index, void *userData), void *userData) {
    if (!wm || !theme || !items || count <= 0 || count > MENU_MAX_ITEMS) return 0;
    if (g_menu.open) menu_free_window(&g_menu);

    struct MenuState *m = &g_menu;
    memset(m, 0, sizeof(*m));
    m->wm = wm;
    m->theme = theme;
    m->count = count;
    m->onSelect = onSelect;
    m->userData = userData;
    for (int i = 0; i < count; i++) m->items[i] = items[i];
    menu_measure(m, theme);

    /* 越界钳制：菜单优先向上/向左展开（右键点在任务栏条目上时，
       Windows 就是往画布内收）。 */
    if (x + m->width > wm->width) x = wm->width - m->width;
    if (y + m->height > wm->height) y = wm->height - m->height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (!VmWm_AddWindow(wm, VM_MENU_WINDOW_ID, "menu", x, y, m->width, m->height,
                        NULL, m)) {
        return 0;
    }
    VmWm_SetWindowFlags(wm, VM_MENU_WINDOW_ID,
                        VM_WIN_UNDECORATED | VM_WIN_TOPMOST | VM_WIN_PINNED |
                            VM_WIN_NOFOCUS);
    VmWm_SetWindowPaint(wm, VM_MENU_WINDOW_ID, menu_paint, m);
    VmWm_SetWindowClickHandler(wm, VM_MENU_WINDOW_ID, menu_click, m);
    VmWm_SetPopup(wm, VM_MENU_WINDOW_ID, menu_dismiss, NULL);
    m->open = 1;
    VmWm_Invalidate(wm);
    return VM_MENU_WINDOW_ID;
}

void VmMenu_Close(void) {
    menu_free_window(&g_menu);
}

int VmMenu_IsOpen(void) {
    return g_menu.open;
}
