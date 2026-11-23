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
        } else if (strcmp(node->tag, "BitmapImage") == 0 ||
                   strcmp(node->tag, "ImageBrush") == 0) {
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_IMAGE, err, errCap);
            if (!r) return 0;
            /* ImageBrush 用 ImageSource，BitmapImage 用 UriSource；
               {DynamicResource ...} 形式没有直接路径，跳过（留空 uri）。 */
            const char *src = JpxXamlNode_Attr(node, "UriSource");
            if (!src) src = JpxXamlNode_Attr(node, "ImageSource");
            if (src && src[0] != '{') snprintf(r->uri, sizeof(r->uri), "%s", src);
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

/* RetroBar 的主题只描述任务栏（155 个 key 全是 Taskbar/Tray/TaskButton/StartButton/Clock），
   没有窗口边框、标题栏与控件资源。窗口侧按 Luna 惯例给固定值，任务栏侧才查表。
   用静态字面量：VmTheme 只持有指针，调用方（资源表）生命期之外的值必须常驻。 */
/* 逐段归一化：丢弃空段与 "."，".." 回退一段。不依赖 OS 的相对路径解析，
   因为主题文件里的路径是 Windows 风格，而解析结果要同时能 fopen。 */
static void normalize_path(const char *src, char *out, size_t cap) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", src ? src : "");
    int outLen = 0;
    out[0] = '\0';
    const char *p = tmp;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/' && *p != '\\') p++;
        int segLen = (int)(p - seg);
        if (segLen == 1 && seg[0] == '.') continue;
        if (segLen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (outLen > 0 && out[outLen - 1] != '/') outLen--;
            if (outLen > 0) outLen--;
            out[outLen] = '\0';
            continue;
        }
        if (outLen + segLen + 2 >= (int)cap) break;
        if (outLen) out[outLen++] = '/';
        memcpy(out + outLen, seg, (size_t)segLen);
        outLen += segLen;
        out[outLen] = '\0';
    }
}

void JpxXamlTheme_ResolveImages(JpxXamlResourceTable *table, const char *themeFilePath) {
    if (!table) return;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", themeFilePath ? themeFilePath : "");
    char *slash = strrchr(dir, '/');
    char *bslash = strrchr(dir, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) *slash = '\0';

    for (int i = 0; i < table->count; i++) {
        JpxXamlResource *r = &table->items[i];
        if (r->kind != JPX_XAML_RES_IMAGE || !r->uri[0]) continue;
        /* 已经是绝对路径（盘符或根）时不拼接。 */
        if (r->uri[0] == '/' || r->uri[0] == '\\' ||
            (r->uri[1] == ':' && r->uri[2] == '/')) continue;
        char joined[512];
        snprintf(joined, sizeof(joined), "%s/%s", dir, r->uri);
        normalize_path(joined, r->uri, sizeof(r->uri));
    }
}

/* 图片字节缓冲：单进程一次只服务一个主题包，串行读取即可复用。 */
static unsigned char s_imgBuf[262144];

static const char *mime_of(const char *uri) {
    size_t n = uri ? strlen(uri) : 0;
    if (n > 4 && (uri[n - 4] == '.')) {
        const char *ext = uri + n - 3;
        if (strcmp(ext, "jpg") == 0) return "image/jpeg";
        if (strcmp(ext, "bmp") == 0) return "image/bmp";
        if (strcmp(ext, "gif") == 0) return "image/gif";
    }
    return "image/png";
}

int JpxXamlTheme_ImageAt(void *userData, const char *resPackId, int index,
                         const char **imgId, const char **mime,
                         const char **data, size_t *size) {
    (void)resPackId;
    const JpxXamlResourceTable *table = (const JpxXamlResourceTable *)userData;
    if (!table || index < 0 || !imgId || !data || !size) return 0;
    int seen = 0;
    for (int i = 0; i < table->count; i++) {
        const JpxXamlResource *r = &table->items[i];
        if (r->kind != JPX_XAML_RES_IMAGE || !r->uri[0]) continue;
        if (seen++ != index) continue;
        FILE *f = fopen(r->uri, "rb");
        if (!f) return 0;
        size_t n = fread(s_imgBuf, 1, sizeof(s_imgBuf), f);
        fclose(f);
        if (n == 0) return 0;
        *imgId = r->key;
        if (mime) *mime = mime_of(r->uri);
        *data = (const char *)s_imgBuf;
        *size = n;
        return 1;
    }
    return 0;
}

/* 渐变资源 → 首末 stop 颜色：VmTheme 的成对 token 只接受两个颜色。 */
static void gradient_ends(const JpxXamlResourceTable *table, const char *key,
                          const char **from, const char **to) {
    const JpxXamlResource *r = JpxRes_Find(table, key);
    if (!r || r->stopCount < 2) return;
    if (r->kind != JPX_XAML_RES_LINEAR_GRADIENT &&
        r->kind != JPX_XAML_RES_RADIAL_GRADIENT) return;
    *from = r->stops[0].color;
    *to = r->stops[r->stopCount - 1].color;
}

void JpxXamlTheme_DeriveTheme(const JpxXamlResourceTable *table, VmTheme *out) {
    if (!out) return;
    *out = *VmTheme_Default();

    /* 窗口与控件：Luna 白底蓝框；字号按 9pt 中文界面（≈12px）。 */
    out->bgColor = "rgba(255,255,255,1)";
    out->fgColor = "rgba(0,0,0,1)";
    out->borderColor = "rgba(0,84,227,1)";
    out->inputBgColor = "rgba(255,255,255,1)";
    out->disabledColor = "rgba(128,128,128,1)";
    out->selectedColor = "rgba(49,106,197,0.35)";
    out->radius = 3;
    out->padding = 6;
    out->gap = 6;
    out->fontSize = 12;
    /* 桌面：RetroBar 不含壁纸，用 XP 默认的蓝天渐变近似。 */
    out->desktopFrom = "rgba(30,111,217,1)";
    out->desktopTo = "rgba(111,168,232,1)";

    /* 标题栏：Luna 竖直渐变（上方亮蓝、下方深蓝），失焦转灰蓝。 */
    out->titleActiveFrom = "rgba(0,88,238,1)";
    out->titleActiveTo = "rgba(61,149,240,1)";
    out->titleIdleFrom = "rgba(123,162,231,1)";
    out->titleIdleTo = "rgba(107,143,214,1)";

    /* 开始按钮：Luna 绿，文字白色加粗观感由字号与颜色近似。 */
    out->startFrom = "rgba(63,154,63,1)";
    out->startTo = "rgba(45,122,45,1)";
    out->startTextColor = "rgba(255,255,255,1)";

    /* 任务按钮：未激活浅蓝、激活深蓝（按下态）。 */
    out->taskButtonFrom = "rgba(59,142,232,1)";
    out->taskButtonTo = "rgba(28,95,192,1)";
    out->taskButtonActiveFrom = "rgba(28,95,192,1)";
    out->taskButtonActiveTo = "rgba(15,74,158,1)";
    out->taskButtonTextColor = "rgba(255,255,255,1)";

    /* 托盘区与时钟。 */
    out->trayFrom = "rgba(29,126,232,1)";
    out->trayTo = "rgba(11,100,207,1)";
    out->clockTextColor = "rgba(255,255,255,1)";

    /* WM 装饰：paint 填 style id 才会命中前端的 styleCache 走渐变；
       边框必须是纯色（drawBorder 不查 style）。 */
    out->chrome.titleHeight = 30;
    out->chrome.borderWidth = 3;
    out->chrome.titleFontSize = 12;
    out->chrome.titleFocusColor = "s_title_active";
    out->chrome.titleIdleColor = "s_title_idle";
    out->chrome.titleTextColor = "rgba(255,255,255,1)";
    out->chrome.borderFocusColor = out->borderColor;
    out->chrome.borderIdleColor = out->borderColor;
    out->chrome.showShadow = 0;

    if (!table) return;
    /* 任务栏：用主题里真实存在的 key 覆盖（此前查的 6 个 key 在 XP 里并不存在）。 */
    out->panelColor = JpxRes_Color(table, "TaskbarBackground", out->panelColor);
    out->accentColor = JpxRes_Color(table, "TaskButtonBackground", out->accentColor);
    out->accentTextColor = JpxRes_Color(table, "ButtonForeground", out->accentTextColor);
    out->taskButtonTextColor =
        JpxRes_Color(table, "ButtonForeground", out->taskButtonTextColor);
    out->clockTextColor = JpxRes_Color(table, "ClockForeground", out->clockTextColor);
    out->startTextColor = JpxRes_Color(table, "StartLabel", out->startTextColor);
    out->panelHeight = (int)JpxRes_Number(table, "TaskbarHeight", out->panelHeight);
    /* 任务栏按钮/托盘的渐变端点直接用主题里的多 stop 资源（首末 stop），
       比代码内手写的 Luna 近似更贴近主题文件。 */
    gradient_ends(table, "TaskButtonBackground", &out->taskButtonFrom, &out->taskButtonTo);
    gradient_ends(table, "TaskButtonBackgroundPressed",
                  &out->taskButtonActiveFrom, &out->taskButtonActiveTo);
    gradient_ends(table, "TrayBackground", &out->trayFrom, &out->trayTo);
    /* 位图皮肤：主题里有开始按钮图就用它，比手写绿色渐变更接近原版。 */
    if (JpxRes_Find(table, "StartButtonImage")) out->startImageId = "StartButtonImage";
    if (JpxRes_Find(table, "StartIconImage")) out->startIconId = "StartIconImage";
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
