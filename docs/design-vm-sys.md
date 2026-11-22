# 平台能力抽象层

状态：接口冻结（2026-11-22）。实现见 `vm-desktop/vm_sys_win32.c`，测试 `test_sys`。

## 1. 为什么需要

ProcMan 要列进程、看内存、看 CPU、结束进程。这四件事两个平台都有，但形态完全不同：

| 能力 | Windows | Linux |
|---|---|---|
| 枚举 | `CreateToolhelp32Snapshot` | 遍历 `/proc` |
| 内存 | `GetProcessMemoryInfo` → `WorkingSetSize` | `/proc/pid/statm` → RSS |
| CPU | `GetProcessTimes`（累计内核+用户时间） | `/proc/pid/stat` 的 utime+stime |
| 终止 | `OpenProcess` + `TerminateProcess` | `kill(2)` |
| 状态 | 无 sleep/zombie 概念 | `R`/`S`/`Z` 明确 |

不抽象的话，ProcMan 里会散落 `#ifdef _WIN32`，且第二份实现要把这些逻辑再抄一遍。

## 2. 边界：系统事实 vs 桌面语义

这是本次设计最重要的一条分界：

| 层 | 结构 | 拥有什么 | 不拥有什么 |
|---|---|---|---|
| `vm_sys` | `VmSysProc` | pid / ppid / 线程数 / 内存 / CPU / 名字 / 内核状态 | 不知道有窗口、不知道是哪个应用 |
| WMS | `VmIpcProcRow` | appHandle / 窗口数 / 桌面状态（未响应） / appId | 不知道 ppid / 线程数 |

两者字段几乎不重叠，说明**它们本来就不是同一件事**。WMS 组装 `VmIpcProcRow` 时，从 `VmSysProc` 取 `pid/memBytes/cpuPermille`，从自己的 app 表取 `appHandle/winCount/appId`。

如果图省事把 `appHandle` 塞进 `VmSysProc`，`vm_sys` 就会被迫知道「应用」这个概念，抽象层立刻腐烂。

## 3. 两个容易设计错的点

### 3.1 CPU 占用必须有状态

内核只给「累计 CPU 时间」，不给「当前占用率」。算占用率需要两次采样差分：

```
cpu% = (Δkernel + Δuser) / (Δwall × cpuCount)
```

所以 `VmSys_ListProcs` **不是纯函数**：内部保存上一次采样的 `pid → (kernel, user, wall)`，每次调用返回「自上次调用以来」的占用。

代价：第一次调用一律返回 0。这是接口契约的一部分，ProcMan 首帧显示 0 是正常的，不该为了「首帧好看」去伪造数据。

替代设计是「调用方传两次快照自己算」，但那等于把差分的脏活推给每个调用方，且快照结构要暴露出去。

### 3.2 不编造状态

Windows 没有可移植的 `sleep`/`zombie` 概念。抽象层返回 `VM_SYS_PROC_RUNNING` 或 `UNKNOWN`，**不假装**能区分睡眠和运行。

Linux 实现时自然能给出 `S`/`Z`，那时调用方要能容忍「同一个字段在两个平台的精度不同」——这是真实的，不该用假数据抹平。

## 4. 容量

`VM_SYS_MAX_PROCS` 定为 **512**。实测一台普通 Windows 开发机有 332 个进程，最初写 256 时连测试进程自己都查不到（截断在中途），`test_sys` 直接失败。

接口保证：即使 `cap` 小于实际进程数，`*total` 仍返回真实总数，调用方据此提示「已截断」。

## 5. 错误必须可区分

```c
typedef enum { VM_SYS_OK, VM_SYS_ERR_NOT_FOUND, VM_SYS_ERR_DENIED, VM_SYS_ERR_PLATFORM } VmSysResult;
```

- `NOT_FOUND`：进程已经退出 → 界面刷新即可，不要弹错误
- `DENIED`：进程还在但权限不足 → 提示「需要提权」
- `PLATFORM`：当前平台没实现 → ProcMan 显示「本平台未实现」

把这两种混成一个 `int` 布尔值，ProcMan 就会对着一个已退出的进程反复重试。

## 6. 未实现

- **Linux 实现**：`vm_sys_stub.c` 目前对所有调用返回 `PLATFORM`，保证非 Windows 能编译能跑。真实现只需新加 `vm_sys_linux.c`，读 `/proc`，接口不变。
- **进程树、句柄、模块、GPU**：ProcMan 不需要，不抽象。
- **跨用户/提权**：不做 UAC 提升，权限不足就如实报 `DENIED`。

## 7. 与 IPC 的关系

`VmSysProc` 是定长 POD，WMS 拿到后直接映射成 `VmIpcProcRow` 发给 ProcMan，不需要中间格式。

ProcMan 通过 `PROC_LIST_REQ` 拿的是**桌面视角**的行（含 appHandle、窗口数），WMS 用 `vm_sys` 补系统字段。因此 ProcMan 自己**不直接调 `vm_sys`**——否则每个应用都有杀进程的权限，且 WMS 无法统一限流。
