# WMS 线路协议 · 设计记录

> **这不是现状。** 本文冻结于 2026-11-24，是 2026-11-22 版本的**重写**：只写「线路层要长什么样」。
> 进程归属、崩溃边界、工作区/主题/资源的归属决策见 `docs/design-vm-wms-process.md`。
> 协议常量与结构体的最终形态落在 `vm-desktop/vm_ipc.h`；本文与它是同一份决策的两种表达，改一处必须改另一处。
> 当前 `vm_ipc.h` 尚未包含本文新增的消息与结构，按 `design-vm-wms-process.md` 的 P0 阶段同步。
> 当前实现见 `docs/arch-vmdesk.md`。

## 1. 范围

只定义 WMS 与「内容生产者」之间的线路层：

- 帧格式与 POD 约束
- 角色与权限
- 消息表与 payload
- 时序（启动、输入、关闭、崩溃、外壳重启）
- 过期与超时
- 失败模式
- 版本演进

不定义：谁应该拥有哪个状态（见进程模型文档）、跨端协议（见 `design-vm-draw-protocol.md`）。

一句话：**WMS 是唯一拥有桌面状态的进程，内容生产者只回自己的命令数组。**

## 2. 传输

**TCP `127.0.0.1`，WMS 监听 `主端口 + 1`。**

- 复用已有 `ws2_32` socket 代码与 `vm_server` 的事件循环，不引入新依赖。
- 内容生产者启动后**主动连回来**，所以 WMS 不需要知道怎么启动程序。
- 一连接一进程，断开即隔离边界。
- IPC 的监听 fd 与连接 fd 必须并进 `vm_server` 的同一个 `select` 循环（`vm_server.c:958`），不另开线程；理由见进程模型文档第 9 节。

不选命名管道：Windows 专有，IOCP 模型要另写一套，收益只有几微秒延迟。

## 3. 帧格式与 POD 约束

```text
VmIpcHeader { magic(4) type(2) flags(2) len(4) }  = 12 B
payload                                            = len B
```

- payload 全是定长 POD，内联数组、无指针，可直接 `memcpy`。
- 变长尾随（命令数组、窗口表、进程表）跟在定长头之后，条数写在定长头里。
- `vm_ipc.h` 用 `_Static_assert` 守护关键结构尺寸。指针被打包成垃圾发到对端，症状是对方崩在随机地址，很难查，所以必须在编译期挡住。
- `len` 超过 `VM_IPC_MAX_PAYLOAD`（新增，暂定 768 KB）直接断连：说明对端版本错或恶意，不能按 `len` 分配。

命令数组的序列化直接复用 `VmDrawCommand` 的内存布局（`vm_display.h:141-156`），不做二次编码。因此**两端的 `vm_display.h` 必须同版本编译**，见第 10 节。

## 4. 角色与权限

`HELLO.role` 在连接建立时声明，之后不可变更。

| role | 值 | 能力 |
|---|---|---|
| `APP` | 0 | 只能操作自己名下的窗口：CREATE / DESTROY / RETITLE / CONTENT / INVALIDATE |
| `SHELL` | 1 | APP 的全部能力 + 全局窗口枚举 + 全局窗口控制 + 工作区声明 + 主题切换通知 |
| `PROCMAN` | 2 | APP 的全部能力 + 进程表查询 + 杀进程 |

权限校验在 WMS 侧按 `role` + 「这个 winId 是否属于本连接」双重判断。越权请求静默忽略并写日志，**不断连**：断连会让一个越权 bug 变成「应用莫名退出」，现场信息全丢。

`role` 只做能力位，不做身份认证。真正的鉴权（谁可以冒充 SHELL）不在本次范围。

## 5. 消息

### 5.1 内容生产者 → WMS

| 消息 | payload | 角色 | 说明 |
|---|---|---|---|
| `HELLO` | `VmIpcHello` | 全部 | 注册：`appId` / `name` / `pid` / `role` |
| `WINDOW_CREATE` | `VmIpcWindowCreate` | 全部 | 建窗口（几何 + 标题 + flags） |
| `WINDOW_DESTROY` | `VmIpcWindowId` | 全部 | 主动关闭自己的窗口（进程还可能继续运行） |
| `WINDOW_RETITLE` | `VmIpcWindowRetitle` | 全部 | 改自己窗口的标题 |
| `WINDOW_CONTENT` | `VmIpcWindowContent` + `VmDrawCommand[count]` | 全部 | 回布局结果 |
| `INVALIDATE` | `VmIpcWindowId` | 全部 | 请求重跑该窗口的 layout |
| `WINDOW_ENUM_REQ` | 空 | SHELL | 要全量窗口快照 |
| `WINDOW_CTRL` | `VmIpcWindowCtrl` | SHELL | 全局窗口控制（激活/最小化/关闭…） |
| `WORKAREA_SET` | `VmIpcWorkArea` | SHELL | 声明工作区（画布减去任务栏） |
| `THEME_SET` | `VmIpcThemeSet` | SHELL | 主题已切换，通知 WMS 重新解析与下发 |
| `PROC_LIST_REQ` | 空 | PROCMAN | 要进程表 |
| `PROC_KILL_REQ` | `VmIpcWindowId`（复用 `appHandle`） | PROCMAN | 杀进程 |
| `BYE` | 空 | 全部 | 正常退出 |

### 5.2 WMS → 内容生产者

| 消息 | payload | 目标 | 说明 |
|---|---|---|---|
| `HELLO_ACK` | `VmIpcHelloAck` | 全部 | 分配 `appHandle`，给屏幕尺寸、工作区、超时参数 |
| `LAYOUT_REQ` | `VmIpcLayoutReq` | 全部 | 「用这个 body 尺寸跑一次布局」 |
| `INPUT` | `VmIpcInput` | 全部 | 指针 / 滚轮 / 键盘（窗口局部坐标） |
| `WINDOW_CLOSED` | `VmIpcWindowId` | 全部 | WMS 已移除该窗口，不要再引用 |
| `WINDOW_ENUM_REPLY` | `VmIpcWindowEnumReply` + `VmIpcWindowInfo[count]` | SHELL | 窗口快照 |
| `WINDOWS_CHANGED` | 空 | SHELL | 「窗口集合或状态变了，来取快照」 |
| `THEME_CHANGED` | `VmIpcThemeSet` | 全部 | 主题已切换，各自重新解析 |
| `PROC_LIST_REPLY` | `VmIpcProcListReply` + `VmIpcProcRow[count]` | PROCMAN | 进程表 |
| `SHUTDOWN` | 空 | 全部 | WMS 正在退出，收到后自行退出 |

### 5.3 关键决策：事件推送用「变了就来取」而不是增量

`WINDOWS_CHANGED` 只是一个空 payload 通知，外壳收到后发 `WINDOW_ENUM_REQ` 重取全量。理由：

- 状态合并逻辑只在 WMS 一处，外壳不做增量合并，不会出现「漏一个事件导致条目永久错」。
- 同一轮 `select` 里发生多次变化只发一次，天然合并了事件风暴。
- 代价是一次往返 + 约 1.1 KB（10 个窗口），本机回环无压力。

同理，`THEME_SET` / `THEME_CHANGED` 只传主题名，不传 token 表；各进程自己解析主题目录（决策理由见进程模型文档 5.2）。

### 5.4 新增结构

```c
/* 窗口快照的一行。ENUM_REPLY 尾随 VmIpcWindowInfo[count]。 */
typedef struct {
    int32_t winId;
    int32_t appHandle;
    char title[VM_IPC_MAX_TITLE];
    int32_t x, y, w, h;
    int32_t z;
    int32_t createdSeq;     /* 创建序：raises 不改，外壳需要稳定顺序 */
    uint32_t flags;         /* VM_WIN_* 位或 */
    int32_t focused;
    int32_t minimized;
    int32_t maximized;
} VmIpcWindowInfo;          /* 112 B */

typedef struct {
    int32_t count;
    int32_t reserved;
} VmIpcWindowEnumReply;     /* 8 B + count × 112 B */

/* 原子动作，不复合：外壳自己组合成「点任务栏条目」的语义，
   这样外壳改行为不需要改 WMS。 */
typedef enum {
    VM_IPC_CTRL_FOCUS = 0,
    VM_IPC_CTRL_RESTORE,
    VM_IPC_CTRL_MINIMIZE,
    VM_IPC_CTRL_MAXIMIZE,
    VM_IPC_CTRL_UNMAXIMIZE,
    VM_IPC_CTRL_CLOSE       /* 走窗口的关闭请求回调，与装饰 × 同一条路径 */
} VmIpcCtrlAction;

typedef struct {
    int32_t winId;
    int32_t action;
} VmIpcWindowCtrl;          /* 8 B */

typedef struct {
    int32_t x, y, w, h;
} VmIpcWorkArea;            /* 16 B */

typedef struct {
    /* 主题目录名，空 = 默认主题。复用 appId 的 32 字节容量：
       主题目录名与 appId 同量级，需要更长的名字时再开独立常量。 */
    char name[VM_IPC_MAX_APPID];
} VmIpcThemeSet;               /* 32 B */
```

`VmIpcHello` 把原有的 `reserved` 字段换成语义明确的 `role`：

```c
typedef struct {
    char appId[VM_IPC_MAX_APPID];
    char name[VM_IPC_MAX_NAME];
    int32_t pid;
    int32_t role;       /* VmIpcRole：0 APP / 1 SHELL / 2 PROCMAN */
} VmIpcHello;           /* 104 B，尺寸不变（原 reserved 字段换语义） */
```

> 原结构里 `pid` 后面就是 `reserved`，直接把它改成 `role` 即可，**尺寸与 `_Static_assert` 都不用动**。预保留字段在这里正好派上用场。

### 5.5 「写」的归属

窗口属性的写操作只有窗口所有者能发起：`WINDOW_RETITLE`（标题）、`WINDOW_CREATE` 时定的几何。外壳没有写权限，只有控制权（`WINDOW_CTRL`）。

理由：权限面。让外壳能改任何窗口的标题/几何，等于给外壳一个「静默篡改他人窗口」的能力，而它真实需求只有「激活/最小化/关闭」。移动窗口由 WMS 的拖拽路径处理，不需要外部 API。

## 6. 时序

### 6.1 启动

```text
app/producer                  wms
 |--- HELLO(role) ------------->|   建 app 记录，分配 handle
 |<-- HELLO_ACK ----------------|   屏幕尺寸 + 工作区 + 超时
 |--- WINDOW_CREATE ----------->|   建窗口，标脏
 |                              |   (帧循环发现脏)
 |<-- LAYOUT_REQ(epoch=1) ------|
 |--- WINDOW_CONTENT(epoch=1) ->|   存进 layer.commands
 |                              |   (下一帧合成，内容可见)
```

**首帧有一帧延迟。** 这是设计选择，不是缺陷：WMS 发完 `LAYOUT_REQ` 立刻继续合成，不会等待。

外壳的连接额外三步：`WINDOW_ENUM_REQ` → `WORKAREA_SET` → `WINDOW_CREATE`（任务栏窗口）。

### 6.2 输入

```text
输入事件 → WMS 命中测试 + 焦点仲裁（它才知道 z 序）
        → INPUT 发给命中窗口所属的连接
生产者处理控件 → 状态变了就 INVALIDATE
        → WMS 标脏 → LAYOUT_REQ → CONTENT → 下一帧
```

焦点仲裁、拖动、最小化、z 序**全部留在 WMS**，生产者不参与。生产者只知道「我的某个窗口被点了 / 被按了键」。

### 6.3 关闭与崩溃

- 生产者发 `BYE` 或直接断开 → WMS 移除该连接名下的**所有**窗口，标脏，从进程表摘除，给外壳发 `WINDOWS_CHANGED`。
- WMS 决定关闭某窗口（外壳点了 ×）→ 发 `WINDOW_CLOSED`，窗口立即从合成里消失。
- **TCP 断开是唯一真相**：不做「应用假装还活着」的软状态。

### 6.4 外壳重启

```text
外壳断开 → WMS 移除任务栏窗口，工作区保留最后值（不退回全屏）
外壳重启 → HELLO(SHELL) → WINDOW_ENUM_REQ → 用快照重建条目
        → WORKAREA_SET → WMS 恢复最大化窗口几何
```

全量枚举是必需能力，事件推送只是优化：否则外壳离线期间发生的窗口变化会永久丢失。

### 6.5 主题切换

```text
外壳（设置面板）→ THEME_SET(name) → WMS
WMS：解析新主题 → VmTheme_AppendResources → 广播资源包给浏览器
     → 给所有连接发 THEME_CHANGED(name)
各生产者：重新解析主题目录 → 下一帧用新色画
```

可能出现「生产者已用新主题画完，浏览器还在拉旧资源包」的短暂不一致。可接受（一帧级）。若实测可感知，在 `WINDOW_CONTENT` 之外再加主题代次号，让 WMS 丢弃旧代次内容。

## 7. 三个关键决策（沿用 2026-11-22 版，仍然有效）

**7.1 异步内容拉取，WMS 从不阻塞等生产者**

`LAYOUT_REQ` 发出后本帧用**旧内容**继续合成。这是「一个应用卡住不拖垮桌面」的全部实现——不是靠超时检测，而是靠根本不等待。单线程 `select` 循环让这条成为结构性保证，而不是纪律要求。

**7.2 `epoch` 丢弃过期回包**

快速 resize 时会有多个 `LAYOUT_REQ` 在途。`WINDOW_CONTENT` 带回 `epoch`，与窗口当前 `epoch` 不符就丢弃，避免旧尺寸的内容盖上新尺寸。

**7.3 超时只影响显示，不影响正确性**

超过 `VM_IPC_LAYOUT_TIMEOUT_FRAMES`（30 帧 ≈ 0.5 s）没回包，窗口标记 `VM_IPC_PROC_UNRESPONSIVE`，进程表显示「未响应」，内容仍是上一份。用户可以决定是否杀掉它。

## 8. 失败模式

| 现象 | 处理 |
|---|---|
| 生产者崩溃 / 断连 | 移除其全部窗口，标脏，进程表更新，`WINDOWS_CHANGED` 给外壳 |
| 生产者卡在 layout | 旧内容继续显示，30 帧后标记未响应；桌面不受影响 |
| 回包越界 / `magic` 错 / `len` 超上限 | 丢弃该连接（视为恶意或版本不匹配），不影响其他连接 |
| `WINDOW_CONTENT.count` 超配额 | 截断到 `VM_IPC_MAX_CONTENT_COMMANDS`，置 `overflow`，日志告警 |
| 越权请求（APP 调 SHELL 消息） | 静默忽略 + 日志，不断连 |
| 未知 `type` | 忽略该消息 + 日志，不断连（见第 10 节） |
| WMS 退出 | 广播 `SHUTDOWN`，各连接自行退出 |
| `WINDOW_CTRL` 指向已关闭窗口 | 静默忽略 + 日志（与「断开是唯一真相」一致） |

## 9. 容量预算

| 项 | 值 | 依据 |
|---|---|---|
| 单连接窗口数 | 8 | `VM_IPC_MAX_WINDOWS_PER_APP`，与 `VM_WM_MAX_WINDOWS` 留出外壳余量 |
| 单次内容命令数 | 1024 | `VM_IPC_MAX_CONTENT_COMMANDS = VM_WM_LAYER_CAPACITY` |
| 单条命令 | 496 B | `sizeof(VmDrawCommand)`，`test_frame.c:531` 钉着 ≤ 512 |
| 一个脏窗口典型回包 | ≈ 15 KB | 30 条 × 496 B |
| 60 Hz 满打满算 | ≈ 900 KB/s | 本机回环无压力 |
| 窗口快照 | 8 + 10 × 112 = 1128 B | `VM_WM_MAX_WINDOWS = 10` |
| 最大连接数 | 16 | `VM_IPC_MAX_APPS`，含外壳与 ProcMan |

## 10. 版本演进

- **`magic` 错 → 断连**：版本不匹配或非本协议流量。
- **未知 `type` → 忽略该消息 + 日志，不断连**：这样加新消息不需要同时升级两端，先升级的一方不会打掉旧对端。
- **结构体尺寸变化 → 必须同步升级两端**：`VmDrawCommand` 直接按内存布局传输，不同尺寸会让 `len` 与实际解释不一致。`_Static_assert` 会先在编译期挡住最常见的情况。
- 协议不携带版本号字段：`flags` 保留给将来的版本协商，当前必须为 0。

## 11. 落地状态与依赖

| 项 | 状态 |
|---|---|
| `vm_ipc.h`（协议头、消息枚举、结构体、尺寸断言） | 已存在（2026-11-22），本次新增 7 条消息与 6 个结构（含 1 个动作枚举） |
| `vm_ipc.c`（帧编解码、连接包装） | **不存在**，需新写 |
| 消息集合的收发实现 | 随 WMS broker 与外壳/应用适配一起做 |

分步顺序、验收判据、可执行体划分见 `design-vm-wms-process.md` 第 10、11 节。本文不重复，避免两处维护。

## 12. 暂不做

- 跨进程剪贴板（`VmClipboard` 是进程内静态，需要上移到 WMS）。
- 应用私有资源注册（`RES_REGISTER`）：M1 阶段只有 WMS 生成资源包。
- 应用间拖放、单实例、启动器、图标注册。
- 鉴权与沙箱（`role` 只是能力位）。
- 协议压缩、批量合并、心跳保活（`select` 超时就是帧节拍）。

## 13. 未决问题

- **`WINDOW_ENUM_REPLY` 的分片**：10 个窗口 1128 B 不需要分片，但如果将来窗口数上限提高，需要定义「多帧回包 + more 标志」。当前不做。
- **外壳的 z 序稳定性**：任务栏与开始菜单都是 `TOPMOST`，先后由创建序决定；外壳重启后创建序会变。需要在 WMS 侧给外壳窗口一个稳定优先级，而不是依赖创建序（决策与讨论见 `design-vm-wms-process.md` 第 13 节）。
- **`WINDOW_CONTENT` 的坐标系校验**：协议要求坐标是画布绝对坐标（与 `layer.commands` 语义一致），但 WMS 无法低成本校验。防不了恶意客户端画到别人的窗口上；单机自用可接受，写在这里备查。
