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

/* 任务是「特殊窗口」：无装饰、置顶、不可拖动、不抢焦点。
   这样 z 序、命中、Clay 指针、键盘路径全部复用 WM 现成实现。
   字体由 wm 使用的主题决定，度量走 vm_font，因此调用前需先 VmFont_LoadFile。 */
VmShell *VmShell_Create(VmWm *wm, const VmTheme *theme, int height);
void VmShell_Destroy(VmShell *shell);

/* 窗口打开/关闭/标题变化后调用；布局每帧重读窗口列表，通常不需要。 */
void VmShell_Refresh(VmShell *shell);

#ifdef __cplusplus
}
#endif

#endif
