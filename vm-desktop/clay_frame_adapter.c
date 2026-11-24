#include "clay_frame_adapter.h"

#include <stdio.h>
#include <string.h>

/* clay.h is provided by the external Clay checkout; pass -I<clay-dir> at build time. */
#include "clay.h"

static const char *color_to_css(Clay_Color c) {
    /* Caller-owned scratch via a small ring; the display list borrows strings only until
       serialization in the same frame. A static buffer per call site is sufficient here. */
    static char buf[8][64];
    static int idx;
    char *b = buf[idx++ & 7];
    /* Clay_Color 是 float（0..255）。%g 会输出小数 RGB，canvas-cpp 的
       ColorParser 只认整数；直接把 float 传给 %u 又会读成 0，必须先转换。 */
    snprintf(b, 64, "rgba(%u,%u,%u,%.3f)",
             (unsigned)(c.r + 0.5f), (unsigned)(c.g + 0.5f),
             (unsigned)(c.b + 0.5f), c.a / 255.0f);
    return b;
}

static VmRect to_rect(Clay_BoundingBox box, int ox, int oy) {
    VmRect r = {box.x + ox, box.y + oy, box.width, box.height};
    return r;
}

static float max_radius(Clay_CornerRadius r) {
    float m = r.topLeft;
    if (r.topRight > m) m = r.topRight;
    if (r.bottomLeft > m) m = r.bottomLeft;
    if (r.bottomRight > m) m = r.bottomRight;
    return m;
}

void ClayFrame_AppendCommands(
    VmCmdBuffer *out,
    const void *renderCommands,
    int commandCount,
    int offsetX,
    int offsetY,
    const char *(*fontResolver)(uint16_t fontId),
    const char *(*imgResolver)(const void *imageData)) {
    if (!out || !renderCommands) return;
    const Clay_RenderCommand *commands = (const Clay_RenderCommand *)renderCommands;

    for (int i = 0; i < commandCount; i++) {
        const Clay_RenderCommand *rc = &commands[i];
        if (rc->commandType == CLAY_RENDER_COMMAND_TYPE_NONE) continue;
        char id[32];
        snprintf(id, sizeof(id), "%u", rc->id);
        VmRect rect = to_rect(rc->boundingBox, offsetX, offsetY);

        switch (rc->commandType) {
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                c->args.drawRect.rect = rect;
                VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint),
                                  color_to_css(rc->renderData.rectangle.backgroundColor));
                c->args.drawRect.cornerRadius = max_radius(rc->renderData.rectangle.cornerRadius);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                Clay_BorderRenderData d = rc->renderData.border;
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_BORDER, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                c->args.drawBorder.rect = rect;
                VmDisplay_CopyStr(c->args.drawBorder.paint, sizeof(c->args.drawBorder.paint),
                                  color_to_css(d.color));
                c->args.drawBorder.widths[0] = d.width.left;
                c->args.drawBorder.widths[1] = d.width.right;
                c->args.drawBorder.widths[2] = d.width.top;
                c->args.drawBorder.widths[3] = d.width.bottom;
                c->args.drawBorder.cornerRadius = max_radius(d.cornerRadius);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                Clay_TextRenderData d = rc->renderData.text;
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_TEXT, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                c->args.drawText.rect = rect;
                /* Text is not null-terminated; copy into owned storage. */
                size_t n = d.stringContents.length < VM_DISPLAY_MAX_TEXT - 1
                               ? (size_t)d.stringContents.length : VM_DISPLAY_MAX_TEXT - 1;
                memcpy(c->args.drawText.text, d.stringContents.chars, n);
                c->args.drawText.text[n] = '\0';
                c->args.drawText.fontId = d.fontId;
                c->args.drawText.fontSize = d.fontSize;
                VmDisplay_CopyStr(c->args.drawText.color, sizeof(c->args.drawText.color),
                                  color_to_css(d.textColor));
                c->args.drawText.letterSpacing = d.letterSpacing;
                c->args.drawText.lineHeight = d.lineHeight;
                if (fontResolver) (void)fontResolver(d.fontId);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_IMAGE: {
                Clay_ImageRenderData d = rc->renderData.image;
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_IMG, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                VmDisplay_CopyStr(c->args.drawImg.imgId, sizeof(c->args.drawImg.imgId),
                                  imgResolver ? imgResolver(d.imageData) : "");
                c->args.drawImg.rect = rect;
                VmDisplay_CopyStr(c->args.drawImg.tint, sizeof(c->args.drawImg.tint),
                                  color_to_css(d.backgroundColor));
                c->args.drawImg.cornerRadius = max_radius(d.cornerRadius);
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: {
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_CLIP_PUSH, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                c->args.clip = rect;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: {
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_CLIP_POP, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                c->args.clip = rect;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_START: {
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_OVERLAY_PUSH, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                VmDisplay_CopyStr(c->args.paint, sizeof(c->args.paint),
                                  color_to_css(rc->renderData.overlayColor.color));
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_OVERLAY_COLOR_END: {
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_OVERLAY_POP, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_CUSTOM: {
                VmDrawCommand *c = VmCmdBuffer_Append(out, VM_CMD_DRAW_RECT, id);
                if (!c) continue;   /* 配额溢出：VmCmdBuffer_Append 已记账，这里继续算出真实需求 */
                c->rect = rect;
                c->args.drawRect.rect = rect;
                VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint),
                                  "rgba(255,0,255,1)");
                c->args.drawRect.cornerRadius = 0;
                break;
            }
            case CLAY_RENDER_COMMAND_TYPE_NONE:
                break;
        }
    }
}
