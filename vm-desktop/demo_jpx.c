/* JPX 等价示例：与 demo_wm 同样的两个窗口，但应用代码只 include jpx.h。
   构建：python tools/build_vm.py → demo_jpx.exe
   运行：demo_jpx.exe 9000 ../assets */

#include "jpx/jpx.h"
#include "vm_sys.h"
#include "vm_fs.h"

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

    Jpx_Label(win, "Tab moves focus; click an input to focus it");
    Jpx_Label(win, "Name");
    Jpx_TextInput(win, "name", state->name, sizeof(state->name), &state->nameLen);
    Jpx_Label(win, "Email");
    Jpx_TextInput(win, "email", state->email, sizeof(state->email), &state->emailLen);

    Jpx_RowBegin(win);
    int clicked = Jpx_Button(win, "btn", "Click me");
    Jpx_RowEnd(win);
    if (clicked) {
        state->clicks++;
        snprintf(state->label, sizeof(state->label), "Clicked: %d", state->clicks);
    }

    /* label 由按钮点击或 Enter 写入，布局里不能每帧覆盖，否则应用层的消息被冲掉。 */
    Jpx_Label(win, state->label[0] ? state->label : "Clicked: 0");
}

static void layout_b(int win, void *user) {
    WindowBState *state = (WindowBState *)user;
    Jpx_Label(win, "Window B");
    Jpx_TextInput(win, "notes", state->notes, sizeof(state->notes), &state->notesLen);
    Jpx_PanelBegin(win);
    Jpx_Label(win, "A panel drawn from theme tokens");
    Jpx_Separator(win);
    Jpx_Label(win, "Second line");
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

    Jpx_Label(win, "Scroll this list with the wheel");
    if (Jpx_ListBegin(win, "list", 120, 20)) {
        for (int i = 0; i < 20; i++) {
            snprintf(rowText[i], sizeof(rowText[i]), "row %02d", i);
            cols[0] = rowText[i];
            cols[1] = (i % 2) ? "odd" : "even";
            if (Jpx_ListRow(win, "list", cols, widths, 2, state->selected == i)) {
                state->selected = i;
                Jpx_Invalidate(g_app);
            }
        }
        Jpx_ListEnd(win);
    }
    snprintf(status, sizeof(status), "selected = %d (0 = none)", state->selected);
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

static void launcher_close_menu(void) {
    if (g_menuWin) {
        Jpx_WindowClose(g_app, g_menuWin);
        g_menuWin = 0;
    }
}

static void launcher_tick(void *user) {
    (void)user;
    if (!g_app) return;
    if (g_pendingCloseMenu) {
        g_pendingCloseMenu = 0;
        launcher_close_menu();
        Jpx_Invalidate(g_app);
    }
    if (g_pendingLaunch < 0) return;
    int which = g_pendingLaunch;
    g_pendingLaunch = -1;

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

static void layout_start(int win, void *user) {
    (void)user;
    if (Jpx_Button(win, "start", "开始")) {
        if (g_menuWin) launcher_close_menu();
        else g_menuWin = Jpx_WindowOpen(g_app, &g_menuDesc);
        Jpx_Invalidate(g_app);
    }
}

static void layout_menu(int win, void *user) {
    (void)user;
    Jpx_Label(win, "启动程序");
    if (Jpx_Button(win, "m_proc", "ProcMan - 进程管理")) {
        g_pendingLaunch = 0;
        Jpx_Invalidate(g_app);
    }
    if (Jpx_Button(win, "m_files", "Files - 文件浏览")) {
        g_pendingLaunch = 1;
        Jpx_Invalidate(g_app);
    }
    Jpx_Separator(win);
    if (Jpx_Button(win, "m_close", "关闭菜单")) {
        g_pendingCloseMenu = 1;
        Jpx_Invalidate(g_app);
    }
}

int main(int argc, char **argv) {
    static char fontPath[512];
    JpxOptions opt = {0};
    opt.width = 800;
    opt.height = 600;
    opt.tileW = 100;
    opt.tileH = 100;
    opt.port = argc > 1 ? (unsigned short)atoi(argv[1]) : 9000;
    opt.webRoot = argc > 2 ? argv[2] : "../assets";
    snprintf(fontPath, sizeof(fontPath), "%s/font.ttf", opt.webRoot);
    opt.fontPath = fontPath;
    opt.logLevel = 2;

    g_app = Jpx_Create(&opt);
    if (!g_app) {
        fprintf(stderr, "demo_jpx: Jpx_Create failed\n");
        return 1;
    }

    /* 与 demo_wm 的按钮视觉对齐：hover 用橙色。 */
    JpxTheme theme = *Jpx_Theme(g_app);
    theme.hoverColor = "rgba(240,140,40,1)";
    Jpx_SetTheme(g_app, &theme);

    /* 基础窗口上三下二，底部留给开始按钮。ProcMan / Files 不在这里打开，
       由开始菜单启动；菜单弹出时覆盖在下方窗口之上。 */
    JpxWindowDesc a = {0};
    a.title = "Window A";
    a.x = 30;
    a.y = 30;
    a.w = 245;
    a.h = 190;
    a.layout = layout_a;
    a.user = &g_a;
    a.onClose = on_close;
    a.onKey = on_key;

    JpxWindowDesc b = {0};
    b.title = "Window B";
    b.x = 285;
    b.y = 30;
    b.w = 245;
    b.h = 190;
    b.layout = layout_b;
    b.user = &g_b;
    b.onClose = on_close;

    JpxWindowDesc c = {0};
    c.title = "List";
    c.x = 540;
    c.y = 30;
    c.w = 230;
    c.h = 190;
    c.layout = layout_c;
    c.user = &g_c;
    c.onClose = on_close;

    JpxWindowDesc start = {0};
    start.title = "Start";
    start.x = 30;
    start.y = 510;
    start.w = 140;
    start.h = 80;
    start.layout = layout_start;
    start.onClose = on_close;

    g_procDesc.title = "ProcMan";
    g_procDesc.x = 30;
    g_procDesc.y = 240;
    g_procDesc.w = 370;
    g_procDesc.h = 250;
    g_procDesc.layout = layout_d;
    g_procDesc.user = &g_procs;
    g_procDesc.onClose = on_proc_close;

    g_filesDesc.title = "Files";
    g_filesDesc.x = 420;
    g_filesDesc.y = 240;
    g_filesDesc.w = 350;
    g_filesDesc.h = 250;
    g_filesDesc.layout = layout_e;
    g_filesDesc.user = &g_files;
    g_filesDesc.onClose = on_files_close;

    g_menuDesc.title = "Start Menu";
    g_menuDesc.x = 30;
    g_menuDesc.y = 290;
    g_menuDesc.w = 220;
    g_menuDesc.h = 200;
    g_menuDesc.layout = layout_menu;

    procs_sample(&g_procs);
    if (!VmFs_Cwd(g_files.path, sizeof(g_files.path)))
        snprintf(g_files.path, sizeof(g_files.path), ".");
    files_refresh(&g_files);

    if (Jpx_WindowOpen(g_app, &a) == 0 || Jpx_WindowOpen(g_app, &b) == 0 ||
        Jpx_WindowOpen(g_app, &c) == 0 || Jpx_WindowOpen(g_app, &start) == 0) {
        fprintf(stderr, "demo_jpx: window open failed\n");
        Jpx_Destroy(g_app);
        return 1;
    }
    Jpx_TimerEvery(g_app, 2000, procs_tick, &g_procs);
    Jpx_TimerEvery(g_app, 60, launcher_tick, NULL);

    int rc = Jpx_Run(g_app);
    Jpx_Destroy(g_app);
    return rc;
}
