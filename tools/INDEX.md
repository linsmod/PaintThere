# tools 索引

- `vm-desktop/clay_frame_adapter.c/.h` — Clay `RenderCommandArray` 到 VM 绘制帧 JSON 的 adapter（阶段 1）。
- `vm-desktop/demo_frame.c` — 单窗口 Clay 布局 demo，输出 `demo_frame.json`。构建：`cd vm-desktop; gcc -std=c11 -I<clay-dir> -I. demo_frame.c clay_frame_adapter.c -o demo_frame.exe`。本机 clay 目录为 `H:\github_repos\clay`。

构建与验证使用标准命令：`dotnet build ConsoleApp2/RemoteForm.csproj`、`dotnet build TestHarness/TestHarness.csproj`，浏览器验证使用 `agent-browser`。详见 `docs/debug-跨端图绘指令流.md`。
