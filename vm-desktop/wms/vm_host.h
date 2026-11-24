/*
 * vm_host.h - 本项目实现的 VmWmHost（合成宿主）
 *
 * wmwms 只负责窗口元数据与 IPC；合成、装饰、瓦片切分、资源包在这里：
 *   - 维护一份渲染用的 VmWm（几何/z/焦点从 wmwms 的事件同步过来）
 *   - on_content 把 IPC 推来的命令存进 VmWindowLayer.content
 *   - on_tick 跑 VmWm_BuildFrame → VmTileGrid_Build → VmWm_ServerBroadcastFrame
 *
 * 线程模型同 wmwms：所有回调在服务端唯一的 libuv loop 里同步执行。
 */
#ifndef VM_HOST_H
#define VM_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "wms_host.h"

typedef struct VmHost VmHost;

/* 创建宿主：建渲染副本、瓦片网格、默认主题资源包。
   width/height 是画布尺寸，tileW/tileH 是瓦片尺寸（通常 100×100）。 */
VmHost *VmHost_Create(int width, int height, int tileW, int tileH);

void VmHost_Destroy(VmHost *host);

/* 交给 VmWm_ServerSetHost 用；host 为 NULL 时返回 NULL */
const VmWmHost *VmHost_Callbacks(VmHost *host);

/* 访问渲染副本（诊断用；正常路径不需要） */
struct VmWm *VmHost_Wm(VmHost *host);

#ifdef __cplusplus
}
#endif
#endif /* VM_HOST_H */
