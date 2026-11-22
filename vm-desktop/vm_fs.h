#ifndef VM_FS_H
#define VM_FS_H

/* 平台文件能力抽象层。
 *
 * 只收敛两个跨平台事实：列目录、取条目元数据。职责和 vm_sys 一样严格限定在
 * 内核提供的能力上——不排序、不过滤、不判断「这是不是项目文件」。
 * 排序与展示留给桌面层。
 *
 * 所有字符串都是 UTF-8。Windows 实现必须走 FindFirstFileW /
 * GetFileAttributesW 再转 UTF-8：用 ANSI 版会让中文名变成乱码，
 * 而且含中文的路径本身就会打不开。
 *
 * 结果结构是定长 POD（内联数组、无指针），可以直接 memcpy 或放进 IPC。
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8 下一个中文字符占 3 字节，255 个中文字符的文件名可达 765 字节；
   512 能覆盖绝大多数真实名字，超长的按字节截断（不会写出界）。 */
#define VM_FS_NAME_MAX 512
#define VM_FS_PATH_MAX 1024
#define VM_FS_MAX_ENTRIES 512

typedef struct {
    char name[VM_FS_NAME_MAX];  /* UTF-8，仅文件名，不含路径；不含 "." 和 ".." */
    uint64_t size;              /* 目录填 0 */
    uint64_t mtimeMs;           /* Unix epoch 毫秒；取不到填 0 */
    int isDir;                  /* 0 或 1 */
} VmFsEntry;

typedef enum {
    VM_FS_OK = 0,
    VM_FS_ERR_NOT_FOUND,
    VM_FS_ERR_DENIED,
    VM_FS_ERR_PLATFORM
} VmFsResult;

/* 列目录。返回写入 out 的条数（<= cap）；条目总数写入 *total，
 * cap 不足时 *total 仍返回真实总数，调用方据此提示「已截断」。
 * 顺序是平台原始顺序（Windows FindFirstFile / POSIX readdir），不承诺排序。 */
int VmFs_ListDir(const char *path, VmFsEntry *out, int cap, int *total);

/* 取单个路径的元数据。错误必须能区分「不存在」和「权限不足」，
 * 否则界面会对已消失的路径反复重试，或对无权访问的路径假装不存在。 */
VmFsResult VmFs_Stat(const char *path, VmFsEntry *out);

/* 当前工作目录（UTF-8）。成功返回 1。 */
int VmFs_Cwd(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif
