#ifndef VM_MENU_VIEW_H
#define VM_MENU_VIEW_H

/* 弹出菜单的纯绘制/命中层（M7.2）。
   与 vm_menu.c 的区别：不认识 VmWm、不建窗口、不管 popup 生命周期；
   窗口由调用方（vm_shell）建，坐标由调用方给。 */

#include "vm_display.h"
#include "vm_menu.h"
#include "vm_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_MENU_VIEW_MAX_ITEMS 32

typedef struct {
    VmMenuItem items[VM_MENU_VIEW_MAX_ITEMS];
    int count;
    const VmTheme *theme;
    /* 画布坐标下的最终矩形（已钳进画布）。 */
    int x, y, w, h;
    int itemY[VM_MENU_VIEW_MAX_ITEMS];
    int itemH[VM_MENU_VIEW_MAX_ITEMS];
    int hover;   /* 画布坐标下的悬停条目，-1 = 无 */
} VmMenuView;

/* 量宽排高 + 越界钳制。anchor 是偏好左上角（通常是按下点）。 */
void VmMenuView_Layout(VmMenuView *v, const VmTheme *theme,
                       const VmMenuItem *items, int count,
                       int anchorX, int anchorY, int canvasW, int canvasH);

/* 产出 body 局部命令（0,0 起，尺寸 v->w/v->h）。 */
void VmMenuView_Paint(const VmMenuView *v, VmCmdBuffer *out);

/* 画布坐标命中；返回条目下标，-1 表示不在菜单内或落在分隔/禁用项上。 */
int VmMenuView_Hit(const VmMenuView *v, int cx, int cy);

/* 更新悬停条目（画布坐标）；返回 1 表示悬停变化，需要重推内容。 */
int VmMenuView_SetHover(VmMenuView *v, int cx, int cy);

#ifdef __cplusplus
}
#endif

#endif
