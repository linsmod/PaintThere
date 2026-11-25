#ifndef VM_APP_UI_HELLO_H
#define VM_APP_UI_HELLO_H

/* A1 的最小界面：背景、一行标签、一个按钮、一个输入框 + caret。
   故意不引入 Clay / XAML —— 先验证协议、坐标与 epoch，
   UI 库在 M7 再接。坐标全部是 body 局部。 */

#include <stddef.h>

#include "vm_display.h"

typedef struct {
    int bodyW, bodyH;      /* 来自 EVT_LAYOUT_REQ */
    int pressed;           /* 按钮 toggle 状态 */
    int clicks;
    char text[256];
    size_t textLen;
    int focused;           /* 输入框是否聚焦 */
    int caretOn;
} HelloState;

void Hello_Paint(const HelloState *s, VmCmdBuffer *out);

/* 按钮矩形（body 局部）；命中判定与绘制共用一份几何。 */
void Hello_ButtonRect(const HelloState *s, int *x, int *y, int *w, int *h);
void Hello_InputRect(const HelloState *s, int *x, int *y, int *w, int *h);

#endif
