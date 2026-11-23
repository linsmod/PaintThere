#ifndef VM_SHELL_H
#define VM_SHELL_H

#include "vm_theme.h"
#include "vm_wm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 任务栏窗口 id。正常窗口 id 尽量避开这个值。 */
#define VM_SHELL_WINDOW_ID 0x7F00
#define VM_SHELL_DEFAULT_HEIGHT 36

typedef struct VmShell VmShell;

/* 开始按钮点击回调：由宿主决定打开/关闭开始菜单，shell 不解释应用语义。 */
typedef void (*VmShellStartHandler)(void *userData);
void VmShell_SetStartHandler(VmShell *shell, VmShellStartHandler handler, void *userData);

/* 任务栏右键菜单「属性」回调：宿主据此打开设置窗口（RetroBar 的
   Properties 对话框）。shell 只发通知，不解释设置语义。 */
typedef void (*VmShellSettingsHandler)(void *userData);
void VmShell_SetSettingsHandler(VmShell *shell, VmShellSettingsHandler handler,
                                void *userData);

/* 任务栏选项（RetroBar 的 Properties 开关子集）。默认全开。 */
void VmShell_SetShowClock(VmShell *shell, int show);
int  VmShell_ShowClock(const VmShell *shell);
void VmShell_SetShowTrayIcon(VmShell *shell, int show);
int  VmShell_ShowTrayIcon(const VmShell *shell);

/* 任务是「特殊窗口」：无装饰、置顶、不可拖动、不抢焦点。
   内容用 VmWm_SetWindowPaint 手绘（需要渐变/位图填充，Clay 只能单色），
   命中用 VmWm_SetWindowClickHandler。字体度量走 vm_font，调用前需先 VmFont_LoadFile。 */
VmShell *VmShell_Create(VmWm *wm, const VmTheme *theme, int height);
void VmShell_Destroy(VmShell *shell);

/* 运行时换肤：shell 只持有指针，调用方需保证 theme 生命周期长于 shell。
   换色后需 VmShell_Refresh + 对应窗口置脏才会体现在下一帧。 */
void VmShell_SetTheme(VmShell *shell, const VmTheme *theme);
/* 运行时改高度（主题的 TaskbarHeight）：同步窗口矩形与工作区。 */
void VmShell_SetHeight(VmShell *shell, int height);

/* 窗口打开/关闭/标题变化后调用；布局每帧重读窗口列表，通常不需要。 */
void VmShell_Refresh(VmShell *shell);

#ifdef __cplusplus
}
#endif

#endif
