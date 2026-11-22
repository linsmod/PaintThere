#ifndef VM_SYS_H
#define VM_SYS_H

/* 平台能力抽象层。
 *
 * 只收敛「内核提供的、两个平台都有但形态不同」的事实：进程枚举、内存、CPU、
 * 终止进程。桌面语义（哪个 pid 对应哪个应用、有几个窗口、是否响应）不在这里，
 * 那是 WMS 的 app 表。
 *
 * 结果结构是定长 POD（内联数组、无指针），可以直接跨进程 memcpy，
 * 也可以直接被 WMS 组装成 VmIpcProcRow 发给 ProcMan。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 系统层能拿到的进程状态。注意与 VmIpcProcState 不同：
 * 那是桌面层判断的「是否响应布局请求」，这里是内核视角的调度状态。
 * Windows 没有可移植的 sleep/zombie 概念，只能给出 RUNNING 或 UNKNOWN，
 * 不要为了让两个平台「看起来一致」而编造状态。 */
typedef enum {
    VM_SYS_PROC_RUNNING = 0,
    VM_SYS_PROC_SLEEPING = 1,
    VM_SYS_PROC_ZOMBIE = 2,
    VM_SYS_PROC_UNKNOWN = 3
} VmSysProcState;

/* 建议的输出容量。实测一台普通 Windows 开发机就有 330+ 进程，
   256 会在中途截断、连调用方自己都查不到；调用方仍可传更小的 cap，
   *total 永远返回真实总数，界面据此提示「已截断」。 */
#define VM_SYS_MAX_PROCS 512
#define VM_SYS_NAME_MAX 64

typedef struct {
    int32_t pid;
    int32_t ppid;
    int32_t threads;
    uint32_t state;          /* VmSysProcState */
    uint64_t memBytes;       /* Windows 工作集 / Linux RSS：只承诺「粗略占用」 */
    uint32_t cpuPermille;    /* 自上次调用 VmSys_ListProcs 以来的占用，1000 = 单核满 */
    char name[VM_SYS_NAME_MAX];  /* Windows ImageName / Linux comm，均不带路径 */
} VmSysProc;

typedef enum {
    VM_SYS_OK = 0,
    VM_SYS_ERR_NOT_FOUND,   /* 进程不存在（已退出） */
    VM_SYS_ERR_DENIED,      /* 存在但权限不足 */
    VM_SYS_ERR_PLATFORM     /* 当前平台没有实现 */
} VmSysResult;

/* 枚举进程。
 * 返回写入 out 的条数（<= cap）；进程总数写入 *total（可能大于 cap，调用方据此提示「已截断」）。
 * cpuPermille 是**有状态**的：内部保存上一次采样的累计 CPU 时间，
 * 每次调用返回「上次调用到现在」的占用。第一次调用一律返回 0。 */
int VmSys_ListProcs(VmSysProc *out, int cap, int *total);

/* 终止进程。必须区分「不存在」和「拒绝访问」：前者说明已经退出（界面刷新即可），
 * 后者要提示用户提权，两者混在一起会让 ProcMan 反复重试。 */
VmSysResult VmSys_KillProc(int32_t pid);

int32_t  VmSys_SelfPid(void);
uint64_t VmSys_UptimeMs(void);
int      VmSys_CpuCount(void);
uint64_t VmSys_MemTotalBytes(void);
uint64_t VmSys_MemAvailBytes(void);

/* 平台实现选择：编译期由 CMake 决定实现文件，这里不出现 #ifdef。 */

#ifdef __cplusplus
}
#endif

#endif
