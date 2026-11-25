# tools 索引

- `build_vm.py` — 一次构建 vm-desktop 全部 C 目标（`vm_wms` / `vm_rdp` / `vm_shell` / `vm_app` / `vm_renderer`）：`python tools/build_vm.py`。统一 CMake 工程 `vm-desktop/CMakeLists.txt`，产物都在 `vm-desktop/build/`。脚本同步 canvas-cpp 的 `libcanvas.a` / `antjsb.dll`，再 cmake 配置 + 构建。环境变量：`CANVAS_BUILD_DIR`、`CANVAS_SOURCE_DIR`、`CLAY_DIR`、`VM_BUILD_DIR`。注意 Windows 下需先停掉运行中的 exe 才能覆盖（启动脚本会先按管道/端口停旧实例）。
- `vm-desktop/CMakeLists.txt` — 统一 CMake 工程：`vm_wms` / `vm_rdp` / `vm_shell` / `vm_app` 与测试；`vm_renderer` 目标链接 canvas-cpp/ant，其余不链 Clay（`vm_ui_clay` 库除外）。
- `extract_system_font.py` — 从系统 TTC 提取单个字体为 TTF 并可选子集化：`python tools/extract_system_font.py simsun --subset`，输出 `temp/fonts/simsun.ttf`（约 2.5 MB）。用途：`demo_xaml --font` 指定主题字体，前端与 guest 共用同一文件。依赖 fontTools（已装 4.56）；系统字体有版权，输出不进仓库（`temp/` 不受 git 管理）。TTC 现在两端都能直读（stb 初始化用 `GetFontOffsetForIndex`），提取只为把 18 MB 减到 2.5 MB。
- `start_vm_renderer.py` — 一键启动 C 版 VM 渲染器：`python tools/build_vm.py && python tools/start_vm_renderer.py`。默认拉起多进程桌面（`multi`：`vm_wms` + `vm_shell` + `vm_app` + `vm_rdp`，端口 9050）与 `vm_renderer.exe`，给渲染器设 `CANVAS_PAGE_URL` 对齐 SSE 端口，进程级禁用代理；Ctrl+C 按 `vmwm_ui_` 管道名收掉整组。调试用环境变量：`VM_RENDERER_APP`（默认 `multi`）、`VM_DESKTOP_PORT`、`VM_RENDERER_FRAMES`（>0 时跑满帧数自动退出）。
- `assets/vm_renderer.js` — C 渲染器页面：复用 `app.js` 原语 + wasm 字体 + `EventSource`，无兼容 shim。将 `app.js` 所需但 SDK 缺失的标准 API（`ctx.canvas`、context 缓存）在页面入口对齐。
- `start_vm_desktop.py` — 启动浏览器版 VM 桌面：默认 `multi`，顺序拉起 `vm_wms` / `vm_shell` / `vm_app` / `vm_rdp`（共用 `vmwm_ui_<pid>` 管道，端口 9050），四个进程日志写到 `temp/desktop-<name>.err.txt`，Ctrl+C 或脚本退出时按逆序收掉；重跑会先清理上一组（命令行含 `vmwm_ui_`）。第一个位置参数也可以是 build/ 下任意 exe 名（需自行提供 wms 侧）。控制参数：`--port N`（优先于 `VM_DESKTOP_PORT`）、`--no-browser`；多进程只认 `--font`，单进程原样透传。调试用环境变量：`VM_DESKTOP_PORT`、`VM_DESKTOP_NO_BROWSER=1`、`VM_DESKTOP_APP`（命令行参数优先）。
- `drive_vm_desktop.py` — 用 HTTP `/input` 合成指针事件驱动 VM 桌面，全程保持一条 SSE 连接累积帧流，用于「确定坐标 + 精确时序」的交互复现：`python tools/drive_vm_desktop.py 9051 down 40 582 wait 300 up 40 582 snap s1`。动作：`move / down / up / key / snap / wait / alive`；`down X Y` 会先发一帧 move 再按下（保证 Clay 有 hover 帧）。抓包目录由 `VM_DRIVE_OUT` 决定，默认 `temp/`。限制：`POST /input` 的 body 必须与请求头在同一次 send 里发出，服务端按单次 recv 取 body。
- `precompile_themes.py` — 主题预编译编排：`python tools/precompile_themes.py <themesDir>` 把 RetroBar 风格 .xaml 全量预编译进 `assets/themes/<slug>/`（xalm.bin + strings.bin + resid2path.bin + 图片按 `<资源id>.png`）。预编译本体在 C 侧（`test_jpx_xaml_theme.exe --precompile <theme.xaml> <outDir>`，合并/归一化单一来源），本脚本只做 slug 命名、逐主题调用与产物核对。运行时 `vm_app --ui xaml --theme <slug>` 直接读二进制，不再解析 XAML 文本。格式细节见 `docs/task-jp-xaml-theme.md` S9 节。
- `export_theme_css.py` — 主题导出器（反编译）：`python tools/export_theme_css.py --all` 把预编译三件套原地导出为 CSS 子集（每个主题目录内的 `theme.css`，与 bins/图片一起入库；`--out` 指临时目录），`--check` 跑「导出→`--compile-css` 编译回→三件套逐字节比对」（当前 19/19 一致）；`--all` 同时维护 `themes/index.json`。协议级独立实现（Python struct 直读，不链 C 代码），与 xaml 工具链只共享冻结的二进制协议。CSS 子集语法见 `docs/task-jp-xaml-theme.md` S10 节。
- **主题预览站点（traycmd 托管）**：short id `38097e6f`，`http://127.0.0.1:41010/`（workdir=assets，qcmd serve 41010，随托盘自启）。入口 `/`（主题列表，数据源 themes/index.json），交互预览 `/preview.html?theme-dir=<slug>`。停止：托盘"网站"分区或 `qcmd kill 38097e6f`。
- `inspect_vm_frame.py` — 回放 `drive_vm_desktop.py` 抓到的 SSE 流，按前端语义维护 48 块瓦片当前内容，把 tile 局部坐标还原成画布绝对坐标并列出文本：`python tools/inspect_vm_frame.py temp/s1.txt --hist`。`--text K` 过滤文本、`--hist` 看每次增量帧改了哪些块、`--tiles` 看每块命令条数。
- `assets/` — 共享前端资源（`app.js`、`systemColors.js`、`font_wasm.js`、`stb_font_wasm.mjs/.wasm`、`font.ttf`），供 `vm_rdp` 与浏览器/C 渲染器共用，消除多份副本。当前 `font.ttf` 为宋体子集（`extract_system_font.py simsun --subset` 产物，2.5MB，XP 中文 UI 真实用字；原 simhei 9.7MB 可从 git 历史恢复）。`rdp/rdp_server.c` 静态资源上限 16MB。中文 UI 依赖此字体，勿随意替换成纯拉丁字体；注意 stb 只渲染 SimSun 轮廓，不用其内嵌点阵。
- `vm-desktop/clay_frame_adapter.c/.h` — Clay `RenderCommandArray` 到 VM 绘制帧 JSON 的 adapter。
- `vm-desktop/stb_font_wasm.c` — stb_truetype wasm 绑定（loadFont/measureText/rasterizeText）。本机 emcc 在
  `H:\github_repos\emsdk\upstream\emscripten\emcc.exe`（不在 PATH），需要先设 `EM_CONFIG`：

  ```powershell
  $env:EM_CONFIG='H:\github_repos\emsdk\.emscripten'
  & 'H:\github_repos\emsdk\upstream\emscripten\emcc.exe' -O2 `
    -s EXPORTED_FUNCTIONS='["_loadFont","_measureText","_rasterizeText","_alloc","_freeMem"]' `
    -s EXPORTED_RUNTIME_METHODS='["HEAPU8","HEAPU32"]' -s ALLOW_MEMORY_GROWTH=1 `
    -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=StbFontWasm -s ENVIRONMENT=web,node `
    -I'H:\github_repos\mystbfont_c' stb_font_wasm.c -o '..\assets\stb_font_wasm.mjs'
  ```

  产物 `assets/stb_font_wasm.{mjs,wasm}` 受 git 管理，改源码后必须重编并一起提交。
- `vm-desktop/font_probe.c` — native stb_truetype 度量探针，用于与 wasm 对比。
- `vm-desktop/test_wasm_font.mjs` — wasm 度量/光栅化测试脚本。
- `vm-desktop/vm_wm.c/.h` — guest 最小 WM：窗口树/z 序/焦点/标题栏拖动/多窗口帧合成。窗口内容契约是 `VmCmdBuffer`，不认识 Clay 或其它 UI 库。
- `vm-desktop/vm_template.c/.h` — 模板层绘制：`VmCmd_AppendRect` + `VmTemplate_PaintLayers`（外 4 边 → 内 3 边 → 高光）。任务栏与将来的 XAML Direct 后端共用。
- `vm-desktop/ui/` — `vm_ui_core` 静态库（M7.3）：`ui_focus.c` 焦点表/Tab 循环、`ui_text_edit.c` 文本编辑状态机（从 `JpxTextEdit` 迁移）、`ui_hit.c` 逆序命中、`ui_input.h` 与 wms 无关的事件结构。零布局引擎依赖，retain/imm 共用，第三方 app 可链。`test_ui.exe --probe` 26 项。
- `vm-desktop/ui/clay/` — `vm_ui_clay` 静态库（M7.3b）：Clay 布局 + 按钮/复选框/输入框/标签，状态来自 `vm_ui_core`，产出 `VmCmdBuffer`。无 `JpxApp` 单例、不认识 `VmWm`。控件用固定高度（空内容 FIT 会算 0 高）；合成点击需带 `mousemove`，否则 `Clay_Hovered()` 为假。
- `vm-desktop/vm_menu_view.c/.h` — 弹出菜单的纯绘制/命中层（M7.2）：不认识 `VmWm`，窗口由 `vm_shell` 建；`VmMenuView_Layout/Paint/Hit/SetHover`。
- `vm-desktop/vm_display.c/.h` — 协议无关内部模型 `VmDisplayList`，typed 命令与自有字符串存储，结构体层 diff。
- `vm-desktop/vm_serialize_json.c/.h` — 唯一 JSON serializer：`VmDisplayList` → frame / command array JSON。
- `vm-desktop/clay_frame_adapter.c/.h` — Clay 命令 → `VmDisplayList`（不再直接生成 JSON）。
- `vm-desktop/jpx_xaml/` — jp_xaml 声明式 UI 层：`jpx_xaml.h` 公开头；`jpx_xaml_parser.c` XAML 子集解析；`jpx_xaml_state.c` 视图/绑定/handler 纯逻辑；`jpx_xaml_view.c` 每帧回放为 JPX 调用（旧路径）；`jpx_xaml_render_direct.c` + `jpx_xaml_backend_cmdbuf.c` + `jpx_xaml_direct_ui.c` 为 M7.1 的 Direct 后端（直接出 `VmCmdBuffer`，含命中/按钮/复选框/输入框/列表交互，不依赖 Clay，CMake 静态库 `jpx_xaml_direct`）。`test_jpx_xaml.exe --probe` 67 项。
- **old infra（2026-11-25 弃用）**：`demo_jpx` / `demo_xaml` / `jpx/` / `jpx_xaml/` 保留代码但不再开发，由 `vm-desktop/app`（`vm_app.exe`）取代，M6 验收后删除。见 `docs/task-vm-app.md`。
- `vm-desktop/demo_xaml.c` — jp_xaml 表单 demo：XAML 内嵌描述界面，C 侧只有 state 结构体与 handler。构建由 `python tools/build_vm.py` 统一完成；启动 `python tools/start_vm_desktop.py demo_xaml`（默认 9060）。demo 含「切换主题」按钮，演示 RetroBar 风格 ResourceDictionary 热切换。
- `vm-desktop/jpx_xaml/jpx_xaml_theme.c/.h` — ResourceDictionary 主题：解析 Solid/Linear/Radial Brush 与数值资源、按 key 查询、派生 VmTheme、展开 `{StaticResource}`。
- `vm-desktop/test_jpx_xaml_theme.c` — 资源字典解析/查找/渐变/派生的纯逻辑断言。
- `vm-desktop/vm_tiles.c/.h` — Remote tiles 切分：`VmWm_BuildFrame` 的全局坐标帧 → tile 局部命令，命令签名差异只发变化的块；`VmTileGrid_Init` 配置网格。宿主 `vm_host.c` 用它出帧。
- `vm-desktop/rdp/` — 呈现端服务器 `vm_rdp.exe`：libuv 实现的 HTTP / SSE / WebSocket / 静态资源 / respack。帧与资源全部来自 `vm_wms` 的事件，自己不合成；不链 `vm_wm` / Clay。`--port`（默认 9000）、`--pipe`、`--web-root`、`--font`。
- `vm-desktop/shell/` — 桌面外壳进程 `vm_shell.exe`：`--pipe`、`--font`。以 SHELL 角色连 `vm_wms`，提供任务栏、开始菜单（XAML）、右键菜单（`vm_menu_view`）、设置面板（XAML，无装饰窗口）。不链 `vm_wm` / Clay；链接 `jpx_xaml_direct`。
- `vm-desktop/app/` — 新架构应用 `vm_app.exe`（M6/M7.4）：`--pipe`、`--title`、`--x`、`--y`、`--font`、`--slow-layout <ms>`、`--resize-after <ms> <w> <h>`、`--ui hello|xaml`、`--xaml <file>`、`--theme <slug>`、`--themes-root <dir>`、`--check-theme <slug>`。以 APP 角色连 `vm_wms`，只回 body 局部 `VmCmdBuffer`；`--ui hello` 手写命令，`--ui xaml` 走 `jpx_xaml_direct`。不链 `vm_wm` / Clay。
- `vm-desktop/vm_shell.c/.h` — 任务栏纯绘制层：`VmShellModel`（条目快照 + 指针）→ `VmCmdBuffer` 或 `VmShellAction`；`vm_shell_local.c` 是单进程适配（`VmWm` → model，action → `VmWm_*`）。
- `vm-desktop/wms/` — WMS 协议层与服务：`wms_ipc.h`（v2 线路协议）、`wms_client.c`（句柄式 SDK）、`wms_server.c`（窗口表/焦点/输入路由）、`vm_host.c`（本项目合成宿主，10 个回调）。

- `e2e_wms.ps1` — WMS 端到端验收：`pwsh tools/e2e_wms.ps1`（`-SkipBuild` 跳过构建）。起 `vm_wms` + `vm_rdp` + `test_wms_app/shell/rdp` + 真实 `vm_shell` / `vm_app`，跑 proto / basic / crash / restart / rdp / shell / app 七个场景共 61 条断言；rdp 场景覆盖 HTTP 首页、SSE 基线、应用内容进帧、respack 容器、HTTP 与 WebSocket 两条输入通道、kill rdp 后重启恢复画面；shell 场景覆盖真实外壳推送任务栏、kill 后任务栏消失且应用保留、重启后重建；app 场景覆盖新 app 建窗与内容、任务栏两条目、点击命中、kill 一个实例后另一个保留。日志落 `temp/e2e-wms-logs/`（不受 git 管理）。每个场景用独立管道名 `\\.\pipe\vmwm_e2e_<tag>_<随机>` 与独立端口（`19xxx`），可与正在运行的实例并存。验收结果与已知限制见 `docs/task-vm-wms-merge.md` §13.9。
- `probe_ws_input.py` — 最小 WebSocket 上行探针，覆盖 curl 到不了的 `/ws` 握手与掩码帧：`python tools/probe_ws_input.py --port 19090 [--kind keydown --key Tab] [--quiet]`。对 `vm_rdp` / `demo_jpx` 都可用；断言落点是对端日志（`input kind=...`）与应用侧收到的 body 局部坐标。e2e 的 rdp 场景调用它。

构建与验证使用标准命令：`python tools/build_vm.py`、`python tools/start_vm_desktop.py`，浏览器验证使用 `agent-browser`。详见 `docs/debug-vmdesk.md`。
