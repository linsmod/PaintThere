# 跨端图绘指令流 · 调试经验

最后核对：2026-11-23

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
- **临时诊断打印优先走 stdout**：`Start-Process -RedirectStandardError` 在 PowerShell 下没能实时拿到 `fprintf(stderr, ...)` 的输出，一度以为"回调根本没被调用"，换成 `printf` 后立刻可见。排查"代码没生效"之前，先确认观测通道本身是通的。

### 渲染分辨率与坐标（前端）

- **画布物理尺寸必须与 DPR、显示缩放脱钩**：`cv.width/height` 与每块瓦片画布按 `renderScale = clamp(zoom × devicePixelRatio, 1, 2.5)` 放大，CSS 尺寸单独按 `zoom = min(vw/800, vh/600, 1)` 设置。只设 CSS 尺寸而物理尺寸恒为 800×600，就是"整体发虚、放大后更糊"的根因。
- **文字要按物理像素光栅**：字形是 wasm 预光栅位图，用逻辑字号光栅再被 canvas 放大就会糊。`drawText` 现在按 `fontSize × renderScale` 光栅，绘制时再缩回逻辑尺寸（`drawRasterScaled`）。
- **协议坐标永远是 800×600 逻辑坐标**：输入换算必须用 `WIDTH/HEIGHT`，不能用 `cv.width`。改完物理尺寸后忘了这一步，表现为"点击位置按 renderScale 偏移"，而且越靠右下偏得越远。
- **瓦片贴屏要 1:1**：`present()` 用 `setTransform(1,0,0,1,0,0)` 后按整数坐标 `drawImage`，任何缩放都会重新引入插值。清空瓦片也要在单位变换下做，否则只清到逻辑尺寸那么大。
- 诊断：页面里 `__vmRenderer.scale()` 返回 `{cssZoom, renderScale, dpr}`，用来确认当前实际渲染倍率。

### VM 桌面（guest 侧）

- **guest 侧观测**：`JPX_TRACE=1` 启动 `demo_jpx` 后，每次输入处理与每帧合成后打印 `[wm]` 转储：窗口集合、z 序、焦点、以及每个窗口的 `cmds / dirty(原因) / built=<frame>`。`dirty=0` 且 `built` 停在旧帧号的窗口，就是“该重建却没人重建”的对象；`over={id need "title"}` 给出超配额窗口的真实需求条数。这份数据全在 guest 内存里，不要去抓跨端帧反推。
- **配额压测**：`VM_WM_LAYER_QUOTA=200` 把单窗口命令配额调小（1..1024）启动，超限窗口会整块显示纯色 + “配额超了 N 条”，用来验证告警与截断路径。
- **窗口层内容被截断会污染后续所有窗口**：`build_one_window` 把内容写进定长层缓冲，写满时末尾的 `clipPop` 丢失；这条未闭合的裁剪会让排在后面的窗口（任务栏在置顶 pass 最后合并）整片消失。现在超配额直接整段丢弃内容、改画纯色块 + "配额超了 N 条"，前缀天然零未闭合。
- **前端瓦片被污染不会自愈**：tile 差分按内容 hash 只发变化的块。某块曾渲染出未闭合裁剪后，只要它的命令不再变化就一直保持错误画面，必须重启或强制全量才恢复。改动渲染逻辑后先刷新页面再判断"改没改好"。
- **`CLAY_BORDER_ALL(w)` 隐含 `betweenChildren = w`**：Clay 会为容器的**每对相邻子元素**生成一条 `w` 像素的分隔矩形（`TOP_TO_BOTTOM` 是横线）。列表容器 368 行 → 367 条 1px 矩形，宽度等于容器宽、颜色等于 border 色，看起来像"莫名其妙的细线"，并直接把单窗口命令数从 60 抬到 447。只想画外框时用 `CLAY_BORDER_OUTSIDE(w)`（`betweenChildren = 0`）。
- **不要用 `Clay_OnHover` 做点击判定**：该回调在 `Clay_SetPointerState` 内部被调用，且用的是**更新前**的 `pointerInfo`；`PRESSED_THIS_FRAME` 会滞留到下一次指针事件，于是"按下"在抬起时才触发一次，"再点一次"就变成第二次触发。外壳（任务栏）已改为读 `VmWm.pointerDownSeq` 的按下沿。
- **`Clay_Hovered()` 必须在元素声明内部求值**：写在 `CLAY(...)` 之前拿到的是父容器的 hover，同一窗口里所有按钮会一起高亮。`CLAY` 宏先 `OpenElement` 再求值声明，所以 `.backgroundColor = Clay_Hovered() ? a : b` 才是本元素的。
- **外壳 onHover 会带着旧位置回调**（根因未查明）：指针在别的窗口上时，任务栏条目的回调仍可能被触发一次（日志里 `down=0 seq=N`）。当前在 `shell_item_press` 里加了「`wm->pointerX/Y` 必须落在任务栏矩形内」的兜底挡掉；真正的触发路径还没定位，遇到类似"在 A 上按下却触发了 B 的条目"先查这里。
- **读 guest dump 要同时看 `min / max / focused`**：只看一眼焦点容易误判。曾把"点任务栏条目后窗口被 raise"当成 bug，实际是该窗口当时处于最小化状态、走了还原分支（还原会重新聚焦并 raise）。
- **测试里点标题栏按钮不要写死坐标**：按钮从两个变三个时，最小化按钮整体左移 20px，两条断言直接失效。按钮位置由 `chrome_button_rects` 决定，测试应跟着几何算；最大化后标题栏在 `y = 0..30`，与还原态不同。
- **`agent-browser` 的输出流不会结束**：`agent-browser eval ... | Select-Object -Last N` 永远等不到 EOF。取尾部会挂住，改成直接输出或用 `-First`；同理 `| Out-Null` 后接后续命令是安全的。
- **emsdk 已装在本机但不在 PATH**：`H:\github_repos\emsdk\upstream\emscripten\emcc.exe`，需要
  `$env:EM_CONFIG='H:\github_repos\emsdk\.emscripten'` 才能找到 node/python/LLVM。
  `where emcc` 找不到不代表没安装 —— 先看 `emsdk/upstream/emscripten`。
- **`inspect_vm_frame.py` 的对象断言**：手工解析 SSE 时 `ops` 里可能混有非 dict 元素，`p.get('ops')` 之前要判 `isinstance(p, dict)`，否则报 `'list' object has no attribute 'get'`。

## 调试脚本

- `tools/drive_vm_desktop.py` — 用 HTTP `/input` 合成指针事件驱动 `demo_jpx`（move / down / up / key / wait / snap），全程保持一条 SSE 连接并累积帧流。用来复现"点哪里、窗口怎么变"。
- `tools/inspect_vm_frame.py` — 回放抓到的 SSE 流，按前端语义维护 48 块瓦片，把 tile 局部坐标还原成画布绝对坐标后列出文本；`--hist` 看每次增量帧改了哪些块，`--tiles` 看每块命令数。

## 环境信息

- OS：Windows，PowerShell Core。
- SDK：`dotnet` 10.0.404；目标框架 `net9.0-windows`。
- 浏览器自动化：`agent-browser 0.27.0`（需 node 可用），截图默认落在 `C:\Users\linswin\.agent-browser\tmp\screenshots\`。
- 依赖包：`Newtonsoft.Json 13.0.3`（清理后唯一 NuGet 依赖）。
