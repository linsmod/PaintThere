/*
 * vmwm_server.h - 窗口管理服务（wm_server）内部接口
 *
 * 本头文件仅供 wm_server 进程使用。窗口状态只存在于服务端内存，
 * 客户端（APP / SHELL / RDP）只能通过 IPC 间接操作，永远拿不到本进程指针。
 *
 * 本层不做的事：合成、瓦片、JSON 序列化、装饰绘制、资源包、HTTP。
 * 那些通过 VmWmHost 回调交给宿主（见 vmwm_host.h）。
 */
#ifndef VMWM_SERVER_H
#define VMWM_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "wms_ipc.h"
#include "wms_host.h"
#include "wms_peer.h"

/* ---------------- 服务生命周期 ---------------- */
/* 创建并监听命名管道；失败返回非 VMWM_OK */
VmWmResult VmWm_ServerInit(const char* pipe_path);

/* 进入事件循环，阻塞直到 VmWm_ServerStop（正常返回 0） */
int VmWm_ServerRun(void);

/* 停止事件循环 */
void VmWm_ServerStop(void);

/* ---------------- 宿主接入 ---------------- */
/* 注册宿主（合成层）。传 NULL 表示摘掉宿主，此时内容/帧相关能力返回
 * VMWM_ERR_NOT_READY，元数据能力不受影响。 */
void            VmWm_ServerSetHost(const VmWmHost* host);
const VmWmHost* VmWm_ServerHost(void);

/* ---------------- 帧下发（供宿主调用） ---------------- */
/* 广播给所有「有 FRAME 能力且订阅了对应事件」的连接 */
VmWmResult VmWm_ServerBroadcastFrame(const char* json, uint32_t len, int full);

/* 只发给指定客户端（fullframe 请求的应答） */
VmWmResult VmWm_ServerSendFrame(uint32_t client_id, const char* json,
                                uint32_t len, int full);

/* 只发给指定客户端的资源包 */
VmWmResult VmWm_ServerSendRespack(uint32_t client_id, const char* id,
                                  const char* json, uint32_t len);

/* 请求某个窗口重跑布局（宿主改了 body 尺寸时用；几何走 IPC 时服务端自己会发） */
VmWmResult VmWm_ServerRequestLayout(uint64_t win_id);

/* ---------------- 窗口内核操作 ---------------- */
/* owner = 客户端 id，0 表示无主窗口（任何有管理能力的客户端都可管理） */
VmWmResult VmWm_CreateWindow(const VmWmReqCreate* req, uint32_t owner, uint64_t* out_id);
VmWmResult VmWm_ServerRemoveWindow(uint64_t win_id, int32_t* out_prev_focus);
VmWmResult VmWm_MoveWindow(uint64_t win_id, int32_t x, int32_t y);
VmWmResult VmWm_ResizeWindow(uint64_t win_id, int32_t w, int32_t h);
VmWmResult VmWm_ConfigureWindow(uint64_t win_id, int32_t x, int32_t y, int32_t w, int32_t h);
VmWmResult VmWm_StackWindow(uint64_t win_id, uint32_t mode);
VmWmResult VmWm_SetFocus(uint64_t win_id);
VmWmResult VmWm_MapWindow(uint64_t win_id, int mapped);
VmWmResult VmWm_SetWindowTitle(uint64_t win_id, const char* title);
VmWmResult VmWm_GetWindowInfo(uint64_t win_id, VmWmWindowInfo* out);

/* 返回实际写入的条数 */
uint32_t   VmWm_ListWindows(VmWmWindowInfo* out, uint32_t max);

/* 当前焦点窗口 id，没有则 0 */
uint64_t   VmWm_GetFocus(void);

/* 当前工作区（默认等于屏幕尺寸；外壳可通过 CMD_SET_WORKAREA 声明） */
const VmWmReqWorkArea* VmWm_GetWorkArea(void);

/* 告诉服务端画布尺寸：工作区未声明前默认整屏，也是新连接 HANDHAKE 的尺寸来源 */
void VmWm_ServerSetScreenSize(int32_t w, int32_t h);

/* ---------------- 连接查询（调试 / 联调用） ---------------- */
uint32_t VmWm_ServerClientCount(void);

/* 取某个连接的进程身份；client_id 不存在返回 VMWM_ERR_NOT_CONNECTED */
VmWmResult VmWm_ServerGetPeer(uint32_t client_id, VmWmPeer* out);

#ifdef __cplusplus
}
#endif
#endif /* VMWM_SERVER_H */
