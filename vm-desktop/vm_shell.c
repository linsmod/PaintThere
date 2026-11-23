#include "vm_shell.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm_font.h"

/* 任务栏 = 一个无装饰/置顶/不可拖动/不抢焦点的特殊窗口。
   内容用 VmWm_SetWindowPaint 手绘：Clay 的 backgroundColor 只吃单色，
   XP 任务栏需要多 stop 渐变与位图填充。命中用 VmWm_SetWindowClickHandler
   （NOFOCUS 窗口拿不到焦点，只能走按下通道）。 */

#define SHELL_START_W 78
#define SHELL_ITEM_W 110
#define SHELL_TRAY_W 76
#define SHELL_PAD 3

struct VmShell {
    VmWm *wm;
    int width, height;
    const VmTheme *theme;
    VmShellStartHandler startHandler;
    void *startUserData;
    /* 已处理过的按下序号：一次按下只触发一次，与重建帧数无关。 */
    int lastDownSeq;
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

/* 绘制与命中共用的几何：开始按钮固定宽，条目按创建序排列。 */
typedef struct {
    int startX, startW;
    int itemsX;
    int n;
    int ids[VM_WM_MAX_WINDOWS];
} ShellLayout;

static void shell_geometry(VmShell *sh, ShellLayout *L) {
    memset(L, 0, sizeof(*L));
    L->startX = SHELL_PAD;
    L->startW = SHELL_START_W;
    L->itemsX = L->startX + L->startW + SHELL_PAD;
    L->n = VmWm_EnumerateCreated(sh->wm, L->ids, VM_WM_MAX_WINDOWS);
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

    /* 开始按钮：有位图皮肤优先用位图（XP 的竖排三态取上 1/3），
       其次是渐变（s_start），最后退回强调色单色。 */
    const int by = y + 2;
    const int bh = h - 4;
    if (t->startImageId) {
        char imgRef[160];
        snprintf(imgRef, sizeof(imgRef), "%s#0,0,1,0.3333", t->startImageId);
        append_img(out, "tb_start", x + L.startX, by, L.startW, bh, imgRef, 4);
    } else {
        const char *startPaint = t->startFrom ? "s_start" : t->accentColor;
        append_rect(out, "tb_start", x + L.startX, by, L.startW, bh, startPaint,
                    t->startFrom ? 4.0f : 6.0f);
    }
    {
        const char *label = "开始";
        int tw = VmFont_Width(fontId, fontSize, label, -1);
        const char *color = t->startTextColor ? t->startTextColor : t->accentTextColor;
        int labelX = x + L.startX + (L.startW - tw) / 2;
        /* 有徽标时文字右移，给图标留出位置（XP 的徽标在左、文字在右）。 */
        if (t->startIconId) {
            int icon = bh - 8;
            append_img(out, "tb_start_icon", x + L.startX + 6, by + (bh - icon) / 2,
                       icon, icon, t->startIconId, 0);
            labelX = x + L.startX + 6 + icon + 4;
        }
        append_text(out, "tb_start_label", labelX, by + (bh - lineStep) / 2,
                    tw > 0 ? tw : L.startW, lineStep, label, fontId, fontSize, color);
    }

    /* 任务条目：按创建序固定排列，标题超宽按像素截断。 */
    int ix = x + L.itemsX;
    for (int i = 0; i < L.n; i++) {
        if (!shell_item_visible(sh, L.ids[i])) continue;
        VmWindow *win = VmWm_FindWindow(sh->wm, L.ids[i]);
        if (!win) continue;
        const char *paint;
        if (win->focused && t->taskButtonActiveFrom) paint = "s_task_active";
        else if (t->taskButtonFrom) paint = "s_task";
        else paint = win->focused ? t->accentColor : t->fgColor;

        char idbuf[48];
        snprintf(idbuf, sizeof(idbuf), "tb_item_%d", win->id);
        append_rect(out, idbuf, ix, by, SHELL_ITEM_W, bh, paint, 3.0f);

        /* XP 条目有 1px 深色内边 + 亮色内线；没有皮肤时留空。 */
        if (t->taskButtonFrom) {
            snprintf(idbuf, sizeof(idbuf), "tb_item_edge_%d", win->id);
            append_border(out, idbuf, ix, by, SHELL_ITEM_W, bh, 1, "rgba(0,0,0,0.28)");
        }

        char label[128];
        truncate_utf8(fontId, fontSize, win->title, SHELL_ITEM_W - 14, label, sizeof(label));
        snprintf(idbuf, sizeof(idbuf), "tb_item_text_%d", win->id);
        const char *color = t->taskButtonTextColor ? t->taskButtonTextColor : t->accentTextColor;
        append_text(out, idbuf, ix + 7, by + (bh - lineStep) / 2,
                    SHELL_ITEM_W - 14, lineStep, label, fontId, fontSize, color);
        ix += SHELL_ITEM_W + SHELL_PAD;
    }

    /* 右侧托盘：XP 有独立浅色区，时钟落在其中。 */
    int trayX = x + w - SHELL_TRAY_W;
    if (t->trayFrom) {
        append_rect(out, "tb_tray", trayX, y, SHELL_TRAY_W, h, "s_tray", 0.0f);
        append_border(out, "tb_tray_edge", trayX, y, 1, h, 1, "rgba(0,0,0,0.22)");
    }
    {
        static char clock[16];
        time_t now = time(NULL);
        struct tm *tmVal = localtime(&now);
        clock[0] = '\0';
        if (tmVal) strftime(clock, sizeof(clock), "%H:%M", tmVal);
        int cw = VmFont_Width(fontId, fontSize, clock, -1);
        const char *color = t->clockTextColor ? t->clockTextColor : t->fgColor;
        append_text(out, "tb_clock", trayX + (SHELL_TRAY_W - cw) / 2,
                    y + (h - lineStep) / 2, cw > 0 ? cw : SHELL_TRAY_W, lineStep,
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

static int shell_click(int winId, int x, int y, void *userData) {
    (void)winId;
    VmShell *sh = (VmShell *)userData;
    if (!sh) return 0;
    VmWindow *self = VmWm_FindWindow(sh->wm, VM_SHELL_WINDOW_ID);
    if (!self) return 0;
    if (x < self->x || x >= self->x + self->w ||
        y < self->y || y >= self->y + self->h) return 0;
    /* 按下沿：同一次按下只处理一次，与后续重建帧数无关。 */
    if (sh->wm->pointerDownSeq == sh->lastDownSeq) return 0;
    sh->lastDownSeq = sh->wm->pointerDownSeq;

    const int lx = x - self->x;
    ShellLayout L;
    shell_geometry(sh, &L);

    if (lx >= L.startX && lx < L.startX + L.startW) {
        if (sh->startHandler) sh->startHandler(sh->startUserData);
        return 1;
    }

    int ix = L.itemsX;
    for (int i = 0; i < L.n; i++) {
        if (!shell_item_visible(sh, L.ids[i])) continue;
        if (lx >= ix && lx < ix + SHELL_ITEM_W) {
            shell_activate_window(sh, L.ids[i]);
            return 1;
        }
        ix += SHELL_ITEM_W + SHELL_PAD;
    }
    /* 点在任务栏空白处也消费，避免穿透到桌面窗口。 */
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

VmShell *VmShell_Create(VmWm *wm, const VmTheme *theme, int height) {
    if (!wm) return NULL;
    if (height <= 0) height = VM_SHELL_DEFAULT_HEIGHT;

    VmShell *sh = (VmShell *)calloc(1, sizeof(VmShell));
    if (!sh) return NULL;
    sh->wm = wm;
    sh->theme = theme ? theme : VmTheme_Default();
    sh->width = wm->width;
    sh->height = height;

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
