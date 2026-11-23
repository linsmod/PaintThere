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
    const char *startLabelShadowColor;  /* StartLabelShadow 效果的颜色（XP 阴影字） */
    const char *startImageId;   /* 开始按钮底图（竖排三态，取上 1/3） */
    const char *startIconId;    /* 开始按钮左侧的徽标 */
    /* 任务栏条目：普通态、悬停态与聚焦/按下态 */
    const char *taskButtonFrom, *taskButtonTo;
    const char *taskButtonHoverFrom, *taskButtonHoverTo;
    const char *taskButtonActiveFrom, *taskButtonActiveTo;
    const char *taskButtonTextColor;
    /* 条目 bevel：未聚焦凸起（上/左浅、下/右深），聚焦按下反转。 */
    const char *taskBorderTopColor, *taskBorderBottomColor;
    const char *taskBorderLeftColor, *taskBorderRightColor;
    /* 右侧托盘区与时钟 */
    const char *trayFrom, *trayTo;
    const char *trayIconId;      /* 「显示桌面」按钮图标（RetroBar 的
                                    ShowDesktopIconImageSmall，在托盘之外） */
    const char *showDesktopFrom, *showDesktopTo;  /* 显示桌面按钮底纹 */
    const char *clockTextColor;
    int radius;
    int padding;
    int gap;
    int fontId;
    int fontSize;
    int panelHeight;   /* 任务栏高度；0 表示用 VM_SHELL_DEFAULT_HEIGHT */
    /* 任务栏几何与文字样式：由主题决定，shell 不写死像素。
       0 表示让 shell 用最小兜底值（仅用于没有皮肤的默认主题）。
       来源 key 见 jpx_xaml_theme.c 的 DeriveTheme；兜底值 = System.xaml
       （RetroBar 全主题共享基座）与 XP 惯例。 */
    int panelPad;                 /* 栏左内边距（开始按钮前的空隙） */
    int panelStartWidth;          /* 开始按钮宽（StartButton.MinWidth） */
    int panelButtonHeight;        /* 开始按钮与任务条目高（StartButton.Height） */
    int panelStartMarginRight;    /* StartButton.Margin.Right（与条目区隔 1px） */
    int panelStartIconW;          /* StartIcon.Width（XP 是 25×20 非正方形） */
    int panelStartIconH;          /* StartIcon.Height */
    int panelStartIconMarginLeft; /* StartIcon.Margin.Left */
    int panelStartLabelFontSize;  /* StartLabel.FontSize（XP 19px，0 = 通用字号） */
    int panelStartLabelMarginRight; /* StartLabel.Margin.Right（XP 26） */
    int panelStartLabelShadow;    /* StartLabelShadow 资源存在时置 1 */
    int panelItemWidthDefault;    /* TaskButtonWidth（条目默认宽 160） */
    int panelItemWidthMin;        /* TaskButtonMinWidth（收缩下限 45） */
    int panelItemMarginRight;     /* TaskButtonMargin.Right（条目间距 3） */
    int panelTaskListMarginLeft;  /* TaskListScrollViewer.Margin.Left（4） */
    int panelTaskTextInsetX;      /* TaskButton 内容左缩进（模板 Content.Margin.Left） */
    int panelTrayMarginLeft;      /* Tray.Margin.Left（托盘与条目区之间 10） */
    int clockMarginLeft;          /* Clock.Margin（8,1,11,6） */
    int clockMarginTop;
    int clockMarginRight;
    int clockMarginBottom;
    int panelShowDesktopIconW;    /* ShowDesktopIcon.Width */
    int panelShowDesktopIconH;    /* ShowDesktopIcon.Height */
    int panelShowDesktopMarginL;  /* ShowDesktopButton.Content.Margin.Left */
    int panelShowDesktopMarginR;  /* ShowDesktopButton.Content.Margin.Right */
    int panelRadius;              /* 任务栏按钮圆角（模板 CornerRadius） */
    int boldChromeText;       /* 标题栏与「开始」是否伪粗体（主题的 FontWeight） */
} VmTheme;

const VmTheme *VmTheme_Default(void);

/* 唯一 resources 生成者：definePattern / defineStyle。WM 与 JPX 共用一份。 */
void VmTheme_AppendResources(VmDisplayList *out, const VmTheme *theme);

#ifdef __cplusplus
}
#endif

#endif
