/* 平台能力抽象层的非 Windows 实现：全部返回「当前平台没有实现」。
 *
 * 存在的意义是让调用方在别的平台上仍然能编译、能跑，只是 ProcMan 会显示
 * 「本平台未实现」而不是崩在缺符号上。真正的 Linux 实现（读 /proc）另写一个文件，
 * 届时把这里的同名函数各自用 #ifndef 包住即可。
 */

#include "vm_sys.h"

#ifndef _WIN32

int VmSys_ListProcs(VmSysProc *out, int cap, int *total) {
    (void)out;
    (void)cap;
    if (total) *total = 0;
    return 0;
}

VmSysResult VmSys_KillProc(int32_t pid) {
    (void)pid;
    return VM_SYS_ERR_PLATFORM;
}

int32_t VmSys_SelfPid(void) { return 0; }
uint64_t VmSys_UptimeMs(void) { return 0; }
int VmSys_CpuCount(void) { return 1; }
uint64_t VmSys_MemTotalBytes(void) { return 0; }
uint64_t VmSys_MemAvailBytes(void) { return 0; }

#endif /* !_WIN32 */
