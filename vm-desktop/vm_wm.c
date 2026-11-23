#include "vm_wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_font.h"

#ifdef _WIN32
#include <windows.h>
static unsigned long long wm_now_ms(void) { return GetTickCount64(); }
#else
#include <time.h>
static unsigned long long wm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned long long)ts.tv_nsec / 1000000ULL;
}
#endif

#define VM_DOUBLE_CLICK_MS 400

/* White arrow template (same as the WebSocket channel). Frontend tints it via drawImg. */
static const char VM_CURSOR_SVG_B64[] =
    "PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHdpZHRoPScyNCcgaGVpZ2h0PScyNCcgdmlld0JveD0nMCAwIDI0IDI0Jz48cGF0aCBkPSdNNCwyIEw0LDIwIEw5LDE1IEwxMi41LDIyIEwxNS41LDIwLjUgTDEyLDEzLjUgTDIwLDEzIFonIGZpbGw9JyNmZmYnIHN0cm9rZT0nIzAwMCcgc3Ryb2tlLXdpZHRoPScxJy8+PC9zdmc+";

const char *VmWm_CursorDataUrl(void) {
    static char dataUrl[512];
    snprintf(dataUrl, sizeof(dataUrl), "data:image/svg+xml;base64,%s", VM_CURSOR_SVG_B64);
    return dataUrl;
}

VmWm *VmWm_Create(int width, int height) {
    VmWm *wm = (VmWm *)calloc(1, sizeof(VmWm));
    if (!wm) return NULL;
    wm->width = width;
    wm->height = height;
    wm->nextZ = 1;
    wm->nextSeq = 0;
    wm->focusId = -1;
    wm->dragId = -1;
    wm->layoutWinId = -1;
    wm->overflowWinId = -1;
    wm->lastTitleDownId = -1;
    wm->workX = 0;
    wm->workY = 0;
    wm->workW = width;
    wm->workH = height;
    wm->quotaCommands = VM_WM_LAYER_DEFAULT_QUOTA;
    /* 配额可由环境变量调小做压测/演示：VM_WM_LAYER_QUOTA=200 */
    {
        const char *quota = getenv("VM_WM_LAYER_QUOTA");
        if (quota && *quota) {
            int v = atoi(quota);
            if (v < 1) v = 1;
            if (v > VM_WM_LAYER_CAPACITY) v = VM_WM_LAYER_CAPACITY;
            wm->quotaCommands = v;
        }
    }
    wm->focusClientId[0] = '\0';
    wm->chrome = VmTheme_Default()->chrome;
    wm->contentClip = 1;
    wm->dirty = 1;
    return wm;
}

/* 置脏统一走这里：contentDirty 与「原因」一起记，dump 时能直接看出
   某个窗口是谁弄脏的、还是根本没人弄脏。 */
static void layer_dirty(VmWindow *win, const char *why) {
    if (!win) return;
    win->layer.contentDirty = 1;
    win->layer.dirtyWhy = why;
}

static void wm_dirty(VmWm *wm, const char *why) {
    if (!wm) return;
    wm->dirty = 1;
    wm->dirtyWhy = why;
}

/* 全体窗口内容作废：装饰参数或主题变了，没有哪个窗口能幸免。 */
static void mark_all_content_dirty(VmWm *wm, const char *why) {
    for (int i = 0; i < wm->count; i++) layer_dirty(&wm->windows[i], why);
}

/* 窗口列表/焦点/最小化变化：通知订阅者（任务栏据此立即重建）。 */
static void notify_window_list(VmWm *wm) {
    if (wm && wm->listHandler) wm->listHandler(wm, wm->listUserData);
}

void VmWm_SetWindowListHandler(VmWm *wm, VmWmWindowListHandler handler, void *userData) {
    if (!wm) return;
    wm->listHandler = handler;
    wm->listUserData = userData;
    notify_window_list(wm);
}

void VmWm_SetChrome(VmWm *wm, const VmChrome *chrome) {
    if (!wm) return;
    wm->chrome = chrome ? *chrome : VmTheme_Default()->chrome;
    mark_all_content_dirty(wm, "chrome");
    wm_dirty(wm, "chrome");
}

void VmWm_SetContentClip(VmWm *wm, int enabled) {
    if (!wm) return;
    wm->contentClip = enabled ? 1 : 0;
    mark_all_content_dirty(wm, "content-clip");
    wm_dirty(wm, "content-clip");
}

/* 帧级的「有东西变了」但定位不到窗口：保守地把全部内容作废。
   能定位到窗口时用 VmWm_InvalidateWindow，只重建那一个。 */
void VmWm_Invalidate(VmWm *wm) {
    if (!wm) return;
    mark_all_content_dirty(wm, "invalidate-all");
    wm_dirty(wm, "invalidate-all");
}

void VmWm_InvalidateWindow(VmWm *wm, int id) {
    if (!wm) return;
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    layer_dirty(win, "invalidate-window");
    wm_dirty(wm, "invalidate-window");
}

int VmWm_IsDirty(const VmWm *wm) {
    return wm ? wm->dirty : 0;
}

int VmWm_ConsumeDirty(VmWm *wm) {
    if (!wm || !wm->dirty) return 0;
    wm->dirty = 0;
    return 1;
}

void VmWm_Free(VmWm *wm) {
    if (!wm) return;
    /* Clay contexts own arenas allocated in AddWindow; free their memory via free(). */
    for (int i = 0; i < wm->count; i++) {
        if (wm->windows[i].clayCtx) {
            /* Clay has no destroy API; arena memory is tracked by caller in demo. */
        }
    }
    free(wm);
}

static int compare_z(const void *a, const void *b) {
    const VmWindow *wa = (const VmWindow *)a;
    const VmWindow *wb = (const VmWindow *)b;
    return wa->z - wb->z;
}

VmWindow *VmWm_AddWindow(VmWm *wm, int id, const char *title,
                         int x, int y, int w, int h,
                         VmWindowLayout layout, void *userData) {
    if (!wm || wm->count >= VM_WM_MAX_WINDOWS) return NULL;
    VmWindow *win = &wm->windows[wm->count++];
    memset(win, 0, sizeof(*win));
    win->id = id;
    win->title = title;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->layout = layout;
    win->userData = userData;
    win->z = wm->nextZ++;
    win->createdSeq = wm->nextSeq++;
    win->focused = 0;
    win->minimized = 0;
    win->flags = 0;
    layer_dirty(win, "new");   /* 新窗口没有内容，首次合成必须构建 */
    wm_dirty(wm, "add-window");
    notify_window_list(wm);
    return win;
}

void VmWm_RemoveWindow(VmWm *wm, int id) {
    if (!wm) return;
    for (int i = 0; i < wm->count; i++) {
        if (wm->windows[i].id != id) continue;
        for (int j = i + 1; j < wm->count; j++) wm->windows[j - 1] = wm->windows[j];
        wm->count--;
        if (wm->focusId == id) wm->focusId = -1;
        if (wm->dragId == id) wm->dragId = -1;
        wm_dirty(wm, "remove-window");
        notify_window_list(wm);
        return;
    }
}

VmWindow *VmWm_FindWindow(VmWm *wm, int id) {
    if (!wm) return NULL;
    for (int i = 0; i < wm->count; i++) if (wm->windows[i].id == id) return &wm->windows[i];
    return NULL;
}

void VmWm_WindowBody(const VmWm *wm, const VmWindow *win, VmRect *out) {
    if (!out) return;
    if (!win) { *out = (VmRect){0, 0, 0, 0}; return; }
    int bw = wm ? wm->chrome.borderWidth : 2;
    int th = wm ? wm->chrome.titleHeight : VM_WM_TITLE_HEIGHT;
    if (win->flags & VM_WIN_UNDECORATED) {
        *out = (VmRect){(float)win->x, (float)win->y, (float)win->w, (float)win->h};
        return;
    }
    int bw2 = 2 * bw;
    int w = win->w - bw2;
    int h = win->h - th - bw;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    *out = (VmRect){(float)(win->x + bw), (float)(win->y + th), (float)w, (float)h};
}

static int rect_contains(VmRect r, int x, int y) {
    return x >= (int)r.x && x < (int)(r.x + r.w) && y >= (int)r.y && y < (int)(r.y + r.h);
}

/* 标题栏右侧按钮：最小化 / 最大化 / 关闭（从右往左）。
   命中与绘制共用同一份几何，避免两处漂移。 */
static void chrome_button_rects(const VmWm *wm, const VmWindow *win,
                                VmRect *closeRect, VmRect *maxRect, VmRect *minRect,
                                int *sizeOut) {
    int bw = wm->chrome.borderWidth;
    int th = wm->chrome.titleHeight;
    int size = th - 2 * bw - 8;
    if (size > 16) size = 16;
    if (size < 8) size = 0;
    if (sizeOut) *sizeOut = size;
    if (size == 0) {
        *closeRect = (VmRect){0, 0, 0, 0};
        *maxRect = *closeRect;
        *minRect = *closeRect;
        return;
    }
    int by = win->y + bw + (th - bw - size) / 2;
    int closeX = win->x + win->w - bw - 4 - size;
    int maxX = closeX - size - 4;
    int minX = maxX - size - 4;
    *closeRect = (VmRect){(float)closeX, (float)by, (float)size, (float)size};
    *maxRect = (VmRect){(float)maxX, (float)by, (float)size, (float)size};
    *minRect = (VmRect){(float)minX, (float)by, (float)size, (float)size};
}

/* Hit test: 置顶窗口整层优先，其次按 z 从高到低。 */
VmWindow *VmWm_HitTest(VmWm *wm, int x, int y) {
    if (!wm) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        int wantTop = (pass == 0);
        for (int z = wm->nextZ - 1; z >= 1; z--) {
            for (int i = 0; i < wm->count; i++) {
                VmWindow *win = &wm->windows[i];
                if (win->z != z || win->minimized) continue;
                int top = (win->flags & VM_WIN_TOPMOST) ? 1 : 0;
                if (top != wantTop) continue;
                if (x >= win->x && x < win->x + win->w &&
                    y >= win->y && y < win->y + win->h) return win;
            }
        }
    }
    return NULL;
}

void VmWm_Raise(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->z = wm->nextZ++;
    /* 只是前后次序变了：内容一个字没动，合成阶段按新 z 序重排即可。 */
    wm_dirty(wm, "raise");
}

void VmWm_Focus(VmWm *wm, int id) {
    if (!wm) return;
    VmWindow *target = VmWm_FindWindow(wm, id);
    if (target && (target->flags & VM_WIN_NOFOCUS)) return;
    int prev = wm->focusId;
    for (int i = 0; i < wm->count; i++) wm->windows[i].focused = (wm->windows[i].id == id);
    wm->focusId = id;
    VmWm_Raise(wm, id);
    /* 焦点改变标题/边框着色与控件焦点态：旧焦点与新焦点都要重建内容。 */
    for (int i = 0; i < wm->count; i++) {
        if (wm->windows[i].id == id) layer_dirty(&wm->windows[i], "focus-gain");
        else if (wm->windows[i].id == prev) layer_dirty(&wm->windows[i], "focus-lose");
    }
    wm_dirty(wm, "focus");
    /* 任务栏等界面显示焦点态，必须立刻重建，不能等定时刷新兜底。 */
    notify_window_list(wm);
}

void VmWm_SetWindowFlags(VmWm *wm, int id, unsigned flags) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->flags = flags;
    if (flags & VM_WIN_NOFOCUS) {
        if (wm->focusId == id) wm->focusId = -1;
        win->focused = 0;
    }
    /* 装饰性由 flags 决定（UNDECORATED 会去掉标题与边框）：内容必须重建。 */
    layer_dirty(win, "flags");
    wm_dirty(wm, "flags");
}

unsigned VmWm_WindowFlags(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    return win ? win->flags : 0u;
}

void VmWm_SetCloseHandler(VmWm *wm, int id, VmWindowCloseHandler handler, void *userData) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->closeHandler = handler;
    win->closeUserData = userData;
}

int VmWm_Enumerate(VmWm *wm, int *ids, int cap) {
    if (!wm) return 0;
    int n = 0;
    for (int z = 1; z < wm->nextZ; z++) {
        for (int i = 0; i < wm->count; i++) {
            if (wm->windows[i].z != z) continue;
            if (ids && n < cap) ids[n] = wm->windows[i].id;
            n++;
        }
    }
    return n;
}

int VmWm_EnumerateCreated(VmWm *wm, int *ids, int cap) {
    if (!wm) return 0;
    int order[VM_WM_MAX_WINDOWS];
    for (int i = 0; i < wm->count; i++) {
        int seq = wm->windows[i].createdSeq;
        int j = i;
        while (j > 0 && wm->windows[order[j - 1]].createdSeq > seq) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    int n = 0;
    for (int i = 0; i < wm->count; i++) {
        if (ids && n < cap) ids[n] = wm->windows[order[i]].id;
        n++;
    }
    return n;
}

void VmWm_Minimize(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->minimized = 1;
    if (wm->focusId == id) { wm->focusId = -1; win->focused = 0; }
    if (wm->dragId == id) wm->dragId = -1;
    layer_dirty(win, "minimize");   /* 失焦会改标题着色 */
    wm_dirty(wm, "minimize");
    notify_window_list(wm);
}

void VmWm_Restore(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->minimized = 0;
    layer_dirty(win, "restore");
    if (!(win->flags & VM_WIN_NOFOCUS)) VmWm_Focus(wm, id);
    wm_dirty(wm, "restore");
    notify_window_list(wm);
}

int VmWm_IsMinimized(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    return win ? win->minimized : 0;
}

void VmWm_SetWorkArea(VmWm *wm, int x, int y, int w, int h) {
    if (!wm) return;
    if (w <= 0) w = wm->width;
    if (h <= 0) h = wm->height;
    wm->workX = x;
    wm->workY = y;
    wm->workW = w;
    wm->workH = h;
    /* 已最大化的窗口跟着新工作区走（任务栏高度变化时尤其需要）。 */
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        if (!win->maximized) continue;
        win->x = wm->workX;
        win->y = wm->workY;
        win->w = wm->workW;
        win->h = wm->workH;
        layer_dirty(win, "workarea");
    }
    wm_dirty(wm, "workarea");
}

/* 只有普通可拖动窗口能最大化：任务栏、桌面层、弹出菜单都不行。 */
static int can_maximize(const VmWindow *win) {
    if (!win) return 0;
    if (win->flags & (VM_WIN_UNDECORATED | VM_WIN_PINNED | VM_WIN_NOFOCUS)) return 0;
    return 1;
}

int VmWm_Maximize(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!wm || !can_maximize(win) || win->maximized) return 0;
    win->restoreX = win->x;
    win->restoreY = win->y;
    win->restoreW = win->w;
    win->restoreH = win->h;
    win->maximized = 1;
    win->x = wm->workX;
    win->y = wm->workY;
    win->w = wm->workW;
    win->h = wm->workH;
    layer_dirty(win, "maximize");
    wm_dirty(wm, "maximize");
    notify_window_list(wm);
    return 1;
}

int VmWm_Unmaximize(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!wm || !win || !win->maximized) return 0;
    win->maximized = 0;
    win->x = win->restoreX;
    win->y = win->restoreY;
    win->w = win->restoreW;
    win->h = win->restoreH;
    layer_dirty(win, "unmaximize");
    wm_dirty(wm, "unmaximize");
    notify_window_list(wm);
    return 1;
}

int VmWm_ToggleMaximize(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!wm || !win) return 0;
    return win->maximized ? VmWm_Unmaximize(wm, id) : VmWm_Maximize(wm, id);
}

int VmWm_IsMaximized(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    return win ? win->maximized : 0;
}

int VmWm_FocusId(const VmWm *wm) {
    return wm ? wm->focusId : -1;
}

void VmWm_Move(VmWm *wm, int id, int x, int y) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    if (win->maximized) return;   /* 最大化时不可拖动 */
    /* 钳制到工作区：左边/上边不许整窗拖出屏幕，右边/下边至少留出可抓的区域。 */
    int minX = wm->workX - win->w + 60;
    int maxX = wm->workX + wm->workW - 60;
    int minY = wm->workY;
    int maxY = wm->workY + wm->workH - wm->chrome.titleHeight;
    if (x < minX) x = minX;
    if (x > maxX) x = maxX;
    if (y < minY) y = minY;
    if (y > maxY) y = maxY;
    if (win->x == x && win->y == y) return;
    win->x = x;
    win->y = y;
    /* 内容命令在生成时就带了 body 偏移，位置一变每条坐标都过期：必须重建。 */
    layer_dirty(win, "move");
    wm_dirty(wm, "move");
}

void VmWm_KeyModifiers(VmWm *wm, int *ctrl, int *shift, int *alt) {
    if (ctrl) *ctrl = wm ? wm->keyCtrl : 0;
    if (shift) *shift = wm ? wm->keyShift : 0;
    if (alt) *alt = wm ? wm->keyAlt : 0;
}

void VmWm_SetKeyHandler(VmWm *wm, int id, VmWindowKeyHandler handler, void *userData) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->keyHandler = handler;
    win->keyUserData = userData;
}

void VmWm_SetWheelHandler(VmWm *wm, int id, VmWindowWheelHandler handler, void *userData) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->wheelHandler = handler;
    win->wheelUserData = userData;
}

void VmWm_SetWindowPaint(VmWm *wm, int id, VmWindowPaint paint, void *userData) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->paint = paint;
    if (userData) win->userData = userData;
    layer_dirty(win, "paint");
    wm_dirty(wm, "set-paint");
}

void VmWm_SetWindowClickHandler(VmWm *wm, int id, VmWindowClickHandler handler,
                                void *userData) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->clickHandler = handler;
    win->clickUserData = userData;
}

/* --- tiny JSON field extractors (input events are simple flat objects) --- */
static const char *find_key(const char *json, const char *key) {
    static char scratch[128];
    snprintf(scratch, sizeof(scratch), "\"%s\"", key);
    return strstr(json, scratch);
}

static int extract_int(const char *json, const char *key, int fallback) {
    const char *p = find_key(json, key);
    if (!p) return fallback;
    p = strchr(p, ':');
    if (!p) return fallback;
    return atoi(p + 1);
}

static void extract_str(const char *json, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    const char *p = find_key(json, key);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
}

/* Push pointer state into every window's Clay context.
   局部坐标按各自 body 矩形换算，非焦点窗口也能 hover；
   press 只给指针命中的那个窗口（无焦点窗口如任务栏也要能收到点击），避免多窗口同时按下。 */
static void sync_pointer_state(VmWm *wm) {
    VmWindow *hit = wm->pointerValid ? VmWm_HitTest(wm, wm->pointerX, wm->pointerY) : NULL;
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        if (!win->clayCtx) continue;
        Clay_SetCurrentContext(win->clayCtx);
        VmRect body;
        VmWm_WindowBody(wm, win, &body);
        float localX = (float)wm->pointerX - body.x;
        float localY = (float)wm->pointerY - body.y;
        int inside = wm->pointerValid && !win->minimized &&
                     localX >= 0 && localY >= 0 &&
                     localX < body.w && localY < body.h;
        if (inside) {
            int press = (hit == win) && wm->pointerDown;
            Clay_SetPointerState((Clay_Vector2){localX, localY}, press);
        } else {
            Clay_SetPointerState((Clay_Vector2){-1000.0f, -1000.0f}, 0);
        }
    }
}

static void update_cursor(VmWm *wm, const char *id, const char *color,
                          int x, int y, int visible) {
    if (!wm || !id || !*id) return;
    for (int i = 0; i < wm->cursorCount; i++) {
        if (strcmp(wm->cursors[i].id, id) == 0) {
            wm->cursors[i].x = x;
            wm->cursors[i].y = y;
            wm->cursors[i].visible = visible;
            wm->cursors[i].lastSeen = time(NULL);
            snprintf(wm->cursors[i].color, sizeof(wm->cursors[i].color), "%s", color);
            return;
        }
    }
    if (wm->cursorCount >= VM_WM_MAX_CURSORS) return;
    VmCursor *c = &wm->cursors[wm->cursorCount++];
    snprintf(c->id, sizeof(c->id), "%s", id);
    snprintf(c->color, sizeof(c->color), "%s", color);
    c->x = x; c->y = y; c->visible = visible;
    c->lastSeen = time(NULL);
}

int VmWm_HandleInput(VmWm *wm, const char *jsonBody, size_t length) {
    if (!wm || !jsonBody) return 0;
    char kind[32] = {0};
    char clientId[64] = {0};
    char color[32] = {0};
    char key[64] = {0};
    char code[64] = {0};
    extract_str(jsonBody, "kind", kind, sizeof(kind));
    extract_str(jsonBody, "clientId", clientId, sizeof(clientId));
    extract_str(jsonBody, "color", color, sizeof(color));
    extract_str(jsonBody, "key", key, sizeof(key));
    extract_str(jsonBody, "code", code, sizeof(code));
    int x = extract_int(jsonBody, "x", -1);
    int y = extract_int(jsonBody, "y", -1);
    int button = extract_int(jsonBody, "button", 0);
    int deltaY = extract_int(jsonBody, "deltaY", 0);
    int ctrl = extract_int(jsonBody, "ctrl", 0);
    int shift = extract_int(jsonBody, "shift", 0);
    int alt = extract_int(jsonBody, "alt", 0);
    int changed = 0;

    /* 修饰键跟随最近一次键盘事件：keyHandler 的签名不含它们，由查询接口取。 */
    if (strcmp(kind, "keydown") == 0 || strcmp(kind, "keyup") == 0) {
        wm->keyCtrl = ctrl;
        wm->keyShift = shift;
        wm->keyAlt = alt;
    }

    int isDown = (strcmp(kind, "mousedown") == 0);
    int isUp = (strcmp(kind, "mouseup") == 0);
    int isMove = (strcmp(kind, "mousemove") == 0);
    int isWheel = (strcmp(kind, "wheel") == 0);

    /* Track every client's pointer; all are drawn into the shared frame. */
    if (*clientId) {
        update_cursor(wm, clientId, *color ? color : "#FFFFFF", x, y, 1);
        /* Focus arbitration: a mousedown claims focus for that client. */
        if (isDown) snprintf(wm->focusClientId, sizeof(wm->focusClientId), "%s", clientId);
    }

    /* 全局指针：跨窗口 hover 判定用，任何客户端的指针事件都会更新它。 */
    if (x >= 0 && y >= 0 && (isMove || isDown || isUp || isWheel)) {
        wm->pointerX = x;
        wm->pointerY = y;
        wm->pointerValid = 1;
    }
    if (isDown) {
        wm->pointerDown = 1;
        wm->pointerDownSeq++;   /* 一次按下只算一次，供外壳做按下沿判定 */
    }
    if (isUp) wm->pointerDown = 0;

    VmWindow *hit = VmWm_HitTest(wm, x, y);
    /* 指针事件的失效单元是「命中的窗口」与「上次命中的窗口」：
       hover 态转移时旧窗口要擦掉高亮，新窗口要画上。 */
    if (isMove || isDown || isUp || isWheel) {
        int hitId = hit ? hit->id : -1;
        if (wm->lastHitId != hitId) {
            VmWindow *prevHit = VmWm_FindWindow(wm, wm->lastHitId);
            if (prevHit) layer_dirty(prevHit, "hit-leave");
            wm->lastHitId = hitId;
        }
        if (hit) layer_dirty(hit, "hit");
    }
    /* Focus arbitration: window state changes only from the owning client.
       A mousedown from any client claims ownership first. */
    int ownsFocus = (*clientId && strcmp(wm->focusClientId, clientId) == 0);
    if (isDown) ownsFocus = 1; /* this client just claimed focus above */

    /* 装饰按钮先于焦点与拖动：命中即消费，不再启动拖动。 */
    if (ownsFocus && isDown && hit && !(hit->flags & VM_WIN_UNDECORATED)) {
        VmRect closeRect, maxRect, minRect;
        int size = 0;
        chrome_button_rects(wm, hit, &closeRect, &maxRect, &minRect, &size);
        if (size > 0 && rect_contains(closeRect, x, y)) {
            if (hit->closeHandler) hit->closeHandler(hit->id, hit->closeUserData);
            wm_dirty(wm, "close-button");
            sync_pointer_state(wm);
            return 1;
        }
        if (size > 0 && rect_contains(maxRect, x, y)) {
            VmWm_ToggleMaximize(wm, hit->id);
            sync_pointer_state(wm);
            return 1;
        }
        if (size > 0 && rect_contains(minRect, x, y)) {
            VmWm_Minimize(wm, hit->id);
            sync_pointer_state(wm);
            return 1;
        }
    }

    /* 手绘窗口（任务栏）没有焦点，按下即交给它自己的命中逻辑，命中就消费掉。 */
    if (ownsFocus && isDown && button == 0 && hit && hit->clickHandler) {
        if (hit->clickHandler(hit->id, x, y, hit->clickUserData)) {
            sync_pointer_state(wm);
            return 1;
        }
    }

    if (ownsFocus && (isDown || strcmp(kind, "click") == 0) && hit) {
        if (!(hit->flags & VM_WIN_NOFOCUS)) {
            VmWm_Focus(wm, hit->id);
            changed = 1;
        }
        int draggable = !(hit->flags & (VM_WIN_UNDECORATED | VM_WIN_PINNED | VM_WIN_NOFOCUS));
        if (draggable && y < hit->y + wm->chrome.titleHeight && button == 0) {
            /* 双击标题栏 = 最大化 / 还原。按在标题栏但不在按钮上才算。 */
            unsigned long long now = wm_now_ms();
            if (wm->lastTitleDownId == hit->id &&
                now - wm->lastTitleDownMs <= VM_DOUBLE_CLICK_MS) {
                wm->lastTitleDownId = -1;
                VmWm_ToggleMaximize(wm, hit->id);
                sync_pointer_state(wm);
                return 1;
            }
            wm->lastTitleDownId = hit->id;
            wm->lastTitleDownMs = now;
            if (!hit->maximized) {
                wm->dragId = hit->id;
                wm->dragOffX = x - hit->x;
                wm->dragOffY = y - hit->y;
            }
        }
    }

    if (ownsFocus && isMove && wm->dragId >= 0) {
        VmWm_Move(wm, wm->dragId, x - wm->dragOffX, y - wm->dragOffY);
        changed = 1;
    }

    if (ownsFocus && isUp && wm->dragId >= 0) {
        wm->dragId = -1;
        changed = 1;
    }

    /* Update local pointer for the focused window (content offset). */
    VmWindow *focus = VmWm_FindWindow(wm, wm->focusId);
    if (ownsFocus && focus && (isMove || isDown || isUp || strcmp(kind, "click") == 0)) {
        VmRect body;
        VmWm_WindowBody(wm, focus, &body);
        focus->pointerX = x - (int)body.x;
        focus->pointerY = y - (int)body.y;
        if (isDown) focus->pointerDown = 1;
        if (isUp) focus->pointerDown = 0;
    }
    /* hover 与焦点无关：非焦点窗口也要按指针位置刷新，否则鼠标移过去不会高亮。 */
    sync_pointer_state(wm);

    /* 滚轮：交给指针所在窗口，WM 不消费滚动语义。 */
    if (isWheel && hit && hit->wheelHandler) {
        VmRect body;
        VmWm_WindowBody(wm, hit, &body);
        int lx = x - (int)body.x;
        int ly = y - (int)body.y;
        if (hit->wheelHandler(hit->id, lx, ly, deltaY, hit->wheelUserData)) changed = 1;
    }

    /* Keyboard: only the focus client's key reaches the focused window's handler. */
    if (ownsFocus && focus && (strcmp(kind, "keydown") == 0 || strcmp(kind, "keyup") == 0)) {
        if (*key && focus->keyHandler) {
            focus->keyHandler(focus->id, key, code, strcmp(kind, "keydown") == 0,
                              focus->keyUserData);
            changed = 1;
        }
    }

    /* 指针移动会改变 hover，键盘会改变输入态：都要求重出一帧。 */
    if (isMove || isDown || isUp || isWheel ||
        strcmp(kind, "keydown") == 0 || strcmp(kind, "keyup") == 0) {
        /* 键盘只可能改变焦点窗口的内容（输入文本、caret、控件描边）。 */
        if (focus) layer_dirty(focus, "input-focus");
        wm_dirty(wm, "input");
    }
    if (changed) wm_dirty(wm, "input-changed");
    return changed;
}

static void append_rect_cmd(VmCmdBuffer *out, const char *id,
                            int x, int y, int w, int h,
                            const char *paint, int radius) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawRect.rect = r;
    VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint), paint);
    c->args.drawRect.cornerRadius = (float)radius;
}

static void append_border_cmd(VmCmdBuffer *out, const char *id,
                              int x, int y, int w, int h, int width, const char *color) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_BORDER, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawBorder.rect = r;
    VmDisplay_CopyStr(c->args.drawBorder.paint, sizeof(c->args.drawBorder.paint), color);
    c->args.drawBorder.widths[0] = c->args.drawBorder.widths[1] = width;
    c->args.drawBorder.widths[2] = c->args.drawBorder.widths[3] = width;
}

static void append_cursor_commands(VmWm *wm, VmDisplayList *out) {
    if (wm->cursorCount <= 0) return;
    time_t now = time(NULL);
    for (int i = 0; i < wm->cursorCount; i++) {
        VmCursor *c = &wm->cursors[i];
        if (!c->visible || now - c->lastSeen > 3) continue;
        char id[80];
        snprintf(id, sizeof(id), "cursor_%s", c->id);
        VmDrawCommand *d = VmDisplay_Append(out, VM_CMD_DRAW_IMG, id);
        if (!d) return;
        VmRect r = {(float)c->x, (float)c->y, 24, 24};
        d->rect = r;
        VmDisplay_CopyStr(d->args.drawImg.imgId, sizeof(d->args.drawImg.imgId), "cursor");
        d->args.drawImg.rect = r;
        VmDisplay_CopyStr(d->args.drawImg.tint, sizeof(d->args.drawImg.tint), c->color);
        d->args.drawImg.cornerRadius = 0;
    }
}

static void append_text_cmd(VmCmdBuffer *out, const char *id,
                            int x, int y, int w, int h, const char *text,
                            int fontId, int fontSize, const char *color) {
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

static void append_title_cmd(VmCmdBuffer *out, const char *id,
                             int x, int y, int w, const VmChrome *ch,
                             const char *title, const char *color) {
    int lineStep = VmFont_LineHeight(ch->titleFontId, ch->titleFontSize);
    if (lineStep <= 0) lineStep = ch->titleFontSize;
    int pad = 8;
    int tx = x + ch->borderWidth + pad;
    int ty = y + ch->borderWidth + (ch->titleHeight - ch->borderWidth - lineStep) / 2;
    if (ty < y + ch->borderWidth) ty = y + ch->borderWidth;
    int tw = w - 2 * ch->borderWidth - 2 * pad;
    if (tw < 0) tw = 0;
    append_text_cmd(out, id, tx, ty, tw, lineStep, title, ch->titleFontId, ch->titleFontSize, color);
    /* XP 标题是粗体；stb 光栅化不做合成粗体，同色偏移 1px 再画一次近似。 */
    if (ch->titleTextColor) {
        char id2[80];
        snprintf(id2, sizeof(id2), "%s_b", id);
        append_text_cmd(out, id2, tx + 1, ty, tw, lineStep, title,
                        ch->titleFontId, ch->titleFontSize, color);
    }
}

static void append_clip_cmd(VmCmdBuffer *out, VmCmdType type, const char *id, VmRect r) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, type, id);
    if (!c) return;
    c->rect = r;
    c->args.clip = r;
}

/* 返回 1 表示超配额。超限时整段内容被替换为纯色块 + 提示文字，
   needOut 给出该窗口真实需要的命令条数（写入数 + 被丢弃数）。 */
static int build_one_window(VmWm *wm, VmCmdBuffer *out, VmWindow *win,
                            const VmChrome *ch, const char *(*fontResolver)(uint16_t),
                            int *needOut) {
    char idbuf[64];
    VmRect body;
    VmWm_WindowBody(wm, win, &body);
    int bodyX = (int)body.x, bodyY = (int)body.y;
    int bodyW = (int)body.w, bodyH = (int)body.h;

    if (ch->showShadow) {
        snprintf(idbuf, sizeof(idbuf), "w%d_shadow", win->id);
        append_rect_cmd(out, idbuf, win->x - 6, win->y - 4,
                        win->w + 12, win->h + 12, "rgba(0,0,0,0.42)", 10);
    }

    /* 现代主题是圆角卡片；XP 窗口是直角，只保留标题栏顶部一点圆角。
       最大化后贴满工作区：全部直角，且不画外框（与 XP 行为一致）。 */
    const int maxed = win->maximized;
    const int bodyRadius = (ch->showShadow && !maxed) ? 8 : 0;
    const int titleRadius = maxed ? 0 : (ch->showShadow ? 7 : 4);

    snprintf(idbuf, sizeof(idbuf), "w%d_body", win->id);
    append_rect_cmd(out, idbuf, bodyX, bodyY, bodyW, bodyH,
                    win->bodyStyleId ? win->bodyStyleId : ch->windowStyleId, bodyRadius);

    if (!(win->flags & VM_WIN_UNDECORATED)) {
        const int bw = ch->borderWidth;
        const int th = ch->titleHeight;

        snprintf(idbuf, sizeof(idbuf), "w%d_title", win->id);
        append_rect_cmd(out, idbuf, win->x + bw, win->y + bw,
                        win->w - 2 * bw, th - bw,
                        win->focused ? ch->titleFocusColor : ch->titleIdleColor, titleRadius);

        snprintf(idbuf, sizeof(idbuf), "w%d_ttext", win->id);
        /* 主题给了标题文字色就统一用它（XP 失焦也是白字）；否则沿用现代深浅两色。 */
        append_title_cmd(out, idbuf, win->x, win->y, win->w, ch, win->title,
                         ch->titleTextColor ? ch->titleTextColor
                             : (win->focused ? "rgba(255,255,255,1)"
                                             : "rgba(202,208,224,1)"));

        VmRect closeRect, maxRect, minRect;
        int size = 0;
        chrome_button_rects(wm, win, &closeRect, &maxRect, &minRect, &size);
        if (size > 0) {
            int overMin = wm->pointerValid &&
                          rect_contains(minRect, wm->pointerX, wm->pointerY);
            int overMax = wm->pointerValid &&
                          rect_contains(maxRect, wm->pointerX, wm->pointerY);
            int overClose = wm->pointerValid &&
                            rect_contains(closeRect, wm->pointerX, wm->pointerY);
            /* XP 的标题按钮是常驻渐变块（关闭红）；现代主题只在 hover 时提示。 */
            if (!ch->showShadow) {
                snprintf(idbuf, sizeof(idbuf), "w%d_btn_min_bg", win->id);
                append_rect_cmd(out, idbuf, (int)minRect.x, (int)minRect.y, size, size,
                                "s_cap_btn", 3);
                snprintf(idbuf, sizeof(idbuf), "w%d_btn_max_bg", win->id);
                append_rect_cmd(out, idbuf, (int)maxRect.x, (int)maxRect.y, size, size,
                                "s_cap_btn", 3);
                snprintf(idbuf, sizeof(idbuf), "w%d_btn_close_bg", win->id);
                append_rect_cmd(out, idbuf, (int)closeRect.x, (int)closeRect.y, size, size,
                                "s_cap_close", 3);
            }
            if (overMin) {
                snprintf(idbuf, sizeof(idbuf), "w%d_min_hover", win->id);
                append_rect_cmd(out, idbuf, (int)minRect.x, (int)minRect.y,
                                size, size, "rgba(255,255,255,0.18)", 4);
            }
            if (overMax) {
                snprintf(idbuf, sizeof(idbuf), "w%d_max_hover", win->id);
                append_rect_cmd(out, idbuf, (int)maxRect.x, (int)maxRect.y,
                                size, size, "rgba(255,255,255,0.18)", 4);
            }
            if (overClose) {
                snprintf(idbuf, sizeof(idbuf), "w%d_close_hover", win->id);
                append_rect_cmd(out, idbuf, (int)closeRect.x, (int)closeRect.y,
                                size, size, "rgba(232,92,110,0.92)", 4);
            }
            int barW = size / 2, barH = 2;
            snprintf(idbuf, sizeof(idbuf), "w%d_min", win->id);
            append_rect_cmd(out, idbuf,
                            (int)minRect.x + (size - barW) / 2,
                            (int)minRect.y + size / 2 - barH / 2,
                            barW, barH, "rgba(255,255,255,0.85)", 0);

            /* 最大化图标：空心方框；已最大化时画成两个叠放的方框表示"还原"。 */
            int inset = 4;
            int bw2 = size - inset * 2, bh2 = size - inset * 2;
            if (bw2 >= 4 && bh2 >= 4) {
                if (win->maximized) {
                    snprintf(idbuf, sizeof(idbuf), "w%d_max_back", win->id);
                    append_border_cmd(out, idbuf,
                                      (int)maxRect.x + inset + 2, (int)maxRect.y + inset - 2,
                                      bw2 - 2, bh2 - 2, 1, "rgba(255,255,255,0.55)");
                    snprintf(idbuf, sizeof(idbuf), "w%d_max", win->id);
                    append_border_cmd(out, idbuf,
                                      (int)maxRect.x + inset, (int)maxRect.y + inset,
                                      bw2 - 2, bh2 - 2, 1, "rgba(255,255,255,0.92)");
                } else {
                    snprintf(idbuf, sizeof(idbuf), "w%d_max", win->id);
                    append_border_cmd(out, idbuf,
                                      (int)maxRect.x + inset, (int)maxRect.y + inset,
                                      bw2, bh2, 1, "rgba(255,255,255,0.92)");
                }
            }

            int lineStep = VmFont_LineHeight(ch->titleFontId, ch->titleFontSize);
            if (lineStep <= 0) lineStep = ch->titleFontSize;
            int glyphW = VmFont_Width(ch->titleFontId, ch->titleFontSize, "×", -1);
            snprintf(idbuf, sizeof(idbuf), "w%d_close", win->id);
            append_text_cmd(out, idbuf,
                            (int)closeRect.x + (size - glyphW) / 2,
                            (int)closeRect.y + (size - lineStep) / 2,
                            glyphW > 0 ? glyphW : size, lineStep,
                            "×", ch->titleFontId, ch->titleFontSize,
                            "rgba(255,255,255,0.95)");
        }

        if (!maxed) {
            snprintf(idbuf, sizeof(idbuf), "w%d_border", win->id);
            append_border_cmd(out, idbuf, win->x, win->y, win->w, win->h, bw,
                              win->focused ? ch->borderFocusColor : ch->borderIdleColor);
        }
    }

    /* 窗口内容裁到 body：防止控件溢出画到别的窗口上。 */
    int clipped = wm->contentClip && (win->layout || win->paint) && bodyW > 0 && bodyH > 0;
    int decorCount = out->count;   /* 装饰命令到此为止，截断时回退到这里 */
    if (clipped) {
        snprintf(idbuf, sizeof(idbuf), "w%d_clip", win->id);
        append_clip_cmd(out, VM_CMD_CLIP_PUSH, idbuf, body);
    }

    int truncated = 0;
    if (win->paint) {
        /* 手绘窗口：内容和装饰共用同一份坐标约定，paint 直接往缓冲里追加。 */
        win->paint(out, bodyX, bodyY, bodyW, bodyH, win->userData);
        truncated = (out->count >= out->cap);
    } else if (win->layout && win->clayCtx) {
        Clay_SetCurrentContext(win->clayCtx);
        /* 让宿主知道「当前在跑谁的布局」：布局回调里发的作废能收窄到这一个窗口，
           而不是退化成全量作废（任务栏这类非 JPX 窗口尤其需要）。 */
        int prevLayout = wm->layoutWinId;
        wm->layoutWinId = win->id;
        Clay_RenderCommandArray cmds = win->layout(win->userData);
        ClayFrame_AppendCommands(out, cmds.internalArray, cmds.length,
                                 bodyX, bodyY, fontResolver, NULL);
        truncated = (out->count >= out->cap);
        wm->layoutWinId = prevLayout;
    }

    if (truncated) {
        /* 超配额：整段内容丢弃，改画纯色块 + 一行字。
           这样层里没有半截裁剪，绝不会把后面的窗口（任务栏）裁掉。
           简单优先：宁可这个窗口显示「超了」，也不让它影响别人。 */
        int need = out->count + out->rejected;
        if (needOut) *needOut = need;
        out->count = decorCount;
        out->rejected = 0;

        snprintf(idbuf, sizeof(idbuf), "w%d_over", win->id);
        append_rect_cmd(out, idbuf, bodyX, bodyY, bodyW, bodyH, "rgba(150,24,48,1)", 0);

        char msg[128];
        snprintf(msg, sizeof(msg), "配额超了 %d 条", need - wm->quotaCommands);
        int fs = 20;
        int tw = VmFont_Width(ch->titleFontId, fs, msg, -1);
        if (tw <= 0) tw = (int)strlen(msg) * (fs / 2);
        int lh = VmFont_LineHeight(ch->titleFontId, fs);
        if (lh <= 0) lh = fs;
        int tx = bodyX + (bodyW - tw) / 2;
        int ty = bodyY + (bodyH - lh) / 2;
        if (tx < bodyX) tx = bodyX;
        if (ty < bodyY) ty = bodyY;
        snprintf(idbuf, sizeof(idbuf), "w%d_overtext", win->id);
        append_text_cmd(out, idbuf, tx, ty, tw, lh, msg, ch->titleFontId, fs,
                        "rgba(255,255,255,1)");
        return 1;
    }

    if (clipped) {
        snprintf(idbuf, sizeof(idbuf), "w%d_clip_end", win->id);
        append_clip_cmd(out, VM_CMD_CLIP_POP, idbuf, body);
    }
    return 0;
}

/* 把一条窗口图层并入输出流；容量不足返回 0，由调用方置 overflow。
   cap 由调用方给：告警帧会给横幅预留两条，不能把整帧塞死。 */
static int append_layer(VmDisplayList *out, const VmWindowLayer *layer, int cap) {
    if (!layer || layer->count <= 0) return 1;
    if (out->count + layer->count > cap) return 0;
    memcpy(&out->commands[out->count], layer->commands,
           (size_t)layer->count * sizeof(VmDrawCommand));
    out->count += layer->count;
    return 1;
}

void VmWm_BuildFrame(VmWm *wm, VmDisplayList *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t)) {
    if (!wm || !out) return;
    const VmChrome *ch = &wm->chrome;

    /* 阶段 1：只为脏窗口重建内容 —— 唯一接触 Clay 的地方。
       超限信息每帧重算，但只在「溢出窗口自己没被重建」时沿用上一帧：
       它没重建，说明内容没变，告警不该消失。 */
    int prevOverflowWin = wm->overflowWinId;
    int prevOverflowCount = wm->overflowCount;
    char prevOverflowTitle[64];
    snprintf(prevOverflowTitle, sizeof(prevOverflowTitle), "%s", wm->overflowTitle);
    int rebuiltIds[VM_WM_MAX_WINDOWS];
    int rebuiltCount = 0;
    wm->overflowWinId = -1;
    wm->overflowCount = 0;
    wm->overflowTitle[0] = '\0';
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        VmWindowLayer *layer = &win->layer;
        if (!layer->contentDirty && !layer->chromeDirty) continue;
        if (rebuiltCount < VM_WM_MAX_WINDOWS) rebuiltIds[rebuiltCount++] = win->id;
        wm->layoutCount++;
        VmCmdBuffer buf = {layer->commands, wm->quotaCommands, 0, 0};
        int need = 0;
        int truncated = build_one_window(wm, &buf, win, ch, fontResolver, &need);
        layer->count = buf.count;
        /* 超出配额：内容已被替换成纯色块 + 提示，这里只记录与告警。 */
        if (truncated) {
            wm->layerOverflow = 1;
            if (need > wm->overflowCount) {
                wm->overflowCount = need;
                wm->overflowWinId = win->id;
                snprintf(wm->overflowTitle, sizeof(wm->overflowTitle), "%s",
                         win->title ? win->title : "");
            }
        }
        layer->contentDirty = 0;
        layer->chromeDirty = 0;
        layer->dirtyWhy = NULL;
        layer->builtFrame = frameId;
    }

    /* 上一帧的溢出窗口本帧没被重建，说明内容没变：沿用，告警不闪。
       它若被重建了，本帧的结果（可能已不溢出）才是准的。 */
    if (wm->overflowWinId < 0 && prevOverflowWin >= 0) {
        int touched = 0;
        for (int k = 0; k < rebuiltCount; k++) {
            if (rebuiltIds[k] == prevOverflowWin) { touched = 1; break; }
        }
        if (!touched) {
            wm->overflowWinId = prevOverflowWin;
            wm->overflowCount = prevOverflowCount;
            snprintf(wm->overflowTitle, sizeof(wm->overflowTitle), "%s", prevOverflowTitle);
        }
    }

    /* 阶段 2：按 z 序合并 —— 纯命令拷贝，不再跑任何布局。 */
    VmDisplay_Clear(out);
    out->frameId = frameId;
    const int frameCap = VM_DISPLAY_MAX_COMMANDS;
    for (int pass = 0; pass < 2; pass++) {
        int wantTop = pass;
        for (int z = 1; z < wm->nextZ; z++) {
            for (int i = 0; i < wm->count; i++) {
                VmWindow *win = &wm->windows[i];
                if (win->z != z || win->minimized) continue;
                int top = (win->flags & VM_WIN_TOPMOST) ? 1 : 0;
                if (top != wantTop) continue;
                if (!append_layer(out, &win->layer, frameCap)) wm->layerOverflow = 1;
            }
        }
    }
    append_cursor_commands(wm, out);
}

/* z 序变化只改变重叠区域的显示：把窗口两两重叠的矩形（外扩 2px 覆盖边框）
   并成一个区域交给服务端做脏区，前端不做任何计算。 */
int VmWm_OverlapUnion(VmWm *wm, VmRect *out) {
    if (!wm || !out) return 0;
    int found = 0;
    for (int i = 0; i < wm->count; i++) {
        const VmWindow *a = &wm->windows[i];
        if (a->minimized) continue;
        for (int j = i + 1; j < wm->count; j++) {
            const VmWindow *b = &wm->windows[j];
            if (b->minimized) continue;
            int x1 = (a->x > b->x ? a->x : b->x) - 2;
            int y1 = (a->y > b->y ? a->y : b->y) - 2;
            int ax2 = a->x + a->w, bx2 = b->x + b->w;
            int ay2 = a->y + a->h, by2 = b->y + b->h;
            int x2 = (ax2 < bx2 ? ax2 : bx2) + 2;
            int y2 = (ay2 < by2 ? ay2 : by2) + 2;
            if (x2 <= x1 || y2 <= y1) continue;
            if (!found) {
                out->x = (float)x1;
                out->y = (float)y1;
                out->w = (float)(x2 - x1);
                out->h = (float)(y2 - y1);
                found = 1;
            } else {
                float ox2 = out->x + out->w, oy2 = out->y + out->h;
                float nx1 = out->x < (float)x1 ? out->x : (float)x1;
                float ny1 = out->y < (float)y1 ? out->y : (float)y1;
                float nx2 = ox2 > (float)x2 ? ox2 : (float)x2;
                float ny2 = oy2 > (float)y2 ? oy2 : (float)y2;
                out->x = nx1;
                out->y = ny1;
                out->w = nx2 - nx1;
                out->h = ny2 - ny1;
            }
        }
    }
    return found;
}

/* 只读诊断转储：一行汇总 + 一行一窗口，字段固定，便于 grep / diff 两次快照。
   dirty 为 0 且 built 停在旧帧号的窗口，就是「该重建却没人重建」的对象。 */
void VmWm_Dump(const VmWm *wm, const char *tag) {
    if (!wm) return;
    printf("[wm] %s n=%d focus=%d nextZ=%d layout=%u overflow=%d over={id=%d need=%d \"%s\"} "
           "dirty=%d(%s) ptr=(%d,%d) down=%d lastHit=%d\n",
           tag ? tag : "-", wm->count, wm->focusId, wm->nextZ, wm->layoutCount,
           wm->layerOverflow, wm->overflowWinId, wm->overflowCount, wm->overflowTitle,
           wm->dirty, wm->dirtyWhy ? wm->dirtyWhy : "-",
           wm->pointerX, wm->pointerY, wm->pointerDown, wm->lastHitId);
    for (int i = 0; i < wm->count; i++) {
        const VmWindow *w = &wm->windows[i];
        printf("[wm]   id=%-5d z=%-2d seq=%-2d flags=0x%02x f=%d min=%d max=%d cmds=%-3d "
               "dirty=%d(%-15s) built=%-4u rect=(%d,%d,%d,%d) \"%s\"\n",
               w->id, w->z, w->createdSeq, w->flags, w->focused, w->minimized,
               w->maximized, w->layer.count, w->layer.contentDirty,
               w->layer.dirtyWhy ? w->layer.dirtyWhy : "-",
               w->layer.builtFrame, w->x, w->y, w->w, w->h, w->title ? w->title : "");
        /* 命令条数偏多时给出类型分布：一眼分辨「内容行太多」还是「装饰重复」。 */
        if (w->layer.count > 40) {
            int nRect = 0, nText = 0, nBorder = 0, nClip = 0;
            for (int k = 0; k < w->layer.count; k++) {
                switch (w->layer.commands[k].type) {
                    case VM_CMD_DRAW_RECT: nRect++; break;
                    case VM_CMD_DRAW_TEXT: nText++; break;
                    case VM_CMD_DRAW_BORDER: nBorder++; break;
                    case VM_CMD_CLIP_PUSH:
                    case VM_CMD_CLIP_POP: nClip++; break;
                    default: break;
                }
            }
            printf("[wm]     cmds rect=%d text=%d border=%d clip=%d\n",
                   nRect, nText, nBorder, nClip);
        }
    }
    fflush(stdout);
}
