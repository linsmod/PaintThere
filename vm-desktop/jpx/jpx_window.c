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

static int window_key_cb(int windowId, const char *key, const char *code, int isDown,
                         void *userData) {
    JpxWindow *w = (JpxWindow *)userData;
    if (!w || !w->desc.onKey) return 0;
    JpxApp *app = w->app;
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
    app->currentWin = w->id;
    if (w->desc.layout) w->desc.layout(w->id, w->desc.user);
    app->currentWin = prev;
    Clay__CloseElement();

    return Clay_EndLayout(0.016f);
}
