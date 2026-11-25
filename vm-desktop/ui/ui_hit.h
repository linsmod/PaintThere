#ifndef VM_UI_HIT_H
#define VM_UI_HIT_H

/* 控件命中：按绘制顺序（后画的在上）逆序查包含点且可交互的矩形。
   retain 与 imm 共用同一份规则；imm 每帧填矩形，retain 在树上查（M7.3）。 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int id;        /* 控件 id，由调用方定义（通常是节点下标） */
    int x, y, w, h;
    int enabled;   /* 0 = 不参与命中（禁用/装饰） */
} VmUiHitRect;

/* 返回命中的 id；-1 表示未命中。 */
int VmUiHit_Test(const VmUiHitRect *rects, int count, int x, int y);

#ifdef __cplusplus
}
#endif

#endif
