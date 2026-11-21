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
| `vm-desktop/clay_frame_adapter.c/.h` | Clay `RenderCommandArray` → JSON 帧；分块构建与坐标偏移 |
| `vm-desktop/stb_font_wasm.c` | stb_truetype wasm 绑定，导出 `loadFont/measureText/rasterizeText` |
| `vm-desktop/demo_wm.c` | 双窗口 demo，含 hover/click、多客户端指针、本地背景、自适应帧率 |
| `ConsoleApp2/app.js` | 前端原语执行：图片/字体/样式/图样/绘制程序/图层 |
| `ConsoleApp2/font_wasm.js` + `stb_font_wasm.mjs/.wasm` | 浏览器端 wasm 字体封装 |
| `ConsoleApp2/main.js` | WebSocket 通道的 frame envelope 与指针层（另一条通道，保留） |

## 常用命令

```powershell
# 构建 C demo（按需选一个）
cd h:\github_repos\跨端图绘指令流\vm-desktop
gcc -std=c11 -I"H:\github_repos\clay" -I. demo_wm.c clay_frame_adapter.c vm_wm.c vm_server.c -lws2_32 -o demo_wm.exe
gcc -std=c11 -I"H:\github_repos\clay" -I. demo_frame.c demo_layout.c clay_frame_adapter.c -o demo_frame.exe

# 编译 wasm 字体（先按上面设置 emsdk 环境变量）
emcc -O2 -s EXPORTED_FUNCTIONS='["_loadFont","_measureText","_rasterizeText","_alloc","_freeMem"]' -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32"]' -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=StbFontWasm -s ENVIRONMENT=web,node -I"H:\github_repos\mystbfont_c" stb_font_wasm.c -o stb_font_wasm.mjs
Copy-Item stb_font_wasm.mjs,stb_font_wasm.wasm ..\ConsoleApp2\

# 启动 WM demo（9000 端口；webRoot 指向前端源码目录）
$p = Start-Process -FilePath ".\demo_wm.exe" -ArgumentList "9000","../ConsoleApp2" -PassThru -RedirectStandardOutput wm.log -RedirectStandardError wm.err
# 若验证需要字体，先拷贝到 webRoot：
Copy-Item "H:\github_repos\mystbfont_c\fonts\NotoSans-Regular.ttf" ..\ConsoleApp2\font.ttf

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
Remove-Item ..\ConsoleApp2\font.ttf -ErrorAction SilentlyContinue
```

## 验证清单（按阶段）

1. **构建**：两个 `dotnet build` 通过；gcc 编译 `demo_frame` / `demo_wm` 通过；emcc 编译 wasm 通过。
2. **字体一致性**：`font_probe.exe <ttf> <size> <text>` 与 `node test_wasm_font.mjs <ttf> <size> <text>` 的宽度一致（本机验证过 91/70/247）。
3. **协议**：`curl -N /events` 能收到 `event: resources` + 首帧；帧中无 `wm_bg`，窗口背景为 `"s_window"`。
4. **自适应帧率**：空闲 2 秒后再连接，frameId 不变；一次输入后 frameId +1。
5. **渲染**：浏览器检查 `bg` canvas 全不透明、`c` canvas 标题栏颜色像素；console 无错误。
6. **交互**：hover 按钮由蓝变橙；单次 click 使 `Clicked: 0` → `Clicked: 1`。
7. **多客户端**：两个客户端持续上报不同颜色指针，浏览器看到对应颜色箭头（约 150 像素/箭头）。

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

## 退出条件

全部验证清单通过、console 无错误、截图留证后，关闭浏览器会话并停止本任务启动的 demo 进程；清理 `ConsoleApp2/font.ttf` 与测试二进制。遗留增强方向登记在 `docs/task-vm-instruction-desktop.md` 末尾。
