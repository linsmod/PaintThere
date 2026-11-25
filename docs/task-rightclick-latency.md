# 任务：右键菜单延迟定位与优化（2026-11-25）

## 现象

任务栏右键出菜单内容慢，用户明确“不是 shell 做了延迟”。实测从右键到浏览器首帧 204–266ms。

## 定位方法

- 用 `tools/drive_vm_desktop.py` 扩展 `rdown/rup/times` 动作与时间戳，保持 SSE 连接抓帧到达时间。
- 给 shell / wms / rdp 日志加统一 `uv_hrtime()` ms 时间戳（取模 100000），三端同一时钟基准。

初始时间线（ms，统一基准）：

```text
19995 rdp 输入 → wms（同 ms，即时）
20010 shell EVT_POINTER (+15)
20025 shell EVT_INPUT   (+30)
20072 wms create window (+77)     ← shell 发出建窗请求
20102 shell layout req  (+107)
20198 shell menu open   (+203)    ← CreateWindow 等 ACK 126ms
20214 shell menu push   (+219)
SSE 首帧 ≈ +266
```

## 根因

1. **Windows `Sleep(1)` 实际 ~15.6ms 时钟粒度**。`wms_client.c` 的 `client_call` 等 ACK、`Wms_PollEvents` 都用 `vmwm_sleep_ms(1)`，导致每次同步 WMS 请求至少 15ms。`open_menu` 里 CreateWindow/SetTitle/SetClassHint/MapWindow 四次同步请求叠加 ≈ 126ms。
2. **shell 主循环 `uv_run(UV_RUN_ONCE)` + 16ms timer**：事件 poll 被 timer 阻塞，EVT_POINTER/EVT_INPUT 各晚 1 tick。
3. shell 把 action 存 pending、等 on_timer 处理，建窗又两轮 timer。

rdp / wms / shm 都是即时的，不是瓶颈。

## 修复

- `wms_client.c`：新增 `vmwm_yield_short()`，Windows 下 `Sleep(0)` + 0.2ms 短自旋（避免 15.6ms 粒度），POSIX 保持 1ms nanosleep；`client_call` 等 ACK 与 `Wms_PollEvents` 改用它。
- `shell/main.c`：
  - 主循环 `uv_run(UV_RUN_NOWAIT)` + 每轮 `Wms_PollEvents(0)`，不再被 timer 阻塞；
  - Windows `Sleep(0)` 代替 `Sleep(1)`；
  - on_timer 周期 16ms → 4ms（pending action 更快 drain，WMS 帧节拍仍 16ms）。
- 日志加时间戳便于回归（shell / wms_log.h / rdp）。

## 验证

- 修复后统一时间线：`shell input down → menu layout 8ms → open/push 同 ms`；同步请求从 126ms 降到 ~1ms。
- 端到端：`mousedown → SSE 首帧` 从 266ms 降到 **47ms**。
- `python tools/build_vm.py` 全量通过；`pwsh tools/e2e_wms.ps1` **61/61**。
- 浏览器打开桌面无错误，右键链路可用。

## 注意

- `Sleep(0)` + 短自旋会让 wms_client 轮询循环 CPU 略高，但每次等待通常 <1ms，实测无异常；长时间同步请求等待依赖 deadline 退出，不会死循环。
- 如果 guest 环境（POSIX）时钟粒度不同，`vmwm_yield_short` 仍走 nanosleep(1ms)，行为不变。
