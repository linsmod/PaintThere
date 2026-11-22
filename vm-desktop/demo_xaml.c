/* jp_xaml 表单示例：界面结构写在 XAML 里，应用代码只有 state + handler。
   构建：python tools/build_vm.py → demo_xaml.exe
   运行：demo_xaml.exe 9060 ../assets
   验证：应用源码不出现 clay / vm_display / vm_tiles / json 词汇。 */

#include "jpx.h"
#include "jpx_xaml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static JpxApp *g_app;

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
