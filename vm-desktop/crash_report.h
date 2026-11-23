#ifndef CRASH_REPORT_H
#define CRASH_REPORT_H

/* 崩溃钩子（可复用）：未处理异常或致命 CRT 信号发生时，
 * 向 stderr 打印符号化调用堆栈，并在 dump 目录写一个 minidump 与 .rpt 文本报告。
 *
 * 用法：exe 链接 crash_report 静态库，在 main 第一行调用一次：
 *     CrashReport_Install(NULL);
 *
 * dump_dir: minidump 输出目录；NULL 用 exe 所在目录。
 *           环境变量 CRASH_DUMP_DIR 优先于参数（便于不改代码切换输出位置）。
 *
 * 测试钩子：环境变量 CRASH_REPORT_TEST=1 时，安装完成后立即触发一次
 *           空指针写入，用于验证钩子本身；正常使用不要设置该变量。
 *
 * 崩溃后退出码为异常码（如 0xC0000005）。
 *
 * 调试接口：随时打印当前线程的符号化堆栈到 stderr（不崩溃、不写 dump）。
 */
void CrashReport_Install(const char *dump_dir);
void CrashReport_PrintStackNow(void);

#endif /* CRASH_REPORT_H */
