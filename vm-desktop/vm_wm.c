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
    wm->focusId = -1;
    wm->dragId = -1;
    wm->focusClientId[0] = '\0';
    wm->chrome = VmTheme_Default()->chrome;
    wm->contentClip = 1;
    wm->dirty = 1;
    return wm;
}

void VmWm_SetChrome(VmWm *wm, const VmChrome *chrome) {
    if (!wm) return;
    wm->chrome = chrome ? *chrome : VmTheme_Default()->chrome;
    wm->dirty = 1;
}

void VmWm_SetContentClip(VmWm *wm, int enabled) {
    if (!wm) return;
    wm->contentClip = enabled ? 1 : 0;
    wm->dirty = 1;
}

void VmWm_Invalidate(VmWm *wm) {
    if (wm) wm->dirty = 1;
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
   hover 与 press 分开：指针落在哪个窗口就给哪个窗口局部坐标，非焦点窗口也能 hover 高亮；
   press 只给焦点窗口，避免多个窗口同时进入按下态。 */
static void sync_pointer_state(VmWm *wm) {
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        if (!win->clayCtx) continue;
        Clay_SetCurrentContext(win->clayCtx);
        const int bw = wm->chrome.borderWidth;
        int localX = wm->pointerX - (win->x + bw);
        int localY = wm->pointerY - (win->y + wm->chrome.titleHeight + bw);
        int inside = wm->pointerValid && !win->minimized &&
                     localX >= 0 && localY >= 0 &&
                     localX < win->w - 2 * bw && localY < win->h - wm->chrome.titleHeight - 2 * bw;
        if (inside) {
            Clay_SetPointerState((Clay_Vector2){(float)localX, (float)localY},
                                 win->focused && wm->pointerDown);
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
    int changed = 0;

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
    /* Focus arbitration: window state changes only from the owning client.
       A mousedown from any client claims ownership first. */
    int ownsFocus = (*clientId && strcmp(wm->focusClientId, clientId) == 0);
    if (isDown) ownsFocus = 1; /* this client just claimed focus above */

    if (ownsFocus && (isDown || strcmp(kind, "click") == 0) && hit) {
        VmWm_Focus(wm, hit->id);
        changed = 1;
        if (y < hit->y + wm->chrome.titleHeight && button == 0) {
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
        focus->pointerX = x - (focus->x + wm->chrome.borderWidth);
        focus->pointerY = y - (focus->y + wm->chrome.titleHeight + wm->chrome.borderWidth);
        if (isDown) focus->pointerDown = 1;
        if (isUp) focus->pointerDown = 0;
    }
    /* hover 与焦点无关：非焦点窗口也要按指针位置刷新，否则鼠标移过去不会高亮。 */
    sync_pointer_state(wm);

    /* 滚轮：交给指针所在窗口，WM 不消费滚动语义。 */
    if (isWheel && hit && hit->wheelHandler) {
        int lx = x - (hit->x + wm->chrome.borderWidth);
        int ly = y - (hit->y + wm->chrome.titleHeight + wm->chrome.borderWidth);
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
        wm->dirty = 1;
    }
    if (changed) wm->dirty = 1;
    return changed;
}

static void append_rect_cmd(VmDisplayList *out, const char *id,
                            int x, int y, int w, int h,
                            const char *paint, int radius) {
    VmDrawCommand *c = VmDisplay_Append(out, VM_CMD_DRAW_RECT, id);
    if (!c) return;
    VmRect r = {(float)x, (float)y, (float)w, (float)h};
    c->rect = r;
    c->args.drawRect.rect = r;
    VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint), paint);
    c->args.drawRect.cornerRadius = (float)radius;
}

static void append_border_cmd(VmDisplayList *out, const char *id,
                              int x, int y, int w, int h, int width, const char *color) {
    VmDrawCommand *c = VmDisplay_Append(out, VM_CMD_DRAW_BORDER, id);
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

static void append_title_cmd(VmDisplayList *out, const char *id,
                             int x, int y, int w, const VmChrome *ch,
                             const char *title, const char *color) {
    VmDrawCommand *c = VmDisplay_Append(out, VM_CMD_DRAW_TEXT, id);
    if (!c) return;
    int lineStep = VmFont_LineHeight(ch->titleFontId, ch->titleFontSize);
    if (lineStep <= 0) lineStep = ch->titleFontSize;
    int pad = 8;
    int tx = x + ch->borderWidth + pad;
    int ty = y + ch->borderWidth + (ch->titleHeight - ch->borderWidth - lineStep) / 2;
    if (ty < y + ch->borderWidth) ty = y + ch->borderWidth;
    int tw = w - 2 * ch->borderWidth - 2 * pad;
    if (tw < 0) tw = 0;
    VmRect box = {(float)tx, (float)ty, (float)tw, (float)lineStep};
    c->rect = box;
    c->args.drawText.rect = box;
    VmDisplay_CopyStr(c->args.drawText.text, sizeof(c->args.drawText.text), title);
    c->args.drawText.fontId = (uint16_t)ch->titleFontId;
    c->args.drawText.fontSize = (uint16_t)ch->titleFontSize;
    VmDisplay_CopyStr(c->args.drawText.color, sizeof(c->args.drawText.color), color);
}

static void append_clip_cmd(VmDisplayList *out, VmCmdType type, const char *id, VmRect r) {
    VmDrawCommand *c = VmDisplay_Append(out, type, id);
    if (!c) return;
    c->rect = r;
    c->args.clip = r;
}

void VmWm_BuildFrame(VmWm *wm, VmDisplayList *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t)) {
    if (!wm || !out) return;
    VmDisplay_Clear(out);
    out->frameId = frameId;

    const VmChrome *ch = &wm->chrome;
    const int bw = ch->borderWidth > 0 ? ch->borderWidth : 0;
    const int th = ch->titleHeight > 0 ? ch->titleHeight : 0;
    char idbuf[64];
    /* Desktop background is a draw program on the bg layer, not sent per-frame. */

    for (int z = 1; z < wm->nextZ; z++) {
        for (int i = 0; i < wm->count; i++) {
            VmWindow *win = &wm->windows[i];
            if (win->z != z || win->minimized) continue;

            int bodyX = win->x + bw, bodyY = win->y + th;
            int bodyW = win->w - 2 * bw, bodyH = win->h - th - bw;
            if (bodyW < 0) bodyW = 0;
            if (bodyH < 0) bodyH = 0;

            snprintf(idbuf, sizeof(idbuf), "w%d_body", win->id);
            append_rect_cmd(out, idbuf, bodyX, bodyY, bodyW, bodyH, ch->windowStyleId, 0);

            snprintf(idbuf, sizeof(idbuf), "w%d_title", win->id);
            append_rect_cmd(out, idbuf, win->x + bw, win->y + bw,
                            win->w - 2 * bw, th - bw,
                            win->focused ? ch->titleFocusColor : ch->titleIdleColor, 0);

            snprintf(idbuf, sizeof(idbuf), "w%d_ttext", win->id);
            append_title_cmd(out, idbuf, win->x, win->y, win->w, ch, win->title,
                             "rgba(255,255,255,1)");

            snprintf(idbuf, sizeof(idbuf), "w%d_border", win->id);
            append_border_cmd(out, idbuf, win->x, win->y, win->w, win->h, bw,
                              win->focused ? ch->borderFocusColor : ch->borderIdleColor);

            /* 窗口内容裁到 body：防止控件溢出画到别的窗口上。 */
            int clipped = wm->contentClip && win->layout && bodyW > 0 && bodyH > 0;
            if (clipped) {
                snprintf(idbuf, sizeof(idbuf), "w%d_clip", win->id);
                append_clip_cmd(out, VM_CMD_CLIP_PUSH, idbuf,
                                (VmRect){(float)bodyX, (float)bodyY,
                                         (float)bodyW, (float)bodyH});
            }

            if (win->layout && win->clayCtx) {
                Clay_SetCurrentContext(win->clayCtx);
                Clay_RenderCommandArray cmds = win->layout(win->userData);
                ClayFrame_AppendCommands(out, cmds.internalArray, cmds.length,
                                         bodyX, bodyY, fontResolver, NULL);
            }

            if (clipped) {
                snprintf(idbuf, sizeof(idbuf), "w%d_clip_end", win->id);
                append_clip_cmd(out, VM_CMD_CLIP_POP, idbuf,
                                (VmRect){(float)bodyX, (float)bodyY,
                                         (float)bodyW, (float)bodyH});
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
