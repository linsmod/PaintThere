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
    const char *inputBgColor, *disabledColor;
    const char *hoverColor;   /* NULL 时用 accent 提亮 20% 推导 */
    int fontId, fontSize, radius, padding, gap;
} JpxTheme;

/* ---- 窗口 ---- */
typedef void (*JpxLayoutFn)(int winId, void *user);
typedef void (*JpxCloseFn)(int winId, void *user);
typedef int  (*JpxKeyFn)(int winId, const char *key, const char *code,
                         int isDown, int ctrl, int shift, int alt, void *user);

typedef struct {
    const char *title;
    int x, y, w, h;
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
void Jpx_Invalidate(JpxApp *app);

int  Jpx_TimerEvery(JpxApp *app, int ms, void (*fn)(void *), void *user);
void Jpx_TimerCancel(JpxApp *app, int timerId);

void Jpx_ClipboardSet(const char *utf8);
int  Jpx_ClipboardGet(char *out, int cap);

void Jpx_Log(JpxApp *app, int level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif
