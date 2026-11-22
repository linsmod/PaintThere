#ifndef VM_SERIALIZE_JSON_H
#define VM_SERIALIZE_JSON_H

#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Serialize a display list into a {type:"frame",frameId,commands:[...]} JSON string.
   Returns a heap-allocated string the caller must free. */
char *VmSerialize_FrameJson(const VmDisplayList *list);

/* Serialize a command list (without frame envelope) for the 'resources' SSE event. */
char *VmSerialize_CommandArrayJson(const VmDisplayList *list);

/* 增量帧：完整命令列表 + 服务端算好的唯一重绘矩形（dirty 为空表示不用画）。
   list 提供 commands 与 frameId，delta 提供重绘区域。 */
char *VmSerialize_DeltaFrameJson(const VmDisplayList *list, const VmDisplayDelta *delta);

/* ---- 分层合成指令流 ----
   前端只按顺序执行这些 op，不再感知脏区、不存基线、不做 diff。 */
typedef struct VmOps VmOps;

VmOps *VmOps_Create(void);
void VmOps_Destroy(VmOps *ops);
/* 创建离屏层；前端已有同 id 层时忽略。 */
void VmOps_CreateLayer(VmOps *ops, const char *id, int w, int h);
/* 后续绘制命令定向到该层。 */
void VmOps_Bind(VmOps *ops, const char *id);
/* 清除当前绑定层的矩形。 */
void VmOps_ClearLayer(VmOps *ops, VmRect r);
/* 追加绘制命令（命令本身复用 emit_command 的格式）。 */
void VmOps_Draw(VmOps *ops, const VmDrawCommand *cmds, int count);
/* 清除主画布矩形。 */
void VmOps_ClearScreen(VmOps *ops, VmRect r);
/* 把层的一段区域贴到主画布。 */
void VmOps_Composite(VmOps *ops, const char *src, VmRect srcRect, int dx, int dy);
/* 返回 {"type":"ops","ops":[...]}，调用方 free。 */
char *VmOps_Finish(VmOps *ops);

#ifdef __cplusplus
}
#endif

#endif
