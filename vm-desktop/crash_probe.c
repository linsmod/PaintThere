/* 崩溃钩子探针：先直接打印当前堆栈（验证符号解析），再触发真实崩溃。
 * 用法：crash_probe.exe；设 CRASH_DUMP_DIR 可重定向 dump 目录。
 * 只用于开发验证，不进入 test-all。 */
#include "crash_report.h"

#include <stdio.h>
#include <windows.h>

static int helper_that_crashes(void) {
    *(volatile int *)0 = 42;
    return 0;
}

int main(void) {
    CrashReport_Install(NULL);
    printf("[probe] 当前堆栈：\n");
    CrashReport_PrintStackNow();
    fflush(stdout);
    Sleep(1500);
    return helper_that_crashes();
}
