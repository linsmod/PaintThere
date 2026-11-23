#ifndef VM_THEME_H
#define VM_THEME_H

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 窗口装饰参数。WM 只用它画标题栏与边框，不认识控件。 */
typedef struct {
    int titleHeight;              /* 默认 28 */
    int borderWidth;              /* 默认 2 */
    const char *windowStyleId;    /* 窗口体背景样式 id，默认 "s_window" */
    const char *titleFocusColor;
    const char *titleIdleColor;
    const char *borderFocusColor;
    const char *borderIdleColor;
    const char *titleTextColor;   /* 空则用 WM 默认（聚焦白、失焦灰蓝） */
    int titleFontId;
    int titleFontSize;
    /* 现代主题窗口外发光；XP 没有阴影，按主题关闭。 */
    int showShadow;
} VmChrome;

/* 全量主题 token：WM 装饰 + 桌面背景 + 面板 + 控件。
   带 From/To 的成对字段表示「渐变皮肤」：非 NULL 时 VmTheme_AppendResources
   生成同名 pattern 与 s_* style，WM/shell 直接用 style id 当 paint。
   为 NULL 时沿用单色字段，现代深色主题不受影响。 */
typedef struct {
    VmChrome chrome;
    const char *desktopFrom;
    const char *desktopTo;
    const char *panelStyleId;
    const char *panelColor;
    const char *bgColor;
    const char *fgColor;
    const char *borderColor;
    const char *accentColor;
    const char *accentTextColor;
    const char *inputBgColor;
    const char *disabledColor;
    const char *selectedColor;
    /* XP 风格窗口标题栏（chrome.titleFocusColor/IdleColor 置为 s_title_*） */
    const char *titleActiveFrom, *titleActiveTo;
    const char *titleIdleFrom, *titleIdleTo;
    /* 开始按钮：纯色 accent 换成绿渐变 */
    const char *startFrom, *startTo;
    const char *startTextColor;
    /* 任务栏条目：普通态与聚焦/按下态 */
    const char *taskButtonFrom, *taskButtonTo;
    const char *taskButtonActiveFrom, *taskButtonActiveTo;
    const char *taskButtonTextColor;
    /* 右侧托盘区与时钟 */
    const char *trayFrom, *trayTo;
    const char *clockTextColor;
    int radius;
    int padding;
    int gap;
    int fontId;
    int fontSize;
} VmTheme;

const VmTheme *VmTheme_Default(void);

/* 唯一 resources 生成者：definePattern / defineStyle。WM 与 JPX 共用一份。 */
void VmTheme_AppendResources(VmDisplayList *out, const VmTheme *theme);

#ifdef __cplusplus
}
#endif

#endif
