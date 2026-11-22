#include "vm_shell.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_font.h"

/* 任务栏 = 一个无装饰/置顶/不可拖动/不抢焦点的特殊窗口。
   z 序、命中、Clay 指针、键盘全部复用 WM 现成路径，WM 不需要 panel 区概念。 */

struct VmShell {
    VmWm *wm;
    Clay_Context *ctx;
    void *arenaMem;
    int width, height;
    const VmTheme *theme;
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
    if (pointerData.state != CLAY_POINTER_DATA_PRESSED_THIS_FRAME) return;
    if (!g_shell) return;
    int wid = (int)(intptr_t)userData;
    VmWindow *win = VmWm_FindWindow(g_shell->wm, wid);
    if (!win) return;
    if (win->minimized) VmWm_Restore(g_shell->wm, wid);
    else VmWm_Focus(g_shell->wm, wid);
}

static Clay_RenderCommandArray shell_layout(void *userData) {
    VmShell *sh = (VmShell *)userData;
    Clay_BeginLayout();
    CLAY(CLAY_ID("taskbar"), {
        .layout = {
            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
            .padding = CLAY_PADDING_ALL(4),
            .childGap = 4,
            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 0},
    }) {
        int ids[VM_WM_MAX_WINDOWS];
        int n = VmWm_Enumerate(sh->wm, ids, VM_WM_MAX_WINDOWS);
        for (int i = 0; i < n; i++) {
            VmWindow *win = VmWm_FindWindow(sh->wm, ids[i]);
            if (!win || win->id == VM_SHELL_WINDOW_ID) continue;
            Clay_Color bg;
            Clay_Color fg;
            if (win->focused) {
                bg = (Clay_Color){52, 140, 220, 255};
                fg = (Clay_Color){255, 255, 255, 255};
            } else if (win->minimized) {
                bg = (Clay_Color){255, 255, 255, 16};
                fg = (Clay_Color){255, 255, 255, 140};
            } else {
                bg = (Clay_Color){255, 255, 255, 36};
                fg = (Clay_Color){255, 255, 255, 215};
            }
            CLAY(CLAY_IDI("tb_item", (uint32_t)win->id), {
                .layout = {
                    .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED((float)(sh->height - 8))},
                    .padding = {8, 6, 0, 0},
                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                },
                .backgroundColor = bg,
                .cornerRadius = CLAY_CORNER_RADIUS(4),
            }) {
                Clay_OnHover(shell_item_press, (void *)(intptr_t)win->id);
                Clay_String label = {
                    .length = (int32_t)strlen(win->title ? win->title : ""),
                    .chars = win->title ? win->title : "",
                };
                CLAY_TEXT(label, CLAY_TEXT_CONFIG({
                    .fontId = 0,
                    .fontSize = 12,
                    .textColor = fg,
                }));
            }
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
    VmWm_Invalidate(shell->wm);
}
