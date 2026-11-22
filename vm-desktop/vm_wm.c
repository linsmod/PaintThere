#include "vm_wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm_font.h"

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
    wm->focusClientId[0] = '\0';
    wm->chrome = VmTheme_Default()->chrome;
    wm->contentClip = 1;
    wm->dirty = 1;
    return wm;
}

/* 全体窗口内容作废：装饰参数或主题变了，没有哪个窗口能幸免。 */
static void mark_all_content_dirty(VmWm *wm) {
    for (int i = 0; i < wm->count; i++) wm->windows[i].layer.contentDirty = 1;
}

void VmWm_SetChrome(VmWm *wm, const VmChrome *chrome) {
    if (!wm) return;
    wm->chrome = chrome ? *chrome : VmTheme_Default()->chrome;
    mark_all_content_dirty(wm);
    wm->dirty = 1;
}

void VmWm_SetContentClip(VmWm *wm, int enabled) {
    if (!wm) return;
    wm->contentClip = enabled ? 1 : 0;
    mark_all_content_dirty(wm);
    wm->dirty = 1;
}

/* 帧级的「有东西变了」但定位不到窗口：保守地把全部内容作废。
   能定位到窗口时用 VmWm_InvalidateWindow，只重建那一个。 */
void VmWm_Invalidate(VmWm *wm) {
    if (!wm) return;
    mark_all_content_dirty(wm);
    wm->dirty = 1;
}

void VmWm_InvalidateWindow(VmWm *wm, int id) {
    if (!wm) return;
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->layer.contentDirty = 1;
    wm->dirty = 1;
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
    win->layer.contentDirty = 1;   /* 新窗口没有内容，首次合成必须构建 */
    wm->dirty = 1;
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
        wm->dirty = 1;
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

/* 标题栏右侧按钮：最小化 / 关闭。命中与绘制共用同一份几何，避免两处漂移。 */
static void chrome_button_rects(const VmWm *wm, const VmWindow *win,
                                VmRect *closeRect, VmRect *minRect, int *sizeOut) {
    int bw = wm->chrome.borderWidth;
    int th = wm->chrome.titleHeight;
    int size = th - 2 * bw - 8;
    if (size > 16) size = 16;
    if (size < 8) size = 0;
    if (sizeOut) *sizeOut = size;
    if (size == 0) {
        *closeRect = (VmRect){0, 0, 0, 0};
        *minRect = *closeRect;
        return;
    }
    int by = win->y + bw + (th - bw - size) / 2;
    int closeX = win->x + win->w - bw - 4 - size;
    int minX = closeX - size - 4;
    *closeRect = (VmRect){(float)closeX, (float)by, (float)size, (float)size};
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
    wm->dirty = 1;
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
        if (wm->windows[i].id == id || wm->windows[i].id == prev)
            wm->windows[i].layer.contentDirty = 1;
    }
    wm->dirty = 1;
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
    win->layer.contentDirty = 1;
    wm->dirty = 1;
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
    win->layer.contentDirty = 1;   /* 失焦会改标题着色 */
    wm->dirty = 1;
}

void VmWm_Restore(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->minimized = 0;
    win->layer.contentDirty = 1;
    if (!(win->flags & VM_WIN_NOFOCUS)) VmWm_Focus(wm, id);
    wm->dirty = 1;
}

int VmWm_IsMinimized(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    return win ? win->minimized : 0;
}

int VmWm_FocusId(const VmWm *wm) {
    return wm ? wm->focusId : -1;
}

void VmWm_Move(VmWm *wm, int id, int x, int y) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    if (win->x == x && win->y == y) return;
    win->x = x;
    win->y = y;
    /* 内容命令在生成时就带了 body 偏移，位置一变每条坐标都过期：必须重建。 */
    win->layer.contentDirty = 1;
    wm->dirty = 1;
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
    if (isDown) wm->pointerDown = 1;
    if (isUp) wm->pointerDown = 0;

    VmWindow *hit = VmWm_HitTest(wm, x, y);
    /* 指针事件的失效单元是「命中的窗口」与「上次命中的窗口」：
       hover 态转移时旧窗口要擦掉高亮，新窗口要画上。 */
    if (isMove || isDown || isUp || isWheel) {
        int hitId = hit ? hit->id : -1;
        if (wm->lastHitId != hitId) {
            VmWindow *prevHit = VmWm_FindWindow(wm, wm->lastHitId);
            if (prevHit) prevHit->layer.contentDirty = 1;
            wm->lastHitId = hitId;
        }
        if (hit) hit->layer.contentDirty = 1;
    }
    /* Focus arbitration: window state changes only from the owning client.
       A mousedown from any client claims ownership first. */
    int ownsFocus = (*clientId && strcmp(wm->focusClientId, clientId) == 0);
    if (isDown) ownsFocus = 1; /* this client just claimed focus above */

    /* 装饰按钮先于焦点与拖动：命中即消费，不再启动拖动。 */
    if (ownsFocus && isDown && hit && !(hit->flags & VM_WIN_UNDECORATED)) {
        VmRect closeRect, minRect;
        int size = 0;
        chrome_button_rects(wm, hit, &closeRect, &minRect, &size);
        if (size > 0 && rect_contains(closeRect, x, y)) {
            if (hit->closeHandler) hit->closeHandler(hit->id, hit->closeUserData);
            wm->dirty = 1;
            sync_pointer_state(wm);
            return 1;
        }
        if (size > 0 && rect_contains(minRect, x, y)) {
            VmWm_Minimize(wm, hit->id);
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
            wm->dragId = hit->id;
            wm->dragOffX = x - hit->x;
            wm->dragOffY = y - hit->y;
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
        if (focus) focus->layer.contentDirty = 1;
        wm->dirty = 1;
    }
    if (changed) wm->dirty = 1;
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
}

static void append_clip_cmd(VmCmdBuffer *out, VmCmdType type, const char *id, VmRect r) {
    VmDrawCommand *c = VmCmdBuffer_Append(out, type, id);
    if (!c) return;
    c->rect = r;
    c->args.clip = r;
}

static void build_one_window(VmWm *wm, VmCmdBuffer *out, VmWindow *win,
                             const VmChrome *ch, const char *(*fontResolver)(uint16_t)) {
    char idbuf[64];
    VmRect body;
    VmWm_WindowBody(wm, win, &body);
    int bodyX = (int)body.x, bodyY = (int)body.y;
    int bodyW = (int)body.w, bodyH = (int)body.h;

    snprintf(idbuf, sizeof(idbuf), "w%d_body", win->id);
    append_rect_cmd(out, idbuf, bodyX, bodyY, bodyW, bodyH,
                    win->bodyStyleId ? win->bodyStyleId : ch->windowStyleId, 0);

    if (!(win->flags & VM_WIN_UNDECORATED)) {
        const int bw = ch->borderWidth;
        const int th = ch->titleHeight;

        snprintf(idbuf, sizeof(idbuf), "w%d_title", win->id);
        append_rect_cmd(out, idbuf, win->x + bw, win->y + bw,
                        win->w - 2 * bw, th - bw,
                        win->focused ? ch->titleFocusColor : ch->titleIdleColor, 0);

        snprintf(idbuf, sizeof(idbuf), "w%d_ttext", win->id);
        append_title_cmd(out, idbuf, win->x, win->y, win->w, ch, win->title,
                         "rgba(255,255,255,1)");

        VmRect closeRect, minRect;
        int size = 0;
        chrome_button_rects(wm, win, &closeRect, &minRect, &size);
        if (size > 0) {
            int barW = size / 2, barH = 2;
            snprintf(idbuf, sizeof(idbuf), "w%d_min", win->id);
            append_rect_cmd(out, idbuf,
                            (int)minRect.x + (size - barW) / 2,
                            (int)minRect.y + size / 2 - barH / 2,
                            barW, barH, "rgba(255,255,255,0.85)", 0);

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

        snprintf(idbuf, sizeof(idbuf), "w%d_border", win->id);
        append_border_cmd(out, idbuf, win->x, win->y, win->w, win->h, bw,
                          win->focused ? ch->borderFocusColor : ch->borderIdleColor);
    }

    /* 窗口内容裁到 body：防止控件溢出画到别的窗口上。 */
    int clipped = wm->contentClip && win->layout && bodyW > 0 && bodyH > 0;
    if (clipped) {
        snprintf(idbuf, sizeof(idbuf), "w%d_clip", win->id);
        append_clip_cmd(out, VM_CMD_CLIP_PUSH, idbuf, body);
    }

    if (win->layout && win->clayCtx) {
        Clay_SetCurrentContext(win->clayCtx);
        Clay_RenderCommandArray cmds = win->layout(win->userData);
        ClayFrame_AppendCommands(out, cmds.internalArray, cmds.length,
                                 bodyX, bodyY, fontResolver, NULL);
    }

    if (clipped) {
        snprintf(idbuf, sizeof(idbuf), "w%d_clip_end", win->id);
        append_clip_cmd(out, VM_CMD_CLIP_POP, idbuf, body);
    }
}

/* 把一条窗口图层并入输出流；容量不足返回 0，由调用方置 overflow。 */
static int append_layer(VmDisplayList *out, const VmWindowLayer *layer) {
    if (!layer || layer->count <= 0) return 1;
    if (out->count + layer->count > VM_DISPLAY_MAX_COMMANDS) return 0;
    memcpy(&out->commands[out->count], layer->commands,
           (size_t)layer->count * sizeof(VmDrawCommand));
    out->count += layer->count;
    return 1;
}

void VmWm_BuildFrame(VmWm *wm, VmDisplayList *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t)) {
    if (!wm || !out) return;
    const VmChrome *ch = &wm->chrome;

    /* 阶段 1：只为脏窗口重建内容 —— 唯一接触 Clay 的地方。 */
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        VmWindowLayer *layer = &win->layer;
        if (!layer->contentDirty && !layer->chromeDirty) continue;
        wm->layoutCount++;
        VmCmdBuffer buf = {layer->commands, VM_WM_LAYER_COMMANDS, 0};
        build_one_window(wm, &buf, win, ch, fontResolver);
        layer->count = buf.count;
        /* 写满说明该窗口内容超出配额，帧里会缺命令：显式记录，不静默通过。 */
        if (buf.count >= VM_WM_LAYER_COMMANDS) wm->layerOverflow = 1;
        layer->contentDirty = 0;
        layer->chromeDirty = 0;
        layer->builtFrame = frameId;
    }

    /* 阶段 2：按 z 序合并 —— 纯命令拷贝，不再跑任何布局。 */
    VmDisplay_Clear(out);
    out->frameId = frameId;
    for (int pass = 0; pass < 2; pass++) {
        int wantTop = pass;
        for (int z = 1; z < wm->nextZ; z++) {
            for (int i = 0; i < wm->count; i++) {
                VmWindow *win = &wm->windows[i];
                if (win->z != z || win->minimized) continue;
                int top = (win->flags & VM_WIN_TOPMOST) ? 1 : 0;
                if (top != wantTop) continue;
                if (!append_layer(out, &win->layer)) wm->layerOverflow = 1;
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
