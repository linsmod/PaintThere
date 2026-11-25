/* DEPRECATED（2026-11-25）：old infra 的执行层（回放为 Jpx_*）。
   parser/state 会被新 app 复用；本文件随旧 app 在 M7 删除。 */

#include "jpx_xaml_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 每帧把元素树回放成 JPX 调用。这是唯一调用 Jpx_* 的 jp_xaml TU。 */

/* ---------------- 回放辅助 ---------------- */

static void render_text_input(JpxXamlView *view, int win, int index,
                              const JpxXamlNode *node) {
    char idbuf[JPX_XAML_ATTR_NAME_CAP + 16];
    const char *id = JpxXaml__NodeId(node, index, idbuf, sizeof(idbuf));
    JpxXamlInputSlot *slot = &view->inputs[index];
    if (!slot->used) {
        slot->used = 1;
        const char *initial = JpxXamlNode_Attr(node, "Text");
        if (!initial) initial = JpxXamlNode_Text(node);
        if (initial) snprintf(slot->buf, sizeof(slot->buf), "%s", initial);
        slot->len = strlen(slot->buf);
        snprintf(slot->synced, sizeof(slot->synced), "%s", slot->buf);
    }

    const char *field = JpxXaml__BindingField(JpxXamlNode_Attr(node, "Text"));
    JpxXamlBinding *b = field ? JpxXaml__FindBinding(view, field) : NULL;
    if (b && b->kind == JPX_XAML_BIND_TEXT && view->model) {
        char *modelText = (char *)view->model + b->offset;
        size_t modelLen = strnlen(modelText, b->cap ? b->cap : JPX_XAML_INPUT_CAP);
        /* handler 改了 model：把新值灌回稳定 buf，避免编辑状态被误判。 */
        if (modelLen != slot->len || memcmp(modelText, slot->synced, modelLen) != 0) {
            size_t n = modelLen < sizeof(slot->buf) - 1 ? modelLen : sizeof(slot->buf) - 1;
            memcpy(slot->buf, modelText, n);
            slot->buf[n] = '\0';
            slot->len = n;
            snprintf(slot->synced, sizeof(slot->synced), "%s", slot->buf);
        }
    }

    /* 必须按 buf 的实际长度传：Jpx_TextInput 用 ed.len != *len 判断外部改写，
       传旧缓存值会在按键后的下一帧把编辑状态重建为空，抹掉刚输入的字符。 */
    size_t len = strlen(slot->buf);
    Jpx_TextInput(win, id, slot->buf, sizeof(slot->buf), &len);
    if (len != slot->len || strcmp(slot->buf, slot->synced) != 0) {
        slot->len = len < sizeof(slot->buf) ? len : sizeof(slot->buf) - 1;
        slot->buf[slot->len] = '\0';
        snprintf(slot->synced, sizeof(slot->synced), "%s", slot->buf);
        if (b && b->kind == JPX_XAML_BIND_TEXT && view->model) {
            char *modelText = (char *)view->model + b->offset;
            size_t cap = b->cap ? b->cap : JPX_XAML_INPUT_CAP;
            snprintf(modelText, cap, "%s", slot->buf);
        }
        JpxXaml__Fire(view, index, "TextChanged");
    }
}

static void render_checkbox(JpxXamlView *view, int win, int index,
                            const JpxXamlNode *node) {
    char idbuf[JPX_XAML_ATTR_NAME_CAP + 16];
    const char *id = JpxXaml__NodeId(node, index, idbuf, sizeof(idbuf));
    const char *label = JpxXamlNode_Attr(node, "Content");
    if (!label) label = JpxXamlNode_Text(node);

    const char *field = JpxXaml__BindingField(JpxXamlNode_Attr(node, "IsChecked"));
    JpxXamlBinding *b = field ? JpxXaml__FindBinding(view, field) : NULL;
    int checked = 0;
    const char *literal = JpxXamlNode_Attr(node, "IsChecked");
    if (literal && strcmp(literal, "True") == 0) checked = 1;
    if (b && view->model) {
        if (b->kind == JPX_XAML_BIND_BOOL)
            checked = *(unsigned char *)((char *)view->model + b->offset) ? 1 : 0;
        else if (b->kind == JPX_XAML_BIND_INT)
            checked = *(int *)((char *)view->model + b->offset) ? 1 : 0;
    }

    if (Jpx_Checkbox(win, id, label ? label : "", &checked)) {
        if (b && view->model) {
            if (b->kind == JPX_XAML_BIND_BOOL)
                *(unsigned char *)((char *)view->model + b->offset) = (unsigned char)checked;
            else if (b->kind == JPX_XAML_BIND_INT)
                *(int *)((char *)view->model + b->offset) = checked;
        }
        JpxXaml__Fire(view, index, "Click");
    }
}

static void render_listbox(JpxXamlView *view, int win, int index,
                           const JpxXamlNode *node) {
    char idbuf[JPX_XAML_ATTR_NAME_CAP + 16];
    const char *id = JpxXaml__NodeId(node, index, idbuf, sizeof(idbuf));
    const char *itemsAttr = JpxXamlNode_Attr(node, "Items");
    static char itemsBuf[JPX_XAML_ATTR_VALUE_CAP];
    const char *items[JPX_XAML_MAX_ITEMS];
    int count = 0;
    if (itemsAttr) {
        snprintf(itemsBuf, sizeof(itemsBuf), "%s", itemsAttr);
        char *p = itemsBuf;
        if (*p) items[count++] = p;
        while (*p && count < JPX_XAML_MAX_ITEMS) {
            if (*p == ';') { *p = '\0'; items[count++] = p + 1; }
            p++;
        }
    }

    const char *field = JpxXaml__BindingField(JpxXamlNode_Attr(node, "SelectedIndex"));
    JpxXamlBinding *b = field ? JpxXaml__FindBinding(view, field) : NULL;
    int selected = 0;
    if (b && b->kind == JPX_XAML_BIND_INT && view->model)
        selected = *(int *)((char *)view->model + b->offset);
    else {
        const char *lit = JpxXamlNode_Attr(node, "SelectedIndex");
        if (lit) selected = atoi(lit);
    }
    int rawSelected = selected;   /* -1 = 无初始选中；只有平铺 ListBox 需要钳 0 */
    if (selected < 0) selected = 0;

    const char *rowsAttr = JpxXamlNode_Attr(node, "Rows");
    /* Rows 属性限制可视行数：超出部分走 Jpx_ListBegin 的可滚动变体
       （滚轮翻页，固定行高 22 + 2 间距）。不带 Rows 时保持原行为：
       全部条目平铺渲染。 */
    int visibleRows = rowsAttr ? atoi(rowsAttr) : count;
    int changed = 0;
    if (rowsAttr && count > 0) {
        const int rowPitch = 24;   /* 行高 22 + 2 间距 */
        if (Jpx_ListBegin(win, id, visibleRows * rowPitch + 4, count)) {
            for (int i = 0; i < count; i++) {
                if (Jpx_ListRow(win, id, &items[i], NULL, 1, rawSelected == i)) {
                    selected = rawSelected = i;
                    changed = 1;
                }
            }
            Jpx_ListEnd(win);
        }
    } else if (Jpx_ListBox(win, id, items, count, &selected, visibleRows)) {
        changed = 1;
    }
    if (changed) {
        if (b && b->kind == JPX_XAML_BIND_INT && view->model)
            *(int *)((char *)view->model + b->offset) = selected;
        JpxXaml__Fire(view, index, "SelectionChanged");
    }
}

static void render_node(JpxXamlView *view, int win, int index) {
    const JpxXamlNode *node = JpxXamlDoc_Node(&view->doc, index);
    if (!node) return;
    const char *tag = node->tag;

    if (strcmp(tag, "Window") == 0) {
        for (int c = node->firstChild; c >= 0; c = view->doc.nodes[c].nextSibling)
            render_node(view, win, c);
        return;
    }
    if (strcmp(tag, "StackPanel") == 0) {
        const char *orient = JpxXamlNode_Attr(node, "Orientation");
        int horizontal = orient && strcmp(orient, "Horizontal") == 0;
        if (horizontal) Jpx_RowBegin(win); else Jpx_ColumnBegin(win);
        for (int c = node->firstChild; c >= 0; c = view->doc.nodes[c].nextSibling)
            render_node(view, win, c);
        if (horizontal) Jpx_RowEnd(win); else Jpx_ColumnEnd(win);
        return;
    }
    if (strcmp(tag, "Border") == 0) {
        Jpx_PanelBegin(win);
        for (int c = node->firstChild; c >= 0; c = view->doc.nodes[c].nextSibling)
            render_node(view, win, c);
        Jpx_PanelEnd(win);
        return;
    }
    if (strcmp(tag, "TextBlock") == 0) {
        const char *text = JpxXamlNode_Attr(node, "Text");
        if (!text) text = JpxXamlNode_Text(node);
        const char *field = JpxXaml__BindingField(text);
        JpxXamlBinding *b = field ? JpxXaml__FindBinding(view, field) : NULL;
        if (b && b->kind == JPX_XAML_BIND_TEXT && view->model)
            text = (const char *)view->model + b->offset;
        JpxTextStyle style = {0};
        const char *wrap = JpxXamlNode_Attr(node, "Wrap");
        const char *align = JpxXamlNode_Attr(node, "Align");
        style.wrap = !(wrap && strcmp(wrap, "False") == 0);
        if (align) style.align = atoi(align);
        Jpx_LabelEx(win, text ? text : "", &style);
        return;
    }
    if (strcmp(tag, "Button") == 0) {
        char idbuf[JPX_XAML_ATTR_NAME_CAP + 16];
        const char *id = JpxXaml__NodeId(node, index, idbuf, sizeof(idbuf));
        const char *text = JpxXamlNode_Attr(node, "Content");
        if (!text) text = JpxXamlNode_Text(node);
        if (Jpx_Button(win, id, text ? text : "")) JpxXaml__Fire(view, index, "Click");
        return;
    }
    if (strcmp(tag, "CheckBox") == 0) { render_checkbox(view, win, index, node); return; }
    if (strcmp(tag, "TextBox") == 0) { render_text_input(view, win, index, node); return; }
    if (strcmp(tag, "ListBox") == 0) { render_listbox(view, win, index, node); return; }
    if (strcmp(tag, "Separator") == 0) { Jpx_Separator(win); return; }
    if (strcmp(tag, "Spacer") == 0) {
        const char *w = JpxXamlNode_Attr(node, "Width");
        const char *h = JpxXamlNode_Attr(node, "Height");
        Jpx_Spacer(win, w ? atoi(w) : 0, h ? atoi(h) : 0);
        return;
    }
    for (int c = node->firstChild; c >= 0; c = view->doc.nodes[c].nextSibling)
        render_node(view, win, c);
}

void JpxXaml_Render(JpxXamlView *view, int winId) {
    if (!view || view->doc.count == 0) return;
    for (int c = view->doc.nodes[0].firstChild; c >= 0; c = view->doc.nodes[c].nextSibling)
        render_node(view, winId, c);
    /* 不在这里无条件置脏：JPX 控件只在交互发生时自己置脏，空闲必须零帧。
       handler 改了状态后由 handler 显式 Jpx_Invalidate。 */
}
