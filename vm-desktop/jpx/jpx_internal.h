#ifndef JPX_INTERNAL_H
#define JPX_INTERNAL_H

/* JPX 内部共享声明：应用代码不得 include。 */

#include "jpx.h"

#include "clay.h"
#include "vm_display.h"
#include "vm_font.h"
#include "vm_server.h"
#include "vm_shell.h"
#include "vm_theme.h"
#include "vm_tiles.h"
#include "vm_wm.h"

#define JPX_MAX_TIMERS 8
#define JPX_TITLE_CAP 64

typedef struct {
    int used;
    int id;
    int ms;
    unsigned long long last;
    void (*fn)(void *);
    void *user;
} JpxTimer;

typedef struct {
    JpxApp *app;
    int id;                    /* VmWm 窗口 id，同时也是应用侧 winId */
    JpxWindowDesc desc;        /* layout / user / onClose / onKey 的副本 */
    char title[JPX_TITLE_CAP];
    Clay_Context *ctx;
    void *arenaMem;
    int used;
    int pointerDownLast;   /* 上一帧本窗口是否被按下，用于取按下沿 */
    int pressEdge;         /* 本帧是否发生「按下」沿 */
} JpxWindow;

struct JpxApp {
    JpxOptions opt;
    JpxTheme theme;

    VmWm *wm;
    VmServer *server;
    VmShell *shell;
    VmTileGrid grid;
    VmDisplayList frame;
    VmDisplayList desktopProgram;

    JpxWindow windows[VM_WM_MAX_WINDOWS];
    int nextWinId;
    int currentWin;            /* 正在执行 layout 的窗口 id */
    unsigned int frameId;
    int dirty;
    int running;

    char *resJson;
    char *tilesJson;
    char *fullJson;

    int ctrl, shift, alt;      /* 最近一次按键事件的修饰键 */
    JpxTimer timers[JPX_MAX_TIMERS];
    int nextTimerId;

    /* 控件内部装饰元素的帧内序号；每帧开头归零，用于生成唯一元素 id。 */
    uint32_t localSerial;
};

/* 单进程单 JpxApp：Clay 的 current context 与度量函数都是进程级状态。 */
JpxApp *Jpx__App(void);
JpxWindow *Jpx__Window(JpxApp *app, int winId);
void Jpx__Invalidate(JpxApp *app);

Clay_Color Jpx__Color(const char *text, Clay_Color fallback);
Clay_ElementId Jpx__Id(int win, const char *id);
Clay_String Jpx__Str(const char *text);

/* 无应用语义 id 的装饰元素：父元素 id + 帧内序号，避免同窗口内重复 id。
   对应 Clay 的 CLAY_SIDI_LOCAL 语义。 */
Clay_ElementId Jpx__LocalId(JpxApp *app, const char *name);

/* jpx_window.c */
JpxWindow *Jpx__WindowCreate(JpxApp *app, const JpxWindowDesc *desc);
void Jpx__WindowDestroy(JpxApp *app, int winId);
Clay_RenderCommandArray Jpx__WindowRunLayout(void *userData);

/* jpx_theme.c */
const JpxTheme *Jpx__DefaultTheme(void);
void Jpx__ApplyChrome(JpxApp *app);

/* jpx_widget.c：由 jpx_window.c 的根容器调用 */
void Jpx__OpenRoot(JpxApp *app, JpxWindow *win);

#endif
