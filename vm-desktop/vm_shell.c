#include "vm_shell.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm_font.h"
#include "vm_shell_internal.h"

/* 任务栏内容的生产者：只读 VmShellModel 快照，产出绘制命令或动作。
   这里没有 VmWm、没有 Clay、没有 IPC —— 数据源由调用方提供，
   所以同一份代码既能服务单进程 demo，也能服务独立的 vm_shell 进程。

   几何与样式全部来自 VmTheme 的 panel* token（由主题资源/Style 派生），
   这里不写死像素；token 为 0 时才用最小兜底值。 */

/* 主题没给值时的兜底：只保证「不会画出负尺寸」，不代表任何主题观感。
   几何正主在主题 token（见 DeriveTheme 的 key 映射），这里只是 0 兜底。 */
#define SHELL_FALLBACK_START_W 72
#define SHELL_FALLBACK_ITEM_W 110
#define SHELL_FALLBACK_ITEM_MIN 45

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
    int n;                                /* model 里的条目总数 */
    int nVis;                             /* 可见条目数 */
    int idx[VM_SHELL_MAX_ITEMS];          /* 可见条目在 model 里的下标 */
} ShellLayout;

static int item_visible(const VmShellItem *it) {
    if (!it || it->id == VM_SHELL_WINDOW_ID) return 0;
    /* 桌面图标层与弹出菜单是无装饰窗口，不占任务栏条目。 */
    return !it->undecorated;
}

/* 任务条目 i 的矩形（index 是可见条目序号）。 */
static void shell_item_rect(const ShellLayout *L, int index, int *x, int *w) {
    *x = L->itemsX + index * (L->itemW + L->gap);
    *w = L->itemW + (index < L->itemExtra ? 1 : 0);
}

static int shell_start_height(const VmShell *sh) {
    const int h = sh->height;
    int bh = sh->theme->panelStartButtonHeight > 0 ? sh->theme->panelStartButtonHeight : h - 8;
    if (bh < 8) bh = h - 4;
    return bh;
}

static int shell_item_height(const VmShell *sh) {
    const int h = sh->height;
    int bh = sh->theme->panelItemHeight > 0 ? sh->theme->panelItemHeight : h;
    if (bh < 8) bh = h;
    return bh;
}

static float shell_radius(const VmShell *sh) {
    /* 0 = 直角（经典主题无 CornerRadius 资源），是显式值，不回退；
       派生层按 key 缺失给 0，默认主题（现代深色）自带 5。 */
    return (float)sh->theme->panelRadius;
}

static void shell_geometry(const VmShell *sh, const VmShellModel *m, int w,
                           ShellLayout *L) {
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
    L->n = m ? m->count : 0;
    L->nVis = 0;
    for (int i = 0; i < L->n; i++) {
        if (!item_visible(&m->items[i])) continue;
        L->idx[L->nVis++] = i;
    }

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
    L->trayX = w - clockZone;   /* 托盘 GroupBox 左缘 */
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
    int nVis = L->nVis;
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

static int pointer_over(const VmShellModel *m, int x, int y, int w, int h) {
    if (!m || !m->pointerValid) return 0;
    return m->pointerX >= x && m->pointerX < x + w &&
           m->pointerY >= y && m->pointerY < y + h;
}

void VmShell_Paint(VmShell *sh, const VmShellModel *m, VmCmdBuffer *out,
                   int x, int y, int w, int h) {
    if (!sh || !m || !sh->theme) return;
    const VmTheme *t = sh->theme;
    const int fontId = t->fontId;
    const int fontSize = t->fontSize > 0 ? t->fontSize : 13;
    const int lineStep = VmFont_LineHeight(fontId, fontSize) > 0
                             ? VmFont_LineHeight(fontId, fontSize) : fontSize;

    ShellLayout L;
    shell_geometry(sh, m, w, &L);

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
    int overStart = pointer_over(m, x + L.startX, sby, L.startW, sbh);
    if (t->startImageId) {
        const char *seg = overStart
            ? (m->pointerDown ? "0,0.6666,1,0.3333" : "0,0.3333,1,0.3333")
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
    for (int i = 0; i < L.nVis; i++) {
        const VmShellItem *it = &m->items[L.idx[i]];
        int ix, iw;
        shell_item_rect(&L, i, &ix, &iw);
        ix += x;
        if (ix + iw > x + L.itemsRight) break;   /* 无滚动：放不下的不再画 */
        /* 悬停判定先于绘制：状态决定模板层集选择。 */
        int hovered = pointer_over(m, ix, by, iw, bh);

        /* 模板层集：focused → Pressed；hovered 且非 focused → Hover（缺则
           normal 层集 + 旧悬停叠加）；层集不可用走 From/To + bevel 旧路径。 */
        const VmTaskButtonLayer *st = NULL;
        if (it->focused) st = t->taskActive.stopCount >= 2 ? &t->taskActive : NULL;
        else if (hovered) st = t->taskHover.stopCount >= 2 ? &t->taskHover : NULL;
        if (!st && t->taskNormal.stopCount >= 2) st = &t->taskNormal;

        const char *paint;
        if (st) paint = it->focused ? "s_task_active"
                    : (hovered && t->taskHover.stopCount >= 2) ? "s_task_hover"
                    : "s_task";
        else if (it->focused && t->taskButtonActiveFrom) paint = "s_task_active";
        else if (t->taskButtonFrom) paint = "s_task";
        else paint = it->focused ? t->accentColor : t->fgColor;

        /* 绘制区 = 条目盒内缩模板 Padding（XP ButtonBorder 0,3,0,2）：
           渐变体不画满整栏，栏顶/底露出背景条；开始按钮无此内缩，
           所以看起来比条目高。悬停判定仍用整条目盒。 */
        int padTop = t->panelItemPadTop > 0 ? t->panelItemPadTop : 0;
        int padBottom = t->panelItemPadBottom > 0 ? t->panelItemPadBottom : 0;
        if (padTop + padBottom > bh - 8) padTop = padBottom = 0;   /* 防栏过矮 */
        const int iy = by + padTop;
        const int ih = bh - padTop - padBottom;

        char idbuf[48];
        snprintf(idbuf, sizeof(idbuf), "tb_item_%d", it->id);
        append_rect(out, idbuf, ix, iy, iw, ih, paint, radius);

        if (st) {
            /* RetroBar 模板层次：主渐变 → 外 4 边 → 内 3 边 → 对角高光层。
               各状态有自己的边色，不做 bevel 反转。 */
            if (st->borderTop) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_top_%d", it->id);
                append_rect(out, idbuf, ix, iy, iw, 1, st->borderTop, 0.0f);
            }
            if (st->borderLeft) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_left_%d", it->id);
                append_rect(out, idbuf, ix, iy + 1, 1, ih - 2, st->borderLeft, 0.0f);
            }
            if (st->borderBottom) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_bottom_%d", it->id);
                append_rect(out, idbuf, ix, iy + ih - 1, iw, 1, st->borderBottom, 0.0f);
            }
            if (st->borderRight) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_right_%d", it->id);
                append_rect(out, idbuf, ix + iw - 1, iy + 1, 1, ih - 2, st->borderRight, 0.0f);
            }
            if (st->innerLeft) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_il_%d", it->id);
                append_rect(out, idbuf, ix + 1, iy + 1, 1, ih - 3, st->innerLeft, 0.0f);
            }
            if (st->innerRight) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_ir_%d", it->id);
                append_rect(out, idbuf, ix + iw - 2, iy + 1, 1, ih - 3, st->innerRight, 0.0f);
            }
            if (st->innerBottom) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_ib_%d", it->id);
                append_rect(out, idbuf, ix + 1, iy + ih - 2, iw - 2, 1, st->innerBottom, 0.0f);
            }
            if (st->hlStopCount >= 2) {
                const char *hl = it->focused ? "s_task_active_hl"
                               : (hovered && t->taskHover.stopCount >= 2) ? "s_task_hover_hl"
                               : "s_task_hl";
                snprintf(idbuf, sizeof(idbuf), "tb_item_hl_%d", it->id);
                append_rect(out, idbuf, ix + 2, iy + 1, iw - 4, ih - 3, hl, radius);
            }
            /* hover 无层集但主题给了悬停渐变：在层集之上叠加。 */
            if (hovered && !it->focused && t->taskHover.stopCount < 2 &&
                t->taskButtonHoverFrom) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_hover_%d", it->id);
                append_rect(out, idbuf, ix, iy, iw, ih, "s_task_hover", radius);
            }
        } else {
            /* 悬停：主题给了悬停渐变就用它，否则只在原底色上叠一层提亮。 */
            if (hovered && !it->focused) {
                snprintf(idbuf, sizeof(idbuf), "tb_item_hover_%d", it->id);
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
                if (it->focused) {
                    const char *swap;
                    swap = top; top = bottom; bottom = swap;
                    swap = left; left = right; right = swap;
                }
                if (top) {
                    snprintf(idbuf, sizeof(idbuf), "tb_item_top_%d", it->id);
                    append_rect(out, idbuf, ix, iy, iw, 1, top, 0.0f);
                }
                if (bottom) {
                    snprintf(idbuf, sizeof(idbuf), "tb_item_bottom_%d", it->id);
                    append_rect(out, idbuf, ix, iy + ih - 1, iw, 1, bottom, 0.0f);
                }
                if (left) {
                    snprintf(idbuf, sizeof(idbuf), "tb_item_left_%d", it->id);
                    append_rect(out, idbuf, ix, iy + 1, 1, ih - 2, left, 0.0f);
                }
                if (right) {
                    snprintf(idbuf, sizeof(idbuf), "tb_item_right_%d", it->id);
                    append_rect(out, idbuf, ix + iw - 1, iy + 1, 1, ih - 2, right, 0.0f);
                }
            }
        }

        char label[128];
        int inset = t->panelTaskTextInsetX;   /* 0 = 贴边，显式值不回退 */
        truncate_utf8(fontId, fontSize, it->title, iw - inset - 4, label,
                      sizeof(label));
        snprintf(idbuf, sizeof(idbuf), "tb_item_text_%d", it->id);
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

int VmShell_HitTest(VmShell *sh, const VmShellModel *m, int lx, int ly,
                    int button, int down, VmShellAction *out) {
    if (!sh || !m || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (!down) return 0;   /* 只有按下产生动作 */

    /* 按下沿：同一次按下只处理一次，与后续重建帧数无关。左右键分开记。 */
    if (button == 2) {
        if (m->downSeq <= sh->lastRightSeq) return 0;
        sh->lastRightSeq = m->downSeq;
    } else {
        if (m->downSeq <= sh->lastDownSeq) return 0;
        sh->lastDownSeq = m->downSeq;
    }

    const int w = sh->width;
    ShellLayout L;
    shell_geometry(sh, m, w, &L);

    if (lx >= L.startX && lx < L.startX + L.startW) {
        out->kind = VM_SHELL_ACTION_START;
        out->x = lx;
        out->y = ly;
        return 1;
    }

    /* 显示桌面按钮：全部最小化 / 再次点击还原（XP 语义）。 */
    if (L.showDesktopW > 0 && lx >= L.showDesktopX && lx < L.showDesktopX + L.showDesktopW) {
        int anyVisible = 0;
        for (int i = 0; i < L.nVis; i++) {
            if (!m->items[L.idx[i]].minimized) anyVisible = 1;
        }
        out->kind = VM_SHELL_ACTION_TOGGLE_DESKTOP;
        out->minimizeAll = anyVisible;
        out->x = lx;
        out->y = ly;
        return 1;
    }

    const int bh = shell_item_height(sh);
    const int by = bh >= sh->height ? 0 : (sh->height - bh) / 2;
    for (int i = 0; i < L.nVis; i++) {
        int ix, iw;
        shell_item_rect(&L, i, &ix, &iw);
        if (ix + iw > L.itemsRight) break;
        if (lx >= ix && lx < ix + iw && ly >= by && ly < by + bh) {
            const VmShellItem *it = &m->items[L.idx[i]];
            out->winId = it->id;
            out->x = lx;
            out->y = ly;
            if (button == 2) {
                out->kind = VM_SHELL_ACTION_CONTEXT_MENU;
                return 1;
            }
            /* 激活语义与桌面系统一致：最小化的还原、最大化的先还原尺寸、
               已聚焦的收起为最小化、其余聚焦到前面。 */
            out->kind = VM_SHELL_ACTION_ACTIVATE;
            if (it->minimized) {
                out->doRestore = 1;
            } else if (it->maximized) {
                out->doUnmaximize = 1;
                out->doFocus = 1;
            } else if (it->focused) {
                out->doMinimize = 1;
            } else {
                out->doFocus = 1;
            }
            return 1;
        }
    }

    /* 任务栏空白处：左键只消费防穿透；右键是属性菜单（RetroBar 行为）。 */
    if (button == 2) {
        out->kind = VM_SHELL_ACTION_SETTINGS;
        out->x = lx;
        out->y = ly;
        return 1;
    }
    return 1;
}

VmShell *VmShell_New(const VmTheme *theme, int width, int height) {
    if (width <= 0) width = 800;
    if (height <= 0) height = VM_SHELL_DEFAULT_HEIGHT;
    VmShell *sh = (VmShell *)calloc(1, sizeof(VmShell));
    if (!sh) return NULL;
    sh->theme = theme ? theme : VmTheme_Default();
    sh->width = width;
    sh->height = height;
    sh->showClock = 1;
    sh->showTrayIcon = 1;
    return sh;
}

void VmShell_Delete(VmShell *shell) {
    free(shell);
}

/* 纯设置：只改配置字段。单进程适配层在自己换主题/选项后负责作废窗口。 */
void VmShell_SetTheme(VmShell *shell, const VmTheme *theme) {
    if (!shell) return;
    shell->theme = theme ? theme : VmTheme_Default();
}

void VmShell_SetShowClock(VmShell *shell, int show) {
    if (!shell) return;
    shell->showClock = show ? 1 : 0;
}

int VmShell_ShowClock(const VmShell *shell) {
    return shell ? shell->showClock : 0;
}

void VmShell_SetShowTrayIcon(VmShell *shell, int show) {
    if (!shell) return;
    shell->showTrayIcon = show ? 1 : 0;
}

int VmShell_ShowTrayIcon(const VmShell *shell) {
    return shell ? shell->showTrayIcon : 0;
}

int VmShell_Width(const VmShell *shell) {
    return shell ? shell->width : 0;
}

int VmShell_Height(const VmShell *shell) {
    return shell ? shell->height : 0;
}
