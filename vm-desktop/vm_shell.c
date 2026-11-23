#include "vm_shell.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm_font.h"
#include "vm_menu.h"

/* 任务栏 = 一个无装饰/置顶/不可拖动/不抢焦点的特殊窗口。
   内容用 VmWm_SetWindowPaint 手绘：Clay 的 backgroundColor 只吃单色，
   XP 任务栏需要多 stop 渐变与位图填充。命中用 VmWm_SetWindowClickHandler
   （NOFOCUS 窗口拿不到焦点，只能走按下通道）。

   几何与样式全部来自 VmTheme 的 panel* token（由主题资源/Style 派生），
   这里不写死像素；token 为 0 时才用最小兜底值。 */

/* 主题没给值时的兜底：只保证「不会画出负尺寸」，不代表任何主题观感。
   几何正主在主题 token（见 DeriveTheme 的 key 映射），这里只是 0 兜底。 */
#define SHELL_FALLBACK_START_W 72
#define SHELL_FALLBACK_ITEM_W 110
#define SHELL_FALLBACK_ITEM_MIN 45

struct VmShell {
    VmWm *wm;
    int width, height;
    const VmTheme *theme;
    VmShellStartHandler startHandler;
    void *startUserData;
    VmShellSettingsHandler settingsHandler;
    void *settingsUserData;
    /* 任务栏选项（RetroBar Properties 开关子集），默认全开。 */
    int showClock;
    int showTrayIcon;
    /* 已处理过的按下序号：一次按下只触发一次，与重建帧数无关。
       左右键分开记：右键菜单打开/关闭不影响左键的按下沿。 */
    int lastDownSeq;
    int lastRightSeq;
};

static VmShell *g_shell;

static void append_rect(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                        const char *paint, float radius) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawRect.rect = r;
    VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint), paint);
    c->args.drawRect.cornerRadius = radius;
}

static void append_border(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                          int width, const char *color) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_BORDER, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawBorder.rect = r;
    VmDisplay_CopyStr(c->args.drawBorder.paint, sizeof(c->args.drawBorder.paint), color);
    c->args.drawBorder.widths[0] = c->args.drawBorder.widths[1] = (uint16_t)width;
    c->args.drawBorder.widths[2] = c->args.drawBorder.widths[3] = (uint16_t)width;
}

static void append_text(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                        const char *text, int fontId, int fontSize, const char *color) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_TEXT, id);
    if (!c) return;
    VmRect box = {(float)x, (float)y, (float)w, (float)h};
    c->rect = box;
    c->args.drawText.rect = box;
    VmDisplay_CopyStr(c->args.drawText.text, sizeof(c->args.drawText.text), text);
    c->args.drawText.fontId = (uint16_t)fontId;
    c->args.drawText.fontSize = (uint16_t)fontSize;
    VmDisplay_CopyStr(c->args.drawText.color, sizeof(c->args.drawText.color), color);
}

static void append_path(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                        const char *d, const char *paint) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_PATH, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawPath.rect = r;
    VmDisplay_CopyStr(c->args.drawPath.path, sizeof(c->args.drawPath.path), d);
    VmDisplay_CopyStr(c->args.drawPath.paint, sizeof(c->args.drawPath.paint), paint);
}

static void append_img(VmCmdBuffer *out, const char *id, int x, int y, int w, int h,
                       const char *imgId, int radius) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_IMG, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawImg.rect = r;
    VmDisplay_CopyStr(c->args.drawImg.imgId, sizeof(c->args.drawImg.imgId), imgId);
    c->args.drawImg.cornerRadius = (float)radius;
}

/* 按像素宽截断 UTF-8：从末尾去掉完整字符，直到放得下。 */
static void truncate_utf8(int fontId, int px, const char *text, int maxW,
                          char *out, size_t cap) {
    snprintf(out, cap, "%s", text ? text : "");
    if (maxW <= 0) { out[0] = '\0'; return; }
    while (out[0] && VmFont_Width(fontId, px, out, -1) > maxW) {
        size_t n = strlen(out);
        while (n > 0) {
            n--;
            if (((unsigned char)out[n] & 0xC0) != 0x80) break;
        }
        out[n] = '\0';
    }
}

/* 绘制与命中共用的几何。
   条目宽是 RetroBar 语义：默认 TaskButtonWidth，窗口多了收缩铺满整行
   （余数像素摊给前几条），到下限 TaskButtonMinWidth 为止；托盘宽内容驱动。 */
typedef struct {
    int pad;
    int startX, startW;
    int itemsX, itemW, itemExtra, gap;   /* itemExtra：前 N 条各加 1px 铺满整行 */
    int itemsRight;                       /* 条目区右界（显示桌面按钮或托盘左缘） */
    int showDesktopX, showDesktopW;
    int trayX;                            /* 托盘 GroupBox 左缘 */
    int n;                                /* 枚举到的窗口数 */
    int ids[VM_WM_MAX_WINDOWS];
} ShellLayout;

static int shell_item_visible(VmShell *sh, int id);

/* 任务条目 i 的矩形（index 是可见条目序号）。 */
static void shell_item_rect(const ShellLayout *L, int index, int *x, int *w) {
    *x = L->itemsX + index * (L->itemW + L->gap);
    *w = L->itemW + (index < L->itemExtra ? 1 : 0);
}

static void shell_geometry(VmShell *sh, ShellLayout *L) {
    memset(L, 0, sizeof(*L));
    const VmTheme *t = sh->theme;
    const int fontId = t->fontId;
    const int fontSize = t->fontSize > 0 ? t->fontSize : 13;
    L->pad = t->panelPad;
    L->startW = t->panelStartWidth > 0 ? t->panelStartWidth : SHELL_FALLBACK_START_W;
    L->startX = L->pad;
    int startMR = t->panelStartMarginRight;
    int listML = t->panelTaskListMarginLeft;
    /* 间距直接用主题值：派生层已按 key 缺失兜底，这里不能再把显式 0
       当缺失回退（XP Blue 的 TaskButtonMargin 覆盖为空 = 0 = 条目相邻，
       回退会让 3px 缝在消费端复活）。 */
    L->gap = t->panelItemMarginRight;
    int itemDef = t->panelItemWidthDefault > 0 ? t->panelItemWidthDefault
                                               : SHELL_FALLBACK_ITEM_W;
    int itemMin = t->panelItemWidthMin > 0 ? t->panelItemWidthMin
                                           : SHELL_FALLBACK_ITEM_MIN;
    L->n = VmWm_EnumerateCreated(sh->wm, L->ids, VM_WM_MAX_WINDOWS);

    /* 右侧区（内容驱动，从右往左）：时钟（Clock Margin）→ 托盘 GroupBox
       左缘 → Tray.Margin.Left → 显示桌面按钮。 */
    int clockZone = 0;
    if (sh->showClock) {
        char clock[16];
        time_t now = time(NULL);
        struct tm *tmVal = localtime(&now);
        clock[0] = '\0';
        if (tmVal) strftime(clock, sizeof(clock), "%H:%M", tmVal);
        clockZone = t->clockMarginLeft + VmFont_Width(fontId, fontSize, clock, -1) +
                    t->clockMarginRight;
    }
    L->trayX = sh->width - clockZone;   /* 托盘 GroupBox 左缘 */
    int sdIconW = t->panelShowDesktopIconW > 0 ? t->panelShowDesktopIconW : 16;
    L->showDesktopW = (sh->showTrayIcon && t->trayIconId)
                          ? t->panelShowDesktopMarginL + sdIconW +
                            t->panelShowDesktopMarginR
                          : 0;
    L->showDesktopX = L->trayX - t->panelTrayMarginLeft - L->showDesktopW;
    L->itemsRight = L->showDesktopX;

    /* 条目区：默认宽放得下就用默认（尾部留空），放不下收缩铺满整行。 */
    L->itemsX = L->startX + L->startW + startMR + listML;
    int avail = L->itemsRight - L->itemsX;
    int nVis = 0;
    for (int i = 0; i < L->n; i++) {
        if (shell_item_visible(sh, L->ids[i])) nVis++;
    }
    if (nVis <= 0 || avail <= 0) {
        L->itemW = itemDef;
        return;
    }
    int total = nVis * (itemDef + L->gap);
    if (total <= avail) {
        L->itemW = itemDef;
        return;
    }
    int base = (avail / nVis) - L->gap;
    if (base < itemMin) {
        L->itemW = itemMin;   /* 超出的条目绘制时越界截断（无滚动，登记 todo） */
    } else {
        L->itemW = base;
        L->itemExtra = avail - (base + L->gap) * nVis;
        if (L->itemExtra < 0) L->itemExtra = 0;
    }
}

/* 开始按钮高度由主题给；允许超出栏高（XP：StartButton.Height=32 放进 30 高栏，
   Top 对齐、底部溢出画布外）。任务条目 0 = 占满栏（RetroBar 的纵向 Stretch），
   主题给正值时按给定高度居中。 */
static int shell_start_height(VmShell *sh) {
    const int h = sh->height;
    int bh = sh->theme->panelStartButtonHeight > 0 ? sh->theme->panelStartButtonHeight : h - 8;
    if (bh < 8) bh = h - 4;
    return bh;
}

static int shell_item_height(VmShell *sh) {
    const int h = sh->height;
    int bh = sh->theme->panelItemHeight > 0 ? sh->theme->panelItemHeight : h;
    if (bh < 8) bh = h;
    return bh;
}

static float shell_radius(VmShell *sh) {
    /* 0 = 直角（经典主题无 CornerRadius 资源），是显式值，不回退；
       派生层按 key 缺失给 0，默认主题（现代深色）自带 5。 */
    return (float)sh->theme->panelRadius;
}

static int shell_item_visible(VmShell *sh, int id) {
    VmWindow *win = VmWm_FindWindow(sh->wm, id);
    if (!win || win->id == VM_SHELL_WINDOW_ID) return 0;
    /* 桌面图标层与弹出菜单是无装饰窗口，不占任务栏条目。 */
    return (win->flags & VM_WIN_UNDECORATED) == 0;
}

static void shell_paint(VmCmdBuffer *out, int x, int y, int w, int h, void *userData) {
    VmShell *sh = (VmShell *)userData;
    if (!sh || !sh->theme) return;
    const VmTheme *t = sh->theme;
    const int fontId = t->fontId;
    const int fontSize = t->fontSize > 0 ? t->fontSize : 13;
    const int lineStep = VmFont_LineHeight(fontId, fontSize) > 0
                             ? VmFont_LineHeight(fontId, fontSize) : fontSize;

    ShellLayout L;
    shell_geometry(sh, &L);

    /* 几何全部来自主题：高度、圆角、间距、图标尺寸都不在这里写死。
       开始按钮高于栏时顶对齐（XP：32 高按钮放进 30 高栏，底部溢出画布外）；
       条目占满栏（panelItemHeight=0）时 by=y、bh=h。 */
    const int sbh = shell_start_height(sh);
    const int sby = sbh >= h ? y : y + (h - sbh) / 2;
    const int bh = shell_item_height(sh);
    const int by = bh >= h ? y : y + (h - bh) / 2;
    const float radius = shell_radius(sh);

    /* 开始按钮：有位图皮肤就用位图三态（XP 的竖排雪碧取不同 1/3），
       其次是渐变（s_start），最后退回强调色单色。 */
    int overStart = 0;
    if (sh->wm->pointerValid) {
        int px = sh->wm->pointerX, py = sh->wm->pointerY;
        overStart = px >= x + L.startX && px < x + L.startX + L.startW &&
                    py >= sby && py < sby + sbh;
    }
    if (t->startImageId) {
        const char *seg = overStart
            ? (sh->wm->pointerDown ? "0,0.6666,1,0.3333" : "0,0.3333,1,0.3333")
            : "0,0,1,0.3333";
        char imgRef[160];
        snprintf(imgRef, sizeof(imgRef), "%s#%s", t->startImageId, seg);
        append_img(out, "tb_start", x + L.startX, sby, L.startW, sbh, imgRef, (int)radius);
    } else {
        const char *startPaint = t->startFrom ? "s_start" : t->accentColor;
        append_rect(out, "tb_start", x + L.startX, sby, L.startW, sbh, startPaint, radius);
    }
    {
        /* XP 排版：StartIcon 25×20 左 10px，StartLabel 左对齐跟在图标后、
           右侧留 StartLabel.Margin.Right 空隙；字号取 StartLabel.FontSize；
           StartLabelShadow 阴影 = 同文深色副本偏移 (1,1) 先画。 */
        const char *label = "开始";
        const int labelSize = t->panelStartLabelFontSize > 0
                                  ? t->panelStartLabelFontSize : fontSize;
        const int labelLine = VmFont_LineHeight(fontId, labelSize) > 0
                                  ? VmFont_LineHeight(fontId, labelSize) : labelSize;
        int tw = VmFont_Width(fontId, labelSize, label, -1);
        const char *color = t->startTextColor ? t->startTextColor : t->accentTextColor;
        int labelX, labelW;
        if (t->startIconId) {
            int iconW = t->panelStartIconW > 0 ? t->panelStartIconW : sbh - 8;
            int iconH = t->panelStartIconH > 0 ? t->panelStartIconH : iconW;
            int iconX = x + L.startX + t->panelStartIconMarginLeft;
            append_img(out, "tb_start_icon", iconX, sby + (sbh - iconH) / 2,
                       iconW, iconH, t->startIconId, 0);
            labelX = iconX + iconW;
            labelW = x + L.startX + L.startW - t->panelStartLabelMarginRight - labelX;
        } else {
            labelX = x + L.startX + (L.startW - tw) / 2;
            labelW = tw > 0 ? tw : L.startW;
        }
        int labelY = sby + (sbh - labelLine) / 2;
        if (labelW > 0) {
            if (t->startLabelShadowColor) {
                append_text(out, "tb_start_label_sh", labelX + 1, labelY + 1,
                            labelW, labelLine, label, fontId, labelSize,
                            t->startLabelShadowColor);
            }
            append_text(out, "tb_start_label", labelX, labelY,
                        labelW, labelLine, label, fontId, labelSize, color);
            if (t->boldChromeText) {
                append_text(out, "tb_start_label_b", labelX + 1, labelY,
                            labelW, labelLine, label, fontId, labelSize, color);
            }
        }
    }

    /* 任务条目：RetroBar 语义宽度（默认宽 → 收缩铺满 → 下限截断），
       标题超宽按像素截断。 */
    int vis = 0;
    for (int i = 0; i < L.n; i++) {
        if (!shell_item_visible(sh, L.ids[i])) continue;
        int ix, iw;
        shell_item_rect(&L, vis++, &ix, &iw);
        ix += x;
        if (ix + iw > x + L.itemsRight) break;   /* 无滚动：放不下的不再画 */
        VmWindow *win = VmWm_FindWindow(sh->wm, L.ids[i]);
        if (!win) continue;
        const char *paint;
        if (win->focused && t->taskButtonActiveFrom) paint = "s_task_active";
        else if (t->taskButtonFrom) paint = "s_task";
        else paint = win->focused ? t->accentColor : t->fgColor;

        /* 绘制区 = 条目盒内缩模板 Padding（XP ButtonBorder 0,3,0,2）：
           渐变体不画满整栏，栏顶/底露出背景条；开始按钮无此内缩，
           所以看起来比条目高。悬停判定仍用整条目盒。 */
        int padTop = t->panelItemPadTop > 0 ? t->panelItemPadTop : 0;
        int padBottom = t->panelItemPadBottom > 0 ? t->panelItemPadBottom : 0;
        if (padTop + padBottom > bh - 8) padTop = padBottom = 0;   /* 防栏过矮 */
        const int iy = by + padTop;
        const int ih = bh - padTop - padBottom;

        char idbuf[48];
        snprintf(idbuf, sizeof(idbuf), "tb_item_%d", win->id);
        append_rect(out, idbuf, ix, iy, iw, ih, paint, radius);

        /* 悬停：主题给了悬停渐变就用它，否则只在原底色上叠一层提亮。 */
        int hovered = 0;
        if (sh->wm->pointerValid) {
            int px = sh->wm->pointerX, py = sh->wm->pointerY;
            hovered = px >= ix && px < ix + iw && py >= by && py < by + bh;
        }
        if (hovered && !win->focused) {
            snprintf(idbuf, sizeof(idbuf), "tb_item_hover_%d", win->id);
            if (t->taskButtonHoverFrom) {
                append_rect(out, idbuf, ix, iy, iw, ih, "s_task_hover", radius);
            } else {
                append_rect(out, idbuf, ix, iy, iw, ih,
                            "rgba(255,255,255,0.14)", radius);
            }
        }

        /* 条目 bevel：未聚焦凸起（上/左浅、下/右深），聚焦按下反转
           （XP Luna 的四边 1px，颜色来自主题边框资源；画在内缩盒边缘）。 */
        {
            const char *top = t->taskBorderTopColor;
            const char *left = t->taskBorderLeftColor;
            const char *bottom = t->taskBorderBottomColor;
            const char *right = t->taskBorderRightColor;
            if (win->focused) {
                const char *swap;
                swap = top; top = bottom; bottom = swap;
                swap = left; left = right; right = swap;
            }
            if (top) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_top_%d", win->id);
                append_rect(out, idbuf, ix, iy, iw, 1, top, 0.0f);
            }
            if (bottom) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_bottom_%d", win->id);
                append_rect(out, idbuf, ix, iy + ih - 1, iw, 1, bottom, 0.0f);
            }
            if (left) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_left_%d", win->id);
                append_rect(out, idbuf, ix, iy + 1, 1, ih - 2, left, 0.0f);
            }
            if (right) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_right_%d", win->id);
                append_rect(out, idbuf, ix + iw - 1, iy + 1, 1, ih - 2, right, 0.0f);
            }
        }

        char label[128];
        int inset = t->panelTaskTextInsetX;   /* 0 = 贴边，显式值不回退 */
        truncate_utf8(fontId, fontSize, win->title, iw - inset - 4, label,
                      sizeof(label));
        snprintf(idbuf, sizeof(idbuf), "tb_item_text_%d", win->id);
        const char *color = t->taskButtonTextColor ? t->taskButtonTextColor : t->accentTextColor;
        append_text(out, idbuf, ix + inset, iy + (ih - lineStep) / 2,
                    iw - inset - 4, lineStep, label, fontId, fontSize, color);
    }

    /* 右侧：显示桌面按钮（托盘外、全高、内容驱动宽）+ 托盘 GroupBox
       （独立浅色区，左缘 1px 分隔）+ 时钟（Clock Margin 落位）。 */
    if (L.showDesktopW > 0 && t->trayIconId) {
        int iw = t->panelShowDesktopIconW > 0 ? t->panelShowDesktopIconW : 16;
        int ih = t->panelShowDesktopIconH > 0 ? t->panelShowDesktopIconH : iw;
        if (t->showDesktopFrom) {
            append_rect(out, "tb_showdesktop", x + L.showDesktopX, y,
                        L.showDesktopW, h, "s_showdesktop", 0.0f);
        }
        append_img(out, "tb_showdesktop_icon",
                   x + L.showDesktopX + t->panelShowDesktopMarginL,
                   y + (h - ih) / 2, iw, ih, t->trayIconId, 0);
    }
    if (t->trayFrom) {
        int trayW = x + w - (x + L.trayX);
        append_rect(out, "tb_tray", x + L.trayX, y, trayW, h, "s_tray", 0.0f);
        append_border(out, "tb_tray_edge", x + L.trayX, y, 1, h, 1,
                      "rgba(0,0,0,0.22)");
    }
    if (sh->showClock) {
        static char clock[16];
        time_t now = time(NULL);
        struct tm *tmVal = localtime(&now);
        clock[0] = '\0';
        if (tmVal) strftime(clock, sizeof(clock), "%H:%M", tmVal);
        int cw = VmFont_Width(fontId, fontSize, clock, -1);
        const char *color = t->clockTextColor ? t->clockTextColor : t->fgColor;
        int cx = x + L.trayX + t->clockMarginLeft;
        int availH = h - t->clockMarginTop - t->clockMarginBottom;
        int cy = y + t->clockMarginTop + (availH > lineStep ? (availH - lineStep) / 2 : 0);
        append_text(out, "tb_clock", cx, cy, cw > 0 ? cw : 1, lineStep,
                    clock, fontId, fontSize, color);
    }
}

/* 任务栏条目语义与桌面系统一致：最小化的还原、最大化的先还原尺寸、
   已聚焦的收起为最小化、其余聚焦到前面。 */
static void shell_activate_window(VmShell *sh, int wid) {
    VmWindow *win = VmWm_FindWindow(sh->wm, wid);
    if (!win) return;
    if (win->minimized) {
        VmWm_Restore(sh->wm, wid);
    } else if (win->maximized) {
        VmWm_Unmaximize(sh->wm, wid);
        VmWm_Focus(sh->wm, wid);
    } else if (win->focused) {
        VmWm_Minimize(sh->wm, wid);
    } else {
        VmWm_Focus(sh->wm, wid);
    }
}

/* 显示桌面 toggle：有未最小化的可见窗口 → 全部最小化；否则全部还原。 */
static void shell_toggle_show_desktop(VmShell *sh) {
    int ids[VM_WM_MAX_WINDOWS];
    int n = VmWm_EnumerateCreated(sh->wm, ids, VM_WM_MAX_WINDOWS);
    int anyVisible = 0;
    for (int i = 0; i < n; i++) {
        if (!shell_item_visible(sh, ids[i])) continue;
        VmWindow *win = VmWm_FindWindow(sh->wm, ids[i]);
        if (win && !win->minimized) anyVisible = 1;
    }
    for (int i = 0; i < n; i++) {
        if (!shell_item_visible(sh, ids[i])) continue;
        if (anyVisible) VmWm_Minimize(sh->wm, ids[i]);
        else VmWm_Restore(sh->wm, ids[i]);
    }
}

/* ---- 右键上下文菜单（RetroBar 交互）---- */

/* 条目菜单的动作表：与菜单项一一对应（分隔线占位为 0）。 */
enum {
    TASK_MENU_RESTORE = 1,
    TASK_MENU_MAXIMIZE,
    TASK_MENU_MINIMIZE,
    TASK_MENU_CLOSE,
    TASK_MENU_SEP
};

typedef struct {
    VmShell *sh;
    int wid;
    int actions[5];   /* index → TASK_MENU_* */
    int count;
} TaskMenuCtx;

/* 菜单单例一次一个，ctx 用静态存储保到回调触发（栈数组会悬空）。 */
static TaskMenuCtx g_taskMenu;

static void shell_task_menu_select(int index, void *userData) {
    TaskMenuCtx *ctx = (TaskMenuCtx *)userData;
    VmShell *sh = ctx->sh;
    VmWindow *win = VmWm_FindWindow(sh->wm, ctx->wid);
    if (!win) return;   /* 菜单打开期间窗口可能已被关掉 */
    switch (ctx->actions[index]) {
        case TASK_MENU_RESTORE:
            if (win->maximized) VmWm_Unmaximize(sh->wm, ctx->wid);
            else if (win->minimized) VmWm_Restore(sh->wm, ctx->wid);
            break;
        case TASK_MENU_MAXIMIZE:
            VmWm_Maximize(sh->wm, ctx->wid);
            break;
        case TASK_MENU_MINIMIZE:
            VmWm_Minimize(sh->wm, ctx->wid);
            break;
        case TASK_MENU_CLOSE:
            /* 与装饰 × 同一条路径：只发关闭请求，真正移除由持有者决定。 */
            if (win->closeHandler) win->closeHandler(win->id, win->closeUserData);
            else VmWm_RemoveWindow(sh->wm, ctx->wid);
            break;
        default:
            break;
    }
}

static void shell_task_context_menu(VmShell *sh, int wid, int x, int y) {
    VmWindow *win = VmWm_FindWindow(sh->wm, wid);
    if (!win) return;
    /* 菜单项按窗口状态动态生成（XP 的系统菜单语义）。 */
    VmMenuItem items[5];
    int *actions = g_taskMenu.actions;
    g_taskMenu.sh = sh;
    g_taskMenu.wid = wid;
    g_taskMenu.count = 0;
    if (win->maximized || win->minimized) {
        items[g_taskMenu.count].text = win->maximized ? "还原大小" : "还原";
        actions[g_taskMenu.count] = TASK_MENU_RESTORE;
        g_taskMenu.count++;
    } else {
        items[g_taskMenu.count].text = "最大化";
        actions[g_taskMenu.count] = TASK_MENU_MAXIMIZE;
        g_taskMenu.count++;
    }
    if (!win->minimized) {
        items[g_taskMenu.count].text = "最小化";
        actions[g_taskMenu.count] = TASK_MENU_MINIMIZE;
        g_taskMenu.count++;
    }
    items[g_taskMenu.count].text = NULL;
    actions[g_taskMenu.count] = TASK_MENU_SEP;
    g_taskMenu.count++;
    items[g_taskMenu.count].text = "关闭";
    actions[g_taskMenu.count] = TASK_MENU_CLOSE;
    g_taskMenu.count++;
    for (int i = 0; i < g_taskMenu.count; i++) items[i].enabled = 1;

    /* 锚在按下点，越界由 VmMenu 钳制进画布。 */
    VmMenu_Open(sh->wm, sh->theme, x, y, items, g_taskMenu.count,
                shell_task_menu_select, &g_taskMenu);
}

static void shell_bar_menu_select(int index, void *userData) {
    VmShell *sh = (VmShell *)userData;
    if (index == 0 && sh->settingsHandler) sh->settingsHandler(sh->settingsUserData);
}

static int shell_click(int winId, int x, int y, int button, void *userData) {
    (void)winId;
    VmShell *sh = (VmShell *)userData;
    if (!sh) return 0;
    VmWindow *self = VmWm_FindWindow(sh->wm, VM_SHELL_WINDOW_ID);
    if (!self) return 0;
    if (x < self->x || x >= self->x + self->w ||
        y < self->y || y >= self->y + self->h) return 0;
    /* 按下沿：同一次按下只处理一次，与后续重建帧数无关。左右键分开记。 */
    if (button == 2) {
        if (sh->wm->pointerDownSeq == sh->lastRightSeq) return 0;
        sh->lastRightSeq = sh->wm->pointerDownSeq;
    } else {
        if (sh->wm->pointerDownSeq == sh->lastDownSeq) return 0;
        sh->lastDownSeq = sh->wm->pointerDownSeq;
    }

    const int lx = x - self->x;
    ShellLayout L;
    shell_geometry(sh, &L);

    if (lx >= L.startX && lx < L.startX + L.startW) {
        if (sh->startHandler) sh->startHandler(sh->startUserData);
        return 1;
    }

    /* 显示桌面按钮：全部最小化 / 再次点击还原（XP 语义）。 */
    if (L.showDesktopW > 0 && lx >= L.showDesktopX && lx < L.showDesktopX + L.showDesktopW) {
        if (button == 0) shell_toggle_show_desktop(sh);
        return 1;
    }

    int vis = 0;
    for (int i = 0; i < L.n; i++) {
        if (!shell_item_visible(sh, L.ids[i])) continue;
        int ix, iw;
        shell_item_rect(&L, vis++, &ix, &iw);
        if (ix + iw > L.itemsRight) break;
        if (lx >= ix && lx < ix + iw) {
            if (button == 2) shell_task_context_menu(sh, L.ids[i], x, y);
            else shell_activate_window(sh, L.ids[i]);
            return 1;
        }
    }
    /* 任务栏空白处：左键只消费防穿透；右键是属性菜单（RetroBar 行为）。 */
    if (button == 2) {
        static const VmMenuItem kBarMenu[] = {
            {"属性", 1},
        };
        /* 菜单锚在按下点上方：VmMenu 越界时自动钳制进画布。 */
        VmMenu_Open(sh->wm, sh->theme, x, y - 1, kBarMenu, 1, shell_bar_menu_select, sh);
    }
    return 1;
}

/* 窗口集合 / 焦点 / 最小化变化：条目与高亮都要立刻更新，
   不能等每秒时钟刷新兜底（那会有最长 1 秒的不同步）。 */
static void shell_windows_changed(VmWm *wm, void *userData) {
    VmShell *sh = (VmShell *)userData;
    if (!wm || !sh) return;
    VmWm_InvalidateWindow(wm, VM_SHELL_WINDOW_ID);
}

void VmShell_SetStartHandler(VmShell *shell, VmShellStartHandler handler, void *userData) {
    if (!shell) return;
    shell->startHandler = handler;
    shell->startUserData = userData;
}

void VmShell_SetSettingsHandler(VmShell *shell, VmShellSettingsHandler handler,
                                void *userData) {
    if (!shell) return;
    shell->settingsHandler = handler;
    shell->settingsUserData = userData;
}

void VmShell_SetShowClock(VmShell *shell, int show) {
    if (!shell) return;
    shell->showClock = show ? 1 : 0;
    VmShell_Refresh(shell);
}

int VmShell_ShowClock(const VmShell *shell) {
    return shell ? shell->showClock : 0;
}

void VmShell_SetShowTrayIcon(VmShell *shell, int show) {
    if (!shell) return;
    shell->showTrayIcon = show ? 1 : 0;
    VmShell_Refresh(shell);
}

int VmShell_ShowTrayIcon(const VmShell *shell) {
    return shell ? shell->showTrayIcon : 0;
}

VmShell *VmShell_Create(VmWm *wm, const VmTheme *theme, int height) {
    if (!wm) return NULL;
    if (height <= 0) height = VM_SHELL_DEFAULT_HEIGHT;

    VmShell *sh = (VmShell *)calloc(1, sizeof(VmShell));
    if (!sh) return NULL;
    sh->wm = wm;
    sh->theme = theme ? theme : VmTheme_Default();
    sh->width = wm->width;
    sh->height = height;
    sh->showClock = 1;
    sh->showTrayIcon = 1;

    int y = wm->height - height;
    VmWindow *win = VmWm_AddWindow(wm, VM_SHELL_WINDOW_ID, "taskbar",
                                   0, y, sh->width, height, NULL, sh);
    if (!win) { free(sh); return NULL; }
    win->bodyStyleId = sh->theme->panelStyleId;
    VmWm_SetWindowFlags(wm, VM_SHELL_WINDOW_ID,
                        VM_WIN_UNDECORATED | VM_WIN_TOPMOST | VM_WIN_PINNED | VM_WIN_NOFOCUS);
    VmWm_SetWindowPaint(wm, VM_SHELL_WINDOW_ID, shell_paint, sh);
    VmWm_SetWindowClickHandler(wm, VM_SHELL_WINDOW_ID, shell_click, sh);

    /* 工作区 = 画布减去任务栏：最大化与拖动钳制都按它算。 */
    VmWm_SetWorkArea(wm, 0, 0, wm->width, wm->height - height);

    g_shell = sh;
    /* 订阅窗口集合/焦点变化：条目与高亮跟着立即更新。 */
    VmWm_SetWindowListHandler(wm, shell_windows_changed, sh);
    return sh;
}

void VmShell_Destroy(VmShell *shell) {
    if (!shell) return;
    VmWm_RemoveWindow(shell->wm, VM_SHELL_WINDOW_ID);
    if (g_shell == shell) g_shell = NULL;
    free(shell);
}

void VmShell_SetTheme(VmShell *shell, const VmTheme *theme) {
    if (!shell) return;
    shell->theme = theme ? theme : VmTheme_Default();
    VmShell_Refresh(shell);
}

void VmShell_SetHeight(VmShell *shell, int height) {
    if (!shell || height <= 0 || height == shell->height) return;
    VmWindow *win = VmWm_FindWindow(shell->wm, VM_SHELL_WINDOW_ID);
    if (!win) return;
    shell->height = height;
    /* 直接改矩形：任务栏钉在底部，不走 VmWm_Move（那会按工作区钳制）。 */
    win->y = shell->wm->height - height;
    win->h = height;
    VmWm_SetWorkArea(shell->wm, 0, 0, shell->wm->width, shell->wm->height - height);
    VmShell_Refresh(shell);
}

void VmShell_Refresh(VmShell *shell) {
    if (!shell) return;
    /* 只有任务栏自己在变：窗口列表由 paint 每帧重读，不需要作废其他窗口。 */
    VmWm_InvalidateWindow(shell->wm, VM_SHELL_WINDOW_ID);
}
