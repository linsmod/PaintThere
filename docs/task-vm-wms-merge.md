# 任务：WMS 进程化合并（wmwms + vm-desktop）

> 自包含执行文档（2026-11-24 立项，同日按 wmwms 协议 v2 修订）。
> 可在会话中断或上下文丢失后据此恢复。
> 上游设计：`docs/design-vm-wms-process.md`（进程模型，**传输层部分已被本文取代**）、`docs/design-vm-ipc.md`（线路协议，**协议形态已被取代，仅作决策历史**）。
> 外部来源仓库：`H:\myGitRepos\wmwms`（用户实现并跑通的独立窗口管理服务，协议 v2）。
> 交接说明：该仓库的 `SEAM.md` 是接入契约的原始出处，本文与它冲突时以代码为准，并把差异记在 §5.7。
> 进度见文末「进度」，每完成一步更新勾选。

## 1. 目标与验收

把当前单进程桌面拆成四个进程，使外壳与应用的崩溃不互相传播，多个呈现端共享同一份合成结果。

验收定义（每条都可手工复现）：

1. `taskkill /F /IM vm_shell.exe` → 应用窗口内容与输入继续工作，任务栏消失；重启外壳后条目、焦点高亮、工作区正确重建。
2. `taskkill /F /IM demo_jpx.exe` → 该应用窗口消失，任务栏与其他应用不受影响，浏览器连接不断。
3. `taskkill /F /IM vm_rdp.exe` → 窗口状态与内容保留；重启 rdp 后浏览器重新拿到 fullframe 恢复画面，应用无需重推内容。
4. 跨端协议（SSE / tiles / POST /input / respack）零改动，浏览器端代码不动。
5. 单进程模式保留可用，`test_frame --probe` 等纯逻辑测试不依赖 IPC。

## 2. 现状基线（2026-11-24 实测/核对）

### 2.1 当前项目

`demo_jpx` 一个进程同时承担：窗口管理、外壳（任务栏/菜单/设置面板）、Clay UI、HTTP/SSE、tiles 切分。

四段硬耦合（详见 `design-vm-wms-process.md` §1）：

- 外壳由应用宿主创建：`jpx_app.c:325-329`
- 外壳直接改工作区：`vm_shell.c:736-737`
- 外壳直接调 `VmWm_*` 控制 API：`vm_shell.c:497-527`、`vm_shell.c:555-573`
- 共享 `g_jpx` 单例与 Clay 进程级 current context

关键结构事实：**窗口内容缓冲本来就在窗口管理器里**。

```61:69:vm-desktop/vm_wm.h
typedef struct {
    VmDrawCommand commands[VM_WM_LAYER_CAPACITY];
    int count;
    int contentDirty;
    int chromeDirty;
    uint32_t builtFrame;
    const char *dirtyWhy;
} VmWindowLayer;
```

`VmWindow.layer` 是内嵌成员（`vm_wm.h:99`）。合成、差分、切块也都在这一侧，所以「内容 + 合成 + 切块留在 vm_wms 进程」是现状的自然延续。

### 2.2 wmwms（外部仓库，协议 v2）

实测规模（本次统计，不含 `build/`）：

| 位置 | 行数 | 说明 |
|---|---|---|
| `include/` | 757 | 7 个头文件：`vmwm_ipc.h` 348、`vmwm_client.h` 109、`vmwm_host.h` 73、`vmwm_server.h` 69、`vmwm_buf.h` 69、`vmwm_peer.h` 47、`vmwm_codec.h` 42 |
| `src/` | 2433 | `wm_server.c` 1248、`vmwm_client.c` 752、`vmwm_peer.c` 138、`vmwm_mock_host.c` 130、`main.c` 86、`vmwm_codec.c` 67、`vmwm_log.h` 12 |
| `test/` | 812 | `test_wms_ipc.c` 271、`test_app.c` 185、`test_shell.c` 156、`test_rdp.c` 125、`test_peer.c` 75 |
| `scripts/` | 242 | `run_e2e.ps1` |

该仓库自报的验证结果（`SEAM.md` §14，**本次未复跑**）：`test_wms_ipc` 36/36、`test_peer` 7/7、e2e 23/23；四进程在线、内容被采纳、点击命中后转局部坐标、RDP 收到 fullframe + 增量帧 + 资源包；APP 强杀后一个 tick 内回收窗口；外壳退出后窗口保留、新外壳枚举到同一 `win_id`。

**它提供什么**（`SEAM.md` §1）：窗口表与 `win_id` 分配、位置尺寸、映射、标题、class、size hint、transient、z 序、`created_seq`、焦点仲裁、工作区与主题的**广播**、事件分发、内容通道（接收 + 校验代次 + 交宿主）、输入路由（画布 → 命中 → 抬升聚焦 → body 局部）、帧下发（把宿主产出的 JSON 打成 `EVT_FRAME`）、连接生命周期与进程关联（`vmwm_peer`）。

**它不提供什么**：合成、瓦片切分、retained diff、JSON 序列化、装饰绘制、主题资源解析、资源包生成、`VmDrawCommand` 的定义、HTTP/SSE、进程管理（无 PROCMAN）。

### 2.3 测试基线

`docs/test-timing.md`：`test_frame` 120 断言 / 30 ms、`test_menu` 30 / 38 ms、`test_sys` 16 / 53 ms、`test_text_edit` 66 / 13 ms、`test_jpx_xaml` 47 / 47 ms、`test_jpx_xaml_theme` 104 / 114 ms、`test_fs` 19 / 9 ms。全部支持 `--probe`。

## 3. 目标架构

### 3.1 四类进程

```text
                      浏览器 / C 渲染器
                            ▲
                            │ SSE: tiles JSON
┌───────────────────────────┴──────────────────────────────────┐
│  vm_rdp  远程桌面服务器（薄）                                  │
│    uv_tcp :9000   HTTP / SSE / 静态资源 / /font.ttf           │
│    缓存最近一次 fullframe，新连接先发它                          │
│    POST /input → Wms_SendInput（画布坐标，不等 ACK）             │
│    不持有窗口表、不持有内容、不做合成、不切瓦片                    │
└───────────────────────────┬──────────────────────────────────┘
                            │ EVT_FRAME / EVT_FULLFRAME / EVT_RESOURCES
                            │ CMD_INPUT（MSG，单向）
┌───────────────────────────┴──────────────────────────────────┐
│  vm_wms  窗口管理服务                                          │
│                                                              │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ wmwms 层（可直接替换）：窗口表、z、焦点、事件、连接生命周期 │  │
│  └────────────────────────┬───────────────────────────────┘  │
│                           │ VmWmHost 回调                    │
│  ┌────────────────────────┴───────────────────────────────┐  │
│  │ 宿主（本项目实现）：渲染副本 + 装饰 + 合成 + 差分 + 切块   │  │
│  │                    + 资源包生成                          │  │
│  └────────────────────────────────────────────────────────┘  │
└───────────────────────────┬──────────────────────────────────┘
                            │ REQ/ACK 元数据 + EVT_LAYOUT_REQ / EVT_INPUT
                            │ CMD_WINDOW_CONTENT
                     vm_shell / app × N
```

**关键结构**：wmwms 的窗口表是**权威**；本项目在同一个进程里通过 `VmWmHost` 回调表挂一个「合成宿主」，其中维护一份**渲染副本**（`VmWm`）与内容缓冲。合成、差分、切块、资源包全部在宿主里，不从 wmwms 仓库搬。

### 3.2 数据归属

| 数据 | 权威 | 副本 | 消费者 |
|---|---|---|---|
| 窗口几何 / z / 焦点 / mapped | wmwms 窗口表 | 宿主 `VmWm`（渲染副本，由 `on_window_event` 同步） | 合成 |
| 内容命令 | **app / shell** | 宿主的 `VmWindow.layer` + epoch | 合成 |
| 合成后全帧命令 | 宿主（内存内，不跨进程） | — | 宿主自己切块 |
| tiles JSON | 宿主 | rdp 缓存最近 fullframe | 浏览器 |
| 装饰（标题栏/边框） | 宿主 `VmWm_BuildFrame` | — | 合成 |
| 主题 | 各进程自己解析 `assets/themes/<name>/` | — | 各自用途 |

### 3.3 三条连接

```text
app / shell ──► vm_wms   元数据 REQ/ACK + EVT_LAYOUT_REQ / EVT_INPUT + CMD_WINDOW_CONTENT
rdp         ──► vm_wms   订阅 EVT_FRAME|EVT_FULLFRAME|EVT_RESOURCES + CMD_INPUT + 请求 fullframe
```

每个客户端进程一条连接；SDK 是句柄式的，一个进程可以开多条（但当前不需要）。

## 4. 决策记录（已冻结）

| 编号 | 决策 | 理由 |
|---|---|---|
| **D1** | 内容缓冲与命令级合成都在 vm_wms **进程内**（由宿主实现） | 内容本来就在 `VmWindow.layer`；搬出去会让每帧传约 1 MB 全帧命令 |
| **D2** | rdp 用订阅方式拿帧（`EVT_FRAME`） | 复用 wmwms 的 EVT 广播；多 rdp 天然共享同一份合成结果 |
| **D3** | 传输统一 libuv + 命名管道 | wmwms 已用 mingw64 编通；管道省掉端口分配并天然限制本机访问 |
| **D4** | 协议采用 wmwms 的 16 B 头，**废弃 `vm_ipc.h`** | 已实现优先于纸面冻结；wmwms 的 `CMD_LIST_WINDOWS` + `EVT_*` 覆盖了 `vm_ipc.h` 的大部分设计 |
| **D5** | 内容与 `EVT_INPUT` 坐标是 **body 局部**；`CMD_INPUT` 传画布绝对坐标 | 应用不知道装饰厚度；装饰偏移由 wmwms 按宿主 `body_rect` 减掉 |
| **D6** | 客户端 SDK 句柄式（`WmsClient*`） | 原实现全是 static 全局，一个进程只能连一条 |
| **D7** | ~~wm_server 用 VmWm 的窗口表~~ → **反转：wmwms 窗口表是权威，本项目维护渲染副本** | wmwms 要能独立于主项目发布，不能依赖 `VmWm`；宿主侧用 `on_window_event` 同步副本 |
| **D8** | `win_id` 线上 `uint64_t`，服务端内部窄化 `int32_t` | 协议不变；`VmWm` 的 `int id` 不扩散 |
| **D9** | 合成由宿主（`VmWmHost` 回调表）实现，wmwms 只搬运 | 让 wmwms 能脱离合成层单独发布；本项目改动集中在宿主 |
| **D10** | 内容通道用「定长头 + `count × cmd_size`」，协议不写死 `VmDrawCommand` | 主项目改 `VmDrawCommand` 不会破协议；`cmd_size` 由客户端上报、服务端校验 ≤ 512 |
| **D11** | 越权请求回 `VMWM_ERR_PERMISSION`（不断连） | 可观测性；比「静默忽略」更容易定位外壳配置错误 |
| **D12** | 不做 PROCMAN 角色 | 进程表与杀进程不在本次范围；`vmwm_peer` 只做连接背后的进程探活 |

## 5. 协议 v2（以实现为准）

### 5.1 基本形态

- 包头 16 B：`magic('VmWM') | cmd(type:2bit + id:14bit) | seq | body_len | reserved`，`#pragma pack(1)`，小端。
- 类型 4 种：`REQ(0)` / `ACK(1)` / `EVT(2)` / `MSG(3)`。`MSG` 永不回 ACK。
- 上限：`VMWM_MAX_PAYLOAD = 1 MB`（`body_len` 超限或 magic 错 → 立刻断连）；连接 32、窗口 128、单次内容 1024 条 × `cmd_size ≤ 512`。
- `EVT_ALL = 0x7FF`（11 个事件位）。

### 5.2 角色与能力

| 角色 | 能力位 |
|---|---|
| `APP(1)` | `OWN_WINDOWS \| CONTENT` |
| `SHELL(2)` | `OWN_WINDOWS \| ALL_WINDOWS \| ENUM \| CONTENT \| INPUT \| WORKAREA \| THEME` |
| `RDP(3)` | `FRAME \| RESPACK \| ENUM \| INPUT` |

单一真源是 `VmWm_RoleCaps()`（`vmwm_ipc.h:380-393`），服务端鉴权与文档都由它派生。

### 5.3 命令与事件

请求命令（v1 的 17 条保留 + v2 新增）：

| cmd | id | 类型 | 角色 | 载荷 |
|---|---|---|---|---|
| `CMD_INPUT` | 17 | MSG | SHELL / RDP | `VmWmReqInput` |
| `CMD_WINDOW_CONTENT` | 18 | REQ | APP / SHELL | `VmWmReqContent` + `count × cmd_size` 字节 |
| `CMD_SET_WORKAREA` | 19 | REQ | SHELL | `VmWmReqWorkArea` |
| `CMD_THEME_SET` | 20 | REQ | SHELL | `VmWmReqTheme` |
| `CMD_REQUEST_FULLFRAME` | 21 | REQ | RDP | 无载荷 |
| `CMD_REQUEST_RESPACK` | 22 | REQ | RDP | `VmWmReqRespack` |

事件：

| evt | 值 | 目标 | 载荷 |
|---|---|---|---|
| `EVT_LAYOUT_REQ` | `1<<5` | APP / SHELL | `VmWmEvtLayoutReq` |
| `EVT_INPUT` | `1<<6` | APP / SHELL | `VmWmReqInput`（复用布局） |
| `EVT_FRAME` | `1<<7` | RDP | `VmWmEvtFrame` + JSON |
| `EVT_FULLFRAME` | `1<<8` | RDP | `VmWmEvtFrame` + JSON |
| `EVT_THEME_CHANGED` | `1<<9` | 全部 | `VmWmReqTheme` |
| `EVT_RESOURCES` | `1<<10` | RDP | `VmWmEvtRespack` + JSON |

### 5.4 结构尺寸（全部 `_Static_assert` 钉死）

| 结构 | 字节 |
|---|---|
| `VmWmMsgHdr` | 16 |
| `VmWmAck` | 16 |
| `VmWmAckHello` | 44 |
| `VmWmWindowInfo` | 172 |
| `VmWmReqInput` | 96 |
| `VmWmReqContent` | 24 |
| `VmWmEvtLayoutReq` | 20 |
| `VmWmEvtFrame` | 8 |
| `VmWmEvtRespack` | 36 |
| `VmWmReqWorkArea` | 16 |
| `VmWmReqTheme` | 32 |

### 5.5 坐标系（D5 的落地细则）

| 方向 | 坐标 |
|---|---|
| `CMD_WINDOW_CONTENT` 的命令数组 | **body 局部**（客户端画的就是 body） |
| `CMD_INPUT`（rdp → wms） | `win_id == 0`：画布绝对坐标，由服务端命中；`win_id != 0`：窗口内坐标 |
| `EVT_INPUT`（wms → app） | **一律 body 局部**，装饰偏移由服务端按宿主 `body_rect` 减掉 |
| 合成 | 宿主把 body 内容按 `body_rect` 偏移叠到窗口上 |

### 5.6 「请求-应答」与「事件」的分工

**数据不塞 ACK。** `CMD_REQUEST_FULLFRAME` / `CMD_REQUEST_RESPACK` 的 ACK 只表示「已受理」（`result=VMWM_OK`，`value` 对 fullframe 是当前 `frame_id`），数据走 `EVT_FULLFRAME` / `EVT_RESOURCES`：

```996:1006:H:\myGitRepos\wmwms\src\server\wm_server.c
    case CMD_REQUEST_FULLFRAME: {
        CAP_FAIL(c, VMWM_CAP_FRAME, seq, "fullframe");
        if (!g_host_set || !g_host.on_frame_request) {
            send_ack(c, seq, VMWM_ERR_NOT_READY, 0);
            return;
        }
        /* ACK 只表示「已受理」，数据走 EVT_FULLFRAME，避免 1 MB 塞进 ACK */
        send_ack(c, seq, VMWM_OK, g_frame_id);
        g_host.on_frame_request(g_host.user, c->id);
        return;
    }
```

理由：ACK 载荷也受 1 MB 限制，全帧 JSON 接近上限时会截断或把同步调用拖到超时。

> **注意**：`vmwm_ipc.h:137-138` 的注释写的是「ACK 体为 fullframe JSON」「ACK 体为资源包 JSON」，与实现不一致，接入时按实现（走事件）写代码。这条已记为需要上游修正的注释。

### 5.7 与上游设计的差异（SEAM §12 的 8 条，已确认）

| # | 上游口径 | 实际实现 | 处理 |
|---|---|---|---|
| 1 | TCP 主端口+1、12 B 头、768 KB | 命名管道、16 B 头、1 MB | 采纳（D3/D4） |
| 2 | 有 PROCMAN 角色 | 没有 | 采纳（D12） |
| 3 | 内容用画布绝对坐标 | body 局部 | 采纳（D5） |
| 4 | `CMD_WINDOW_CONTENT` 直接放 `VmDrawCommand` 数组 | 定长头 + `count × cmd_size` | 采纳（D10） |
| 5 | 5 个事件 | 6 个（多 `EVT_RESOURCES`），`EVT_ALL=0x7FF` | 采纳 |
| 6 | 越权静默忽略 | 回 `VMWM_ERR_PERMISSION` | 采纳（D11） |
| 7 | 先接元数据 | 一致，且用 `VMWM_ERR_NOT_READY` 显式表达「合成未接」 | 采纳，M2/M3 可分开上线 |
| 8 | 帧 JSON 由 vm_wms 生成 | 由宿主生成，wmwms 只搬运 | 采纳（D9） |

主项目侧要跟着改的三处：`test_frame.c:534` 的 `sizeof(VmIpcHeader)==12` 断言、任何沿用 768 KB 的检查、`vm_ipc.h` 的全部引用点。

## 6. 宿主接入契约（本项目要写的核心代码）

所有回调都在 wm_server 的单线程 loop 里同步调用，不得阻塞。

| `VmWmHost` 回调 | 本项目实现 | 对应现有代码 |
|---|---|---|
| `on_content(user, win, epoch, count, cmd_size, cmds, overflow)` | 拷贝到 `VmWindow.layer.commands`（校验 `cmd_size == sizeof(VmDrawCommand)`），置 `contentDirty` | `vm_wm.c` 的 layer 结构 |
| `on_window_event(user, evt, info)` | `EVT_WINDOW_NEW` → `VmWm_AddWindow(layout=NULL)`；改几何/z → 直接改 `VmWindow` 字段并标脏；焦点 → `VmWindow.focused` | `VmWm_AddWindow` / `VmWm_Move` |
| `on_window_removed(user, win_id)` | `VmWm_RemoveWindow` | 同 |
| `body_rect(user, win, w, h, out)` | 直接返回 `VmWm_WindowBody()` 的结果 | `vm_wm.h:222` |
| `hit_test(user, x, y)` | 返回 `VmWm_HitTest()->id` | `vm_wm.h:183` |
| `on_tick(user, frame_id)` | 脏则 `VmWm_BuildFrame` → `VmDisplay_Diff` → `VmTileGrid_Build` → `VmWm_ServerBroadcastFrame(json, len, 0)` | `jpx_app.c:204-233` |
| `on_frame_request(user, client_id)` | 组 fullframe → `VmWm_ServerSendFrame(client_id, json, len, 1)` | `jpx_app.c:235-240` |
| `on_respack_request(user, client_id, id)` | 从当前资源包缓存取 → `VmWm_ServerSendRespack` | `jpx_app.c:192-197` |
| `on_workarea(user, area)` | `VmWm_SetWorkArea` | `vm_shell.c:737` |
| `on_theme(user, name)` | 解析主题 → `VmWm_SetChrome` + `VmTheme_AppendResources` → 重建资源包 → 广播 `THEME_CHANGED` | `jpx_app.c:389-431` |

**渲染副本的关键约定**：宿主里的 `VmWm` 不再是权威。它只由 `on_window_event` / `on_window_removed` 驱动，不调用 `VmWm_Move` / `VmWm_Focus` / `VmWm_HandleInput`。这些窗口管理逻辑（拖拽、命中、焦点仲裁）全部由 wmwms 承担。

`build_one_window` 需要新增第三条分支（内容来自 layer）：

```c
if (win->paint) {
    win->paint(out, bodyX, bodyY, bodyW, bodyH, win->userData);   /* demo_wm 本地模式 */
} else if (win->layer.count > 0) {
    /* 复制 layer.commands，rect 与各 args 的 rect 加 (bodyX, bodyY) 偏移 */
} else {
    /* 空内容：只画装饰与背景 */
}
```

Clay 分支（`win->layout && win->clayCtx`）删除，`vm_wm.h` 的 `#include "clay.h"` 与 `clay_frame_adapter.h` 一并删除。

## 7. 文件级迁移映射

### 7.1 直接迁入（改名）

| 源（`H:\myGitRepos\wmwms`） | 目标（本项目） |
|---|---|
| `include/vmwm_ipc.h` | `vm-desktop/wms/wms_ipc.h` |
| `include/vmwm_host.h` | `vm-desktop/wms/wms_host.h` |
| `include/vmwm_server.h` | `vm-desktop/wms/wms_server.h` |
| `include/vmwm_client.h` | `vm-desktop/wms/wms_client.h` |
| `include/vmwm_peer.h` | `vm-desktop/wms/wms_peer.h` |
| `include/vmwm_codec.h` | `vm-desktop/wms/wms_codec.h` |
| `include/vmwm_buf.h` | `vm-desktop/wms/wms_buf.h` |
| `src/common/vmwm_codec.c` | `vm-desktop/wms/wms_codec.c` |
| `src/common/vmwm_peer.c` | `vm-desktop/wms/wms_peer.c` |
| `src/common/vmwm_log.h` | `vm-desktop/wms/wms_log.h` |
| `src/client/vmwm_client.c` | `vm-desktop/wms/wms_client.c` |
| `src/server/wm_server.c` | `vm-desktop/wms/wms_server.c` |
| `src/server/vmwm_mock_host.c` | `vm-desktop/wms/wms_mock_host.c` |
| `src/server/main.c` | `vm-desktop/wms/main.c`（产物 `vm_wms.exe`） |
| `test/test_wms_ipc.c` | `vm-desktop/wms/test_wms_ipc.c` |
| `test/test_peer.c` | `vm-desktop/wms/test_peer.c` |
| `test/test_app.c` / `test_shell.c` / `test_rdp.c` | `vm-desktop/wms/test_wms_app.c` / `_shell.c` / `_rdp.c` |
| `scripts/run_e2e.ps1` | `tools/e2e_wms.ps1` |

### 7.2 本项目要新写

| 文件 | 作用 |
|---|---|
| `vm-desktop/wms/vm_host.c/.h` | `VmWmHost` 的本项目实现（§6 那张表），持有 `VmWm` 渲染副本 + 资源包 |
| `vm-desktop/rdp/main.c` | rdp 入口：uv_tcp + HTTP + SSE |
| `vm-desktop/rdp/rdp_server.c/.h` | HTTP/SSE/静态资源，从 `vm_server.c` 移植，`select` 改 `uv_tcp` |
| `vm-desktop/shell/main.c` | 外壳入口 |

### 7.3 需要改造

| 文件 | 改造 |
|---|---|
| `vm-desktop/vm_wm.c` | `build_one_window` 加 layer 分支、删 Clay 分支；删 `sync_pointer_state`；`VmWm_HandleInput` 保留但不再用于远程模式（给 `demo_wm` 本地用） |
| `vm-desktop/vm_server.c` | 拆出 HTTP/SSE 到 `rdp/`；`select` 改 `uv_tcp`；帧改为「收 `EVT_FRAME` 缓存 + 推送」；输入改为 `Wms_SendInput` |
| `vm-desktop/vm_shell.c` | 直接调 `VmWm_*` 改为 `Wms_*`；工作区改 `CMD_SET_WORKAREA` |
| `vm-desktop/vm_menu.c` | 同上，随外壳进 shell 进程 |
| `vm-desktop/jpx/jpx_window.c` | 去掉 `VmWm_AddWindow` / `Clay_SetCurrentContext`；内容改为 `Wms_PushContent` |
| `vm-desktop/jpx/jpx_app.c` | 加远程模式；删 `VmShell_Create`；`Jpx__Invalidate` 改为「脏了推内容」 |
| `vm-desktop/test_frame.c` | **第 534 行断言改为 `sizeof(VmWmMsgHdr)==16`**，删 `#include "vm_ipc.h"` |
| `vm-desktop/CMakeLists.txt` | 加 `vm_wms` / `vm_rdp` / `vm_shell` 三个 target + libuv 查找逻辑（照搬 wmwms 的 `find_package` / `find_path` 分支） |

### 7.4 删除

| 文件 | 时机 |
|---|---|
| `vm-desktop/vm_ipc.h` | M1（先改 `test_frame.c`，再删头） |
| `jpx.h` 的 6 个 `Jpx_Shell*` 转发 + `jpx_app.c` 对应实现 | M6 |

## 8. 分步实施

每步给出「做什么 / 验收 / 恢复点」。恢复点是中断后重新开始该步的入口。

### M0 基线固化

**做什么**

1. 构建全部 target：`cmake -B vm-desktop/build -S vm-desktop -G "MinGW Makefiles"` 后 `cmake --build vm-desktop/build -j 8`。
2. 逐个跑 `--probe`，与 §2.3 对比。
3. 抓一帧基线：

```powershell
.\vm-desktop\build\demo_jpx.exe 9000 ..\assets
# 另开终端
python tools\drive_vm_desktop.py 9000 snap baseline wait 1500
python tools\inspect_vm_frame.py temp\baseline.txt --tiles
```

4. 把 `temp/baseline.txt` 另存为 `temp/m0-baseline-fullframe.txt`（不进 Git）。

**验收**：测试全过、`inspect_vm_frame.py` 能解析基线。

**恢复点**：`temp/m0-baseline-fullframe.txt` 存在。

### M1 协议层落入本项目

**做什么**

1. 按 §7.1 迁入 7 个头 + 6 个 .c 到 `vm-desktop/wms/`，统一改名为 `wms_*` / `Wms_*` 前缀。
2. `test_frame.c`：删 `#include "vm_ipc.h"`，第 534 行断言改 `sizeof(VmWmMsgHdr)==16`。
3. 删除 `vm-desktop/vm_ipc.h`。
4. `CMakeLists.txt` 加 libuv 查找（照搬 wmwms 的 `find_package(libuv CONFIG QUIET)` → `find_path/find_library` 回退），新增 target：
   - `wms_core`（`wms_codec.c`，无 socket 依赖）
   - `wms_peer`（`wms_peer.c`）
   - `wms_client`（`wms_client.c`，链 `wms_core` + libuv）
5. 迁入 `test_wms_ipc.c` / `test_peer.c` 并接入构建。

**验收**：`test_wms_ipc.exe --probe` 36/36、`test_peer.exe --probe` 7/7；`test_frame.exe --probe` 仍 120/120（断言数量不变，只换了尺寸常量）。

**恢复点**：`vm-desktop/wms/` 目录可独立编译，`vm_ipc.h` 已删。

### M2 vm_wms 元数据上线（不接合成）

**做什么**

1. `vm-desktop/wms/main.c`：`VmWm_ServerInit(pipe)` + `VmWm_ServerSetScreenSize(800,600)` + **不注册宿主**（走 `VMWM_ERR_NOT_READY` 分支）。
2. 构建 `vm_wms.exe`，跑 `tools/e2e_wms.ps1` 的元数据场景（握手、建窗、改几何、枚举快照、断线回收）。
3. 此时浏览器还没有画面（没有宿主）。

**验收**：e2e 元数据部分全过；`vm_wms.exe` 能用 `test_wms_shell.exe` / `test_wms_app.exe` 驱动。

**恢复点**：`vm_wms.exe` 独立启动、接受连接。

### M3 宿主接入：内容 → 合成 → 帧

**做什么**

1. 写 `vm-desktop/wms/vm_host.c`，按 §6 实现 10 个回调。
2. `main.c` 改为 `VmWm_ServerSetHost(&host)`，宿主持有 `VmWm` 渲染副本 + `VmTileGrid` + 资源包缓存。
3. `build_one_window` 加 layer 分支、删 Clay 分支、删 `sync_pointer_state`、`vm_wm.h` 去掉 Clay 头。
4. 连接 `rdp` 角色：用 `test_wms_rdp.exe` 订阅 `EVT_FRAME`，请求 fullframe。

**验收**：`test_wms_rdp.exe` 能拿到 fullframe + 增量帧；用 `inspect_vm_frame.py` 回放，结构与 `temp/m0-baseline-fullframe.txt` 一致（瓦片数、命令语义；坐标因去掉 Clay 布局会有差异，记录差异清单）。

**恢复点**：`vm_wms.exe` 带宿主跑真实帧。

### M4 rdp 拆分

**做什么**

1. `vm-desktop/rdp/`：`rdp_server.c` 从 `vm_server.c` 移植 HTTP/SSE，`select` 改 `uv_tcp`。
2. rdp 以 `VMWM_CLIENT_RDP` 连 `vm_wms`，订阅 `EVT_FRAME|EVT_FULLFRAME|EVT_RESOURCES`。
3. 新连接 → `Wms_RequestFullframe()` → 收 `EVT_FULLFRAME` → 作为 SSE `event: fullframe` 下发。
4. `POST /input` → `Wms_SendInput(win_id=0, 画布坐标)`，立即回 204。
5. `GET /respack/<id>` → 用缓存的 `EVT_RESOURCES` 内容响应。
6. 慢 rdp：写队列超阈值断开（对齐原 `vm_push_frames` 策略）。

**验收**：浏览器连 rdp 端口，画面/输入/主题切换正常；§1 第 3 条（kill rdp 后重启恢复）成立。

**恢复点**：`vm_wms.exe` + `vm_rdp.exe` 两进程即可服务浏览器。

### M5 外壳独立

**做什么**

1. `vm-desktop/shell/main.c` + 迁入 `vm_shell.c` / `vm_menu.c` / 主题解析。
2. 任务栏通过 `Wms_CreateWindow` 建窗，内容用手绘 `paint` 生成命令数组后 `Wms_PushContent`。
3. 条目：`Wms_ListWindows` 首次同步 + `EVT_*` 增量。
4. 工作区：`Wms_SetWorkArea`。

**验收**：§1 第 1 条（kill shell 不影响窗口；重启重建）。

**恢复点**：三进程（wms / rdp / shell）跑通，应用仍可用单进程模式。

### M6 应用独立

**做什么**

1. `Jpx_Create` 加远程模式（`JpxOptions.wmsPipe` 非空）：连 `vm_wms`（APP），不再 `VmWm_Create` / `VmShell_Create` / `VmServer_Create`。
2. `Jpx_WindowOpen` → `Wms_CreateWindow`；`Jpx_WindowSetTitle` → `Wms_SetTitle`。
3. 收 `EVT_LAYOUT_REQ` → 用 `lr->width/height` 重跑 layout → `Wms_PushContent(win, lr->epoch, cmds, n, sizeof(VmDrawCommand))`。
4. 收 `EVT_INPUT` → 坐标已是 body 局部，直接进现有按下沿/键盘路由。
5. `demo_jpx` 改远程模式启动。

**验收**：§1 第 2 条；`test_frame --probe` / `test_menu --probe` 仍通过。

**恢复点**：`demo_jpx` 不再链接 `vm_wm.c` / `vm_server.c` / `vm_shell.c`。

### M7 清理与文档同步

**做什么**

1. 删 `jpx.h` 的 6 个 `Jpx_Shell*` 转发与实现。
2. 改写 `docs/design-vm-ipc.md`（顶部标注已被取代，保留决策历史）与 `docs/design-vm-wms-process.md`（传输层与落地步骤按实际改）。
3. 更新 `docs/arch-vmdesk.md`、`docs/INDEX.md`、`docs/test-timing.md`、`tools/INDEX.md`。
4. 新工作流写入 `workflows/` 并登记。

**验收**：`git grep -n "vm_ipc.h"` 无结果；文档里的可执行体列表与 CMake target 一致。

## 9. 测试与验证

| 层 | 方法 |
|---|---|
| 协议编解码 | `test_wms_ipc.exe --probe`（36 项） |
| 进程身份 | `test_peer.exe --probe`（7 项） |
| 窗口元数据 + 端到端 | `tools/e2e_wms.ps1`（23 条断言，四进程场景） |
| 合成与瓦片 | `test_frame --probe` + `inspect_vm_frame.py` 对比 M0 基线 |
| 崩溃容错 | §1 的 5 条手工验收 |
| 界面语义 | `test_menu --probe`、`test_jpx_xaml_theme --probe` |

**测试隔离**：`vm_wms` / `vm_rdp` / `vm_shell` 的 e2e 各用独立管道名（`--pipe \\.\pipe\vmwm_test_<pid>`）与端口（`19xxx`），不碰用户正在运行的实例。`test_wms_ipc` / `test_peer` 不开 socket。

## 10. 风险与已知限制

| 风险 | 状态 | 缓解 |
|---|---|---|
| **工具链 CRT** | **已定：mingw64** | wmwms 的 CMakeLists 默认 hint 就是 `C:/msys64/mingw64`，与 `build_vm.py` 一致；不再有跨编译器链接问题 |
| **Linux 分支未验证** | 已知 | `vmwm_peer.c` 的 pidfd/`SO_PEERCRED` 与 unix socket 路径都没编译过。目标平台是 Windows，Linux 迁移时第一件事是编译 + 跑 e2e |
| **wm_wms 崩溃 = 全丢** | 设计边界 | 元数据、渲染副本、合成状态一起没；rdp 只保留最后一帧 JSON（画面冻结）。不做自动恢复 |
| **全帧 JSON 跨进程** | 已知 | 全帧约 48 KB，只在 fullframe 时发生；增量帧是变化块。若接近 1 MB 上限，wmwms 返回 `VMWM_ERR_LIMIT`，需要宿主自己切块 |
| **输入两跳延迟** | 已知 | 浏览器 → rdp → wms → app，每跳单向无阻塞；实测目标 < 2 ms |
| **`vm_wm.c` 的窗口管理逻辑变成死代码** | 待清理 | 远程模式下 `VmWm_Move/Focus/HandleInput` 不再被调用，只服务 `demo_wm` 本地模式。M7 评估是否删除 |
| **帧不分片** | 已知 | 单帧 JSON ≤ `VMWM_MAX_PAYLOAD - sizeof(VmWmEvtFrame)`；当前 800×600 远低于此 |
| **退出不 flush** | 已知 | `VmWm_ServerStop` 直接关 loop，已入队未写出的数据会丢；窗口状态本来不持久化 |

**已知限制（本次不做）**：

- WMS 高可用与热重启；崩溃后桌面恢复。
- 跨进程剪贴板。
- 应用私有资源注册（只有宿主生成资源包）。
- 启动器（手工启动各进程）。
- PROCMAN 角色、进程表、杀进程。
- `/auto/<op>` 中应用语义的 op（拆进程后只保留主题相关 op）。

## 11. 开工前需要落地的前置项

| # | 事项 | 状态 |
|---|---|---|
| 1 | 目标平台 = Windows（Linux 分支保留不验证） | 已定 |
| 2 | 工具链 = mingw64（与 `build_vm.py` 一致） | 已定 |
| 3 | `vm_shell` 独立可执行 | 已定（e2e 就是这么跑的） |
| 4 | 管道命名与测试隔离（`--pipe \\.\pipe\vmwm_test_<pid>`） | 已定 |
| 5 | `test_frame.c:534` 断言改为 `sizeof(VmWmMsgHdr)==16` | **待做（M1 第一步）** |
| 6 | `vmwm_ipc.h:137-138` 注释与实现不一致（ACK vs 事件） | **待上游修**；本项目按实现写代码，已在 §5.6 标注 |
| 7 | 删除 `vm-desktop/vm_ipc.h` 及其引用 | **待做（M1）** |

## 12. 进度

| 步 | 状态 | 备注 |
|---|---|---|
| M0 基线固化 | 已完成（2026-11-24） | 见 §13.1 |
| M1 协议层落入本项目 | 已完成（2026-11-24） | 见 §13.2 |
| M2 vm_wms 元数据上线 | 已完成（2026-11-24） | 见 §13.3 |
| M3 宿主接入 | 已完成（2026-11-24） | 见 §13.4 |
| M4 rdp 拆分 | 未开始 | |
| M5 外壳独立 | 未开始 | |
| M6 应用独立 | 未开始 | |
| M7 清理与文档 | 未开始 | |

## 13. 执行记录

### 13.1 M0（2026-11-24）

命令与结果：

```powershell
C:\msys64\mingw64\bin\cmake.exe --build vm-desktop/build -j 8      # exit 0，全部 target 已是最新
```

测试基线（`--probe`，本次实测耗时）：

| 测试 | 断言 | 耗时 | 结果 |
|---|---|---|---|
| `test_frame` | 120 | 19 ms | 全过 |
| `test_menu` | 30 | 15 ms | 全过 |
| `test_sys` | 16 | 63 ms | 全过 |
| `test_text_edit` | 66 | 11 ms | 全过 |
| `test_jpx_xaml` | 47 | 56 ms | 全过 |
| `test_jpx_xaml_theme` | **137** | 116 ms | 全过（`test-timing.md` 原记 104，已修正） |
| `test_fs` | 19 | 13 ms | **1 项既有失败**，见下 |

基线帧：

```powershell
.\vm-desktop\build\demo_jpx.exe 9020 assets
python tools\drive_vm_desktop.py 9020 snap baseline wait 1500
```

结果：`temp/m0-baseline-fullframe.txt` 166531 B，`fullframe=1`、`setTile=96`；瓦片里含任务栏条目（`开始` / `表单演示` / `主题面板` / `列表`）与时钟。

**观察**：`test_fs` 的 `chinese filename round-trips as UTF-8` 失败，根因是测试硬编码了 `docs/arch-跨端图绘指令流.md`，而该文件已在提交 `c673c81 clearn old names` 里被改名为 `arch-vmdesk.md`。属于既有缺陷（早于本任务），已登记 `issues/INDEX.md`，本次不修以免混入无关 diff。

### 13.2 M1（2026-11-24）

做了什么：

1. 从 `H:\myGitRepos\wmwms` 复制 19 个文件到 `vm-desktop/wms/`（原仓库只读，未改动），文件名 `vmwm_*` → `wms_*`；`test_app/test_shell/test_rdp/test_peer` 加 `wms_` 前缀避免与现有 `test_*` 重名。
2. include 重写 30 处（`#include "vmwm_xxx.h"` → `"wms_xxx.h"`）。
3. 唯一符号改名：`VmWm_RemoveWindow` → `VmWm_ServerRemoveWindow`（4 处：1 定义 + 2 调用 + 1 声明）。它与本项目 `vm_wm.h` 的同名函数签名冲突，两者会在 `vm_wms.exe` 里同时链接。其余符号名保持不变（`VmWm*` 类型、`VMWM_*` 宏、`VmWm_Server*` 函数、`Wms_*` 客户端 API），以降低与上游同步的成本。
4. `test_frame.c`：删 `#include "vm_ipc.h"`，断言改 `sizeof(VmWmMsgHdr) == 16`。
5. 删除 `vm-desktop/vm_ipc.h`（全仓已无引用）。
6. `CMakeLists.txt` 增加 libuv 查找与 4 个 target：`wms_core` / `wms_peer` / `wms_client` / `test_wms_ipc` / `test_wms_peer`；`test_frame` 的 include 目录加 `vm-desktop/wms`。

**观察（下次迁移同类文件要注意）**：

- 4 个文件用了相对路径 include `#include "../common/vmwm_log.h"`，正则 `#include "vmwm_*.h"` 匹配不到，构建时才报错；迁移脚本必须同时覆盖相对路径形式。
- `find_package(libuv CONFIG QUIET)` 在本机直接命中 `libuv::uv`（MSYS2 装了 CMake config），没有走 `find_path` 回退分支。回退分支仍未验证。
- `wms_server.c` / `wms_mock_host.c` / `main.c` 目前**没有 target**，M1 不编译它们；它们的可编译性由 M2 建 `vm_wms` 时验证。

验收结果：

```powershell
.\vm-desktop\build\test_wms_ipc.exe --probe     # 36 checks, 0 failed
.\vm-desktop\build\test_wms_peer.exe --probe    # checks=7 failed=0, ALL PASS
.\vm-desktop\build\test_frame.exe --probe       # 120 checks, 0 failures
```

全套 9 个测试除 `test_fs` 的既有失败外全部通过；`demo_jpx` / `demo_wm` / `demo_xaml` / `vm_renderer` 均重新链接成功。

**恢复点**：`vm-desktop/wms/` 下 19 个文件可编译，`libwms_core.a` / `libwms_peer.a` / `libwms_client.a` 生成，`vm_ipc.h` 已删。

### 13.3 M2（2026-11-24）

做了什么：

1. CMake 增加 `vm_wms`（`wms_server.c` + `wms_mock_host.c` + `main.c`）与 `test_wms_app` / `test_wms_shell` / `test_wms_rdp` 三个客户端演示 target，全部加入输出目录列表。
2. libuv 选择改为**静态优先**：MSYS2 的 `libuv::uv` 是 `SHARED IMPORTED`（指向 `bin/libuv-1.dll`），`libuv::uv_a` 才是 `lib/libuv.a`。先试 `uv_a`，产物自包含，e2e 不需要把 `C:\msys64\mingw64\bin` 加进 `PATH`。
3. 迁入 e2e 脚本 `tools/e2e_wms.ps1`（源 `wmwms/scripts/run_e2e.ps1`），只改产物名、构建目录、cmake 路径与日志落点；断言与场景未改。

**观察**：

- `wms_server.c`（1248 行）在本项目编译无错误，说明 M1 的 include 重写是完整的。
- mock 宿主（`wms_mock_host.c`）产出占位帧，所以「内容 → 帧 → RDP」这条链在还没接真实合成时就能验证。
- basic 场景的 `layout_req win=N epoch=1`、`push content accepted=1`、`input ... kind=1 down=1`、`fullframe frame_id=N`、`frame frame_id=N`、`resources id='default'` 全部命中，协议层与内容通道在本项目可用。
- 若将来装的是只提供动态库的 libuv（无 `uv_a`），`WMS_UV_TARGET` 会回退到 `libuv::uv`，那时 e2e 需要把 DLL 目录加进 `PATH`。

验收：`pwsh tools/e2e_wms.ps1 -SkipBuild` → **23/23 checks passed**，exit 0。

| 场景 | 断言 | 覆盖 |
|---|---|---|
| proto | 2 | `test_wms_ipc` 36/36、`test_wms_peer` 7/7 |
| basic | 14 | 三角色握手与进程身份、布局请求、内容采纳、body 局部坐标输入、工作区、枚举、焦点、fullframe、增量帧、资源包 |
| crash | 3 | 强杀 APP 后服务端检测对端消失、回收窗口、清理连接 |
| restart | 4 | 外壳退出不连带回收 APP 窗口、新外壳枚举到同一 `win_id` |

**未验证**：`vm_wms` 当前挂 mock 宿主，帧内容是占位 JSON，不是真实画面。真实合成要等 M3 的 `vm_host.c` 接进去。

**恢复点**：`vm_wms.exe` 可独立启动（`vm-desktop\build\vm_wms.exe --pipe \\.\pipe\vmwm_dev`），`tools/e2e_wms.ps1` 可重复运行。

### 13.4 M3（2026-11-24）

做了什么：

1. `vm_wm.h` 的 `VmWindowLayer` 增加外部内容槽：`external` / `contentCount` / `content[VM_WM_LAYER_CAPACITY]`。`content` 存客户端推来的原始命令（body 局部坐标），`commands` 仍是合成结果。
2. `vm_wm.c` 新增 `shift_command()` 与 external 分支：合成时把 `content` 平移到画布坐标；`clipped` 条件加上 external（外部内容同样裁到 body）。
3. 新增 `wms/vm_host.c` / `vm_host.h`：实现 10 个 `VmWmHost` 回调，持有 `VmWm` 渲染副本 + `VmDisplayList frame` + `VmTileGrid` + 主题资源包。
4. `wms/main.c`：默认挂真实宿主，`--mock` 才用占位宿主。
5. `vm_wms` target 加上合成链（`vm_wm.c` / `clay_frame_adapter.c` / `jpx_clay_impl.c` / `vm_display.c` / `vm_serialize_json.c` / `vm_tiles.c` / `vm_font.c` / `vm_theme.c`）与 `CLAY_DIR` / `MSTB_DIR` include。
6. `test_wms_app.c`：原来推的是 496 字节填充数据（`cmd_size = 496`），与本项目 `sizeof(VmDrawCommand) = 492` 不匹配，被宿主丢弃；改为构造真实 `VM_CMD_DRAW_RECT` / `VM_CMD_DRAW_TEXT`，`cmd_size = sizeof(VmDrawCommand)`。
7. `test_wms_rdp.c`：加 `--dump <path>`，把 fullframe 原样落盘供事后检查。
8. e2e 增加一条断言：dump 出的 fullframe 必须含 `app_rect_0`（否则「有帧」可能只是装饰）。

**观察**：

- `VmWindowLayer` 每窗口增加约 508 KB（`content` 数组），`VmWm` 常驻从约 5 MB 涨到约 10 MB。`VmWm_Create` 仍是一次性 `calloc`，未出问题；池化仍留给后续 S3。
- `vm_wms` 现在编译期需要 `CLAY_DIR`：`vm_wm.h` 仍暴露 `Clay_Context` 与 `VmWindowLayout`。这是过渡状态，M7 清理。
- 坐标链路已验证：APP 推 `{8,40,62,42}`（body 局部）→ 宿主按 body 原点 `(101,110)` 平移 → tile `(1,1)` 局部 `{9,50,62,42}`。**x+1 / y+10 对所有命令一致**，说明偏移来自同一个 body 原点，不是逐条漂移。
- `demo_jpx` 的单进程路径未受影响：`test_frame` 120/120、`test_menu` 30/30。

验收：`pwsh tools/e2e_wms.ps1 -SkipBuild` → **24/24 checks passed**，exit 0。新增断言「fullframe 含应用内容的绘制命令」通过。

**未验证**：

- 画面视觉效果（颜色、文字排布）没在浏览器里看过；只验证了命令进入帧且坐标自洽。
- 标题栏按钮的 hover 高亮在 `vm_wms` 下不生效：`vm_host` 不持有指针状态，`wm->pointerValid = 0`。M5 迁外壳时用 `EVT_INPUT` 的 move 事件补。
- `on_theme` 只处理默认主题，忽略传入的名字。

**恢复点**：`vm_wms` 默认跑真实合成；`temp/e2e-wms-logs/basic-fullframe.json` 可事后检查帧内容。
