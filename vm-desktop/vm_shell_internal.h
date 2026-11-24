#ifndef VM_SHELL_INTERNAL_H
#define VM_SHELL_INTERNAL_H

/* 外壳内部结构：只给 vm_shell.c（纯绘制）与 vm_shell_local.c（单进程适配）用。
   公开头 vm_shell.h 不暴露窗口系统类型，外壳进程才能不链 vm_wm / Clay。 */

#include "vm_shell.h"

struct VmWm;

struct VmShell {
    const VmTheme *theme;
    int width, height;
    int showClock;
    int showTrayIcon;
    /* 单调递增的按下序号：一次按下只消费一次，与重建帧数无关。
       左右键分开记：右键菜单打开/关闭不影响左键的按下沿。 */
    int lastDownSeq;
    int lastRightSeq;
    /* 单进程模式才有；纯绘制不读它。 */
    struct VmShellLocal *local;
};

struct VmShellLocal {
    struct VmWm *wm;
    VmShell *shell;
    VmShellStartHandler startHandler;
    void *startUserData;
    VmShellSettingsHandler settingsHandler;
    void *settingsUserData;
};

#endif
