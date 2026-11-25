#ifndef VM_UI_INPUT_H
#define VM_UI_INPUT_H

/* 与 wms 协议无关的 UI 事件：宿主把 VmWmReqInput 转成它再交给控件层。
   坐标是 body 局部；x/y 对键盘事件无意义。 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VM_UI_EV_MOVE = 0,
    VM_UI_EV_DOWN,
    VM_UI_EV_UP,
    VM_UI_EV_WHEEL,
    VM_UI_EV_KEY
} VmUiEventKind;

typedef struct {
    VmUiEventKind kind;
    int x, y;
    int button;
    int wheel;        /* 滚轮：正负行数 */
    char key[32];     /* 键盘：可打印字符或 Backspace/ArrowLeft/Tab/... */
    int ctrl, shift, alt;
} VmUiEvent;

#ifdef __cplusplus
}
#endif

#endif
