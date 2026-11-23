# 架构现状

最后核对：2026-11-24
本文描述当前实现，不记录修复历史。项目在 guest 侧用 C 构建桌面/窗口管理器与应用外壳，把绘制指令通过 SSE 下发到浏览器或 C 渲染器，原始输入经 HTTP POST 回传。早期 WinForms/.NET 程序及其 WebSocket 通道已移除，本文不再描述它们。

## 组成

- `vm-desktop/`：guest 侧 C 程序与库。
  - `vm_wm.c/.h`：窗口树、z 序、焦点、拖动、命中测试、多窗口帧合成；窗口语义只在 guest 内。
  - `vm_display.c/.h`、`vm_serialize_json.c/.h`：typed 内部模型 `VmDisplayList` 与唯一的 JSON 序列化；在结构体层做 retained diff，不基于字符串比较。
  - `vm_tiles.c/.h`：把全局帧切为瓦片；按命令签名只发变化块，`showLoading` / `hideLoading` 包住批次。
  - `clay_frame_adapter.c/.h`：Clay `RenderCommandArray` → `VmDisplayList`。
  - `vm_font.c/.h`：native stb_truetype 度量，口径对齐浏览器 wasm 光栅化；`vm-desktop/stb_font_wasm.c` 编出 `assets/stb_font_wasm.mjs/.wasm`。
  - `vm_theme.c/.h`、`vm_shell.c/.h`、`vm_menu.c/.h`：主题 token、任务栏外壳、弹出菜单。
  - `vm_clipboard.c/.h`、`vm_fs*.c`、`vm_sys*.c`、`vm_ipc.h`：guest 内剪贴板、文件系统/系统能力抽象与进程间协议定义。
  - `vm_server.c/.h`：C HTTP 服务：`GET /events`（SSE 帧与资源）、`POST /input`（原始输入）、静态资源白名单；`demo_jpx` 另注册 `/auto/<op>` 与 `/respack/<id>`。
  - `demo_wm.c`、`demo_jpx.c`、`demo_xaml.c`：分别演示 WM/多客户端指针、JPX 桌面外壳、jp_xaml 表单与主题热切换。
  - `jpx/`、`jpx_xaml/`：JPX 应用框架与声明式 UI 的解析、视图、主题预编译。
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
- **内部模型与序列化分离**：`VmDisplayList` 是 typed 结构体，JSON 只是当前序列化格式；diff 在结构体层做，不基于字符串。
- **自适应帧率**：有变化才出帧，空闲零帧；首帧与新连接必达。
- **瓦片增量**：48 块 100×100 瓦片覆盖 800×600 逻辑画布，只有变化块下发；物理尺寸按 `renderScale` 放大，贴屏 1:1。
- **资源一次性下发**：`definePattern` / `defineStyle` / `defineDrawProgram` / `loadImg` 等按 id 缓存；主题包由 `loadResPack(id)` 加客户端 GET 拉取。
- **字体同源**：guest 的 `vm_font` 与浏览器的 wasm 光栅化共用同一 TTF 与度量口径，不使用 Canvas `fillText`。

## 已知限制

- C 渲染器宿主没有 `fetch`，`loadResPack` 会被静默跳过；C 侧主题热切换当前不可用。
- `assets/font.ttf` 当前为宋体子集，覆盖 XP 中文 UI 用字；替换字体需同时满足 stb 轮廓渲染与两端一致性。
- 静态资源上限由 `vm_server.c` 决定（当前 16 MB），新增大资源需要核对。
- 现行测试为 `vm-desktop/test_*.exe --probe` 纯逻辑断言；浏览器验证依赖 `agent-browser` 与截图/像素检查，尚未形成自动回归套件。
