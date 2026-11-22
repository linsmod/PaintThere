#include "jpx_xaml_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 视图生命周期、绑定/handler 表、节点查询、辅助函数。
   本 TU 不调用任何 Jpx_* 函数，因此可独立做逻辑测试。 */

const char *JpxXamlNode_Tag(const JpxXamlNode *node) { return node ? node->tag : ""; }

JpxXamlBinding *JpxXaml__FindBinding(JpxXamlView *view, const char *name) {
    if (!view || !name) return NULL;
    for (int i = 0; i < JPX_XAML_MAX_BINDINGS; i++) {
        JpxXamlBinding *b = &view->bindings[i];
        if (b->used && strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}

JpxXamlHandlerSlot *JpxXaml__FindHandler(JpxXamlView *view, const char *name) {
    if (!view || !name) return NULL;
    for (int i = 0; i < JPX_XAML_MAX_HANDLERS; i++) {
        JpxXamlHandlerSlot *h = &view->handlers[i];
        if (h->used && strcmp(h->name, name) == 0) return h;
    }
    return NULL;
}

JpxXamlView *JpxXamlView_Create(JpxApp *app, const char *xaml, void *model,
                                char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!xaml) return NULL;
    JpxXamlView *view = (JpxXamlView *)calloc(1, sizeof(*view));
    if (!view) return NULL;
    if (!JpxXamlDoc_Parse(&view->doc, xaml, err, errCap)) {
        free(view);
        return NULL;
    }
    view->app = app;
    view->model = model;
    return view;
}

void JpxXamlView_Destroy(JpxXamlView *view) { free(view); }

void *JpxXamlView_Model(JpxXamlView *view) { return view ? view->model : NULL; }
JpxApp *JpxXamlView_App(JpxXamlView *view) { return view ? view->app : NULL; }

static int blank_name(const char *name) {
    if (!name || !*name) return 1;
    for (const char *p = name; *p; p++) {
        if (!isspace((unsigned char)*p)) return 0;
    }
    return 1;
}

int JpxXamlView_Bind(JpxXamlView *view, const char *name, JpxXamlBindKind kind,
                     size_t offset, size_t cap) {
    if (!view || blank_name(name)) return 0;
    JpxXamlBinding *b = JpxXaml__FindBinding(view, name);
    if (!b) {
        for (int i = 0; i < JPX_XAML_MAX_BINDINGS; i++) {
            if (!view->bindings[i].used) { b = &view->bindings[i]; break; }
        }
    }
    if (!b) return 0;
    b->used = 1;
    snprintf(b->name, sizeof(b->name), "%s", name);
    b->kind = kind;
    b->offset = offset;
    b->cap = cap;
    return 1;
}

int JpxXamlView_On(JpxXamlView *view, const char *name,
                   JpxXamlHandler fn, void *userData) {
    if (!view || blank_name(name) || !fn) return 0;
    JpxXamlHandlerSlot *h = JpxXaml__FindHandler(view, name);
    if (!h) {
        for (int i = 0; i < JPX_XAML_MAX_HANDLERS; i++) {
            if (!view->handlers[i].used) { h = &view->handlers[i]; break; }
        }
    }
    if (!h) return 0;
    h->used = 1;
    snprintf(h->name, sizeof(h->name), "%s", name);
    h->fn = fn;
    h->userData = userData;
    return 1;
}

int JpxXamlView_NodeCount(JpxXamlView *view) { return view ? view->doc.count : 0; }
const JpxXamlNode *JpxXamlView_Node(JpxXamlView *view, int index) {
    return JpxXamlDoc_Node(view ? &view->doc : NULL, index);
}

const char *JpxXaml__BindingField(const char *value) {
    static const char prefix[] = "{Binding ";
    if (!value || strncmp(value, prefix, sizeof(prefix) - 1) != 0) return NULL;
    const char *p = value + sizeof(prefix) - 1;
    while (*p == ' ') p++;
    const char *end = p;
    while (*end && *end != '}' && *end != '.') end++;
    if (*end != '}') return NULL;
    size_t len = (size_t)(end - p);
    if (len == 0) return NULL;
    static char name[JPX_XAML_ATTR_NAME_CAP];
    if (len >= sizeof(name)) return NULL;
    memcpy(name, p, len);
    name[len] = '\0';
    return name;
}

void JpxXaml__Fire(JpxXamlView *view, int nodeIndex, const char *eventAttr) {
    const JpxXamlNode *node = JpxXamlDoc_Node(&view->doc, nodeIndex);
    const char *name = JpxXamlNode_Attr(node, eventAttr);
    if (!name || !*name) return;
    JpxXamlHandlerSlot *h = JpxXaml__FindHandler(view, name);
    if (h) h->fn(view, nodeIndex, h->userData);
}

const char *JpxXaml__NodeId(const JpxXamlNode *node, int index,
                            char *buf, size_t cap) {
    const char *name = JpxXamlNode_Attr(node, "Name");
    if (!name) name = JpxXamlNode_Attr(node, "x:Name");
    if (name && *name) snprintf(buf, cap, "%s", name);
    else snprintf(buf, cap, "n%d", index);
    return buf;
}
