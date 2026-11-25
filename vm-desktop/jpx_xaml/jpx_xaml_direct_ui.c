/*
 * jpx_xaml_direct_ui.c - Direct 后端的交互层（M7.1）
 *
 * 渲染（jpx_xaml_render_direct.c）记录每个控件的矩形与状态；这里负责命中、
 * 按下/抬起、复选框切换、输入框编辑与列表选中，并把结果写回 model 绑定。
 * 不依赖 JPX / Clay。
 */
#include "jpx_xaml_internal.h"

#include <stdio.h>
#include <string.h>

#define DIRECT_LIST_ROW_H 18

static const JpxXamlNode *node_at(JpxXamlView *v, int i) {
    return JpxXamlDoc_Node(&v->doc, i);
}

static int is_control(const char *tag) {
    return tag && (!strcmp(tag, "Button") || !strcmp(tag, "CheckBox") ||
                   !strcmp(tag, "TextBox") || !strcmp(tag, "ListBox"));
}

static int hit_test(JpxXamlView *v, int x, int y) {
    /* 逆序：后画的在上层。只认可交互控件。 */
    for (int i = v->doc.count - 1; i >= 0; i--) {
        if (!v->nodeHit[i]) continue;
        if (!is_control(node_at(v, i)->tag)) continue;
        if (x >= v->nodeX[i] && x < v->nodeX[i] + v->nodeW[i] &&
            y >= v->nodeY[i] && y < v->nodeY[i] + v->nodeH[i]) {
            return i;
        }
    }
    return -1;
}

/* ---- 绑定读写（与渲染共用同一套 bindings 表） ---- */

static JpxXamlBinding *find_binding(JpxXamlView *v, const JpxXamlNode *n, const char *attrName) {
    if (!v || !v->model || !n) return NULL;
    const char *field = JpxXaml__BindingField(JpxXamlNode_Attr(n, attrName));
    return field ? JpxXaml__FindBinding(v, field) : NULL;
}

static void store_text(JpxXamlView *v, const JpxXamlNode *n, const char *attrName,
                       const char *val) {
    JpxXamlBinding *b = find_binding(v, n, attrName);
    if (!b || b->kind != JPX_XAML_BIND_TEXT) return;
    char *dst = (char *)v->model + b->offset;
    snprintf(dst, b->cap ? b->cap : JPX_XAML_INPUT_CAP, "%s", val ? val : "");
}

static void store_bool(JpxXamlView *v, const JpxXamlNode *n, const char *attrName, int val) {
    JpxXamlBinding *b = find_binding(v, n, attrName);
    if (!b) return;
    if (b->kind == JPX_XAML_BIND_BOOL)
        *(unsigned char *)((char *)v->model + b->offset) = (unsigned char)(val ? 1 : 0);
    else if (b->kind == JPX_XAML_BIND_INT)
        *(int *)((char *)v->model + b->offset) = val ? 1 : 0;
}

static void store_int(JpxXamlView *v, const JpxXamlNode *n, const char *attrName, int val) {
    JpxXamlBinding *b = find_binding(v, n, attrName);
    if (b && b->kind == JPX_XAML_BIND_INT)
        *(int *)((char *)v->model + b->offset) = val;
}

static int read_bool(JpxXamlView *v, const JpxXamlNode *n, const char *attrName) {
    const char *raw = JpxXamlNode_Attr(n, attrName);
    JpxXamlBinding *b = find_binding(v, n, attrName);
    if (b) {
        if (b->kind == JPX_XAML_BIND_BOOL)
            return *(unsigned char *)((char *)v->model + b->offset) ? 1 : 0;
        if (b->kind == JPX_XAML_BIND_INT)
            return *(int *)((char *)v->model + b->offset) != 0;
    }
    return raw && !strcmp(raw, "True");
}

int JpxXaml_DirectPointer(JpxXamlView *view, int x, int y, int down) {
    int hit, dirty = 0;
    if (!view) return 0;
    hit = hit_test(view, x, y);

    if (down) {
        if (view->pressNode >= 0 && view->pressNode != hit &&
            view->pressNode < JPX_XAML_MAX_NODES) {
            view->direct[view->pressNode].pressed = 0;
            dirty = 1;
        }
        view->pressNode = hit;
        if (hit >= 0) {
            const char *tag = node_at(view, hit)->tag;
            view->direct[hit].pressed = 1;
            /* 点输入框即聚焦；点别处清焦点。 */
            if (!strcmp(tag, "TextBox")) {
                view->focusNode = hit;
                view->direct[hit].focused = 1;
            } else {
                if (view->focusNode >= 0) view->direct[view->focusNode].focused = 0;
                view->focusNode = -1;
            }
            dirty = 1;
        } else if (view->focusNode >= 0) {
            view->direct[view->focusNode].focused = 0;
            view->focusNode = -1;
            dirty = 1;
        }
        return dirty;
    }

    /* 抬起：同一节点上才算 click。 */
    if (view->pressNode >= 0 && view->pressNode < JPX_XAML_MAX_NODES) {
        int i = view->pressNode;
        const char *tag = node_at(view, i)->tag;
        view->direct[i].pressed = 0;
        dirty = 1;
        if (i == hit) {
            if (!strcmp(tag, "Button")) {
                JpxXaml__Fire(view, i, "Click");
            } else if (!strcmp(tag, "CheckBox")) {
                int next = !read_bool(view, node_at(view, i), "IsChecked");
                if (find_binding(view, node_at(view, i), "IsChecked"))
                    store_bool(view, node_at(view, i), "IsChecked", next);
                else
                    view->direct[i].checked = next;
                JpxXaml__Fire(view, i, "Click");
            } else if (!strcmp(tag, "ListBox")) {
                int row = (y - view->nodeY[i] - 4) / DIRECT_LIST_ROW_H;
                if (row >= 0) {
                    view->direct[i].selected = row;
                    store_int(view, node_at(view, i), "SelectedIndex", row);
                    JpxXaml__Fire(view, i, "SelectionChanged");
                }
            }
        }
    }
    view->pressNode = -1;
    return dirty;
}

int JpxXaml_DirectKey(JpxXamlView *view, const char *key, int ctrl, int shift) {
    int i;
    JpxXamlDirectState *st;
    const JpxXamlNode *n;
    (void)ctrl;
    (void)shift;
    if (!view || view->focusNode < 0) return 0;
    i = view->focusNode;
    n = node_at(view, i);
    if (!n || strcmp(n->tag, "TextBox") != 0) return 0;
    if (!key || !*key) return 0;

    st = &view->direct[i];
    if (!st->inited) {
        st->inited = 1;
        st->text[0] = '\0';
        st->len = 0;
    }

    if (!strcmp(key, "Backspace")) {
        if (st->len == 0) return 0;
        size_t k = st->len - 1;
        while (k > 0 && ((unsigned char)st->text[k] & 0xC0) == 0x80) k--;
        st->text[k] = '\0';
        st->len = k;
    } else if (!strcmp(key, "Enter") || !strcmp(key, "Tab")) {
        return 0;
    } else {
        size_t add = strlen(key);
        if (st->len + add + 1 >= sizeof(st->text)) return 0;
        memcpy(st->text + st->len, key, add);
        st->len += add;
        st->text[st->len] = '\0';
    }

    store_text(view, n, "Text", st->text);
    JpxXaml__Fire(view, i, "TextChanged");
    return 1;
}

void JpxXaml_DirectSetCaret(JpxXamlView *view, int on) {
    if (view) view->directCaretOn = on ? 1 : 0;
}
