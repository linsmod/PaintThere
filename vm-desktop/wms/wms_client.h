/*
 * vmwm_client.h - 客户端 SDK（APP / SHELL / RDP 共用）· v2 实例化
 *
 * v1 的 SDK 全是 static 全局（g_loop / g_pipe / g_seq / g_cb），一个进程只能连
 * 一个服务。rdp 需要独立连接、也有的进程要同时扮演 app 与 rdp，所以改成
 * 句柄式：WmsClient* 由 Wms_ClientCreate 创建，各自持有独立的 loop、管道、
 * seq 与回调。
 *
 * 同步模型不变：每个 Wms_Xxx 调用 = 打包请求 -> uv_write -> 等对应 seq 的 ACK。
 * 等待期间照常跑自己的 loop，因此服务端推送的事件也会被分发到用户回调。
 */
#ifndef VMWM_CLIENT_H
#define VMWM_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "wms_ipc.h"
#include "wms_buf.h"

typedef struct WmsClient WmsClient;

/* 一条事件。payload 指向原始载荷，**仅在回调期间有效**；
 * 用下面的 Wms_AsXxx 解码，需要保留请自己拷贝。 */
typedef struct {
    uint32_t       evt;       /* VmWmEvent 位值 */
    const void*    payload;
    uint32_t       len;
} WmsEvent;

typedef void (*WmsEventCb)(WmsClient* c, const WmsEvent* e, void* user);

/* ---------------- 生命周期 ---------------- */
/* 只分配结构，不连接；pipe_path 为 NULL 用默认 */
WmsClient* Wms_ClientCreate(const char* pipe_path, VmWmClientType type);

/* 建立连接并握手。失败返回非 VMWM_OK，客户端仍可 Destroy。 */
VmWmResult Wms_ClientConnect(WmsClient* c);

/* 断开并释放 */
void       Wms_ClientDestroy(WmsClient* c);

int        Wms_ClientIsConnected(const WmsClient* c);

/* 服务端在握手中分配的连接 id */
uint32_t   Wms_ClientId(const WmsClient* c);

VmWmClientType Wms_ClientType(const WmsClient* c);

/* 握手中拿到的桌面事实 */
int32_t    Wms_ScreenWidth(const WmsClient* c);
int32_t    Wms_ScreenHeight(const WmsClient* c);
void       Wms_WorkArea(const WmsClient* c, int32_t* x, int32_t* y,
                        int32_t* w, int32_t* h);
uint32_t   Wms_LayoutTimeoutFrames(const WmsClient* c);

/* ---------------- 事件 ---------------- */
void       Wms_SetEventCallback(WmsClient* c, WmsEventCb cb, void* user);

/* 订阅事件掩码（VmWmEvent 按位或） */
VmWmResult Wms_Subscribe(WmsClient* c, uint32_t mask);

/* 处理到达的事件，最多等待 timeout_ms（0 = 只处理已到达的）。
 * 返回本次分发的事件条数。 */
int        Wms_PollEvents(WmsClient* c, int timeout_ms);

/* ---------------- 窗口操作（APP / SHELL） ----------------
 * flags 取 VmWmWinFlag 位或（与本项目 VM_WIN_* 同值）：
 * UNDECORATED / TOPMOST / PINNED / NOFOCUS。普通应用传 0。 */
VmWmResult Wms_CreateWindow(WmsClient* c, int x, int y, int w, int h,
                            uint32_t flags, uint64_t* out_win_id);
VmWmResult Wms_CloseWindow(WmsClient* c, uint64_t win_id);
VmWmResult Wms_MoveWindow(WmsClient* c, uint64_t win_id, int x, int y);
VmWmResult Wms_ResizeWindow(WmsClient* c, uint64_t win_id, int w, int h);
VmWmResult Wms_ConfigureWindow(WmsClient* c, uint64_t win_id,
                               int x, int y, int w, int h);
VmWmResult Wms_StackWindow(WmsClient* c, uint64_t win_id, VmWmStackMode mode);
VmWmResult Wms_SetFocus(WmsClient* c, uint64_t win_id);
VmWmResult Wms_MapWindow(WmsClient* c, uint64_t win_id);
VmWmResult Wms_UnmapWindow(WmsClient* c, uint64_t win_id);

/* ---------------- 窗口属性 ---------------- */
VmWmResult Wms_SetTitle(WmsClient* c, uint64_t win_id, const char* title);
VmWmResult Wms_SetSizeHint(WmsClient* c, uint64_t win_id,
                           int min_w, int min_h, int max_w, int max_h);
VmWmResult Wms_SetTransientFor(WmsClient* c, uint64_t win_id,
                               uint64_t parent_win_id);
VmWmResult Wms_SetClassHint(WmsClient* c, uint64_t win_id, const char* cls);

/* ---------------- 查询 ---------------- */
VmWmResult Wms_GetGeometry(WmsClient* c, uint64_t win_id,
                           int* out_x, int* out_y, int* out_w, int* out_h);

/* 拉取服务端当前全部窗口快照（已按 z 从高到低）。
 * 返回写入 out 的条数（>=0），失败返回 -1。 */
int        Wms_ListWindows(WmsClient* c, VmWmWindowInfo* out, uint32_t max);

/* ---------------- 内容通道（APP / SHELL） ----------------
 * cmds 是连续 count 条、每条 cmd_size 字节的命令数组，坐标为窗口局部坐标。
 * 返回 accepted=1 表示被采纳；accepted=0 表示 epoch 过期被丢弃。 */
VmWmResult Wms_PushContent(WmsClient* c, uint64_t win_id, uint32_t epoch,
                           const void* cmds, uint32_t count, uint32_t cmd_size,
                           int* out_accepted, int* out_overflow);

/* ---------------- 输入（SHELL / RDP） ----------------
 * MSG 类型，不等 ACK。win_id 为 0 时 x/y 是画布坐标，由服务端命中并转局部。 */
VmWmResult Wms_SendInput(WmsClient* c, const VmWmReqInput* in);

/* ---------------- 全局状态（SHELL） ---------------- */
VmWmResult Wms_SetWorkArea(WmsClient* c, int x, int y, int w, int h);
VmWmResult Wms_SetTheme(WmsClient* c, const char* name);

/* ---------------- 呈现端（RDP） ----------------
 * 请求后 ACK 立即返回，数据以 EVT_FULLFRAME / EVT_RESOURCES 事件异步到达。 */
VmWmResult Wms_RequestFullframe(WmsClient* c);
VmWmResult Wms_RequestRespack(WmsClient* c, const char* id);

/* ---------------- 事件载荷解码 ---------------- */
const VmWmWindowInfo*  Wms_AsWindowInfo(const WmsEvent* e);
const VmWmEvtLayoutReq* Wms_AsLayoutReq(const WmsEvent* e);
const VmWmReqInput*    Wms_AsInput(const WmsEvent* e);
const VmWmReqTheme*    Wms_AsTheme(const WmsEvent* e);
const VmWmEvtPointer*  Wms_AsPointer(const WmsEvent* e);

/* 帧：返回 JSON 起点，长度/帧号写到出参；非帧事件返回 NULL */
const char* Wms_AsFrame(const WmsEvent* e, uint32_t* out_len,
                        uint32_t* out_frame_id);

/* 资源包：返回 JSON 起点，id/长度写到出参 */
const char* Wms_AsRespack(const WmsEvent* e, char* out_id, uint32_t idcap,
                          uint32_t* out_len);

const char* Wms_StrError(VmWmResult r);

#ifdef __cplusplus
}
#endif
#endif /* VMWM_CLIENT_H */
