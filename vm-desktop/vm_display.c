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
        case VM_CMD_LOAD_IMG:
            return str_equal(a->args.loadImg.imgId, b->args.loadImg.imgId) &&
                   str_equal(a->args.loadImg.dataUrl, b->args.loadImg.dataUrl);
        case VM_CMD_DEFINE_PATTERN: {
            const VmDefinePatternArgs *x = &a->args.definePattern, *y = &b->args.definePattern;
            return str_equal(x->patternId, y->patternId) && x->type == y->type &&
                   str_equal(x->color, y->color) && str_equal(x->from, y->from) &&
                   str_equal(x->to, y->to) && str_equal(x->direction, y->direction) &&
                   str_equal(x->imgId, y->imgId) && str_equal(x->tiling, y->tiling);
        }
        case VM_CMD_DEFINE_STYLE: {
            const VmDefineStyleArgs *x = &a->args.defineStyle, *y = &b->args.defineStyle;
            return str_equal(x->styleId, y->styleId) && str_equal(x->bgPattern, y->bgPattern) &&
                   str_equal(x->border, y->border) && str_equal(x->fontId, y->fontId) &&
                   str_equal(x->fontSize, y->fontSize) && str_equal(x->textColor, y->textColor);
        }
        case VM_CMD_DEFINE_DRAW_PROGRAM:
            return str_equal(a->args.defineProgram.programId, b->args.defineProgram.programId) &&
                   str_equal(a->args.defineProgram.layerId, b->args.defineProgram.layerId) &&
                   a->args.defineProgram.commands == b->args.defineProgram.commands;
        case VM_CMD_START_PROGRAM:
        case VM_CMD_PAUSE_PROGRAM:
        case VM_CMD_RESUME_PROGRAM:
            return str_equal(a->args.programControl.programId, b->args.programControl.programId) &&
                   str_equal(a->args.programControl.mode, b->args.programControl.mode);
        case VM_CMD_NONE:
            return 1;
    }
    return 0;
}
