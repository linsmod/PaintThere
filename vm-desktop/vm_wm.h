#ifndef VM_WM_H
#define VM_WM_H

#include <time.h>

#include "clay.h"
#include "clay_frame_adapter.h"
#include "vm_display.h"
#include "vm_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_WM_MAX_WINDOWS 8
#define VM_WM_TITLE_HEIGHT 28
#define VM_WM_MAX_CURSORS 16
/* 单窗口内容配额：实测一窗口 8–30 条命令，64 留出余量；
   超出时合成阶段置 VmWm.layerOverflow，不静默截断。 */
#define VM_WM_LAYER_COMMANDS 64

typedef Clay_RenderCommandArray (*VmWindowLayout)(void *userData);
/* Guest-internal key handler: return 1 if the key was consumed. */
typedef int (*VmWindowKeyHandler)(int windowId, const char *key, const char *code,
                                  int isDown, void *userData);
/* Guest-internal wheel handler: x/y 为窗口局部坐标，deltaY>0 表示向下滚。 */
typedef int (*VmWindowWheelHandler)(int windowId, int x, int y, int deltaY, void *userData);

/* 窗口标志：桌面外壳、对话框这类特殊窗口用，应用层不直接操作。 */
enum {
    VM_WIN_UNDECORATED = 1,   /* 无标题栏与边框，不可拖动 */
    VM_WIN_TOPMOST     = 2,   /* 恒在普通窗口之上 */
    VM_WIN_PINNED      = 4,   /* 不可拖动 */
    VM_WIN_NOFOCUS     = 8    /* 不抢焦点（任务栏、悬浮提示） */
};
/* 装饰上的 × 只发关闭请求，真正移除由持有者决定。 */
typedef void (*VmWindowCloseHandler)(int windowId, void *userData);

/* 窗口内容缓冲：装饰 + 内容，坐标已是画布坐标。
   合成阶段按 z 序并入输出流，窗口因此成为可独立失效的内容单元。 */
typedef struct {
    VmDrawCommand commands[VM_WM_LAYER_COMMANDS];
    int count;
    int contentDirty;   /* 需要重新 layout + 重生成命令 */
    int chromeDirty;    /* 仅装饰变化（标题/边框/按钮着色） */
    uint32_t builtFrame;
} VmWindowLayer;

typedef struct {
    int id;
    const char *title;
    int x, y, w, h;
    int z;            /* higher is on top */
    int createdSeq;   /* 创建序，raise 不改；任务栏等需要稳定顺序的地方用它 */
    int focused;
    int minimized;
    unsigned flags;                     /* VM_WIN_* 位或 */
    const char *bodyStyleId;            /* 正文背景样式 id；NULL 用 chrome->windowStyleId */
    VmWindowCloseHandler closeHandler;
    void *closeUserData;
    Clay_Context *clayCtx;
    VmWindowLayout layout;
    void *userData;
    VmWindowKeyHandler keyHandler;
    void *keyUserData;
    VmWindowWheelHandler wheelHandler;
    void *wheelUserData;
    /* pointer state in window-local layout coordinates */
    int pointerX, pointerY;
    int pointerDown;
    int clicks;             /* number of PRESSED_THIS_FRAME handled by app */
    VmWindowLayer layer;    /* 内容缓冲与失效标记 */
} VmWindow;

typedef struct {
    char id[64];
    char color[32];
    int x, y;
    int visible;
    time_t lastSeen;   /* updated on every input from this client */
} VmCursor;

typedef struct {
    int width, height;
    VmWindow windows[VM_WM_MAX_WINDOWS];
    int count;
    int nextZ;
    int nextSeq;
    int focusId;
    /* drag state */
    int dragId;
    int dragOffX, dragOffY;
    /* multi-client cursors (guest-side only, drawn as primitives) */
    VmCursor cursors[VM_WM_MAX_CURSORS];
    int cursorCount;
    char focusClientId[64];
    /* 最近一次键盘事件的修饰键；keyHandler 签名不含修饰键，用 VmWm_KeyModifiers 取 */
    int keyCtrl, keyShift, keyAlt;
    /* 全局指针（最近一次指针事件的画布坐标）：用于跨窗口 hover 判定 */
    int pointerX, pointerY;
    int pointerDown;
    int pointerValid;
    /* 上一次指针命中的窗口：hover 态转移时旧窗口也要重建。 */
    int lastHitId;
    /* 窗口装饰参数；未设置时用 VmTheme_Default()->chrome */
    VmChrome chrome;
    /* 窗口内容是否裁剪到 body 矩形（默认 1）。 */
    int contentClip;
    /* 装饰或窗口状态变化后置位，由外部帧 provider 消费。 */
    int dirty;
    /* 某个窗口的命令超出 VM_WM_LAYER_COMMANDS，合成时有内容被截断。 */
    int layerOverflow;
    /* 累计调用 build_one_window 的次数：验证「未脏窗口不重跑布局」的观测点。 */
    uint32_t layoutCount;
} VmWm;

VmWm *VmWm_Create(int width, int height);
void VmWm_Free(VmWm *wm);

VmWindow *VmWm_AddWindow(VmWm *wm, int id, const char *title,
                         int x, int y, int w, int h,
                         VmWindowLayout layout, void *userData);
void VmWm_RemoveWindow(VmWm *wm, int id);
VmWindow *VmWm_FindWindow(VmWm *wm, int id);
VmWindow *VmWm_HitTest(VmWm *wm, int x, int y);
void VmWm_Focus(VmWm *wm, int id);
void VmWm_Raise(VmWm *wm, int id);
void VmWm_Move(VmWm *wm, int id, int x, int y);
void VmWm_SetKeyHandler(VmWm *wm, int id, VmWindowKeyHandler handler, void *userData);
/* 最近一次键盘事件的修饰键；keyHandler 内部调用即得该次事件的修饰键。 */
void VmWm_KeyModifiers(VmWm *wm, int *ctrl, int *shift, int *alt);
void VmWm_SetWheelHandler(VmWm *wm, int id, VmWindowWheelHandler handler, void *userData);

void VmWm_SetWindowFlags(VmWm *wm, int id, unsigned flags);
unsigned VmWm_WindowFlags(VmWm *wm, int id);
void VmWm_SetCloseHandler(VmWm *wm, int id, VmWindowCloseHandler handler, void *userData);
/* 按 z 升序写入窗口 id（含最小化与置顶窗口），返回总数（可能大于 cap）。 */
int VmWm_Enumerate(VmWm *wm, int *ids, int cap);
/* 按创建序升序写入窗口 id，与聚焦/raise 无关；任务栏等需要稳定顺序的界面用它。 */
int VmWm_EnumerateCreated(VmWm *wm, int *ids, int cap);
void VmWm_Minimize(VmWm *wm, int id);
void VmWm_Restore(VmWm *wm, int id);
int VmWm_IsMinimized(VmWm *wm, int id);
int VmWm_FocusId(const VmWm *wm);
/* 窗口正文矩形（去掉装饰）；无装饰窗口即整个窗口。 */
void VmWm_WindowBody(const VmWm *wm, const VmWindow *win, VmRect *out);

/* 窗口装饰参数（标题高度/边框宽度/样式 id/颜色/字体）。NULL 恢复默认主题。 */
void VmWm_SetChrome(VmWm *wm, const VmChrome *chrome);
/* 窗口内容裁剪到 body 矩形，默认开启。 */
void VmWm_SetContentClip(VmWm *wm, int enabled);
/* 标记需要出帧；输入导致的状态变化也会置位。 */
void VmWm_Invalidate(VmWm *wm);
/* 只作废一个窗口的内容：调用方能定位到窗口时用它，避免整帧重建。 */
void VmWm_InvalidateWindow(VmWm *wm, int id);
int VmWm_IsDirty(const VmWm *wm);
/* 取走脏标记：有脏返回 1 并清零。 */
int VmWm_ConsumeDirty(VmWm *wm);

/* Handle raw input JSON body. Returns 1 if the input changed window state. */
int VmWm_HandleInput(VmWm *wm, const char *jsonBody, size_t length);

/* Build a composite frame: background + all windows in z order. */
void VmWm_BuildFrame(VmWm *wm, VmDisplayList *out, uint32_t frameId,
                     const char *(*fontResolver)(uint16_t));

/* 窗口两两重叠区域的并集：z 序变化时只有这块区域显示会变。返回 1 表示存在重叠。 */
int VmWm_OverlapUnion(VmWm *wm, VmRect *out);

/* 光标箭头 data URL：一次性放进 resources，不随帧重复下发。 */
const char *VmWm_CursorDataUrl(void);

#ifdef __cplusplus
}
#endif

#endif
