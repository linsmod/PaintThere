# 文件能力抽象层

状态：接口冻结（2026-11-22）。实现见 `vm-desktop/vm_fs_win32.c`，非 Windows 桩见 `vm_fs_stub.c`，测试 `test_fs`。

## 1. 边界

`vm_fs` 只提供两个跨平台事实：列目录、取条目元数据。和 `vm_sys` 同一条分界——

| 属于 `vm_fs` | 不属于 `vm_fs` |
|---|---|
| 名字、大小、修改时间、是否目录 | 排序、过滤隐藏项、图标、类型推断 |
| UTF-8 路径输入输出 | 「这是不是项目文件」「能不能打开」 |

排序留给桌面层：Windows `FindFirstFileW` 和 POSIX `readdir` 的返回顺序不同且都不保证稳定，抽象层若擅自排序，调用方反而失去对顺序的控制。

## 2. UTF-8 是硬约束

这是本次设计中**最容易写错**的地方。

Windows 的 `FindFirstFileA` / `GetFileAttributesA` 按系统 ANSI 码页转码。本项目仓库路径之前含中文，用 A 系列 API 时：

- 输入含中文的路径 → 打不开目录，表现为「列出来是空的」
- 输出中文文件名 → 变成码页字节，UTF-8 解码后是乱码

因此实现一律走 W 系列，输入 `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)`，输出 `WideCharToMultiByte(CP_UTF8, ...)`。

验证方式是 `test_fs` 里两条断言：

- 以 `VmFs_Cwd()` 的结果（含中文的绝对路径）直接 `VmFs_Stat` / `VmFs_ListDir`
- 在 `docs` 目录中精确匹配 `arch-vmdesk.md`

前者验证 UTF-8 → UTF-16，后者验证 UTF-16 → UTF-8。

## 3. 容量与定长结构

`VmFsEntry` 是定长 POD（内联 `name[512]`、无指针），可以直接 `memcpy` 或塞进 IPC，不需要序列化中间层。

`VM_FS_NAME_MAX = 512` 的依据：Windows 文件名上限是 255 个 UTF-16 码元，纯中文名字在 UTF-8 下最多 765 字节。512 覆盖绝大多数真实名字，超长名字按字节截断而不是写出界。调用方拿到的名字可能因此不完整，但结构始终良构。

`VM_FS_MAX_ENTRIES = 512`：目录项容量。`*total` 永远返回真实总数，cap 不足时界面提示「已截断」。

## 4. 错误分类

```c
typedef enum { VM_FS_OK, VM_FS_ERR_NOT_FOUND,
               VM_FS_ERR_DENIED, VM_FS_ERR_PLATFORM } VmFsResult;
```

`NOT_FOUND`（路径不存在）和 `DENIED`（存在但没权限）必须可分：前者只需刷新界面，后者要提示用户换目录或提权。

`VmFs_ListDir` 与 `VmSys_ListProcs` 一样返回条数而非错误码——0 既可能是空目录也可能是打不开，调用方用 `VmFs_Stat` 区分。这是接口契约的一部分，不是遗漏。

## 5. 时间口径

`FILETIME` 是 1601-01-01 起的 100ns 滴答，`mtimeMs` 是 Unix epoch 毫秒，转换时减去 `116444736000000000` 再除以 10000。取不到（如某些特殊目录项）填 0，不伪造当前时间。

目录的 `size` 一律填 0——目录「大小」在 Windows 上没有稳定含义，不同 API 口径不一致。

## 6. 与 IPC 的关系

和 ProcMan 一样，当前 `demo_jpx` 的 Files 窗口**直接调用** `vm_fs`。这是第一版验证形态，不是最终形态：长期应由 WMS 统一提供目录浏览（限流、沙箱、路径白名单），应用不直接持有文件系统权限。

`VmFsEntry` 的定长 POD 形态保证了将来映射成 IPC 行时不需要中间格式。

## 7. 未实现

- **POSIX 实现**：`vm_fs_stub.c` 返回 `VM_FS_ERR_PLATFORM`，真实现用 `opendir`/`readdir`/`stat` 另写文件，接口不变。
- **符号链接目标、权限位、硬链接数**：Files 不需要，不抽象。
- **写操作**：没有创建、删除、重命名、复制。读目录是唯一被授权的能力。
