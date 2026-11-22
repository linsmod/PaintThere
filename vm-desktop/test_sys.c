/* 平台能力抽象层测试：只断言「内核事实」，不碰桌面语义。
   Run: test_sys.exe [--probe] */

#include "vm_sys.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;
static int g_quiet;

static void expect(int cond, const char *what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("FAIL: %s\n", what);
    } else if (!g_quiet) {
        printf("ok: %s\n", what);
    }
}

int main(int argc, char **argv) {
    g_quiet = (argc > 1 && strcmp(argv[1], "--probe") == 0);

    int32_t self = VmSys_SelfPid();
    expect(self > 0, "self pid is positive");
    expect(VmSys_CpuCount() >= 1, "cpu count is at least 1");
    expect(VmSys_MemTotalBytes() > 0, "total memory reported");
    expect(VmSys_MemAvailBytes() <= VmSys_MemTotalBytes(), "avail memory within total");

    static VmSysProc procs[VM_SYS_MAX_PROCS];
    int total = 0;
    int n = VmSys_ListProcs(procs, VM_SYS_MAX_PROCS, &total);
    expect(n > 0, "process list is not empty");
    expect(total >= n, "total is not smaller than returned count");

    /* 自己必须在列表里，且内存占用可读：这是「抽象层真的连上了内核」的证据。 */
    const VmSysProc *me = NULL;
    for (int i = 0; i < n; i++) {
        if (procs[i].pid == self) {
            me = &procs[i];
            break;
        }
    }
    expect(me != NULL, "self appears in process list");
    if (me) {
        expect(me->memBytes > 0, "self working set is readable");
        expect(me->name[0] != '\0', "self process name is filled");
        expect(me->threads >= 1, "self has at least one thread");
    }

    /* cpuPermille 是有状态的：第二次采样才有意义，且必须落在合法区间。 */
    int bad = 0;
    int override = 0;
    for (int i = 0; i < n; i++)
        if (procs[i].cpuPermille > 1000) bad++;
        else if (procs[i].cpuPermille > 0) override++;
    expect(bad == 0, "cpu permille stays within 0..1000");

    n = VmSys_ListProcs(procs, VM_SYS_MAX_PROCS, &total);
    bad = 0;
    for (int i = 0; i < n; i++)
        if (procs[i].cpuPermille > 1000) bad++;
    expect(bad == 0, "cpu permille stays within range on resample");

    /* 截断路径：cap 很小的时候仍要返回真实总数。 */
    VmSysProc one;
    int smallTotal = 0;
    int got = VmSys_ListProcs(&one, 1, &smallTotal);
    expect(got == 1, "cap of 1 returns exactly one row");
    expect(smallTotal >= n, "truncated call still reports full total");

    /* 错误必须可区分：不存在的 pid 不能返回 DENIED。 */
    expect(VmSys_KillProc(-1) == VM_SYS_ERR_NOT_FOUND, "negative pid is not found");
    expect(VmSys_KillProc(0x7FFFFFF0) != VM_SYS_ERR_DENIED,
           "nonexistent pid is not reported as denied");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
