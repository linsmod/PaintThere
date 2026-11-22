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
    int titleFontId;
    int titleFontSize;
} VmChrome;

/* 全量主题 token：WM 装饰 + 桌面背景 + 面板 + 控件。 */
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
