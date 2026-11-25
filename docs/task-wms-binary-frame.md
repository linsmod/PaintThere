# 任务：wms ↔ rdp 二进制帧通道（档位 A：少拷贝）

## 目标

把 `vm_wms` → `vm_rdp` 的帧载荷从 JSON 改为二进制 tile-op 编码；JSON 只在 `vm_rdp` 输出 SSE/WS 给浏览器时生成。保留现有 JSON 事件作为回退/调试路径，兼容 e2e 验收。

## 非目标

- 不做共享内存零拷贝（档位 B）：命名管道仍有内核拷贝；本任务只消除 wms 侧 JSON 序列化与 rdp 侧每客户端重复拷贝。
- 不改窗口元数据命令、`CMD_INPUT`、`CMD_WINDOW_CONTENT`：它们已是二进制定长结构。
- 不改浏览器协议：SSE/WS 仍输出 tile JSON。

## 当前链路（已确认）

```text
vm_app / vm_shell
  → CMD_WINDOW_CONTENT (CmdWindowContent: cmd_size + VmDrawCommand[])
  → vm_wms + vm_host.c
      VmWm_BuildFrame → VmDisplayList
      VmTileGrid_Build → VmTiles (config / setTile / loading ops)
      VmTiles_Finish() → JSON 字符串 (malloc)
  → EVT_FRAME / EVT_FULLFRAME (VmWmEvtFrame + JSON)
  → vm_rdp/main.c Wms_AsFrame() 取 JSON 指针
  → RdpServer_PushFrame(srv, json, len)
  → SSE "data: <json>\n\n" 给浏览器
```

关键点：`Wms_AsFrame()` 已是零拷贝，但 JSON 在 wms 生成、rdp 原样透传，且 SSE 多客户端各自 `memcpy` 一份到 `RdpOut`。

## 设计

### 1. IPC 新事件（并存，不替换）

在 `vm-desktop/wms/wms_ipc.h` 增：

- `EVT_FRAME_BIN = 1 << 12`
- `EVT_FULLFRAME_BIN = 1 << 13`

载荷头复用 `VmWmEvtFrame`（frame_id + len），紧跟 `len` 字节二进制 tile-op 流。magic/version 放在流头：

```text
u32 magic 'BFRM' | u16 version=1 | u16 flags | u32 reserved
u16 opCount
ops...
```

老 `EVT_FRAME` / `EVT_FULLFRAME` 保留。rdp 同时订阅新旧事件；启用开关 `--binary-frames` 时优先消费新事件，未收到新事件前仍接受旧事件。

### 2. 二进制 tile-op 编码

新增 `vm-desktop/vm_frame_bin.h/.c`，定义唯一线路编码（不是 `VmDrawCommand` 内存布局）：

- `VM_BIN_OP_CONFIG`：cols/rows/tileW/tileH/w/h
- `VM_BIN_OP_SET_TILE`：index + cmdCount + 每条命令的紧凑编码
- `VM_BIN_OP_SHOW_LOADING` / `HIDE_LOADING`

命令紧凑编码原则：type 用 u8，rect 用 4×i16 或 i32，`paint`/`text`/`imgId`/`tint` 用 u16 长度前缀 + UTF-8 字节，避免 `char[128]` 定长浪费。数值字段按命令类型只写存在的字段。

编码方向：`VmTiles` → 二进制 buffer。解码方向：二进制 buffer → `VmTiles`（rdp 用），再 `VmTiles_Finish()` 生成浏览器 JSON。这样 wms 和 rdp 共享同一套 tile 语义，JSON 只在 rdp 末端生成。

### 3. wms 侧改造

`vm-desktop/wms/vm_host.c`：

- `build_frame_json()` 拆成 `build_frame_tiles(h, full)` 返回 `VmTiles*`。
- 新增二进制发布：`VmTiles_FinishBin(tiles, &buf, &len)` → `VmWm_ServerBroadcastFrameBin(buf, len, full)`。
- 保留 JSON 发布（调试开关，默认关闭以省 CPU）。
- tile 对象与二进制 buffer 生命周期：一帧一份，广播完成后释放；服务端不持有指针。

`vm-desktop/wms/wms_server.c`：

- 新增 `VmWm_ServerBroadcastFrameBin` / `VmWm_ServerSendFrameBin`，复用掩码/ACK 逻辑，只改事件 bit 与载荷。

### 4. rdp 侧改造

`vm-desktop/rdp/main.c`：

- 订阅增加 `EVT_FRAME_BIN | EVT_FULLFRAME_BIN`。
- `on_wms_event` 对二进制事件用 `Wms_AsFrame()` 取 payload（同一头结构），调用新解码器 → `VmTiles*` → `VmTiles_Finish()` → `RdpServer_PushFrame`。
- 保留旧 JSON 事件分支。

`vm-desktop/rdp/rdp_server.c`：

- `RdpServer_PushFrame` 当前对每个 SSE 连接 `conn_send_parts` 做 memcpy。第一步先做引用计数 iovec：待写缓冲项支持“借用指针 + release 回调”，同一帧多个连接共享，写完成回调统一 release。
- 若实现成本过高，可先保留 memcpy，单独提交二进制帧通道；引用计数作为第二步独立提交。

### 5. 验证

- 单测：`test_frame_bin` 对 CONFIG/SET_TILE/SHOW_LOADING/HIDE_LOADING 各编码→解码→`VmTiles_Finish()`，与直接 JSON 结果逐字节比较（忽略 frameId 差异）。
- e2e：`pwsh tools/e2e_wms.ps1`，重点看 rdp 场景的 SSE 基线、增量帧、应用内容、输入回传。
- 字节对比：`inspect_vm_frame.py` 读 SSE 流，确认 tile 内容与旧路径一致；统计二进制 IPC 长度应小于 JSON。
- 浏览器：启动 `python tools/start_vm_desktop.py`，agent-browser 截图/像素检查，确认画面与交互不退化。

## 步骤与结果（2026-11-25 完成）

- [x] 写本任务文档
- [x] 定义 `vm_frame_bin.h/.c`：`VmBinWriter` 增量写 CONFIG/SET_TILE/SHOW_LOADING/HIDE_LOADING，`VmFrameBin_ToTiles` 解码；命令用 typed 紧凑编码（u8 type + 长度前缀字符串 + IEEE-754 float 位模式）
- [x] `VmTiles` 同时持有 JSON buffer 与 `VmBinWriter`；`SetTileJson` 同时接收类型化命令写二进制镜像；`VmTiles_FinishBin` 输出二进制帧
- [x] wms_ipc 新增 `EVT_FRAME_BIN = 1<<12` / `EVT_FULLFRAME_BIN = 1<<13`，`EVT_ALL` 扩到 `0x3FFF`
- [x] wms_server 新增 `VmWm_ServerBroadcastFrameBin` / `SendFrameBin`，复用 `VmWmEvtFrame` 载荷头
- [x] vm_host 默认双发 JSON+BIN（灰度开关 `VM_WMS_JSON_FRAMES`：0=仅 BIN，1=仅 JSON，未设=双发）
- [x] vm_rdp 只订阅 BIN 事件，解码后 `VmTiles_Finish()` 转 JSON 再 PushFrame/PushFullframe；旧 JSON 事件保留给 `test_wms_rdp`/e2e basic 场景
- [ ] （第二步，未做）RdpOut 引用计数 + iovec 少拷贝：当前 rdp 解码后仍每连接 memcpy JSON
- [x] 全量构建 + `test_frame_bin` + `e2e_wms.ps1` 61/61
- [x] 浏览器验证：blue gradient + hello 窗口 + 任务栏正常；rdp 日志 `fullframe(bin) frame_id=82 len=19122`

### 验证数据

- `test_frame_bin`：同一组 CONFIG/SHOW_LOADING/2 条命令/HIDE_LOADING，JSON 383 字节、BIN 172 字节（约 45%），解码后再序列化 JSON 与原始逐字节一致。
- 构建：`python tools/build_vm.py` 全量 exit 0。
- `pwsh tools/e2e_wms.ps1`：61/61 checks passed。
- 浏览器：`python tools/start_vm_desktop.py --port 19055` + agent-browser，截图 `temp/binframe_desktop.png`，画面完整。
- rdp 日志确认走二进制：`[rdp] fullframe(bin) frame_id=82 len=19122`。

### 已知限制

- 双发期间 wms 一帧序列化两次，CPU 尚未节省；切 `VM_WMS_JSON_FRAMES=0` 后只发 BIN，但 `test_wms_rdp` 需要 JSON（e2e basic 场景）。后续可让 test_wms_rdp 也订阅 BIN 后默认切仅 BIN。
- `RdpOut` 仍是每连接 memcpy，未做引用计数 + iovec（任务第二步）。
- 偶发 `rasterizeText failed: -2`：字体 wasm 异步加载竞态，既有行为，下一帧补画，非本任务引入。

## 风险与注意

- 不要直接 memcpy `VmDrawCommand`：字段含定长 char 数组，浪费带宽且绑死内部布局。
- 旧客户端不认识新 bit：并存事件、订阅 old|new，开关切换。
- `EVT_FRAME` 的载荷头 `VmWmEvtFrame` 是 8 字节，二进制流可直接复用，降低 SDK 改动。
- 资源包 `EVT_RESOURCES` / VMRP 暂不二进制化，仍走 JSON/VMRP。
- 改代码前按工作流停掉 `vm_wms/vm_rdp/vm_shell/vm_app`，避免 Windows 锁 exe。
