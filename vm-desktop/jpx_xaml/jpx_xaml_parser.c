#include "jpx_xaml_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void JpxXamlDoc_Init(JpxXamlDoc *doc) {
    if (!doc) return;
    memset(doc, 0, sizeof(*doc));
}

const JpxXamlNode *JpxXamlDoc_Node(const JpxXamlDoc *doc, int index) {
    if (!doc || index < 0 || index >= doc->count) return NULL;
    return &doc->nodes[index];
}

const char *JpxXamlNode_Attr(const JpxXamlNode *node, const char *name) {
    if (!node || !name) return NULL;
    for (int i = 0; i < node->attrCount; i++) {
        if (strcmp(node->attrs[i].name, name) == 0) return node->attrs[i].value;
    }
    return NULL;
}

const char *JpxXamlNode_Text(const JpxXamlNode *node) {
    return node ? node->text : "";
}

/* ---------------- 扫描器 ---------------- */

typedef struct {
    const char *p;
    int line;
    JpxXamlDoc *doc;
    int stack[JPX_XAML_MAX_NODES];
    int depth;
    char *err;
    size_t errCap;
} Scanner;

static void fail(Scanner *s, const char *msg) {
    if (s->err && s->errCap) snprintf(s->err, s->errCap, "line %d: %s", s->line, msg);
}

static void skip_ws(Scanner *s) {
    while (*s->p) {
        if (*s->p == '\n') { s->line++; s->p++; continue; }
        if (isspace((unsigned char)*s->p)) { s->p++; continue; }
        break;
    }
}

static int starts(const Scanner *s, const char *token) {
    return strncmp(s->p, token, strlen(token)) == 0;
}

static int read_name(Scanner *s, char *out, size_t cap) {
    size_t n = 0;
    while (*s->p && n + 1 < cap) {
        char c = *s->p;
        if (isalnum((unsigned char)c) || c == '_' || c == ':' || c == '-' || c == '.') {
            out[n++] = c;
            s->p++;
        } else {
            break;
        }
    }
    out[n] = '\0';
    return n > 0;
}

/* 解码实体；未知实体返回 0。&amp; 等只处理五个固定名称。 */
static int decode_entities(const char *src, size_t len, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '&') {
            if (o + 1 >= cap) return 0;
            out[o++] = src[i];
            continue;
        }
        static const struct { const char *seq; char ch; } ents[] = {
            {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'},
            {"&quot;", '"'}, {"&apos;", '\''}
        };
        int matched = 0;
        for (size_t e = 0; e < sizeof(ents) / sizeof(ents[0]); e++) {
            size_t sl = strlen(ents[e].seq);
            if (i + sl <= len && memcmp(src + i, ents[e].seq, sl) == 0) {
                if (o + 1 >= cap) return 0;
                out[o++] = ents[e].ch;
                i += sl - 1;
                matched = 1;
                break;
            }
        }
        if (!matched) return 0;
    }
    out[o] = '\0';
    return 1;
}

static int skip_comment(Scanner *s) {
    const char *end = strstr(s->p + 4, "-->");
    if (!end) { fail(s, "unterminated comment"); return 0; }
    while (s->p < end) { if (*s->p == '\n') s->line++; s->p++; }
    s->p += 3;
    return 1;
}

static int read_attr_value(Scanner *s, char *out, size_t cap) {
    skip_ws(s);
    if (*s->p != '=') { fail(s, "expected '=' after attribute name"); return 0; }
    s->p++;
    skip_ws(s);
    char quote = *s->p;
    if (quote != '"' && quote != '\'') { fail(s, "attribute value must be quoted"); return 0; }
    s->p++;
    const char *start = s->p;
    while (*s->p && *s->p != quote) {
        if (*s->p == '\n') s->line++;
        s->p++;
    }
    if (*s->p != quote) { fail(s, "unterminated attribute value"); return 0; }
    size_t len = (size_t)(s->p - start);
    s->p++;
    if (!decode_entities(start, len, out, cap)) {
        fail(s, "invalid entity in attribute value");
        return 0;
    }
    return 1;
}

static int add_node(Scanner *s, const char *tag, int line, int *outIndex) {
    JpxXamlDoc *doc = s->doc;
    if (doc->count >= JPX_XAML_MAX_NODES) { fail(s, "too many nodes"); return 0; }
    int idx = doc->count++;
    JpxXamlNode *node = &doc->nodes[idx];
    memset(node, 0, sizeof(*node));
    /* index 0 是有效节点，树指针必须用 -1 表示「无」，不能用 0。 */
    node->parent = -1;
    node->firstChild = -1;
    node->lastChild = -1;
    node->nextSibling = -1;
    snprintf(node->tag, sizeof(node->tag), "%s", tag);
    node->line = line;
    node->parent = s->depth > 0 ? s->stack[s->depth - 1] : -1;
    if (node->parent >= 0) {
        JpxXamlNode *parent = &doc->nodes[node->parent];
        if (parent->lastChild >= 0) doc->nodes[parent->lastChild].nextSibling = idx;
        else parent->firstChild = idx;
        parent->lastChild = idx;
    }
    *outIndex = idx;
    return 1;
}

static int add_text(Scanner *s, const char *start, size_t len) {
    if (len == 0 || s->depth <= 0) return 1;
    JpxXamlNode *node = &s->doc->nodes[s->stack[s->depth - 1]];
    size_t cur = strlen(node->text);
    char decoded[JPX_XAML_TEXT_CAP];
    if (!decode_entities(start, len, decoded, sizeof(decoded))) {
        fail(s, "invalid entity in text");
        return 0;
    }
    size_t add = strlen(decoded);
    if (cur + add >= sizeof(node->text)) { fail(s, "text too long"); return 0; }
    memcpy(node->text + cur, decoded, add + 1);
    return 1;
}

static int parse_start_tag(Scanner *s) {
    int line = s->line;
    s->p++;  /* '<' */
    char tag[JPX_XAML_TAG_CAP];
    if (!read_name(s, tag, sizeof(tag))) { fail(s, "expected tag name"); return 0; }

    int idx = -1;
    if (!add_node(s, tag, line, &idx)) return 0;
    JpxXamlNode *node = &s->doc->nodes[idx];

    int selfClosing = 0;
    for (;;) {
        skip_ws(s);
        if (!*s->p) { fail(s, "unexpected end of input in tag"); return 0; }
        if (starts(s, "/>")) { s->p += 2; selfClosing = 1; break; }
        if (*s->p == '>') { s->p++; break; }
        char name[JPX_XAML_ATTR_NAME_CAP];
        if (!read_name(s, name, sizeof(name))) { fail(s, "expected attribute name"); return 0; }
        if (node->attrCount >= JPX_XAML_MAX_ATTRS) { fail(s, "too many attributes"); return 0; }
        char value[JPX_XAML_ATTR_VALUE_CAP];
        if (!read_attr_value(s, value, sizeof(value))) return 0;
        JpxXamlAttr *a = &node->attrs[node->attrCount++];
        snprintf(a->name, sizeof(a->name), "%s", name);
        snprintf(a->value, sizeof(a->value), "%s", value);
    }

    if (!selfClosing) {
        if (s->depth >= JPX_XAML_MAX_NODES) { fail(s, "tree too deep"); return 0; }
        s->stack[s->depth++] = idx;
    }
    return 1;
}

static int parse_end_tag(Scanner *s) {
    s->p += 2;  /* '</' */
    char tag[JPX_XAML_TAG_CAP];
    if (!read_name(s, tag, sizeof(tag))) { fail(s, "expected closing tag name"); return 0; }
    skip_ws(s);
    if (*s->p != '>') { fail(s, "expected '>' in closing tag"); return 0; }
    s->p++;
    if (s->depth <= 0) { fail(s, "closing tag without open tag"); return 0; }
    int idx = s->stack[--s->depth];
    if (strcmp(s->doc->nodes[idx].tag, tag) != 0) {
        fail(s, "closing tag does not match open tag");
        return 0;
    }
    return 1;
}

int JpxXamlDoc_Parse(JpxXamlDoc *doc, const char *src, char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!doc || !src) return 0;
    JpxXamlDoc_Init(doc);

    Scanner s;
    memset(&s, 0, sizeof(s));
    s.p = src;
    s.line = 1;
    s.doc = doc;
    s.err = err;
    s.errCap = errCap;

    skip_ws(&s);
    if (starts(&s, "<?xml")) {
        const char *end = strstr(s.p, "?>");
        if (!end) { fail(&s, "unterminated XML declaration"); return 0; }
        while (s.p < end) { if (*s.p == '\n') s.line++; s.p++; }
        s.p += 2;
        skip_ws(&s);
    }

    while (*s.p) {
        if (starts(&s, "<!--")) {
            if (!skip_comment(&s)) return 0;
            continue;
        }
        if (starts(&s, "</")) {
            if (!parse_end_tag(&s)) return 0;
            continue;
        }
        if (*s.p == '<') {
            if (!parse_start_tag(&s)) return 0;
            continue;
        }
        const char *start = s.p;
        while (*s.p && *s.p != '<') {
            if (*s.p == '\n') s.line++;
            s.p++;
        }
        if (!add_text(&s, start, (size_t)(s.p - start))) return 0;
    }

    if (s.depth != 0) { fail(&s, "unclosed tag at end of input"); return 0; }
    return doc->count > 0;
}
