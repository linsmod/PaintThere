# tools 索引

- `vm-desktop/clay_frame_adapter.c/.h` — Clay `RenderCommandArray` 到 VM 绘制帧 JSON 的 adapter。
- `vm-desktop/demo_layout.c/.h` — 共享单窗口 Clay 布局，供 demo_frame/demo_server 复用。
- `vm-desktop/demo_frame.c` — 输出单帧 JSON。构建：`gcc -std=c11 -I<clay-dir> -I. demo_frame.c demo_layout.c clay_frame_adapter.c -o demo_frame.exe`。
- `vm-desktop/stb_font_wasm.c` — stb_truetype wasm 绑定（loadFont/measureText/rasterizeText）。构建需先激活 emsdk：
  `emcc -O2 -s EXPORTED_FUNCTIONS='["_loadFont","_measureText","_rasterizeText","_alloc","_freeMem"]' -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32"]' -s ALLOW_MEMORY_GROWTH=1 -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=StbFontWasm -s ENVIRONMENT=web,node -I<mystbfont-dir> stb_font_wasm.c -o stb_font_wasm.mjs`
- `vm-desktop/font_probe.c` — native stb_truetype 度量探针，用于与 wasm 对比。
- `vm-desktop/test_wasm_font.mjs` — wasm 度量/光栅化测试脚本。
- `vm-desktop/vm_wm.c/.h` — guest 最小 WM：窗口树/z 序/焦点/标题栏拖动/多窗口帧合成。
- `vm-desktop/demo_wm.c` — 双窗口 WM demo（含 hover/click 闭环、多客户端彩色指针、guest 焦点仲裁）。构建：`gcc -std=c11 -I<clay-dir> -I. demo_wm.c clay_frame_adapter.c vm_wm.c vm_server.c -lws2_32 -o demo_wm.exe`；运行 `./demo_wm 9000 ../ConsoleApp2`。截图 `wm_two_windows.png` / `wm_after_drag.png` / `wm_click.png` / `wm_cursors.png` / `wm_local_bg.png`（本地渐变背景）。自适应帧率：无变化不出帧。
- `vm-desktop/vm_server.c/.h` — C 跨平台 HTTP 服务：`GET /events` SSE 推帧、`POST /input` 收原始输入、白名单静态资源。
- `vm-desktop/demo_server.c` — SSE 服务 demo。构建：`gcc -std=c11 -I<clay-dir> -I. demo_server.c demo_layout.c clay_frame_adapter.c vm_server.c -lws2_32 -o demo_server.exe`；运行：`./demo_server 9000 ../ConsoleApp2`。本机 clay 目录 `H:\github_repos\clay`；验证截图 `vm-desktop/demo_render.png`。

构建与验证使用标准命令：`dotnet build ConsoleApp2/RemoteForm.csproj`、`dotnet build TestHarness/TestHarness.csproj`，浏览器验证使用 `agent-browser`。详见 `docs/debug-跨端图绘指令流.md`。
