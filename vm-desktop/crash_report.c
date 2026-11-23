/* 崩溃钩子实现（见 crash_report.h）。
 *
 * Windows 路径：SetUnhandledExceptionFilter + MiniDumpWriteDump(dbghelp)。
 * 堆栈符号来自 PE 自带符号表（RelWithDebInfo 构建），stderr 直接给函数名；
 * 行号级分析用 dump：gdb build\demo_xxx.exe <dump>.dmp 或 cdb -z <dump>.dmp。
 * CRT 信号（SIGABRT/SIGSEGV/SIGFPE/SIGILL）同样走报告路径。
 * 非 Windows 平台为空实现，保证跨平台构建能链接。
 */

#include "crash_report.h"

#ifndef _WIN32

#include <stddef.h>
void CrashReport_Install(const char *dump_dir) { (void)dump_dir; }

#else

#include <windows.h>
#include <dbghelp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CRASH_REPORT_TEST_ENV "CRASH_REPORT_TEST"
#define CRASH_DUMP_DIR_ENV "CRASH_DUMP_DIR"

static char g_dump_dir[MAX_PATH];

/* stderr 可能被丢弃（窗口化启动、重定向失败），同时写一份 .rpt。
 * 两个 FILE 用同一组输出函数，保持内容一致。 */
static void report_fmt(FILE *out, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(out, "%s\n", buf);
}

static void report_line(FILE *out, const char *text) {
    fprintf(out, "%s\n", text);
}

static BOOL utf8_to_wide(const char *utf8, wchar_t *wide, DWORD chars) {
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)chars) > 0;
}

static void write_minidump(const wchar_t *dump_path, EXCEPTION_POINTERS *ep) {
    HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_EXCEPTION_INFORMATION *mei_p = NULL;
    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        mei_p = &mei;
    }
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpNormal, mei_p, NULL, NULL);
    CloseHandle(file);
}

/* dbghelp 符号引擎：Install 时初始化（崩溃时再初始化既慢又容易二次出错）。 */
static BOOL g_sym_ready = FALSE;

static void sym_engine_init(void) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    g_sym_ready = SymInitializeW(GetCurrentProcess(), NULL, TRUE);
}

/* 当前线程的符号化堆栈。异常过滤器/CRT 信号处理器都在故障线程上执行，
 * RtlCaptureStackBackTrace 抓到的就是崩溃现场。
 * 符号解析失败时打印 <模块基址+偏移>，可用 addr2line 离线换行号：
 *   addr2line -e demo_xxx.exe 0x<偏移> */
static void print_stack(FILE *out) {
    void *frames[62];
    unsigned short count = RtlCaptureStackBackTrace(0, 62, frames, NULL);
    if (!g_sym_ready) {
        sym_engine_init();
        if (!g_sym_ready) {
            report_line(out, "[crash] SymInitialize 失败，堆栈只有模块基址+偏移");
        }
    }
    for (unsigned short i = 0; i < count; i++) {
        DWORD64 addr = (DWORD64)(uintptr_t)frames[i];
        char module[MAX_PATH] = "?";
        DWORD64 base = 0;
        IMAGEHLP_MODULEW64 module_info;
        memset(&module_info, 0, sizeof(module_info));
        module_info.SizeOfStruct = sizeof(module_info);
        if (SymGetModuleInfoW64(GetCurrentProcess(), addr, &module_info)) {
            const wchar_t *base_name = wcsrchr(module_info.ImageName, L'\\');
            base_name = base_name ? base_name + 1 : module_info.ImageName;
            WideCharToMultiByte(CP_UTF8, 0, base_name, -1, module,
                                (int)sizeof(module), NULL, NULL);
            base = module_info.BaseOfImage;
        }
        char symbol_buf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO *symbol = (SYMBOL_INFO *)symbol_buf;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        if (g_sym_ready &&
            SymFromAddr(GetCurrentProcess(), addr, &displacement, symbol)) {
            report_fmt(out, "  #%u  %s!%s+0x%llx", i, module, symbol->Name,
                       (unsigned long long)displacement);
        } else if (base) {
            report_fmt(out, "  #%u  %s!base+0x%llx", i, module,
                       (unsigned long long)(addr - base));
        } else {
            report_fmt(out, "  #%u  0x%llx", i, (unsigned long long)addr);
        }
    }
}

void CrashReport_PrintStackNow(void) {
    print_stack(stderr);
    fflush(stderr);
}

static const char *exception_code_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
    default: return "EXCEPTION_OTHER";
    }
}

/* 生成同名 .rpt / .dmp 路径：<dump_dir>\<exe>_<pid>_<时间戳>.{rpt,dmp}。
 * dump_narrow 供打印（stderr 按 UTF-8 输出），dump_wide 供 CreateFileW。 */
static void make_report_paths(char *dump_narrow, size_t dump_narrow_chars,
                              wchar_t *dump_wide, size_t dump_wide_chars,
                              char *rpt_path, size_t rpt_path_chars) {
    wchar_t exe_w[MAX_PATH];
    char exe_name[MAX_PATH] = "process";
    if (GetModuleFileNameW(NULL, exe_w, MAX_PATH) > 0) {
        const wchar_t *base = wcsrchr(exe_w, L'\\');
        base = base ? base + 1 : exe_w;
        WideCharToMultiByte(CP_UTF8, 0, base, -1, exe_name,
                            (int)sizeof(exe_name), NULL, NULL);
        char *dot = strrchr(exe_name, '.');
        if (dot) *dot = '\0';
    }
    time_t now = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    snprintf(rpt_path, rpt_path_chars, "%s\\%s_%lu_%s.rpt",
             g_dump_dir, exe_name, (unsigned long)GetCurrentProcessId(), stamp);
    snprintf(dump_narrow, dump_narrow_chars, "%s\\%s_%lu_%s.dmp",
             g_dump_dir, exe_name, (unsigned long)GetCurrentProcessId(), stamp);
    if (!utf8_to_wide(dump_narrow, dump_wide, (DWORD)dump_wide_chars)) {
        dump_wide[0] = L'\0';
    }
}

/* 逐级创建 dump 目录（UTF-8 路径），目录已存在时静默通过。 */
static void ensure_dump_dir(const char *dir) {
    wchar_t wide[MAX_PATH];
    if (!utf8_to_wide(dir, wide, MAX_PATH)) return;
    for (wchar_t *p = wide; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t saved = *p;
            *p = L'\0';
            if (p > wide && p[-1] != L':') CreateDirectoryW(wide, NULL);
            *p = saved;
        }
    }
    CreateDirectoryW(wide, NULL);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *rec = ep->ExceptionRecord;
    char rpt_path[MAX_PATH] = {0};
    char dump_narrow[MAX_PATH] = {0};
    wchar_t dump_path[MAX_PATH] = {0};
    make_report_paths(dump_narrow, sizeof(dump_narrow), dump_path, MAX_PATH,
                      rpt_path, sizeof(rpt_path));

    FILE *rpt = rpt_path[0] ? fopen(rpt_path, "w") : NULL;
    FILE *outs[2] = { stderr, rpt };
    for (int o = 0; o < 2; o++) {
        FILE *out = outs[o];
        if (!out) continue;
        report_fmt(out, "[crash] 未处理异常 0x%08lX (%s) @ 0x%p",
                   (unsigned long)rec->ExceptionCode,
                   exception_code_name(rec->ExceptionCode),
                   rec->ExceptionAddress);
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
            rec->NumberParameters >= 2) {
            report_fmt(out, "[crash] %s地址 0x%p",
                       rec->ExceptionInformation[0] ? "写" : "读",
                       (void *)(uintptr_t)rec->ExceptionInformation[1]);
        }
        report_line(out, "[crash] 堆栈（当前线程，Release 优化下可能受内联影响）：");
        print_stack(out);
        report_fmt(out, "[crash] minidump: %s", dump_narrow);
        report_fmt(out, "[crash] 文本报告: %s", rpt_path);
        report_fmt(out, "[crash] 行号级分析: cdb -z <dump>.dmp（MinGW 符号需 gdb：gdb <exe> <dump>.dmp）");
        fflush(out);
        if (out != stderr) fclose(out);
    }
    write_minidump(dump_path, ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void signal_crash_handler(int sig) {
    /* CRT 信号没有异常指针，minidump 不带异常上下文，仍可用于定位线程。 */
    char rpt_path[MAX_PATH] = {0};
    char dump_narrow[MAX_PATH] = {0};
    wchar_t dump_path[MAX_PATH] = {0};
    make_report_paths(dump_narrow, sizeof(dump_narrow), dump_path, MAX_PATH,
                      rpt_path, sizeof(rpt_path));
    FILE *rpt = rpt_path[0] ? fopen(rpt_path, "w") : NULL;
    FILE *outs[2] = { stderr, rpt };
    for (int o = 0; o < 2; o++) {
        FILE *out = outs[o];
        if (!out) continue;
        report_fmt(out, "[crash] 致命信号 %d (SIGSEGV=11/SIGFPE=8/SIGILL=4/SIGABRT=22)", sig);
        report_line(out, "[crash] 堆栈（当前线程）：");
        print_stack(out);
        report_fmt(out, "[crash] minidump: %s", dump_narrow);
        report_fmt(out, "[crash] 文本报告: %s", rpt_path);
        fflush(out);
        if (out != stderr) fclose(out);
    }
    write_minidump(dump_path, NULL);
    _exit(3);
}

static void trigger_test_crash(void) {
    fprintf(stderr, "[crash] CRASH_REPORT_TEST=1，2 秒后触发测试崩溃 ...");
    fflush(stderr);
    Sleep(2000);
    fprintf(stderr, " 现在\n");
    fflush(stderr);
    *(volatile int *)0 = 42;
}

void CrashReport_Install(const char *dump_dir) {
    const char *env_dir = getenv(CRASH_DUMP_DIR_ENV);
    const char *dir = env_dir && env_dir[0] ? env_dir : dump_dir;
    if (dir && dir[0]) {
        snprintf(g_dump_dir, sizeof(g_dump_dir), "%s", dir);
    } else {
        wchar_t exe_w[MAX_PATH];
        if (GetModuleFileNameW(NULL, exe_w, MAX_PATH) > 0) {
            wchar_t drive[_MAX_DRIVE], dir_part[_MAX_DIR];
            _wsplitpath(exe_w, drive, dir_part, NULL, NULL);
            wchar_t dir_w[MAX_PATH];
            _wmakepath(dir_w, drive, dir_part, NULL, NULL);
            WideCharToMultiByte(CP_UTF8, 0, dir_w, -1, g_dump_dir,
                                (int)sizeof(g_dump_dir), NULL, NULL);
        } else {
            snprintf(g_dump_dir, sizeof(g_dump_dir), ".");
        }
    }
    SetUnhandledExceptionFilter(crash_filter);
    ensure_dump_dir(g_dump_dir);
    sym_engine_init();
    signal(SIGSEGV, signal_crash_handler);
    signal(SIGFPE, signal_crash_handler);
    signal(SIGILL, signal_crash_handler);
    signal(SIGABRT, signal_crash_handler);
    const char *test = getenv(CRASH_REPORT_TEST_ENV);
    if (test && strcmp(test, "1") == 0) trigger_test_crash();
}

#endif /* _WIN32 */
