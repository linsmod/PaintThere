/* jp_xaml ResourceDictionary 主题断言：解析、合并、资源查找、渐变、派生主题、序列化。
   不链接 Clay/WM/网络。Run: test_jpx_xaml_theme.exe [--probe]
   调试探针：--file <theme.xaml> 单文件；--dir <themesDir> 扫目录全部 .xaml。
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <direct.h>

#include "jpx_xaml_theme.h"
#include "jpx_html_theme.h"
#include "vm_serialize_json.h"

/* --file <path>：加载真实主题文件（含合并）并打印资源摘要（调试用）。 */
static int probe_file(const char *path) {
    static JpxXamlResourceTable table;
    char err[256];
    int ok = JpxXamlTheme_LoadFile(&table, path, err, sizeof(err));
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
    return ok ? 0 : 1;
}

/* --compile-css <theme.css> <outDir>：CSS 子集 → 资源表 → 预编译写出
   （css 导出目录需自包含图片；解析与写出由 exporter 的 --check 调用）。 */
static int compile_css(const char *cssPath, const char *outDir) {
    static JpxXamlResourceTable table;
    char err[256];
    if (!JpxHtmlTheme_LoadCss(&table, cssPath, err, sizeof(err))) {
        printf("compile-css: parse failed: %s\n", err);
        return 1;
    }
    char base[512];
    snprintf(base, sizeof(base), "%s", cssPath);
    char *slash = strrchr(base, '/');
    char *bslash = strrchr(base, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) *slash = '\0';
    JpxXamlTheme_ResolveImages(&table, base);
    if (!JpxXamlTheme_WritePrecompiled(&table, outDir, err, sizeof(err))) {
        printf("compile-css: write failed: %s\n", err);
        return 1;
    }
    printf("compiled %s -> %s (%d resources)\n", cssPath, outDir, table.count);
    return 0;
}

/* --precompile <theme.xaml> <outDir>：合并 → 图片绝对化 → 预编译写出（编排脚本调用）。 */
static int precompile(const char *themePath, const char *outDir) {
    static JpxXamlResourceTable table;
    char err[256];
    if (!JpxXamlTheme_LoadFile(&table, themePath, err, sizeof(err))) {
        printf("precompile: load failed: %s\n", err);
        return 1;
    }
    /* 图片相对路径以主题文件所在目录为基准。 */
    char base[512];
    snprintf(base, sizeof(base), "%s", themePath);
    char *slash = strrchr(base, '/');
    char *bslash = strrchr(base, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) *slash = '\0';
    JpxXamlTheme_ResolveImages(&table, base);
    if (!JpxXamlTheme_WritePrecompiled(&table, outDir, err, sizeof(err))) {
        printf("precompile: write failed: %s\n", err);
        return 1;
    }
    printf("precompiled %s -> %s (%d resources)\n", themePath, outDir, table.count);
    return 0;
}

/* --dir <themesDir>：加载目录下全部 .xaml，报资源数与失败（适配验收用）。 */
static int probe_dir(const char *dirPath) {
    static const char *keys[] = {
        "TaskbarBackground", "TaskButtonBackground", "TrayBackground",
        "TaskbarHeight", "ButtonFace", "StartButtonImage", "StartLabel",
        "ButtonForeground", "ClockForeground"
    };
    static JpxXamlResourceTable table;
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s/*.xaml", dirPath);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .xaml in %s\n", dirPath); return 1; }
    int files = 0, fails = 0;
    do {
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dirPath, fd.cFileName);
        char err[256];
        int ok = JpxXamlTheme_LoadFile(&table, full, err, sizeof(err));
        files++;
        if (!ok) fails++;
        printf("%-40s ok=%d res=%3d %s\n", fd.cFileName, ok, ok ? table.count : 0,
               ok ? "" : err);
        if (ok) {
            printf("    ");
            for (int k = 0; k < (int)(sizeof(keys) / sizeof(keys[0])); k++) {
                const JpxXamlResource *r = JpxRes_Find(&table, keys[k]);
                printf("%s=%s ", keys[k],
                       r ? (r->kind == JPX_XAML_RES_IMAGE
                                ? (r->uri[0] ? "img" : "dyn") : "yes") : "-");
            }
            printf("\n");
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d files, %d failures\n", files, fails);
    return fails ? 1 : 0;
}

/* 测试数据目录（独立于其他测试；temp/ 不受 Git 管理，重复运行覆盖写）。 */
static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL  cannot write %s\n", path); exit(1); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
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

/* 厚度/模板几何采集：RetroBar 的任务栏间距全在 Margin/Padding/Thickness
   里，任务栏观感由这些数字决定。 */
static void test_geometry_collection(void) {
    section("thickness and template geometry");
    JpxXamlResourceTable table;
    char err[256];
    const char *xaml =
        "<ResourceDictionary>"
        /* Thickness 资源：属性形式。 */
        "<Thickness x:Key='TaskButtonMargin' Right='3' />"
        /* Thickness 资源：文本形式，2 值 = (左右,上下)。 */
        "<Thickness x:Key='Pad2'>8,3</Thickness>"
        /* Setter：字面量四值。 */
        "<Style x:Key='StartButton'>"
        "<Setter Property='Margin' Value='0,0,1,-3' />"
        "<Setter Property='MinWidth' Value='99' />"
        "</Style>"
        /* Setter：{DynamicResource} 引用 Thickness 资源。 */
        "<Style x:Key='TaskButton'>"
        "<Setter Property='Margin' Value='{DynamicResource TaskButtonMargin}' />"
        /* 模板内命名元素的几何与圆角。 */
        "<Setter Property='Template'>"
        "<Setter.Value>"
        "<ControlTemplate>"
        "<Border Padding='0,3,0,2' x:Name='ButtonBorder'>"
        "<Border BorderThickness='0,1,0,0' CornerRadius='2' "
        "        x:Name='ButtonTopBorder' />"
        "</Border>"
        "</ControlTemplate>"
        "</Setter.Value>"
        "</Setter>"
        "</Style>"
        /* 阴影效果颜色。 */
        "<DropShadowEffect x:Key='StartLabelShadow' Color='#FF00609C' />"
        "</ResourceDictionary>";
    expect(JpxXamlTheme_Parse(&table, xaml, err, sizeof(err)), "geometry xaml parses");
    if (!table.count) { printf("  err=%s\n", err); return; }
    expect(JpxRes_Number(&table, "TaskButtonMargin.Right", -1) == 3,
           "thickness attr form collected");
    expect(JpxRes_Number(&table, "TaskButtonMargin.Left", 0) == 0,
           "missing thickness side defaults to 0");
    expect(JpxRes_Number(&table, "Pad2.Left", -1) == 8 &&
           JpxRes_Number(&table, "Pad2.Top", -1) == 3,
           "thickness two-value text form collected");
    expect(JpxRes_Number(&table, "StartButton.Margin.Right", -1) == 1 &&
           JpxRes_Number(&table, "StartButton.Margin.Bottom", -1) == -3,
           "style margin setter collected (negative value kept)");
    expect(JpxRes_Number(&table, "StartButton.MinWidth", -1) == 99,
           "metric setter still collected alongside thickness");
    expect(JpxRes_Number(&table, "TaskButton.Margin.Right", -1) == 3,
           "DynamicResource thickness reference resolved");
    expect(JpxRes_Number(&table, "TaskButton.ButtonBorder.Padding.Top", -1) == 3,
           "template named-element padding collected");
    expect(JpxRes_Number(&table,
               "TaskButton.ButtonTopBorder.BorderThickness.Top", -1) == 1,
           "template border thickness collected (two-value form)");
    expect(JpxRes_Number(&table, "TaskButton.ButtonTopBorder.CornerRadius", -1) == 2,
           "template corner radius collected");
    expect_str(JpxRes_Color(&table, "StartLabelShadow.Brush", NULL),
               "rgba(0,96,156,1.000)", "drop shadow color collected");

    /* 组合 key 全长（base + ".Bottom"）超 VM_DISPLAY_MAX_ID 时整组跳过：
       add_res 会把超长 key 截断出带尾点的短 key，与别的资源碰撞，
       CSS 往返（导出全名 → 编译回截断）也对不上。 */
    expect(JpxXamlTheme_Parse(&table,
        "<ResourceDictionary>"
        "<SolidColorBrush x:Key='Filler' Color='#FF112233' />"
        "<Style x:Key='ScrollButtonKeyPaddingThatIsDeliberatelyLong000000'>"
        "<Setter Property='Margin' Value='1,2,3,4' />"
        "</Style>"
        "</ResourceDictionary>", err, sizeof(err)), "over-long style parses");
    expect(JpxRes_Number(&table,
        "ScrollButtonKeyPaddingThatIsDeliberatelyLong000000.Margin.Bottom",
        -1) == -1,
        "over-length composite keys skipped whole");
}

/* MergedDictionaries 递归合并：WPF 语义 = 本地 > 后合并 > 先合并。 */
static void test_merge(void) {
    section("merged dictionaries");
    const char *dir = "temp/test_theme_merge";
    _mkdir("temp");
    _mkdir(dir);
    char path[256];
    char err[256];
    static JpxXamlResourceTable table;

    /* 链式：top -> mid -> base，本地覆盖基座。 */
    write_file("temp/test_theme_merge/base.xaml",
        "<ResourceDictionary>"
        "<SolidColorBrush x:Key='BaseColor' Color='#FF101010' />"
        "<SolidColorBrush x:Key='Shared' Color='#FF0000FF' />"
        "</ResourceDictionary>");
    write_file("temp/test_theme_merge/mid.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='base.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "<SolidColorBrush x:Key='Shared' Color='#FF00FF00' />"
        "<SolidColorBrush x:Key='MidColor' Color='#FF202020' />"
        "</ResourceDictionary>");
    write_file("temp/test_theme_merge/top.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='mid.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "<SolidColorBrush x:Key='TopColor' Color='#FF303030' />"
        "</ResourceDictionary>");
    snprintf(path, sizeof(path), "%s/top.xaml", dir);
    expect(JpxXamlTheme_LoadFile(&table, path, err, sizeof(err)), "chain merge loads");
    if (!table.count) { printf("  err=%s\n", err); return; }
    expect_str(JpxRes_Color(&table, "Shared", NULL), "rgba(0,255,0,1.000)",
               "local entry overrides merged base");
    expect_str(JpxRes_Color(&table, "BaseColor", NULL), "rgba(16,16,16,1.000)",
               "base key reachable through chain");
    expect_str(JpxRes_Color(&table, "MidColor", NULL), "rgba(32,32,32,1.000)",
               "mid key reachable");
    expect_str(JpxRes_Color(&table, "TopColor", NULL), "rgba(48,48,48,1.000)",
               "top key reachable");

    /* 兄弟字典：后合并者优先（WPF 逆序查找）。 */
    write_file("temp/test_theme_merge/first.xaml",
        "<ResourceDictionary><SolidColorBrush x:Key='Sib' Color='#FF111111' /></ResourceDictionary>");
    write_file("temp/test_theme_merge/second.xaml",
        "<ResourceDictionary><SolidColorBrush x:Key='Sib' Color='#FF222222' /></ResourceDictionary>");
    write_file("temp/test_theme_merge/two.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='first.xaml' />"
        "<ResourceDictionary Source='second.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "</ResourceDictionary>");
    snprintf(path, sizeof(path), "%s/two.xaml", dir);
    expect(JpxXamlTheme_LoadFile(&table, path, err, sizeof(err)), "sibling merge loads");
    expect_str(JpxRes_Color(&table, "Sib", NULL), "rgba(34,34,34,1.000)",
               "later merged dictionary wins");

    /* 纯转发主题（Vista Classic 形态）：本地 0 资源也解析成功。 */
    write_file("temp/test_theme_merge/forward.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='base.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "</ResourceDictionary>");
    snprintf(path, sizeof(path), "%s/forward.xaml", dir);
    expect(JpxXamlTheme_LoadFile(&table, path, err, sizeof(err)), "forward-only theme loads");
    expect_str(JpxRes_Color(&table, "BaseColor", NULL), "rgba(16,16,16,1.000)",
               "forward-only theme inherits base resources");

    /* 防环：自引用在深度上限处报错。 */
    write_file("temp/test_theme_merge/self.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='self.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "</ResourceDictionary>");
    snprintf(path, sizeof(path), "%s/self.xaml", dir);
    expect(!JpxXamlTheme_LoadFile(&table, path, err, sizeof(err)),
           "self reference rejected");
    expect(strstr(err, "depth") != NULL, "cycle reported via depth limit");

    /* 缺失 source 报错。 */
    write_file("temp/test_theme_merge/missing.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='nope.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "</ResourceDictionary>");
    snprintf(path, sizeof(path), "%s/missing.xaml", dir);
    expect(!JpxXamlTheme_LoadFile(&table, path, err, sizeof(err)),
           "missing merged source rejected");
}

/* 预编译往返：写 xalm.bin/strings.bin/resid2path.bin → 读回，断言等价。
   覆盖：合并链、Style 尺寸、STRING、PATH（归一化+bbox）、图片复制与 resid2path。 */
static void test_precompiled(void) {
    section("precompiled roundtrip");
    const char *dir = "temp/test_theme_pre";
    _mkdir("temp");
    _mkdir(dir);
    char err[256];
    static JpxXamlResourceTable table;

    /* 数据目录：一张假图片（内容不重要，只走复制链路）。 */
    write_file("temp/test_theme_pre_src.png", "PNGDATA");

    /* 基座 + 子主题（本地覆盖 + 各类资源）。 */
    write_file("temp/test_theme_merge/pre_base.xaml",
        "<ResourceDictionary>"
        "<LinearGradientBrush x:Key='TaskbarBackground' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#FF3888E9' Offset='0.0' />"
        "<GradientStop Color='#FF1941A5' Offset='1.0' />"
        "</LinearGradientBrush>"
        "</ResourceDictionary>");
    write_file("temp/test_theme_merge/pre_top.xaml",
        "<ResourceDictionary>"
        "<ResourceDictionary.MergedDictionaries>"
        "<ResourceDictionary Source='pre_base.xaml' />"
        "</ResourceDictionary.MergedDictionaries>"
        "<system:Double x:Key='TaskbarHeight'>30</system:Double>"
        "<Style x:Key='StartButton' TargetType='Button'>"
        "<Setter Property='Height' Value='32' />"
        "</Style>"
        "<FontFamily x:Key='GlobalFontFamily'>Tahoma</FontFamily>"
        "<DrawingBrush x:Key='TaskButtonThumbnail'>"
        "<DrawingBrush.Drawing>"
        "<GeometryDrawing Geometry='M1,1 L1,3 L3,3 L3,1 Z' Brush='#FF808080' />"
        "</DrawingBrush.Drawing>"
        "</DrawingBrush>"
        "<BitmapImage x:Key='StartIconImage' UriSource='../test_theme_pre_src.png' />"
        "</ResourceDictionary>");
    expect(JpxXamlTheme_LoadFile(&table, "temp/test_theme_merge/pre_top.xaml",
                                 err, sizeof(err)), "merged theme loads for precompile");
    JpxXamlTheme_ResolveImages(&table, "temp/test_theme_merge");
    expect(JpxXamlTheme_WritePrecompiled(&table, dir, err, sizeof(err)),
           "write precompiled");
    if (!table.count) { printf("  err=%s\n", err); return; }

    static JpxXamlResourceTable back;
    expect(JpxXamlTheme_LoadPrecompiled(&back, dir, err, sizeof(err)),
           "load precompiled");
    if (!back.count) { printf("  err=%s\n", err); return; }

    /* 数值与合并链。 */
    expect(JpxRes_Number(&back, "TaskbarHeight", 0) == 30, "number survives");
    expect(JpxRes_Number(&back, "StartButton.Height", 0) == 32,
           "style metric survives as id");
    /* 渐变：stops 与方向。 */
    const JpxXamlResource *tb = JpxRes_Find(&back, "TaskbarBackground");
    expect(tb && tb->kind == JPX_XAML_RES_LINEAR_GRADIENT, "gradient kind survives");
    expect(tb && tb->stopCount == 2, "gradient stop count survives");
    expect(tb && strcmp(tb->stops[1].color, "rgba(25,65,165,1.000)") == 0,
           "gradient stop color survives");
    /* 字符串资源。 */
    const JpxXamlResource *ff = JpxRes_Find(&back, "GlobalFontFamily");
    expect(ff && ff->kind == JPX_XAML_RES_STRING, "string kind survives");
    expect(ff && strcmp(ff->value, "Tahoma") == 0, "string value survives");
    /* PATH：归一化 0..1 + bbox 记录原始包围盒；<Key>.Brush 派生色。 */
    const JpxXamlResource *pth = JpxRes_Find(&back, "TaskButtonThumbnail");
    expect(pth && pth->kind == JPX_XAML_RES_PATH, "path kind survives");
    expect(pth && strcmp(pth->value, "M0,0 L0,1 L1,1 L1,0 Z") == 0,
           "path normalized to 0..1");
    expect(pth && pth->bbox[0] == 1.0f && pth->bbox[2] == 3.0f &&
           pth->bbox[3] == 3.0f, "path bbox recorded");
    const JpxXamlResource *pb = JpxRes_Find(&back, "TaskButtonThumbnail.Brush");
    expect(pb && pb->kind == JPX_XAML_RES_COLOR &&
           strcmp(pb->color, "rgba(128,128,128,1.000)") == 0,
           "drawing brush color survives as <Key>.Brush");
    /* 图片：resid2path → IMAGE 资源，uri 是主题目录内相对路径。 */
    const JpxXamlResource *img = JpxRes_Find(&back, "StartIconImage");
    expect(img && img->kind == JPX_XAML_RES_IMAGE, "image kind survives via resid2path");
    expect(img && strcmp(img->uri, "StartIconImage.png") == 0,
           "image renamed to <id>.png");
    /* 复制的图片文件存在。 */
    FILE *f = fopen("temp/test_theme_pre/StartIconImage.png", "rb");
    expect(f != NULL, "image file copied into theme dir");
    if (f) fclose(f);

    /* 绝对化后查询路径与写前一致（同一 ResolveImages 语义）。 */
    JpxXamlTheme_ResolveImages(&back, dir);
    img = JpxRes_Find(&back, "StartIconImage");
    expect(img && strstr(img->uri, "test_theme_pre") != NULL,
           "image path resolves against precompiled dir");
}

/* PATH 归一化边界：相对命令等不支持的几何跳过，不影响整个主题解析。 */
static void test_path_limits(void) {
    section("path limits");
    JpxXamlResourceTable table;
    char err[256];
    expect(JpxXamlTheme_Parse(&table,
        "<ResourceDictionary>"
        "<SolidColorBrush x:Key='Anchor' Color='#FF000000' />"
        "<DrawingBrush x:Key='RelPath'><DrawingBrush.Drawing>"
        "<GeometryDrawing Geometry='M1,1 l2,2 Z' />"
        "</DrawingBrush.Drawing></DrawingBrush>"
        "</ResourceDictionary>", err, sizeof(err)),
        "relative-command theme still parses");
    expect(JpxRes_Find(&table, "RelPath") == NULL,
           "relative-command geometry skipped");
}
static void test_classic_derive(void) {
    section("classic theme degrade");
    static const char *CLASSIC =
        "<ResourceDictionary>"
        "<SolidColorBrush x:Key='ButtonFace' Color='#FFC0C0C0' />"
        "<SolidColorBrush x:Key='ButtonHighlight' Color='#FFFFFFFF' />"
        "<SolidColorBrush x:Key='ButtonShadow' Color='#FF808080' />"
        "<SolidColorBrush x:Key='ButtonForeground' Color='#FF000000' />"
        "<SolidColorBrush x:Key='ClockForeground' Color='#FF000000' />"
        "</ResourceDictionary>";
    JpxXamlResourceTable table;
    char err[256];
    expect(JpxXamlTheme_Parse(&table, CLASSIC, err, sizeof(err)),
           "classic key set parses");
    VmTheme theme;
    JpxXamlTheme_DeriveTheme(&table, &theme);
    expect_str(theme.panelColor, "rgba(192,192,192,1.000)",
               "panel falls back to ButtonFace");
    expect_str(theme.taskButtonFrom, "rgba(192,192,192,1.000)",
               "task buttons fall back to ButtonFace flat");
    expect_str(theme.trayFrom, "rgba(192,192,192,1.000)",
               "tray matches taskbar gray");
    expect_str(theme.startFrom, "rgba(192,192,192,1.000)",
               "classic start button is gray");
    expect_str(theme.accentTextColor, "rgba(0,0,0,1.000)",
               "classic text is black");
}

/* CSS 子集编译：类型推断、顺序保持、未知值报错。 */
static void test_html_theme(void) {
    section("css subset compile");
    const char *dir = "temp/test_theme_css";
    _mkdir("temp");
    _mkdir(dir);
    write_file("temp/test_theme_css/sample.css",
        ":root {\n"
        "  --TaskbarHeight: 30;\n"
        "  --TaskbarBackground: linear-gradient(dir(0.0000,0.0000,0.0000,1.0000),\n"
        "        rgba(56,136,233,1.000) 0, rgba(25,65,165,1.000) 1);\n"
        "  --GlobalFontFamily: 'Tahoma';\n"
        "  --TaskButtonThumbnail: path(1,1,3,3, 'M0,0 L0,1 L1,1 L1,0Z');\n"
        "  --StartIconImage: url(StartIconImage.png);\n"
        "}\n");
    static JpxXamlResourceTable table;
    char err[256];
    expect(JpxHtmlTheme_LoadCss(&table, "temp/test_theme_css/sample.css",
                                err, sizeof(err)), "css parses");
    if (!table.count) { printf("  err=%s\n", err); return; }
    expect_int(table.count, 5, "five declarations in order");
    expect_int((int)table.items[0].kind, (int)JPX_XAML_RES_NUMBER, "number kind");
    expect(table.items[0].number == 30.0, "number value");
    expect_int((int)table.items[1].kind, (int)JPX_XAML_RES_LINEAR_GRADIENT,
               "linear kind");
    expect_str(table.items[1].direction, "0.0000,0.0000,0.0000,1.0000",
               "direction verbatim");
    expect_int(table.items[1].stopCount, 2, "stop count");
    expect_str(table.items[1].stops[1].color, "rgba(25,65,165,1.000)",
               "stop color verbatim");
    expect_int((int)table.items[2].kind, (int)JPX_XAML_RES_STRING, "string kind");
    expect_str(table.items[2].value, "Tahoma", "string value");
    expect_int((int)table.items[3].kind, (int)JPX_XAML_RES_PATH, "path kind");
    expect_str(table.items[3].value, "M0,0 L0,1 L1,1 L1,0Z", "path data");
    expect(table.items[3].bbox[2] == 3.0f && table.items[3].bbox[3] == 3.0f,
           "path bbox");
    expect_int((int)table.items[4].kind, (int)JPX_XAML_RES_IMAGE, "image kind");
    expect_str(table.items[4].uri, "StartIconImage.png", "image relative path");

    /* 严格性：空值报错；裸 token（命名颜色）合法。 */
    write_file("temp/test_theme_css/bad.css", ":root { --X: ; }\n");
    expect(!JpxHtmlTheme_LoadCss(&table, "temp/test_theme_css/bad.css",
                                 err, sizeof(err)),
           "empty value rejected");
    write_file("temp/test_theme_css/named.css", ":root { --C: Transparent; }\n");
    expect(JpxHtmlTheme_LoadCss(&table, "temp/test_theme_css/named.css",
                                err, sizeof(err)),
           "named color accepted");
    expect_str(table.count ? table.items[0].color : "", "Transparent",
               "named color verbatim");
}

int main(int argc, char **argv) {
    if (argc > 3 && strcmp(argv[1], "--precompile") == 0)
        return precompile(argv[2], argv[3]);
    if (argc > 3 && strcmp(argv[1], "--compile-css") == 0)
        return compile_css(argv[2], argv[3]);
    if (argc > 2 && strcmp(argv[1], "--file") == 0) return probe_file(argv[2]);
    if (argc > 2 && strcmp(argv[1], "--dir") == 0) return probe_dir(argv[2]);
    g_probe = argc > 1 && strcmp(argv[1], "--probe") == 0;
    test_parse();
    test_resolve();
    test_derive();
    test_geometry_collection();
    test_patterns();
    test_images();
    test_merge();
    test_precompiled();
    test_path_limits();
    test_html_theme();
    test_classic_derive();
    test_respack_command();
    test_errors();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
