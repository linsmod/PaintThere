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
    /* 标题文字伪粗体（stb 不做合成粗体，WM 同色偏移 1px 再画一次）。 */
    int boldTitle;
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
    /* 开始按钮：纯色 accent 换成绿渐变；有位图时优先用位图（XP 的三态图）。 */
    const char *startFrom, *startTo;
    const char *startTextColor;
    const char *startImageId;   /* 开始按钮底图（竖排三态，取上 1/3） */
    const char *startIconId;    /* 开始按钮左侧的徽标 */
    /* 任务栏条目：普通态、悬停态与聚焦/按下态 */
    const char *taskButtonFrom, *taskButtonTo;
    const char *taskButtonHoverFrom, *taskButtonHoverTo;
    const char *taskButtonActiveFrom, *taskButtonActiveTo;
    const char *taskButtonTextColor;
    const char *taskBorderTopColor, *taskBorderBottomColor;
    /* 右侧托盘区与时钟 */
    const char *trayFrom, *trayTo;
    const char *trayIconId;      /* 托盘内的「显示桌面」图标 */
    const char *clockTextColor;
    int radius;
    int padding;
    int gap;
    int fontId;
    int fontSize;
    int panelHeight;   /* 任务栏高度；0 表示用 VM_SHELL_DEFAULT_HEIGHT */
    /* 任务栏几何与文字样式：由主题决定，shell 不写死像素。
       0 表示让 shell 用最小兜底值（仅用于没有皮肤的默认主题）。 */
    int panelPad;             /* 左内边距 */
    int panelStartWidth;      /* 开始按钮宽 */
    int panelButtonHeight;    /* 开始按钮与任务条目高 */
    int panelItemWidth;       /* 任务条目宽 */
    int panelItemGap;         /* 条目间距 */
    int panelTrayWidth;       /* 托盘宽 */
    int panelRadius;          /* 任务栏按钮圆角 */
    int panelStartIconSize;   /* 开始按钮徽标边长 */
    int boldChromeText;       /* 标题栏与「开始」是否伪粗体（主题的 FontWeight） */
} VmTheme;

const VmTheme *VmTheme_Default(void);

/* 唯一 resources 生成者：definePattern / defineStyle。WM 与 JPX 共用一份。 */
void VmTheme_AppendResources(VmDisplayList *out, const VmTheme *theme);

#ifdef __cplusplus
}
#endif

#endif
