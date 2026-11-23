#include "jpx_xaml_theme.h"
#include "jpx_xaml_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void JpxXamlTheme_Init(JpxXamlResourceTable *table) {
    if (table) memset(table, 0, sizeof(*table));
}

/* ---------------- 颜色转换 ---------------- */

/* RetroBar 主题用 #AARRGGBB / #RRGGBB；前端用 rgba()。 */
static void color_to_css(const char *src, char *out, size_t cap) {
    if (!src || !*src) { if (cap) out[0] = '\0'; return; }
    if (src[0] != '#' || cap < 24) {
        snprintf(out, cap, "%s", src);
        return;
    }
    const char *hex = src + 1;
    size_t n = strlen(hex);
    unsigned r = 0, g = 0, b = 0, a = 255;
    if (n == 8) {
        if (sscanf(hex, "%2x%2x%2x%2x", &a, &r, &g, &b) != 4) {
            snprintf(out, cap, "%s", src);
            return;
        }
    } else if (n == 6) {
        if (sscanf(hex, "%2x%2x%2x", &r, &g, &b) != 3) {
            snprintf(out, cap, "%s", src);
            return;
        }
    } else {
        snprintf(out, cap, "%s", src);
        return;
    }
    snprintf(out, cap, "rgba(%u,%u,%u,%.3f)", r, g, b, (double)a / 255.0);
}

static double attr_double(const JpxXamlNode *node, const char *name, double def) {
    const char *v = JpxXamlNode_Attr(node, name);
    return v ? atof(v) : def;
}

static JpxXamlResource *add_res(JpxXamlResourceTable *table, const char *key,
                                JpxXamlResKind kind, char *err, size_t errCap) {
    if (!key || !*key) {
        if (err && errCap) snprintf(err, errCap, "resource missing x:Key");
        return NULL;
    }
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].key, key) == 0) return &table->items[i];
    }
    if (table->count >= JPX_XAML_MAX_RESOURCES) {
        if (err && errCap) snprintf(err, errCap, "too many resources");
        return NULL;
    }
    JpxXamlResource *r = &table->items[table->count++];
    memset(r, 0, sizeof(*r));
    r->used = 1;
    r->kind = kind;
    snprintf(r->key, sizeof(r->key), "%s", key);
    return r;
}

/* 收集一个 Brush 节点的 GradientStop 子节点。返回 stop 数。 */
static int collect_stops(const JpxXamlDoc *doc, int parent, JpxXamlResource *r) {
    int n = 0;
    for (int c = doc->nodes[parent].firstChild; c >= 0; c = doc->nodes[c].nextSibling) {
        const JpxXamlNode *node = &doc->nodes[c];
        if (strcmp(node->tag, "GradientStop") != 0) continue;
        if (n >= VM_PATTERN_MAX_STOPS) break;
        color_to_css(JpxXamlNode_Attr(node, "Color"),
                     r->stops[n].color, sizeof(r->stops[n].color));
        r->stops[n].offset = (float)attr_double(node, "Offset", (double)n);
        n++;
    }
    r->stopCount = n;
    return n;
}

static void parse_direction(const JpxXamlNode *node, JpxXamlResource *r, int radial) {
    if (radial) {
        /* GradientOrigin="0.15,0.15" 取第一对坐标作 cx,cy；RadiusX 作 r。 */
        const char *origin = JpxXamlNode_Attr(node, "GradientOrigin");
        double ox = 0.5, oy = 0.5;
        if (origin) {
            double a = 0.5, b = 0.5;
            if (sscanf(origin, "%lf,%lf", &a, &b) == 2) { ox = a; oy = b; }
        }
        double rad = attr_double(node, "RadiusX", 0.5);
        snprintf(r->direction, sizeof(r->direction), "%.4f,%.4f,%.4f", ox, oy, rad);
    } else {
        const char *sp = JpxXamlNode_Attr(node, "StartPoint");
        const char *ep = JpxXamlNode_Attr(node, "EndPoint");
        double x0 = 0, y0 = 0, x1 = 1, y1 = 1;
        if (sp) { double a = 0, b = 0; if (sscanf(sp, "%lf,%lf", &a, &b) == 2) { x0 = a; y0 = b; } }
        if (ep) { double a = 1, b = 1; if (sscanf(ep, "%lf,%lf", &a, &b) == 2) { x1 = a; y1 = b; } }
        snprintf(r->direction, sizeof(r->direction), "%.4f,%.4f,%.4f,%.4f", x0, y0, x1, y1);
    }
}

static int build_resources(const JpxXamlDoc *doc, JpxXamlResourceTable *table,
                           char *err, size_t errCap) {
    for (int i = 0; i < doc->count; i++) {
        const JpxXamlNode *node = &doc->nodes[i];
        const char *key = JpxXamlNode_Attr(node, "x:Key");
        if (!key) continue;
        if (strcmp(node->tag, "SolidColorBrush") == 0) {
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_COLOR, err, errCap);
            if (!r) return 0;
            color_to_css(JpxXamlNode_Attr(node, "Color"), r->color, sizeof(r->color));
        } else if (strcmp(node->tag, "LinearGradientBrush") == 0) {
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_LINEAR_GRADIENT, err, errCap);
            if (!r) return 0;
            collect_stops(doc, i, r);
            parse_direction(node, r, 0);
        } else if (strcmp(node->tag, "RadialGradientBrush") == 0) {
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_RADIAL_GRADIENT, err, errCap);
            if (!r) return 0;
            collect_stops(doc, i, r);
            parse_direction(node, r, 1);
        } else if (strcmp(node->tag, "system:Double") == 0) {
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_NUMBER, err, errCap);
            if (!r) return 0;
            r->number = atof(JpxXamlNode_Text(node));
        }
    }
    return 1;
}

int JpxXamlTheme_Parse(JpxXamlResourceTable *table, const char *xaml,
                       char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!table || !xaml) return 0;
    /* 1024 节点约 600KB，栈上会溢出；主题解析串行执行，用 static 复用。 */
    static JpxXamlDoc doc;
    if (!JpxXamlDoc_Parse(&doc, xaml, err, errCap)) return 0;
    JpxXamlTheme_Init(table);
    if (!build_resources(&doc, table, err, errCap)) {
        JpxXamlTheme_Init(table);
        return 0;
    }
    return table->count > 0;
}

const JpxXamlResource *JpxRes_Find(const JpxXamlResourceTable *table, const char *key) {
    if (!table || !key) return NULL;
    for (int i = 0; i < table->count; i++) {
        if (strcmp(table->items[i].key, key) == 0) return &table->items[i];
    }
    return NULL;
}

const char *JpxRes_Color(const JpxXamlResourceTable *table, const char *key,
                         const char *fallback) {
    const JpxXamlResource *r = JpxRes_Find(table, key);
    if (!r) return fallback;
    if (r->kind == JPX_XAML_RES_COLOR) return r->color;
    /* 渐变也提供一个代表色（首 stop），供只能消费单色的 VmTheme token 使用。
       RetroBar 的 TaskbarBackground 等 key 多为 LinearGradientBrush。 */
    if ((r->kind == JPX_XAML_RES_LINEAR_GRADIENT ||
         r->kind == JPX_XAML_RES_RADIAL_GRADIENT) && r->stopCount > 0)
        return r->stops[0].color;
    return fallback;
}

double JpxRes_Number(const JpxXamlResourceTable *table, const char *key, double fallback) {
    const JpxXamlResource *r = JpxRes_Find(table, key);
    return (r && r->kind == JPX_XAML_RES_NUMBER) ? r->number : fallback;
}

int JpxXamlTheme_AppendPatterns(const JpxXamlResourceTable *table, VmDisplayList *list) {
    if (!table || !list) return 0;
    int added = 0;
    for (int i = 0; i < table->count; i++) {
        const JpxXamlResource *r = &table->items[i];
        VmPatternType type;
        if (r->kind == JPX_XAML_RES_COLOR) type = VM_PATTERN_SOLID;
        else if (r->kind == JPX_XAML_RES_LINEAR_GRADIENT) type = VM_PATTERN_GRADIENT;
        else if (r->kind == JPX_XAML_RES_RADIAL_GRADIENT) type = VM_PATTERN_RADIAL;
        else continue;
        VmResourceCommand *c = VmDisplay_AppendResource(list, VM_CMD_DEFINE_PATTERN, r->key);
        if (!c) break;
        VmDefinePatternArgs *p = &c->args.definePattern;
        VmDisplay_CopyStr(p->patternId, sizeof(p->patternId), r->key);
        p->type = type;
        VmDisplay_CopyStr(p->color, sizeof(p->color), r->color);
        p->stopCount = r->stopCount;
        for (int s = 0; s < r->stopCount && s < VM_PATTERN_MAX_STOPS; s++) p->stops[s] = r->stops[s];
        VmDisplay_CopyStr(p->direction, sizeof(p->direction), r->direction);
        added++;
    }
    return added;
}

void JpxXamlTheme_DeriveTheme(const JpxXamlResourceTable *table, VmTheme *out) {
    if (!out) return;
    *out = *VmTheme_Default();
    if (!table) return;
    const char *accent = JpxRes_Color(table, "AccentColor", out->accentColor);
    const char *panel = JpxRes_Color(table, "TaskbarBackground", out->panelColor);
    const char *fg = JpxRes_Color(table, "TaskTextColor", out->fgColor);
    const char *bg = JpxRes_Color(table, "WindowBackground", out->bgColor);
    const char *title = JpxRes_Color(table, "TaskButtonBackground", accent);
    const char *idle = JpxRes_Color(table, "TaskbarBackground", out->chrome.titleIdleColor);
    out->accentColor = accent;
    out->accentTextColor = JpxRes_Color(table, "accentTextColor", out->accentTextColor);
    out->panelColor = panel;
    out->fgColor = fg;
    out->bgColor = bg;
    out->desktopFrom = JpxRes_Color(table, "DesktopFrom", out->desktopFrom);
    out->desktopTo = JpxRes_Color(table, "DesktopTo", out->desktopTo);
    out->chrome.titleFocusColor = title;
    out->chrome.titleIdleColor = idle;
    out->chrome.borderFocusColor = accent;
}

const char *JpxXamlTheme_Resolve(const JpxXamlResourceTable *table,
                                 const char *expr, const char *fallback) {
    if (!expr) return fallback;
    static const char *prefixes[] = { "{StaticResource ", "{DynamicResource " };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t n = strlen(prefixes[i]);
        if (strncmp(expr, prefixes[i], n) != 0) continue;
        const char *p = expr + n;
        const char *end = strchr(p, '}');
        if (!end) return fallback;
        size_t len = (size_t)(end - p);
        static char key[VM_DISPLAY_MAX_ID];
        if (len == 0 || len >= sizeof(key)) return fallback;
        memcpy(key, p, len);
        key[len] = '\0';
        const JpxXamlResource *r = JpxRes_Find(table, key);
        if (r && r->kind == JPX_XAML_RES_COLOR) return r->color;
        return fallback;
    }
    return fallback;
}
