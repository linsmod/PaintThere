# tools 索引

- `build_vm.py` — 一次构建桌面服务端 `demo_wm.exe` 与 C 渲染器 `vm_renderer.exe`：`python tools/build_vm.py`。统一 CMake 工程 `vm-desktop/CMakeLists.txt`，产物都在 `vm-desktop/build/`。脚本同步 canvas-cpp 的 `libcanvas.a` / `antjsb.dll`，再 cmake 配置 + 构建。环境变量：`CANVAS_BUILD_DIR`、`CANVAS_SOURCE_DIR`、`CLAY_DIR`、`VM_BUILD_DIR`。注意 Windows 下需先停掉运行中的 exe 才能覆盖（启动脚本会先按端口停旧实例）。
- `vm-desktop/CMakeLists.txt` — 统一 CMake 工程：`demo_wm` 目标链接 Clay + ws2_32，`vm_renderer` 目标链接 canvas-cpp/ant；两者一次 `--build` 产出。
- `start_vm_renderer.py` — 一键启动 C 版 VM 渲染器：`python tools/build_vm.py && python tools/start_vm_renderer.py`。同时拉起 SSE 服务端与 `vm_renderer.exe`，进程级禁用代理，Ctrl+C 一起停止。调试用环境变量：`VM_DESKTOP_PORT`、`VM_RENDERER_FRAMES`（>0 时跑满帧数自动退出）。
- `assets/vm_renderer.js` — C 渲染器页面：复用 `app.js` 原语 + wasm 字体 + `EventSource`，无兼容 shim。将 `app.js` 所需但 SDK 缺失的标准 API（`ctx.canvas`、context 缓存）在页面入口对齐。
- `start_vm_desktop.py` — 零参数启动浏览器版 VM 桌面：`python tools/start_vm_desktop.py` 启 `demo_wm`；`python tools/start_vm_desktop.py demo_jpx` 启 JPX 应用外壳（开始菜单 / ProcMan / Files）。自动定位 `assets/`、显式传 webRoot 绝对路径、检查 `build_vm.py` 产物、按端口停掉目标程序的旧实例后启动并打开浏览器。脚本在前台保持运行，按 Ctrl+C 会停止它拉起的 demo。两者默认端口不同（`demo_wm` 9000 / `demo_jpx` 9050），可同时开着对比。调试用环境变量：`VM_DESKTOP_PORT`、`VM_DESKTOP_NO_BROWSER=1`、`VM_DESKTOP_APP`（命令行参数优先）。
- `drive_vm_desktop.py` — 用 HTTP `/input` 合成指针事件驱动 VM 桌面，全程保持一条 SSE 连接累积帧流，用于「确定坐标 + 精确时序」的交互复现：`python tools/drive_vm_desktop.py 9051 down 40 582 wait 300 up 40 582 snap s1`。动作：`move / down / up / key / snap / wait / alive`；`down X Y` 会先发一帧 move 再按下（保证 Clay 有 hover 帧）。抓包目录由 `VM_DRIVE_OUT` 决定，默认 `temp/`。限制：`POST /input` 的 body 必须与请求头在同一次 send 里发出，服务端按单次 recv 取 body。
- `inspect_vm_frame.py` — 回放 `drive_vm_desktop.py` 抓到的 SSE 流，按前端语义维护 48 块瓦片当前内容，把 tile 局部坐标还原成画布绝对坐标并列出文本：`python tools/inspect_vm_frame.py temp/s1.txt --hist`。`--text K` 过滤文本、`--hist` 看每次增量帧改了哪些块、`--tiles` 看每块命令条数。
- `assets/` — 共享前端资源（`app.js`、`systemColors.js`、`font_wasm.js`、`stb_font_wasm.mjs/.wasm`、`font.ttf`），供 demo_wm/demo_server 与 ConsoleApp2/TestHarness 共用，消除多份副本。当前 `font.ttf` 为 `C:\Windows\Fonts\simhei.ttf`（9.7MB，含中文）；原拉丁子集 Noto Sans 可从 `H:\github_repos\mystbfont_c\fonts\NotoSans-Regular.ttf` 恢复。`vm_server.c` 静态资源上限 16MB。中文 UI 依赖此字体，勿随意替换成纯拉丁字体。
- `vm-desktop/clay_frame_adapter.c/.h` — Clay `RenderCommandArray` 到 VM 绘制帧 JSON 的 adapter。
- `vm-desktop/demo_layout.c/.h` — 共享单窗口 Clay 布局，供 demo_frame/demo_server 复用。
- `vm-desktop/demo_frame.c` — 输出单帧 JSON。构建：`gcc -std=c11 -I<clay-dir> -I. demo_frame.c demo_layout.c clay_frame_adapter.c -o demo_frame.exe`。
- `vm-desktop/stb_font_wasm.c` — stb_truetype wasm 绑定（loadFont/measureText/rasterizeText）。构建需先激活 emsdk：
  `emcc -O2 -s EXPORTED_FUNCTIONS='["_loadFont","_measureText","_rasterizeText","_alloc","_freeMem"]' -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32"]' -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=StbFontWasm -s ENVIRONMENT=web,node -I<mystbfont-dir> stb_font_wasm.c -o stb_font_wasm.mjs`
- `vm-desktop/font_probe.c` — native stb_truetype 度量探针，用于与 wasm 对比。
- `vm-desktop/test_wasm_font.mjs` — wasm 度量/光栅化测试脚本。
- `vm-desktop/vm_wm.c/.h` — guest 最小 WM：窗口树/z 序/焦点/标题栏拖动/多窗口帧合成。
- `vm-desktop/vm_display.c/.h` — 协议无关内部模型 `VmDisplayList`，typed 命令与自有字符串存储，结构体层 diff。
- `vm-desktop/vm_serialize_json.c/.h` — 唯一 JSON serializer：`VmDisplayList` → frame / command array JSON。
- `vm-desktop/clay_frame_adapter.c/.h` — Clay 命令 → `VmDisplayList`（不再直接生成 JSON）。
- `vm-desktop/jpx_xaml/` — jp_xaml 声明式 UI 层：`jpx_xaml.h` 公开头；`jpx_xaml_parser.c` XAML 子集解析；`jpx_xaml_state.c` 视图/绑定/handler 纯逻辑；`jpx_xaml_view.c` 每帧回放为 JPX 调用。`test_jpx_xaml.exe --probe` 纯逻辑断言。
- `vm-desktop/demo_xaml.c` — jp_xaml 表单 demo：XAML 内嵌描述界面，C 侧只有 state 结构体与 handler。构建由 `python tools/build_vm.py` 统一完成；启动 `python tools/start_vm_desktop.py demo_xaml`（默认 9060）。
- `vm-desktop/demo_wm.c` — 双窗口 WM demo。构建：`gcc -std=c11 -I<clay-dir> -I. demo_wm.c clay_frame_adapter.c vm_display.c vm_serialize_json.c vm_wm.c vm_server.c -lws2_32 -o demo_wm.exe`；默认运行 `./demo_wm 9000`（webRoot 默认 `../assets`）。截图 `wm_two_windows.png` / `wm_after_drag.png` / `wm_click.png` / `wm_cursors.png` / `wm_local_bg.png` / `wm_internal_model.png` / `wm_incremental.png`。自适应帧率：无变化不出帧；有变化时结构体层 diff 只发变化命令。
- `vm-desktop/vm_tiles.c/.h` — Remote tiles 切分：`VmWm_BuildFrame` 的全局坐标帧 → tile 局部命令，命令签名差异只发变化的块；`VmTileGrid_Init` 配置网格。构建 `demo_wm` 时必须链接 `vm_tiles.c`。
- `vm-desktop/vm_server.c/.h` — C 跨平台 HTTP 服务：`GET /events` SSE 推帧、`POST /input` 收原始输入、白名单静态资源。
- `vm-desktop/demo_server.c` — SSE 服务 demo。构建：`gcc -std=c11 -I<clay-dir> -I. demo_server.c demo_layout.c clay_frame_adapter.c vm_server.c -lws2_32 -o demo_server.exe`；运行：`./demo_server 9000`（webRoot 默认 `../assets`）。本机 clay 目录 `H:\github_repos\clay`；验证截图 `vm-desktop/demo_render.png`。

构建与验证使用标准命令：`dotnet build ConsoleApp2/RemoteForm.csproj`、`dotnet build TestHarness/TestHarness.csproj`，浏览器验证使用 `agent-browser`。详见 `docs/debug-跨端图绘指令流.md`。
