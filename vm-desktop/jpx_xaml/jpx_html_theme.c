/* CSS 子集主题编译器：theme.css → JpxXamlResourceTable → WritePrecompiled。
   协议（资源表结构、三件套格式、预定义 tagId）与 jp_xaml 工具链一致；
   本文件是独立解析实现，不引用 jp_xaml 的 XAML 解析器。 */

#include "jpx_html_theme.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSS_MAX_DECLARATIONS JPX_XAML_MAX_RESOURCES

/* 与 jpx_xaml_theme.c 的 add_res 语义一致（先到先得），但独立实现：
   两个工具链只共享协议，不共享解析代码。 */
static JpxXamlResource *html_add(JpxXamlResourceTable *table, const char *key,
                                 JpxXamlResKind kind, char *err, size_t errCap,
                                 int *fresh) {
    *fresh = 0;
    if (!key || !*key) { snprintf(err, errCap, "empty key"); return NULL; }
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].key, key) == 0) return &table->items[i];
    }
    if (table->count >= JPX_XAML_MAX_RESOURCES) {
        snprintf(err, errCap, "too many resources (css)");
        return NULL;
    }
    JpxXamlResource *r = &table->items[table->count++];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->kind = kind;
    snprintf(r->key, sizeof(r->key), "%s", key);
    *fresh = 1;
    return r;
}

static char *skip_ws(char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

/* 按顶层逗号切分（括号深度归零才算分隔；单引号串内不分隔）。 */
static int split_top_commas(char *s, char *parts[], int maxParts) {
    int n = 0;
    int depth = 0, inQuote = 0;
    parts[n++] = s;
    for (; *s; s++) {
        if (inQuote) {
            if (*s == '\'') inQuote = 0;
            continue;
        }
        if (*s == '\'') { inQuote = 1; continue; }
        if (*s == '(') { depth++; continue; }
        if (*s == ')') { depth--; continue; }
        if (*s == ',' && depth == 0 && n < maxParts) {
            *s = '\0';
            parts[n++] = s + 1;
        }
    }
    return n;
}

static char *trim(char *s) {
    s = skip_ws(s);
    size_t n = strlen(s);
    while (n && (unsigned char)s[n - 1] <= ' ') s[--n] = '\0';
    return s;
}

/* 取 "(...)" 内的内容：p 指向 '('，返回 ')' 之后的位置；内容原地截断。 */
static char *paren_inner(char *p, char **inner) {
    if (*p != '(') return NULL;
    *inner = p + 1;
    int depth = 0, inQuote = 0;
    for (; *p; p++) {
        if (inQuote) {
            if (*p == '\'') inQuote = 0;
            continue;
        }
        if (*p == '\'') { inQuote = 1; continue; }
        if (*p == '(') depth++;
        else if (*p == ')' && --depth == 0) {
            *p = '\0';
            return p + 1;
        }
    }
    return NULL;
}

static int parse_stops(char *spec, JpxXamlResource *r, char *err, size_t errCap);

/* 解析 "<gradient>(dir(...), stops...)"：t 指向 "<gradient>(" 之后。
   dir 内容逐字回显（导出侧保证精确），stops 交给 parse_stops。 */
static int parse_gradient_body(char *rest, const char *key, JpxXamlResource *r,
                               char *err, size_t errCap) {
    if (strncmp(rest, "dir(", 4) != 0) {
        snprintf(err, errCap, "%s: expected dir(...)", key);
        return 0;
    }
    char *inner;
    char *after = paren_inner(rest + 3, &inner);
    if (!after) {
        snprintf(err, errCap, "%s: unterminated dir()", key);
        return 0;
    }
    char *d = trim(inner);
    for (char *q = d; *q; q++) {
        if (*q != ',' && *q != '.' && *q != '-' && *q != '+' &&
            !isdigit((unsigned char)*q) && *q != ' ') {
            snprintf(err, errCap, "%s: bad dir() content", key);
            return 0;
        }
    }
    if (!*d || strlen(d) >= sizeof(r->direction)) {
        snprintf(err, errCap, "%s: bad dir() length", key);
        return 0;
    }
    snprintf(r->direction, sizeof(r->direction), "%s", d);
    if (*after != ',') {
        snprintf(err, errCap, "%s: missing stops after dir()", key);
        return 0;
    }
    /* 渐变值以收尾的 ')' 结束（canonical），stops 里颜色括号都已配平，
       最后一个 ')' 一定是渐变收尾，剥掉再切段。 */
    char *spec = after + 1;
    size_t sl = strlen(spec);
    if (sl && spec[sl - 1] == ')') spec[sl - 1] = '\0';
    return parse_stops(spec, r, err, errCap);
}

/* 解析渐变 stops："COLOR off" 逐段；COLOR 是 rgba(...)/#hex（无空格内嵌）。 */
static int parse_stops(char *spec, JpxXamlResource *r, char *err, size_t errCap) {
    char *parts[VM_PATTERN_MAX_STOPS];
    int n = split_top_commas(spec, parts, VM_PATTERN_MAX_STOPS);
    if (n < 2) { snprintf(err, errCap, "gradient needs >= 2 stops"); return 0; }
    for (int i = 0; i < n; i++) {
        char *t = trim(parts[i]);
        char *sp = t;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (!*sp) { snprintf(err, errCap, "stop %d missing offset", i); return 0; }
        *sp++ = '\0';
        char *off = trim(sp);
        char *end;
        double d = strtod(off, &end);
        if (end == off || *trim(end)) {
            snprintf(err, errCap, "stop %d bad offset", i);
            return 0;
        }
        r->stops[i].offset = (float)d;
        snprintf(r->stops[i].color, sizeof(r->stops[i].color), "%s", t);
    }
    r->stopCount = n;
    return 1;
}

/* spec 指向 "path(" 之后："x0,y0,x1,y1, 'data')"（值以收尾 ')' 结束，canonical）。 */
static int parse_path_value(char *spec, JpxXamlResource *r, char *err, size_t errCap) {
    size_t sl = strlen(spec);
    if (!sl || spec[sl - 1] != ')') {
        snprintf(err, errCap, "path() missing close paren");
        return 0;
    }
    spec[sl - 1] = '\0';
    char *parts[8];
    int n = split_top_commas(spec, parts, 8);
    if (n != 5) { snprintf(err, errCap, "path needs bbox(4) + data"); return 0; }
    for (int k = 0; k < 4; k++) {
        char *end;
        double d = strtod(trim(parts[k]), &end);
        if (end == trim(parts[k]) || *trim(end)) {
            snprintf(err, errCap, "path bbox %d bad", k);
            return 0;
        }
        r->bbox[k] = (float)d;
    }
    char *data = trim(parts[4]);
    size_t ln = strlen(data);
    if (ln < 2 || data[0] != '\'' || data[ln - 1] != '\'') {
        snprintf(err, errCap, "path data must be single-quoted");
        return 0;
    }
    data[ln - 1] = '\0';
    snprintf(r->value, sizeof(r->value), "%s", data + 1);
    return 1;
}

/* 值分派：返回 1 成功；kind 由值语法决定（严格，未知形态报错保证确定性）。 */
static int parse_value(char *val, const char *key, JpxXamlResourceTable *table,
                       char *err, size_t errCap) {
    char *t = trim(val);
    int fresh = 0;
    JpxXamlResource *r;

    if (strncmp(t, "linear-gradient(", 16) == 0) {
        r = html_add(table, key, JPX_XAML_RES_LINEAR_GRADIENT, err, errCap, &fresh);
        if (!r) return 0;
        if (!fresh) return 1;
        return parse_gradient_body(t + 16, key, r, err, errCap);
    }
    if (strncmp(t, "radial-gradient(", 16) == 0) {
        r = html_add(table, key, JPX_XAML_RES_RADIAL_GRADIENT, err, errCap, &fresh);
        if (!r) return 0;
        if (!fresh) return 1;
        return parse_gradient_body(t + 16, key, r, err, errCap);
    }
    if (strncmp(t, "path(", 5) == 0) {
        r = html_add(table, key, JPX_XAML_RES_PATH, err, errCap, &fresh);
        if (!r) return 0;
        if (!fresh) return 1;
        if (!parse_path_value(t + 5, r, err, errCap)) return 0;
        return 1;
    }
    if (strncmp(t, "url(", 4) == 0) {
        r = html_add(table, key, JPX_XAML_RES_IMAGE, err, errCap, &fresh);
        if (!r) return 0;
        if (!fresh) return 1;
        char *close = strchr(t + 4, ')');
        if (!close) { snprintf(err, errCap, "%s: url missing )", key); return 0; }
        *close = '\0';
        char *u = trim(t + 4);
        if (!*u || *u == '\'' || *u == '"') {
            snprintf(err, errCap, "%s: url() takes bare relative path", key);
            return 0;
        }
        snprintf(r->uri, sizeof(r->uri), "%s", u);
        return 1;
    }
    if (t[0] == '\'') {
        size_t ln = strlen(t);
        if (ln < 2 || t[ln - 1] != '\'') {
            snprintf(err, errCap, "%s: string must close quote", key);
            return 0;
        }
        t[ln - 1] = '\0';
        r = html_add(table, key, JPX_XAML_RES_STRING, err, errCap, &fresh);
        if (!r) return 0;
        if (fresh) snprintf(r->value, sizeof(r->value), "%s", t + 1);
        return 1;
    }
    if (t[0] == '#' || strncmp(t, "rgba(", 5) == 0) {
        r = html_add(table, key, JPX_XAML_RES_COLOR, err, errCap, &fresh);
        if (!r) return 0;
        if (fresh) snprintf(r->color, sizeof(r->color), "%s", t);
        return 1;
    }
    /* 数字：整串必须能完整解析为浮点。 */
    {
        char *end;
        double d = strtod(t, &end);
        if (end != t && *trim(end) == '\0') {
            r = html_add(table, key, JPX_XAML_RES_NUMBER, err, errCap, &fresh);
            if (!r) return 0;
            if (fresh) r->number = d;
            return 1;
        }
    }
    /* 其余裸 token：命名颜色（XAML 侧非 # 值原样透传，如 Transparent、sc# 系列串）。
       STRING 导出恒带引号，裸 token 不会与之混淆；颜色值按原串收录保证字节一致。 */
    if (!*t) {
        snprintf(err, errCap, "%s: empty value", key);
        return 0;
    }
    r = html_add(table, key, JPX_XAML_RES_COLOR, err, errCap, &fresh);
    if (!r) return 0;
    if (fresh) snprintf(r->color, sizeof(r->color), "%s", t);
    return 1;
}

int JpxHtmlTheme_LoadCss(JpxXamlResourceTable *table, const char *cssPath,
                         char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!table || !cssPath) {
        if (err && errCap) snprintf(err, errCap, "bad argument");
        return 0;
    }
    JpxXamlTheme_Init(table);
    static char buf[262144];
    FILE *f = fopen(cssPath, "rb");
    if (!f) {
        snprintf(err, errCap, "cannot open %s", cssPath);
        return 0;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* 定位 :root 块；块外内容必须是空白（严格）。 */
    char *root = strstr(buf, ":root");
    if (!root) { snprintf(err, errCap, "missing :root block"); return 0; }
    char *p = skip_ws(root + 5);
    if (*p != '{') { snprintf(err, errCap, "expected { after :root"); return 0; }
    p++;
    char *closeBrace = strrchr(p, '}');
    if (!closeBrace) { snprintf(err, errCap, "unterminated :root block"); return 0; }
    *closeBrace = '\0';
    for (char *q = skip_ws(closeBrace + 1); *q; q++) {
        if (!isspace((unsigned char)*q)) {
            snprintf(err, errCap, "content after :root block (strict subset)");
            return 0;
        }
    }

    int decls = 0;
    for (;;) {
        p = skip_ws(p);
        if (!*p) break;
        if (strncmp(p, "--", 2) != 0) {
            snprintf(err, errCap, "expected --Key declaration (got \"%.16s\")", p);
            return 0;
        }
        char *name = p + 2;
        char *colon = strchr(name, ':');
        if (!colon) { snprintf(err, errCap, "--%s missing ':'", name); return 0; }
        *colon = '\0';
        char *key = trim(name);
        char *val = colon + 1;
        char *semi = strchr(val, ';');
        if (!semi) { snprintf(err, errCap, "--%s missing ';'", key); return 0; }
        *semi = '\0';
        if (!parse_value(val, key, table, err, errCap)) return 0;
        decls++;
        if (decls > CSS_MAX_DECLARATIONS) {
            snprintf(err, errCap, "too many declarations");
            return 0;
        }
        p = semi + 1;
    }
    return table->count > 0;
}
