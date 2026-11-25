/*
 * jpx_xaml_render_direct.c - jp_xaml 的 Direct 回放（M7.1）
 *
 * 与 jpx_xaml_view.c 的区别：这里不调用任何 Jpx_*，只把元素树翻译成
 * JpxXamlBackend 调用（最终是 VmCmdBuffer）。布局 v1 只有绝对定位 +
 * StackPanel 的纵向/横向堆叠；Repeat / Layers / States 在 P2 接入。
 */
#include "jpx_xaml_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIRECT_FONT_ID 0
#define DIRECT_FONT_SIZE 14
#define DIRECT_LINE_DEFAULT 18
#define DIRECT_GAP_DEFAULT 6
#define DIRECT_TEXT_COLOR "rgba(230,232,236,1)"
#define DIRECT_BTN_COLOR "rgba(48,96,180,1)"
#define DIRECT_INPUT_BG "rgba(24,26,30,1)"
#define DIRECT_INPUT_BORDER "rgba(90,96,104,1)"

typedef struct {
    const JpxXamlBackend *b;
    const JpxXamlDoc *doc;
    void *ctx;
    int fontId, fontSize;
} DirectCtx;

static const JpxXamlNode *node_at(DirectCtx *dc, int index) {
    return JpxXamlDoc_Node(dc->doc, index);
}

static const char *attr(const JpxXamlNode *n, const char *k) {
    return JpxXamlNode_Attr(n, k);
}

static int attr_int(const JpxXamlNode *n, const char *k, int def) {
    const char *v = JpxXamlNode_Attr(n, k);
    return (v && *v) ? atoi(v) : def;
}

static int line_h(DirectCtx *dc) {
    int h = dc->b && dc->b->line_height
                ? dc->b->line_height(dc->ctx, dc->fontId, dc->fontSize) : 0;
    return h > 0 ? h : DIRECT_LINE_DEFAULT;
}

static const char *paint_of(const JpxXamlNode *n, const char *key, const char *def) {
    const char *v = attr(n, key);
    return (v && *v) ? v : def;
}

static void emit_rect(DirectCtx *dc, const char *id, int x, int y, int w, int h,
                      const char *paint, float radius) {
    if (!paint || !*paint || !dc->b || !dc->b->rect) return;
    dc->b->rect(dc->ctx, id, (VmRect){(float)x, (float)y, (float)w, (float)h},
                paint, radius);
}

static void emit_border(DirectCtx *dc, const char *id, int x, int y, int w, int h,
                        int width, const char *paint) {
    if (!paint || !*paint || !dc->b || !dc->b->border) return;
    int w4[4] = {width, width, width, width};
    dc->b->border(dc->ctx, id, (VmRect){(float)x, (float)y, (float)w, (float)h},
                  paint, w4);
}

static void emit_text(DirectCtx *dc, const char *id, int x, int y, int w, int h,
                      const char *s, const char *color) {
    if (!s || !*s || !dc->b || !dc->b->text) return;
    dc->b->text(dc->ctx, id, (VmRect){(float)x, (float)y, (float)w, (float)h},
                s, dc->fontId, dc->fontSize, color);
}

static void walk(DirectCtx *dc, int index, int x, int y, int w, int h);

/* Window / StackPanel / 未知容器：按流式堆叠排子节点。 */
static void flow_children(DirectCtx *dc, const JpxXamlNode *n, int x, int y, int w, int h,
                          int horizontal) {
    int gap = attr_int(n, "Gap", DIRECT_GAP_DEFAULT);
    int cx = x, cy = y;
    for (int c = n->firstChild; c >= 0; c = node_at(dc, c)->nextSibling) {
        const JpxXamlNode *ch = node_at(dc, c);
        if (!ch) break;
        int cw = attr_int(ch, "Width", 0);
        int chh = attr_int(ch, "Height", 0);
        if (horizontal) {
            if (cw <= 0) cw = w;
            if (chh <= 0) chh = h;
            walk(dc, c, cx, cy, cw, chh);
            cx += cw + gap;
        } else {
            if (chh <= 0) chh = line_h(dc) + 8;
            if (cw <= 0) cw = w;
            walk(dc, c, cx, cy, cw, chh);
            cy += chh + gap;
        }
    }
}

static void walk(DirectCtx *dc, int index, int x, int y, int w, int h) {
    const JpxXamlNode *n = node_at(dc, index);
    char idbuf[64];
    if (!n) return;

    if (strcmp(n->tag, "Window") == 0) {
        flow_children(dc, n, x, y, w, h, 0);
        return;
    }

    if (strcmp(n->tag, "StackPanel") == 0) {
        const char *o = attr(n, "Orientation");
        flow_children(dc, n, x, y, w, h, o && strcmp(o, "Horizontal") == 0);
        return;
    }

    if (strcmp(n->tag, "Border") == 0) {
        const char *bg = paint_of(n, "Background", NULL);
        int began = 0;
        if (bg && dc->b && dc->b->frame_begin) {
            dc->b->frame_begin(dc->ctx, n->tag, n,
                               (VmRect){(float)x, (float)y, (float)w, (float)h});
            began = 1;
        }
        int pad = attr_int(n, "Padding", 0);
        flow_children(dc, n, x + pad, y + pad, w - pad * 2, h - pad * 2, 0);
        if (began && dc->b->frame_end) dc->b->frame_end(dc->ctx, n->tag);
        return;
    }

    if (strcmp(n->tag, "TextBlock") == 0) {
        const char *t = attr(n, "Text");
        if (!t) t = JpxXamlNode_Text(n);
        const char *color = paint_of(n, "Foreground", DIRECT_TEXT_COLOR);
        JpxXaml__NodeId(n, index, idbuf, sizeof(idbuf));
        emit_text(dc, idbuf, x, y, w, h, t, color);
        return;
    }

    if (strcmp(n->tag, "Button") == 0) {
        const char *t = attr(n, "Content");
        if (!t) t = JpxXamlNode_Text(n);
        const char *bg = paint_of(n, "Background", DIRECT_BTN_COLOR);
        const char *fg = paint_of(n, "Foreground", "rgba(255,255,255,1)");
        JpxXaml__NodeId(n, index, idbuf, sizeof(idbuf));
        emit_rect(dc, idbuf, x, y, w, h, bg, 4.0f);
        emit_text(dc, idbuf, x + 10, y + (h - line_h(dc)) / 2, w - 20, line_h(dc), t, fg);
        return;
    }

    if (strcmp(n->tag, "CheckBox") == 0) {
        const char *t = attr(n, "Content");
        if (!t) t = JpxXamlNode_Text(n);
        JpxXaml__NodeId(n, index, idbuf, sizeof(idbuf));
        emit_rect(dc, idbuf, x, y + (h - 14) / 2, 14, 14, DIRECT_INPUT_BG, 2.0f);
        emit_border(dc, idbuf, x, y + (h - 14) / 2, 14, 14, 1, DIRECT_INPUT_BORDER);
        emit_text(dc, idbuf, x + 20, y + (h - line_h(dc)) / 2, w - 20, line_h(dc),
                  t, DIRECT_TEXT_COLOR);
        return;
    }

    if (strcmp(n->tag, "TextBox") == 0) {
        const char *t = attr(n, "Text");
        JpxXaml__NodeId(n, index, idbuf, sizeof(idbuf));
        emit_rect(dc, idbuf, x, y, w, h, DIRECT_INPUT_BG, 3.0f);
        emit_border(dc, idbuf, x, y, w, h, 1, DIRECT_INPUT_BORDER);
        emit_text(dc, idbuf, x + 8, y + (h - line_h(dc)) / 2, w - 16, line_h(dc),
                  t, DIRECT_TEXT_COLOR);
        return;
    }

    if (strcmp(n->tag, "ListBox") == 0) {
        JpxXaml__NodeId(n, index, idbuf, sizeof(idbuf));
        emit_rect(dc, idbuf, x, y, w, h, DIRECT_INPUT_BG, 3.0f);
        emit_border(dc, idbuf, x, y, w, h, 1, DIRECT_INPUT_BORDER);
        const char *items = attr(n, "Items");
        if (items && *items) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", items);
            char *p = buf;
            int row = 0, lh = line_h(dc);
            char rowId[72];
            while (*p && row < 8 && (row + 1) * lh <= h - 4) {
                char *sep = strchr(p, ';');
                if (sep) *sep = '\0';
                snprintf(rowId, sizeof(rowId), "%s_r%d", idbuf, row);
                emit_text(dc, rowId, x + 8, y + 4 + row * lh, w - 16, lh,
                          p, DIRECT_TEXT_COLOR);
                row++;
                if (!sep) break;
                p = sep + 1;
            }
        }
        return;
    }

    if (strcmp(n->tag, "Separator") == 0) {
        JpxXaml__NodeId(n, index, idbuf, sizeof(idbuf));
        emit_rect(dc, idbuf, x, y, w, 1, DIRECT_INPUT_BORDER, 0.0f);
        return;
    }

    if (strcmp(n->tag, "Spacer") == 0) return;

    /* 未知标签：递归子节点，不报错（与旧回放的降级行为一致）。 */
    flow_children(dc, n, x, y, w, h, 0);
}

void JpxXaml_RenderDirect(JpxXamlView *view, const JpxXamlBackend *backend,
                          void *ctx, int x, int y, int w, int h) {
    DirectCtx dc;
    if (!view || !backend || view->doc.count == 0) return;
    dc.b = backend;
    dc.doc = &view->doc;
    dc.ctx = ctx;
    dc.fontId = DIRECT_FONT_ID;
    dc.fontSize = DIRECT_FONT_SIZE;
    walk(&dc, 0, x, y, w, h);
}
