/*
 * vmwm_peer.h - 「这条连接背后是哪个进程」的平台抽象
 *
 * 服务端不信任客户端自报的 pid（HELLO 里没有 pid 字段，将来也不加）：
 * 身份必须由内核提供，即从自己 accept 到的那条连接上反向索取。
 *
 *   Windows：管道句柄 → GetNamedPipeClientProcessId → OpenProcess 拿进程句柄
 *   POSIX  ：AF_UNIX socket → SO_PEERPIDFD（内核 >= 6.5）/ SO_PEERCRED
 *   其它    ：返回未验证的空身份（mock），服务端照常工作，只是没有进程关联
 *
 * 语义与 Windows 句柄对齐的关键点：
 *   - handle 持有的是「进程实例」，pid 复用不影响判断，所以退出检测安全；
 *   - verified == 0 时 pid/uid 全为 0，调用方不得据此做任何动作。
 */
#ifndef VMWM_PEER_H
#define VMWM_PEER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum {
    VMWM_PEER_HANDLE_NONE   = 0,
    VMWM_PEER_HANDLE_PROCESS = 1,   /* Win32 进程句柄 */
    VMWM_PEER_HANDLE_PIDFD   = 2    /* Linux pidfd */
} VmWmPeerHandleKind;

typedef struct {
    uint32_t           pid;         /* 0 = 未知 */
    uint32_t           uid;         /* 0 = 未知（Windows 无对应概念） */
    int                verified;    /* 1 = 内核提供，可信任 */
    int                handle_kind; /* VmWmPeerHandleKind */
    intptr_t           handle;      /* 句柄或 fd；0 表示不可用 */
} VmWmPeer;

/* 从服务端已 accept 的流取对端身份。
 * fd：Windows 为 HANDLE（uv_fileno 的结果），POSIX 为 socket fd。
 * 失败不报错，返回 verified = 0 的空身份。 */
VmWmPeer VmWm_PeerFromFd(intptr_t fd);

/* 释放句柄。幂等，可对未验证的身份调用。 */
void     VmWm_PeerRelease(VmWmPeer* p);

/* 进程是否还活着：1 活着 / 0 已退出 / -1 无法判断（未验证）。
 * 只用句柄/pidfd，不做 kill(pid,0) 那种有竞态的探活。 */
int      VmWm_PeerAlive(const VmWmPeer* p);

/* 打日志用的单行描述，写入 buf */
const char* VmWm_PeerDescribe(const VmWmPeer* p, char* buf, unsigned len);

#ifdef __cplusplus
}
#endif
#endif /* VMWM_PEER_H */
