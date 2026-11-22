/* 平台文件能力抽象层的 Windows 实现。
 *
 * 全程走 W 系列 API。输入路径用 MultiByteToWideChar(CP_UTF8) 转宽字符，
 * 输出文件名用 WideCharToMultiByte(CP_UTF8) 转回；任何 A 系列 API 都会
 * 按系统 ANSI 码页转码，中文路径直接失效。
 */

#include "vm_fs.h"

#ifdef _WIN32

#include <windows.h>

#include <string.h>
#include <wchar.h>

/* FILETIME 是 1601-01-01 起 100ns 滴答；Unix epoch 从 1970-01-01 起。 */
#define WIN_EPOCH_DIFF_100NS 116444736000000000ull

static int utf8_to_wide(const char *src, wchar_t *dst, int dstCap) {
    if (!src || !dst || dstCap <= 0) return 0;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (n <= 0 || n > dstCap) return 0;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, n);
    return 1;
}

static void wide_to_utf8(const wchar_t *src, char *dst, int dstCap) {
    if (!dst || dstCap <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (n > 0 && n <= dstCap)
        WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, n, NULL, NULL);
}

static uint64_t ft_to_unix_ms(const FILETIME *ft) {
    uint64_t v = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (v <= WIN_EPOCH_DIFF_100NS) return 0;
    return (v - WIN_EPOCH_DIFF_100NS) / 10000ull;
}

static VmFsResult classify_error(DWORD err) {
    if (err == ERROR_ACCESS_DENIED) return VM_FS_ERR_DENIED;
    return VM_FS_ERR_NOT_FOUND;
}

int VmFs_ListDir(const char *path, VmFsEntry *out, int cap, int *total) {
    if (total) *total = 0;
    if (!out || cap <= 0 || !path || !path[0]) return 0;

    wchar_t dir[VM_FS_PATH_MAX];
    if (!utf8_to_wide(path, dir, VM_FS_PATH_MAX)) return 0;

    wchar_t pattern[VM_FS_PATH_MAX + 4];
    size_t dirLen = wcslen(dir);
    if (dirLen + 3 > VM_FS_PATH_MAX + 4) return 0;
    memcpy(pattern, dir, dirLen * sizeof(wchar_t));
    pattern[dirLen] = L'\\';
    pattern[dirLen + 1] = L'*';
    pattern[dirLen + 2] = L'\0';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int n = 0;
    int seen = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        seen++;
        if (n >= cap) continue;

        VmFsEntry *e = &out[n];
        memset(e, 0, sizeof(*e));
        wide_to_utf8(fd.cFileName, e->name, VM_FS_NAME_MAX);
        e->isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        if (!e->isDir)
            e->size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e->mtimeMs = ft_to_unix_ms(&fd.ftLastWriteTime);
        n++;
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    if (total) *total = seen;
    return n;
}

VmFsResult VmFs_Stat(const char *path, VmFsEntry *out) {
    if (!path || !path[0] || !out) return VM_FS_ERR_NOT_FOUND;

    wchar_t wide[VM_FS_PATH_MAX];
    if (!utf8_to_wide(path, wide, VM_FS_PATH_MAX)) return VM_FS_ERR_NOT_FOUND;

    DWORD attr = GetFileAttributesW(wide);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return classify_error(GetLastError());

    memset(out, 0, sizeof(*out));
    const wchar_t *slash = wcsrchr(wide, L'\\');
    if (!slash) slash = wcsrchr(wide, L'/');
    wide_to_utf8(slash ? slash + 1 : wide, out->name, VM_FS_NAME_MAX);
    out->isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

    HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME mt;
        if (GetFileTime(h, NULL, NULL, &mt)) out->mtimeMs = ft_to_unix_ms(&mt);
        LARGE_INTEGER sz;
        if (!out->isDir && GetFileSizeEx(h, &sz) && sz.QuadPart > 0)
            out->size = (uint64_t)sz.QuadPart;
        CloseHandle(h);
    }
    return VM_FS_OK;
}

int VmFs_Cwd(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    wchar_t wide[VM_FS_PATH_MAX];
    DWORD n = GetCurrentDirectoryW(VM_FS_PATH_MAX, wide);
    if (n == 0 || n >= VM_FS_PATH_MAX) return 0;
    wide_to_utf8(wide, out, cap);
    return out[0] != '\0';
}

#endif /* _WIN32 */
