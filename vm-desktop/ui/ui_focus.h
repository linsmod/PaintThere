#ifndef VM_UI_FOCUS_H
#define VM_UI_FOCUS_H

/* 控件焦点表：Tab 顺序、当前焦点、按下沿。
   纯状态机，不依赖布局引擎或窗口系统（M7.3，retain / imm 共用）。 */

#ifdef __cplusplus
extern "C" {
#endif

#define VM_UI_MAX_FOCUS 64

typedef struct {
    int ids[VM_UI_MAX_FOCUS];
    int count;
    int current;   /* 当前聚焦的控件 id；-1 表示无 */
} VmUiFocus;

void VmUiFocus_Reset(VmUiFocus *f);
/* 按 Tab 顺序追加；重复 id 不重复登记。返回 1 表示新登记。 */
int  VmUiFocus_Add(VmUiFocus *f, int id);
void VmUiFocus_Remove(VmUiFocus *f, int id);
void VmUiFocus_Set(VmUiFocus *f, int id);
int  VmUiFocus_Get(const VmUiFocus *f);
int  VmUiFocus_Contains(const VmUiFocus *f, int id);

/* 循环移动焦点：dir > 0 向后，dir < 0 向前。返回新的焦点 id；空表返回 -1。 */
int  VmUiFocus_Cycle(VmUiFocus *f, int dir);

#ifdef __cplusplus
}
#endif

#endif
