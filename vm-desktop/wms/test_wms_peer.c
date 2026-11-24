/*
 * test_peer.c - 进程身份抽象的自测（Windows 段真实现，其它平台跳过）
 *
 * 这里验证的是「探活语义」，不是「从管道拿身份」那一步——后者已由 e2e 的
 * 服务端日志 cover（handshake ... peer pid=... src=proc-handle）。
 *
 * 关键断言：已退出的进程必须判 0，未验证身份必须判 -1（不能让上层误以为活着）；
 * 判活只看句柄，不做 kill(pid,0) 那种在僵尸/EPERM 上会骗人的探活。
 */
#include <stdio.h>
#include <string.h>

#include "wms_peer.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static int g_checks = 0;
static int g_fails  = 0;

static void check(int cond, const char* what)
{
    g_checks++;
    if (!cond) {
        g_fails++;
        printf("[FAIL] %s\n", what);
    }
}

int main(void)
{
#ifdef _WIN32
    VmWmPeer me, dead;
    char     buf[128];

    /* 1. 自己的进程：句柄方式应当判活 */
    memset(&me, 0, sizeof(me));
    me.pid         = (uint32_t)GetCurrentProcessId();
    me.verified    = 1;
    me.handle_kind = VMWM_PEER_HANDLE_PROCESS;
    me.handle      = (intptr_t)OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, me.pid);

    check(me.handle != 0, "OpenProcess(self)");
    check(VmWm_PeerAlive(&me) == 1, "self is alive");
    check(strstr(VmWm_PeerDescribe(&me, buf, sizeof(buf)), "proc-handle") != NULL,
          "describe names handle kind");
    VmWm_PeerRelease(&me);
    check(me.verified == 0 && me.handle == 0, "release clears the peer");

    /* 2. 未验证身份：必须返回 -1，上层不得据此断连 */
    memset(&dead, 0, sizeof(dead));
    check(VmWm_PeerAlive(&dead) == -1, "unverified peer -> -1");
    check(VmWm_PeerAlive(NULL) == -1, "NULL peer -> -1");

    /* 3. 已退出的进程：保留句柄时仍要判 0（pid 复用也不认错实例） */
    {
        STARTUPINFOA        si;
        PROCESS_INFORMATION pi;
        char                cmd[] = "cmd.exe /c exit";

        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);

        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 5000);

            dead.pid         = (uint32_t)pi.dwProcessId;
            dead.verified    = 1;
            dead.handle_kind = VMWM_PEER_HANDLE_PROCESS;
            dead.handle      = (intptr_t)pi.hProcess;

            check(VmWm_PeerAlive(&dead) == 0, "exited process -> 0");
            VmWm_PeerRelease(&dead);
        } else {
            check(0, "CreateProcess for exit probe");
        }
    }
#else
    printf("test_peer: 跳过（pidfd / SO_PEERCRED 路径需在 Linux 上验证）\n");
#endif

    printf("checks=%d failed=%d\n", g_checks, g_fails);
    if (!g_fails) printf("ALL PASS\n");
    return g_fails ? 1 : 0;
}
