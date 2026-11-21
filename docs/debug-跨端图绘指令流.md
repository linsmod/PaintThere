# 跨端图绘指令流 · 调试经验

最后核对：2026-11-21

## 启动与验证

- 主程序：`dotnet build ConsoleApp2/RemoteForm.csproj` 后运行输出目录的 `RemoteForm.exe`。
- 测试台：`dotnet build TestHarness/TestHarness.csproj`，运行 `TestHarness.exe`。它通过 csproj 链接主项目核心源码，用独立的 `TestForm` 验证，不依赖任何业务窗体。
- 浏览器打开 `http://localhost:8080/index.html`，8081 为 WebSocket。
- 用浏览器自动化派发合成事件验证回传：
  - 点击按钮：`canvas.dispatchEvent(new MouseEvent('click',{clientX:73,clientY:207,bubbles:true,button:0}))`，按钮在 WinForms 客户区 (30,290,160x42)，浏览器坐标 = 设计坐标 / 1.5。
  - hover：`mousemove` / `mouseenter` / `mouseleave` 同理。
  - 验证方式：对比按钮区域 `getImageData` 的像素和（文字变化）或取样点颜色（悬停变色）。

## 深挖过的坑与结论

- **首帧不推送**：`paintCommands.SequenceEqual(this.commands)` 在 `commands` 为 null 时抛异常并被 catch 吞掉，导致永远不发指令。必须判空。
- **旧协议 Loading 报错**：前端改造为结构化指令后不再直接 import 绘制函数，旧字符串指令 `eval` 找不到 `drawLabel`。eval 兼容路径需要把函数绑定在作用域内。
- **click 不触发控件处理器**：不要用 `handler.Name.Contains("Click")` 判断（lambda 名是 `<.ctor>b__0`）；也不要反射取 `MethodInfo` 后传 `form` 调用（闭包 lambda 的 Target 是编译器生成的闭包类，会报 target type 不匹配）。可靠做法是调用控件自身的 `OnClick` 虚方法，让 WinForms 分发；LinkLabel 特殊传 `LinkLabelLinkClickedEventArgs`。
- **覆盖层遮挡导致 click 失效**：给窗体加 Dock=Fill 的透明子控件后，命中测试会落到覆盖层。`HitTestRecursive` 用 `child.Bounds.Contains` 而非 `ClientRectangle.Contains`，并跳过 `OverlayPanel` 子类。
- **分层窗口位置错位**：`UpdateLayeredWindow` 的 `pptDst` 参数是窗口在屏幕上的位置，必须传宿主客户区原点 `_host.PointToScreen(Point.Empty)`；传 `(0,0)` 会把窗口挪到屏幕左上角。
- **分层窗口遮挡行为**：不要用 `WS_EX_TOPMOST`（会永远浮在最上面）。用宿主句柄作为 `CreateWindowEx` 的 owner 参数，owned window 跟随宿主 z-order，宿主被遮挡时指针也被遮挡。宿主最小化时手动 `ShowWindow(SW_HIDE)`。
- **DPI 一致**：主程序与测试台都必须调用 `SetProcessDPIAware()`，否则两套进程 DPI 感知不同导致坐标错位。当前 `dpiScale=1.5` 是设计缩放常量，未做 Per-Monitor DPI。
- **跨线程创建控件**：`GetFormID` 在后台 Timer 线程调用，创建窗口/控件必须 `form.Invoke` 切到 UI 线程，否则 `Control.ControlCollection.Add` 抛跨线程异常。
- **子控件坐标**：递归渲染时子控件 `Left/Top` 是相对父容器的，必须累加父容器偏移才是窗体绝对坐标；命中测试则相反，逐层减去父容器 Left/Top。
- **构建环境**：本机只有 .NET SDK（无 VS MSBuild），`IWshRuntimeLibrary` COMReference 无法编译，清理前主项目需要用临时桩件绕过。清理后已移除 COM 引用，可直接 `dotnet build`。
- **PowerShell 注意**：长链命令（多次 `agent-browser` + `Start-Sleep`）容易被切成后台执行；拆分短命令执行。不要在 PowerShell 用 heredoc（`<<`）。

## 环境信息

- OS：Windows，PowerShell Core。
- SDK：`dotnet` 10.0.404；目标框架 `net9.0-windows`。
- 浏览器自动化：`agent-browser 0.27.0`（需 node 可用），截图默认落在 `C:\Users\linswin\.agent-browser\tmp\screenshots\`。
- 依赖包：`Newtonsoft.Json 13.0.3`（清理后唯一 NuGet 依赖）。
