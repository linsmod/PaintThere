#ifndef VM_TILES_H
#define VM_TILES_H

#include "vm_display.h"
#include "vm_serialize_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_TILE_MAX 128

/* 远端瓦片网格的 guest 侧状态：只保存每块的命令签名，不保存整份显示列表。 */
typedef struct {
    int cols, rows, tileW, tileH, w, h;
    unsigned long long hash[VM_TILE_MAX];
    int hasPrev;
} VmTileGrid;

void VmTileGrid_Init(VmTileGrid *grid, int cols, int rows, int tileW, int tileH, int w, int h);

/* 把全局坐标完整帧切成 tile 局部坐标的小命令列表。
   full=1 时下发 config 与所有块，且不修改基线；返回 0 表示无变化、out 未使用。 */
int VmTileGrid_Build(VmTileGrid *grid, const VmDisplayList *frame, int full, VmTiles *out);

#ifdef __cplusplus
}
#endif

#endif
