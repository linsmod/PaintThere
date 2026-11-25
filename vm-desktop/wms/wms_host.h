/*
 * vmwm_host.h - 宿主（合成层）接入点
 *
 * 这一层的定位：wmwms 只负责「窗口元数据 + 协议 + 连接生命周期」，不认识
 * 绘制命令、不做合成、不切瓦片、不生成资源包。那些都在宿主里：
 *
 *   独立联调 / e2e  → VmWm_MockHost()（内置模拟宿主，产出占位 JSON）
 *   并入 vm_wms     → 主项目把 VmWm_BuildFrame + VmDisplay_Diff +
 *                     VmTileGrid_Build 实现成这些回调
 *
 * 线程模型：所有回调都在服务端唯一的 libuv loop 线程上同步调用，
 * 回调内不得阻塞，也不得自己起线程。
 *
 * 内存约定：回调参数里的指针（内容命令、JSON）只在回调期间有效，
 * 需要保留必须自行拷贝。
 */
#ifndef VMWM_HOST_H
#define VMWM_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "wms_ipc.h"

typedef struct {
    int32_t x, y, w, h;
} VmWmRect;

typedef struct VmWmHost {
    void* user;

    /* ---- 窗口内容 ---- */
    /* 收到某窗口的最新内容命令（已通过归属/代次/配额校验）。
     * cmds 指向 count × cmd_size 字节，第一条命令在第 0 字节。
     * 宿主在此写入自己的 layer、标脏，下一次 on_tick 合成。 */
    void (*on_content)(void* user, uint64_t win_id, uint32_t epoch,
                       uint32_t count, uint32_t cmd_size,
                       const void* cmds, int overflow);

    /* ---- 窗口状态 ---- */
    /* 服务端权威表已更新；evt 为 EVT_* 位值。
     * 宿主用它维护渲染副本（矩形/z/flags/title），无需自己去查。 */
    void (*on_window_event)(void* user, uint32_t evt,
                            const VmWmWindowInfo* info);

    /* 窗口已被移除，宿主必须丢弃它的内容缓冲。 */
    void (*on_window_removed)(void* user, uint64_t win_id);

    /* ---- 几何与命中 ---- */
    /* body 区域（去掉装饰）在窗口内的矩形。返回 0 表示「整窗即 body」，
     * 服务端按 (0,0,w,h) 处理。有装饰的窗口由宿主返回真实偏移。 */
    int (*body_rect)(void* user, uint64_t win_id, int32_t w, int32_t h,
                     VmWmRect* out);

    /* 画布坐标下的命中测试，返回 win_id（0 = 未命中）。
     * 未注册时服务端用自带的 z 序近似实现。 */
    uint64_t (*hit_test)(void* user, int32_t x, int32_t y);

    /* 装饰按钮命中（画布坐标）：0=非按钮 1=关闭 2=最大化 3=最小化。
     * 服务端据此执行 remove / unmap；最大化无状态，仅消费点击。
     * 未注册时装饰按钮不生效（点击按普通输入转发）。 */
    int (*hit_test_decor)(void* user, uint64_t win_id, int32_t x, int32_t y);

    /* ---- 帧节拍 ---- */
    /* 每个 tick（默认 16 ms）调用一次，frame_id 单调递增。
     * 宿主在这里做：消费脏 → 合成 → 差分 → 切块 → VmWm_ServerBroadcastFrame()。 */
    void (*on_tick)(void* user, uint32_t frame_id);

    /* ---- 呈现端请求 ---- */
    /* RDP 请求全帧基线：宿主用 VmWm_ServerSendFrame(client_id, json, len, 1) 回。
     * 未注册时服务端 ACK VMWM_ERR_NOT_READY。 */
    void (*on_frame_request)(void* user, uint32_t client_id);

    /* RDP 请求资源包：宿主用 VmWm_ServerSendRespack(client_id, id, json, len) 回。 */
    void (*on_respack_request)(void* user, uint32_t client_id, const char* id);

    /* ---- 全局状态 ---- */
    /* 外壳声明了工作区（画布减去任务栏）。宿主据此钳制/恢复最大化窗口。 */
    void (*on_workarea)(void* user, const VmWmReqWorkArea* area);

    /* 外壳切了主题。宿主重新解析主题目录、重建资源包、通知各应用。 */
    void (*on_theme)(void* user, const char* name);
} VmWmHost;

/* 内置模拟宿主：不合成真实画面，只产出一个占位 JSON 证明链路通。
 * 用于独立联调与 e2e；接入真实合成层后不要注册它。 */
const VmWmHost* VmWm_MockHost(void);

#ifdef __cplusplus
}
#endif
#endif /* VMWM_HOST_H */
