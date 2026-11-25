# 任务：vm_rdp 按 clientId 建会话

> 执行记录，可用于会话中断恢复；当前架构见 `docs/arch-vmdesk.md`。

## 背景

`vm_rdp` 原来只按 16 条 TCP 连接截断，连接与客户端没有身份关联：每条 client 占 2 条
连接（`/events` SSE + `/ws`），标签页刷新、重连、复制的标签页会留下旧连接，很快把
16 条占满；占用满后新连接没有稳定说法，客户端侧表现为请求 pending。

## 目标与决策

1. 连接按 `clientId` 归入会话，一个 `clientId` 只允许一条会话。
2. 一条会话最多一条 SSE + 一条 WS；同角色再来新连接 = 重连/新标签页接管：先断旧的，再绑新的。
3. `clientId` 存在 `localStorage`（用户决策：每浏览器一个，不按标签页），因此同浏览器多标签页共用一条会话，新标签页接管画面。
4. 容量：会话上限 `RDP_MAX_SESSIONS = 8`（≈ 原来的 16 条 viewer 连接），连接硬上限
   `RDP_MAX_CONNS = 64`（含静态资源等短连接）。超限**直接断开**，不把连接挂在队列里。
5. 旧标签页必须知道自己是被接管，而不是断网：旧 SSE 收 `event: control` /
   `{"op":"takenOver"}`，旧 WS 收 close 4001；前端据此停止重连并显示接管提示。

## 改动

### `vm-desktop/rdp/rdp_server.c`

- 新增 `RdpSession { clientId, sse, ws }` 与 `RdpServer.sessions[] / sessionCount`，`RdpConn.session` 回指所属会话。
- `query_param()`：`/events?client=<id>`、`/ws?client=<id>` 取 id，只接受 `[A-Za-z0-9._-]`；缺省仍可连接（探针/旧前端），但明确记日志「不做会话去重」。
- `session_acquire()`：按 id 查表，没有就占空槽；表满返回 NULL。
- `session_bind()`：同角色旧连接先 `conn_takeover_close()` 再占用槽位；槽位写入新连接，旧连接关闭回调只在「槽位仍等于自己」时清空，避免把新连接带走。
- `conn_takeover_close()`：SSE 发 `takenOver` 控制事件、WS 发 close 4001，`closeAfterFlush` 保证通知先出内核再关；就绪后再关闭，前端不会误判成断线重连。
- 会话满：`start_sse` / `start_ws` 直接回 `503 Service Unavailable` 并断开（不升级 WS、不排队）。
- 连接硬上限：`reject_connection()` 仍走 accept + 立即 close；同时修掉原来把 `uv_tcp_t` 放在栈上、`uv_close` 回调时已越界的隐患（改为堆分配 + `on_reject_closed` 释放）。

### `assets/vm_renderer.js`

- `CLIENT_ID` 改从 `localStorage` 读写（`vmCid`），失败时退化为每次加载一个新 id。
- `/events`、`/ws` 都带 `?client=<id>`。
- `control` 事件新增 `takenOver`：置 `takenOver` 标志、关闭 EventSource 与 WS、显示「会话已被其他标签页接管」覆盖层；此后 `connectEvents` / `connectInput` 都不再重连。
- WS `onclose` 收到 4001 同样按接管处理。
- 覆盖层文字由常量改为变量 `disconnectText`，断线文案不变。

### `tools/`

- `probe_ws_input.py`：`--client-id` 同时写进 `/ws?client=<id>` 与输入体。
- 新增 `probe_rdp_session.py`：SSE 接管、SSE+WS 共存、WS 接管、会话表满 503、槽位回收，5 组共 13 条断言。

## 验证

```powershell
python tools/build_vm.py
# 独立端口起 vm_rdp（不需要 vm_wms，SSE/WS 层就能验）
python tools/probe_rdp_session.py --port 19099
```

探针结果（2026-11-25）：13 条全部通过——旧 SSE 收到 `takenOver` 后关闭、新 SSE 保持；
同 clientId 的 SSE 与 WS 互不驱逐；旧 WS 收到 close 4001；第 9 个 clientId 得 503 而已
在表里的 clientId 重连仍 200；关闭后槽位可再用。

浏览器验证（`agent-browser`，临时桌面 `vm_wms + vm_shell + vm_app + vm_rdp`，端口 19051）：

- 第一个标签页：`takenOver:false`、`frames:1`、`localStorage.vmCid` 有值。
- 开第二个标签页（同源同 id）：正常出帧。
- 切回第一个标签页：`takenOver:true`、`disconnected:true`、`input:"http"`（WS 已断且不重连），
  画面为冻结帧 + 居中「会话已被其他标签页接管」；像素取点 `bg=24,26,32,199` 即覆盖层颜色。
- 服务端日志：`client 'd3vl29vxo9j' takes over: closing previous ws/sse connection`，
  之后无重复重连记录（旧页面没有抢回会话）。

## 恢复点

若会话中断：源码改动集中在上述三个文件，逐条对照「改动」章节即可确认是否已应用；
验证用 `tools/probe_rdp_session.py`（不需要上游 wms），浏览器验证按上面步骤重跑。
