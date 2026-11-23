/* jp_xaml 表单示例：界面结构写在 XAML 里，应用代码只有 state + handler。
   构建：python tools/build_vm.py → demo_xaml.exe
   运行：demo_xaml.exe 9060 ../assets
   验证：应用源码不出现 clay / vm_display / vm_tiles / json 词汇。 */

#include "jpx.h"
#include "jpx_xaml.h"
#include "jpx_xaml_theme.h"
#include "jpx_html_theme.h"
#include "crash_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JpxApp *g_app;

/* 内置 XP Luna 资源字典：不传 --theme 时的默认外观。
   数值取自 RetroBar 的 Windows XP Blue.xaml（任务栏 5-stop、托盘、任务按钮、高度 30）。
   不含位图（StartButtonImage 是外部资源），开始按钮走绿色渐变。
   自包含：默认外观不依赖 RetroBar 仓库是否在机器上。 */
static const char *XP_THEME =
    "<ResourceDictionary>"
      "<system:Double x:Key='TaskbarHeight'>30</system:Double>"
      "<Style x:Key='StartButton' TargetType='Button'>"
        "<Setter Property='MinWidth' Value='99' />"
        "<Setter Property='Height' Value='32' />"
      "</Style>"
      "<LinearGradientBrush x:Key='TaskbarBackground' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#3888e9' Offset='0.0' />"
        "<GradientStop Color='#4993e6' Offset='0.05' />"
        "<GradientStop Color='#2257d5' Offset='0.18' />"
        "<GradientStop Color='#2663e0' Offset='0.9' />"
        "<GradientStop Color='#1941a5' Offset='1.0' />"
      "</LinearGradientBrush>"
      "<LinearGradientBrush x:Key='TrayBackground' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#16adf0' Offset='0.0' />"
        "<GradientStop Color='#19b9f3' Offset='0.05' />"
        "<GradientStop Color='#1290e8' Offset='0.18' />"
        "<GradientStop Color='#0d8dea' Offset='0.6' />"
        "<GradientStop Color='#0fa0ef' Offset='1.0' />"
      "</LinearGradientBrush>"
      "<LinearGradientBrush x:Key='TaskButtonBackground' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#4892f7' Offset='0.0' />"
        "<GradientStop Color='#3980f4' Offset='0.17' />"
      "</LinearGradientBrush>"
      "<LinearGradientBrush x:Key='TaskButtonBackgroundPressed' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#123d94' Offset='0.0' />"
        "<GradientStop Color='#2156b7' Offset='1.0' />"
      "</LinearGradientBrush>"
      "<LinearGradientBrush x:Key='TaskButtonBackgroundHover' StartPoint='0,0' EndPoint='0,1'>"
        "<GradientStop Color='#8bc0ff' Offset='0.0' />"
        "<GradientStop Color='#59a4ff' Offset='0.05' />"
        "<GradientStop Color='#569fff' Offset='0.9' />"
        "<GradientStop Color='#2a81ff' Offset='1.0' />"
      "</LinearGradientBrush>"
      "<SolidColorBrush x:Key='TaskButtonTopBorder' Color='#3172da' />"
      "<SolidColorBrush x:Key='TaskButtonBottomBorder' Color='#2652bc' />"
      "<SolidColorBrush x:Key='TaskButtonLeftBorder' Color='#3067dd' />"
      "<SolidColorBrush x:Key='TaskButtonRightBorder' Color='#264fad' />"
      "<SolidColorBrush x:Key='TaskbarBorder' Color='#3168d5' />"
      "<SolidColorBrush x:Key='ButtonForeground' Color='#ffffff' />"
      "<SolidColorBrush x:Key='ClockForeground' Color='#ffffff' />"
      "<SolidColorBrush x:Key='ItemButtonForeground' Color='#ffffff' />"
      "<BitmapImage x:Key='StartButtonImage' UriSource='xp/startxpbluebutton.png' />"
      "<BitmapImage x:Key='StartIconImage' UriSource='xp/startxpblue.png' />"
      "<BitmapImage x:Key='ShowDesktopIconImageSmall' UriSource='xp/desktopxp-sm.png' />"
    "</ResourceDictionary>";

static JpxXamlResourceTable g_theme;

static const char *FORM_XAML =
    "<Window Title='jp_xaml 表单' Width='420' Height='430'>"
      "<StackPanel Orientation='Vertical'>"
        "<TextBlock Text='XAML 描述界面，C 侧只写状态与 handler' Wrap='False' />"
        "<Spacer Height='6' />"
        "<TextBlock Text='姓名' />"
        "<TextBox Name='name' Text='{Binding name}' TextChanged='OnNameChanged' />"
        "<TextBlock Text='邮箱' />"
        "<TextBox Name='email' Text='{Binding email}' />"
        "<TextBlock Text='备注' />"
        "<TextBox Name='notes' Text='{Binding notes}' />"
        "<Spacer Height='6' />"
        "<CheckBox Name='agree' Content='同意条款' IsChecked='{Binding agree}' Click='OnAgree' />"
        "<CheckBox Name='news' Content='订阅更新' IsChecked='{Binding news}' />"
        "<Spacer Height='6' />"
        "<TextBlock Text='项目' />"
        "<ListBox Name='items' Items='Alpha;Beta;Gamma;Delta' SelectedIndex='{Binding selected}' "
                 "SelectionChanged='OnSelect' />"
        "<Spacer Height='6' />"
        "<StackPanel Orientation='Horizontal'>"
          "<Button Name='submit' Content='提交' Click='OnSubmit' />"
          "<Button Name='clear' Content='清空' Click='OnClear' />"
          "<Button Name='theme' Content='切换主题' Click='OnTheme' />"
        "</StackPanel>"
        "<TextBlock Text='{Binding status}' />"
      "</StackPanel>"
    "</Window>";

typedef struct {
    char name[64];
    char email[64];
    char notes[64];
    unsigned char agree;
    unsigned char news;
    int selected;
    int submitCount;
    char status[96];
} FormState;

static FormState g_state;

static void layout(int winId, void *user) {
    JpxXaml_Render((JpxXamlView *)user, winId);
}

static void on_name_changed(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)nodeIndex;
    (void)userData;
    FormState *state = (FormState *)JpxXamlView_Model(view);
    snprintf(state->status, sizeof(state->status), "姓名: %s", state->name);
    Jpx_Invalidate(JpxXamlView_App(view));
}

static void on_agree(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)nodeIndex;
    (void)userData;
    FormState *state = (FormState *)JpxXamlView_Model(view);
    snprintf(state->status, sizeof(state->status), "同意: %s",
             state->agree ? "是" : "否");
    Jpx_Invalidate(JpxXamlView_App(view));
}

static void on_select(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)nodeIndex;
    (void)userData;
    FormState *state = (FormState *)JpxXamlView_Model(view);
    snprintf(state->status, sizeof(state->status), "选中: %d", state->selected);
    Jpx_Invalidate(JpxXamlView_App(view));
}

static void on_submit(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)nodeIndex;
    (void)userData;
    FormState *state = (FormState *)JpxXamlView_Model(view);
    state->submitCount++;
    snprintf(state->status, sizeof(state->status), "提交 #%d: %s",
             state->submitCount, state->name);
    Jpx_Invalidate(JpxXamlView_App(view));
}

static void on_clear(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)nodeIndex;
    (void)userData;
    FormState *state = (FormState *)JpxXamlView_Model(view);
    state->name[0] = '\0';
    state->email[0] = '\0';
    state->notes[0] = '\0';
    state->agree = 0;
    state->news = 0;
    state->selected = 0;
    snprintf(state->status, sizeof(state->status), "已清空");
    Jpx_Invalidate(JpxXamlView_App(view));
}

static void on_theme(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)nodeIndex;
    (void)userData;
    FormState *state = (FormState *)JpxXamlView_Model(view);
    if (JpxXamlTheme_Apply(JpxXamlView_App(view), &g_theme))
        snprintf(state->status, sizeof(state->status), "主题已切换");
    else
        snprintf(state->status, sizeof(state->status), "主题切换失败");
    Jpx_Invalidate(JpxXamlView_App(view));
}

static void on_close(int winId, void *user) {
    (void)user;
    Jpx_WindowClose(g_app, winId);
}

int main(int argc, char **argv) {
    CrashReport_Install(NULL);
    static char fontPath[512];
    JpxOptions opt = {0};
    opt.width = 800;
    opt.height = 600;
    opt.tileW = 100;
    opt.tileH = 100;
    opt.port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9060;
    opt.webRoot = argc > 2 ? argv[2] : "../assets";
    /* 可选：--theme <path> 加载真实 ResourceDictionary 文本（开发调试用）；
       --preset <name> 加载预编译主题 assets/themes/<name>（启动路径）；
       --theme-css <file.css> 加载 CSS 子集主题（jpx_html_theme 编译）；
       --font <path> 指定主题字体（可指向系统字体，前端与 guest 共用同一文件）。 */
    const char *themePath = NULL;
    const char *presetName = NULL;
    const char *cssPath = NULL;
    const char *fontOverride = NULL;
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--theme") == 0) themePath = argv[i + 1];
        else if (strcmp(argv[i], "--preset") == 0) presetName = argv[i + 1];
        else if (strcmp(argv[i], "--theme-css") == 0) cssPath = argv[i + 1];
        else if (strcmp(argv[i], "--font") == 0) fontOverride = argv[i + 1];
    }
    if (fontOverride) snprintf(fontPath, sizeof(fontPath), "%s", fontOverride);
    else snprintf(fontPath, sizeof(fontPath), "%s/font.ttf", opt.webRoot);
    opt.fontPath = fontPath;
    opt.logLevel = 2;

    g_app = Jpx_Create(&opt);
    if (!g_app) {
        fprintf(stderr, "demo_xaml: Jpx_Create failed\n");
        return 1;
    }

    memset(&g_state, 0, sizeof(g_state));
    g_state.status[0] = '\0';

    char err[256];
    JpxXamlView *view = JpxXamlView_Create(g_app, FORM_XAML, &g_state, err, sizeof(err));
    if (!view) {
        fprintf(stderr, "demo_xaml: XAML parse failed: %s\n", err);
        Jpx_Destroy(g_app);
        return 1;
    }

    JpxXamlView_Bind(view, "name", JPX_XAML_BIND_TEXT, offsetof(FormState, name),
                     sizeof(g_state.name));
    JpxXamlView_Bind(view, "email", JPX_XAML_BIND_TEXT, offsetof(FormState, email),
                     sizeof(g_state.email));
    JpxXamlView_Bind(view, "notes", JPX_XAML_BIND_TEXT, offsetof(FormState, notes),
                     sizeof(g_state.notes));
    JpxXamlView_Bind(view, "agree", JPX_XAML_BIND_BOOL, offsetof(FormState, agree), 1);
    JpxXamlView_Bind(view, "news", JPX_XAML_BIND_BOOL, offsetof(FormState, news), 1);
    JpxXamlView_Bind(view, "selected", JPX_XAML_BIND_INT, offsetof(FormState, selected), 4);
    JpxXamlView_Bind(view, "status", JPX_XAML_BIND_TEXT, offsetof(FormState, status),
                     sizeof(g_state.status));

    JpxXamlView_On(view, "OnNameChanged", on_name_changed, NULL);
    JpxXamlView_On(view, "OnAgree", on_agree, NULL);
    JpxXamlView_On(view, "OnSelect", on_select, NULL);
    JpxXamlView_On(view, "OnSubmit", on_submit, NULL);
    JpxXamlView_On(view, "OnClear", on_clear, NULL);
    JpxXamlView_On(view, "OnTheme", on_theme, NULL);

    char themeErr[256];
    int themeLoaded = 0;
    char presetDir[512];
    char cssDir[512];
    if (themePath) {
        /* LoadFile 递归合并 MergedDictionaries（XP 家族二级继承等），开发调试用。 */
        themeLoaded = JpxXamlTheme_LoadFile(&g_theme, themePath, themeErr, sizeof(themeErr));
        if (!themeLoaded) fprintf(stderr, "demo_xaml: theme load failed: %s\n", themeErr);
    } else if (cssPath) {
        /* CSS 子集主题（jpx_html_theme，独立解析实现）。 */
        themeLoaded = JpxHtmlTheme_LoadCss(&g_theme, cssPath, themeErr, sizeof(themeErr));
        if (!themeLoaded) fprintf(stderr, "demo_xaml: css theme failed: %s\n", themeErr);
        else {
            snprintf(cssDir, sizeof(cssDir), "%s", cssPath);
            char *slash = strrchr(cssDir, '/');
            char *bslash = strrchr(cssDir, '\\');
            if (bslash > slash) slash = bslash;
            if (slash) *slash = '\0';
        }
    } else if (presetName) {
        /* 预编译主题：assets/themes/<name>（xalm.bin + strings.bin + resid2path.bin + 图片）。 */
        snprintf(presetDir, sizeof(presetDir), "%s/themes/%s", opt.webRoot, presetName);
        themeLoaded = JpxXamlTheme_LoadPrecompiled(&g_theme, presetDir, themeErr, sizeof(themeErr));
        if (!themeLoaded) fprintf(stderr, "demo_xaml: preset load failed: %s\n", themeErr);
    } else {
        themeLoaded = JpxXamlTheme_Parse(&g_theme, XP_THEME, themeErr, sizeof(themeErr));
        if (!themeLoaded) fprintf(stderr, "demo_xaml: builtin theme parse failed: %s\n", themeErr);
    }
    if (themeLoaded) {
        /* 图片相对路径基准：预编译主题取其目录；.xaml 主题取主题目录
           （RetroBar 全部主题同目录，"../Resources/.." 都能落到正确位置）；
           内置主题取 Web 资源根目录。 */
        char imgBase[512];
        snprintf(imgBase, sizeof(imgBase), "%s", opt.webRoot);
        if (themePath) {
            snprintf(imgBase, sizeof(imgBase), "%s", themePath);
            char *slash = strrchr(imgBase, '/');
            char *bslash = strrchr(imgBase, '\\');
            if (bslash > slash) slash = bslash;
            if (slash) *slash = '\0';
        } else if (presetName) {
            snprintf(imgBase, sizeof(imgBase), "%s", presetDir);
        } else if (cssPath) {
            snprintf(imgBase, sizeof(imgBase), "%s", cssDir);
        }
        JpxXamlTheme_ResolveImages(&g_theme, imgBase);
        printf("demo_xaml: theme loaded from %s, %d resources\n",
               themePath ? themePath
                         : (presetName ? presetName : (cssPath ? cssPath : "builtin")),
               g_theme.count);
        /* 图片由资源包打包下发：client 一次 GET 拿「命令数组 + 全部图片」。 */
        Jpx_ResPack_SetImageProvider(g_app, JpxXamlTheme_ImageAt, &g_theme);
    }

    JpxWindowDesc desc = {0};
    desc.title = "jp_xaml 表单";
    /* 高度 560：让列表、按钮行与状态文本都落在 body 内，避免被内容裁剪丢掉。 */
    desc.x = 190;
    desc.y = 30;
    desc.w = 420;
    desc.h = 560;
    desc.layout = layout;
    desc.user = view;
    desc.onClose = on_close;

    if (Jpx_WindowOpen(g_app, &desc) == 0) {
        fprintf(stderr, "demo_xaml: window open failed\n");
        JpxXamlView_Destroy(view);
        Jpx_Destroy(g_app);
        return 1;
    }

    /* 启动即应用主题：打开就是主题外观，不需要先点「切换主题」。
       资源以资源包下发，client 收 loadResPack(id) 后自己 GET /respack/<id>，
       刷新页面也会从首事件的同一个 id 恢复。 */
    if (g_theme.count > 0 && JpxXamlTheme_Apply(g_app, &g_theme)) {
        snprintf(g_state.status, sizeof(g_state.status), "主题: XP（%s）",
                 themePath ? "外置"
                           : (presetName ? "预编译" : (cssPath ? "css" : "内置")));
        Jpx_Invalidate(g_app);
    }

    int rc = Jpx_Run(g_app);
    JpxXamlView_Destroy(view);
    Jpx_Destroy(g_app);
    return rc;
}
