# 跨端图绘指令流 · 架构现状

最后核对：2026-11-21
！注意所有含net程序的描述均为过期。
本文描述当前实现，不记录修复历史。历史修法见 `docs/fix-*.md`（如后续建立）。项目目标是把 Windows WinForms 窗体的界面状态实时推送到浏览器 Canvas 渲染，并把浏览器的交互事件回传到 WinForms 控件，形成双向同步。

## 项目结构

- `ConsoleApp2/`：主程序。纯图绘指令流，已移除早期的 APNs 推送代理业务。
- `TestHarness/`：独立测试台。通过 csproj 链接 `ConsoleApp2` 的核心源文件，用一个干净的 `TestForm` 验证渲染、事件回传与远程指针，不依赖业务窗体。

`ConsoleApp2` 关键文件：

- `Program.cs`：进程入口。设置 DPI 感知、创建 `SurfaceController`、启动 `DemoForm`，并通过 `Load` 事件启动静态文件服务器。
- `DemoForm.cs`：演示窗体，含 Label/TextBox/CheckBox/LinkLabel/Button/嵌套 Panel 及一个 500ms 计时器，用于展示布局和增量更新。
- `SurfaceController.cs`：核心控制器。定时采样窗体、生成指令、广播；接收浏览器事件并分发到 WinForms 控件；管理远程指针层。
- `FastPaintCommandGenerator.cs`：把 WinForms 控件树直接翻译成结构化绘图指令。
- `PaintCommand.cs`：结构化指令模型。
- `ServerWebSocket.cs` / `ServerWebSocketImpl.cs`：WebSocket 服务端，负责广播指令和接收事件。
- `StaticFileServer.cs`：在 8080 端口提供前端静态文件。
- `CursorOverlayPanel.cs`：Win32 分层透明窗口，作为远程鼠标指针层。
- `OverlayPanel.cs`：透明鼠标穿透控件基类，被指令生成器跳过。
- `Utility.cs`：颜色、字体、坐标与控件查找辅助。
- `DOM/DOMEventModels.cs`、`DOM/DOMEventConverter.cs`、`DOM/DOMEventSerializer.cs`：浏览器 DOM 事件的模型、反序列化与分发。

前端静态文件（`ConsoleApp2` 与 `TestHarness` 输出目录各一份）：

- `index.html`：包含 `<canvas id="formCanvas">` 与透明覆盖的 `<canvas id="cursorCanvas">`，加载 `main.js`。
- `main.js`：WebSocket 客户端、渲染循环、事件采集与回传、结构化指令执行、脏矩形增量重绘；另处理 welcome/cursors 消息并驱动远端指针层。
- `app.js`：Canvas 绘制函数库，导出 `commandHandlers` 映射。
- `loadingSplash.js`：未连接时显示的加载动画。
- `connection.js`：断线覆盖层绘制（当前渲染循环主要用 loadingSplash）。
- `systemColors.js`：WinForms 系统色到 CSS 颜色的映射。

## 端口与进程模型

- `8080/tcp`：静态文件服务（`StaticFileServer`，`HttpListener`，仅本机 `IsLocal`）。
- `8081/tcp`：WebSocket 服务（`WebSocketServerImpl`，`HttpListener` + `AcceptWebSocketAsync`）。

启动顺序：`Program.Main` 先 `SetProcessDPIAware()`，创建 `SurfaceController`（构造函数启动 60fps 定时器和 WebSocket 后台任务），再 `Application.Run(new DemoForm())`；窗体 Load 后启动 8080 静态服务。

## 数据流

### 下行：WinForms → 浏览器（界面渲染）

1. `SurfaceController` 的 `System.Timers.Timer` 每 `1000/60` ms 触发 `DebounceTimer_Elapsed`，用 `Interlocked.Exchange` 防止重入。
2. `UpdateRemote` 取 `Utility.GetTopMostForm()` 得到当前最顶层窗体，分配 `formId`（`Guid`）。
3. `FastPaintCommandGenerator.Generate(form)` 递归遍历控件树，直接读取控件属性生成 `List<PaintCommand>`，跳过 `OverlayPanel` 子类，子控件坐标累加父容器偏移得到窗体客户区绝对坐标。
4. 与上一帧 `commands` 做 `SequenceEqual` 值比较；相同则跳过，不同则更新缓存。
5. `ServerWebSocket.SendCommandsToClient` → `WebSocketServerImpl.BroadcastAsync`：
   - `WrapWithUpdateCommands` 在首尾插入 `#beginUpdate`/`#endUpdate` 指令（携带 formId），一次 JSON 序列化。
   - 对每个客户端检查发送限流（60fps）和字节级重复（`Serialized.Equals`），通过 `SendAsync` 发送文本帧。
6. 浏览器 `main.js` 的 `ws.onmessage` 只把 `JSON.parse` 后的数组存入 `pendingFrame`；渲染循环 `requestAnimationFrame` 调用 `applyFrame` 消费。
7. `applyFrame`：
   - 用 `id:cmd:args` 拼出签名，与 `lastFrameSignature` 比较，相同则跳过。
   - 结构化指令通过 `handlers[cmd](...args)` 调用 `app.js` 的绘制函数，不使用 `eval`。旧协议（字符串数组）仍保留 eval 兼容路径。
   - 增量重绘：按 `id` 对比 `lastCommands` 与当前帧，收集变化控件（含消失控件）的 `rect`，取并集后 `ctx.clip` + `clearRect`，再执行全部指令。无脏区则不重画。

### 上行：浏览器 → WinForms（交互事件）

1. `main.js` 的 `setupEventListeners` 在 canvas 上绑定 click/dblclick/mousemove/mousedown/mouseup/mouseenter/mouseleave/mouseout/contextmenu/wheel、键盘、触摸和表单事件。
2. `handleEvent` 按事件类型做 33ms 防抖；`isUpdating` 为真时丢弃；`createDomEvent` 构造标准 DOM 事件对象（含 `clientX/clientY`、`target`、`canvasInfo` 等），附加 `winFormId` 后 `ws.send(JSON.stringify(eventData))`。
3. `WebSocketServerImpl` 收到文本帧 → `MessageReceived` → `SurfaceController.HandleWebSocketEvent`：
   - 反序列化为 `DomEvent`（`DomEventConverter` 按 `type` 选择具体子类）。
   - 校验 `domEvent.winFormId == GetFormID(currentForm)`，不匹配则丢弃。
   - `EventDispatcher.DispatchEvent` 按事件类型调用已注册 handler。
4. `HandleMouseEvent` / `HandleWheelEvent` / `HandleKeyEvent` 把坐标经 `CreateScaledPoint`（乘 `dpiScale`，当前 1.5）换算后，触发对应 WinForms 事件：
   - 点击/悬停：用 `HitTestRecursive` 跳过覆盖层找到最深的真实控件，再反射调用控件的 `OnClick`/`OnMouseEnter`/`OnMouseLeave` 虚方法，由 WinForms 分发所有订阅处理器（含闭包 lambda）。LinkLabel 传入 `LinkLabelLinkClickedEventArgs`。
   - 鼠标按下/抬起/移动/滚轮、键盘：用反射得到的 `Form.OnMouseDown` 等虚方法在窗体上触发。

### 多客户端指针通道（独立于控件指令流）

1. 建连后服务端分配 `clientId` 与调色板颜色，下发 `{"type":"welcome","clientId":...,"color":...,"cursorImage":"data:image/svg+xml;base64,..."}`；前端调用 `loadImg('cursor', cursorImage)` 加载白色箭头模板，按 `tint` 在离屏 canvas 染色并缓存。
2. 前端 `mousemove` 以 33ms 节流上报 `{"type":"cursor","clientId":...,"x":...,"y":...,"visible":true}`，独立于 `handleEvent`，不受 `isUpdating` 影响；坐标用 `canvas.getBoundingClientRect()` 换算到 canvas 内部坐标系。
3. `WebSocketServerImpl.ProcessMessageAsync` 分流 `cursor` 消息，用连接自身的 `ClientContext.Id` 覆盖消息中的 `clientId`，更新 `CursorX/CursorY/CursorVisible/LastCursorTime`，不走 `DomEvent` 反序列化。
4. `SurfaceController` 的 60fps 定时器调用 `BroadcastCursorsAsync`，向所有客户端广播 `{"type":"cursors","list":[...]}` 快照，内容无变化时跳过发送；超过 3 秒未上报的客户端置为 `visible:false`。
5. 前端 `renderCursors` 在 rAF 中清空并重绘 `cursorCanvas`，跳过自己的 `clientId`；`drawImg` 在图片未就绪时静默返回，下一帧自动补画。指针层不参与 `applyFrame` 的签名去重与脏矩形 diff。

## 关键设计点

- **结构化指令替代 eval**：下行指令是 `{id, cmd, args, rect}` 对象，`app.js` 导出 `commandHandlers` 映射直接函数调用。字符串指令仅作为旧服务端兼容保留。
- **值去重与帧限流**：服务端用 `SequenceEqual` 值比较 + 字节级 `Serialized` 比较 + 60fps 发送限流；客户端用签名比较。两端都在无变化时跳过工作。
- **脏矩形增量重绘**：指令带 `rect`，前端只对变化区域重画，减少 Canvas 全清全绘。
- **坐标缩放**：设计尺寸 1024×768 的 canvas 以 CSS `1/1.5` 缩放显示；浏览器事件坐标乘 `dpiScale=1.5` 还原为 WinForms 客户区坐标。`dpiScale` 是常量，不是系统 DPI 自动感知。
- **远程指针**：`CursorOverlayPanel` 是 Win32 分层窗口（`WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW`），以宿主窗体为 owner，去掉 `WS_EX_TOPMOST`，因此跟随窗体 z-order、被其他窗口遮挡时同步被遮挡；`UpdateLayeredWindow` 用透明位图画 `Cursors.Default` 箭头，鼠标位置由 `InvalidateCursorOverlay` 更新。宿主最小化时隐藏指针层。主程序和测试台都调用 `SetProcessDPIAware()` 保证坐标一致。
- **命中测试**：`HitTestRecursive` 从 `Controls` 末尾向前遍历，跳过 `OverlayPanel`，用 `child.Bounds.Contains(pointInParent)` 判断并递归深入，避免 Dock=Fill 的透明覆盖层拦截点击。

## 已知限制

- `dpiScale = 1.5` 为硬编码的设计缩放，与操作系统实际 DPI 比例无关；非 150% 显示缩放环境下坐标需重新校准。
- 前端 `applyFrame` 在脏区存在时仍会执行当前帧的全部指令，只是用 `clip` 限制绘制区域；并非只执行变化的那几条指令。
- `ConsoleApp2` 输出的可执行文件仍名为 `RemoteForm.exe`（项目名 `RemoteForm.csproj` 未改，避免影响 sln 引用）。
- 系统 DPI 感知由 `SetProcessDPIAware()`（System-DPI aware）提供，未实现 Per-Monitor DPI 感知。
- 自动化测试尚未建立；当前验证靠手动启动 + 浏览器交互 + 截图像素对比。相关验证方式见 `docs/debug-跨端图绘指令流.md`（若建立）。
