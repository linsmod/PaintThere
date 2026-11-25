#ifndef VM_APP_UI_XAML_H
#define VM_APP_UI_XAML_H

/* 新 app 的 XAML 表单后端（M7.4 / A6）。
   复用 jp_xaml 的 parser/state，执行走 Direct 后端（直接出 VmCmdBuffer），
   不依赖 JpxApp / Clay。 */

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* xaml 为 NULL 时用内置表单；返回 0 表示解析失败（details 写入 err）。 */
int  XamlUi_Init(const char *xamlPath, char *err, size_t errCap);

/* 加载预编译主题目录（xalm.bin 三件套），并作为 {StaticResource} 的来源。 */
int  XamlUi_LoadTheme(const char *themeDir, char *err, size_t errCap);

void XamlUi_Paint(VmCmdBuffer *out, int w, int h);
int  XamlUi_Pointer(int x, int y, int down);
int  XamlUi_Key(const char *key, int ctrl, int shift);
void XamlUi_SetCaret(int on);

#ifdef __cplusplus
}
#endif

#endif
