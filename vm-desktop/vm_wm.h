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

typedef Clay_RenderCommandArray (*VmWindowLayout)(void *userData);
/* Guest-internal key handler: return 1 if the key was consumed. */
typedef int (*VmWindowKeyHandler)(int windowId, const char *key, const char *code,
                                  int isDown, void *userData);
/* Guest-internal wheel handler: x/y 为窗口局部坐标，deltaY>0 表示向下滚。 */
typedef int (*VmWindowWheelHandler)(int windowId, int x, int y, int deltaY, void *userData);

typedef struct {
    int id;
    const char *title;
    int x, y, w, h;
    int z;            /* higher is on top */
    int focused;
    int minimized;
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
    int focusId;
    /* drag state */
    int dragId;
    int dragOffX, dragOffY;
    /* multi-client cursors (guest-side only, drawn as primitives) */
    VmCursor cursors[VM_WM_MAX_CURSORS];
    int cursorCount;
    char focusClientId[64];
    /* 全局指针（最近一次指针事件的画布坐标）：用于跨窗口 hover 判定 */
    int pointerX, pointerY;
    int pointerDown;
    int pointerValid;
    /* 窗口装饰参数；未设置时用 VmTheme_Default()->chrome */
    VmChrome chrome;
    /* 窗口内容是否裁剪到 body 矩形（默认 1）。 */
    int contentClip;
    /* 装饰或窗口状态变化后置位，由外部帧 provider 消费。 */
    int dirty;
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
void VmWm_SetWheelHandler(VmWm *wm, int id, VmWindowWheelHandler handler, void *userData);

/* 窗口装饰参数（标题高度/边框宽度/样式 id/颜色/字体）。NULL 恢复默认主题。 */
void VmWm_SetChrome(VmWm *wm, const VmChrome *chrome);
/* 窗口内容裁剪到 body 矩形，默认开启。 */
void VmWm_SetContentClip(VmWm *wm, int enabled);
/* 标记需要出帧；输入导致的状态变化也会置位。 */
void VmWm_Invalidate(VmWm *wm);
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
