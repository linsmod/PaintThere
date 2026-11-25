#ifndef VM_WM_H
#define VM_WM_H

#include <time.h>

#include "vm_display.h"
#include "vm_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 任务栏 + 桌面层 + 应用窗口 + 开始菜单 + 设置窗口 + 右键弹窗要能同时存在；
   8 会正好顶格，弹窗失败比内存代价更伤。 */
#define VM_WM_MAX_WINDOWS 10
#define VM_WM_TITLE_HEIGHT 28
#define VM_WM_MAX_CURSORS 16
/* 单窗口内容缓冲的容量上限（编译期，层数组按它分配）。 */
#define VM_WM_LAYER_CAPACITY 1024
/* 默认单窗口命令配额。普通窗口 20–30 条，列表类窗口（ProcMan/Files）实测
   400 条以上：配额过小会截断内容并丢掉末尾的 clipPop，未闭合的裁剪会把
   后续窗口（任务栏在最后）整片吃掉。
   可用环境变量 VM_WM_LAYER_QUOTA 覆盖（1..VM_WM_LAYER_CAPACITY），
   超出时合成阶段置 VmWm.layerOverflow 并在画面上大字告警。 */
#define VM_WM_LAYER_DEFAULT_QUOTA 1024

/* 窗口内容布局：往 out 追加绘制命令（坐标已是画布坐标）。
   Clay / XAML / 手算布局都实现这个签名；WM 不认识任何 UI 库，
   Clay 只是 producer 侧的一个适配器（见 vm_ui_clay 与 vm_app）。 */
typedef void (*VmWindowLayout)(VmCmdBuffer *out, int x, int y, int w, int h,
                               void *userData);
/* 手绘窗口内容：直接往 out 追加绘制命令（坐标已是画布坐标）。
   用于需要渐变/图片填充这类 Clay 布局表达不了的界面（任务栏）。 */
typedef void (*VmWindowPaint)(VmCmdBuffer *out, int x, int y, int w, int h, void *userData);
/* 手绘窗口的点击：按下时把画布坐标交给窗口自己的命中逻辑，返回 1 表示已消费。
   button 为鼠标键号（0 左键 / 2 右键），NOFOCUS 窗口（任务栏）拿不到焦点，
   只能走这条通道。 */
typedef int (*VmWindowClickHandler)(int windowId, int x, int y, int button,
                                    void *userData);
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

typedef struct VmWm VmWm;
/* 窗口集合 / 焦点 / 最小化变化后的通知。任务栏这类「列出所有窗口」的界面
   靠它立即重建，而不是等定时刷新。回调里只允许置脏，不得增删窗口。 */
typedef void (*VmWmWindowListHandler)(VmWm *wm, void *userData);

/* 窗口内容缓冲：装饰 + 内容，坐标已是画布坐标。
   合成阶段按 z 序并入输出流，窗口因此成为可独立失效的内容单元。

   两种内容来源：
   - 本地（paint / Clay layout）：build_one_window 直接按画布坐标生成；
   - 外部（IPC 推来的命令数组）：存在 content[] 里，坐标是窗口 body 局部，
     合成时按 body 原点平移后拼进 commands。external = 1 时不再走 paint/layout。 */
typedef struct {
    VmDrawCommand commands[VM_WM_LAYER_CAPACITY];
    int count;
    int contentDirty;   /* 需要重新 layout + 重生成命令 */
    int chromeDirty;    /* 仅装饰变化（标题/边框/按钮着色） */
    uint32_t builtFrame;
    /* 最后一次把 contentDirty 置 1 的原因（字面量，仅诊断用）。 */
    const char *dirtyWhy;
    /* 外部内容：原始命令（body 局部坐标），与 commands 分开存放。 */
    int external;
    int contentCount;
    VmDrawCommand content[VM_WM_LAYER_CAPACITY];
} VmWindowLayer;

typedef struct {
    int id;
    const char *title;
    int x, y, w, h;
    int z;            /* higher is on top */
    int createdSeq;   /* 创建序，raise 不改；任务栏等需要稳定顺序的地方用它 */
    int focused;
    int minimized;
    int maximized;    /* 最大化时占据 workArea，restoreRect 留给还原 */
    int restoreX, restoreY, restoreW, restoreH;
    unsigned flags;                     /* VM_WIN_* 位或 */
    const char *bodyStyleId;            /* 正文背景样式 id；NULL 用 chrome->windowStyleId */
    VmWindowCloseHandler closeHandler;
    void *closeUserData;
    VmWindowLayout layout;
    void *userData;
    VmWindowPaint paint;              /* 非空时优先于 layout：直接输出绘制命令 */
    VmWindowClickHandler clickHandler;
    void *clickUserData;
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

struct VmWm {
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
    /* 每次 mousedown 递增。外壳等「不抢焦点但要点一次算一次」的窗口用它
       做按下沿判定，不依赖 Clay 的 PRESSED_THIS_FRAME（该状态只在有指针
       事件的帧推进，同一次按下会在后续重建帧里重复触发）。 */
    int pointerDownSeq;
    /* 上一次指针命中的窗口：hover 态转移时旧窗口也要重建。 */
    int lastHitId;
    /* 双击标题栏检测：上一次按在哪个窗口的标题栏、什么时候（毫秒）。 */
    int lastTitleDownId;
    unsigned long long lastTitleDownMs;
    /* 工作区：窗口可用的屏幕区域（扣掉任务栏）。最大化与拖动钳制都用它。 */
    int workX, workY, workW, workH;
    /* 正在跑 layout 的窗口 id（-1 表示不在布局中）。宿主据它把「布局中作废」
       收窄到单个窗口，而不是退化成全量作废。 */
    int layoutWinId;
    /* 窗口装饰参数；未设置时用 VmTheme_Default()->chrome */
    VmChrome chrome;
    /* 窗口内容是否裁剪到 body 矩形（默认 1）。 */
    int contentClip;
    /* 装饰或窗口状态变化后置位，由外部帧 provider 消费。 */
    int dirty;
    /* 最后一次把 dirty 置 1 的原因（字面量，仅诊断用）。 */
    const char *dirtyWhy;
    /* 单窗口命令配额（运行时，来自 VM_WM_LAYER_QUOTA 或默认值）。 */
    int quotaCommands;
    /* 某个窗口的命令超出配额，合成时有内容被截断。 */
    int layerOverflow;
    /* 累计调用 build_one_window 的次数：验证「未脏窗口不重跑布局」的观测点。 */
    uint32_t layoutCount;
    /* 本轮合成中超出单窗口配额的窗口（-1 表示没有）：画面上会大字告警。 */
    int overflowWinId;
    int overflowCount;   /* 该窗口真实需要的命令条数 = 写入数 + 被丢弃数 */
    char overflowTitle[64];
    /* 窗口列表变化通知（任务栏等界面订阅）。 */
    VmWmWindowListHandler listHandler;
    void *listUserData;
    /* popup 菜单（右键上下文菜单等）：打开中的菜单窗口 id 与外点关闭回调。
       mousedown 落在 popup 之外时先 dismiss 并消费本次按下。
       popupWindowId <= 0 表示当前没有 popup。 */
    int popupWindowId;
    void (*popupDismiss)(void *userData);
    void *popupUserData;
};

void VmWm_SetWindowListHandler(VmWm *wm, VmWmWindowListHandler handler, void *userData);

VmWm *VmWm_Create(int width, int height);
void VmWm_Free(VmWm *wm);

VmWindow *VmWm_AddWindow(VmWm *wm, int id, const char *title,
                         int x, int y, int w, int h,
                         VmWindowLayout layout, void *userData);
void VmWm_RemoveWindow(VmWm *wm, int id);
VmWindow *VmWm_FindWindow(VmWm *wm, int id);
VmWindow *VmWm_HitTest(VmWm *wm, int x, int y);

/* 标题栏按钮命中（画布坐标）。远程宿主用它把「点到 ×」转成 wms 命令。 */
enum {
    VM_WM_DECOR_NONE = 0,
    VM_WM_DECOR_CLOSE = 1,
    VM_WM_DECOR_MAXIMIZE = 2,
    VM_WM_DECOR_MINIMIZE = 3
};
int VmWm_WindowDecorHit(VmWm *wm, VmWindow *win, int x, int y);
void VmWm_Focus(VmWm *wm, int id);
void VmWm_Raise(VmWm *wm, int id);
void VmWm_Move(VmWm *wm, int id, int x, int y);
void VmWm_SetKeyHandler(VmWm *wm, int id, VmWindowKeyHandler handler, void *userData);
/* 手绘内容：设置后该窗口不再跑 Clay 布局，装饰之外的命令全部由 paint 产出。 */
void VmWm_SetWindowPaint(VmWm *wm, int id, VmWindowPaint paint, void *userData);
void VmWm_SetWindowClickHandler(VmWm *wm, int id, VmWindowClickHandler handler,
                                void *userData);
/* 注册 popup 菜单窗口（右键上下文菜单）：mousedown 落在它之外时先调 dismiss
   并消费本次按下（Windows 行为：外点只关菜单，不穿透）。关闭菜单后要清掉
   （popupWindowId 传 <= 0）。vm_menu 是它的第一个使用者。 */
void VmWm_SetPopup(VmWm *wm, int windowId,
                   void (*dismiss)(void *userData), void *userData);
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

/* 工作区：窗口可放置的区域（通常是画布减去任务栏）。默认等于整个画布。
   最大化用它做目标矩形，拖动时用它做钳制边界。 */
void VmWm_SetWorkArea(VmWm *wm, int x, int y, int w, int h);
/* 最大化 / 还原尺寸 / 切换。无装饰或 NOFOCUS 的窗口不参与，返回 0。 */
int VmWm_Maximize(VmWm *wm, int id);
int VmWm_Unmaximize(VmWm *wm, int id);
int VmWm_ToggleMaximize(VmWm *wm, int id);
int VmWm_IsMaximized(VmWm *wm, int id);
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

/* 只读诊断转储：窗口集合 / z 序 / 焦点 / 每个窗口的失效状态与原因。
   不改变任何状态，供 guest 侧排查「谁该重建、谁没重建」。tag 用来区分调用点。 */
void VmWm_Dump(const VmWm *wm, const char *tag);

/* 光标箭头 data URL：一次性放进 resources，不随帧重复下发。 */
const char *VmWm_CursorDataUrl(void);

#ifdef __cplusplus
}
#endif

#endif
