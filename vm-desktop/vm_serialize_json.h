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

/* ---- Remote tiles 指令流 ----
   前端只管理远端小画布网格：config / setTile + showLoading / hideLoading。
   窗口、图层、z 序与脏区都不进入协议。 */
typedef struct VmTiles VmTiles;

VmTiles *VmTiles_Create(void);
void VmTiles_Destroy(VmTiles *tiles);
void VmTiles_SetFrameId(VmTiles *tiles, uint32_t frameId);
/* 网格定义，全量帧下发，前端幂等处理。 */
void VmTiles_Config(VmTiles *tiles, int cols, int rows, int tileW, int tileH, int w, int h);
/* 批次边界：showLoading 后暂停呈现，hideLoading 时一次性贴齐。 */
void VmTiles_ShowLoading(VmTiles *tiles);
void VmTiles_HideLoading(VmTiles *tiles);
/* 更新一块 tile：index = row*cols+col，cmds 为 tile 局部坐标绘制原语。 */
void VmTiles_SetTile(VmTiles *tiles, int index, const VmDrawCommand *cmds, int count);
/* Use pre-serialized command JSON and the matching typed commands (for binary mirror). */
void VmTiles_SetTileJson(VmTiles *tiles, int index, const char *cmdsJson,
                         const VmDrawCommand *cmds, int count);
/* 返回 {"type":"tiles","frameId":N,"ops":[...]}，调用方 free。 */
char *VmTiles_Finish(VmTiles *tiles);
/* Finish binary form instead of JSON. Caller frees returned bytes. */
uint8_t *VmTiles_FinishBin(VmTiles *tiles, size_t *outLen);

#ifdef __cplusplus
}
#endif

#endif
