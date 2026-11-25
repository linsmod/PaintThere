/*
 * main.c - vm_shell 入口（桌面外壳进程）
 *
 * 职责（docs/task-vm-wms-merge.md §8 M5.1）：
 *   本进程只做「窗口列表快照 + 任务栏内容 + 输入命中」，
 *   通过 VMWM_CLIENT_SHELL 连 vm_wms：建任务栏窗口、推内容、收输入与指针。
 *   不合成、不切瓦片、不持窗口表权威，也不链接 vm_wm / Clay。
 *
 * 用法：
 *   vm_shell --pipe \\.\pipe\vmwm_dev --font assets/font.ttf
 *
 * 线程模型：与 vm_rdp 相同。WmsClient 自带 loop，主循环里用
 * Wms_PollEvents(c, 0) 轮询；16 ms timer 保证空闲时也会醒来。
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uv.h"

#include "wms_client.h"
#include "wms_ipc.h"

#include "jpx_xaml.h"
#include "jpx_xaml_backend_cmdbuf.h"

#include "vm_display.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "vm_font.h"
#include "vm_menu_view.h"
#include "vm_shell.h"
#include "vm_theme.h"

#define SHELL_LOG(fmt, ...)                                              \
    do {                                                                 \
        fprintf(stderr, "[shell %5u] " fmt,                              \
                (unsigned)(uv_hrtime() / 1000000ull) % 100000u,          \
                ##__VA_ARGS__);                                          \
        fputc('\n', stderr);                                             \
        fflush(stderr);                                                  \
    } while (0)

static uv_loop_t  g_loop;
static uv_timer_t g_timer;
static WmsClient *g_wms;
static const char *g_pipe = VMWM_PIPE_NAME;
static int         g_stop;

static VmShell     *g_shell;
static VmShellModel g_model;
static uint64_t     g_taskbar;
static int32_t      g_bodyW, g_bodyH;
static int32_t      g_screenW, g_screenH;
static uint32_t     g_epoch;
static int          g_dirty;
static int          g_resync;
static int          g_clockMinute = -1;

/* 弹出菜单（任务栏右键）：独立窗口，内容由 VmMenuView 产出。 */
static VmMenuView   g_menu;
static uint64_t     g_menuWin;
static uint32_t     g_menuEpoch;
static int32_t      g_menuBodyW, g_menuBodyH;
static int          g_menuOpen;
static int          g_menuDirty;
static int          g_menuTarget;
static int          g_menuAction[VM_MENU_VIEW_MAX_ITEMS];

/* 事件回调里不能发起同步 WMS 请求：client_call 会重入 uv_run。
   回调只记录意图，主循环统一执行。 */
static int          g_pendingCloseMenu;
static int          g_pendingOpenMenu;
static int          g_pendingMenuTarget, g_pendingMenuX, g_pendingMenuY;
static int          g_pendingMenuAct, g_pendingMenuActTarget;
static VmShellAction g_pendingShellAct;
static int          g_hasPendingShellAct;

/* 开始菜单：XAML（Direct 后端）描述的固定条目。 */
static JpxXamlView *g_startView;
static uint64_t     g_startWin;
static uint32_t     g_startEpoch;
static int32_t      g_startBodyW, g_startBodyH;
static int          g_startOpen;
static int          g_startDirty;
static int          g_pendingOpenStart;
static int          g_pendingCloseStart;
static int          g_pendingOpenSettings;
static int          g_pendingCloseSettings;
static VmDrawCommand g_startCmds[VM_DISPLAY_MAX_COMMANDS];

static const char kStartXaml[] =
    "<Window>"
    "  <StackPanel Orientation='Vertical' Gap='2'>"
    "    <Button Name='settings' Content='设置' Height='28' Click='OnItem' />"
    "    <Button Name='showdesktop' Content='显示桌面' Height='28' Click='OnItem' />"
    "    <Separator Height='6' />"
    "    <Button Name='about' Content='关于 vm_shell' Height='28' Click='OnItem' />"
    "  </StackPanel>"
    "</Window>";

/* 设置面板：普通装饰窗口 + XAML 表单。 */
typedef struct {
    unsigned char showClock, showTray;
} SettingsModel;

static JpxXamlView *g_settingsView;
static uint64_t     g_settingsWin;
static uint32_t     g_settingsEpoch;
static int32_t      g_settingsBodyW, g_settingsBodyH;
static int          g_settingsOpen;
static int          g_settingsDirty;
static SettingsModel g_settingsModel;
static VmDrawCommand g_settingsCmds[VM_DISPLAY_MAX_COMMANDS];

/* 无装饰窗口：wms 的装饰按钮在远程模式下还没有处理者（见 issues），
   所以设置面板自带标题与关闭按钮，避免点了 × 没反应。 */
static const char kSettingsXaml[] =
    "<Window>"
    "  <StackPanel Orientation='Vertical' Gap='8'>"
    "    <TextBlock Text='外壳设置' Height='20' />"
    "    <CheckBox Name='clock' Content='显示时钟' Height='24'"
    "              IsChecked='{Binding showClock}' Click='OnToggle' />"
    "    <CheckBox Name='tray' Content='显示托盘图标' Height='24'"
    "              IsChecked='{Binding showTray}' Click='OnToggle' />"
    "    <Button Name='close' Content='关闭' Height='26' Width='80' Click='OnClose' />"
    "  </StackPanel>"
    "</Window>";

enum {
    MENU_ACT_NONE = 0,
    MENU_ACT_ACTIVATE,
    MENU_ACT_MINIMIZE,
    MENU_ACT_CLOSE,
    MENU_ACT_SETTINGS
};

/* 内容推送缓冲：VmDisplayList 量级，不能放栈。 */
static VmDrawCommand g_cmds[VM_DISPLAY_MAX_COMMANDS];
static VmDrawCommand g_menuCmds[VM_DISPLAY_MAX_COMMANDS];
static const VmTheme *g_theme;

static const VmShellItem *find_item(int winId) {
    for (int i = 0; i < g_model.count; i++) {
        if (g_model.items[i].id == winId) return &g_model.items[i];
    }
    return NULL;
}

/* ---------------- 窗口快照 ---------------- */

static int cmp_created(const void *a, const void *b) {
    const VmShellItem *x = (const VmShellItem *)a;
    const VmShellItem *y = (const VmShellItem *)b;
    return x->createdSeq - y->createdSeq;
}

/* 全量重拉：窗口数 ≤ 10，增量合并的错误代价比重新枚举高，M5.1 先全量。 */
static void resync_windows(void) {
    VmWmWindowInfo list[VMWM_MAX_WINDOWS];
    int n = g_wms ? Wms_ListWindows(g_wms, list, VMWM_MAX_WINDOWS) : 0;
    g_model.count = 0;
    for (int i = 0; i < n && g_model.count < VM_SHELL_MAX_ITEMS; i++) {
        const VmWmWindowInfo *wi = &list[i];
        VmShellItem *it = &g_model.items[g_model.count++];
        it->id = (int)wi->win_id;
        snprintf(it->title, sizeof(it->title), "%s", wi->title);
        it->focused = wi->focused ? 1 : 0;
        it->minimized = wi->mapped ? 0 : 1;
        it->maximized = 0;   /* wmwms 不管理最大化状态 */
        it->undecorated = (wi->flags & VMWM_WINFLAG_UNDECORATED) != 0;
        it->createdSeq = (int)wi->created_seq;
    }
    qsort(g_model.items, (size_t)g_model.count, sizeof(VmShellItem), cmp_created);
    SHELL_LOG("resync: %d windows", g_model.count);
    for (int i = 0; i < g_model.count; i++) {
        const VmShellItem *it = &g_model.items[i];
        SHELL_LOG("  item id=%d '%s' undec=%d min=%d focused=%d",
                  it->id, it->title, it->undecorated, it->minimized, it->focused);
    }
    g_dirty = 1;
}

/* ---------------- 内容推送 ---------------- */

static void push_content(void) {
    VmCmdBuffer buf;
    int accepted = 0, overflow = 0;
    VmWmResult r;

    if (!g_wms || !g_taskbar || g_epoch == 0) return;   /* 等首个 layout_req */
    buf.commands = g_cmds;
    buf.cap = (int)(sizeof(g_cmds) / sizeof(g_cmds[0]));
    buf.count = 0;
    buf.rejected = 0;

    VmShell_Paint(g_shell, &g_model, &buf, 0, 0, g_bodyW, g_bodyH);

    r = Wms_PushContent(g_wms, g_taskbar, g_epoch, g_cmds,
                        (uint32_t)buf.count, (uint32_t)sizeof(VmDrawCommand),
                        &accepted, &overflow);
    SHELL_LOG("push content count=%d accepted=%d overflow=%d -> %s",
              buf.count, accepted, overflow, Wms_StrError(r));
    if (r == VMWM_OK && accepted) g_dirty = 0;
}

/* ---------------- 弹出菜单 ---------------- */

static void push_menu(void) {
    VmCmdBuffer buf;
    int accepted = 0, overflow = 0;
    VmWmResult r;
    if (!g_wms || !g_menuWin || g_menuEpoch == 0) return;
    buf.commands = g_menuCmds;
    buf.cap = (int)(sizeof(g_menuCmds) / sizeof(g_menuCmds[0]));
    buf.count = 0;
    buf.rejected = 0;
    VmMenuView_Paint(&g_menu, &buf);
    r = Wms_PushContent(g_wms, g_menuWin, g_menuEpoch, g_menuCmds, (uint32_t)buf.count,
                        (uint32_t)sizeof(VmDrawCommand), &accepted, &overflow);
    SHELL_LOG("menu push count=%d accepted=%d -> %s", buf.count, accepted, Wms_StrError(r));
    if (r == VMWM_OK && accepted) g_menuDirty = 0;
}

static void close_menu(void) {
    if (!g_menuOpen && !g_menuWin) return;
    if (g_wms && g_menuWin) Wms_CloseWindow(g_wms, g_menuWin);
    SHELL_LOG("menu close");
    g_menuOpen = 0;
    g_menuWin = 0;
    g_menuEpoch = 0;
    g_menuDirty = 0;
    g_menu.count = 0;
}

static void apply_menu(int act) {
    uint64_t id = (uint64_t)g_menuTarget;
    switch (act) {
        case MENU_ACT_ACTIVATE:
            Wms_MapWindow(g_wms, id);
            Wms_StackWindow(g_wms, id, VMWM_STACK_TOP);
            Wms_SetFocus(g_wms, id);
            SHELL_LOG("menu activate win=%d", g_menuTarget);
            break;
        case MENU_ACT_MINIMIZE:
            Wms_UnmapWindow(g_wms, id);
            SHELL_LOG("menu minimize win=%d", g_menuTarget);
            break;
        case MENU_ACT_CLOSE:
            Wms_CloseWindow(g_wms, id);
            SHELL_LOG("menu close win=%d", g_menuTarget);
            break;
        case MENU_ACT_SETTINGS:
            SHELL_LOG("settings requested (panel not migrated yet)");
            break;
        default:
            break;
    }
}

static void open_menu(int target, int ax, int ay) {
    VmMenuItem items[5];
    int n = 0;
    const VmShellItem *it;
    VmWmResult r;

    if (!g_wms || !g_theme) return;
    if (g_menuOpen) close_menu();

    g_menuTarget = target;
    it = find_item(target);
    if (it) {
        items[n].text = it->minimized ? "还原" : "激活";
        items[n].enabled = 1;
        g_menuAction[n++] = MENU_ACT_ACTIVATE;
        items[n].text = "最小化";
        items[n].enabled = 1;
        g_menuAction[n++] = MENU_ACT_MINIMIZE;
        items[n].text = NULL;
        items[n].enabled = 0;
        g_menuAction[n++] = MENU_ACT_NONE;
        items[n].text = "关闭";
        items[n].enabled = 1;
        g_menuAction[n++] = MENU_ACT_CLOSE;
    } else {
        items[n].text = "属性";
        items[n].enabled = 1;
        g_menuAction[n++] = MENU_ACT_SETTINGS;
    }

    VmMenuView_Layout(&g_menu, g_theme, items, n, ax, ay, g_screenW, g_screenH);
    g_menuOpen = 1;
    g_menuWin = 0;
    g_menuEpoch = 0;
    g_menuDirty = 0;

    r = Wms_CreateWindow(g_wms, g_menu.x, g_menu.y, g_menu.w, g_menu.h,
                         VMWM_WINFLAG_UNDECORATED | VMWM_WINFLAG_TOPMOST |
                         VMWM_WINFLAG_PINNED | VMWM_WINFLAG_NOFOCUS,
                         &g_menuWin);
    if (r != VMWM_OK) {
        SHELL_LOG("menu create failed (%s)", Wms_StrError(r));
        g_menuOpen = 0;
        g_menuWin = 0;
        return;
    }
    Wms_SetTitle(g_wms, g_menuWin, "Menu");
    Wms_SetClassHint(g_wms, g_menuWin, "VmShell.Menu");
    Wms_MapWindow(g_wms, g_menuWin);
    SHELL_LOG("menu open target=%d at (%d,%d) size=%dx%d",
              target, g_menu.x, g_menu.y, g_menu.w, g_menu.h);
}

static int inside_menu(int x, int y) {
    return g_menuOpen && x >= g_menu.x && x < g_menu.x + g_menu.w &&
           y >= g_menu.y && y < g_menu.y + g_menu.h;
}

/* ---------------- 开始菜单（XAML Direct 后端） ---------------- */

static void on_start_item(JpxXamlView *view, int nodeIndex, void *userData) {
    const JpxXamlNode *n = JpxXamlView_Node(view, nodeIndex);
    const char *name = n ? JpxXamlNode_Attr(n, "Name") : NULL;
    (void)userData;
    if (!name || !*name) return;

    if (!strcmp(name, "settings")) {
        g_pendingOpenSettings = 1;
    } else if (!strcmp(name, "showdesktop")) {
        int anyVisible = 0;
        for (int i = 0; i < g_model.count; i++) {
            const VmShellItem *it = &g_model.items[i];
            if (it->undecorated || it->id == (int)g_taskbar) continue;
            if (!it->minimized) anyVisible = 1;
        }
        memset(&g_pendingShellAct, 0, sizeof(g_pendingShellAct));
        g_pendingShellAct.kind = VM_SHELL_ACTION_TOGGLE_DESKTOP;
        g_pendingShellAct.minimizeAll = anyVisible;
        g_hasPendingShellAct = 1;
    } else {
        SHELL_LOG("start item '%s'", name);
    }
    g_pendingCloseStart = 1;
}

static void push_start(void) {
    VmCmdBuffer buf;
    int accepted = 0, overflow = 0;
    VmWmResult r;
    if (!g_wms || !g_startView || !g_startWin || g_startEpoch == 0) return;
    buf.commands = g_startCmds;
    buf.cap = (int)(sizeof(g_startCmds) / sizeof(g_startCmds[0]));
    buf.count = 0;
    buf.rejected = 0;
    JpxXaml_RenderDirect(g_startView, JpxXamlBackend_CmdBuf(), &buf,
                         0, 0, g_startBodyW, g_startBodyH);
    r = Wms_PushContent(g_wms, g_startWin, g_startEpoch, g_startCmds, (uint32_t)buf.count,
                        (uint32_t)sizeof(VmDrawCommand), &accepted, &overflow);
    SHELL_LOG("start push count=%d accepted=%d -> %s", buf.count, accepted, Wms_StrError(r));
    if (r == VMWM_OK && accepted) g_startDirty = 0;
}

static void close_start(void) {
    if (!g_startOpen && !g_startWin) return;
    if (g_wms && g_startWin) Wms_CloseWindow(g_wms, g_startWin);
    SHELL_LOG("start menu close");
    g_startOpen = 0;
    g_startWin = 0;
    g_startEpoch = 0;
    g_startDirty = 0;
}

static void open_start(void) {
    char err[256];
    const int w = 200, h = 132;
    int y;
    VmWmResult r;

    if (!g_wms) return;
    if (!g_startView) {
        g_startView = JpxXamlView_Create(NULL, kStartXaml, NULL, err, sizeof(err));
        if (!g_startView) {
            SHELL_LOG("start menu parse failed: %s", err);
            return;
        }
        JpxXamlView_On(g_startView, "OnItem", on_start_item, NULL);
    }
    if (g_startOpen) close_start();

    y = g_screenH - VmShell_Height(g_shell) - h;
    if (y < 0) y = 0;
    g_startOpen = 1;
    g_startWin = 0;
    g_startEpoch = 0;
    g_startDirty = 0;

    r = Wms_CreateWindow(g_wms, 0, y, w, h,
                         VMWM_WINFLAG_UNDECORATED | VMWM_WINFLAG_TOPMOST |
                         VMWM_WINFLAG_PINNED | VMWM_WINFLAG_NOFOCUS,
                         &g_startWin);
    if (r != VMWM_OK) {
        SHELL_LOG("start menu create failed (%s)", Wms_StrError(r));
        g_startOpen = 0;
        g_startWin = 0;
        return;
    }
    Wms_SetTitle(g_wms, g_startWin, "StartMenu");
    Wms_SetClassHint(g_wms, g_startWin, "VmShell.StartMenu");
    Wms_MapWindow(g_wms, g_startWin);
    SHELL_LOG("start menu open at (0,%d) size=%dx%d", y, w, h);
}

static int inside_start(int x, int y) {
    return g_startOpen && g_startWin && x >= 0 && x < 200 &&
           y >= (g_screenH - VmShell_Height(g_shell) - 132) &&
           y < (g_screenH - VmShell_Height(g_shell));
}

/* ---------------- 设置面板 ---------------- */

static void on_settings_toggle(JpxXamlView *view, int nodeIndex, void *userData) {
    const JpxXamlNode *n = JpxXamlView_Node(view, nodeIndex);
    (void)userData;
    /* 复选框已经把值写回 model；这里应用到任务栏并重推。 */
    VmShell_SetShowClock(g_shell, g_settingsModel.showClock ? 1 : 0);
    VmShell_SetShowTrayIcon(g_shell, g_settingsModel.showTray ? 1 : 0);
    g_dirty = 1;
    SHELL_LOG("settings toggle name='%s' clock=%d tray=%d",
              n ? JpxXamlNode_Attr(n, "Name") : "?",
              g_settingsModel.showClock, g_settingsModel.showTray);
}

static void close_settings(void) {
    if (!g_settingsOpen && !g_settingsWin) return;
    if (g_wms && g_settingsWin) Wms_CloseWindow(g_wms, g_settingsWin);
    SHELL_LOG("settings close");
    g_settingsOpen = 0;
    g_settingsWin = 0;
    g_settingsEpoch = 0;
    g_settingsDirty = 0;
}

static void on_settings_close(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    g_pendingCloseSettings = 1;
}

static void push_settings(void) {
    VmCmdBuffer buf;
    int accepted = 0, overflow = 0;
    VmWmResult r;
    if (!g_wms || !g_settingsView || !g_settingsWin || g_settingsEpoch == 0) return;
    buf.commands = g_settingsCmds;
    buf.cap = (int)(sizeof(g_settingsCmds) / sizeof(g_settingsCmds[0]));
    buf.count = 0;
    buf.rejected = 0;
    JpxXaml_RenderDirect(g_settingsView, JpxXamlBackend_CmdBuf(), &buf,
                         0, 0, g_settingsBodyW, g_settingsBodyH);
    r = Wms_PushContent(g_wms, g_settingsWin, g_settingsEpoch, g_settingsCmds,
                        (uint32_t)buf.count, (uint32_t)sizeof(VmDrawCommand),
                        &accepted, &overflow);
    SHELL_LOG("settings push count=%d accepted=%d -> %s",
              buf.count, accepted, Wms_StrError(r));
    if (r == VMWM_OK && accepted) g_settingsDirty = 0;
}

static void open_settings(void) {
    char err[256];
    VmWmResult r;
    if (!g_wms) return;

    if (!g_settingsView) {
        g_settingsView = JpxXamlView_Create(NULL, kSettingsXaml, &g_settingsModel,
                                            err, sizeof(err));
        if (!g_settingsView) {
            SHELL_LOG("settings parse failed: %s", err);
            return;
        }
        JpxXamlView_Bind(g_settingsView, "showClock", JPX_XAML_BIND_BOOL,
                         offsetof(SettingsModel, showClock), 1);
        JpxXamlView_Bind(g_settingsView, "showTray", JPX_XAML_BIND_BOOL,
                         offsetof(SettingsModel, showTray), 1);
        JpxXamlView_On(g_settingsView, "OnToggle", on_settings_toggle, NULL);
        JpxXamlView_On(g_settingsView, "OnClose", on_settings_close, NULL);
    }

    g_settingsModel.showClock = (unsigned char)VmShell_ShowClock(g_shell);
    g_settingsModel.showTray = (unsigned char)VmShell_ShowTrayIcon(g_shell);

    if (g_settingsOpen && g_settingsWin) {
        Wms_StackWindow(g_wms, g_settingsWin, VMWM_STACK_TOP);
        Wms_SetFocus(g_wms, g_settingsWin);
        return;
    }

    g_settingsOpen = 1;
    g_settingsWin = 0;
    g_settingsEpoch = 0;
    g_settingsDirty = 0;
    r = Wms_CreateWindow(g_wms, 240, 150, 280, 180,
                         VMWM_WINFLAG_UNDECORATED | VMWM_WINFLAG_TOPMOST,
                         &g_settingsWin);
    if (r != VMWM_OK) {
        SHELL_LOG("settings create failed (%s)", Wms_StrError(r));
        g_settingsOpen = 0;
        g_settingsWin = 0;
        return;
    }
    Wms_SetTitle(g_wms, g_settingsWin, "外壳设置");
    Wms_SetClassHint(g_wms, g_settingsWin, "VmShell.Settings");
    Wms_MapWindow(g_wms, g_settingsWin);
    SHELL_LOG("settings open");
}

/* ---------------- 动作执行 ---------------- */

static void apply_action(const VmShellAction *a) {
    switch (a->kind) {
        case VM_SHELL_ACTION_START:
            g_pendingOpenStart = 1;
            break;
        case VM_SHELL_ACTION_SETTINGS:
        case VM_SHELL_ACTION_CONTEXT_MENU:
            /* 建菜单窗口是同步请求，交给主循环（事件回调里会重入 uv_run）。 */
            g_pendingMenuTarget = a->kind == VM_SHELL_ACTION_CONTEXT_MENU ? a->winId : 0;
            g_pendingMenuX = a->x;
            g_pendingMenuY = g_screenH - VmShell_Height(g_shell) + a->y;
            g_pendingOpenMenu = 1;
            break;
        case VM_SHELL_ACTION_TOGGLE_DESKTOP: {
            int n = 0;
            for (int i = 0; i < g_model.count; i++) {
                const VmShellItem *it = &g_model.items[i];
                if (it->undecorated || it->id == (int)g_taskbar) continue;
                if (a->minimizeAll) Wms_UnmapWindow(g_wms, (uint64_t)it->id);
                else Wms_MapWindow(g_wms, (uint64_t)it->id);
                n++;
            }
            SHELL_LOG("toggle desktop %s %d windows", a->minimizeAll ? "minimize" : "restore", n);
            break;
        }
        case VM_SHELL_ACTION_ACTIVATE: {
            uint64_t id = (uint64_t)a->winId;
            if (a->doRestore) Wms_MapWindow(g_wms, id);
            if (a->doMinimize) Wms_UnmapWindow(g_wms, id);
            if (a->doFocus) {
                Wms_StackWindow(g_wms, id, VMWM_STACK_TOP);
                Wms_SetFocus(g_wms, id);
            }
            SHELL_LOG("activate win=%d restore=%d minimize=%d focus=%d",
                      a->winId, a->doRestore, a->doMinimize, a->doFocus);
            break;
        }
        default:
            break;
    }
}

/* ---------------- WMS 事件 ---------------- */

static void on_wms_event(WmsClient *c, const WmsEvent *e, void *user) {
    (void)c;
    (void)user;

    switch (e->evt) {
    case EVT_WINDOW_CLOSE: {
        const VmWmWindowInfo *info = Wms_AsWindowInfo(e);
        if (g_settingsOpen && info && info->win_id == g_settingsWin) {
            SHELL_LOG("settings closed");
            g_settingsOpen = 0;
            g_settingsWin = 0;
            g_settingsEpoch = 0;
            g_settingsDirty = 0;
        }
        g_resync = 1;
        break;
    }
    case EVT_WINDOW_NEW:
    case EVT_WINDOW_CONFIGURE:
    case EVT_FOCUS_CHANGE:
    case EVT_TITLE_CHANGE:
        g_resync = 1;
        break;
    case EVT_POINTER: {
        const VmWmEvtPointer *p = Wms_AsPointer(e);
        if (!p) break;
        if (p->kind == 1) SHELL_LOG("pointer down (%d,%d)", (int)p->x, (int)p->y);
        /* 菜单开着：在菜单内更新 hover；按在菜单外就关掉（Windows 行为）。
           关闭动作放到主循环，避免在事件回调里发同步请求。 */
        if (g_menuOpen) {
            if (p->down && !inside_menu(p->x, p->y)) {
                g_pendingCloseMenu = 1;
                break;
            }
            if (VmMenuView_SetHover(&g_menu, p->x, p->y)) g_menuDirty = 1;
        }
        if (g_startOpen && p->down && !inside_start(p->x, p->y)) {
            g_pendingCloseStart = 1;
            break;
        }
        g_model.pointerX = p->x;
        g_model.pointerY = p->y;
        g_model.pointerValid = 1;
        if (p->down && !g_model.pointerDown) g_model.downSeq++;
        g_model.pointerDown = (int)p->down;
        g_dirty = 1;
        break;
    }
    case EVT_INPUT: {
        const VmWmReqInput *in = Wms_AsInput(e);
        VmShellAction a;
        if (!in) break;
        if (in->kind == 1 && in->is_down)
            SHELL_LOG("input down win=%llu (%d,%d) button=%d",
                      (unsigned long long)in->win_id, (int)in->x, (int)in->y, (int)in->button);
        /* 设置窗口：down/up 都交给 XAML Direct 交互层（click 在 up 时成立）。 */
        if (g_settingsOpen && in->win_id == g_settingsWin) {
            if (g_settingsView && (in->kind == 1 || in->kind == 2))
                JpxXaml_DirectPointer(g_settingsView, in->x, in->y, in->kind == 1);
            g_settingsDirty = 1;
            g_dirty = 1;
            break;
        }
        if (in->kind != 1 || !in->is_down) break;
        /* 菜单点击优先：body 局部 → 画布坐标再命中；执行放主循环。 */
        if (g_menuOpen && in->win_id == g_menuWin) {
            int idx = VmMenuView_Hit(&g_menu, g_menu.x + in->x, g_menu.y + in->y);
            g_pendingMenuAct = idx >= 0 ? g_menuAction[idx] : MENU_ACT_NONE;
            g_pendingMenuActTarget = g_menuTarget;
            g_pendingCloseMenu = 1;
            g_dirty = 1;
            break;
        }
        /* 开始菜单点击：交给 XAML Direct 交互层，动作在主循环执行。 */
        if (g_startOpen && in->win_id == g_startWin) {
            if (in->kind == 1 && in->is_down && g_startView) {
                JpxXaml_DirectPointer(g_startView, in->x, in->y, 1);
                JpxXaml_DirectPointer(g_startView, in->x, in->y, 0);
                g_startDirty = 1;
            }
            g_dirty = 1;
            break;
        }
        /* 服务端已转成 body 局部坐标；任务栏无装饰，body 即整窗。 */
        if (VmShell_HitTest(g_shell, &g_model, in->x, in->y,
                            in->button == 2 ? 2 : 0, 1, &a)) {
            SHELL_LOG("input id=%llu kind=1 down=1 (%d,%d) -> action %d",
                      (unsigned long long)in->win_id, in->x, in->y, (int)a.kind);
            g_pendingShellAct = a;
            g_hasPendingShellAct = 1;
            g_dirty = 1;
        }
        break;
    }
    case EVT_LAYOUT_REQ: {
        const VmWmEvtLayoutReq *lr = Wms_AsLayoutReq(e);
        if (!lr) break;
        /* 菜单窗口的 layout_req 先于 CreateWindow 返回，按"菜单开着且还没拿到
           win_id"认领；它必然不是任务栏。 */
        if (g_menuOpen && g_menuWin == 0 && lr->win_id != g_taskbar) {
            g_menuWin = lr->win_id;
            g_menuEpoch = lr->epoch;
            g_menuBodyW = lr->width;
            g_menuBodyH = lr->height;
            g_menuDirty = 1;
            SHELL_LOG("menu layout epoch=%u body=%dx%d",
                      (unsigned)lr->epoch, lr->width, lr->height);
            break;
        }
        if (g_settingsOpen && g_settingsWin == 0 && lr->win_id != g_taskbar &&
            lr->win_id != g_menuWin && lr->win_id != g_startWin) {
            g_settingsWin = lr->win_id;
            g_settingsEpoch = lr->epoch;
            g_settingsBodyW = lr->width;
            g_settingsBodyH = lr->height;
            g_settingsDirty = 1;
            SHELL_LOG("settings layout epoch=%u body=%dx%d",
                      (unsigned)lr->epoch, lr->width, lr->height);
            break;
        }
        if (g_startOpen && g_startWin == 0 && lr->win_id != g_taskbar &&
            lr->win_id != g_menuWin) {
            g_startWin = lr->win_id;
            g_startEpoch = lr->epoch;
            g_startBodyW = lr->width;
            g_startBodyH = lr->height;
            g_startDirty = 1;
            SHELL_LOG("start layout epoch=%u body=%dx%d",
                      (unsigned)lr->epoch, lr->width, lr->height);
            break;
        }
        /* 服务端在建窗请求的处理里就发了 layout_req，那时 Wms_CreateWindow
           还没返回，g_taskbar 仍是 0。外壳只建一个窗口，所以第一条
           layout_req 必然属于任务栏，直接认领。 */
        if (g_taskbar == 0) g_taskbar = lr->win_id;
        if (lr->win_id == g_taskbar) {
            g_bodyW = lr->width;
            g_bodyH = lr->height;
            g_epoch = lr->epoch;
            SHELL_LOG("taskbar layout epoch=%u body=%dx%d",
                      (unsigned)lr->epoch, g_bodyW, g_bodyH);
            g_dirty = 1;
        }
        break;
    }
    case EVT_THEME_CHANGED: {
        const VmWmReqTheme *t = Wms_AsTheme(e);
        /* M5.1 只有默认主题；带名字的主题解析在后续接。 */
        SHELL_LOG("theme changed -> '%s' (only default applied)", t ? t->name : "");
        VmShell_SetTheme(g_shell, VmTheme_Default());
        g_dirty = 1;
        break;
    }
    default:
        break;
    }
}

/* ---------------- 连接与主循环 ---------------- */

static int connect_wms(void) {
    VmWmResult r;
    const VmTheme *theme;

    g_wms = Wms_ClientCreate(g_pipe, VMWM_CLIENT_SHELL);
    if (!g_wms) return 0;
    Wms_SetEventCallback(g_wms, on_wms_event, NULL);

    r = Wms_ClientConnect(g_wms);
    if (r != VMWM_OK) {
        SHELL_LOG("wms connect failed (%s)", Wms_StrError(r));
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
        return 0;
    }

    r = Wms_Subscribe(g_wms, EVT_WINDOW_NEW | EVT_WINDOW_CLOSE | EVT_WINDOW_CONFIGURE |
                             EVT_FOCUS_CHANGE | EVT_TITLE_CHANGE |
                             EVT_LAYOUT_REQ | EVT_INPUT | EVT_POINTER |
                             EVT_THEME_CHANGED);
    if (r != VMWM_OK) {
        SHELL_LOG("wms subscribe failed (%s)", Wms_StrError(r));
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
        return 0;
    }

    int32_t sw = Wms_ScreenWidth(g_wms);
    int32_t sh = Wms_ScreenHeight(g_wms);
    int h;
    theme = VmTheme_Default();

    g_theme = theme;
    g_screenW = sw;
    g_screenH = sh;

    h = theme->panelHeight > 0 ? theme->panelHeight : VM_SHELL_DEFAULT_HEIGHT;
    g_shell = VmShell_New(theme, sw, h);
    if (!g_shell) return 0;

    /* 工作区 = 画布减去任务栏。 */
    Wms_SetWorkArea(g_wms, 0, 0, sw, sh - h);

    r = Wms_CreateWindow(g_wms, 0, sh - h, sw, h,
                         VMWM_WINFLAG_UNDECORATED | VMWM_WINFLAG_TOPMOST |
                         VMWM_WINFLAG_PINNED | VMWM_WINFLAG_NOFOCUS,
                         &g_taskbar);
    if (r != VMWM_OK) {
        SHELL_LOG("create taskbar failed (%s)", Wms_StrError(r));
        return 0;
    }
    Wms_SetTitle(g_wms, g_taskbar, "Taskbar");
    Wms_SetClassHint(g_wms, g_taskbar, "VmShell");
    Wms_MapWindow(g_wms, g_taskbar);

    SHELL_LOG("wms connected pipe='%s' client_id=%u screen=%dx%d taskbar=%llu h=%d",
              g_pipe, (unsigned)Wms_ClientId(g_wms), (int)sw, (int)sh,
              (unsigned long long)g_taskbar, h);

    g_bodyW = sw;
    g_bodyH = h;
    g_resync = 1;
    return 1;
}

static void disconnect_wms(void) {
    if (g_wms) {
        Wms_ClientDestroy(g_wms);
        g_wms = NULL;
    }
}

static void on_timer(uv_timer_t *t) {
    (void)t;
    if (!g_wms || !Wms_ClientIsConnected(g_wms)) return;

    /* 时钟每分钟变一次：强制重建一次任务栏，不依赖窗口事件。 */
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    int minute = tmv ? tmv->tm_min : -1;
    if (minute != g_clockMinute) {
        g_clockMinute = minute;
        g_dirty = 1;
    }

    if (g_resync) {
        g_resync = 0;
        resync_windows();
    }
    /* 本轮的同步 WMS 请求都在这里做，事件回调只记录意图。 */
    if (g_pendingCloseMenu) {
        g_pendingCloseMenu = 0;
        close_menu();
    }
    if (g_pendingOpenMenu) {
        g_pendingOpenMenu = 0;
        open_menu(g_pendingMenuTarget, g_pendingMenuX, g_pendingMenuY);
    }
    if (g_pendingMenuAct != MENU_ACT_NONE) {
        int act = g_pendingMenuAct;
        g_pendingMenuAct = MENU_ACT_NONE;
        g_menuTarget = g_pendingMenuActTarget;
        apply_menu(act);
    }
    if (g_hasPendingShellAct) {
        g_hasPendingShellAct = 0;
        apply_action(&g_pendingShellAct);
    }
    if (g_pendingCloseStart) {
        g_pendingCloseStart = 0;
        close_start();
    }
    if (g_pendingOpenStart) {
        g_pendingOpenStart = 0;
        open_start();
    }
    if (g_pendingCloseSettings) {
        g_pendingCloseSettings = 0;
        close_settings();
    }
    if (g_pendingOpenSettings) {
        g_pendingOpenSettings = 0;
        open_settings();
    }

    if (g_menuDirty) push_menu();
    if (g_startDirty) push_start();
    if (g_settingsDirty) push_settings();
    if (g_dirty) push_content();
}

int main(int argc, char **argv) {
    const char *fontPath = "assets/font.ttf";
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
            g_pipe = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            fontPath = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: vm_shell [--pipe <path>] [--font <ttf>]\n");
            printf("  default pipe : %s\n", VMWM_PIPE_NAME);
            printf("  protocol     : wms v%u (role SHELL)\n", (unsigned)VMWM_VERSION);
            return 0;
        } else {
            printf("unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (VmFont_LoadFile(fontPath, VmTheme_Default()->fontId) < 0)
        SHELL_LOG("font load failed: %s", fontPath);

    if (uv_loop_init(&g_loop) != 0) {
        printf("uv_loop_init failed\n");
        return 1;
    }

    if (!connect_wms()) {
        SHELL_LOG("startup failed");
        uv_loop_close(&g_loop);
        return 1;
    }

    uv_timer_init(&g_loop, &g_timer);
    /* 4ms tick: pending actions (menu/start/settings) drain in a few ticks,
       not two 16ms rounds. WMS frame cadence stays at its own 16ms. */
    uv_timer_start(&g_timer, on_timer, 4, 4);

    while (!g_stop) {
        /* NOWAIT + poll every iteration: never block on the timer before
           draining WMS events (otherwise pointer/input arrive 1 tick late). */
        uv_run(&g_loop, UV_RUN_NOWAIT);
        if (g_wms) Wms_PollEvents(g_wms, 0);
#ifdef _WIN32
        /* Sleep(1) rounds to ~15.6ms on Windows; yield without the rounding. */
        Sleep(0);
#else
        usleep(200);
#endif
        if (g_wms && !Wms_ClientIsConnected(g_wms)) {
            SHELL_LOG("wms disconnected, exiting (restart the shell to recover)");
            break;
        }
    }

    uv_timer_stop(&g_timer);
    disconnect_wms();
    VmShell_Delete(g_shell);
    uv_close((uv_handle_t *)&g_timer, NULL);
    uv_run(&g_loop, UV_RUN_DEFAULT);
    uv_loop_close(&g_loop);

    SHELL_LOG("exited");
    return 0;
}
