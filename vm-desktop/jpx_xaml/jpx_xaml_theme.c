#include "jpx_xaml_theme.h"
#include "jpx_xaml_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>     /* _mkdir */
#else
#include <sys/stat.h>   /* mkdir */
#endif

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

/* 返回资源槽位；isNew 置 1 表示新创建、0 表示 key 已存在（合并语义下
   先到先得，调用方必须跳过已存在条目的字段填充）。 */
static JpxXamlResource *add_res(JpxXamlResourceTable *table, const char *key,
                                JpxXamlResKind kind, char *err, size_t errCap,
                                int *isNew) {
    if (isNew) *isNew = 0;
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
    if (isNew) *isNew = 1;
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

/* 只收单值尺寸属性：Margin/Padding 是多值，需要时再单独解析。 */
static int metric_prop(const char *prop) {
    return strcmp(prop, "Height") == 0 || strcmp(prop, "Width") == 0 ||
           strcmp(prop, "MinWidth") == 0 || strcmp(prop, "MaxWidth") == 0 ||
           strcmp(prop, "FontSize") == 0;
}

/* 前置声明：合并递归在定义之前使用（定义在图片路径一节）。 */
static void normalize_path(const char *src, char *out, size_t cap);

/* ---------------- PATH 资源：几何归一化 ---------------- */

/* SVG path data（绝对坐标）走查：pass 1 收集 bbox，pass 2 输出归一化 0..1 坐标。
   支持 M/L/H/V/C/S/Q/T/Z；相对命令（小写）与 A（弧）不支持，返回 0（调用方跳过资源）。
   H/V 需要当前点，walk 内部跟踪 cx/cy。 */
static int geom_walk(const char *raw, int emit, char *out, size_t cap, float bbox[4]) {
    const char *p = raw;
    char cmd = 0;
    float cx = 0, cy = 0;
    size_t o = 0;
    if (emit && cap) out[0] = '\0';
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (isalpha((unsigned char)*p)) {
            char c0 = (char)*p++;
            if (c0 != (char)toupper((unsigned char)c0)) return 0;   /* 相对命令 */
            if (c0 == 'A') return 0;                                /* 弧 */
            cmd = c0;
            if (emit && o + 3 < cap) {
                if (o && out[o - 1] != ' ') out[o++] = ' ';
                out[o++] = cmd;
                out[o] = '\0';
            }
            continue;
        }
        int need;
        switch (cmd) {
            case 'M': case 'L': case 'T': need = 2; break;
            case 'C': need = 6; break;
            case 'S': case 'Q': need = 4; break;
            case 'H': case 'V': need = 1; break;
            default: return 0;  /* Z 后不该有数字 / 数字出现在命令前 */
        }
        double v[6];
        int nv = 0;
        while (nv < need) {
            char *end;
            double d = strtod(p, &end);
            if (end == p) return 0;
            v[nv++] = d;
            p = end;
            while (*p == ' ' || *p == ',') p++;
            if (nv < need && !(*p && (isdigit((unsigned char)*p) || *p == '.' ||
                                      *p == '-' || *p == '+')))
                return 0;
        }
        /* SVG 允许同一命令后接多组坐标；这里不支持，避免错误重组。 */
        if (*p && (isdigit((unsigned char)*p) || *p == '.' || *p == '-' || *p == '+'))
            return 0;
        double px[3], py[3];
        int np = 0;
        if (cmd == 'H') { cx = (float)v[0]; px[0] = v[0]; py[0] = cy; np = 1; }
        else if (cmd == 'V') { cy = (float)v[0]; px[0] = cx; py[0] = v[0]; np = 1; }
        else {
            for (int k = 0; k + 1 < need; k += 2) { px[np] = v[k]; py[np] = v[k + 1]; np++; }
            cx = (float)v[need - 2];
            cy = (float)v[need - 1];
        }
        for (int k = 0; k < np; k++) {
            if (px[k] < bbox[0]) bbox[0] = (float)px[k];
            if (py[k] < bbox[1]) bbox[1] = (float)py[k];
            if (px[k] > bbox[2]) bbox[2] = (float)px[k];
            if (py[k] > bbox[3]) bbox[3] = (float)py[k];
        }
        if (emit) {
            float w = bbox[2] - bbox[0], h = bbox[3] - bbox[1];
            for (int k = 0; k < np; k++) {
                float nx = w > 0 ? (float)((px[k] - bbox[0]) / w) : 0.0f;
                float ny = h > 0 ? (float)((py[k] - bbox[1]) / h) : 0.0f;
                char piece[40];
                if (cmd == 'H') snprintf(piece, sizeof(piece), "%.4g", nx);
                else if (cmd == 'V') snprintf(piece, sizeof(piece), "%.4g", ny);
                else snprintf(piece, sizeof(piece), "%.4g,%.4g", nx, ny);
                if (o + strlen(piece) + 2 >= cap) return 0;
                if (o && out[o - 1] != cmd) out[o++] = ' ';
                memcpy(out + o, piece, strlen(piece));
                o += strlen(piece);
                out[o] = '\0';
            }
        }
    }
    return cmd != 0;
}

/* 在子孙节点里找带指定属性的节点，返回节点下标（找不到返回 -1）。 */
static int find_attr_desc(const JpxXamlDoc *doc, int idx, const char *attr) {
    for (int c = doc->nodes[idx].firstChild; c >= 0; c = doc->nodes[c].nextSibling) {
        if (JpxXamlNode_Attr(&doc->nodes[c], attr)) return c;
        int deeper = find_attr_desc(doc, c, attr);
        if (deeper >= 0) return deeper;
    }
    return -1;
}

static int build_resources(const JpxXamlDoc *doc, JpxXamlResourceTable *table,
                           char *err, size_t errCap) {
    for (int i = 0; i < doc->count; i++) {
        const JpxXamlNode *node = &doc->nodes[i];
        const char *key = JpxXamlNode_Attr(node, "x:Key");
        if (!key) continue;
        if (strcmp(node->tag, "SolidColorBrush") == 0) {
            int fresh = 0;
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_COLOR, err, errCap, &fresh);
            if (!r) return 0;
            if (fresh) color_to_css(JpxXamlNode_Attr(node, "Color"), r->color, sizeof(r->color));
        } else if (strcmp(node->tag, "LinearGradientBrush") == 0) {
            int fresh = 0;
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_LINEAR_GRADIENT, err, errCap, &fresh);
            if (!r) return 0;
            if (!fresh) continue;
            collect_stops(doc, i, r);
            parse_direction(node, r, 0);
        } else if (strcmp(node->tag, "RadialGradientBrush") == 0) {
            int fresh = 0;
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_RADIAL_GRADIENT, err, errCap, &fresh);
            if (!r) return 0;
            if (!fresh) continue;
            collect_stops(doc, i, r);
            parse_direction(node, r, 1);
        } else if (strcmp(node->tag, "system:Double") == 0) {
            int fresh = 0;
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_NUMBER, err, errCap, &fresh);
            if (!r) return 0;
            if (fresh) r->number = atof(JpxXamlNode_Text(node));
        } else if (strcmp(node->tag, "BitmapImage") == 0 ||
                   strcmp(node->tag, "ImageBrush") == 0) {
            int fresh = 0;
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_IMAGE, err, errCap, &fresh);
            if (!r) return 0;
            if (!fresh) continue;
            /* ImageBrush 用 ImageSource，BitmapImage 用 UriSource；
               {DynamicResource ...} 形式没有直接路径，跳过（留空 uri）。 */
            const char *src = JpxXamlNode_Attr(node, "UriSource");
            if (!src) src = JpxXamlNode_Attr(node, "ImageSource");
            if (src && src[0] != '{') snprintf(r->uri, sizeof(r->uri), "%s", src);
        } else if (strcmp(node->tag, "FontFamily") == 0 ||
                   strcmp(node->tag, "system:String") == 0) {
            /* 字符串资源（如 GlobalFontFamily）：值进 strings.bin，运行时 id 查表。 */
            int fresh = 0;
            JpxXamlResource *r = add_res(table, key, JPX_XAML_RES_STRING, err, errCap, &fresh);
            if (!r) return 0;
            if (fresh) snprintf(r->value, sizeof(r->value), "%s", JpxXamlNode_Text(node));
        } else if (strcmp(node->tag, "DrawingBrush") == 0) {
            /* PATH：取子孙节点的 Geometry 属性，归一化到 0..1（bbox 记录原始包围盒）；
               不支持的几何（相对命令/弧/多组坐标）跳过该资源。填充色收为 <Key>.Brush。 */
            int gidx = find_attr_desc(doc, i, "Geometry");
            if (gidx >= 0) {
                const char *geo = JpxXamlNode_Attr(&doc->nodes[gidx], "Geometry");
                float bb[4] = {1e30f, 1e30f, -1e30f, -1e30f};
                char norm[192];
                if (geom_walk(geo, 0, NULL, 0, bb) &&
                    bb[0] <= bb[2] && bb[1] <= bb[3] &&
                    geom_walk(geo, 1, norm, sizeof(norm), bb)) {
                    int fresh = 0;
                    JpxXamlResource *r =
                        add_res(table, key, JPX_XAML_RES_PATH, err, errCap, &fresh);
                    if (!r) return 0;
                    if (fresh) {
                        snprintf(r->value, sizeof(r->value), "%s", norm);
                        memcpy(r->bbox, bb, sizeof(bb));
                    }
                }
                const char *brush = JpxXamlNode_Attr(&doc->nodes[gidx], "Brush");
                if (brush && brush[0]) {
                    char col[VM_DISPLAY_MAX_COLOR];
                    if (brush[0] == '#') {
                        color_to_css(brush, col, sizeof(col));
                    } else {
                        /* {StaticResource}/{DynamicResource} 引用：查当前已收集的表。 */
                        const char *resolved = JpxXamlTheme_Resolve(table, brush, NULL);
                        snprintf(col, sizeof(col), "%s", resolved ? resolved : "");
                    }
                    if (col[0]) {
                        char mk[96];
                        snprintf(mk, sizeof(mk), "%s.Brush", key);
                        int fresh2 = 0;
                        JpxXamlResource *rb =
                            add_res(table, mk, JPX_XAML_RES_COLOR, err, errCap, &fresh2);
                        if (!rb) return 0;
                        if (fresh2) snprintf(rb->color, sizeof(rb->color), "%s", col);
                    }
                }
            }
        } else if (strcmp(node->tag, "Style") == 0) {
            /* Style/模板不执行，但尺寸要由主题决定：把直接子 Setter 里
               的单值尺寸收成 "<StyleKey>.<Property>" 数值资源。 */
            for (int c = node->firstChild; c >= 0; c = doc->nodes[c].nextSibling) {
                const JpxXamlNode *setter = &doc->nodes[c];
                if (strcmp(setter->tag, "Setter") != 0) continue;
                const char *prop = JpxXamlNode_Attr(setter, "Property");
                const char *val = JpxXamlNode_Attr(setter, "Value");
                if (!prop || !val || val[0] == '{') continue;
                if (!metric_prop(prop)) continue;
                char mk[96];
                snprintf(mk, sizeof(mk), "%s.%s", key, prop);
                int fresh = 0;
                JpxXamlResource *r = add_res(table, mk, JPX_XAML_RES_NUMBER, err, errCap, &fresh);
                if (!r) return 0;
                if (fresh) r->number = atof(val);
            }
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

/* ---------------- MergedDictionaries 递归合并 ---------------- */

/* WPF 语义：本地条目 > 后合并的字典 > 先合并的字典（合并字典按加入逆序查找）。
   实现：本文件资源先 add-if-missing 入表（先到先得，优先级最高），
   再从最后一个 source 往前递归合并（后合并者先占位）。 */
#define JPX_XAML_MAX_MERGE_DEPTH 8
#define JPX_XAML_MAX_MERGE_SRCS 8

/* 取路径的目录部分（含 / 或 \ 都识别）；无分隔符时返回空串（按 CWD 相对）。 */
static void dirname_of(const char *path, char *out, size_t cap) {
    const char *slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (!slash) { if (cap) out[0] = '\0'; return; }
    size_t n = (size_t)(slash - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* 递归加载一个主题文件并合并进 table。返回 0 表示读文件/解析失败（err 已写）。 */
static int load_into(JpxXamlResourceTable *table, const char *path, int depth,
                     char *err, size_t errCap) {
    if (depth >= JPX_XAML_MAX_MERGE_DEPTH) {
        snprintf(err, errCap, "theme merge depth exceeded at %s (cycle?)", path);
        return 0;
    }
    /* RetroBar 最大主题约 143KB；文本与 doc 都只在递归下探前使用，
       串行解析可以 static 复用，父层递归返回后不再读 buf/doc。 */
    static char buf[262144];
    static JpxXamlDoc doc;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errCap, "cannot open theme file: %s", path);
        return 0;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    JpxXamlDoc_Init(&doc);
    if (!JpxXamlDoc_Parse(&doc, buf, err, errCap)) {
        char lineErr[192];
        snprintf(lineErr, sizeof(lineErr), "%s", err);
        snprintf(err, errCap, "%s: %s", path, lineErr);
        return 0;
    }
    /* 本文件自有资源（含 Style 尺寸）优先入表。 */
    if (!build_resources(&doc, table, err, errCap)) return 0;
    /* 收集 MergedDictionaries 里的 Source。 */
    char sources[JPX_XAML_MAX_MERGE_SRCS][300];
    int nsrc = 0;
    for (int i = 0; i < doc.count && nsrc < JPX_XAML_MAX_MERGE_SRCS; i++) {
        const JpxXamlNode *node = &doc.nodes[i];
        if (strcmp(node->tag, "ResourceDictionary") != 0) continue;
        const char *src = JpxXamlNode_Attr(node, "Source");
        if (!src || !*src) continue;
        snprintf(sources[nsrc++], sizeof(sources[0]), "%s", src);
    }
    char dir[512];
    dirname_of(path, dir, sizeof(dir));
    /* 逆序递归：后合并的字典先入表。 */
    for (int i = nsrc - 1; i >= 0; i--) {
        char joined[512];
        if (dir[0]) snprintf(joined, sizeof(joined), "%s/%s", dir, sources[i]);
        else snprintf(joined, sizeof(joined), "%s", sources[i]);
        char full[512];
        normalize_path(joined, full, sizeof(full));
        if (!load_into(table, full, depth + 1, err, errCap)) return 0;
    }
    return 1;
}

int JpxXamlTheme_LoadFile(JpxXamlResourceTable *table, const char *path,
                          char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!table || !path) {
        if (err && errCap) snprintf(err, errCap, "bad argument");
        return 0;
    }
    JpxXamlTheme_Init(table);
    if (!load_into(table, path, 0, err, errCap)) {
        JpxXamlTheme_Init(table);
        return 0;
    }
    return table->count > 0;
}

/* ---------------- 预编译：xalm.bin / strings.bin / resid2path.bin ---------------- */
/* 小端显式读写，不依赖结构体内存布局。字符串全部进 strings.bin（NUL 结尾池，
   u32 offset 引用），xalm.bin 的 record 只持有偏移。 */

#define JPX_XAML_POOL_CAP 131072
#define JPX_XAML_POOL_REF_CAP 2048
#define JPX_XAML_XALM_REC 100   /* 1+1+2 + 32 + 32 + 4 + 4 + 16 + 8 */

static char s_pool[JPX_XAML_POOL_CAP];
static unsigned s_poolLen;

/* 写侧池：先收集引用，再 qsort 去重定偏移。strings.bin 与表序无关
   （css 编译回的资源顺序可能与 xaml 不同，字节一致要求池规范化）。 */
static const char *s_refs[JPX_XAML_POOL_REF_CAP];
static unsigned s_refOff[JPX_XAML_POOL_REF_CAP];
static int s_refCount;

static void pool_reset(void) { s_refCount = 0; s_poolLen = 0; }

static void pool_ref(const char *s) {
    if (!s) s = "";
    for (int i = 0; i < s_refCount; i++) {
        if (strcmp(s_refs[i], s) == 0) return;
    }
    if (s_refCount >= JPX_XAML_POOL_REF_CAP) return;
    s_refs[s_refCount++] = s;
}

static int cmp_ref(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int pool_build(char *err, size_t errCap) {
    qsort(s_refs, (size_t)s_refCount, sizeof(char *), cmp_ref);
    int n = 0;
    for (int i = 0; i < s_refCount; i++) {
        if (n && strcmp(s_refs[n - 1], s_refs[i]) == 0) continue;
        s_refs[n++] = s_refs[i];
    }
    s_refCount = n;
    for (int i = 0; i < n; i++) {
        size_t len = strlen(s_refs[i]) + 1;
        if (s_poolLen + len > sizeof(s_pool)) {
            snprintf(err, errCap, "string pool overflow");
            return 0;
        }
        s_refOff[i] = s_poolLen;
        memcpy(s_pool + s_poolLen, s_refs[i], len);
        s_poolLen += (unsigned)len;
    }
    return 1;
}

static unsigned pool_off(const char *s) {
    if (!s) s = "";
    for (int i = 0; i < s_refCount; i++) {
        if (strcmp(s_refs[i], s) == 0) return s_refOff[i];
    }
    return 0;
}

static void put_u32le(FILE *f, unsigned v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static unsigned get_u32le(const unsigned char *b) {
    return (unsigned)b[0] | ((unsigned)b[1] << 8) | ((unsigned)b[2] << 16) |
           ((unsigned)b[3] << 24);
}

static void put_f32le(FILE *f, float x) {
    unsigned v;
    memcpy(&v, &x, 4);
    put_u32le(f, v);
}

static float get_f32le(const unsigned char *b) {
    unsigned v = get_u32le(b);
    float x;
    memcpy(&x, &v, 4);
    return x;
}

/* f64 按本机字节序（x86/ARM64 均小端；预编译产物不做跨端交换）。 */
static double get_f64le(const unsigned char *b) {
    double x;
    memcpy(&x, b, 8);
    return x;
}

static void mkdir_p(const char *dir) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0777);
#endif
            *p = c;
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0777);
#endif
}

/* 图片落盘名：<资源id>.<原扩展名>，id 命名天然免碰撞。 */
static void image_name(const JpxXamlResource *r, char *out, size_t cap) {
    const char *dot = strrchr(r->uri, '.');
    char ext[8] = "png";
    if (dot && dot[1] && strlen(dot) <= 6) {
        int ok = 1;
        for (const char *p = dot + 1; *p; p++) {
            if (!isalnum((unsigned char)*p)) { ok = 0; break; }
        }
        if (ok) snprintf(ext, sizeof(ext), "%s", dot + 1);
    }
    snprintf(out, cap, "%s.%s", r->key, ext);
}

static int copy_file(const char *src, const char *dst, char *err, size_t errCap) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        snprintf(err, errCap, "cannot read image: %s", src);
        return 0;
    }
    static unsigned char buf[262144];
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); snprintf(err, errCap, "cannot write image: %s", dst); return 0; }
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 1;
}

int JpxXamlTheme_WritePrecompiled(const JpxXamlResourceTable *table, const char *dir,
                                  char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!table || !dir || !table->count) {
        if (err && errCap) snprintf(err, errCap, "bad argument or empty table");
        return 0;
    }
    mkdir_p(dir);
    pool_reset();
    pool_ref("");   /* offset 0 保留为空串（空串排序最先，规范化后仍为 0） */

    static char r2pName[JPX_XAML_MAX_RESOURCES][160];
    static const JpxXamlResource *r2pRes[JPX_XAML_MAX_RESOURCES];
    int r2pCount = 0;
    int recCount = 0;   /* 非 IMAGE 资源数（进 xalm.bin record 的） */

    /* 第一遍：收集字符串引用 + 复制图片。 */
    for (int i = 0; i < table->count; i++) {
        const JpxXamlResource *r = &table->items[i];
        if (r->kind == JPX_XAML_RES_IMAGE) {
            if (!r->uri[0]) continue;   /* DynamicResource 形式，无路径可打包 */
            image_name(r, r2pName[r2pCount], sizeof(r2pName[0]));
            char dst[512];
            snprintf(dst, sizeof(dst), "%s/%s", dir, r2pName[r2pCount]);
            if (!copy_file(r->uri, dst, err, errCap)) return 0;
            r2pRes[r2pCount] = r;
            pool_ref(r->key);
            pool_ref(r2pName[r2pCount]);
            r2pCount++;
            continue;
        }
        recCount++;
        pool_ref(r->key);
        const char *val = "";
        if (r->kind == JPX_XAML_RES_COLOR) val = r->color;
        else if (r->kind == JPX_XAML_RES_LINEAR_GRADIENT ||
                 r->kind == JPX_XAML_RES_RADIAL_GRADIENT) val = r->direction;
        else if (r->kind == JPX_XAML_RES_STRING || r->kind == JPX_XAML_RES_PATH)
            val = r->value;
        pool_ref(val);
        for (int s = 0; s < r->stopCount && s < VM_PATTERN_MAX_STOPS; s++)
            pool_ref(r->stops[s].color);
    }
    if (!pool_build(err, errCap)) return 0;

    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/strings.bin", dir);
    f = fopen(path, "wb");
    if (!f) { snprintf(err, errCap, "cannot write %s", path); return 0; }
    fwrite(s_pool, 1, s_poolLen, f);
    fclose(f);

    snprintf(path, sizeof(path), "%s/xalm.bin", dir);
    f = fopen(path, "wb");
    if (!f) { snprintf(err, errCap, "cannot write %s", path); return 0; }
    fwrite("XALM", 1, 4, f);
    put_u32le(f, 1);                    /* version */
    put_u32le(f, (unsigned)recCount);
    put_u32le(f, s_poolLen);
    for (int i = 0; i < table->count; i++) {
        const JpxXamlResource *r = &table->items[i];
        if (r->kind == JPX_XAML_RES_IMAGE) continue;
        const char *val = "";
        if (r->kind == JPX_XAML_RES_COLOR) val = r->color;
        else if (r->kind == JPX_XAML_RES_LINEAR_GRADIENT ||
                 r->kind == JPX_XAML_RES_RADIAL_GRADIENT) val = r->direction;
        else if (r->kind == JPX_XAML_RES_STRING || r->kind == JPX_XAML_RES_PATH)
            val = r->value;
        unsigned char rec[JPX_XAML_XALM_REC];
        memset(rec, 0, sizeof(rec));
        rec[0] = (unsigned char)r->kind;
        rec[1] = (unsigned char)r->stopCount;
        for (int s = 0; s < r->stopCount && s < VM_PATTERN_MAX_STOPS; s++) {
            float off = r->stops[s].offset;
            unsigned bits;
            memcpy(&bits, &off, 4);
            rec[4 + s * 4] = (unsigned char)bits;
            rec[5 + s * 4] = (unsigned char)(bits >> 8);
            rec[6 + s * 4] = (unsigned char)(bits >> 16);
            rec[7 + s * 4] = (unsigned char)(bits >> 24);
            unsigned c = pool_off(r->stops[s].color);
            rec[36 + s * 4] = (unsigned char)c;
            rec[37 + s * 4] = (unsigned char)(c >> 8);
            rec[38 + s * 4] = (unsigned char)(c >> 16);
            rec[39 + s * 4] = (unsigned char)(c >> 24);
        }
        unsigned ko = pool_off(r->key), vo = pool_off(val);
        rec[68] = (unsigned char)ko; rec[69] = (unsigned char)(ko >> 8);
        rec[70] = (unsigned char)(ko >> 16); rec[71] = (unsigned char)(ko >> 24);
        rec[72] = (unsigned char)vo; rec[73] = (unsigned char)(vo >> 8);
        rec[74] = (unsigned char)(vo >> 16); rec[75] = (unsigned char)(vo >> 24);
        for (int k = 0; k < 4; k++) {
            float bvi = (r->kind == JPX_XAML_RES_PATH) ? r->bbox[k] : 0.0f;
            unsigned bits;
            memcpy(&bits, &bvi, 4);
            rec[76 + k * 4] = (unsigned char)bits;
            rec[77 + k * 4] = (unsigned char)(bits >> 8);
            rec[78 + k * 4] = (unsigned char)(bits >> 16);
            rec[79 + k * 4] = (unsigned char)(bits >> 24);
        }
        double num = (r->kind == JPX_XAML_RES_NUMBER) ? r->number : 0.0;
        memcpy(rec + 92, &num, 8);
        fwrite(rec, 1, sizeof(rec), f);
    }
    fclose(f);

    snprintf(path, sizeof(path), "%s/resid2path.bin", dir);
    f = fopen(path, "wb");
    if (!f) { snprintf(err, errCap, "cannot write %s", path); return 0; }
    fwrite("R2PP", 1, 4, f);
    put_u32le(f, (unsigned)r2pCount);
    put_u32le(f, s_poolLen);
    for (int i = 0; i < r2pCount; i++) {
        unsigned k = pool_off(r2pRes[i]->key);
        unsigned p = pool_off(r2pName[i]);
        put_u32le(f, k);
        put_u32le(f, p);
    }
    fclose(f);
    return 1;
}

int JpxXamlTheme_LoadPrecompiled(JpxXamlResourceTable *table, const char *dir,
                                 char *err, size_t errCap) {
    if (err && errCap) err[0] = '\0';
    if (!table || !dir) {
        if (err && errCap) snprintf(err, errCap, "bad argument");
        return 0;
    }
    JpxXamlTheme_Init(table);
    static unsigned char buf[262144];
    char path[512];
    FILE *f;

    /* strings.bin → 字符串池（读入即池本体，偏移直接可用）。 */
    snprintf(path, sizeof(path), "%s/strings.bin", dir);
    f = fopen(path, "rb");
    if (!f) { snprintf(err, errCap, "cannot open %s", path); return 0; }
    s_poolLen = (unsigned)fread(s_pool, 1, sizeof(s_pool) - 1, f);
    s_pool[s_poolLen] = '\0';
    fclose(f);
    /* 池里最后一段可能不以 NUL 结尾（写侧保证会，但防御性补上）。 */
    if (s_poolLen == 0 || s_pool[s_poolLen - 1] != '\0') s_pool[s_poolLen++] = '\0';

#define POOL_STR(off) (((off) < s_poolLen) ? s_pool + (off) : "")

    snprintf(path, sizeof(path), "%s/xalm.bin", dir);
    f = fopen(path, "rb");
    if (!f) { snprintf(err, errCap, "cannot open %s", path); return 0; }
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 16 || memcmp(buf, "XALM", 4) != 0) {
        snprintf(err, errCap, "bad xalm.bin in %s", dir);
        return 0;
    }
    unsigned version = get_u32le(buf + 4);
    unsigned resCount = get_u32le(buf + 8);
    unsigned stringsSize = get_u32le(buf + 12);
    if (version != 1 || stringsSize > s_poolLen ||
        16 + (size_t)resCount * JPX_XAML_XALM_REC > n) {
        snprintf(err, errCap, "unsupported xalm.bin (v%u, %u res)", version, resCount);
        return 0;
    }
    for (unsigned i = 0; i < resCount; i++) {
        const unsigned char *rec = buf + 16 + i * JPX_XAML_XALM_REC;
        JpxXamlResKind kind = (JpxXamlResKind)rec[0];
        if (kind <= JPX_XAML_RES_NONE || kind > JPX_XAML_RES_PATH) continue;
        int fresh = 0;
        JpxXamlResource *r = add_res(table, POOL_STR(get_u32le(rec + 68)), kind,
                                     err, errCap, &fresh);
        if (!r) return 0;
        if (!fresh) continue;
        r->stopCount = rec[1] > VM_PATTERN_MAX_STOPS ? VM_PATTERN_MAX_STOPS : rec[1];
        for (int s = 0; s < r->stopCount; s++) {
            r->stops[s].offset = get_f32le(rec + 4 + s * 4);
            snprintf(r->stops[s].color, sizeof(r->stops[s].color), "%s",
                     POOL_STR(get_u32le(rec + 36 + s * 4)));
        }
        const char *val = POOL_STR(get_u32le(rec + 72));
        if (kind == JPX_XAML_RES_COLOR) snprintf(r->color, sizeof(r->color), "%s", val);
        else if (kind == JPX_XAML_RES_LINEAR_GRADIENT ||
                 kind == JPX_XAML_RES_RADIAL_GRADIENT)
            snprintf(r->direction, sizeof(r->direction), "%s", val);
        else if (kind == JPX_XAML_RES_STRING || kind == JPX_XAML_RES_PATH)
            snprintf(r->value, sizeof(r->value), "%s", val);
        else if (kind == JPX_XAML_RES_NUMBER) r->number = get_f64le(rec + 92);
        if (kind == JPX_XAML_RES_PATH) {
            for (int k = 0; k < 4; k++) r->bbox[k] = get_f32le(rec + 76 + k * 4);
        }
    }

    snprintf(path, sizeof(path), "%s/resid2path.bin", dir);
    f = fopen(path, "rb");
    if (!f) { snprintf(err, errCap, "cannot open %s", path); return 0; }
    n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n < 12 || memcmp(buf, "R2PP", 4) != 0) {
        snprintf(err, errCap, "bad resid2path.bin in %s", dir);
        return 0;
    }
    unsigned imgCount = get_u32le(buf + 4);
    unsigned stringsSize2 = get_u32le(buf + 8);
    if (stringsSize2 > s_poolLen || 12 + (size_t)imgCount * 8 > n) {
        snprintf(err, errCap, "bad resid2path.bin layout");
        return 0;
    }
    for (unsigned i = 0; i < imgCount; i++) {
        const unsigned char *e = buf + 12 + i * 8;
        int fresh = 0;
        JpxXamlResource *r = add_res(table, POOL_STR(get_u32le(e)), JPX_XAML_RES_IMAGE,
                                     err, errCap, &fresh);
        if (!r) return 0;
        if (fresh) snprintf(r->uri, sizeof(r->uri), "%s", POOL_STR(get_u32le(e + 4)));
    }
#undef POOL_STR
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

void JpxXamlTheme_ResolveImages(JpxXamlResourceTable *table, const char *baseDir) {
    if (!table || !baseDir) return;
    for (int i = 0; i < table->count; i++) {
        JpxXamlResource *r = &table->items[i];
        if (r->kind != JPX_XAML_RES_IMAGE || !r->uri[0]) continue;
        /* 已经是绝对路径（盘符或根）时不拼接。 */
        if (r->uri[0] == '/' || r->uri[0] == '\\' ||
            (r->uri[1] == ':' && r->uri[2] == '/')) continue;
        char joined[512];
        snprintf(joined, sizeof(joined), "%s/%s", baseDir, r->uri);
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

/* rgba() 字符串亮度调节：factor<1 变暗、>1 变亮（clamp 到 255）。
   调用方需要稳定的返回指针（VmTheme 只存指针），用轮换缓冲池；
   同一表达式里最多用 4 个（activeFrom/To、hoverFrom/To），池给 6 个。 */
static const char *shade_css(const char *css, double factor) {
    static char bufs[6][VM_DISPLAY_MAX_COLOR];
    static int idx = 0;
    char *out = bufs[idx];
    idx = (idx + 1) % 6;
    unsigned r = 0, g = 0, b = 0;
    double a = 1.0;
    if (sscanf(css, "rgba(%u,%u,%u,%lf)", &r, &g, &b, &a) != 4) return css;
    double sr = r * factor, sg = g * factor, sb = b * factor;
    r = (unsigned)(sr > 255 ? 255 : sr);
    g = (unsigned)(sg > 255 ? 255 : sg);
    b = (unsigned)(sb > 255 ? 255 : sb);
    snprintf(out, VM_DISPLAY_MAX_COLOR, "rgba(%u,%u,%u,%.3f)", r, g, b, a);
    return out;
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
    /* 任务栏分级 fallback：主题真实 key（渐变取首末 stop）→ 经典主题灰色系
       （95-98/2000 没有 TaskbarBackground/TrayBackground/TaskbarHeight，
       只有 ButtonFace 一族纯色）→ 保持 Luna 默认。 */
    const char *panel = JpxRes_Color(table, "TaskbarBackground", NULL);
    if (!panel) panel = JpxRes_Color(table, "TrayBackground", NULL);
    if (!panel) panel = JpxRes_Color(table, "ButtonFace", NULL);
    if (panel) out->panelColor = panel;

    /* 文字色：StartLabel 在 95-98 里不存在，退到 ButtonForeground（黑）。 */
    out->accentTextColor = JpxRes_Color(table, "ButtonForeground", out->accentTextColor);
    out->taskButtonTextColor = out->accentTextColor;
    out->clockTextColor = JpxRes_Color(table, "ClockForeground", out->clockTextColor);
    out->startTextColor = JpxRes_Color(table, "StartLabel",
                            JpxRes_Color(table, "ButtonForeground", out->startTextColor));
    out->panelHeight = (int)JpxRes_Number(table, "TaskbarHeight", out->panelHeight);
    /* 任务栏几何：优先取主题 Style 里的尺寸（StartButton.Height/MinWidth 等），
       主题没写的才落回 Luna 的观感值。shell 只消费这些 token。 */
    out->panelPad = 3;
    out->panelStartWidth = (int)JpxRes_Number(table, "StartButton.MinWidth", 99);
    out->panelButtonHeight = (int)JpxRes_Number(table, "StartButton.Height", 32);
    out->panelItemWidth = (int)JpxRes_Number(table, "TaskButton.Width", 110);
    out->panelItemGap = 3;
    out->panelTrayWidth = (int)JpxRes_Number(table, "Tray.Width", 76);
    out->panelRadius = 3;
    out->panelStartIconSize = 22;
    out->boldChromeText = 1;
    out->chrome.boldTitle = 1;
    /* 任务按钮：按下渐变 → 纯色（Watercolor 的 TaskButtonActive）→
       经典 ButtonFace 扁平（激活态压暗区分）。 */
    const char *tbFrom = NULL, *tbTo = NULL;
    gradient_ends(table, "TaskButtonBackground", &tbFrom, &tbTo);
    if (!tbFrom) { tbFrom = JpxRes_Color(table, "TaskButtonBackground", NULL); tbTo = tbFrom; }
    if (!tbFrom) { tbFrom = JpxRes_Color(table, "ButtonFace", NULL); tbTo = tbFrom; }
    if (tbFrom) {
        out->taskButtonFrom = tbFrom;
        out->taskButtonTo = tbTo;
        out->accentColor = tbFrom;
    }
    const char *tbAFrom = NULL, *tbATo = NULL;
    gradient_ends(table, "TaskButtonBackgroundPressed", &tbAFrom, &tbATo);
    if (!tbAFrom) { tbAFrom = JpxRes_Color(table, "TaskButtonActive", NULL); tbATo = tbAFrom; }
    if (!tbAFrom && tbFrom) {
        tbAFrom = shade_css(tbFrom, 0.85);
        tbATo = shade_css(tbTo ? tbTo : tbFrom, 0.85);
    }
    if (tbAFrom) { out->taskButtonActiveFrom = tbAFrom; out->taskButtonActiveTo = tbATo; }
    /* hover：主题 hover 渐变 → 主题色提亮。 */
    const char *tbHFrom = NULL, *tbHTo = NULL;
    gradient_ends(table, "TaskButtonBackgroundHover", &tbHFrom, &tbHTo);
    if (!tbHFrom && tbFrom) {
        tbHFrom = shade_css(tbFrom, 1.15);
        tbHTo = shade_css(tbTo ? tbTo : tbFrom, 1.15);
    }
    if (tbHFrom) { out->taskButtonHoverFrom = tbHFrom; out->taskButtonHoverTo = tbHTo; }
    /* 托盘：主题渐变 → 纯色 → 经典同任务栏灰。 */
    const char *trayFrom = NULL, *trayTo = NULL;
    gradient_ends(table, "TrayBackground", &trayFrom, &trayTo);
    if (!trayFrom) { trayFrom = JpxRes_Color(table, "TrayBackground", NULL); trayTo = trayFrom; }
    if (!trayFrom) { trayFrom = panel; trayTo = panel; }
    if (trayFrom) { out->trayFrom = trayFrom; out->trayTo = trayTo; }
    /* 开始按钮：位图优先；无位图时 StartButtonBackground（Watercolor）→
       任务栏色（Aero 无位图时取蓝渐变端点，经典取灰）→ 保持 Luna 绿。 */
    if (!out->startImageId) {
        const char *sf = NULL, *st = NULL;
        sf = JpxRes_Color(table, "StartButtonBackground", NULL);
        if (sf) st = sf;
        if (!sf) gradient_ends(table, "TaskbarBackground", &sf, &st);
        if (!sf && panel) { sf = panel; st = panel; }
        if (sf) { out->startFrom = sf; out->startTo = st; }
    }
    /* 条目 3D 边框：XP 有专用 Top/BottomBorder；经典用高光/阴影。 */
    out->taskBorderTopColor = JpxRes_Color(table, "TaskButtonTopBorder",
                                JpxRes_Color(table, "ButtonHighlight", out->taskBorderTopColor));
    out->taskBorderBottomColor = JpxRes_Color(table, "TaskButtonBottomBorder",
                                JpxRes_Color(table, "ButtonShadow", out->taskBorderBottomColor));
    /* 位图皮肤：uri 为空的（Aero 的 {DynamicResource} 形式，解析器拿不到路径）
       视为不存在，走上面的色块降级，避免前端 drawImg 空图。 */
    const JpxXamlResource *img = JpxRes_Find(table, "StartButtonImage");
    if (img && img->uri[0]) out->startImageId = "StartButtonImage";
    img = JpxRes_Find(table, "StartIconImage");
    if (img && img->uri[0]) out->startIconId = "StartIconImage";
    img = JpxRes_Find(table, "ShowDesktopIconImageSmall");
    if (img && img->uri[0]) out->trayIconId = "ShowDesktopIconImageSmall";
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
