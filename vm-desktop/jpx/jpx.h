#ifndef JPX_H
#define JPX_H

/* JPX：guest 侧应用框架的唯一公开入口。
   应用只 include 这个头文件，不接触 Clay / VmDisplay / tiles / JSON / frameId。 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JpxApp JpxApp;

typedef struct {
    int width, height;        /* 逻辑桌面，如 800×600 */
    int tileW, tileH;         /* 远端瓦片尺寸，如 100×100；网格由 JPX 推导 */
    const char *fontPath;     /* 与前端同一份 TTF */
    const char *webRoot;
    unsigned short port;
    int logLevel;             /* 0 静默 / 1 告警 / 2 信息 */
} JpxOptions;

typedef struct {
    const char *bgColor, *fgColor, *borderColor, *accentColor, *accentTextColor;
    const char *inputBgColor, *disabledColor, *selectedColor;
    const char *hoverColor;   /* NULL 时用 accent 提亮 20% 推导 */
    int fontId, fontSize, radius, padding, gap;
} JpxTheme;

/* ---- 窗口 ---- */
typedef void (*JpxLayoutFn)(int winId, void *user);
typedef void (*JpxCloseFn)(int winId, void *user);
typedef int  (*JpxKeyFn)(int winId, const char *key, const char *code,
                         int isDown, int ctrl, int shift, int alt, void *user);

/* 窗口标志：与 VM_WIN_* 同值，应用可给菜单、桌面层等特权窗口使用。 */
enum {
    JPX_WIN_UNDECORATED = 1,
    JPX_WIN_TOPMOST     = 2,
    JPX_WIN_PINNED      = 4,
    JPX_WIN_NOFOCUS     = 8
};

typedef struct {
    const char *title;
    int x, y, w, h;
    unsigned flags;       /* JPX_WIN_* 位或；普通窗口填 0 */
    JpxLayoutFn layout;
    void *user;
    JpxCloseFn onClose;   /* 被点 × 时调用；应用自行调 Jpx_WindowClose 才真正关闭 */
    JpxKeyFn onKey;       /* 控件未消费的键；返回 1 表示已处理 */
} JpxWindowDesc;

/* ---- 布局 ---- */
void Jpx_RowBegin(int win);
void Jpx_RowEnd(int win);
void Jpx_ColumnBegin(int win);
void Jpx_ColumnEnd(int win);
void Jpx_PanelBegin(int win);
void Jpx_PanelEnd(int win);
void Jpx_ScrollBegin(int win, const char *id, int height);
void Jpx_ScrollEnd(int win);
void Jpx_Spacer(int win, int w, int h);

/* ---- 控件（返回值 = 本帧是否发生该交互） ---- */
typedef struct { int wrap; int align; } JpxTextStyle;   /* align: 0 左 / 1 中 / 2 右 */
void Jpx_Label(int win, const char *text);
void Jpx_LabelEx(int win, const char *text, const JpxTextStyle *style);
int  Jpx_Button(int win, const char *id, const char *text);
int  Jpx_Checkbox(int win, const char *id, const char *label, int *checked);
int  Jpx_TextInput(int win, const char *id, char *buf, size_t cap, size_t *len);
int  Jpx_ListBox(int win, const char *id, const char *const *items, int count,
                 int *selected, int visibleRows);
void Jpx_Separator(int win);

/* ---- 可滚动列表（固定行高 22px，滚轮驱动）----
   height：可视高度；totalRows：总行数（调用方知道，用来算滚动上限）。
   行高固定，所以不需要在 layout 后反查内容高度。 */
int  Jpx_ListBegin(int win, const char *id, int height, int totalRows);
/* 一行多列：第一列自动伸展，其余按 widths[i] 固定宽（widths 为 NULL 时全部 FIT）。
   返回 1 表示本帧该行被点击。 */
int  Jpx_ListRow(int win, const char *id, const char *const *cols,
                 const int *widths, int ncols, int selected);
/* 行尾附加一个按钮：返回值 bit0 = 行被点击，bit1 = 按钮被点击（两者互斥）。 */
int  Jpx_ListRowEx(int win, const char *id, const char *const *cols,
                   const int *widths, int ncols, int selected, const char *btnText);
/* 当前可见的行下标区间（含部分可见的首尾行）。用临时缓冲拼列文本时先查它，
   只格式化这几行，不必为几百行数据准备几百份字符串缓冲。 */
void Jpx_ListVisibleRange(int win, const char *id, int *first, int *last);
void Jpx_ListEnd(int win);

/* ---- 运行时 ---- */
JpxApp *Jpx_Create(const JpxOptions *opt);
void    Jpx_Destroy(JpxApp *app);
int     Jpx_Run(JpxApp *app);                  /* 阻塞；0 正常退出 */
void    Jpx_Stop(JpxApp *app);
void    Jpx_SetTheme(JpxApp *app, const JpxTheme *theme);
const JpxTheme *Jpx_Theme(JpxApp *app);

int  Jpx_WindowOpen(JpxApp *app, const JpxWindowDesc *desc);   /* 返回 winId > 0 */
void Jpx_WindowClose(JpxApp *app, int winId);
void Jpx_WindowSetTitle(JpxApp *app, int winId, const char *title);
/* 最大化 / 还原 / 切换 / 查询。最大化占满工作区（画布减去任务栏）。
   标题栏双击与 □ 按钮走同一条 WM 路径，应用调用只是等价入口。 */
void Jpx_WindowMaximize(JpxApp *app, int winId);
void Jpx_WindowUnmaximize(JpxApp *app, int winId);
void Jpx_WindowToggleMaximize(JpxApp *app, int winId);
int  Jpx_WindowIsMaximized(JpxApp *app, int winId);
void Jpx_Invalidate(JpxApp *app);

int  Jpx_TimerEvery(JpxApp *app, int ms, void (*fn)(void *), void *user);
void Jpx_TimerCancel(JpxApp *app, int timerId);

/* 只刷新任务栏窗口（时钟/开始按钮态），不重建其它窗口内容。 */
void Jpx_ShellRefresh(JpxApp *app);
/* 任务栏开始按钮回调：应用只给函数指针，不接触 VmShell。 */
typedef void (*JpxStartHandler)(void *userData);
void Jpx_SetStartHandler(JpxApp *app, JpxStartHandler handler, void *userData);

void Jpx_ClipboardSet(const char *utf8);
int  Jpx_ClipboardGet(char *out, int cap);

void Jpx_Log(JpxApp *app, int level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
