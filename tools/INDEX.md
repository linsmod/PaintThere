# tools 索引

- `start_vm_renderer.py` — 一键启动 C 版 VM 渲染器：`python tools/start_vm_renderer.py`。同时拉起 SSE 服务端与 `vm_renderer.exe`，进程级禁用代理（避免本地请求走系统代理），Ctrl+C 一起停止。调试用环境变量：`VM_DESKTOP_PORT`、`VM_RENDERER_FRAMES`（>0 时跑满帧数自动退出）。
- `vm-desktop/renderer/` — C 宿主（canvas-cpp/ant 引擎），构建：CMake `-S vm-desktop/renderer -B vm-desktop/renderer/build -G "MinGW Makefiles" -DCANVAS_SOURCE=<canvas-cpp源码> -DANT_ROOT=<ant目录>` 后 `--build`。运行由 `start_vm_renderer.py` 负责。
- `assets/vm_renderer.js` — C 渲染器页面：复用 `app.js` 原语 + wasm 字体 + `EventSource`，无兼容 shim。将 `app.js` 所需但 SDK 缺失的标准 API（`ctx.canvas`、context 缓存）在页面入口对齐。
- `start_vm_desktop.py` — 零参数启动浏览器版 VM 桌面：`python tools/start_vm_desktop.py`。自动定位 `assets/`、显式传 webRoot 绝对路径、停掉旧的 demo_wm 进程并打开浏览器。调试用环境变量：`VM_DESKTOP_PORT`、`VM_DESKTOP_NO_BROWSER=1`。
- `assets/` — 共享前端资源（`app.js`、`systemColors.js`、`font_wasm.js`、`stb_font_wasm.mjs/.wasm`、`font.ttf`），供 demo_wm/demo_server 与 ConsoleApp2/TestHarness 共用，消除多份副本。
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
- `vm-desktop/demo_wm.c` — 双窗口 WM demo。构建：`gcc -std=c11 -I<clay-dir> -I. demo_wm.c clay_frame_adapter.c vm_display.c vm_serialize_json.c vm_wm.c vm_server.c -lws2_32 -o demo_wm.exe`；默认运行 `./demo_wm 9000`（webRoot 默认 `../assets`）。截图 `wm_two_windows.png` / `wm_after_drag.png` / `wm_click.png` / `wm_cursors.png` / `wm_local_bg.png` / `wm_internal_model.png` / `wm_incremental.png`。自适应帧率：无变化不出帧；有变化时结构体层 diff 只发变化命令。
- `vm-desktop/vm_server.c/.h` — C 跨平台 HTTP 服务：`GET /events` SSE 推帧、`POST /input` 收原始输入、白名单静态资源。
- `vm-desktop/demo_server.c` — SSE 服务 demo。构建：`gcc -std=c11 -I<clay-dir> -I. demo_server.c demo_layout.c clay_frame_adapter.c vm_server.c -lws2_32 -o demo_server.exe`；运行：`./demo_server 9000`（webRoot 默认 `../assets`）。本机 clay 目录 `H:\github_repos\clay`；验证截图 `vm-desktop/demo_render.png`。

构建与验证使用标准命令：`dotnet build ConsoleApp2/RemoteForm.csproj`、`dotnet build TestHarness/TestHarness.csproj`，浏览器验证使用 `agent-browser`。详见 `docs/debug-跨端图绘指令流.md`。
