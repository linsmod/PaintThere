/* 平台能力抽象层的 Windows 实现。
 *
 * 枚举用 Toolhelp 快照（一次快照拿 pid/ppid/线程数/名字），
 * 内存与 CPU 需要逐进程 OpenProcess：系统进程会失败，此时填 0 并标 UNKNOWN，
 * 不要让一个失败拖垮整张表。
 */

#include "vm_sys.h"

#ifdef _WIN32

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <string.h>

/* CPU 占用靠两次采样差分：内核只给累计时间，不给「当前占用率」。 */
typedef struct {
    int32_t pid;
    uint64_t kernel100ns;
    uint64_t user100ns;
    int used;
} CpuSample;

static CpuSample g_prev[VM_SYS_MAX_PROCS];
static int g_prevCount;
static uint64_t g_prevWall100ns;

static uint64_t ft_to_u64(const FILETIME *ft) {
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static uint64_t wall_now_100ns(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ft_to_u64(&ft);
}

static const CpuSample *find_prev(int32_t pid) {
    for (int i = 0; i < g_prevCount; i++)
        if (g_prev[i].used && g_prev[i].pid == pid) return &g_prev[i];
    return NULL;
}

int VmSys_CpuCount(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

uint64_t VmSys_UptimeMs(void) {
    return (uint64_t)GetTickCount64();
}

int32_t VmSys_SelfPid(void) {
    return (int32_t)GetCurrentProcessId();
}

uint64_t VmSys_MemTotalBytes(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    return (uint64_t)ms.ullTotalPhys;
}

uint64_t VmSys_MemAvailBytes(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    return (uint64_t)ms.ullAvailPhys;
}

int VmSys_ListProcs(VmSysProc *out, int cap, int *total) {
    if (total) *total = 0;
    if (!out || cap <= 0) return 0;

    uint64_t wall = wall_now_100ns();
    uint64_t wallDelta = (g_prevWall100ns && wall > g_prevWall100ns) ? wall - g_prevWall100ns : 0;
    int cpuCount = VmSys_CpuCount();
    int n = 0;
    int seen = 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    /* 新一代快照直接覆盖上一代：没用到的槽位会在结束时被清掉。 */
    CpuSample next[VM_SYS_MAX_PROCS];
    memset(next, 0, sizeof(next));

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            seen++;
            if (n >= cap) continue;   /* 继续数总数，但不再写 out */

            VmSysProc *p = &out[n];
            memset(p, 0, sizeof(*p));
            p->pid = (int32_t)pe.th32ProcessID;
            p->ppid = (int32_t)pe.th32ParentProcessID;
            p->threads = (int32_t)pe.cntThreads;
            p->state = VM_SYS_PROC_UNKNOWN;
            strncpy(p->name, pe.szExeFile, VM_SYS_NAME_MAX - 1);

            /* 逐进程拿内存与 CPU 累计时间；失败时保持 0，不中断整表。 */
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                   FALSE, pe.th32ProcessID);
            if (h) {
                PROCESS_MEMORY_COUNTERS_EX mc;
                mc.cb = sizeof(mc);
                if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&mc, sizeof(mc)))
                    p->memBytes = (uint64_t)mc.WorkingSetSize;

                FILETIME c, e, k, u;
                if (GetProcessTimes(h, &c, &e, &k, &u)) {
                    if (n < VM_SYS_MAX_PROCS) {
                        next[n].pid = p->pid;
                        next[n].kernel100ns = ft_to_u64(&k);
                        next[n].user100ns = ft_to_u64(&u);
                        next[n].used = 1;
                    }
                    const CpuSample *prev = find_prev(p->pid);
                    if (prev && wallDelta > 0) {
                        uint64_t dk = next[n].kernel100ns > prev->kernel100ns
                                          ? next[n].kernel100ns - prev->kernel100ns : 0;
                        uint64_t du = next[n].user100ns > prev->user100ns
                                          ? next[n].user100ns - prev->user100ns : 0;
                        /* 1000 = 单核满载：wallDelta 是整机时间，乘核数再除。 */
                        uint64_t permille = (dk + du) * 1000ull * (uint64_t)cpuCount / wallDelta;
                        p->cpuPermille = permille > 1000 ? 1000 : (uint32_t)permille;
                    }
                }
                DWORD code = 0;
                if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE)
                    p->state = VM_SYS_PROC_RUNNING;
                CloseHandle(h);
            }
            n++;
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    memcpy(g_prev, next, sizeof(g_prev));
    g_prevCount = n < VM_SYS_MAX_PROCS ? n : VM_SYS_MAX_PROCS;
    g_prevWall100ns = wall;

    if (total) *total = seen;
    return n;
}

VmSysResult VmSys_KillProc(int32_t pid) {
    if (pid <= 0) return VM_SYS_ERR_NOT_FOUND;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) {
        DWORD err = GetLastError();
        return err == ERROR_ACCESS_DENIED ? VM_SYS_ERR_DENIED : VM_SYS_ERR_NOT_FOUND;
    }
    BOOL ok = TerminateProcess(h, 1);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (ok) return VM_SYS_OK;
    return err == ERROR_ACCESS_DENIED ? VM_SYS_ERR_DENIED : VM_SYS_ERR_NOT_FOUND;
}

#endif /* _WIN32 */
