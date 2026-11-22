#include "vm_shell.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_font.h"

#include <time.h>

/* 任务栏 = 一个无装饰/置顶/不可拖动/不抢焦点的特殊窗口。
   z 序、命中、Clay 指针、键盘全部复用 WM 现成路径，WM 不需要 panel 区概念。 */

struct VmShell {
    VmWm *wm;
    Clay_Context *ctx;
    void *arenaMem;
    int width, height;
    const VmTheme *theme;
    VmShellStartHandler startHandler;
    void *startUserData;
    /* 已处理过的按下序号：一次按下只触发一次，与重建帧数无关。 */
    int lastDownSeq;
};

/* Clay_OnHover 只回传 void*，单进程一个任务栏，用静态回指取 wm。 */
static VmShell *g_shell;

static void shell_clay_error(Clay_ErrorData data) {
    fwrite(data.errorText.chars, 1, data.errorText.length, stderr);
}

static Clay_Dimensions shell_measure(Clay_StringSlice text, Clay_TextElementConfig *config,
                                     void *userData) {
    (void)userData;
    VmFontMetrics m = VmFont_Measure(config->fontId, config->fontSize, text.chars, text.length);
    return (Clay_Dimensions){(float)m.width, (float)m.lineStep};
}

static void shell_item_press(Clay_ElementId id, Clay_PointerData pointerData, void *userData) {
    (void)id;
    (void)pointerData;
    if (!g_shell) return;
    VmWm *wm = g_shell->wm;
    if (!wm->pointerDown) return;
    /* 按下沿由 WM 的按下序号推导，不用 Clay 的 PRESSED_THIS_FRAME：
       后者只在有指针事件的帧推进，同一次按下会在后续重建帧里重复触发
       （表现为「开始菜单开了立刻又关」）。 */
    if (wm->pointerDownSeq == g_shell->lastDownSeq) return;
    g_shell->lastDownSeq = wm->pointerDownSeq;
    /* -1 是开始按钮：回调交给宿主，shell 不打开任何窗口。 */
    intptr_t tag = (intptr_t)userData;
    if (tag < 0) {
        if (g_shell->startHandler) g_shell->startHandler(g_shell->startUserData);
        return;
    }
    int wid = (int)tag;
    VmWindow *win = VmWm_FindWindow(g_shell->wm, wid);
    if (!win) return;
    if (win->minimized) VmWm_Restore(g_shell->wm, wid);
    else VmWm_Focus(g_shell->wm, wid);
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

static Clay_RenderCommandArray shell_layout(void *userData) {
    VmShell *sh = (VmShell *)userData;
    Clay_BeginLayout();
    CLAY(CLAY_ID("taskbar"), {
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
            .padding = {6, 6, 4, 4},
            .childGap = 6,
            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 0},
    }) {
        /* 开始按钮：任务栏只发回调，菜单由宿主当作普通窗口管理。 */
        CLAY(CLAY_ID("tb_start"), {
            .layout = {
                .sizing = {CLAY_SIZING_FIXED(72), CLAY_SIZING_FIXED((float)(sh->height - 8))},
                .padding = {10, 6, 0, 0},
                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
            },
            .backgroundColor = Clay_Hovered()
                ? (Clay_Color){124, 122, 255, 230}
                : (Clay_Color){124, 122, 255, 160},
            .cornerRadius = CLAY_CORNER_RADIUS(6),
        }) {
            Clay_OnHover(shell_item_press, (void *)(intptr_t)-1);
            Clay_String label = {.length = 6, .chars = "开始"};
            CLAY_TEXT(label, CLAY_TEXT_CONFIG({
                .fontId = 0, .fontSize = 13,
                .textColor = (Clay_Color){255, 255, 255, 255},
            }));
        }

        int ids[VM_WM_MAX_WINDOWS];
        /* 条目按创建序固定排列：z 序会随聚焦/raise 变化，条目会在点击时来回跳。 */
        int n = VmWm_EnumerateCreated(sh->wm, ids, VM_WM_MAX_WINDOWS);
        for (int i = 0; i < n; i++) {
            VmWindow *win = VmWm_FindWindow(sh->wm, ids[i]);
            if (!win || win->id == VM_SHELL_WINDOW_ID) continue;
            /* 桌面图标层与弹出菜单是无装饰窗口，不占任务栏条目。 */
            if (win->flags & VM_WIN_UNDECORATED) continue;
            Clay_Color bg;
            Clay_Color fg;
            if (win->focused) {
                bg = (Clay_Color){124, 122, 255, 210};
                fg = (Clay_Color){255, 255, 255, 255};
            } else if (win->minimized) {
                bg = (Clay_Color){255, 255, 255, 12};
                fg = (Clay_Color){255, 255, 255, 110};
            } else {
                bg = (Clay_Color){255, 255, 255, 30};
                fg = (Clay_Color){226, 231, 240, 220};
            }
            CLAY(CLAY_IDI("tb_item", (uint32_t)win->id), {
                .layout = {
                    .sizing = {CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED((float)(sh->height - 8))},
                    .padding = {8, 6, 0, 0},
                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                },
                /* 单行 + 水平裁剪：标题比条目长时截断，不能换行撑出任务栏。 */
                .clip = {.horizontal = true, .childOffset = {0, 0}},
                .backgroundColor = bg,
                .cornerRadius = CLAY_CORNER_RADIUS(5),
            }) {
                Clay_OnHover(shell_item_press, (void *)(intptr_t)win->id);
                Clay_String label = {
                    .length = (int32_t)strlen(win->title ? win->title : ""),
                    .chars = win->title ? win->title : "",
                };
                CLAY_TEXT(label, CLAY_TEXT_CONFIG({
                    .fontId = 0, .fontSize = 12, .textColor = fg,
                    .wrapMode = CLAY_TEXT_WRAP_NONE,
                }));
            }
        }

        /* 右侧时钟：宿主每秒调 VmShell_Refresh 驱动重绘。
           Clay 保留字符串指针到 EndLayout 之后，必须是 static。 */
        static char clock[16] = {0};
        time_t now = time(NULL);
        struct tm *tmVal = localtime(&now);
        if (tmVal) strftime(clock, sizeof(clock), "%H:%M", tmVal);
        CLAY(CLAY_ID("tb_clock"), {
            .layout = {
                .sizing = {CLAY_SIZING_FIXED(56), CLAY_SIZING_FIXED((float)(sh->height - 8))},
                .padding = {8, 6, 0, 0},
                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER},
            },
        }) {
            Clay_String label = {.length = (int32_t)strlen(clock), .chars = clock};
            CLAY_TEXT(label, CLAY_TEXT_CONFIG({
                .fontId = 0, .fontSize = 13,
                .textColor = (Clay_Color){226, 231, 240, 235},
            }));
        }
    }
    return Clay_EndLayout(0.016f);
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

    uint64_t total = Clay_MinMemorySize();
    sh->arenaMem = malloc((size_t)total);
    if (!sh->arenaMem) { free(sh); return NULL; }
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(total, sh->arenaMem);
    sh->ctx = Clay_Initialize(arena, (Clay_Dimensions){(float)sh->width, (float)height},
                              (Clay_ErrorHandler){shell_clay_error});
    if (!sh->ctx) { free(sh->arenaMem); free(sh); return NULL; }
    Clay_SetCurrentContext(sh->ctx);
    Clay_SetMeasureTextFunction(shell_measure, NULL);

    int y = wm->height - height;
    VmWindow *win = VmWm_AddWindow(wm, VM_SHELL_WINDOW_ID, "taskbar",
                                   0, y, sh->width, height, shell_layout, sh);
    if (!win) { free(sh->arenaMem); free(sh); return NULL; }
    win->clayCtx = sh->ctx;
    win->bodyStyleId = sh->theme->panelStyleId;
    VmWm_SetWindowFlags(wm, VM_SHELL_WINDOW_ID,
                        VM_WIN_UNDECORATED | VM_WIN_TOPMOST | VM_WIN_PINNED | VM_WIN_NOFOCUS);

    g_shell = sh;
    /* 订阅窗口集合/焦点变化：条目与高亮跟着立即更新。 */
    VmWm_SetWindowListHandler(wm, shell_windows_changed, sh);
    return sh;
}

void VmShell_Destroy(VmShell *shell) {
    if (!shell) return;
    VmWm_RemoveWindow(shell->wm, VM_SHELL_WINDOW_ID);
    if (g_shell == shell) g_shell = NULL;
    free(shell->arenaMem);
    free(shell);
}

void VmShell_Refresh(VmShell *shell) {
    if (!shell) return;
    /* 只有任务栏自己在变：窗口列表由 layout 每帧重读，不需要作废其他窗口。 */
    VmWm_InvalidateWindow(shell->wm, VM_SHELL_WINDOW_ID);
}
