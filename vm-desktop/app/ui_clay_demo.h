#ifndef VM_APP_UI_CLAY_DEMO_H
#define VM_APP_UI_CLAY_DEMO_H

/* 新 app 的 Clay 后端示例（M7.3b）：布局与视觉来自 vm_ui_clay，
   焦点/编辑来自 vm_ui_core。 */

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

int  ClayUi_Init(void);
void ClayUi_Paint(VmCmdBuffer *out, int w, int h);
void ClayUi_Pointer(int x, int y, int down);
int  ClayUi_Key(const char *key, int ctrl, int shift);
void ClayUi_SetCaret(int on);

#ifdef __cplusplus
}
#endif

#endif
