/* jp_xaml 表单示例：界面结构写在 XAML 里，应用代码只有 state + handler。
   构建：python tools/build_vm.py → demo_xaml.exe
   运行：demo_xaml.exe 9060 ../assets
   验证：应用源码不出现 clay / vm_display / vm_tiles / json 词汇。 */

#include "jpx.h"
#include "jpx_xaml.h"
#include "jpx_xaml_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JpxApp *g_app;

/* RetroBar 风格资源字典：暖橙主题，用于验证热切换。 */
static const char *WARM_THEME =
    "<ResourceDictionary>"
      "<SolidColorBrush x:Key='AccentColor' Color='#FFFF8C42' />"
      "<SolidColorBrush x:Key='TaskbarBackground' Color='#FF2A1F1A' />"
      "<SolidColorBrush x:Key='TaskTextColor' Color='#FFFFF0E0' />"
      "<SolidColorBrush x:Key='WindowBackground' Color='#FF3A2A20' />"
      "<SolidColorBrush x:Key='TaskButtonBackground' Color='#FFFF8C42' />"
      "<SolidColorBrush x:Key='accentTextColor' Color='#FF2A1F1A' />"
      "<SolidColorBrush x:Key='DesktopFrom' Color='#FF2A1F1A' />"
      "<SolidColorBrush x:Key='DesktopTo' Color='#FF4A3020' />"
      "<system:Double x:Key='TaskbarHeight'>30</system:Double>"
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
    static char fontPath[512];
    JpxOptions opt = {0};
    opt.width = 800;
    opt.height = 600;
    opt.tileW = 100;
    opt.tileH = 100;
    opt.port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9060;
    opt.webRoot = argc > 2 ? argv[2] : "../assets";
    /* 可选：--theme <path> 加载真实 ResourceDictionary 文件（如 RetroBar 的 XP Blue）。 */
    const char *themePath = NULL;
    for (int i = 3; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--theme") == 0) themePath = argv[i + 1];
    }
    snprintf(fontPath, sizeof(fontPath), "%s/font.ttf", opt.webRoot);
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
    const char *themeXml = WARM_THEME;
    char *themeFile = NULL;
    if (themePath) {
        FILE *tf = fopen(themePath, "rb");
        if (tf) {
            fseek(tf, 0, SEEK_END);
            long n = ftell(tf);
            fseek(tf, 0, SEEK_SET);
            themeFile = (char *)malloc((size_t)n + 1);
            if (themeFile) {
                if (fread(themeFile, 1, (size_t)n, tf) == (size_t)n) {
                    themeFile[n] = '\0';
                    themeXml = themeFile;
                } else { free(themeFile); themeFile = NULL; }
            }
            fclose(tf);
        } else {
            fprintf(stderr, "demo_xaml: cannot open theme file: %s\n", themePath);
        }
    }
    if (!JpxXamlTheme_Parse(&g_theme, themeXml, themeErr, sizeof(themeErr))) {
        fprintf(stderr, "demo_xaml: theme parse failed: %s\n", themeErr);
    } else {
        printf("demo_xaml: theme loaded from %s, %d resources\n",
               themeFile ? themePath : "builtin", g_theme.count);
    }
    free(themeFile);

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

    int rc = Jpx_Run(g_app);
    JpxXamlView_Destroy(view);
    Jpx_Destroy(g_app);
    return rc;
}
