# 任务：Remote Tiles 协议替换分层合成指令

> 执行记录（2026-11-22）。本文自包含，可在会话中断后恢复。不是长期待办清单。

## 背景与问题

提交 `86ceb00` 把协议改成 `{"type":"ops"}` + `create/bind/clear/clearScreen/composite`，层 id 为 `bg` / `w1` / `w2` / `cursor`，由服务端决定 z 序、窗口局部坐标和屏幕脏区。前端因此理解「窗口 / 图层 / 合成」——桌面业务逻辑泄漏到跨端协议。

用户决策（2026-11-22）：

- tiles 是**远端持有的小画布网格**，由 WM 下发的指令管理（配置 / 更新 / 销毁），不是 guest 光栅化位图后传输。
- 跨端只保留 tile 管理语义；窗口、图层、z 序、脏区、`composite` 全部收回 guest。
- 额外提供 `showLoading` / `hideLoading`：一批指令执行期间显示遮罩且不呈现，批次结束后一次性贴齐，画面才稳定。
- 网格参数走协议可配置，demo 先用 100×100（800×600 → 8×6 = 48 块）。

## 目标协议

下行 envelope：

```json
{"type":"tiles","frameId":42,"ops":[
  {"op":"showLoading"},
  {"op":"config","cols":8,"rows":6,"tileW":100,"tileH":100,"w":800,"h":600},
  {"op":"setTile","index":12,"cmds":[{"id":"...","cmd":"drawRect","args":[...],"rect":[...]}]},
  {"op":"hideLoading"}
]}
```

- `config`：定义网格，全量帧下发，幂等；尺寸变化则重建 tile canvas。
- `setTile`：`index = row*cols+col`；前端先清空该 tile canvas，再在**tile 局部坐标**执行 `cmds`（沿用现有绘制原语）。
- `showLoading` / `hideLoading`：成对包裹一批 `setTile`。`showLoading` 后暂停 present 并画遮罩；`hideLoading` 时一次性把所有 tile 贴到主画布。
- 删除 `VmOps` 的 `create/bind/clear/clearScreen/composite`；`app.js` 的 `setDrawTarget` 仅作为前端内部渲染机制保留，不再是协议概念。
- 资源（`definePattern` / `defineStyle` / `defineDrawProgram` / `startDrawProgram` / `loadImg` 光标）仍走 `event: resources`。
- 帧率策略不变：无变化不出帧；新连接先 resources + fullframe。字体位图/图片解码就绪后由前端用 tile 命令快照本地重放，不触发服务端重发。

## 实现步骤

### 1. 序列化层（`vm-desktop/vm_serialize_json.c/.h`）

- 删除 `VmOps` 全部 API。
- 新增 `VmTiles` API：`VmTiles_Create/Destroy`、`VmTiles_SetFrameId`、`VmTiles_Config`、`VmTiles_ShowLoading`、`VmTiles_HideLoading`、`VmTiles_SetTile`、`VmTiles_Finish`。
- `VmTiles_Finish` 输出 `{"type":"tiles","frameId":N,"ops":[...]}`；复用 `emit_command` 序列化 tile 内命令。

### 2. 新模块 `vm-desktop/vm_tiles.c/.h`

- `VmTileGrid`：网格参数 + 每块 `cur` / `prev` `VmDisplayList`（上限 128 块）。
- `VmTileGrid_Init(grid, cols, rows, tileW, tileH, w, h)`。
- `VmTileGrid_Build(grid, frame, full, tiles)`：
  - 逐块遍历 frame 命令：绘制命令按 `rect` 与 tile 相交才纳入；`clipPush/clipPop/overlayPush/overlayPop` 每块都纳入以保持配对；`loadImg` 跳过（已进 resources）。
  - 复制命令并把所有坐标减去 tile 原点。
  - 与 `prev` 用 `VmDisplay_CommandEqual` 比较；仅变化的块发 `setTile`。
  - 有更新时前后加 `showLoading` / `hideLoading`；`full` 时加 `config`；无任何变化返回 0（不出帧）。
  - `full` 不修改 `prev`（与旧 `BuildOps(full)` 语义一致，late client 不影响增量基线）。
- 加入 `tools/INDEX.md` 与构建命令（demo_wm 链接新 .c）。

### 3. guest 侧接入（`vm-desktop/demo_wm.c`、`vm_wm.c/.h`）

- `demo_wm.c`：删除旧 `DemoWindowLayer` / `BuildOps`；改用 `VmWm_BuildFrame` + `VmTileGrid_Build`。
- 光标 SVG 资源移入 `BuildResources`；`vm_wm.c` 新增 `VmWm_CursorDataUrl()` 提供 data URL，`append_cursor_commands` 不再逐帧发 `loadImg`。
- 删除 `VmWm_BuildWindowLayer` / `VmWm_BuildCursorLayer`（本轮引入且已无用）。
- `FrameProvider` 仍按 `g_dirty` 自适应；`FullFrameProvider` 保留供新客户端建立基线；`/redraw` 端点与 `VmRedrawHandler` 已删除。

### 4. 前端（`assets/vm_renderer.js`、`assets/app.js`）

- `vm_renderer.js`：删除 `layers` / `registerLayer` / 旧 `execOps`；新增 tile 管理：`config` 创建 48 个小 canvas，`setTile` 清空并以 `setDrawTarget(tileCtx)` 执行 cmds，`showLoading` 画遮罩并暂停 present，`hideLoading` 调 `present`（清主画布后按 index 贴所有 tile）。
- `tileCmds` 保存每块最后一份命令快照；字体/图片就绪回调经 `scheduleReplay` 合并到一次 rAF，重画受影响的 tile 后按需 present，不再 POST `/redraw`。
- 无 `showLoading/hideLoading` 的帧在末尾兜底 present。
- `__vmRenderer` 增 `tiles` / `loading` 字段，便于宿主探针验证。
- `app.js`：`loadImg` 图片 onload 后触发 `textNeedsReplay()`，否则资源-only 的光标要等到下一次状态变化才出现；更新 `setDrawTarget` 注释，不再提「层绑定」。

### 5. 文档与索引

- `docs/design-vm-draw-protocol.md`：把「图层模型」替换为 tiles 协议说明；更新 envelope / SDK 前瞻性相关段落。
- `docs/task-vm-instruction-desktop.md`：追加本轮决策与结果指针。
- `workflows/wf-vm-instruction-desktop.md`：更新常用构建命令（链接 `vm_tiles.c`）、验证清单（tiles envelope / loading / 48 块）。
- `docs/INDEX.md`、`tools/INDEX.md`、`workflows/INDEX.md` 按需补索引。

### 6. 验证

```powershell
\vm-desktop
gcc -std=c11 -I"H:\github_repos\clay" -I. demo_wm.c clay_frame_adapter.c vm_display.c vm_serialize_json.c vm_tiles.c vm_wm.c vm_server.c -lws2_32 -o demo_wm.exe
```

- `curl -N --max-time 3 http://127.0.0.1:9000/events`：`event: resources` + `event: fullframe`，fullframe 为 `type:tiles`，ops 含 showLoading/config/48 个 setTile/hideLoading。
- 空闲不广播；一次 `POST /input` 后只发变化的 tile（远少于 48）。
- 浏览器（agent-browser）打开页面：`__vmRenderer.ready=true`、`error=''`、`loading=false`、`tiles=48`；像素检查背景/标题/按钮；点击 B 标题栏后焦点色变化；双客户端彩色指针仍可见；console 无错误。
- C 渲染器宿主沿用 `python tools/start_vm_renderer.py`，`[STATE]` 探针已由用户注释，保持不动。

## 恢复点

- 用户未提交的 `vm-desktop/renderer/main.c`（注释 printf）与未跟踪的 `temp/` 不属于本任务，不要改动或清理。
- 关键既有文件：`vm_wm.c` 的 `VmWm_BuildFrame`（全局坐标完整帧）、`vm_serialize_json.c` 的 `emit_command`、`assets/app.js` 的 `commandHandlers` / `setDrawTarget`。
- 若会话中断：从步骤编号继续；每完成一步更新本文档的进度勾选。

## 进度

- [x] 设计确认（用户：tiles 由 WM 指令管理远端资源，不是传输；小图；showLoading/hideLoading）
- [x] 1 序列化层：`VmOps` 已删除，`VmTiles` / `VmTiles_SetTileJson` 已实现。
- [x] 2 vm_tiles 模块：网格切分 + 命令签名差异，只有变化块发 `setTile`。
- [x] 3 guest 接入：`demo_wm.c` 改用 `VmWm_BuildFrame` + `VmTileGrid_Build`；光标资源进 `resources`。
- [x] 4 前端：`vm_renderer.js` 管理 48 个小画布与 loading 批次；`app.js` 图片就绪触发重放。
- [x] 5 文档：设计、任务、工作流与三份索引已更新。
- [x] 6 验证：gcc 构建通过；SSE fullframe 含 48 块 + loading 边界；指针移动增量帧仅 1–2 块；浏览器 48 tiles、焦点蓝灰互换、光标着色、console 无错误，截图 `temp/wm_tiles_focus.png`。
- [x] 7 `/redraw` 移除：用户指出该设计责任倒置、全局广播、自激冗余且无自愈；改为前端保存 tile 命令快照，资源就绪后合并到本地 rAF 重放。服务端删除 `POST /redraw` 与 `VmRedrawHandler`。
- [x] 8 槽位满处理：用户指出新连接挂起不可接受。`vm_accept` 在槽位满时也 accept，回 `event: control` / `{"op":"serverFull","retryMs":2000}` 后断开；前端收到后关闭 EventSource 并退避重连。

## 已知限制

- 2026-11-22 追加：用户运行 `build_vm_renderer.py && start_vm_renderer.py` 后浏览器只剩背景。根因是 `demo_wm.exe`（9:36:44）早于协议改动，9000 仍发旧 `ops`，新前端只认 `tiles`。停掉旧进程并用当前源码重建 `demo_wm.exe` 后，9000 的 SSE 已确认 `type:"tiles"` + `config` + loading 边界；浏览器 48 tiles、无 console 错误。C 渲染器进程也正常启动并持续上报输入。
- 根因修复：用户指出两个 C 项目不该分开构建。改为统一 `vm-desktop/CMakeLists.txt` + `tools/build_vm.py`，一次产出 `demo_wm.exe` 与 `vm_renderer.exe`（都在 `vm-desktop/build/`）；启动脚本只检查产物、停旧实例后启动。旧的 `build_vm_desktop.py`、`build_vm_renderer.py`、`renderer/CMakeLists.txt` 已删除。
- C 渲染器宿主对 `data:image/svg+xml` 的 `loadImg` 失败（日志 `[IMAGE] load failed`），因此原生窗口里彩色箭头不可见。这是 canvas-cpp 宿主既有能力缺口，与 tiles 协议无关；若需要，可把光标资源从 SVG data URL 改为 PNG data URL。
- 宿主 `[STATE]` 探针被用户未提交改动注释掉，C 渲染器内部像素未独立取样；原生窗口的画面由同服务端浏览器路径佐证。
