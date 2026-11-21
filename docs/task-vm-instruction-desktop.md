# 任务：VM 指令化桌面与窗口管理器（方案定义）

> 本文是设计决策与实施计划，不是现状。目标架构落地后另写 `arch-vm-instruction-desktop.md` 描述现状。

## 目标与边界

在 RVVM userland VM 的 guest 内实现桌面 + 窗口管理器，界面通过“非像素指令流”送到浏览器渲染。

**硬边界（用户确认）：**

- 跨端协议只暴露**绘制原语**和**原始输入事件**，不暴露任何 WM/Desktop API。
- 禁止 `createWindow` / `destroyWindow` / `setBounds` / `raise` / `focus` / `close` 等窗口级指令。
- 应用管理、窗口树、层叠、焦点、命中测试、事件分发全部是 guest 内部事务，对跨端不可见。
- 多客户端看到的是 guest 合成后的同一画面；彩色指针属于绘制层叠加，不是 WM API。

## 实测环境（2026-11-21）

来源：RVVM 仓库 `H:\github_repos\RVVM`，release 树 `release.test.windows.x86_64`，临时实例 `probe-desktop`（端口 7951，探查后已 `us rm`）。

- Guest：`Linux 6.6.6 RVVM userland riscv64`，Alpine + busybox。
- 无 `/dev/fb*`、无 `/dev/dri`、无 Xorg/Weston/Wayland；`/proc/net/tcp`、`/proc/devices` 不存在。
- `/data/app/` 有 `test_render*` / `test_game_activity` / `vpsessiond`；直接运行 `test_render.exe` 导致 ssh 连接重置，**不作为基石**。
- guest ↔ 宿主可靠通道：ssh（127.0.0.1:port）；supervisor 另有 WebSocket 终端桥。
- 启动/管理：`python tools/supervisor/us.py --token-file <release>\runtime\supervisor\token start|exec|list|rm`。token 必须显式指定。
- 遗留实例（7910 / 7917 / 7999 orphan，7949 crashed）不是本任务创建，**不要动**。

## 已确认的技术决策

### Clay 的定位

- 来源：`H:\github_repos\clay`（本任务克隆，工作区外）。
- Clay 是**布局 + display-list 生成库**，不是 WM。README 明确“不处理窗口相关任务”。
- 用法：**每个窗口一个 `Clay_Context`**，同一线程内 `Clay_SetCurrentContext` 切换（Clay 不支持多线程同时渲染不同 context）。
- 长处：单头 `clay.h`、零依赖、arena 内存、输出按 z 序排好的 `Clay_RenderCommandArray`、带 `id`/`boundingBox`/`zIndex`、自带可见性裁剪、retained-mode diff 友好。
- 短板：无窗口树/焦点/拖动/键盘事件；文本只做 measure，光栅化交给 renderer；多 context 非线程安全。

### WM 自建

guest 侧自写最小 WM，负责：窗口树、z 序、焦点链、拖动/resize、最小化/恢复、命中测试、原始输入路由、多客户端焦点仲裁。WM 消费 Clay 输出的 render commands 做合成，再送跨端。

### 跨端协议

- 只保留绘制原语 + 原始输入 + `frameId`（仅帧同步，无窗口语义）。
- 通道分三步：原型走 ssh stdout/stderr 长连接；中期在 supervisor 加专用 WebSocket 端点；长期视性能再考虑 core 通道。
- 坐标统一为 guest 逻辑桌面坐标，输入回传时带 `clientId`。

## Clay 命令 → 跨端绘制原语映射

| Clay `RenderCommandType` | 跨端指令 | 说明 |
|---|---|---|
| `RECTANGLE` | `drawRect(x,y,w,h,color,cornerRadius)` | 圆角半径随命令透传 |
| `BORDER` | `drawBorder(rect,color,widths,cornerRadius)` | 或拆成多个 `drawRect` |
| `TEXT` | `drawText(x,y,w,h,text,fontId,fontSize,color,letterSpacing,lineHeight)` | 见“文本方案” |
| `IMAGE` | `drawImg(imgId,x,y,w,h,tint,cornerRadius)` | `void* imageData` 由 guest 注册为 `imgId`，不透传指针 |
| `SCISSOR_START` / `SCISSOR_END` | `clipPush(rect)` / `clipPop()` | 与现有前端 `ctx.clip` 对应 |
| `OVERLAY_COLOR_START/END` | `overlayPush(color)` / `overlayPop()` | 前端可用 `globalAlpha`/合成色实现 |
| `CUSTOM` | 由 adapter 决定 | 逃生口，承载新增原语，不进主协议 |
| `NONE` | 跳过 | — |

Clay 输出的命令已按 z 序升序，跨端保持该顺序即可。`id` 用于 retained diff；`boundingBox` 用于脏矩形。

## 文本方案（决策）

**采用 A：跨端传文本原语，两端用同一字体表与度量函数。**

- guest 侧给 Clay 注入 `Clay_SetMeasureTextFunction`，度量使用约定的字体表（fontId → 字体名/ttf）。
- 跨端 `drawText` 带 `fontId/fontSize/letterSpacing/lineHeight/text`，浏览器按同一字体表用 Canvas 绘制。
- 风险：两端字体度量/换行不一致。阶段 2 必须做一致性验证（同一段文本在 guest 预期矩形与浏览器实际绘制对比）。
- 回退方案 B：guest 侧把文本光栅化成图片，跨端只发 `drawImg`。一致性最好但退回像素路径，仅在 A 验证失败时启用。

## WM 最小状态机

窗口结构（guest 内部，不跨端）：`id, title, rect, zIndex, clayContext, renderCommands, state, focused`。

状态流转：

```text
created → mapped（可见）→ focused / unfocused
       → dragging / resizing → minimized → restored → destroyed
```

事件路由：

```text
浏览器原始输入(type, clientId, x, y, button/key, wheel)
  → guest WM 按 z 序命中窗口
  → 窗口内 Clay_Context: Clay_SetPointerState / Clay_UpdateScrollContainers / 应用回调
  → Clay_BeginLayout → 声明 → Clay_EndLayout
  → render commands → adapter → 跨端指令流
```

多客户端：所有客户端共享同一画面；焦点状态只在 guest；指针由跨端层用 `drawImg` 叠加（复用已实现的多客户端彩色指针）。

## 分阶段实施与验收

- [x] **阶段 0 协议定稿**：绘制原语 JSON schema、`frameId`、原始输入事件 schema。验收：本文定稿，前端 `app.js` 原语清单对齐。
  - 协议设计见 `docs/design-vm-draw-protocol.md`（冻结设计）。
  - `app.js` 已注册原语：`clear` / `drawRect` / `drawBorder` / `drawText` / `drawImg` / `clipPush` / `clipPop` / `overlayPush` / `overlayPop`，并导出 `registerFont` / `drainPrimitiveState`。
  - `main.js` 已识别 `{type:'frame', frameId, commands}` envelope，记录 `currentFrameId`，上行事件带 `frameId` 标记，帧边界清理原语状态栈。
  - 验证：两个项目 `dotnet build` 通过；未做浏览器运行时验证（阶段 1 起配合 adapter）。
- [x] **阶段 1 adapter + C SSE 通道**：Clay `RenderCommandArray` → 跨端指令，端到端验证通过。
  - `vm-desktop/clay_frame_adapter.c/.h`：Clay 命令 → `{type:'frame',frameId,commands}` JSON；颜色转 `rgba(...)` 字符串，坐标拆为独立参数与 `rect` 数组。
  - `vm-desktop/demo_layout.c/.h`：共享的单窗口 Clay 布局，供 `demo_frame` 与 `demo_server` 复用。
  - `vm-desktop/vm_server.c/.h`：C 跨平台 HTTP 服务（Windows winsock / POSIX socket），`GET /events` SSE 推帧、`POST /input` 收原始输入、白名单静态文件（`/` 页面 + `app.js` + `systemColors.js`）。服务端不解释帧或输入语义。
  - `vm-desktop/demo_server.c`：持续重建 Clay 布局推帧，输入 handler 打印原始 body。
  - `ConsoleApp2/ServerWebSocketImpl.cs` 另有 WebSocket 纯传输帧转发（保留，供既有通道使用）。
  - 验证（2026-11-21）：gcc 编译两个 demo 通过；curl 验证静态资源 200 与 POST 200；SSE 连续收到 frameId 递增的帧；浏览器渲染 demo 帧，背景像素精确 64896（800×600 − 752×552），按钮像素 5268（140×40 减圆角），截图 `vm-desktop/demo_render.png`；页面点击后服务端打印原始输入 JSON。
  - 修复过的坑：`Clay_Color` 是 `float` 不是 `uint8_t`；`Clay_SetMeasureTextFunction` 第三参是 `void*`；C HTTP 服务用 `Connection: close`，Node `fetch` 连接复用会触发 undici 断言，改用 curl 或浏览器原生请求。
  - 已知行为：demo 的 frameId 在每次 provider 轮询时递增，即使没有 SSE 客户端；正式 WM 应只在发布帧时递增。
- [x] **阶段 2 文本一致性验证（wasm 光栅化方案）**：
  - 决策更新：不用 Canvas `fillText`，改为把 mystbfont_c 的 stb_truetype 编成 wasm，guest 与浏览器跑同一份代码、用同一 TTF。
  - `vm-desktop/stb_font_wasm.c`：导出 `loadFont` / `measureText` / `rasterizeText` / `alloc` / `freeMem`，直接基于 stb_truetype；Emscripten 7.0.0 编译，产物 `stb_font_wasm.mjs`（7.5KB）+ `stb_font_wasm.wasm`（32KB）。
  - `ConsoleApp2/font_wasm.js`：加载 TTF base64、光栅化文本为 RGBA、染色后绘制；`app.js` 的 `drawText` 改走此路径，新增 `loadFont` 命令，未就绪时由 frame 重放补齐。
  - 验证：native `font_probe` 与 wasm 对三段文本度量完全一致（Hello World=91、跨端图绘指令流=70、长英文=247）；浏览器中 wasm 光栅化中文结果 `w=74,h=23,alpha=378` 与 Node 一致；端到端 SSE 帧渲染标题，蓝色文字像素 206，console 无错误，截图 `vm-desktop/font_render.png`。
  - Emscripten 安装：`H:\github_repos\emsdk`（emcc 7.0.0，装在 H 盘因 C 盘空间不足）；新 shell 需先设置 PATH/EMSDK 环境变量，构建命令见 tools/INDEX.md。
- [ ] **阶段 3 guest WM 最小实现**：窗口创建/销毁/移动/z 序/焦点。验收：两个窗口可切换焦点、拖动。
- [ ] **阶段 4 输入闭环**：浏览器原始输入 → guest → Clay pointer state。验收：hover/click 在浏览器画面生效，文本输入可达焦点窗口。
- [ ] **阶段 5 多客户端与指针**：复用彩色指针 + guest 焦点仲裁。验收：双客户端互见，焦点唯一。
- [ ] **阶段 6 性能优化**：retained diff、脏矩形、frameId 绑定、限流。验收：60fps 下延迟可接受。

预计耗时：阶段 0–2 约 1–2 天；阶段 3–5 约 2–4 天；阶段 6 视实测而定。

## 恢复点

- 设计起点：本文 + `docs/arch-跨端图绘指令流.md`（现有指令流架构）+ `docs/task-multi-client-cursor.md`（多客户端指针）。
- Clay 源码：`H:\github_repos\clay\clay.h`、`README.md`、`renderers/terminal/clay_renderer_terminal_ansi.c`（最简非像素 renderer 参考）。
- VM 环境：`H:\github_repos\RVVM`，token 在 `release.test.windows.x86_64\runtime\supervisor\token`。
- 下一动作：阶段 3，guest WM 最小实现——窗口创建/销毁/移动/z 序/焦点，输出合并后的帧。字体方案已定，不再需要 Canvas 度量对比。

## 已知限制

- 文本一致性未验证；阶段 2 前不得视为已解决。
- `test_render.exe` 会重置 ssh 连接，不依赖它。
- Clay 无键盘焦点/拖放/IME，全部由 WM 或应用自行实现。
- 当前未写任何代码；本文只有方案定义与计划。
