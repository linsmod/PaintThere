#ifndef JPX_INTERNAL_H
#define JPX_INTERNAL_H

/* JPX 内部共享声明：应用代码不得 include。 */

#include "jpx.h"
#include "jpx_text_edit.h"

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
#define JPX_ID_CAP 48
#define JPX_MAX_TEXT_INPUTS 16
#define JPX_MAX_FOCUSABLE 32
#define JPX_MAX_LISTS 8
/* 固定行高：滚动上限靠 totalRows × 行高算出来，不需要询问 Clay 内容高度
   （clip 容器的 boundingBox 是裁剪后的高度，问也问不到）。 */
#define JPX_LIST_ROW_H 22

typedef struct {
    int used;
    int id;
    int ms;
    unsigned long long last;
    void (*fn)(void *);
    void *user;
} JpxTimer;

/* 文本输入框的持久编辑状态，按控件 id 匹配，与声明顺序无关。 */
typedef struct {
    int used;
    char id[JPX_ID_CAP];
    JpxTextEdit ed;
} JpxEditSlot;

/* 本帧登记的可聚焦控件（声明序）：Tab 循环与按键路由用它。 */
typedef struct {
    char id[JPX_ID_CAP];
    size_t *len;             /* 回写用，可为 NULL */
    JpxTextEdit *ed;         /* 文本输入的编辑状态；非文本框为 NULL */
} JpxFocusable;

/* 列表的滚动状态。按 (窗口, 控件 id) 持久化，与声明顺序无关。 */
typedef struct {
    int used;
    int winId;
    char id[JPX_ID_CAP];
    int offset;        /* 已滚动像素，0 表示顶部 */
    int maxOffset;
    int viewH;
    int rowH;
    int totalRows;
    int rowIndex;      /* 本帧行序号，ListBegin 归零 */
    int rx, ry, rw, rh; /* 上一帧的窗口局部矩形：滚轮命中判定用 */
    int hasRect;
} JpxList;

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

    /* 控件焦点：焦点框、可聚焦控件登记、文本输入的持久状态 */
    char focusId[JPX_ID_CAP];
    JpxFocusable focusables[JPX_MAX_FOCUSABLE];
    int focusableCount;
    JpxEditSlot edits[JPX_MAX_TEXT_INPUTS];
} JpxWindow;

struct JpxApp {
    JpxOptions opt;
    JpxTheme theme;
    /* XAML 主题派生出的完整 VmTheme；hasVmTheme 时 shell/WM 用它而非默认值。
       颜色指针借自调用方（ResourceTable），生命周期由调用方保证。 */
    VmTheme vmTheme;
    int hasVmTheme;

    VmWm *wm;
    VmServer *server;
    VmShell *shell;
    VmTileGrid grid;
    VmDisplayList frame;
    VmDisplayList desktopProgram;

    JpxWindow windows[VM_WM_MAX_WINDOWS];
    int nextWinId;
    int currentWin;            /* 正在执行 layout 的窗口 id */
    int layoutWinId;           /* 正在 layout 的窗口；0 表示不在 layout 中。
                                  失效据此定位到单窗口，而不是全量作废。 */
    unsigned int frameId;
    int dirty;
    int running;

    /* 资源包：client 只收 loadResPack(id)，自己来取 /respack/<id>。
       resJson 是首连接一次性 events 的内容（现在只有一条 loadResPack）。 */
    char *resJson;
    char resPackId[64];
    char *resPackJson;
    int resPackSerial;
    char *tilesJson;
    char *fullJson;

    int ctrl, shift, alt;      /* 最近一次按键事件的修饰键 */
    int caretOn;               /* caret 闪烁相位：500ms 翻转一次 */
    JpxList lists[JPX_MAX_LISTS];
    int activeList;            /* 正在声明的列表下标 + 1；0 表示不在列表内 */
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

/* jpx_window.c：控件焦点链 */
JpxEditSlot *Jpx__EditSlot(JpxWindow *win, const char *id);
void Jpx__FocusRegister(JpxWindow *win, const char *id, size_t *len, JpxTextEdit *ed);
void Jpx__FocusTab(JpxWindow *win, int dir);
int  Jpx__FocusKey(JpxWindow *win, const char *key, int ctrl, int shift);

/* jpx_window.c */
JpxWindow *Jpx__WindowCreate(JpxApp *app, const JpxWindowDesc *desc);
void Jpx__WindowDestroy(JpxApp *app, int winId);

/* jpx_theme.c */
const JpxTheme *Jpx__DefaultTheme(void);
void Jpx__ApplyChrome(JpxApp *app);

/* jpx_widget.c：由 jpx_window.c 的根容器调用 */
void Jpx__OpenRoot(JpxApp *app, JpxWindow *win);

/* jpx_widget.c：滚轮命中某个列表就改它的 offset，返回 1 表示已消费。 */
int Jpx__ListWheel(JpxApp *app, int winId, int x, int y, int deltaY);

/* jpx_widget.c：layout 完成后刷新列表矩形（layout 期间查不到元素数据）。 */
void Jpx__ListRefreshRects(JpxApp *app, int winId);

#endif
