#include "vm_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_menu.h"
#include "vm_shell_internal.h"
#include "vm_wm.h"

/* 单进程适配：把 VmWm 当数据源喂给纯绘制层（vm_shell.c），并把
   VmShellAction 翻译回 VmWm_* 调用。拆进程后这段由 shell/main.c 用
   Wms_* 实现同一张动作表。 */

/* ---- 右键上下文菜单（RetroBar 交互）---- */

enum {
    TASK_MENU_RESTORE = 1,
    TASK_MENU_MAXIMIZE,
    TASK_MENU_MINIMIZE,
    TASK_MENU_CLOSE,
    TASK_MENU_SEP
};

typedef struct {
    VmShellLocal *L;
    int wid;
    int actions[5];
    int count;
} TaskMenuCtx;

/* 菜单单例一次一个，ctx 用静态存储保到回调触发（栈数组会悬空）。 */
static TaskMenuCtx g_taskMenu;

static void task_menu_select(int index, void *userData) {
    TaskMenuCtx *ctx = (TaskMenuCtx *)userData;
    VmShellLocal *L = ctx->L;
    VmWindow *win = VmWm_FindWindow(L->wm, ctx->wid);
    if (!win) return;   /* 菜单打开期间窗口可能已被关掉 */
    switch (ctx->actions[index]) {
        case TASK_MENU_RESTORE:
            if (win->maximized) VmWm_Unmaximize(L->wm, ctx->wid);
            else if (win->minimized) VmWm_Restore(L->wm, ctx->wid);
            break;
        case TASK_MENU_MAXIMIZE:
            VmWm_Maximize(L->wm, ctx->wid);
            break;
        case TASK_MENU_MINIMIZE:
            VmWm_Minimize(L->wm, ctx->wid);
            break;
        case TASK_MENU_CLOSE:
            /* 与装饰 × 同一条路径：只发关闭请求，真正移除由持有者决定。 */
            if (win->closeHandler) win->closeHandler(win->id, win->closeUserData);
            else VmWm_RemoveWindow(L->wm, ctx->wid);
            break;
        default:
            break;
    }
}

static void task_context_menu(VmShellLocal *L, int wid, int cx, int cy) {
    VmWindow *win = VmWm_FindWindow(L->wm, wid);
    if (!win) return;
    VmMenuItem items[5];
    int *actions = g_taskMenu.actions;
    g_taskMenu.L = L;
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

    VmMenu_Open(L->wm, L->shell->theme, cx, cy, items, g_taskMenu.count,
                task_menu_select, &g_taskMenu);
}

static void bar_menu_select(int index, void *userData) {
    VmShellLocal *L = (VmShellLocal *)userData;
    if (index == 0 && L->settingsHandler) L->settingsHandler(L->settingsUserData);
}

/* ---------------- 数据源与动作执行 ---------------- */

static void fill_model(VmShellLocal *L, VmShellModel *m) {
    memset(m, 0, sizeof(*m));
    VmWm *wm = L->wm;
    int ids[VM_WM_MAX_WINDOWS];
    int n = VmWm_EnumerateCreated(wm, ids, VM_WM_MAX_WINDOWS);
    for (int i = 0; i < n && m->count < VM_SHELL_MAX_ITEMS; i++) {
        VmWindow *win = VmWm_FindWindow(wm, ids[i]);
        if (!win) continue;
        VmShellItem *it = &m->items[m->count++];
        it->id = win->id;
        snprintf(it->title, sizeof(it->title), "%s", win->title ? win->title : "");
        it->focused = win->focused;
        it->minimized = win->minimized;
        it->maximized = win->maximized;
        it->undecorated = (win->flags & VM_WIN_UNDECORATED) != 0;
        it->createdSeq = win->createdSeq;
    }
    m->pointerX = wm->pointerX;
    m->pointerY = wm->pointerY;
    m->pointerValid = wm->pointerValid;
    m->pointerDown = wm->pointerDown;
    m->downSeq = wm->pointerDownSeq;
}

static void apply_activate(VmShellLocal *L, const VmShellAction *a) {
    if (!VmWm_FindWindow(L->wm, a->winId)) return;
    if (a->doRestore) VmWm_Restore(L->wm, a->winId);
    if (a->doUnmaximize) VmWm_Unmaximize(L->wm, a->winId);
    if (a->doMinimize) VmWm_Minimize(L->wm, a->winId);
    if (a->doFocus) VmWm_Focus(L->wm, a->winId);
}

static void apply_toggle_desktop(VmShellLocal *L, const VmShellModel *m, int minimizeAll) {
    for (int i = 0; i < m->count; i++) {
        const VmShellItem *it = &m->items[i];
        if (it->undecorated || it->id == VM_SHELL_WINDOW_ID) continue;
        if (minimizeAll) VmWm_Minimize(L->wm, it->id);
        else VmWm_Restore(L->wm, it->id);
    }
}

/* ---------------- 窗口回调 ---------------- */

static void shell_paint_cb(VmCmdBuffer *out, int x, int y, int w, int h, void *userData) {
    VmShellLocal *L = (VmShellLocal *)userData;
    VmShellModel m;
    fill_model(L, &m);
    VmShell_Paint(L->shell, &m, out, x, y, w, h);
}

static int shell_click_cb(int winId, int x, int y, int button, void *userData) {
    (void)winId;
    VmShellLocal *L = (VmShellLocal *)userData;
    VmWindow *self = VmWm_FindWindow(L->wm, VM_SHELL_WINDOW_ID);
    if (!self) return 0;
    if (x < self->x || x >= self->x + self->w ||
        y < self->y || y >= self->y + self->h) return 0;

    VmShellModel m;
    fill_model(L, &m);
    VmShellAction a;
    if (!VmShell_HitTest(L->shell, &m, x - self->x, y - self->y, button, 1, &a))
        return 0;

    const int cx = self->x + a.x;
    const int cy = self->y + a.y;
    switch (a.kind) {
        case VM_SHELL_ACTION_START:
            if (L->startHandler) L->startHandler(L->startUserData);
            break;
        case VM_SHELL_ACTION_SETTINGS: {
            static const VmMenuItem kBarMenu[] = { {"属性", 1} };
            VmMenu_Open(L->wm, L->shell->theme, cx, cy - 1, kBarMenu, 1,
                        bar_menu_select, L);
            break;
        }
        case VM_SHELL_ACTION_CONTEXT_MENU:
            task_context_menu(L, a.winId, cx, cy);
            break;
        case VM_SHELL_ACTION_TOGGLE_DESKTOP:
            apply_toggle_desktop(L, &m, a.minimizeAll);
            break;
        case VM_SHELL_ACTION_ACTIVATE:
            apply_activate(L, &a);
            break;
        default:
            break;
    }
    return 1;
}

/* 窗口集合 / 焦点 / 最小化变化：条目与高亮都要立刻更新，
   不能等每秒时钟刷新兜底（那会有最长 1 秒的不同步）。 */
static void windows_changed(VmWm *wm, void *userData) {
    VmShellLocal *L = (VmShellLocal *)userData;
    if (!wm || !L) return;
    VmWm_InvalidateWindow(wm, VM_SHELL_WINDOW_ID);
}

/* ---------------- 生命周期与配置 ---------------- */

VmShell *VmShell_Create(VmWm *wm, const VmTheme *theme, int height) {
    if (!wm) return NULL;
    if (height <= 0) height = VM_SHELL_DEFAULT_HEIGHT;

    VmShell *sh = VmShell_New(theme, wm->width, height);
    if (!sh) return NULL;
    VmShellLocal *L = (VmShellLocal *)calloc(1, sizeof(VmShellLocal));
    if (!L) { VmShell_Delete(sh); return NULL; }
    L->wm = wm;
    L->shell = sh;
    sh->local = L;

    int y = wm->height - height;
    VmWindow *win = VmWm_AddWindow(wm, VM_SHELL_WINDOW_ID, "taskbar",
                                   0, y, sh->width, height, NULL, L);
    if (!win) { free(L); VmShell_Delete(sh); return NULL; }
    win->bodyStyleId = sh->theme->panelStyleId;
    VmWm_SetWindowFlags(wm, VM_SHELL_WINDOW_ID,
                        VM_WIN_UNDECORATED | VM_WIN_TOPMOST | VM_WIN_PINNED | VM_WIN_NOFOCUS);
    VmWm_SetWindowPaint(wm, VM_SHELL_WINDOW_ID, shell_paint_cb, L);
    VmWm_SetWindowClickHandler(wm, VM_SHELL_WINDOW_ID, shell_click_cb, L);

    /* 工作区 = 画布减去任务栏：最大化与拖动钳制都按它算。 */
    VmWm_SetWorkArea(wm, 0, 0, wm->width, wm->height - height);

    /* 订阅窗口集合/焦点变化：条目与高亮跟着立即更新。 */
    VmWm_SetWindowListHandler(wm, windows_changed, L);
    return sh;
}

void VmShell_Destroy(VmShell *shell) {
    if (!shell) return;
    VmShellLocal *L = shell->local;
    if (L) {
        if (L->wm) VmWm_RemoveWindow(L->wm, VM_SHELL_WINDOW_ID);
        free(L);
        shell->local = NULL;
    }
    VmShell_Delete(shell);
}

void VmShell_SetStartHandler(VmShell *shell, VmShellStartHandler handler, void *userData) {
    if (!shell || !shell->local) return;
    shell->local->startHandler = handler;
    shell->local->startUserData = userData;
}

void VmShell_SetSettingsHandler(VmShell *shell, VmShellSettingsHandler handler,
                                void *userData) {
    if (!shell || !shell->local) return;
    shell->local->settingsHandler = handler;
    shell->local->settingsUserData = userData;
}

void VmShell_Refresh(VmShell *shell) {
    if (!shell || !shell->local || !shell->local->wm) return;
    /* 只有任务栏自己在变：窗口列表由 paint 每帧重读，不需要作废其他窗口。 */
    VmWm_InvalidateWindow(shell->local->wm, VM_SHELL_WINDOW_ID);
}

void VmShell_SetHeight(VmShell *shell, int height) {
    if (!shell || height <= 0 || height == shell->height) return;
    shell->height = height;
    VmShellLocal *L = shell->local;
    if (!L || !L->wm) return;
    VmWindow *win = VmWm_FindWindow(L->wm, VM_SHELL_WINDOW_ID);
    if (!win) return;
    /* 直接改矩形：任务栏钉在底部，不走 VmWm_Move（那会按工作区钳制）。 */
    win->y = L->wm->height - height;
    win->h = height;
    VmWm_SetWorkArea(L->wm, 0, 0, L->wm->width, L->wm->height - height);
    VmShell_Refresh(shell);
}
