/*
 * vmwm_peer.c - 进程身份抽象的平台实现
 *
 * 只有 Windows 与 POSIX(AF_UNIX) 两条真实路径；其余情况走 mock，
 * 返回 verified = 0，让上层逻辑在没有平台能力时依然能跑通。
 */
#if !defined(_WIN32)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE      /* struct ucred / SO_PEERPIDFD 需要 */
#  endif
#endif

#include <stdio.h>
#include <string.h>

#include "wms_peer.h"

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <poll.h>
#    include <sys/syscall.h>
#  endif
#endif

VmWmPeer VmWm_PeerFromFd(intptr_t fd)
{
    VmWmPeer p;

    memset(&p, 0, sizeof(p));
    p.handle_kind = VMWM_PEER_HANDLE_NONE;

#if defined(_WIN32)
    {
        DWORD pid = 0;
        HANDLE proc;
        HANDLE pipe = (HANDLE)fd;

        if (!pipe || pipe == INVALID_HANDLE_VALUE) return p;
        if (!GetNamedPipeClientProcessId(pipe, &pid) || pid == 0) return p;

        /* SYNCHRONIZE 才能 WaitForSingleObject；QUERY_LIMITED 只为读信息 */
        proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
        if (!proc) return p;

        p.pid         = (uint32_t)pid;
        p.uid         = 0;
        p.verified    = 1;
        p.handle_kind = VMWM_PEER_HANDLE_PROCESS;
        p.handle      = (intptr_t)proc;
    }
#elif defined(__unix__) || defined(__APPLE__)
    {
        /* 优先 pidfd：它是「进程实例」的句柄，pid 复用也不认错人 */
#  if defined(__linux__) && defined(SO_PEERPIDFD)
        int pidfd = -1;
        socklen_t plen = sizeof(pidfd);
        if (getsockopt((int)fd, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &plen) == 0 &&
            pidfd >= 0) {
            p.pid         = 0;      /* 由 pidfd 反查，非必需 */
            p.verified    = 1;
            p.handle_kind = VMWM_PEER_HANDLE_PIDFD;
            p.handle      = (intptr_t)pidfd;
            return p;
        }
#  endif
        {
            struct ucred cred;
            socklen_t   clen = sizeof(cred);

            if (getsockopt((int)fd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) == 0 &&
                clen == sizeof(cred)) {
                p.pid      = (uint32_t)cred.pid;
                p.uid      = (uint32_t)cred.uid;
                p.verified = 1;
#  if defined(__linux__)
                /* 连接快照的 pid 可能过期，立刻转成 pidfd 消除复用竞态 */
                {
                    long r = syscall(SYS_pidfd_open, (pid_t)cred.pid, 0);
                    if (r >= 0) {
                        p.handle_kind = VMWM_PEER_HANDLE_PIDFD;
                        p.handle      = (intptr_t)r;
                    }
                }
#  endif
            }
        }
    }
#else
    (void)fd;   /* mock：没有平台能力 */
#endif

    return p;
}

void VmWm_PeerRelease(VmWmPeer* p)
{
    if (!p) return;

#if defined(_WIN32)
    if (p->handle_kind == VMWM_PEER_HANDLE_PROCESS && p->handle) {
        CloseHandle((HANDLE)p->handle);
    }
#elif defined(__unix__) || defined(__APPLE__)
    if (p->handle_kind == VMWM_PEER_HANDLE_PIDFD && p->handle > 0) {
        close((int)p->handle);
    }
#endif

    memset(p, 0, sizeof(*p));
}

int VmWm_PeerAlive(const VmWmPeer* p)
{
    if (!p || !p->verified) return -1;

#if defined(_WIN32)
    if (p->handle_kind == VMWM_PEER_HANDLE_PROCESS && p->handle) {
        DWORD r = WaitForSingleObject((HANDLE)p->handle, 0);
        if (r == WAIT_OBJECT_0) return 0;
        if (r == WAIT_TIMEOUT)  return 1;
        return -1;
    }
#elif defined(__linux__)
    if (p->handle_kind == VMWM_PEER_HANDLE_PIDFD && p->handle > 0) {
        struct pollfd pf;
        pf.fd      = (int)p->handle;
        pf.events  = POLLIN;
        pf.revents = 0;
        if (poll(&pf, 1, 0) == 0) return 1;      /* 没事件 = 进程还在 */
        if (pf.revents & (POLLIN | POLLHUP | POLLERR)) return 0;
        return -1;
    }
#endif
    return -1;
}

const char* VmWm_PeerDescribe(const VmWmPeer* p, char* buf, unsigned len)
{
    if (!buf || !len) return "";

    if (!p || !p->verified) {
        snprintf(buf, len, "peer=unverified");
        return buf;
    }
    snprintf(buf, len, "peer pid=%u uid=%u src=%s",
             (unsigned)p->pid, (unsigned)p->uid,
             p->handle_kind == VMWM_PEER_HANDLE_PIDFD ? "pidfd" :
             p->handle_kind == VMWM_PEER_HANDLE_PROCESS ? "proc-handle" : "none");
    return buf;
}
