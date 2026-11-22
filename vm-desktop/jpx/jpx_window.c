#include "jpx_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jpx_clay_error(Clay_ErrorData data) {
    fwrite(data.errorText.chars, 1, data.errorText.length, stderr);
}

static Clay_Dimensions jpx_measure(Clay_StringSlice text, Clay_TextElementConfig *config,
                                   void *userData) {
    (void)userData;
    if (VmFont_IsLoaded(config->fontId)) {
        VmFontMetrics m = VmFont_Measure(config->fontId, config->fontSize,
                                         text.chars, text.length);
        return (Clay_Dimensions){(float)m.width, (float)m.lineStep};
    }
    return (Clay_Dimensions){(float)text.length * (float)config->fontSize * 0.6f,
                             (float)config->fontSize};
}

static Clay_RenderCommandArray window_layout_cb(void *userData);

static void window_close_cb(int windowId, void *userData) {
    (void)windowId;
    JpxWindow *w = (JpxWindow *)userData;
    if (w && w->desc.onClose) w->desc.onClose(w->id, w->desc.user);
}

JpxEditSlot *Jpx__EditSlot(JpxWindow *win, const char *id) {
    if (!win || !id) return NULL;
    for (int i = 0; i < JPX_MAX_TEXT_INPUTS; i++) {
        if (win->edits[i].used && strcmp(win->edits[i].id, id) == 0) return &win->edits[i];
    }
    for (int i = 0; i < JPX_MAX_TEXT_INPUTS; i++) {
        if (win->edits[i].used) continue;
        win->edits[i].used = 1;
        snprintf(win->edits[i].id, sizeof(win->edits[i].id), "%s", id);
        return &win->edits[i];
    }
    return NULL;
}

void Jpx__FocusRegister(JpxWindow *win, const char *id, size_t *len, JpxTextEdit *ed) {
    if (!win || !id) return;
    for (int i = 0; i < win->focusableCount; i++) {   /* 已登记则只更新指针 */
        if (strcmp(win->focusables[i].id, id) == 0) {
            win->focusables[i].len = len;
            win->focusables[i].ed = ed;
            return;
        }
    }
    if (win->focusableCount >= JPX_MAX_FOCUSABLE) return;
    JpxFocusable *f = &win->focusables[win->focusableCount++];
    snprintf(f->id, sizeof(f->id), "%s", id);
    f->len = len;
    f->ed = ed;
}

void Jpx__FocusTab(JpxWindow *win, int dir) {
    if (!win || win->focusableCount == 0) return;
    int cur = -1;
    for (int i = 0; i < win->focusableCount; i++) {
        if (strcmp(win->focusables[i].id, win->focusId) == 0) {
            cur = i;
            break;
        }
    }
    int next;
    if (cur < 0) {
        next = dir > 0 ? 0 : win->focusableCount - 1;
    } else {
        next = (cur + dir + win->focusableCount) % win->focusableCount;
    }
    snprintf(win->focusId, sizeof(win->focusId), "%s", win->focusables[next].id);
    /* 从别处切入输入框时把 caret 放到末尾，避免 caret 停在旧位置。 */
    JpxTextEdit *ed = win->focusables[next].ed;
    if (ed) Jpx__TextEditSetCaret(ed, ed->len);
}

int Jpx__FocusKey(JpxWindow *win, const char *key, int ctrl, int shift) {
    if (!win || !key || !win->focusId[0]) return 0;
    for (int i = 0; i < win->focusableCount; i++) {
        if (strcmp(win->focusables[i].id, win->focusId) != 0) continue;
        JpxTextEdit *ed = win->focusables[i].ed;
        if (!ed) return 0;
        if (!Jpx__TextEditKey(ed, key, ctrl, shift)) return 0;
        if (win->focusables[i].len) *win->focusables[i].len = ed->len;
        return 1;
    }
    return 0;
}

/* 滚轮只驱动列表滚动：命中哪个列表由 jpx 侧按上一帧矩形判定，
   窗口里没有列表时返回 0，事件不被消费。 */
static int window_wheel_cb(int windowId, int x, int y, int deltaY, void *userData) {
    JpxWindow *w = (JpxWindow *)userData;
    if (!w || !w->app) return 0;
    return Jpx__ListWheel(w->app, windowId, x, y, deltaY);
}

static int window_key_cb(int windowId, const char *key, const char *code, int isDown,
                         void *userData) {
    JpxWindow *w = (JpxWindow *)userData;
    if (!w) return 0;
    JpxApp *app = w->app;
    VmWm_KeyModifiers(app->wm, &app->ctrl, &app->shift, &app->alt);

    if (isDown) {
        /* 焦点控件先吃：文本编辑消费掉大多数可打印键与编辑键。 */
        /* 键事件只影响当前窗口：不能走 Jpx__Invalidate 的全量路径，
           否则一次按键会让所有窗口重跑布局。 */
        if (Jpx__FocusKey(w, key, app->ctrl, app->shift)) {
            app->caretOn = 1;
            app->dirty = 1;
            VmWm_InvalidateWindow(app->wm, w->id);
            return 1;
        }
        /* Tab 在窗口内控件间循环，Shift 反向；无可聚焦控件时交给应用。 */
        if (strcmp(key, "Tab") == 0 && w->focusableCount > 0) {
            Jpx__FocusTab(w, app->shift ? -1 : 1);
            app->caretOn = 1;
            app->dirty = 1;
            VmWm_InvalidateWindow(app->wm, w->id);
            return 1;
        }
    }

    if (!w->desc.onKey) return 0;
    return w->desc.onKey(windowId, key, code, isDown, app->ctrl, app->shift, app->alt,
                         w->desc.user);
}

JpxWindow *Jpx__WindowCreate(JpxApp *app, const JpxWindowDesc *desc) {
    if (!app || !app->wm || !desc || !desc->layout) return NULL;
    int slot = -1;
    for (int i = 0; i < VM_WM_MAX_WINDOWS - 1; i++) {   /* 留一格给任务栏 */
        if (!app->windows[i].used) { slot = i; break; }
    }
    if (slot < 0) return NULL;

    JpxWindow *w = &app->windows[slot];
    memset(w, 0, sizeof(*w));
    w->app = app;
    w->id = app->nextWinId++;
    w->desc = *desc;
    snprintf(w->title, sizeof(w->title), "%s", desc->title ? desc->title : "window");

    const int bw = app->wm->chrome.borderWidth;
    const int th = app->wm->chrome.titleHeight;
    int bodyW = desc->w - 2 * bw;
    int bodyH = desc->h - th - bw;
    if (bodyW < 1) bodyW = 1;
    if (bodyH < 1) bodyH = 1;

    uint64_t total = Clay_MinMemorySize();
    w->arenaMem = malloc((size_t)total);
    if (!w->arenaMem) return NULL;
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(total, w->arenaMem);
    w->ctx = Clay_Initialize(arena, (Clay_Dimensions){(float)bodyW, (float)bodyH},
                             (Clay_ErrorHandler){jpx_clay_error});
    if (!w->ctx) { free(w->arenaMem); w->arenaMem = NULL; return NULL; }
    Clay_SetCurrentContext(w->ctx);
    Clay_SetMeasureTextFunction(jpx_measure, NULL);

    VmWindow *vw = VmWm_AddWindow(app->wm, w->id, w->title, desc->x, desc->y,
                                  desc->w, desc->h, window_layout_cb, w);
    if (!vw) {
        free(w->arenaMem);
        w->arenaMem = NULL;
        return NULL;
    }
    vw->clayCtx = w->ctx;
    VmWm_SetCloseHandler(app->wm, w->id, window_close_cb, w);
    VmWm_SetKeyHandler(app->wm, w->id, window_key_cb, w);
    VmWm_SetWheelHandler(app->wm, w->id, window_wheel_cb, w);
    w->used = 1;
    app->dirty = 1;
    return w;
}

void Jpx__WindowDestroy(JpxApp *app, int winId) {
    if (!app) return;
    JpxWindow *w = Jpx__Window(app, winId);
    if (!w) return;
    VmWm_RemoveWindow(app->wm, winId);
    free(w->arenaMem);
    memset(w, 0, sizeof(*w));
    app->dirty = 1;
}

static Clay_RenderCommandArray window_layout_cb(void *userData) {
    JpxWindow *w = (JpxWindow *)userData;
    JpxApp *app = w->app;
    Clay_SetCurrentContext(w->ctx);
    Clay_BeginLayout();
    app->localSerial = 0;
    w->focusableCount = 0;   /* 可聚焦控件每帧按声明序重新登记 */

    /* 按下沿由 WM 的指针状态显式推导：Clay 的 onHover 回调在窗口上下文里
       不可靠地触发，控件交互统一用这个边沿 + Clay_Hovered() 判定。 */
    int downNow = 0;
    if (app->wm->pointerDown && app->wm->pointerValid) {
        VmWindow *hit = VmWm_HitTest(app->wm, app->wm->pointerX, app->wm->pointerY);
        downNow = (hit && hit->id == w->id);
    }
    w->pressEdge = downNow && !w->pointerDownLast;
    w->pointerDownLast = downNow;

    Jpx__OpenRoot(app, w);
    int prev = app->currentWin;
    int prevLayout = app->layoutWinId;
    app->currentWin = w->id;
    app->layoutWinId = w->id;
    if (w->desc.layout) w->desc.layout(w->id, w->desc.user);
    app->layoutWinId = prevLayout;
    app->currentWin = prev;
    Clay__CloseElement();

    Clay_RenderCommandArray cmds = Clay_EndLayout(0.016f);
    /* 元素矩形只有 layout 完成后才查得到：滚轮命中判定依赖它，
       放在 Begin 里查会永远拿到 found=false（首帧之后才有值）。 */
    Jpx__ListRefreshRects(app, w->id);
    return cmds;
}
