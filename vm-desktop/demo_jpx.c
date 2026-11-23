/* JPX 等价示例：与 demo_wm 同样的两个窗口，但应用代码只 include jpx.h。
   构建：python tools/build_vm.py → demo_jpx.exe
   运行：demo_jpx.exe 9000 ../assets */

#include "jpx/jpx.h"
#include "jpx_xaml/jpx_shell_settings.h"
#include "vm_sys.h"
#include "vm_fs.h"
#include "crash_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static JpxApp *g_app;

typedef struct {
    int clicks;
    char label[64];
    char name[64];
    size_t nameLen;
    char email[64];
    size_t emailLen;
} WindowAState;

typedef struct {
    char notes[64];
    size_t notesLen;
} WindowBState;

typedef struct {
    int selected;
} WindowCState;

/* ProcMan 的数据源。采样必须发生在 layout 之外：332 个进程的
   Toolhelp 快照 + OpenProcess × 332 要几十毫秒，放进布局会直接卡帧。 */
typedef struct {
    VmSysProc rows[VM_SYS_MAX_PROCS];
    int count;
    int total;
    unsigned long long sampleMs;
} ProcState;

/* Files 的数据源。目录扫描很快，不需要定时器：进入目录或点刷新时采样即可。 */
typedef struct {
    char path[VM_FS_PATH_MAX];
    VmFsEntry rows[VM_FS_MAX_ENTRIES];
    int count;
    int total;
} FilesState;

static WindowAState g_a;
static WindowBState g_b;
static WindowCState g_c;
static ProcState g_procs;
static FilesState g_files;

/* 开始菜单：默认只开基础窗口，两个演示程序由菜单启动。
   id 为 0 表示当前未打开；关闭后菜单恢复「启动」状态。 */
static int g_menuWin;
static int g_procWin;
static int g_filesWin;
static JpxWindowDesc g_menuDesc;
static JpxWindowDesc g_procDesc;
static JpxWindowDesc g_filesDesc;

/* 任务栏设置：配置持久化在 <webRoot>/shell.ini，任务栏右键「属性」打开。 */
static JpxShellSettings g_shellCfg;
static char g_webRoot[512];

/* 输入框已接管可打印键与编辑键；这里只收控件未消费的键（Enter 等）。 */
static int on_key(int winId, const char *key, const char *code, int isDown,
                  int ctrl, int shift, int alt, void *user) {
    (void)winId;
    (void)code;
    (void)ctrl;
    (void)shift;
    (void)alt;
    if (!isDown) return 0;
    WindowAState *state = (WindowAState *)user;
    if (strcmp(key, "Enter") == 0) {
        snprintf(state->label, sizeof(state->label), "Enter: %s", state->name);
        Jpx_Invalidate(g_app);
        return 1;
    }
    return 0;
}

static void on_close(int winId, void *user) {
    (void)user;
    Jpx_WindowClose(g_app, winId);
}

/* 两个程序窗口被 × 关闭时要清掉 id，否则开始菜单会一直显示「运行中」。 */
static void on_proc_close(int winId, void *user) {
    (void)user;
    Jpx_WindowClose(g_app, winId);
    g_procWin = 0;
    Jpx_Invalidate(g_app);
}

static void on_files_close(int winId, void *user) {
    (void)user;
    Jpx_WindowClose(g_app, winId);
    g_filesWin = 0;
    Jpx_Invalidate(g_app);
}

static void layout_a(int win, void *user) {
    WindowAState *state = (WindowAState *)user;

    Jpx_LabelEx(win, "表单 - Tab 切换焦点", &(JpxTextStyle){.wrap = 0, .align = 0});
    Jpx_Spacer(win, 0, 4);
    Jpx_Label(win, "姓名");
    Jpx_TextInput(win, "name", state->name, sizeof(state->name), &state->nameLen);
    Jpx_Label(win, "邮箱");
    Jpx_TextInput(win, "email", state->email, sizeof(state->email), &state->emailLen);

    Jpx_RowBegin(win);
    int clicked = Jpx_Button(win, "btn", "点击");
    Jpx_Spacer(win, 8, 0);
    Jpx_Label(win, state->label[0] ? state->label : "尚未点击");
    Jpx_RowEnd(win);
    if (clicked) {
        state->clicks++;
        snprintf(state->label, sizeof(state->label), "已点击 %d 次", state->clicks);
    }
}

static void layout_b(int win, void *user) {
    WindowBState *state = (WindowBState *)user;
    Jpx_LabelEx(win, "主题面板", &(JpxTextStyle){.wrap = 0, .align = 0});
    Jpx_TextInput(win, "notes", state->notes, sizeof(state->notes), &state->notesLen);
    Jpx_PanelBegin(win);
    Jpx_Label(win, "颜色与圆角来自主题 token");
    Jpx_Separator(win);
    Jpx_Label(win, "同一份 token 驱动窗口与控件");
    Jpx_PanelEnd(win);
}

/* 可滚动列表：20 行塞进 120px 视口，滚轮每次 3 行。 */
static void layout_c(int win, void *user) {
    WindowCState *state = (WindowCState *)user;
    static char rowText[20][24];
    static const char *cols[2];
    static int widths[2] = {0, 70};
    /* 必须是 static：Clay 只在 EndLayout 之后才把字符串拷进绘制命令，
       layout 回调的栈变量那时已经失效。 */
    static char status[64];

    Jpx_LabelEx(win, "可滚动列表 - 滚轮", &(JpxTextStyle){.wrap = 0, .align = 0});
    static const char *headers[2] = {"项目", "奇偶"};
    Jpx_RowBegin(win);
    for (int i = 0; i < 2; i++) {
        Jpx_LabelEx(win, headers[i], &(JpxTextStyle){.wrap = 0, .align = 0});
    }
    Jpx_RowEnd(win);
    if (Jpx_ListBegin(win, "list", 120, 20)) {
        for (int i = 0; i < 20; i++) {
            snprintf(rowText[i], sizeof(rowText[i]), "行 %02d", i);
            cols[0] = rowText[i];
            cols[1] = (i % 2) ? "奇" : "偶";
            if (Jpx_ListRow(win, "list", cols, widths, 2, state->selected == i)) {
                state->selected = i;
                Jpx_Invalidate(g_app);
            }
        }
        Jpx_ListEnd(win);
    }
    snprintf(status, sizeof(status), "已选 = %d", state->selected);
    Jpx_Label(win, status);
}

static void procs_sample(ProcState *st) {
    unsigned long long t0 = VmSys_UptimeMs();
    st->count = VmSys_ListProcs(st->rows, VM_SYS_MAX_PROCS, &st->total);
    st->sampleMs = VmSys_UptimeMs() - t0;
}

static void procs_tick(void *user) {
    ProcState *st = (ProcState *)user;
    /* 程序被开始菜单关掉后不必再定时采样整张进程表。 */
    if (!g_app || !g_procWin) return;
    procs_sample(st);
    Jpx_Invalidate(g_app);
}

/* ProcMan：进程名 / PID / 内存 / CPU / [结束]。
   只有可见行才格式化文本——332 行全格式化要 332 份字符串缓冲，
   而屏幕上同时最多看到 6 行。 */
static void layout_d(int win, void *user) {
    ProcState *st = (ProcState *)user;
    static char header[128];
    static char cells[24][4][32];
    static const char *cols[4];

    int self = (int)VmSys_SelfPid();
    snprintf(header, sizeof(header), "%d procs (shown %d), sample %llu ms",
             st->total, st->count, st->sampleMs);
    Jpx_Label(win, header);

    if (!Jpx_ListBegin(win, "procs", 120, st->count)) return;
    int first = 0, last = -1;
    Jpx_ListVisibleRange(win, "procs", &first, &last);
    if (last - first >= 24) last = first + 23;

    int widths[4] = {0, 46, 66, 44};
    for (int i = 0; i < st->count; i++) {
        const char *btn = NULL;
        if (i >= first && i <= last) {
            int slot = i - first;
            VmSysProc *p = &st->rows[i];
            snprintf(cells[slot][0], sizeof(cells[slot][0]), "%s", p->name);
            snprintf(cells[slot][1], sizeof(cells[slot][1]), "%d", (int)p->pid);
            snprintf(cells[slot][2], sizeof(cells[slot][2]), "%.1f MB",
                     (double)p->memBytes / (1024.0 * 1024.0));
            snprintf(cells[slot][3], sizeof(cells[slot][3]), "%d%%", p->cpuPermille / 10);
            cols[0] = cells[slot][0];
            cols[1] = cells[slot][1];
            cols[2] = cells[slot][2];
            cols[3] = cells[slot][3];
            /* 不给自己一个「结束」按钮：点了就把演示程序自己杀掉。 */
            if ((int)p->pid != self) btn = "结束";
        } else {
            cols[0] = cols[1] = cols[2] = cols[3] = "";
        }

        int hit = Jpx_ListRowEx(win, "procs", cols, widths, 4, 0, btn);
        if (hit & 2) {
            if (VmSys_KillProc(st->rows[i].pid) == VM_SYS_OK) {
                procs_sample(st);
                Jpx_Invalidate(g_app);
                break;
            }
        }
    }
    Jpx_ListEnd(win);
}

static void files_refresh(FilesState *st) {
    st->count = VmFs_ListDir(st->path, st->rows, VM_FS_MAX_ENTRIES, &st->total);
}

static void files_enter(FilesState *st, const char *name) {
    char next[VM_FS_PATH_MAX];
    int n = snprintf(next, sizeof(next), "%s/%s", st->path, name);
    if (n <= 0 || n >= VM_FS_PATH_MAX) return;
    snprintf(st->path, sizeof(st->path), "%s", next);
    files_refresh(st);
}

static void files_up(FilesState *st) {
    char *slash = strrchr(st->path, '/');
    if (!slash) slash = strrchr(st->path, '\\');
    if (!slash) return;
    if (slash == st->path) st->path[1] = '\0';
    else *slash = '\0';
    files_refresh(st);
}

/* Files：名称 / 大小 / 修改日期 / [打开]。和 ProcMan 一样只格式化可见行，
   Windows 下目录项名字经 UTF-16 -> UTF-8 转换后直接显示中文。 */
static void layout_e(int win, void *user) {
    FilesState *st = (FilesState *)user;
    static char header[VM_FS_PATH_MAX + 64];
    static char cells[24][3][VM_FS_NAME_MAX + 32];
    static const char *cols[3];

    const char *tail = st->path;
    int pathLen = (int)strlen(tail);
    if (pathLen > 56) tail += pathLen - 56;
    snprintf(header, sizeof(header), "%s  (%d items)", tail, st->total);
    Jpx_Label(win, header);

    Jpx_RowBegin(win);
    int up = Jpx_Button(win, "up", "上级");
    int refresh = Jpx_Button(win, "refresh", "刷新");
    Jpx_RowEnd(win);
    if (up) {
        files_up(st);
        Jpx_Invalidate(g_app);
        return;
    }
    if (refresh) {
        files_refresh(st);
        Jpx_Invalidate(g_app);
        return;
    }

    if (!Jpx_ListBegin(win, "files", 180, st->count)) return;
    int first = 0, last = -1;
    Jpx_ListVisibleRange(win, "files", &first, &last);
    if (last - first >= 24) last = first + 23;

    int widths[3] = {0, 56, 84};
    for (int i = 0; i < st->count; i++) {
        const char *btn = NULL;
        if (i >= first && i <= last) {
            VmFsEntry *e = &st->rows[i];
            int slot = i - first;
            snprintf(cells[slot][0], sizeof(cells[slot][0]), "%s%s",
                     e->name, e->isDir ? "/" : "");
            if (e->isDir) {
                snprintf(cells[slot][1], sizeof(cells[slot][1]), "<DIR>");
                btn = "打开";
            } else {
                snprintf(cells[slot][1], sizeof(cells[slot][1]), "%.1f KB",
                         e->size / 1024.0);
            }
            time_t t = (time_t)(e->mtimeMs / 1000);
            struct tm *tmVal = localtime(&t);
            if (tmVal)
                snprintf(cells[slot][2], sizeof(cells[slot][2]), "%04d-%02d-%02d",
                         tmVal->tm_year + 1900, tmVal->tm_mon + 1, tmVal->tm_mday);
            else
                snprintf(cells[slot][2], sizeof(cells[slot][2]), "-");
            cols[0] = cells[slot][0];
            cols[1] = cells[slot][1];
            cols[2] = cells[slot][2];
        } else {
            cols[0] = cols[1] = cols[2] = "";
        }

        int hit = Jpx_ListRowEx(win, "files", cols, widths, 3, 0, btn);
        if (hit & 2) {
            files_enter(st, st->rows[i].name);
            Jpx_Invalidate(g_app);
            break;
        }
    }
    Jpx_ListEnd(win);
}

/* 菜单按钮在 layout 回调里只记意图，实际开关窗口放到定时器里做：
   正在布局的窗口不能在回调中途被销毁。 */
static int g_pendingLaunch = -1;   /* -1 无 / 0 ProcMan / 1 Files */
static int g_pendingCloseMenu;
/* 任务栏右键「属性」：同样只记意图，由 launcher_tick 统一开窗。 */
static int g_pendingSettings;

static void launcher_close_menu(void) {
    if (g_menuWin) {
        Jpx_WindowClose(g_app, g_menuWin);
        g_menuWin = 0;
    }
}

static void launcher_tick(void *user) {
    (void)user;
    if (!g_app) return;
    if (g_pendingSettings) {
        g_pendingSettings = 0;
        JpxShellSettings_Open(g_app, g_webRoot, &g_shellCfg);
        Jpx_Invalidate(g_app);
    }
    if (g_pendingCloseMenu) {
        g_pendingCloseMenu = 0;
        launcher_close_menu();
        Jpx_Invalidate(g_app);
    }
    if (g_pendingLaunch < 0) return;
    int which = g_pendingLaunch;
    g_pendingLaunch = -1;

    if (which == 2) {
        g_menuWin = Jpx_WindowOpen(g_app, &g_menuDesc);
        Jpx_Invalidate(g_app);
        return;
    }

    launcher_close_menu();
    if (which == 0) {
        /* 已经打开就关掉重开，新窗口自然在最上层，等价于切回该程序。 */
        if (g_procWin) {
            Jpx_WindowClose(g_app, g_procWin);
            g_procWin = 0;
        }
        procs_sample(&g_procs);
        g_procWin = Jpx_WindowOpen(g_app, &g_procDesc);
    } else {
        if (g_filesWin) {
            Jpx_WindowClose(g_app, g_filesWin);
            g_filesWin = 0;
        }
        files_refresh(&g_files);
        g_filesWin = Jpx_WindowOpen(g_app, &g_filesDesc);
    }
    Jpx_Invalidate(g_app);
}

/* 任务栏开始按钮：回调里只记意图，由 launcher_tick 统一开关窗口。 */
static void on_start(void *user) {
    (void)user;
    if (!g_app) return;
    if (g_menuWin) g_pendingCloseMenu = 1;
    else {
        g_pendingLaunch = 2;   /* 2 表示打开开始菜单（-1 为无意图） */
    }
    Jpx_Invalidate(g_app);
}

static void layout_menu(int win, void *user) {
    (void)user;
    Jpx_LabelEx(win, "启动程序", &(JpxTextStyle){.wrap = 0, .align = 0});
    Jpx_Separator(win);
    if (Jpx_Button(win, "m_proc", "ProcMan · 进程管理")) {
        g_pendingLaunch = 0;
        Jpx_Invalidate(g_app);
    }
    if (Jpx_Button(win, "m_files", "Files · 文件浏览")) {
        g_pendingLaunch = 1;
        Jpx_Invalidate(g_app);
    }
    Jpx_Separator(win);
    if (Jpx_Button(win, "m_close", "关闭菜单")) {
        g_pendingCloseMenu = 1;
        Jpx_Invalidate(g_app);
    }
}

/* 桌面图标层：无装饰、置底、不抢焦点的大按钮，缩短常用程序的启动路径。 */
static void layout_desktop(int win, void *user) {
    (void)user;
    Jpx_Spacer(win, 0, 18);
    Jpx_LabelEx(win, "快捷方式", &(JpxTextStyle){.wrap = 0, .align = 0});
    if (Jpx_Button(win, "d_proc", "进程管理")) {
        g_pendingLaunch = 0;
        Jpx_Invalidate(g_app);
    }
    if (Jpx_Button(win, "d_files", "文件浏览")) {
        g_pendingLaunch = 1;
        Jpx_Invalidate(g_app);
    }
}

/* 时钟每秒刷新：只作废任务栏窗口，其它窗口零帧。 */
static void clock_tick(void *user) {
    (void)user;
    if (!g_app) return;
    Jpx_ShellRefresh(g_app);
}

/* 任务栏右键「属性」：打开设置窗口。回调里只记意图，
   由 launcher_tick 统一开窗（与开始菜单同一防重入模式）。 */
static void on_settings_open(void *user) {
    (void)user;
    g_pendingSettings = 1;
    Jpx_Invalidate(g_app);
}

int main(int argc, char **argv) {
    CrashReport_Install(NULL);
    static char fontPath[512];
    JpxOptions opt = {0};
    opt.width = 800;
    opt.height = 600;
    opt.tileW = 100;
    opt.tileH = 100;
    opt.port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9000;
    opt.webRoot = argc > 2 ? argv[2] : "../assets";
    snprintf(g_webRoot, sizeof(g_webRoot), "%s", opt.webRoot);
    snprintf(fontPath, sizeof(fontPath), "%s/font.ttf", opt.webRoot);
    opt.fontPath = fontPath;
    opt.logLevel = 2;

    g_app = Jpx_Create(&opt);
    if (!g_app) {
        fprintf(stderr, "demo_jpx: Jpx_Create failed\n");
        return 1;
    }

    /* 现代深色：hover 用亮蓝紫，替换原橙色。 */
    JpxTheme theme = *Jpx_Theme(g_app);
    theme.hoverColor = "rgba(156,152,255,1)";
    Jpx_SetTheme(g_app, &theme);

    /* 桌面层先创建（z 最低），三个应用窗口置中上，底部留给任务栏。 */
    JpxWindowDesc desktop = {0};
    desktop.title = "desktop";
    desktop.x = 20;
    desktop.y = 272;
    desktop.w = 150;
    desktop.h = 200;
    desktop.flags = JPX_WIN_UNDECORATED | JPX_WIN_PINNED | JPX_WIN_NOFOCUS;
    desktop.layout = layout_desktop;

    JpxWindowDesc a = {0};
    a.title = "表单演示";
    a.x = 24;
    a.y = 24;
    a.w = 240;
    a.h = 230;
    a.layout = layout_a;
    a.user = &g_a;
    a.onClose = on_close;
    a.onKey = on_key;

    JpxWindowDesc b = {0};
    b.title = "主题面板";
    b.x = 280;
    b.y = 24;
    b.w = 240;
    b.h = 230;
    b.layout = layout_b;
    b.user = &g_b;
    b.onClose = on_close;

    JpxWindowDesc c = {0};
    c.title = "列表";
    c.x = 536;
    c.y = 24;
    c.w = 240;
    c.h = 230;
    c.layout = layout_c;
    c.user = &g_c;
    c.onClose = on_close;

    g_procDesc.title = "ProcMan · 进程管理";
    g_procDesc.x = 60;
    g_procDesc.y = 160;
    g_procDesc.w = 420;
    g_procDesc.h = 320;
    g_procDesc.layout = layout_d;
    g_procDesc.user = &g_procs;
    g_procDesc.onClose = on_proc_close;

    g_filesDesc.title = "Files · 文件浏览";
    g_filesDesc.x = 340;
    g_filesDesc.y = 180;
    g_filesDesc.w = 420;
    g_filesDesc.h = 320;
    g_filesDesc.layout = layout_e;
    g_filesDesc.user = &g_files;
    g_filesDesc.onClose = on_files_close;

    g_menuDesc.title = "开始菜单";
    g_menuDesc.x = 6;
    g_menuDesc.y = 330;
    g_menuDesc.w = 210;
    g_menuDesc.h = 200;
    g_menuDesc.flags = JPX_WIN_UNDECORATED | JPX_WIN_PINNED;
    g_menuDesc.layout = layout_menu;

    procs_sample(&g_procs);
    if (!VmFs_Cwd(g_files.path, sizeof(g_files.path)))
        snprintf(g_files.path, sizeof(g_files.path), ".");
    files_refresh(&g_files);

    if (Jpx_WindowOpen(g_app, &desktop) == 0 || Jpx_WindowOpen(g_app, &a) == 0 ||
        Jpx_WindowOpen(g_app, &b) == 0 || Jpx_WindowOpen(g_app, &c) == 0) {
        fprintf(stderr, "demo_jpx: window open failed\n");
        Jpx_Destroy(g_app);
        return 1;
    }
    Jpx_SetStartHandler(g_app, on_start, NULL);
    Jpx_SetShellSettingsHandler(g_app, on_settings_open, NULL);
    /* 启动时读 <webRoot>/shell.ini 并应用主题与任务栏开关。 */
    JpxShellSettings_Init(g_app, g_webRoot, NULL, &g_shellCfg);
    Jpx_TimerEvery(g_app, 2000, procs_tick, &g_procs);
    Jpx_TimerEvery(g_app, 60, launcher_tick, NULL);
    Jpx_TimerEvery(g_app, 1000, clock_tick, NULL);

    int rc = Jpx_Run(g_app);
    Jpx_Destroy(g_app);
    return rc;
}
