# 任务：多客户端彩色指针与资源命令

> 历史说明：本文验证的是已移除的 .NET/WinForms WebSocket 多客户端指针通道，相关 `ConsoleApp2` / `TestHarness` 描述均已过期。当前多客户端指针由 `vm-desktop/vm_wm.c` 在 guest 内合成，见 `docs/arch-vmdesk.md`。

## 目标

1. 前端新增资源命令 `loadImg` / `drawImg`：图片加载完成后，带 `imgId` 的 `drawImg` 才能绘制；未加载完时静默跳过，不影响既有控件指令流。
2. 用不同颜色表示不同浏览器客户端的鼠标，并让各客户端看到彼此的鼠标。
3. 不破坏现有 60fps 控件指令流、签名去重、脏矩形增量重绘、事件回传和远程指针层。

## 已确认的方案决策

- **鼠标走独立通道，不进现有 `applyFrame` 主帧**。新增第二个 `<canvas id="cursorCanvas">`（绝对定位、`pointer-events:none`），由 `{"type":"cursors"}` 消息驱动，在主 rAF 中绘制，但不参与 `lastFrameSignature` / `lastCommands` diff。理由：高频鼠标会污染帧签名和脏矩形，破坏增量重绘。
- **保留 `CursorOverlayPanel` 作为宿主本机指针层**（方案 A），不改造为多光标。本期重点是浏览器客户端互见。
- **资源用单张白色箭头 SVG + 前端按 `tint` 染色**，服务端在 welcome 时下发 dataUrl，只传一次。颜色调色板在服务端分配。
- 上行光标消息独立绑定，不经过现有 `handleEvent`（避免被 `isUpdating` 丢弃）。

## 协议设计

### 下行

- `{"type":"welcome","clientId":"...","color":"#...","cursorImage":"data:image/svg+xml;base64,..."}`
  - 建连后立即发送。`clientId` 为本连接唯一标识，`color` 为该客户端指针颜色。
  - 前端收到后调用 `loadImg('cursor', cursorImage)`。
- `{"type":"cursors","list":[{"clientId":"...","color":"#...","x":0,"y":0,"visible":true},...]}`
  - 服务端 60fps 定时广播快照，内容无变化时跳过发送。
- 既有控件帧（JSON 数组）保持原样。`loadImg` / `drawImg` 同时注册进 `commandHandlers`，以便通用指令流也能调用；`drawImg` 的目标 context 由前端指向 `cursorCanvas`。

### 上行

- `{"type":"cursor","clientId":"...","x":0,"y":0,"visible":true}`
  - 前端 mousemove 以 33ms 节流上报，坐标换算为 canvas 内部坐标系（与主 canvas 指令坐标一致）。
  - 服务端用连接自身的 `ClientContext.Id` 覆盖消息中的 `clientId`，避免伪造。
  - 该消息不进入 `DomEvent` 反序列化路径。

## 改动点

- `ConsoleApp2/app.js`：新增 `loadImg` / `drawImg` handler、图片缓存、按颜色染色缓存、`setDrawImgTarget()`。
- `ConsoleApp2/main.js`：消息分流（welcome / cursors / 控件帧）、独立光标上报、rAF 中绘制指针层。
- `ConsoleApp2/index.html`：新增 `cursorCanvas`。
- `ConsoleApp2/ServerWebSocketImpl.cs`：`ClientContext` 增加 `Id`/`Color`/`CursorX`/`CursorY`/`CursorVisible`/`LastCursorTime`；建连发 welcome；`ProcessMessageAsync` 分流 cursor 消息；新增 `BroadcastCursorsAsync()`。
- `ConsoleApp2/ServerWebSocket.cs`：暴露 `BroadcastCursorsAsync()` 给控制器。
- `ConsoleApp2/SurfaceController.cs`：60fps 定时器中调用光标广播。
- `ConsoleApp2/RemoteForm.csproj`、`TestHarness/TestHarness.csproj`：前端文件已是逐项列出，无需新增文件项（只改已有 `index.html`/`app.js`/`main.js`）。

## 步骤

- [x] 写本任务文档
- [x] 实现前端 `loadImg` / `drawImg` 与指针 canvas
- [x] 实现 welcome / cursors 消息分流与光标上报
- [x] 实现服务端 ClientContext、welcome、光标分流与广播
- [x] 在 60fps 定时器中驱动广播
- [x] 增加发送锁，避免 welcome 与光标快照在同一 socket 并发发送
- [x] `dotnet build` 两个项目通过
- [x] 浏览器验证：双客户端互见、既有点击回传
- [x] 更新 `docs/INDEX.md`、`docs/arch-vmdesk.md`，结案

## 验证结果（2026-11-21）

- 构建：`dotnet build ConsoleApp2/RemoteForm.csproj` 与 `dotnet build TestHarness/TestHarness.csproj` 均成功，0 错误；4 个警告均为改动前既有（`LinkClick`、`_stopPropagation`、`_stopImmediatePropagation`、`lastMessage`）。
- welcome：测试台下两个浏览器客户端分别收到 `#F44336` 与 `#2196F3`，并在 console 打印 `welcome client <id> <color>`，无错误日志。
- 服务端广播：Node 原生 WebSocket 探针确认 welcome 后持续收到 `{"type":"cursors","list":[...]}` 快照；上报后对应条目 `visible:true`，坐标正确。
- 双端互见：A 持续上报时，B 的 `cursorCanvas` 出现 234 个非透明像素，取样 rgb≈(245,71,51) 即 A 的红色；B 持续上报时，A 的 `cursorCanvas` 出现 234 个蓝色像素，即 B 的 `#2196F3`。各自跳过自己的光标。
- 既有逻辑：A 派发按钮点击后，按钮区域 `(30,290,160,42)` 像素和由 `4889295` 变为 `4873038`，说明回传仍生效；控件帧持续推送。
- 3 秒未上报的光标会置为 `visible:false`（设计行为）；单次合成 mousemove 因验证间隔超过 3 秒会消失，需用持续上报验证。
- 未单独构造“`loadImg` 加载失败 / 未完成时 `drawImg`”的运行时场景；该分支由 `getTintedImage()` 返回 `null` 时静默 `return` 保证，属代码路径确认，非运行时验证。

## 验证方式

- 构建：`dotnet build ConsoleApp2/RemoteForm.csproj`、`dotnet build TestHarness/TestHarness.csproj`。
- 运行 `TestHarness.exe`，浏览器打开 `http://localhost:8080/index.html`。
- 用 `agent-browser` 打开两个页面，分别派发 `mousemove`（`clientX/clientY` 为设计坐标 / 1.5），检查两个页面的 `cursorCanvas` 是否都出现两种颜色的箭头。
- 检查控制台无 `drawImg` 未加载异常；控件点击回传仍正常（沿用 `docs/debug-vmdesk.md` 的点击验证法）。
- 验证时间预估：构建 2 次约 20 秒，浏览器验证约 3 分钟。

## 恢复点

若会话中断，从上述步骤勾选处继续。所有协议常量集中在 `ServerWebSocketImpl.cs` 与 `main.js` 顶部，避免散落。

## 已知限制（本期不处理）

- 客户端断开后其他客户端的光标在下一次 60fps 广播中移除，最长约 16ms。
- 不缓存远端鼠标图片到磁盘，每次建连随 welcome 下发一次 SVG dataUrl。
- 宿主 Win32 指针层仍为单色系统箭头，不随客户端颜色变化。
