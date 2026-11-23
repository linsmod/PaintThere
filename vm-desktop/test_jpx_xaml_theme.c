/* jp_xaml ResourceDictionary 主题断言：解析、资源查找、渐变、派生主题、序列化。
   不链接 Clay/WM/网络。Run: test_jpx_xaml_theme.exe [--probe]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jpx_xaml_theme.h"
#include "vm_serialize_json.h"

/* --file <path>：解析真实主题文件并打印资源摘要（调试用）。 */
static int probe_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("open failed: %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 1; }
    buf[n] = '\0';
    fclose(f);

    static JpxXamlResourceTable table;
    char err[256];
    int ok = JpxXamlTheme_Parse(&table, buf, err, sizeof(err));
    printf("parse=%d count=%d err=%s\n", ok, table.count, err);
    for (int i = 0; i < table.count && i < 10; i++) {
        const JpxXamlResource *r = &table.items[i];
        printf("  %-32s kind=%d stops=%d color=%s\n",
               r->key, r->kind, r->stopCount, r->color);
    }
    const JpxXamlResource *tb = JpxRes_Find(&table, "TaskbarBackground");
    if (tb) printf("TaskbarBackground kind=%d stops=%d first=%s dir=%s\n",
                   tb->kind, tb->stopCount,
                   tb->stopCount ? tb->stops[0].color : "-", tb->direction);
    free(buf);
    return ok ? 0 : 1;
}

static int g_failures, g_checks, g_probe;

static void section(const char *name) { if (!g_probe) printf("-- %s\n", name); }
static void expect(int cond, const char *what) {
    g_checks++;
    if (cond) { if (!g_probe) printf("PASS  %s\n", what); }
    else { g_failures++; printf("FAIL  %s\n", what); }
}
static void expect_int(int a, int b, const char *what) {
    g_checks++;
    if (a == b) { if (!g_probe) printf("PASS  %s (=%d)\n", what, a); }
    else { g_failures++; printf("FAIL  %s (expected %d, got %d)\n", what, b, a); }
}
static void expect_str(const char *a, const char *b, const char *what) {
    g_checks++;
    if (a && b && strcmp(a, b) == 0) { if (!g_probe) printf("PASS  %s\n", what); }
    else { g_failures++; printf("FAIL  %s (expected \"%s\", got \"%s\")\n", what,
                                b ? b : "(null)", a ? a : "(null)"); }
}

static const char *SAMPLE =
    "<ResourceDictionary>"
      "<SolidColorBrush x:Key='AccentColor' Color='#FF7C7AFF' />"
      "<SolidColorBrush x:Key='TaskbarBackground' Color='#FF10121C' />"
      "<SolidColorBrush x:Key='TaskTextColor' Color='#FFE2E7F0' />"
      "<LinearGradientBrush x:Key='TaskButtonBackground' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#FF7C7AFF' Offset='0.0' />"
        "<GradientStop Color='#FF3D3B99' Offset='1.0' />"
      "</LinearGradientBrush>"
      "<RadialGradientBrush x:Key='TrayToggle' GradientOrigin='0.15,0.15' RadiusX='0.75'>"
        "<GradientStop Color='#FF8ED6F9' Offset='0.0' />"
        "<GradientStop Color='#FF0671EB' Offset='0.9' />"
      "</RadialGradientBrush>"
      "<system:Double x:Key='TaskbarHeight'>30</system:Double>"
    "</ResourceDictionary>";

static void test_parse(void) {
    section("parse resource dictionary");
    JpxXamlResourceTable table;
    char err[256];
    int ok = JpxXamlTheme_Parse(&table, SAMPLE, err, sizeof(err));
    expect(ok, "sample theme parses");
    if (!ok) { printf("  err=%s\n", err); return; }
    expect_int(table.count, 6, "six resources parsed");

    const JpxXamlResource *accent = JpxRes_Find(&table, "AccentColor");
    expect(accent != NULL, "accent resource found");
    if (accent) {
        expect_int((int)accent->kind, (int)JPX_XAML_RES_COLOR, "accent is solid color");
        expect_str(accent->color, "rgba(124,122,255,1.000)", "#AARRGGBB converted to rgba");
    }

    const JpxXamlResource *grad = JpxRes_Find(&table, "TaskButtonBackground");
    expect(grad != NULL, "gradient resource found");
    if (grad) {
        expect_int(grad->kind, JPX_XAML_RES_LINEAR_GRADIENT, "gradient kind is linear");
        expect_int(grad->stopCount, 2, "gradient has two stops");
        expect_str(grad->stops[0].color, "rgba(124,122,255,1.000)", "first stop color");
        expect(grad->stops[1].offset == 1.0f, "second stop offset is 1.0");
        expect_str(grad->direction, "0.0000,0.0000,0.0000,1.0000", "linear direction parsed");
    }

    const JpxXamlResource *radial = JpxRes_Find(&table, "TrayToggle");
    expect(radial != NULL, "radial resource found");
    if (radial) {
        expect_int(radial->kind, JPX_XAML_RES_RADIAL_GRADIENT, "radial kind");
        expect_int(radial->stopCount, 2, "radial has two stops");
        expect_str(radial->direction, "0.1500,0.1500,0.7500", "radial origin/radius parsed");
    }

    expect(JpxRes_Number(&table, "TaskbarHeight", 0) == 30, "Double resource parsed");
    expect(JpxRes_Number(&table, "Missing", 42) == 42, "missing number uses fallback");
    expect_str(JpxRes_Color(&table, "Missing", "#000"), "#000", "missing color uses fallback");
}

static void test_resolve(void) {
    section("resource expressions");
    JpxXamlResourceTable table;
    char err[256];
    expect(JpxXamlTheme_Parse(&table, SAMPLE, err, sizeof(err)), "theme parsed for resolve");
    expect_str(JpxXamlTheme_Resolve(&table, "{StaticResource AccentColor}", "none"),
               "rgba(124,122,255,1.000)", "StaticResource expands");
    expect_str(JpxXamlTheme_Resolve(&table, "{DynamicResource AccentColor}", "none"),
               "rgba(124,122,255,1.000)", "DynamicResource expands (v1 equivalent)");
    expect_str(JpxXamlTheme_Resolve(&table, "{StaticResource Missing}", "fallback"),
               "fallback", "missing key uses fallback");
    expect_str(JpxXamlTheme_Resolve(&table, "rgba(1,2,3,1)", "fallback"),
               "fallback", "plain color is not a resource expression");
    expect_str(JpxRes_Color(&table, "TaskButtonBackground", "none"),
               "rgba(124,122,255,1.000)",
               "gradient resource yields first stop as representative color");
}

static void test_derive(void) {
    section("derive VmTheme");
    JpxXamlResourceTable table;
    char err[256];
    expect(JpxXamlTheme_Parse(&table, SAMPLE, err, sizeof(err)), "theme parsed for derive");
    VmTheme theme;
    JpxXamlTheme_DeriveTheme(&table, &theme);
    expect_str(theme.accentColor, "rgba(124,122,255,1.000)",
               "accent derived from TaskButtonBackground");
    expect_str(theme.panelColor, "rgba(16,18,28,1.000)",
               "panel derived from TaskbarBackground");
    /* XP 皮肤把标题栏交给 style id：paint 命中 styleCache 才会画渐变。 */
    expect_str(theme.chrome.titleFocusColor, "s_title_active", "title focus uses style id");
    expect_str(theme.chrome.titleIdleColor, "s_title_idle", "title idle uses style id");
    expect_str(theme.bgColor, "rgba(255,255,255,1)", "window body is Luna white");
    expect(theme.chrome.borderWidth == 3, "Luna 3px border");
    expect(theme.chrome.showShadow == 0, "Luna has no window shadow");
    expect(theme.titleActiveFrom != NULL && theme.taskButtonFrom != NULL &&
           theme.startFrom != NULL && theme.trayFrom != NULL,
           "gradient skin tokens present");
    expect(theme.chrome.titleHeight == 30, "chrome title height preserved");
}

static void test_patterns(void) {
    section("append patterns");
    JpxXamlResourceTable table;
    char err[256];
    expect(JpxXamlTheme_Parse(&table, SAMPLE, err, sizeof(err)), "theme parsed for patterns");
    static VmDisplayList list;
    VmDisplay_Init(&list);
    int n = JpxXamlTheme_AppendPatterns(&table, &list);
    expect_int(n, 5, "five brush resources appended (3 solid + 1 linear + 1 radial)");
    expect(list.resourceCount >= 5, "resource count increased");
    VmResourceCommand *c = VmDisplay_AppendResource(&list, VM_CMD_DEFINE_PATTERN, "probe");
    expect(c != NULL, "resource capacity >= 6");
}

/* 资源包只下发 id：SSE 里不能出现 definePattern 之类的资源本体。 */
static void test_respack_command(void) {
    section("respack command");
    static VmDisplayList list;
    VmDisplay_Init(&list);
    VmResourceCommand *c = VmDisplay_AppendResource(&list, VM_CMD_LOAD_RES_PACK, "respack");
    expect(c != NULL, "loadResPack appended as resource command");
    if (!c) return;
    VmDisplay_CopyStr(c->args.loadResPack.resPackId,
                      sizeof(c->args.loadResPack.resPackId), "theme-7");
    char *json = VmSerialize_CommandArrayJson(&list);
    expect(json != NULL, "respack command serialized");
    if (json) {
        expect(strstr(json, "\"cmd\":\"loadResPack\"") != NULL, "cmd is loadResPack");
        expect(strstr(json, "\"theme-7\"") != NULL, "respack id carried in args");
        expect(strstr(json, "definePattern") == NULL, "no resource body in the event");
        free(json);
    }
}

/* 图片资源：解析保留原始 uri，ResolveImages 以主题目录为基准归一化。 */
static void test_images(void) {
    section("image resources");
    static JpxXamlResourceTable table;
    char err[256];
    expect(JpxXamlTheme_Parse(&table,
        "<ResourceDictionary>"
        "<BitmapImage x:Key='Ico' UriSource='../Resources/a.png' />"
        "</ResourceDictionary>", err, sizeof(err)), "bitmap image parsed");
    const JpxXamlResource *r = JpxRes_Find(&table, "Ico");
    expect(r != NULL && r->kind == JPX_XAML_RES_IMAGE, "image kind recorded");
    expect(r && strcmp(r->uri, "../Resources/a.png") == 0, "uri kept raw before resolve");
    JpxXamlTheme_ResolveImages(&table, "H:/themes/xp");
    r = JpxRes_Find(&table, "Ico");
    expect(r && strcmp(r->uri, "H:/themes/Resources/a.png") == 0,
           "relative uri resolved against theme dir");
}

static void test_errors(void) {
    section("error reporting");
    JpxXamlResourceTable table;
    char err[256];
    /* x:Key 缺失时解析器跳过该节点，count=0，Parse 返回 0。 */
    expect(!JpxXamlTheme_Parse(&table,
        "<ResourceDictionary><SolidColorBrush Color='#FFFFFF' /></ResourceDictionary>",
        err, sizeof(err)), "resource without x:Key yields no resources");
}

int main(int argc, char **argv) {
    if (argc > 2 && strcmp(argv[1], "--file") == 0) return probe_file(argv[2]);
    g_probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
    test_parse();
    test_resolve();
    test_derive();
    test_patterns();
    test_images();
    test_respack_command();
    test_errors();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
