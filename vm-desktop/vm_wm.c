#include "vm_wm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White arrow template (same as the WebSocket channel). Frontend tints it via drawImg. */
static const char VM_CURSOR_SVG_B64[] =
    "PHN2ZyB4bWxucz0naHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmcnIHdpZHRoPScyNCcgaGVpZ2h0PScyNCcgdmlld0JveD0nMCAwIDI0IDI0Jz48cGF0aCBkPSdNNCwyIEw0LDIwIEw5LDE1IEwxMi41LDIyIEwxNS41LDIwLjUgTDEyLDEzLjUgTDIwLDEzIFonIGZpbGw9JyNmZmYnIHN0cm9rZT0nIzAwMCcgc3Ryb2tlLXdpZHRoPScxJy8+PC9zdmc+";

VmWm *VmWm_Create(int width, int height) {
    VmWm *wm = (VmWm *)calloc(1, sizeof(VmWm));
    if (!wm) return NULL;
    wm->width = width;
    wm->height = height;
    wm->nextZ = 1;
    wm->focusId = -1;
    wm->dragId = -1;
    wm->focusClientId[0] = '\0';
    return wm;
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
    win->focused = 0;
    win->minimized = 0;
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
        return;
    }
}

VmWindow *VmWm_FindWindow(VmWm *wm, int id) {
    if (!wm) return NULL;
    for (int i = 0; i < wm->count; i++) if (wm->windows[i].id == id) return &wm->windows[i];
    return NULL;
}

/* Hit test from top z to bottom; title bar included in window rect. */
VmWindow *VmWm_HitTest(VmWm *wm, int x, int y) {
    if (!wm) return NULL;
    for (int z = wm->nextZ - 1; z >= 1; z--) {
        for (int i = 0; i < wm->count; i++) {
            VmWindow *win = &wm->windows[i];
            if (win->z != z || win->minimized) continue;
            if (x >= win->x && x < win->x + win->w &&
                y >= win->y && y < win->y + win->h) return win;
        }
    }
    return NULL;
}

void VmWm_Raise(VmWm *wm, int id) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (win) win->z = wm->nextZ++;
}

void VmWm_Focus(VmWm *wm, int id) {
    if (!wm) return;
    for (int i = 0; i < wm->count; i++) wm->windows[i].focused = (wm->windows[i].id == id);
    wm->focusId = id;
    VmWm_Raise(wm, id);
}

void VmWm_Move(VmWm *wm, int id, int x, int y) {
    VmWindow *win = VmWm_FindWindow(wm, id);
    if (!win) return;
    win->x = x;
    win->y = y;
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
   Non-focused windows get an off-screen pointer so no stale hover remains. */
static void sync_pointer_state(VmWm *wm) {
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        if (!win->clayCtx) continue;
        Clay_SetCurrentContext(win->clayCtx);
        if (win->focused) {
            Clay_SetPointerState((Clay_Vector2){(float)win->pointerX, (float)win->pointerY},
                                 win->pointerDown != 0);
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
    extract_str(jsonBody, "kind", kind, sizeof(kind));
    extract_str(jsonBody, "clientId", clientId, sizeof(clientId));
    extract_str(jsonBody, "color", color, sizeof(color));
    int x = extract_int(jsonBody, "x", -1);
    int y = extract_int(jsonBody, "y", -1);
    int button = extract_int(jsonBody, "button", 0);
    int changed = 0;

    int isDown = (strcmp(kind, "mousedown") == 0);
    int isUp = (strcmp(kind, "mouseup") == 0);
    int isMove = (strcmp(kind, "mousemove") == 0);

    /* Track every client's pointer; all are drawn into the shared frame. */
    if (*clientId) {
        update_cursor(wm, clientId, *color ? color : "#FFFFFF", x, y, 1);
        /* Focus arbitration: a mousedown claims focus for that client. */
        if (isDown) snprintf(wm->focusClientId, sizeof(wm->focusClientId), "%s", clientId);
    }

    VmWindow *hit = VmWm_HitTest(wm, x, y);
    /* Focus arbitration: window state changes only from the owning client.
       A mousedown from any client claims ownership first. */
    int ownsFocus = (*clientId && strcmp(wm->focusClientId, clientId) == 0);
    if (isDown) ownsFocus = 1; /* this client just claimed focus above */

    if (ownsFocus && (isDown || strcmp(kind, "click") == 0) && hit) {
        VmWm_Focus(wm, hit->id);
        changed = 1;
        if (y < hit->y + VM_WM_TITLE_HEIGHT && button == 0) {
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
        focus->pointerX = x - (focus->x + 2);
        focus->pointerY = y - (focus->y + VM_WM_TITLE_HEIGHT + 2);
        if (isDown) focus->pointerDown = 1;
        if (isUp) focus->pointerDown = 0;
        sync_pointer_state(wm);
    }
    return changed;
}

static void append_rect_cmd(ClayFrameBuffer *out, const char *id,
                            int x, int y, int w, int h,
                            const char *color, int radius) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"cmd\":\"drawRect\",\"args\":[%d,%d,%d,%d,\"%s\",%d],"
             "\"rect\":[%d,%d,%d,%d]}",
             id, x, y, w, h, color, radius, x, y, w, h);
    ClayFrame_AppendRaw(out, buf);
}

static void append_border_cmd(ClayFrameBuffer *out, const char *id,
                              int x, int y, int w, int h, const char *color) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"cmd\":\"drawBorder\",\"args\":[%d,%d,%d,%d,\"%s\",[2,2,2,2],0],"
             "\"rect\":[%d,%d,%d,%d]}",
             id, x, y, w, h, color, x, y, w, h);
    ClayFrame_AppendRaw(out, buf);
}

static void append_cursor_commands(VmWm *wm, ClayFrameBuffer *out) {
    if (wm->cursorCount <= 0) return;
    char buf[512];
    /* loadImg is idempotent on the frontend (cached by imgId). */
    snprintf(buf, sizeof(buf),
             "{\"id\":\"cursor_asset\",\"cmd\":\"loadImg\",\"args\":[\"cursor\",\"data:image/svg+xml;base64,%s\"],\"rect\":[0,0,0,0]}",
             VM_CURSOR_SVG_B64);
    ClayFrame_AppendRaw(out, buf);

    time_t now = time(NULL);
    for (int i = 0; i < wm->cursorCount; i++) {
        VmCursor *c = &wm->cursors[i];
        if (!c->visible || now - c->lastSeen > 3) continue;
        snprintf(buf, sizeof(buf),
                 "{\"id\":\"cursor_%s\",\"cmd\":\"drawImg\",\"args\":[\"cursor\",%d,%d,24,24,\"%s\",0],"
                 "\"rect\":[%d,%d,24,24]}",
                 c->id, c->x, c->y, c->color, c->x, c->y);
        ClayFrame_AppendRaw(out, buf);
    }
}

static void append_title_cmd(ClayFrameBuffer *out, const char *id,
                             int x, int y, const char *title, const char *color) {
    char buf[512];
    /* id|fontId|fontSize|color|text — text is trusted demo title (no quotes). */
    snprintf(buf, sizeof(buf),
             "{\"id\":\"%s\",\"cmd\":\"drawText\",\"args\":[%d,%d,%d,%d,\"%s\",0,14,\"%s\",0,0],"
             "\"rect\":[%d,%d,%d,%d]}",
             id, x + 8, y + 6, 300, VM_WM_TITLE_HEIGHT - 6, title, color,
             x, y, 300, VM_WM_TITLE_HEIGHT);
    ClayFrame_AppendRaw(out, buf);
}

void VmWm_BuildFrame(VmWm *wm, ClayFrameBuffer *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t)) {
    if (!wm || !out) return;
    ClayFrame_Begin(out, frameId);

    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "wm_bg");
    append_rect_cmd(out, idbuf, 0, 0, wm->width, wm->height, "rgba(30,32,38,1)", 0);

    /* z order bottom -> top */
    for (int z = 1; z < wm->nextZ; z++) {
        for (int i = 0; i < wm->count; i++) {
            VmWindow *win = &wm->windows[i];
            if (win->z != z || win->minimized) continue;

            snprintf(idbuf, sizeof(idbuf), "w%d_body", win->id);
            append_rect_cmd(out, idbuf, win->x + 2, win->y + VM_WM_TITLE_HEIGHT,
                            win->w - 4, win->h - VM_WM_TITLE_HEIGHT - 2,
                            "rgba(250,250,252,1)", 0);

            snprintf(idbuf, sizeof(idbuf), "w%d_title", win->id);
            append_rect_cmd(out, idbuf, win->x + 2, win->y + 2,
                            win->w - 4, VM_WM_TITLE_HEIGHT - 2,
                            win->focused ? "rgba(38,110,190,1)" : "rgba(120,126,134,1)", 0);

            snprintf(idbuf, sizeof(idbuf), "w%d_ttext", win->id);
            append_title_cmd(out, idbuf, win->x, win->y, win->title, "rgba(255,255,255,1)");

            snprintf(idbuf, sizeof(idbuf), "w%d_border", win->id);
            append_border_cmd(out, idbuf, win->x, win->y, win->w, win->h,
                              win->focused ? "rgba(38,110,190,1)" : "rgba(90,96,104,1)");

            if (win->layout && win->clayCtx) {
                Clay_SetCurrentContext(win->clayCtx);
                Clay_RenderCommandArray cmds = win->layout(win->userData);
                ClayFrame_AppendCommands(out, cmds.internalArray, cmds.length,
                                         win->x + 2, win->y + VM_WM_TITLE_HEIGHT + 2,
                                         fontResolver, NULL);
            }
        }
    }
    append_cursor_commands(wm, out);
    ClayFrame_End(out);
}
