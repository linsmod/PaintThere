# 工作流：VM 指令化桌面端到端开发

## 适用场景

在 guest 侧用 Clay 做布局、跨端只传绘制原语与原始输入、浏览器渲染的桌面/WM 场景中，做构建、启动、验证与排错。适用于修改 `vm-desktop/*.c`、`ConsoleApp2/app.js` / `main.js`、协议设计文档后。

## 前置条件与环境

- 工作区：`H:\github_repos\跨端图绘指令流`；设计冻结于 `docs/design-vm-draw-protocol.md`，进度见 `docs/task-vm-instruction-desktop.md`。
- Clay 源码（工作区外只读）：`H:\github_repos\clay`。`clay.h` 单头文件，`CLAY_IMPLEMENTATION` 只能在一个编译单元定义。
- mystbfont_c（工作区外只读）：`H:\github_repos\mystbfont_c`。字体度量/光栅化基于其 `stb_truetype.h`。
- Emscripten：`H:\github_repos\emsdk`（emcc 7.0.0，装在 H 盘因 C 盘空间不足）。**每个新 shell 需先设置环境变量**：
  ```powershell
  $env:PATH = "H:\github_repos\emsdk\upstream\emscripten;H:\github_repos\emsdk;H:\github_repos\emsdk\node\24.19.0_64bit;" + $env:PATH
  $env:EMSDK = "H:/github_repos/emsdk"
  $env:EMSDK_NODE = "H:\github_repos\emsdk\node\24.19.0_64bit\node.exe"
  $env:EMSDK_PYTHON = "H:\github_repos\emsdk\python\3.13.3_64bit\python.exe"
  ```
- C 编译器：MSYS2 gcc 15.2.0（`C:\msys64\mingw64\bin\gcc.exe`）；Windows 链接加 `-lws2_32`，POSIX 去掉。
- 浏览器验证：`agent-browser`；本机端口检查 `netstat -ano | findstr "LISTENING" | findstr ":9000"`。

## 相关文件

| 路径 | 作用 |
|---|---|
| `vm-desktop/vm_server.c/.h` | C 跨平台 HTTP 服务：SSE `/events`、`POST /input`、白名单静态资源、`VmResourceProvider` 一次性资源 |
| `vm-desktop/vm_wm.c/.h` | guest 最小 WM：窗口树/z 序/焦点/拖动/指针表/焦点仲裁/多窗口帧合成 |
| `vm-desktop/vm_tiles.c/.h` | 把全局坐标完整帧切成 tile 局部命令，按命令签名只发变化的 tile |
| `vm-desktop/clay_frame_adapter.c/.h` | Clay `RenderCommandArray` → JSON 帧；分块构建与坐标偏移 |
| `vm-desktop/stb_font_wasm.c` | stb_truetype wasm 绑定，导出 `loadFont/measureText/rasterizeText` |
| `vm-desktop/vm_font.c/.h` | native stb_truetype 度量，口径对齐前端 `rasterizeText`（float 累加、末端截断），含 `Wrap` |
| `vm-desktop/vm_theme.c/.h` | `VmChrome` + 控件样式 token，resources 唯一生成者 |
| `vm-desktop/vm_shell.c/.h` | 任务栏：无装饰/置顶/不可拖动/不抢焦点的特殊窗口，Clay 布局列出窗口条目 |
| `vm-desktop/vm_clipboard.c/.h` | guest 内单例剪贴板（4KB UTF-8），协议不传 |
| `vm-desktop/test_frame.c` | 帧断言测试：不开服务、不开浏览器，直接断言 `VmDisplayList` |
| `vm-desktop/demo_wm.c` | 双窗口 demo，含 hover/click、多客户端指针、本地背景、自适应帧率 |
| `ConsoleApp2/app.js` | 前端原语执行：图片/字体/样式/图样/绘制程序/图层 |
| `ConsoleApp2/font_wasm.js` + `stb_font_wasm.mjs/.wasm` | 浏览器端 wasm 字体封装 |
| `ConsoleApp2/main.js` | WebSocket 通道的 frame envelope 与指针层（另一条通道，保留） |

## 常用命令

```powershell
# 一次构建两个 C 项目：demo_wm.exe + vm_renderer.exe（统一 CMake 工程）
cd h:\github_repos\跨端图绘指令流
python tools/build_vm.py
# 产物：vm-desktop/build/demo_wm.exe 与 vm-desktop/build/vm_renderer.exe
# 等效手动命令：
# cmake -S vm-desktop -B vm-desktop/build -G "MinGW Makefiles" -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe -DCANVAS_SOURCE=<canvas-cpp源码> -DANT_ROOT=<ant目录> -DCLAY_DIR=<clay目录> -DCANVAS_JS_ENGINE=ant
# cmake --build vm-desktop/build -j4

# 其它 C demo（按需，独立构建，不进统一工程）
cd vm-desktop
gcc -std=c11 -I"H:\github_repos\clay" -I. demo_frame.c demo_layout.c clay_frame_adapter.c vm_display.c vm_serialize_json.c -o demo_frame.exe
gcc -std=c11 -I. test_display_diff.c vm_display.c -o test_display_diff.exe
cd ..

# 一键运行（同时起 SSE 服务 + 原生渲染窗口；脚本只检查 build_vm.py 产物）
python tools/start_vm_renderer.py
# 调试：$env:VM_RENDERER_FRAMES="90"; python tools/start_vm_renderer.py  # 跑满 90 帧自动退出

# 编译 wasm 字体（先按上面设置 emsdk 环境变量）
emcc -O2 -s EXPORTED_FUNCTIONS='["_loadFont","_measureText","_rasterizeText","_alloc","_freeMem"]' -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32"]' -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=StbFontWasm -s ENVIRONMENT=web,node -I"H:\github_repos\mystbfont_c" stb_font_wasm.c -o stb_font_wasm.mjs
Copy-Item stb_font_wasm.mjs,stb_font_wasm.wasm ..\assets\

# 一键启动（推荐）：零参数，先 python tools/build_vm.py，再运行本脚本停旧进程并开浏览器
python tools/start_vm_desktop.py
# 调试：环境变量改端口 / 不开浏览器
# $env:VM_DESKTOP_PORT="9001"; $env:VM_DESKTOP_NO_BROWSER="1"; python tools/start_vm_desktop.py

# 或手动启动（webRoot 默认 ../assets）
$p = Start-Process -FilePath ".\demo_wm.exe" -ArgumentList "9000" -PassThru -RedirectStandardOutput wm.log -RedirectStandardError wm.err

# 帧断言测试（不开服务，默认按候选路径找字体；最稳是一次性传入绝对路径）
.\vm-desktop\build\test_frame.exe ..\assets\font.ttf
# 集中运行用 --probe：只输出失败项与汇总
.\vm-desktop\build\test_frame.exe --probe
# 文本编辑状态机断言（纯逻辑，毫秒级，无外部依赖）
.\vm-desktop\build\test_text_edit.exe --probe

# 验证资源 + 首帧（自适应帧率下空闲无后续帧）
curl -s --max-time 2 -N http://127.0.0.1:9000/events

# 模拟输入触发新帧
curl -s -X POST -H "Content-Type: application/json" -d '{"kind":"mousemove","x":300,"y":250,"color":"#F44336","clientId":"A"}' http://127.0.0.1:9000/input

# 浏览器验证
agent-browser --session vm open http://127.0.0.1:9000/
agent-browser --session vm wait 1500
agent-browser --session vm console
agent-browser --session vm screenshot vm_check.png

# 收尾（只停本任务启动的进程）
Stop-Process -Id $p.Id
agent-browser --session vm close
```

## 验证清单（按阶段）

1. **构建**：`python tools/build_vm.py` 一次通过并生成 `demo_wm.exe` + `vm_renderer.exe`；两个 `dotnet build` 通过；gcc 编译 `demo_frame` 通过；emcc 编译 wasm 通过。
2. **字体一致性**：`font_probe.exe <ttf> <size> <text>` 与 `node test_wasm_font.mjs <ttf> <size> <text>` 的宽度一致（本机验证过 91/70/247）。
3. **协议**：`curl -N /events` 收到 `event: resources` + `event: fullframe`；fullframe 为 `{"type":"tiles",...}`，ops 依次含 `showLoading` / `config` / 48 个 `setTile` / `hideLoading`；帧中无 `wm_bg`，窗口背景为 `"s_window"`。
   - 增量验证：挂一个 SSE 连接后 `POST /input`，增量帧的 `setTile` 数量应远小于 48（指针移动通常 1–2 块），且始终带 loading 批次边界。
4. **自适应帧率**：空闲 2 秒后再连接，frameId 不变；一次输入后 frameId +1。
5. **渲染**：浏览器检查 `__vmRenderer.ready === true`、`tiles === 48`、`loading === false`、`error === ''`；`bg` canvas 全不透明、`c` canvas 标题栏颜色像素；console 无错误。
6. **交互**：hover 按钮由蓝变橙；单次 click 使 `Clicked: 0` → `Clicked: 1`。
7. **多客户端**：两个客户端持续上报不同颜色指针，浏览器看到对应颜色箭头（约 150 像素/箭头）。
8. **键盘**：焦点客户端点击窗口后发 `keydown`（key=`H`/`i`），帧中文本从空变 `Hi`；非焦点客户端发键被忽略（`changed=0`）。截图 `wm_keyboard.png`。
9. **retained diff**：浏览器 eval 调 `applyFrameDiff(frame,cv)`，相同帧第二次返回 `false`；两命令帧只改一个时仅脏区重画。端到端渲染像素不退化。

## 经验与坑

- `Clay_Color` 字段是 `float`，输出颜色用 `%g`，用 `%u` 会全得 0。
- `Clay_SetMeasureTextFunction` 第三参是 `void *`，不是 `uintptr_t`。
- `Clay_String` 必须指向长生命周期内存；栈上 `label` 数组返回后失效会渲染成乱码。
- `CLAY_TEXT` 宏不接受带逗号的复合字面量，先赋给变量再传。
- `Clay_OnHover` 的 `PRESSED_THIS_FRAME` 需要 mousedown/mouseup 分帧派发；同一次 eval 连发可能不触发。
- Emscripten `ENVIRONMENT='web'` 的胶水在 Node 下无法读 wasm，用 `ENVIRONMENT=web,node`；ES module 用 `EXPORT_ES6=1` 生成 `.mjs`，前端要 `import default`。
- `lastSentFrameId` 初值不能是 0（frameId 从 0 开始），用 `0xFFFFFFFF`。
- 测试页 mousemove 必须换算 CSS 坐标到 canvas 内部坐标；document 上 canvas 外的位置会产生负/超界坐标。
- 指针 3 秒超时；像素检查前必须持续 POST，否则检查为 0。
- C HTTP 服务用 `Connection: close`，Node `fetch` 连接复用会触发 undici 断言，验证静态资源用 `curl`。
- 用户启动的 `RemoteForm.exe` 可能占用 8080/8081；本任务用 9000 避开，不要终止非本任务进程。
- **不要用管道调用 `start_vm_desktop.py`**。脚本 `Popen` 的 `demo_wm.exe` 继承 stdout 且常驻不退出，管道永远等不到 EOF，表现为命令一直挂住（服务其实已起来）。不加管道：`Start-Process python -ArgumentList "tools/start_vm_desktop.py"`；要留日志：`cmd /c "python tools/start_vm_desktop.py > temp\start.log 2>&1"`。
- `demo_wm.exe` 正在运行时 `build_vm.py` 会在链接阶段失败（`cannot open output file demo_wm.exe: Permission denied`），源码其实已全部编译通过。先 `Get-Process demo_wm` + `netstat -ano | findstr ":9000"` 确认归属，再决定是否停。不想停用户实例时，用 `gcc ... -o ..\temp\<dir>\demo_wm_new.exe` 手工链到临时路径、换端口验证，等价于完整构建。
- PowerShell 里给 `curl.exe -d` 传 JSON **用单引号原样传，不要加 `\"` 转义**。转义后 guest 收到的是 `{\"kind\":...}`，`find_key` 匹配不到 `"kind"`，所有输入都变成 `changed=0` 的假阴性，容易误判成「事件没到 guest」。
- native 文本度量的对齐目标是 `rasterizeText` 的 `outW/outH`，不是 `measureText`。后者逐 glyph 截断累加，`"Window A"@14` 得 45，而真正贴到画布上的位图是 48；布局若按 45 算会错位 3px。`vm_font` 按 raster 口径实现（float 累加、末端截断）。
- 帧内容断言优先用 `test_frame.exe`：直接断言 `VmDisplayList`，1 秒内完成、不开服务不开浏览器，比像素比对快一个数量级。浏览器只用来确认最终像素与真实交互。
- `VmWm_BuildFrame` 会给每个带 layout 的窗口发 `clipPush/clipPop`。tile 里两者计数必须相等，这是「内容裁剪已生效」的最快判据（49 个 setTile → 98/98）。
- **合成输入做位置敏感验证时，必须先 `mousemove` 再 `mousedown`**。Clay 的 `PRESSED_THIS_FRAME` 需要先有一帧 hover；只发 `mousedown`，任务栏条目不响应。真实浏览器总会先发 mousemove，所以线上无感，但 curl 验证会误判成「点击失效」。
- **有浏览器连着时不要用 curl 验证位置敏感交互**。浏览器自己的 `mousemove` 会覆盖 WM 的全局指针（`wm->pointerX/pointerY` 是单份的），合成的 `mousedown` 坐标会被下一次真实移动抹掉。做法：另起一个无浏览器连接的实例（换端口）做确定性验证，用户那份留着看画面。
- **同一窗口内 Clay 元素 id 不能重复**。给控件内部装饰元素（标签容器、分隔线、勾选框方框）用固定字符串 id，同窗口出现两个就撞号，Clay 会按「同一元素」复用 hash 项，布局与交互都可能错乱。无应用语义的元素用「父元素 id + 帧内序号」生成 id（`Clay__HashStringWithOffset(name, serial++, Clay_GetOpenElementId())`，等价 `CLAY_SIDI_LOCAL`），帧内唯一且声明顺序不变时跨帧稳定。有交互的控件仍必须用应用给的稳定 id。
- **`CLAY_TEXT(text, &cfg)` 是错的写法**。`Clay__OpenTextElement` 按值收 `Clay_TextElementConfig`，`CLAY_TEXT` 的第二个参数会被当成初始化器：传指针等于把地址塞进结构体首字段，`fontId/fontSize` 变 0（编译器只给隐式转换警告）。症状是**窗口体、边框、按钮矩形都在，所有文本消失**——文本 boundingBox 变成 0×0。正确写法：`CLAY_TEXT(text, CLAY_TEXT_CONFIG({...}))`，或直接调 `Clay__OpenTextElement(text, config)`。
- **判断「Clay 命令进帧了但 tile 里没有」**，先打印每条渲染命令的 `commandType` 与 `boundingBox`：tile 切分按面积相交，0×0 的命令会被静默丢弃。这一步能直接把范围从「序列化/切分」缩到「度量/配置」。
- **`Clay_OnHover` 在多 `Clay_Context`（每窗一份）下可能不触发**：同一位置 `Clay_Hovered()` 为真、`Clay_SetPointerState` 已收到 `PRESSED_THIS_FRAME`，回调仍不调用（`vm_shell` 的上下文里同样写法正常，根因未定位）。控件交互改用 `Clay_Hovered()` + 自己从 WM 指针维护的按下沿（每窗 `pressEdge`），只依赖已验证可靠的部分。
- **`CLAY_SIZING_FIT` 在空容器上会算成 0 高**。单行输入框在「未聚焦且无文本」时没有子元素，FIT 高度 = 0，整个背景矩形被 tile 切分静默丢弃；症状是布局少一截、控件像不存在，而代码路径其实都跑到了（加 printf 能看到函数被调用）。这类控件给固定高度，或保证任何状态下都有子元素撑着。判断方法：`Clay_GetElementData(id).boundingBox` 的宽对、高为 0。
- **union 里放低频大结构会拖累每一个实例**。`VmDrawCommand` 的 union 含 `VmDefinePatternArgs`（896 B），于是每条 `drawRect` 也占 992 B；实测一帧只有 8 条资源命令、200 多条绘制命令，命令数组 496 KB。把资源类移出到独立数组后 496 B / 263 KB。判断方法：找出 union 里最大的成员，乘上数组长度，再问一句「它出现的频率配得上这个代价吗」。同类陷阱：`VmDisplayDelta.changed[]` 复制整条命令，命令一变胖它就跟着变胖。
- **颜色/枚举这类纯函数要单独验**。默认主题色与解析函数的 fallback 恰好相同时，解析恒失败也不会显形；给 `Jpx__Color` 换个非默认色（如橙色 hover）才暴露「`rgb()` 分支永远走不到」的 bug。
- **任务栏/Dock 类条目顺序不能用 z 序**。`VmWm_Enumerate` 是按 z 升序的（合成需要），而 `VmWm_Raise` 在每次聚焦时把窗口 z 抬到最大——直接用它会让人一戳条目就换位。用 `VmWm_EnumerateCreated`（按 `createdSeq`，创建后不变）。判断依据：任何「用户认为位置固定」的 UI 列表，都要问一句它依赖的顺序会不会因为交互而变。

## 退出条件

全部验证清单通过、console 无错误、截图留证后，关闭浏览器会话并停止本任务启动的 demo 进程；清理 `ConsoleApp2/font.ttf` 与测试二进制。遗留增强方向登记在 `docs/task-vm-instruction-desktop.md` 末尾。
