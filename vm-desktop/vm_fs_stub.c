/* 非 Windows 实现：全部返回「本平台未实现」。
 *
 * 存在的意义是让 Files 窗口在别的平台仍能编译、能打开、能显示提示，
 * 而不是崩在缺符号上。真正的 POSIX 实现（opendir/readdir/stat）另写文件，
 * 届时把这里的同名函数各自用 #ifndef 包住即可，接口不变。
 */

#include "vm_fs.h"

#ifndef _WIN32

int VmFs_ListDir(const char *path, VmFsEntry *out, int cap, int *total) {
    (void)path;
    (void)out;
    (void)cap;
    if (total) *total = 0;
    return 0;
}

VmFsResult VmFs_Stat(const char *path, VmFsEntry *out) {
    (void)path;
    (void)out;
    return VM_FS_ERR_PLATFORM;
}

int VmFs_Cwd(char *out, int cap) {
    (void)out;
    (void)cap;
    return 0;
}

#endif /* !_WIN32 */
