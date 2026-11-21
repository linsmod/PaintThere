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

#ifdef __cplusplus
}
#endif

#endif
