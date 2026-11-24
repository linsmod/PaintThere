# WMS 进程模型 · 设计记录

> **传输层部分已被取代（2026-11-24）。** 第 2 节图里的 TCP、第 4 节通道表、第 9 节的单线程 `select`，实际改为 libuv + 命名管道（决策见 `docs/task-vm-wms-merge.md` §4 D3）。进程划分、崩溃边界、归属决策仍然有效。
>
> **这不是现状。** 本文冻结于 2026-11-24 的决策讨论，只写「要改成什么」以及为什么。
> 当前实现见 `docs/arch-vmdesk.md` 与 `vm-desktop/jpx/jpx_app.c`、`vm-desktop/vm_shell.c`。
> 线路层（帧格式、消息表、时序、失败模式）见 `docs/design-vm-ipc.md`；本文只讲进程归属与边界。

## 1. 要解决的问题

两个独立动机，指向同一件事：**把桌面状态与内容生产者拆到不同进程**。

1. **ProcMan 需要进程级视图**：它要列出桌面上有哪些程序、各自占多少内存、能不能杀掉。只要窗口还由一个进程持有，就没有「进程」可管——`VmWindow` 只有 `id/title/矩形`，没有归属。
2. **外壳必须进程级解耦**：任务栏（`vm_shell`）、弹出菜单（`vm_menu`）、设置面板（`jpx_shell_settings`）当前住在应用进程里，外壳里任何一个空指针都会让整个桌面连同所有应用窗口一起消失。

现状做不到第 2 条，原因是四段硬耦合：

**（1）外壳由应用宿主创建**

```325:329:vm-desktop/jpx/jpx_app.c
app->shell = VmShell_Create(app->wm, VmTheme_Default(),
                            VmTheme_Default()->panelHeight);
```

**（2）外壳直接改窗口管理器的工作区**

```736:737:vm-desktop/vm_shell.c
/* 工作区 = 画布减去任务栏：最大化与拖动钳制都按它算。 */
VmWm_SetWorkArea(wm, 0, 0, wm->width, wm->height - height);
```

**（3）外壳直接调窗口管理器的控制 API**：`VmWm_Restore/Unmaximize/Focus/Minimize`（`vm_shell.c:497-510`）、右键菜单里的 `VmWm_RemoveWindow`（`vm_shell.c:569`）、`VmMenu_Open`（`vm_shell.c:608`）。

**（4）崩溃传播**：三者共享同一地址空间、同一个 `g_jpx` 单例、同一个 Clay 全局 current context（`Clay_SetCurrentContext` 是进程级状态）。外壳崩溃即进程崩溃。

所以「外壳崩溃不影响其他进程」在当前结构下不是缺陷修复能达成的，必须改进程模型。

## 2. 目标进程模型

```text
浏览器 / C 渲染器
   ↑ SSE（tiles + 命令流）        ← 唯一对外通道，跨端协议零改动
┌──────────────────────────────────────────────┐
│ WMS 进程（唯一拥有桌面状态）                  │
│   窗口表 / z / 焦点 / 拖拽 / 装饰 / 合成      │
│   tiles + 序列化 + HTTP server               │
│   IPC server（127.0.0.1，主端口 + 1）        │
│   不跑 Clay、不跑外壳逻辑、不理解控件         │
└──────────────────────────────────────────────┘
        ↑ IPC                        ↑ IPC
┌──────────────────┐      ┌────────────────────────────┐
│ 外壳进程          │      │ 应用进程 × N                │
│ 任务栏 / 开始菜单  │      │ Clay / XAML / HTML+CSS /    │
│ 弹出菜单 / 设置面板│      │ GTK，各自生成命令            │
│ 全局窗口视图       │      │ 只拥有自己的窗口内容         │
└──────────────────┘      └────────────────────────────┘
```

拥有关系：

| 角色 | 拥有 | 明确不做 |
|---|---|---|
| **WMS** | 窗口几何、z 序、焦点、拖动、装饰、合成、工作区、进程表、窗口归属 | 不跑 Clay、不生成窗口内容、不理解控件、不读主题包画任务栏 |
| **外壳进程** | 任务栏与菜单的内容、全局窗口视图（只读枚举 + 控制请求）、`shell.ini` 配置、主题目录解析 | 不碰画布、不碰 tile、不改自己的窗口几何 |
| **应用进程** | 自己的状态、布局、命令生成 | 不碰画布、不碰 tile、不碰别人的窗口、不做全局枚举 |
| **浏览器** | 呈现 | — |

崩溃影响矩阵（本文的核心保证）：

| 谁崩了 | 窗口内容 | 任务栏 | 其他应用 | 浏览器连接 |
|---|---|---|---|---|
| 应用进程 | 该应用的窗口消失 | 正常 | 不受影响 | 不断 |
| 外壳进程 | 保留（WMS 持有） | 消失 | 不受影响 | 不断 |
| WMS 进程 | 全丢 | 全丢 | 收到 `SHUTDOWN` 后自行退出 | 断 |

**WMS 仍是单点。** 本文不承诺 WMS 高可用；WMS 崩溃的语义是「桌面重启」，各连接方被通知后退出。写在这里是为了避免被读成「谁崩都没事」。

## 3. 三条硬规则

1. **WMS 是唯一拥有桌面状态的进程。** 窗口矩形、z、焦点、最小化、最大化、工作区只存在于 WMS。任何其他地方出现的同类状态都是缓存，且必须在收到推送后覆盖。
2. **内容生产者只拥有自己的内容。** 应用与外壳都只回「我这个窗口的命令数组」，不回窗口属性。窗口属性只能通过 `WINDOW_CTRL` 请求，由 WMS 决定是否采纳。
3. **TCP 断开是唯一真相。** 不做「应用假装还活着」的软状态。连接一断，该连接名下的全部窗口立即从合成里消失。

## 4. 通道归属

| 通道 | 谁用 | 说明 |
|---|---|---|
| SSE + POST `/input` | 浏览器 ↔ WMS | 跨端协议不变；只有 WMS 对浏览器说话 |
| TCP IPC（主端口 + 1） | WMS ↔ 应用 / 外壳 | 一连接一进程；断开即隔离边界 |
| 文件（主题目录、`shell.ini`） | 各进程各自读 | 只读共享；写只发生在各自的配置文件 |

这条表意味着：**应用进程不再有自己的 HTTP server**。现在 `JpxApp` 持有 `VmServer` 并对外提供 `/events`、`/respack/<id>`、`/auto/<op>`（`jpx_app.c:331-345`），拆进程后这三条全部归 WMS。

## 5. 三个设计难点与决策

### 5.1 工作区归属：WMS 持有，外壳告知

现状是外壳创建任务栏窗口时顺手 `VmWm_SetWorkArea`（`vm_shell.c:737`），因为任务栏高度由主题 token `panelHeight` 决定，而主题解析属于外壳那一层（`jpx_xaml_theme`）。

决策：**工作区由 WMS 持有**，外壳通过 `WORKAREA_SET` 告知需要的高度。理由：

- 外壳崩溃时，WMS 保留最后一次的工作区值，最大化窗口不会跑到屏幕外，也不会突然铺满整屏。
- 外壳未启动时，工作区默认为整个画布（`VmWm_Create` 的既有默认），桌面仍可用。

反方向（外壳持有工作区）会让「外壳崩溃」直接变成几何错乱，与本文目标冲突。

### 5.2 主题分发：各进程自己解析主题目录

`VmTheme` 有约 60 个字符串字段，外加三个 `VmTaskButtonLayer`（各 16 个 stop + 7 个颜色指针），见 `vm_theme.h:54-134`。同一份主题被三方使用：WMS 画窗口装饰（`VmWm_SetChrome`）、外壳画任务栏、应用派生出控件主题（`Jpx_SetVmTheme`）。

两个选项：

| 选项 | 做法 | 代价 |
|---|---|---|
| a) IPC 下发扁平化 token 表 | 所有 `const char*` 变定长数组，随协议演进 | 120+ 字段的定长结构会成为第二个「Clay 地位过高」式的耦合：主题一加字段，协议就得跟着改 |
| b) 各进程自己解析主题目录 | 各进程读 `assets/themes/<name>/`，各跑一遍解析器 | 代码体积（`jpx_xaml_theme.c` 约 66 KB）复制三份；解析必须确定性 |

**决策：选 b。** 主题目录是只读共享文件，解析器是纯函数，各进程结果一致。

由此产生一条硬约束：**主题解析必须是确定性纯函数**，不得依赖进程内地址、时间或未初始化内存。`jpx_xaml_theme.c` 里给 shade 兜底串用的轮换缓冲（见 `vm_theme.h:28-32` 注释）只要在进程内自洽即可，但不得让它影响「同一主题名 → 同一颜色值」。

主题切换的路径：设置面板（外壳进程）改 `shell.ini` → 外壳广播意图 → WMS 解析新主题、重新生成资源包（`VmTheme_AppendResources`）并通知浏览器重新拉取 → 同时通过 IPC 通知各应用「主题已变，重新解析」。

> 资源包（`definePattern` / `defineStyle` / 图片）始终由 WMS 生成，因为只有 WMS 对浏览器说话。见 5.3。

### 5.3 资源通道：只有 WMS 对浏览器

现在应用进程持有 `resPackJson` 并通过**自己的** `VmServer` 提供 `/respack/<id>` 给浏览器（`jpx_app.c:120-197`）。拆进程后这条通道只存在于 WMS。

决策：

- **主题资源由 WMS 统一注册**。主题是全局的，`VmTheme_AppendResources` 本来就只该有一份。
- **应用私有资源（自定义渐变样式、图片）在 M1 阶段不支持**。需要时由后续的 `RES_REGISTER`（应用把资源命令数组交给 WMS 转存）补齐，本次不做。
- 影响面比看起来小：控件配色是内联在命令里的 CSS 颜色字符串（`Jpx__Color` 解析后写进 `drawRect.paint`），不依赖资源。只有渐变皮肤与位图才需要资源。

`design-vm-ipc.md`（2026-11-22 版）第 11 节把「资源跨进程下发」列为暂不做，本文确认该限制，并把它落到「应用私有资源」这个具体范围上。

## 6. 可执行体划分

现有四个可执行体里，`demo_jpx` 一个 target 同时链接了 WM、HTTP 服务、外壳、菜单、JPX、XAML、sys、fs 全套（`CMakeLists.txt:48-78`）。拆分目标：

| 可执行 | 组成 | 依赖 Clay |
|---|---|---|
| `vm_wms`（新增） | `vm_wm` + `vm_display` + `vm_serialize_json` + `vm_tiles` + `vm_server` + `vm_font` + `vm_theme` + `vm_ipc` + `vm_sys*` | **否**（A1 之后；见第 7 节） |
| `vm_shell`（新增） | `vm_shell` + `vm_menu` + `vm_theme` + `jpx_xaml_theme`（解析）+ `vm_ipc` + 外壳自己的 surface 渲染 | 否（任务栏现在就是手绘 `VmWindowPaint`，不依赖 Clay） |
| `demo_jpx`（改为纯应用） | `jpx*` + `jpx_xaml*` + `clay_frame_adapter` + `vm_display` + `vm_ipc` + `vm_font` + `vm_theme` + `vm_sys*` + `vm_fs*` | 是 |
| `test_frame` / `test_menu` | 保持现状，覆盖 A1 之后的 surface 接口 | 目标否 |

`vm_shell` 现在直接依赖 `VmWm*` 与 `VmMenu` 共用 `VmWm`，拆出后两者都改成「IPC 客户端 + 本地 surface」，不再链接 `vm_wm`。

**外壳的窗口也是普通窗口。** 外壳通过 `WINDOW_CREATE` 建立自己的任务栏窗口，flags 为 `UNDECORATED|TOPMOST|PINNED|NOFOCUS`（与现状 `vm_shell.c:731-732` 一致）。WMS 不为外壳特化窗口管理，只按 `role` 放行全局枚举与控制。

## 7. 与 `VmSurface` 抽象的关系

`design-vm-ipc.md`（2026-11-22 版）指出 I3 的风险：

> 它要把 `JpxApp` 从「持有 `VmWm`」改成「远程 WMS 的客户端」，`VmWm` 在应用侧退化成「窗口 id → Clay context」的本地表。

这正是窗口管理需要 `VmSurface` 抽象的原因：应用侧不需要 `VmWm` 残壳，只需要「一块能产出命令的区域」列表。因此：

- **`VmSurface`（A1）是应用侧 IPC 适配的前置**。没有它，应用进程会拖着一个半死不活的 `VmWm`。
- **外壳不依赖它**：任务栏现在就是手绘 `VmWindowPaint`（`vm_shell.c:237`），可以直接迁到独立进程，不必等 A1。这是现有手绘路径的价值。

`VmSurface` 本身的设计不在本文范围，见后续 `docs/design-vm-ui-surface.md`（未写）。

## 8. 启动、握手与恢复

### 8.1 启动

**暂不做启动器。** WMS 不负责 spawn 应用；手工或脚本启动，端口由命令行传入。理由：启动器需要进程池、实例管理、图标注册，属于独立需求；先让协议跑通。

```text
1. 启动 vm_wms --port 9000        → 监听 HTTP 9000 与 IPC 9001
2. 启动 vm_shell --wms 9001      → HELLO(role=SHELL) → ENUM → WORKAREA_SET → CREATE 任务栏窗口
3. 启动 demo_jpx --wms 9001       → HELLO(role=APP) → CREATE 窗口 → 等 LAYOUT_REQ
```

WMS 在 `HELLO_ACK` 里给屏幕尺寸、工作区、布局超时帧数。

### 8.2 外壳重启

外壳崩溃时 TCP 断开，WMS 立即移除它名下的任务栏窗口；工作区保留最后值。所以重启外壳的顺序是：

```text
1. 断开 → WMS 移除任务栏窗口，workArea 保留最后值（不退回全屏）
2. 外壳重启 → HELLO → WINDOW_ENUM_REQ 拿全量快照
3. 用快照重建任务栏条目与焦点高亮，不需要应用参与
4. WORKAREA_SET 重新声明高度 → WMS 恢复最大化窗口的几何
```

**全量枚举是必需的，事件推送只是优化。** 依赖事件流重建会在「外壳不在线期间发生的窗口变化」上丢事件。

### 8.3 应用重启

应用重启后拿不到自己旧窗口的 id（WMS 已随断开清理）。应用重新 `WINDOW_CREATE` 即可，语义上等于「重新打开」而不是「恢复」。

## 9. WMS 的实现约束

**单线程事件循环。** `vm_server.c` 现在用 `select()` 单线程循环（`vm_server.c:958`），同时处理监听 socket、客户端连接与 tick。IPC 的监听与连接 fd 必须**并进同一个 select 集合**，不得另开线程。

理由：`design-vm-ipc.md` 的「WMS 从不阻塞等应用」靠的是「发完 `LAYOUT_REQ` 立刻继续合成」，一旦内容回包在另一个线程里等待，这个保证就要重新论证。单线程下「不等待」是结构性的，不是靠纪律。

`vm_server` 需要新增一个「注册额外 fd + 可读回调」的钩子（例如 `VmServer_AddFd(server, fd, onReadable, userData)`），让 IPC 层挂进去。`vm_server` 仍不认识 IPC 语义，只提供事件循环。

**帧节拍。** 每一轮 `select` 之后：先消费所有 IPC 消息（含内容回包），再判断脏、合成、切 tile、下发。顺序与现状 `frame_provider` 一致（`jpx_app.c:216-233`）。

## 10. 落地步骤

依赖关系：P0/P2 可与 A1 并行；P3 依赖 A1。

| 步 | 内容 | 验收 |
|---|---|---|
| **P0 线路层** | `vm_ipc.c`：帧编解码、消息序列化、连接包装（`vm_ipc.h` 已有协议头） | 往返编解码单测；`sizeof` 断言保持 |
| **P1 WMS broker** | 新建 `vm_wms` 可执行：`VmWm` + `VmServer` + IPC 监听 + app 表 + 窗口归属 + `LAYOUT_REQ` 派发 + 内容回填 | 脚本能 `HELLO` 注册、`CREATE` 出一个空窗口，浏览器看到装饰 |
| **P2 外壳进程** | 新建 `vm_shell` 可执行：角色 SHELL，`WINDOW_ENUM_REQ` 重建任务栏、`WINDOW_CTRL` 激活窗口、`WINDOWS_CHANGED` 收到后重取快照、`WORKAREA_SET` | 任务栏条目与焦点正确；点条目能激活窗口；右键菜单可用 |
| **P3 应用进程** | `Jpx_Create` 增加远程模式：连 WMS、收 `LAYOUT_REQ` 跑本地后端、回 `CONTENT`；`VmWm` 从应用侧移除 | `demo_jpx` 独立进程跑通全部现有窗口 |
| **P4 崩溃验证** | 按第 11 节验收判据逐条验证 | — |
| **P5 清理** | 删除 `Jpx_Shell*` 转发函数（`jpx.h:151-162`）、删除应用侧 `/auto` 依赖 | 应用源码不含 shell 接口 |

P3 是风险点：`JpxApp` 要把「持有 `VmWm`」换成「持有 surface 表 + IPC 连接」，`Jpx__Invalidate` 的窗口定位（现在靠 `wm->layoutWinId`，`jpx_app.c:27-36`）要改成 surface 自报。

## 11. 验收判据

以下全部为**可手工复现**的行为断言，不是「看起来正常」：

1. `taskkill /F /IM vm_shell.exe` → 应用窗口内容与输入继续工作，任务栏消失；重启 `vm_shell.exe` 后条目、焦点高亮、工作区正确重建。
2. `taskkill /F /IM demo_jpx.exe` → 该应用窗口消失，任务栏与其他应用不受影响，浏览器连接不断。
3. 应用卡在 layout → 旧内容继续显示，超过 `VM_IPC_LAYOUT_TIMEOUT_FRAMES`（30 帧）后进程表标记未响应；桌面不卡。
4. 应用 TCP 断开 → 窗口立即消失，无僵尸条目、无残留图层。
5. WMS 退出 → 各连接方收到 `SHUTDOWN` 后退出；浏览器结束 loading 状态。
6. 单进程模式保留可用（`Jpx_Create` 本地模式），`test_frame --probe` / `test_menu --probe` 不依赖 IPC。

第 6 条是测试可行性的底线：不能为了进程化让纯逻辑断言测试变成端到端测试。

## 12. 明确不做

- WMS 高可用与热重启（崩溃 = 桌面重启）。
- 鉴权与沙箱（`role` 只做能力位，不做身份认证）。
- 跨进程剪贴板（`VmClipboard` 仍是进程内静态，需要上移到 WMS）。
- 应用间拖放、单实例、启动器、图标注册。
- 应用私有资源注册（M1 阶段；见 5.3）。
- 应用侧的 `/auto/<op>` 调试接口：拆进程后只保留 WMS 处理的外壳与主题相关 op。

## 13. 未决问题

- **`WINDOW_CTRL` 的失败反馈**：请求激活一个已关闭的窗口时，是静默忽略还是回一个错误？暂定静默忽略 + 日志，与「TCP 断开是唯一真相」一致。
- **外壳的 z 序位置**：任务栏是 `TOPMOST`（现状），开始菜单也是 `TOPMOST`。两个 TOPMOST 窗口的先后由创建序决定，外壳重启后创建序会变。需要在 WMS 侧按 role + 用途给外壳窗口一个稳定优先级，而不是依赖创建序。待实现时定。
- **主题切换的时序**：WMS 重新生成资源包与通知应用是两条独立路径，可能出现「应用已用新主题画完，浏览器还在拉旧资源包」的短暂不一致。可接受（一帧级），但要在 `WINDOW_CONTENT` 的 epoch 之外再加一个主题代次号，避免旧主题内容盖上新主题。
