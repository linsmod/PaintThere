#ifndef VM_UI_CLAY_H
#define VM_UI_CLAY_H

/* Clay 后端的控件层（M7.3b）：布局交给 Clay，状态与焦点来自 vm_ui_core。
   与 jpx_widget 的区别：没有 JpxApp 单例、不认识 VmWm、只产出 VmCmdBuffer。 */

#include <stddef.h>

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmUiClay VmUiClay;

VmUiClay *VmUiClay_Create(void);
void      VmUiClay_Destroy(VmUiClay *ui);

void VmUiClay_Begin(VmUiClay *ui, int w, int h);
void VmUiClay_Label(VmUiClay *ui, const char *text);
int  VmUiClay_Button(VmUiClay *ui, int id, const char *text);
int  VmUiClay_CheckBox(VmUiClay *ui, int id, const char *label, int *checked);
int  VmUiClay_TextInput(VmUiClay *ui, int id, char *buf, size_t cap, int width);
void VmUiClay_End(VmUiClay *ui, VmCmdBuffer *out);

/* 输入：坐标是 body 局部。down=1 按下，0 抬起。 */
void VmUiClay_Pointer(VmUiClay *ui, int x, int y, int down);
/* 返回 1 表示按键被消费（Tab 在控件间移动焦点）。 */
int  VmUiClay_Key(VmUiClay *ui, const char *key, int ctrl, int shift);
void VmUiClay_SetCaret(VmUiClay *ui, int on);

#ifdef __cplusplus
}
#endif

#endif
