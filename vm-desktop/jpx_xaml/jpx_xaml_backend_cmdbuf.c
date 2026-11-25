#include "jpx_xaml_backend_cmdbuf.h"

#include <string.h>

#include "vm_font.h"

static VmDrawCommand *append(VmCmdBuffer *out, VmCmdType type, const char *id) {
    return VmCmdBuffer_Append(out, type, id);
}

static void cb_rect(void *ctx, const char *id, VmRect r, const char *paint, float radius) {
    VmCmdBuffer *out = (VmCmdBuffer *)ctx;
    VmDrawCommand *c = append(out, VM_CMD_DRAW_RECT, id);
    if (!c) return;
    c->rect = r;
    c->args.drawRect.rect = r;
    VmDisplay_CopyStr(c->args.drawRect.paint, sizeof(c->args.drawRect.paint), paint);
    c->args.drawRect.cornerRadius = radius;
}

static void cb_border(void *ctx, const char *id, VmRect r, const char *paint,
                      const int w[4]) {
    VmCmdBuffer *out = (VmCmdBuffer *)ctx;
    VmDrawCommand *c = append(out, VM_CMD_DRAW_BORDER, id);
    if (!c) return;
    c->rect = r;
    c->args.drawBorder.rect = r;
    VmDisplay_CopyStr(c->args.drawBorder.paint, sizeof(c->args.drawBorder.paint), paint);
    for (int i = 0; i < 4; i++) c->args.drawBorder.widths[i] = w[i];
}

static void cb_text(void *ctx, const char *id, VmRect r, const char *s,
                    int fontId, int size, const char *color) {
    VmCmdBuffer *out = (VmCmdBuffer *)ctx;
    VmDrawCommand *c = append(out, VM_CMD_DRAW_TEXT, id);
    if (!c) return;
    c->rect = r;
    c->args.drawText.rect = r;
    VmDisplay_CopyStr(c->args.drawText.text, sizeof(c->args.drawText.text), s);
    c->args.drawText.fontId = (uint16_t)fontId;
    c->args.drawText.fontSize = (uint16_t)size;
    VmDisplay_CopyStr(c->args.drawText.color, sizeof(c->args.drawText.color), color);
}

static void cb_image(void *ctx, const char *id, VmRect r, const char *imgRef) {
    VmCmdBuffer *out = (VmCmdBuffer *)ctx;
    VmDrawCommand *c = append(out, VM_CMD_DRAW_IMG, id);
    if (!c) return;
    c->rect = r;
    c->args.drawImg.rect = r;
    VmDisplay_CopyStr(c->args.drawImg.imgId, sizeof(c->args.drawImg.imgId), imgRef);
}

static void cb_path(void *ctx, const char *id, VmRect r, const char *svgPath,
                    const char *paint) {
    VmCmdBuffer *out = (VmCmdBuffer *)ctx;
    VmDrawCommand *c = append(out, VM_CMD_DRAW_PATH, id);
    if (!c) return;
    c->rect = r;
    c->args.drawPath.rect = r;
    VmDisplay_CopyStr(c->args.drawPath.path, sizeof(c->args.drawPath.path), svgPath);
    VmDisplay_CopyStr(c->args.drawPath.paint, sizeof(c->args.drawPath.paint), paint);
}

static int cb_text_width(void *ctx, int fontId, int size, const char *s) {
    (void)ctx;
    return VmFont_Width(fontId, size, s, -1);
}

static int cb_line_height(void *ctx, int fontId, int size) {
    (void)ctx;
    return VmFont_LineHeight(fontId, size);
}

static void cb_frame_begin(void *ctx, const char *tag, const JpxXamlNode *node, VmRect r) {
    /* 目前只有 Border 带背景：直接画成矩形。容器其他样式在 P2 补。 */
    const char *bg = JpxXamlNode_Attr(node, "Background");
    if (!bg || !*bg) return;
    (void)tag;
    cb_rect(ctx, JpxXamlNode_Attr(node, "Name") ? JpxXamlNode_Attr(node, "Name") : "frame",
            r, bg, 0.0f);
}

static void cb_frame_end(void *ctx, const char *tag) {
    (void)ctx;
    (void)tag;
}

static const JpxXamlBackend kBackend = {
    cb_rect,
    cb_border,
    cb_text,
    cb_image,
    cb_path,
    cb_text_width,
    cb_line_height,
    cb_frame_begin,
    cb_frame_end
};

const JpxXamlBackend *JpxXamlBackend_CmdBuf(void) {
    return &kBackend;
}
