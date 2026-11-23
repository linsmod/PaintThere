#ifndef VM_MENU_H
#define VM_MENU_H

#include "vm_theme.h"
#include "vm_wm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* VM 级通用 popup 菜单控件：右键上下文菜单、任务栏属性菜单的公共实现。
   任务栏（vm_shell）是第一个使用者；开始菜单 popup 化等后续界面复用它。

   范式与任务栏一致：一个 UNDECORATED|TOPMOST|PINNED|NOFOCUS 的特殊窗口，
   手绘 paint + 自己的命中逻辑。同一时刻只允许一个菜单（单例，与 Windows
   行为一致）；再开会先关旧的。菜单开着时，落在它之外的 mousedown 由 WM
   的 popup dismiss 基础设施消费掉（只关菜单，不穿透）。 */

#define VM_MENU_WINDOW_ID 0x7F01   /* 避开 0x7F00（任务栏）与正常窗口 id */

typedef struct {
    const char *text;   /* NULL = 分隔线 */
    int enabled;        /* 0 = 灰字不可点 */
} VmMenuItem;

/* 在 (x, y) 打开菜单（左上角，画布坐标；越界时钳制进画布）。
   items 会被拷贝进菜单状态，text 指针借用调用方（通常为静态字面量），
   回调触发前必须保持有效。返回菜单窗口 id；失败返回 0（参数错/加窗失败）。 */
int VmMenu_Open(VmWm *wm, const VmTheme *theme, int x, int y,
                const VmMenuItem *items, int count,
                void (*onSelect)(int index, void *userData), void *userData);

/* 关闭当前菜单（未打开时无操作）。选中回调内部也可安全调用。 */
void VmMenu_Close(void);

int VmMenu_IsOpen(void);

#ifdef __cplusplus
}
#endif

#endif
