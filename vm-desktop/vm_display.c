#include "vm_display.h"

#include <stdio.h>
#include <string.h>

void VmDisplay_Init(VmDisplayList *list) {
    if (!list) return;
    memset(list, 0, sizeof(*list));
}

void VmDisplay_CopyStr(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, cap, "%s", src);
}

void VmDisplay_Clear(VmDisplayList *list) {
    if (!list) return;
    /* Nested program lists are owned by guest static data in this demo; do not free here. */
    memset(list, 0, sizeof(*list));
}

VmDrawCommand *VmDisplay_Append(VmDisplayList *list, VmCmdType type, const char *id) {
    if (!list || list->count >= VM_DISPLAY_MAX_COMMANDS) return NULL;
    VmDrawCommand *cmd = &list->commands[list->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = type;
    if (id) snprintf(cmd->id, sizeof(cmd->id), "%s", id);
    return cmd;
}

VmResourceCommand *VmDisplay_AppendResource(VmDisplayList *list, VmCmdType type, const char *id) {
    if (!list || list->resourceCount >= VM_DISPLAY_MAX_RESOURCES) return NULL;
    VmResourceCommand *r = &list->resources[list->resourceCount++];
    memset(r, 0, sizeof(*r));
    r->type = type;
    if (id) snprintf(r->id, sizeof(r->id), "%s", id);
    return r;
}

VmDrawCommand *VmDisplay_Find(VmDisplayList *list, const char *id) {
    if (!list || !id) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->commands[i].id, id) == 0) return &list->commands[i];
    }
    return NULL;
}

static int rect_equal(VmRect a, VmRect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

static int str_equal(const char *a, const char *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

static int field_set(const char *s) { return s && *s; }

int VmDisplay_CommandEqual(const VmDrawCommand *a, const VmDrawCommand *b) {
    if (!a || !b) return 0;
    if (a->type != b->type || strcmp(a->id, b->id) != 0) return 0;
    if (!rect_equal(a->rect, b->rect)) return 0;

    switch (a->type) {
        case VM_CMD_CLEAR:
            return 1;
        case VM_CMD_DRAW_RECT: {
            const VmDrawRectArgs *x = &a->args.drawRect, *y = &b->args.drawRect;
            return rect_equal(x->rect, y->rect) && str_equal(x->paint, y->paint) &&
                   x->cornerRadius == y->cornerRadius;
        }
        case VM_CMD_DRAW_BORDER: {
            const VmDrawBorderArgs *x = &a->args.drawBorder, *y = &b->args.drawBorder;
            if (!rect_equal(x->rect, y->rect) || !str_equal(x->paint, y->paint) ||
                x->cornerRadius != y->cornerRadius) return 0;
            for (int i = 0; i < 4; i++) if (x->widths[i] != y->widths[i]) return 0;
            return 1;
        }
        case VM_CMD_DRAW_TEXT: {
            const VmDrawTextArgs *x = &a->args.drawText, *y = &b->args.drawText;
            return rect_equal(x->rect, y->rect) && str_equal(x->text, y->text) &&
                   x->fontId == y->fontId && x->fontSize == y->fontSize &&
                   str_equal(x->color, y->color) &&
                   x->letterSpacing == y->letterSpacing && x->lineHeight == y->lineHeight;
        }
        case VM_CMD_DRAW_IMG: {
            const VmDrawImgArgs *x = &a->args.drawImg, *y = &b->args.drawImg;
            return str_equal(x->imgId, y->imgId) && rect_equal(x->rect, y->rect) &&
                   str_equal(x->tint, y->tint) && x->cornerRadius == y->cornerRadius;
        }
        case VM_CMD_CLIP_PUSH:
        case VM_CMD_CLIP_POP:
            return rect_equal(a->args.clip, b->args.clip);
        case VM_CMD_OVERLAY_PUSH:
        case VM_CMD_OVERLAY_POP:
            return str_equal(a->args.paint, b->args.paint);
        /* 资源与程序控制命令不在 commands 里，不参与逐帧比较。 */
        case VM_CMD_LOAD_IMG:
        case VM_CMD_DEFINE_PATTERN:
        case VM_CMD_DEFINE_STYLE:
        case VM_CMD_DEFINE_DRAW_PROGRAM:
        case VM_CMD_START_PROGRAM:
        case VM_CMD_PAUSE_PROGRAM:
        case VM_CMD_RESUME_PROGRAM:
            return 0;
        case VM_CMD_NONE:
            return 1;
    }
    return 0;
}

void VmDisplay_AddDirty(VmDisplayDelta *delta, VmRect r) {
    if (!delta || r.w <= 0 || r.h <= 0) return;
    if (!delta->hasDirty) {
        delta->dirty = r;
        delta->hasDirty = 1;
        return;
    }
    float x1 = delta->dirty.x < r.x ? delta->dirty.x : r.x;
    float y1 = delta->dirty.y < r.y ? delta->dirty.y : r.y;
    float dx2 = delta->dirty.x + delta->dirty.w;
    float dy2 = delta->dirty.y + delta->dirty.h;
    float rx2 = r.x + r.w;
    float ry2 = r.y + r.h;
    float x2 = dx2 > rx2 ? dx2 : rx2;
    float y2 = dy2 > ry2 ? dy2 : ry2;
    delta->dirty.x = x1;
    delta->dirty.y = y1;
    delta->dirty.w = x2 - x1;
    delta->dirty.h = y2 - y1;
}

void VmDisplay_Diff(const VmDisplayList *prev, const VmDisplayList *current, VmDisplayDelta *delta) {
    if (!delta) return;
    memset(delta, 0, sizeof(*delta));
    if (!current) return;

    /* Current full order (frontend restores z-order from this). */
    for (int i = 0; i < current->count && delta->orderCount < VM_DISPLAY_MAX_COMMANDS; i++) {
        snprintf(delta->order[delta->orderCount], VM_DISPLAY_MAX_ID, "%s",
                 current->commands[i].id);
        delta->orderCount++;
    }

    /* 顺序变化会让遮挡关系变错，但每条命令的 rect/args 都没变；
       这是服务端做出的决定，必须由服务端显式告诉前端整屏重绘。 */
    if (prev) {
        if (prev->count != current->count) {
            delta->orderChanged = 1;
        } else {
            for (int i = 0; i < current->count; i++) {
                if (strcmp(prev->commands[i].id, current->commands[i].id) != 0) {
                    delta->orderChanged = 1;
                    break;
                }
            }
        }
    }

    if (!prev) {
        for (int i = 0; i < current->count && delta->changedCount < VM_DISPLAY_MAX_COMMANDS; i++)
            delta->changed[delta->changedCount++] = current->commands[i];
        return;
    }

    for (int i = 0; i < current->count; i++) {
        const VmDrawCommand *c = &current->commands[i];
        const VmDrawCommand *old = VmDisplay_Find((VmDisplayList *)prev, c->id);
        if (!old || !VmDisplay_CommandEqual(old, c)) {
            if (delta->changedCount < VM_DISPLAY_MAX_COMMANDS)
                delta->changed[delta->changedCount++] = *c;
            VmDisplay_AddDirty(delta, c->rect);
            /* 旧位置也要擦：指针、移动中的窗口只重绘新矩形会留下拖影。 */
            if (old) VmDisplay_AddDirty(delta, old->rect);
        }
    }

    for (int i = 0; i < prev->count; i++) {
        const VmDrawCommand *old = &prev->commands[i];
        if (!VmDisplay_Find((VmDisplayList *)current, old->id)) {
            if (delta->removedCount < VM_DISPLAY_MAX_COMMANDS)
                snprintf(delta->removed[delta->removedCount++], VM_DISPLAY_MAX_ID, "%s", old->id);
            VmDisplay_AddDirty(delta, old->rect);
        }
    }
}
