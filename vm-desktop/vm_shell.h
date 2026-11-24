#ifndef VM_SHELL_H
#define VM_SHELL_H

/* 桌面外壳（任务栏）：两个层次。

   1. 纯绘制层（本文件多数 API）：输入是一份窗口条目快照 VmShellModel，
      输出是绘制命令或一个"要做什么"的动作。不依赖 vm_wm / Clay，
      外壳因此可以住进独立进程，只把命令推给 WMS。
   2. 单进程适配（VmShell_Create 及 VmShellLocal）：把 VmWm 当数据源，
      行为与拆进程前一致，供 demo_jpx / test_frame 使用。 */

#include <stddef.h>

#include "vm_display.h"
#include "vm_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 任务栏窗口 id。正常窗口 id 尽量避开这个值。 */
#define VM_SHELL_WINDOW_ID 0x7F00
#define VM_SHELL_DEFAULT_HEIGHT 36
#define VM_SHELL_MAX_ITEMS 64
#define VM_SHELL_TITLE_CAP 128

typedef struct VmShell VmShell;
typedef struct VmShellLocal VmShellLocal;
struct VmWm;

/* 一个任务条目。绘制与命中只认这个结构，不认 VmWm / IPC。 */
typedef struct {
    int  id;
    char title[VM_SHELL_TITLE_CAP];
    int  focused;
    int  minimized;
    int  maximized;
    int  undecorated;      /* 无装饰窗口（桌面层、弹出菜单）不占条目 */
    int  createdSeq;       /* 稳定排序：不随 raise 变化 */
} VmShellItem;

/* 指针为画布坐标；pointerValid=0 表示当前没有指针信息（无 hover）。 */
typedef struct {
    int count;
    VmShellItem items[VM_SHELL_MAX_ITEMS];
    int pointerX, pointerY, pointerValid, pointerDown;
    int downSeq;           /* 每次按下递增，用于按下沿判定 */
} VmShellModel;

typedef enum {
    VM_SHELL_ACTION_NONE = 0,
    VM_SHELL_ACTION_START,          /* 开始按钮 */
    VM_SHELL_ACTION_SETTINGS,       /* 任务栏空白右键：属性 */
    VM_SHELL_ACTION_TOGGLE_DESKTOP, /* 显示桌面：全部最小化 / 全部还原 */
    VM_SHELL_ACTION_ACTIVATE,       /* 激活条目窗口，步骤见 do* 字段 */
    VM_SHELL_ACTION_CONTEXT_MENU    /* 条目右键菜单 */
} VmShellActionKind;

typedef struct {
    VmShellActionKind kind;
    int winId;
    /* body 局部坐标（任务栏无装饰，等于画布坐标减任务栏原点）；
       菜单锚点用，调用方需要时自己加原点。 */
    int x, y;
    /* ACTIVATE 的具体步骤，已按窗口状态算好，调用方逐项执行。 */
    int doRestore, doUnmaximize, doMinimize, doFocus;
    /* TOGGLE_DESKTOP：1 = 全部最小化，0 = 全部还原 */
    int minimizeAll;
} VmShellAction;

/* ---------------- 纯配置对象（不持有窗口系统） ---------------- */

VmShell *VmShell_New(const VmTheme *theme, int width, int height);
void     VmShell_Delete(VmShell *shell);

void VmShell_SetTheme(VmShell *shell, const VmTheme *theme);
void VmShell_SetHeight(VmShell *shell, int height);
/* 运行时改变尺寸/选项后调用；单进程模式会作废任务栏窗口内容。 */
void VmShell_Refresh(VmShell *shell);

void VmShell_SetShowClock(VmShell *shell, int show);
int  VmShell_ShowClock(const VmShell *shell);
void VmShell_SetShowTrayIcon(VmShell *shell, int show);
int  VmShell_ShowTrayIcon(const VmShell *shell);
int  VmShell_Width(const VmShell *shell);
int  VmShell_Height(const VmShell *shell);

/* 产出任务栏内容命令；(x,y,w,h) 是任务栏窗口在画布上的矩形。 */
void VmShell_Paint(VmShell *shell, const VmShellModel *model, VmCmdBuffer *out,
                   int x, int y, int w, int h);

/* 命中：返回 1 并填 action；0 表示本位置没有动作。
   (lx,ly) 是任务栏 body 局部坐标；down=1 表示这是一次按下。
   按下沿在内部消费：同一次按下重复调用只返回一次。 */
int VmShell_HitTest(VmShell *shell, const VmShellModel *model, int lx, int ly,
                    int button, int down, VmShellAction *out);

/* ---------------- 单进程适配（VmWm 作数据源） ---------------- */

/* 开始按钮点击回调：由宿主决定打开/关闭开始菜单，shell 不解释应用语义。 */
typedef void (*VmShellStartHandler)(void *userData);
void VmShell_SetStartHandler(VmShell *shell, VmShellStartHandler handler, void *userData);

/* 任务栏右键菜单「属性」回调：宿主据此打开设置窗口（RetroBar 的
   Properties 对话框）。shell 只发通知，不解释设置语义。 */
typedef void (*VmShellSettingsHandler)(void *userData);
void VmShell_SetSettingsHandler(VmShell *shell, VmShellSettingsHandler handler,
                                void *userData);

/* 任务是「特殊窗口」：无装饰、置顶、不可拖动、不抢焦点。
   内容手绘（需要渐变/位图填充，Clay 只能单色），命中走 click handler。
   字体度量走 vm_font，调用前需先 VmFont_LoadFile。 */
VmShell *VmShell_Create(struct VmWm *wm, const VmTheme *theme, int height);
void VmShell_Destroy(VmShell *shell);

#ifdef __cplusplus
}
#endif

#endif
