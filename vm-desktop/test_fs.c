/* 平台文件抽象层测试：只断言内核事实（路径、元数据、UTF-8 往返、错误分类）。
   Run: test_fs.exe [--probe] */

#include "vm_fs.h"

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

    /* 仓库路径本身就含中文。cwd 能读、能 Stat、能列目录，等于验证了
     * UTF-8 输入路径到 Windows 宽字符的完整往返。 */
    char cwd[VM_FS_PATH_MAX];
    memset(cwd, 0, sizeof(cwd));
    expect(VmFs_Cwd(cwd, sizeof(cwd)) == 1, "cwd is readable");
    expect(cwd[0] != '\0', "cwd is not empty");

    VmFsEntry cwdEntry;
    expect(VmFs_Stat(cwd, &cwdEntry) == VM_FS_OK, "cwd stats as existing path");
    expect(cwdEntry.isDir == 1, "cwd is a directory");
    expect(cwdEntry.name[0] != '\0', "cwd entry has a basename");

    static VmFsEntry here[VM_FS_MAX_ENTRIES];
    int total = 0;
    int n = VmFs_ListDir(cwd, here, VM_FS_MAX_ENTRIES, &total);
    expect(n > 0, "cwd can be listed through UTF-8 path");
    expect(total >= n, "total is not smaller than returned count");
    expect(n < VM_FS_MAX_ENTRIES, "cwd fits in capacity");

    /* 每个条目都必须是良构的 POD，名字不能空，isDir 只能是 0/1。 */
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (here[i].name[0] == '\0' || (here[i].isDir != 0 && here[i].isDir != 1))
            bad++;
        if (strcmp(here[i].name, ".") == 0 || strcmp(here[i].name, "..") == 0)
            bad++;
    }
    expect(bad == 0, "entries are well formed and dot entries are filtered");

    /* docs 里有一个中文文件名。精确匹配它，验证 UTF-16 -> UTF-8 的输出转换。 */
    static VmFsEntry docs[VM_FS_MAX_ENTRIES];
    int docsTotal = 0;
    int dn = VmFs_ListDir("docs", docs, VM_FS_MAX_ENTRIES, &docsTotal);
    expect(dn > 0, "docs directory is listed");
    int chineseFound = 0;
    int indexFound = 0;
    for (int i = 0; i < dn; i++) {
        if (strcmp(docs[i].name, "arch-\u8de8\u7aef\u56fe\u7ed8\u6307\u4ee4\u6d41.md") == 0)
            chineseFound = 1;
        if (strcmp(docs[i].name, "INDEX.md") == 0) indexFound = 1;
    }
    expect(chineseFound == 1, "chinese filename round-trips as UTF-8");

    /* Stat 一个真实文件：非目录、有大小、有修改时间。 */
    expect(VmFs_Stat("docs/INDEX.md", &cwdEntry) == VM_FS_OK, "known file stats ok");
    expect(cwdEntry.isDir == 0, "known file is not a directory");
    expect(cwdEntry.size > 0, "known file has a nonzero size");
    expect(cwdEntry.mtimeMs > 1577836800000ull, "known file has a plausible mtime");
    expect(indexFound == 1, "known file appears in directory listing");

    /* 截断路径：cap 不足仍返回真实总数。 */
    VmFsEntry one;
    int smallTotal = 0;
    int got = VmFs_ListDir("docs", &one, 1, &smallTotal);
    expect(got == 1, "cap of 1 returns exactly one row");
    expect(smallTotal >= dn, "truncated call still reports full total");

    /* 错误分类：不存在的路径必须是 NOT_FOUND，不能被当成 DENIED。 */
    const char *missing = "docs/__vm_fs_no_such_path__/nothing.txt";
    expect(VmFs_Stat(missing, &cwdEntry) == VM_FS_ERR_NOT_FOUND,
           "missing path is reported as not found");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
