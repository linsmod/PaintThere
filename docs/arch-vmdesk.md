# 架构现状

最后核对：2026-11-24
本文描述当前实现，不记录修复历史。项目在 guest 侧用 C 构建桌面/窗口管理器与应用外壳，把绘制指令通过 SSE 下发到浏览器或 C 渲染器，原始输入经 HTTP POST 回传。早期 WinForms/.NET 程序及其 WebSocket 通道已移除，本文不再描述它们。

## 组成

- `vm-desktop/`：guest 侧 C 程序与库。
  - `vm_wm.c/.h`：窗口树、z 序、焦点、拖动、命中测试、多窗口帧合成；窗口语义只在 guest 内。窗口内容只认 `VmCmdBuffer`，不依赖任何 UI 库。
  - `vm_template.c/.h`：模板层绘制（外 4 边 → 内 3 边 → 高光），任务栏与将来的 XAML Direct 后端共用。
  - `vm_display.c/.h`、`vm_serialize_json.c/.h`：typed 内部模型 `VmDisplayList` 与唯一的 JSON 序列化；在结构体层做 retained diff，不基于字符串比较。
  - `vm_tiles.c/.h`：把全局帧切为瓦片；按命令签名只发变化块，`showLoading` / `hideLoading` 包住批次。
  - `clay_frame_adapter.c/.h`：Clay `RenderCommandArray` → `VmDisplayList`。
  - `vm_font.c/.h`：native stb_truetype 度量，口径对齐浏览器 wasm 光栅化；`vm-desktop/stb_font_wasm.c` 编出 `assets/stb_font_wasm.mjs/.wasm`。
  - `vm_theme.c/.h`：主题 token。
  - `vm_shell.c/.h`：任务栏内容生产者（纯绘制层）。输入 `VmShellModel`（窗口条目快照 + 指针），输出 `VmCmdBuffer` 或 `VmShellAction`；不依赖 `vm_wm` / Clay。
  - `vm_shell_local.c`：单进程适配：把 `VmWm` 填成 model、把 action 翻译回 `VmWm_*`（含右键菜单与显示桌面）。
  - `vm_menu.c/.h`：弹出菜单控件。
  - `vm_clipboard.c/.h`、`vm_fs*.c`、`vm_sys*.c`、`vm_ipc.h`：guest 内剪贴板、文件系统/系统能力抽象与进程间协议定义。
  - `vm_server.c/.h`：C HTTP 服务：`GET /events`（SSE 帧与资源）、`POST /input`（原始输入）、静态资源白名单。单进程模式（`demo_wm`）用它。
  - `wms/`：WMS 进程化层（协议 v2）。`wms_ipc.h` 线路协议、`wms_client.c` 句柄式客户端 SDK、`wms_server.c` 窗口表/焦点/输入路由/连接生命周期、`vm_host.c` 本项目合成宿主（装饰 + 合成 + 瓦片 + 资源包）。产物 `vm_wms.exe`；`--mock` 挂占位宿主。
  - `rdp/`：呈现端服务器（产物 `vm_rdp.exe`）。libuv 实现 HTTP / SSE / WebSocket / 静态资源 / respack；帧来自 `EVT_FRAME` / `EVT_FULLFRAME`，输入走 `CMD_INPUT`。不合成、不持窗口表。
  - `shell/`：桌面外壳进程（产物 `vm_shell.exe`）。以 SHELL 角色连 wms：建任务栏窗口（flags 四件套）、推内容、把输入命中翻成 `Wms_*`；不链 `vm_wm` / Clay。
  - `app/`：新架构应用（产物 `vm_app.exe`）。以 APP 角色连 wms，按 `EVT_LAYOUT_REQ` 产出 body 局部 `VmCmdBuffer` 并推送；`--ui hello|clay|xaml` 三种内容后端。旧 `demo_jpx` / `demo_xaml` 与其依赖的 `jpx/` 已删除（M7.5）。
  - `demo_wm.c`：单进程 WM / 多客户端指针 demo（保留作单进程回归）。
  - `jpx_xaml/`：声明式 UI 的 parser/state、Direct 渲染与交互、ResourceDictionary 主题预编译（`JpxApp`/`Jpx_*` 执行层已删除）。
  - `ui/`：`vm_ui_core`（焦点/文本编辑/命中）+ `ui/clay/`（`vm_ui_clay` 控件层）。
  - `test_*.c`：纯逻辑断言测试，统一支持 `--probe`；命令与耗时见 `docs/test-timing.md`。
- `assets/`：浏览器与 C 渲染器共享的前端资源。
  - `vm_renderer.js`：唯一渲染入口；连接 `/events`，管理瓦片画布与资源批次，上行 `POST /input`。
  - `app.js`：绘制原语与资源命令执行（`commandHandlers`）、`setDrawTarget`、文本重放。
  - `font_wasm.js` + `stb_font_wasm.mjs/.wasm`：浏览器端 stb_truetype 光栅化；`font.ttf` 为浏览器与 guest 共用字体。
  - `themes/`：预编译的 RetroBar 风格主题包；`index.html` / `preview.html` 为主题列表与预览页。
- `tools/`：构建、启动、驱动与帧检查脚本，入口见 `tools/INDEX.md`。
  - `build_vm.py` 一次构建 `demo_wm` 与 `vm_renderer`；`start_vm_desktop.py` / `start_vm_renderer.py` 启动 demo；`drive_vm_desktop.py` 合成输入；`inspect_vm_frame.py` 回放帧；主题预编译与导出脚本维护 `assets/themes/`。

## 数据流

### 下行：guest → 渲染器

1. Clay 在 guest 生成 `RenderCommandArray`，`clay_frame_adapter` 填充 `VmDisplayList`；WM 合成各窗口命令，窗口装饰与内容保持 guest 内语义。
2. `VmDisplayList` 在结构体层做 retained diff，再经 `vm_tiles` 切成瓦片：按命令 `rect` 与瓦片相交收集命令，坐标改为瓦片局部，只有签名变化的瓦片生成 `setTile`；全量帧带 `config`，变化批次前后带 `showLoading` / `hideLoading`。
3. `vm_serialize_json` 输出 `{"type":"tiles","frameId":N,"ops":[...]}`；`vm_server` 通过 SSE `event: fullframe` 与默认消息下发帧，`event: resources` / `resources-update` 下发一次性资源，`event: control` 处理服务端满载。新连接先收到 resources 与 fullframe 建立基线。
4. 无变化不出帧；字体位图或图片解码就绪后由前端本地重放受影响的瓦片，服务端不重发。
5. `vm_renderer.js` 执行 `config` 建瓦片画布，`setTile` 以瓦片局部坐标执行绘制原语，`hideLoading` 时一次性贴齐主画布；`app.js` 的 `commandHandlers` 是唯一原语执行器。

### 上行：渲染器 → guest

1. 浏览器把指针、键盘、滚轮等事件转成原始输入（`kind` / `x` / `y` / `clientId` / `frameId`），`POST /input` 给 C 服务；C 渲染器宿主走同一入口。
2. `vm_server` 只转发原始 body，不解释窗口语义；WM 按 z 序命中窗口、仲裁焦点、交给窗口的 Clay 上下文与 handler。
3. 多客户端共用同一画面；焦点状态只在 guest，非焦点客户端只更新指针。指针由 guest 多客户端指针表生成绘制命令，不暴露窗口 API。

## 关键设计点

- **协议只有绘制原语与原始输入**：不跨端传 `createWindow` / `raise` / `focus` 等窗口级指令；瓦片是远端小画布管理，不是窗口结构。
- **窗口内容契约是 `VmCmdBuffer`**：`VmWindowLayout` 与 `VmWindowPaint` 同形。Clay 只是 producer 侧的适配器（`clay_frame_adapter`），`vm_wms` / `vm_shell` 的链接命令行里没有 Clay。
- **内部模型与序列化分离**：`VmDisplayList` 是 typed 结构体，JSON 只是当前序列化格式；diff 在结构体层做，不基于字符串。
- **自适应帧率**：有变化才出帧，空闲零帧；首帧与新连接必达。
- **瓦片增量**：48 块 100×100 瓦片覆盖 800×600 逻辑画布，只有变化块下发；物理尺寸按 `renderScale` 放大，贴屏 1:1。
- **资源一次性下发**：`definePattern` / `defineStyle` / `defineDrawProgram` / `loadImg` 等按 id 缓存；主题包由 `loadResPack(id)` 加客户端 GET 拉取。
- **字体同源**：guest 的 `vm_font` 与浏览器的 wasm 光栅化共用同一 TTF 与度量口径，不使用 Canvas `fillText`。

## 已知限制

- C 渲染器宿主没有 `fetch`，`loadResPack` 会被静默跳过；C 侧主题热切换当前不可用。
- `assets/font.ttf` 当前为宋体子集，覆盖 XP 中文 UI 用字；替换字体需同时满足 stb 轮廓渲染与两端一致性。
- 静态资源上限由 `vm_server.c` 决定（当前 16 MB），新增大资源需要核对。
- 现行测试为 `vm-desktop/test_*.exe --probe` 纯逻辑断言；跨进程链路由 `tools/e2e_wms.ps1`（61 条）覆盖，浏览器视觉验证仍依赖 `agent-browser` 与截图，未形成像素回归。
- `vm_shell` 已提供任务栏、开始菜单、右键菜单、设置面板；设置面板不持久化，远程模式装饰按钮（×/□/—）无处理者（见 `issues/INDEX.md`）。
- wmwms 不管理最大化状态，`VmShellItem.maximized` 在外壳进程里恒 0。
- 多进程桌面没有一键启动器：`start_vm_desktop.py` 只拉单进程 `demo_wm`，多进程用 e2e 或手动 `vm_wms + vm_rdp + vm_shell + vm_app`。
