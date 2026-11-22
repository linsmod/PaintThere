# WMS 与应用进程的线路协议

状态：设计冻结（2026-11-22）。协议常量在 `vm-desktop/vm_ipc.h`。

## 1. 为什么现在做

ProcMan 是第一个需要「**进程级视图**」的程序：它要列出桌面上有哪些程序、各自占多少内存、能不能杀掉。

只要窗口还由 WMS 单进程持有，就没有「进程」可管——`VmWindow` 只有 `id/title/矩形`，没有归属。所以 ProcMan 的试金石性质是：**它逼出进程边界**，而不是再写一个窗口。

## 2. 责任边界

| 角色 | 拥有 | 明确不做 |
|---|---|---|
| **WMS** | 窗口几何、z 序、焦点、装饰、合成、进程表、生命周期 | 不跑 Clay、不生成窗口内容、不理解控件 |
| **应用进程** | 自己的状态、Clay 布局、命令生成 | 不碰画布、不碰 tile、不碰别人的窗口 |
| **浏览器** | 呈现 | — |

一句话：**WMS 是唯一拥有「桌面状态」的进程，应用只拥有「自己的内容」。**

这与 S1 的两阶段 `BuildFrame` 完全对齐：阶段 1（`build_one_window`）挪到应用进程，阶段 2（按 z 序合并）留在 WMS。

## 3. 传输

**TCP `127.0.0.1`，WMS 监听 `主端口 + 1`。**

- 复用已有 `ws2_32` socket 代码与 `vm_server` 的事件循环，不引入新依赖。
- 应用启动后**主动连回来**，所以 WMS 不需要知道怎么 spawn 程序；启动器负责启动进程并传端口。
- 一连接一进程，断开即隔离边界。

不选命名管道：Windows 专有，IOCP 模型要另写一套，收益只有几微秒延迟。

带宽估算：一个脏窗口约 30 条 × 496 B ≈ **15 KB**；60 Hz 满打满算 900 KB/s，对本地回环无压力。

## 4. 帧格式

```
VmIpcHeader { magic(4) type(2) flags(2) len(4) }  = 12 B
payload                                            = len B
```

payload 全是定长 POD，内联数组、无指针，可直接 `memcpy`。变长尾随（命令数组、进程表）跟在定长头之后，条数写在定长头里。

`vm_ipc.h` 里有 `_Static_assert` 守护关键结构尺寸——指针被打包成垃圾发到对端，症状是对方崩在随机地址，很难查，所以在编译期挡住。

## 5. 消息

### 应用 → WMS

| 消息 | payload | 说明 |
|---|---|---|
| `HELLO` | `VmIpcHello` | 注册：`appId` / `name` / `pid` |
| `WINDOW_CREATE` | `VmIpcWindowCreate` | 建窗口（几何 + 标题 + flags） |
| `WINDOW_DESTROY` | `VmIpcWindowId` | 主动关闭窗口（应用还可能继续运行） |
| `WINDOW_RETITLE` | `VmIpcWindowRetitle` | 改标题 |
| `WINDOW_CONTENT` | `VmIpcWindowContent` + `VmDrawCommand[count]` | 回布局结果 |
| `INVALIDATE` | `VmIpcWindowId` | 请求重跑 layout |
| `PROC_LIST_REQ` | 空 | 要进程表（仅 ProcMan） |
| `PROC_KILL_REQ` | `VmIpcWindowId`（复用 `appHandle`） | 杀进程（仅 ProcMan） |
| `BYE` | 空 | 正常退出 |

### WMS → 应用

| 消息 | payload | 说明 |
|---|---|---|
| `HELLO_ACK` | `VmIpcHelloAck` | 分配 `appHandle`，给屏幕尺寸与超时参数 |
| `LAYOUT_REQ` | `VmIpcLayoutReq` | 「用这个 body 尺寸跑一次布局」 |
| `INPUT` | `VmIpcInput` | 指针 / 滚轮 / 键盘（窗口局部坐标） |
| `WINDOW_CLOSED` | `VmIpcWindowId` | WMS 已移除该窗口，应用不要再引用 |
| `PROC_LIST_REPLY` | `VmIpcProcListReply` + `VmIpcProcRow[count]` | 进程表 |
| `SHUTDOWN` | 空 | WMS 正在退出 |

## 6. 时序

### 启动

```
app                          wms
 |--- HELLO ------------------>|   建 app 记录，分配 handle
 |<-- HELLO_ACK ---------------|
 |--- WINDOW_CREATE ---------->|   建窗口，标脏
 |                             |   (帧循环发现脏)
 |<-- LAYOUT_REQ(epoch=1) -----|
 |--- WINDOW_CONTENT(epoch=1)->|   存进 layer.commands
 |                             |   (下一帧合成，内容可见)
```

**首帧有一帧延迟。** 这是设计选择，不是缺陷：WMS 发完 `LAYOUT_REQ` 立刻继续合成，不会等待。

### 输入

```
输入事件 → WMS 命中测试 + 焦点仲裁（它才知道 z 序）
        → INPUT 发给命中窗口所属的 app
app 处理控件 → 状态变了就 INVALIDATE
        → WMS 标脏 → LAYOUT_REQ → CONTENT → 下一帧
```

焦点仲裁、拖动、最小化、z 序**全部留在 WMS**，应用不参与。应用只知道「我的某个窗口被点了/被按了键」。

### 关闭与崩溃

- 应用发 `BYE` 或直接断开 → WMS 移除该 app 的**所有**窗口，标脏，从进程表摘除。
- WMS 决定关闭某窗口（如未来的「强制关闭」）→ 发 `WINDOW_CLOSED`，窗口立即从合成里消失。
- **TCP 断开是唯一真相**：不做「应用假装还活着」的软状态。

## 7. 三个关键决策

**7.1 异步内容拉取，WMS 从不阻塞等应用**

`LAYOUT_REQ` 发出后本帧用**旧内容**继续合成。这是「一个应用卡住不拖垮桌面」的全部实现——不是靠超时检测，而是靠根本不等待。

**7.2 `epoch` 丢弃过期回包**

快速 resize 时会有多个 `LAYOUT_REQ` 在途。`WINDOW_CONTENT` 带回 `epoch`，与窗口当前 `epoch` 不符就丢弃，避免旧尺寸的内容盖上新尺寸。

**7.3 超时只影响显示，不影响正确性**

超过 `VM_IPC_LAYOUT_TIMEOUT_FRAMES`（30 帧 ≈ 0.5 s）没回包，窗口标记 `VM_IPC_PROC_UNRESPONSIVE`，ProcMan 显示「未响应」，内容仍是上一份。用户可以先杀它。

## 8. ProcMan 接口

ProcMan 是普通应用（走同一套连接），额外有两条消息：

- `PROC_LIST_REQ` → `PROC_LIST_REPLY`：返回每个应用的 `appHandle / pid / winCount / memKB / cpuPermille / state / appId / name`。
- `PROC_KILL_REQ(appHandle)` → WMS `TerminateProcess` + 清理窗口 + 从表里摘除。

权限：协议层暂不做鉴权，注释标注「仅 ProcMan 使用」。真要收紧时，在 `HELLO` 里加 capability 位，WMS 按位校验。

## 9. 失败模式

| 现象 | 处理 |
|---|---|
| 应用崩溃 / 断连 | 移除其全部窗口，标脏，进程表更新 |
| 应用卡在 layout | 旧内容继续显示，30 帧后标记未响应 |
| 应用回包越界 / magic 错 | 丢弃该连接（视为恶意或版本不匹配），不影响其他应用 |
| `count` 超过配额 | 截断到 `VM_IPC_MAX_CONTENT_COMMANDS`，置 `overflow`，日志告警 |
| WMS 退出 | 广播 `SHUTDOWN`，各应用自行退出 |

## 10. 落地步骤

| 步 | 内容 | 验收 |
|---|---|---|
| **I0 清指针** | `args.paint` 改内联数组（已完成）；`defineProgram.commands` 由编码层特殊处理 | `sizeof(VmDrawCommand)` 不变、帧 JSON 逐字节相同 |
| **I1 协议头** | `vm_ipc.h`（已完成）+ 编解码 `vm_ipc.c` | 往返编解码单测 |
| **I2 WMS broker** | 监听、per-app 连接、app 表、窗口归属、`LAYOUT_REQ` 派发、内容回填 | 手工 telnet 式脚本能注册并出窗 |
| **I3 jpx 侧适配** | `Jpx_Create` 增加「远程模式」：连 WMS，收 `LAYOUT_REQ` 跑本地 Clay，回 `CONTENT` | `demo_jpx` 以独立进程跑通两个窗口 |
| **I4 ProcMan** | 走 `PROC_LIST` / `PROC_KILL` | 列出两个 demo 进程并能杀掉其中一个 |

I3 是整个方案的风险点：它要把 `JpxApp` 从「持有 `VmWm`」改成「远程 WMS 的客户端」，`VmWm` 在应用侧退化成「窗口 id → Clay context」的本地表。

## 11. 暂不做

- 跨进程剪贴板（`VmClipboard` 现在是进程内静态，需要上移到 WMS 才谈得上应用间复制）
- 资源/`defineProgram` 的跨进程下发（应用与浏览器之间的资源通道暂由 WMS 转存）
- 应用间拖放、单实例、启动器
- 鉴权与沙箱
