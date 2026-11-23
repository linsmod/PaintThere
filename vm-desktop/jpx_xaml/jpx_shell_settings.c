/* 任务栏设置组件实现。XAML 在 Open 时按扫描到的主题列表动态拼装，
   应用代码只有 state + handler（与 demo_xaml 同一范式）。
   绑定偏移相对 model 基址，所以表单绑定的是组件内部的 ui 影子结构；
   「应用」时才写回调用方的 cfg。 */

#include "jpx_shell_settings.h"

#include "jpx_xaml.h"
#include "jpx_xaml_theme.h"
#include "vm_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_MAX_THEMES 32
#define SETTINGS_NAME_CAP 64

struct JpxShellSettingsCtx {
    JpxApp *app;
    JpxShellSettings *cfg;          /* 借用调用方的存储（长期有效） */
    char configPath[VM_FS_PATH_MAX];
    char themesRoot[VM_FS_PATH_MAX];
    char names[SETTINGS_MAX_THEMES][SETTINGS_NAME_CAP];
    int nameCount;
    int themeIdx;                   /* ListBox 选中下标；-1 = 无 */
    int winId;
    int pendingClose;               /* 布局中不能销毁自己：关闭走意图 + 定时器 */
    int closeTimerId;
    JpxShellSettings ui;            /* 表单绑定基址（model） */
    JpxShellSettings backup;        /* 取消时回滚 cfg */
    JpxXamlView *view;
    /* 当前应用的主题资源：颜色指针借给 VmTheme，必须长期持有。 */
    JpxXamlResourceTable themeTable;
    int themeTableUsed;
    char status[96];
};

static JpxShellSettingsCtx g_set;

/* ---- 配置文件：key=value 行 ---- */

static void trim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = '\0';
}

static int config_read(const char *path, JpxShellSettings *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        if (strcmp(key, "showClock") == 0) cfg->showClock = atoi(val) ? 1 : 0;
        else if (strcmp(key, "showTrayIcon") == 0) cfg->showTrayIcon = atoi(val) ? 1 : 0;
        else if (strcmp(key, "theme") == 0) snprintf(cfg->theme, sizeof(cfg->theme), "%s", val);
    }
    fclose(f);
    return 1;
}

static int config_write(const char *path, const JpxShellSettings *cfg) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "showClock=%d\n", cfg->showClock ? 1 : 0);
    fprintf(f, "showTrayIcon=%d\n", cfg->showTrayIcon ? 1 : 0);
    fprintf(f, "theme=%s\n", cfg->theme);
    fclose(f);
    return 1;
}

/* ---- 主题扫描与应用 ---- */

static void scan_themes(JpxShellSettingsCtx *s) {
    s->nameCount = 0;
    VmFsEntry entries[SETTINGS_MAX_THEMES];
    int total = 0;
    int n = VmFs_ListDir(s->themesRoot, entries, SETTINGS_MAX_THEMES, &total);
    for (int i = 0; i < n && s->nameCount < SETTINGS_MAX_THEMES; i++) {
        if (!entries[i].isDir) continue;
        snprintf(s->names[s->nameCount++], SETTINGS_NAME_CAP, "%s", entries[i].name);
    }
    /* 平台原始顺序不承诺排序：主题列表按名字排，展示才稳定。 */
    for (int i = 1; i < s->nameCount; i++) {
        char tmp[SETTINGS_NAME_CAP];
        snprintf(tmp, sizeof(tmp), "%s", s->names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(s->names[j], tmp) > 0) {
            snprintf(s->names[j + 1], SETTINGS_NAME_CAP, "%s", s->names[j]);
            j--;
        }
        snprintf(s->names[j + 1], SETTINGS_NAME_CAP, "%s", tmp);
    }
}

static int theme_index_of(const JpxShellSettingsCtx *s, const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < s->nameCount; i++) {
        if (strcmp(s->names[i], name) == 0) return i;
    }
    return -1;
}

/* 应用 ui 里的改动到 shell 与主题。失败返回 0（状态文本里写明原因）。 */
static int apply_settings(JpxShellSettingsCtx *s) {
    JpxApp *app = s->app;
    Jpx_ShellSetShowClock(app, s->ui.showClock);
    Jpx_ShellSetShowTrayIcon(app, s->ui.showTrayIcon);
    if (s->themeIdx >= 0 && s->themeIdx < s->nameCount) {
        snprintf(s->cfg->theme, sizeof(s->cfg->theme), "%s", s->names[s->themeIdx]);
    }
    if (!s->cfg->theme[0]) return 1;

    char dir[VM_FS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/%s", s->themesRoot, s->cfg->theme);
    char err[256];
    if (!JpxXamlTheme_LoadPrecompiled(&s->themeTable, dir, err, sizeof(err))) {
        snprintf(s->status, sizeof(s->status), "主题加载失败: %s", err);
        return 0;
    }
    JpxXamlTheme_ResolveImages(&s->themeTable, dir);
    if (!JpxXamlTheme_Apply(app, &s->themeTable)) {
        snprintf(s->status, sizeof(s->status), "主题应用失败");
        return 0;
    }
    s->themeTableUsed = 1;
    /* 资源包里的主题图片：换主题后 provider 指到新表。 */
    Jpx_ResPack_SetImageProvider(app, JpxXamlTheme_ImageAt, &s->themeTable);
    return 1;
}

/* ---- XAML 表单 handler ---- */

static void on_changed(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    snprintf(g_set.status, sizeof(g_set.status), "改动未保存，按「应用」生效");
    Jpx_Invalidate(g_set.app);
}

static void on_select(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    snprintf(g_set.status, sizeof(g_set.status), "主题: %s",
             g_set.themeIdx >= 0 ? g_set.names[g_set.themeIdx] : "(未选择)");
    Jpx_Invalidate(g_set.app);
}

static void do_apply(void) {
    /* 表单绑定的是 ui 影子结构：落盘前先把开关写回调用方的 cfg，
       否则配置文件里永远是打开窗口时的旧值。主题名由 apply_settings 写。 */
    g_set.cfg->showClock = g_set.ui.showClock;
    g_set.cfg->showTrayIcon = g_set.ui.showTrayIcon;
    if (apply_settings(&g_set)) {
        config_write(g_set.configPath, g_set.cfg);
        g_set.backup = *g_set.cfg;
        snprintf(g_set.status, sizeof(g_set.status), "已应用并保存");
    }
    Jpx_Invalidate(g_set.app);
}

static void on_apply(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    do_apply();
}

static void settings_tick(void *userData) {
    JpxShellSettingsCtx *s = (JpxShellSettingsCtx *)userData;
    if (!s || !s->pendingClose) return;
    s->pendingClose = 0;
    if (s->closeTimerId) {
        Jpx_TimerCancel(s->app, s->closeTimerId);
        s->closeTimerId = 0;
    }
    /* 此时已不在布局回调里：销毁窗口安全。 */
    if (s->winId) {
        int id = s->winId;
        s->winId = 0;
        Jpx_WindowClose(s->app, id);
        if (s->view) {
            JpxXamlView_Destroy(s->view);
            s->view = NULL;
        }
    }
    Jpx_Invalidate(s->app);
}

/* 布局回调里只记关闭意图：正在布局的窗口不能在回调中途被销毁
   （Clay 还在往窗口数据里写，销毁会 memmove/释放导致越界）。
   与 demo_jpx 的 launcher_tick 同一模式。 */
static void request_close(void) {
    g_set.pendingClose = 1;
}

static void on_ok(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    do_apply();
    request_close();
}

static void on_cancel(JpxXamlView *view, int nodeIndex, void *userData) {
    (void)view;
    (void)nodeIndex;
    (void)userData;
    *g_set.cfg = g_set.backup;
    g_set.ui = g_set.backup;
    request_close();
    Jpx_Invalidate(g_set.app);
}

static void settings_layout(int winId, void *user) {
    JpxXaml_Render((JpxXamlView *)user, winId);
}

static void on_window_close(int winId, void *user) {
    (void)winId;
    (void)user;
    /* 标题栏 ×：视为取消（回滚未应用的改动）。同样只记意图。 */
    *g_set.cfg = g_set.backup;
    g_set.ui = g_set.backup;
    request_close();
    Jpx_Invalidate(g_set.app);
}

/* ---- 公开 API ---- */

int JpxShellSettings_Init(JpxApp *app, const char *webRoot,
                          const char *configPath, JpxShellSettings *cfg) {
    if (!app || !webRoot || !cfg) return 0;
    memset(&g_set, 0, sizeof(g_set));
    g_set.app = app;
    g_set.cfg = cfg;
    cfg->showClock = 1;
    cfg->showTrayIcon = 1;
    cfg->theme[0] = '\0';
    snprintf(g_set.themesRoot, sizeof(g_set.themesRoot), "%s/themes", webRoot);
    if (configPath && *configPath) {
        snprintf(g_set.configPath, sizeof(g_set.configPath), "%s", configPath);
    } else {
        snprintf(g_set.configPath, sizeof(g_set.configPath), "%s/shell.ini", webRoot);
    }
    int existed = config_read(g_set.configPath, cfg);
    scan_themes(&g_set);
    g_set.ui = *cfg;
    g_set.themeIdx = theme_index_of(&g_set, cfg->theme);
    /* 启动即应用：配置里点了名的主题与开关马上生效。 */
    apply_settings(&g_set);
    g_set.backup = *cfg;
    g_set.status[0] = '\0';
    return existed;
}

int JpxShellSettings_Open(JpxApp *app, const char *webRoot, JpxShellSettings *cfg) {
    if (!app || !cfg) return 0;
    if (g_set.winId) return g_set.winId;

    if (!g_set.app) {
        /* 组件可只作弹窗用（罕见）：先按默认值初始化。 */
        JpxShellSettings_Init(app, webRoot, NULL, cfg);
    } else {
        g_set.app = app;
        g_set.cfg = cfg;
        scan_themes(&g_set);
    }
    g_set.ui = *cfg;
    g_set.backup = *cfg;
    g_set.themeIdx = theme_index_of(&g_set, cfg->theme);
    snprintf(g_set.status, sizeof(g_set.status), "主题: %s",
             g_set.themeIdx >= 0 ? g_set.names[g_set.themeIdx] : "(未选择)");

    /* Items 属性动态拼装：主题列表来自运行时扫描。 */
    char items[SETTINGS_MAX_THEMES * SETTINGS_NAME_CAP];
    items[0] = '\0';
    for (int i = 0; i < g_set.nameCount; i++) {
        if (i > 0) snprintf(items + strlen(items), sizeof(items) - strlen(items), ";");
        snprintf(items + strlen(items), sizeof(items) - strlen(items), "%s", g_set.names[i]);
    }

    char xaml[1600];
    snprintf(xaml, sizeof(xaml),
        "<Window Title='任务栏属性' Width='340' Height='420'>"
          "<StackPanel Orientation='Vertical'>"
            "<TextBlock Text='任务栏' />"
            "<CheckBox Name='clock' Content='显示时钟' IsChecked='{Binding showClock}' Click='OnChanged' />"
            "<CheckBox Name='tray' Content='显示托盘图标' IsChecked='{Binding showTrayIcon}' Click='OnChanged' />"
            "<Spacer Height='8' />"
            "<TextBlock Text='主题（预编译主题目录）' />"
            "<ListBox Name='themes' Items='%s' Rows='8' SelectedIndex='{Binding themeIdx}' SelectionChanged='OnSelect' />"
            "<Spacer Height='8' />"
            "<TextBlock Text='{Binding status}' />"
            "<StackPanel Orientation='Horizontal'>"
              "<Button Name='apply' Content='应用' Click='OnApply' />"
              "<Button Name='ok' Content='确定' Click='OnOk' />"
              "<Button Name='cancel' Content='取消' Click='OnCancel' />"
            "</StackPanel>"
          "</StackPanel>"
        "</Window>",
        items);

    char err[256];
    g_set.view = JpxXamlView_Create(app, xaml, &g_set, err, sizeof(err));
    if (!g_set.view) {
        fprintf(stderr, "shell_settings: XAML parse failed: %s\n", err);
        return 0;
    }
    /* 绑定偏移必须相对 model 基址（&g_set）：ui 字段的真实地址 =
       offsetof(ctx, ui) + offsetof(JpxShellSettings, field)。
       直接用 offsetof(JpxShellSettings, ...) 会把 1 字节写进 ctx 开头的
       app 指针里，点复选框即段错误。 */
    JpxXamlView_Bind(g_set.view, "showClock", JPX_XAML_BIND_BOOL,
                     offsetof(JpxShellSettingsCtx, ui) +
                         offsetof(JpxShellSettings, showClock), 1);
    JpxXamlView_Bind(g_set.view, "showTrayIcon", JPX_XAML_BIND_BOOL,
                     offsetof(JpxShellSettingsCtx, ui) +
                         offsetof(JpxShellSettings, showTrayIcon), 1);
    JpxXamlView_Bind(g_set.view, "themeIdx", JPX_XAML_BIND_INT,
                     offsetof(JpxShellSettingsCtx, themeIdx), 4);
    JpxXamlView_Bind(g_set.view, "status", JPX_XAML_BIND_TEXT,
                     offsetof(JpxShellSettingsCtx, status), sizeof(g_set.status));
    JpxXamlView_On(g_set.view, "OnChanged", on_changed, NULL);
    JpxXamlView_On(g_set.view, "OnSelect", on_select, NULL);
    JpxXamlView_On(g_set.view, "OnApply", on_apply, NULL);
    JpxXamlView_On(g_set.view, "OnOk", on_ok, NULL);
    JpxXamlView_On(g_set.view, "OnCancel", on_cancel, NULL);

    JpxWindowDesc desc = {0};
    desc.title = "任务栏属性";
    /* 470 高 + y=90：底部按钮行完整落在 800×600 工作区内（任务栏 36 高）。 */
    desc.x = 220;
    desc.y = 90;
    desc.w = 340;
    desc.h = 470;
    desc.layout = settings_layout;
    desc.user = g_set.view;
    desc.onClose = on_window_close;

    g_set.winId = Jpx_WindowOpen(app, &desc);
    if (g_set.winId == 0) {
        fprintf(stderr, "shell_settings: window open failed（窗口数已达上限？）\n");
        JpxXamlView_Destroy(g_set.view);
        g_set.view = NULL;
        return 0;
    }
    /* 关闭意图的轮询：60ms 内完成「确定/取消/×」的延迟销毁。 */
    g_set.closeTimerId = Jpx_TimerEvery(app, 60, settings_tick, &g_set);
    Jpx_Invalidate(app);
    return g_set.winId;
}

int JpxShellSettings_IsOpen(void) {
    return g_set.winId != 0;
}
