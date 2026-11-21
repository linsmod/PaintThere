# tools 索引

- `vm-desktop/clay_frame_adapter.c/.h` — Clay `RenderCommandArray` 到 VM 绘制帧 JSON 的 adapter。
- `vm-desktop/demo_layout.c/.h` — 共享单窗口 Clay 布局，供 demo_frame/demo_server 复用。
- `vm-desktop/demo_frame.c` — 输出单帧 JSON。构建：`gcc -std=c11 -I<clay-dir> -I. demo_frame.c demo_layout.c clay_frame_adapter.c -o demo_frame.exe`。
- `vm-desktop/vm_server.c/.h` — C 跨平台 HTTP 服务：`GET /events` SSE 推帧、`POST /input` 收原始输入、白名单静态资源。
- `vm-desktop/demo_server.c` — SSE 服务 demo。构建：`gcc -std=c11 -I<clay-dir> -I. demo_server.c demo_layout.c clay_frame_adapter.c vm_server.c -lws2_32 -o demo_server.exe`；运行：`./demo_server 9000 ../ConsoleApp2`。本机 clay 目录 `H:\github_repos\clay`；验证截图 `vm-desktop/demo_render.png`。

构建与验证使用标准命令：`dotnet build ConsoleApp2/RemoteForm.csproj`、`dotnet build TestHarness/TestHarness.csproj`，浏览器验证使用 `agent-browser`。详见 `docs/debug-跨端图绘指令流.md`。
