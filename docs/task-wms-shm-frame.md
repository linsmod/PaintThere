# 任务：wms ↔ rdp 共享内存帧通道（档位 B）

## 目标

在档位 A（BFRM 二进制帧）基础上，把帧 payload 从命名管道移到共享内存 ring：wms 写 slot，管道只发轻量通知，rdp 直接读映射区。消除 wms→rdp 跨进程的用户态→内核态帧数据拷贝。

## 非目标

- 不改浏览器协议：SSE/WS 仍输出 tile JSON，rdp 侧 JSON 生成仍有拷贝。
- 不改窗口元数据命令、输入通道、资源包路径。
- 不做通用 IPC 框架：只服务帧 payload。

## 设计

### 1. 命名

共享内存名由 pipe 名做 FNV-1a 派生，两端独立算出：

```text
vmwm_shm_<8hex>
```

例如 `\\.\pipe\vmwm_ui_1234` → `vmwm_shm_1a2b3c4d`。避免协议协商名字。

### 2. 布局（小端 / 1 字节对齐）

```text
Header（32B）:
  u32 magic 'VSHM' | u32 version=1 | u32 slotCount | u32 slotSize
  u32 reserved[4]

Slots[slotCount]:
  struct VmShmSlotHeader（16B）:
    u64 seq          // even=ready, odd=writing（seqlock）
    u32 frame_id
    u32 len
  u8 payload[slotSize - 16]
```

参数：`slotCount=8`，`slotSize=256*1024`。总大小约 2 MB。

### 3. 协议通知

`wms_ipc.h` 新增事件：

- `EVT_FRAME_SHM = 1 << 14`
- `EVT_FULLFRAME_SHM = 1 << 15`

载荷头 `VmWmEvtFrameShm`（16B）：

```c
u32 frame_id; u32 slot; u64 seq; u32 len; u32 reserved; // 实际 24B，按 pack(1)
```

通知不含帧数据，只告诉 rdp 去哪个 slot 读。

### 4. 写入（wms）

- wms 启动时 `VmShm_Create(pipeName)` 创建共享内存并映射。
- 发布帧：round-robin 取 slot，seqlock 写：seq++ (odd) → 写 frame_id/len/payload → seq++ (even)。
- 通过 wms_server 广播 `EVT_FRAME_SHM` 通知（复用 FRAME 能力/掩码）。
- 慢 slot：若目标 slot seq 仍为 odd（上一次写未完成，理论上单线程不会发生），跳到下一个 slot。

### 5. 读取（rdp）

- rdp 连上 wms 后 `VmShm_Open(pipeName)` 打开映射（只读）。
- 收到 `EVT_FRAME_SHM`：读 slot header，seqlock 校验（读两次 seq 相同且为 even），用 payload 指针直接调 `VmFrameBin_ToTiles()`，生成 JSON 后 PushFrame/PushFullframe。
- 若 seq 不稳定或 len 超界：丢弃并打印日志；可选请求 fullframe。
- 进程重启：wms 重建共享内存后 seq 从 0 重新计数，rdp 重新 Open（检测 magic/version 不匹配时重开）。

### 6. 生命周期与并发

- wms 持有 mapping 直到退出；rdp 同样。
- 一帧写一次，多 rdp 实例各自映射同一物理页，无额外拷贝。
- slot 覆盖：8 个 slot 循环；rdp 在通知回调内立即消费（同步解码），不长期持有 payload 指针。
- seqlock 用 C11 `__atomic_*` builtins（x64 lock-free），跨进程可见。

## 步骤与结果（2026-11-25 完成）

- [x] 写本任务文档
- [x] 实现 `vm-desktop/wms/vm_shm.h/.c`：pipe 名 FNV 派生 `vmwm_shm_xxxxxxxx`；8 slot × 256KB ring，seqlock（seq 奇=写中/偶=可读）；Windows `CreateFileMapping` / POSIX `shm_open` 条件编译；`VmShm_MapPayload` 返回映射区只读指针
- [x] 单测 `test_shm`：Create+Open 写一帧读一帧，直接指针 memcmp 通过
- [x] 协议：因 cmd id 仅 14 bit，无法新增事件位；SHM 通知复用 `EVT_FRAME_BIN/EVT_FULLFRAME_BIN`，靠载荷头 magic `BSHM` 区分（BFRM=内嵌帧，BSHM=slot 引用），新增 `VmWmEvtShmFrame`（32B）
- [x] wms_server 持有 shm（ServerInit Create / ServerStop Destroy），新增 `VmWm_ServerPublishShmFrame`：写 slot + 广播 32B 通知
- [x] vm_host 传输开关 `VM_WMS_FRAME_TRANSPORT=shm|pipe|both`（默认 both）；单份 BFRM buffer 复用于 shm 与 pipe；`VM_WMS_JSON_FRAMES=0` 可关 legacy JSON
- [x] rdp 连接后打开 shm（失败每轮重试），BIN 事件里检查 magic，BSHM 时 `VmShm_MapPayload` 零拷贝拿 payload 直接 `VmFrameBin_ToTiles` 解码
- [x] 全量构建通过；`test_shm` / `test_frame_bin` 通过；e2e_wms 61/61
- [x] shm-only 验证：`VM_WMS_FRAME_TRANSPORT=shm` 启动四进程，wms/rdp 日志均确认 shm ring，浏览器画面完整（截图 `temp/shm_desktop.png`）

### 验证数据

- `test_shm`: `shm slot=0 seq=2 n=19 len=19 ok=1`
- e2e_wms: 61/61 checks passed（默认 both 模式）
- shm-only: wms 日志 `shm frame ring ready for \\.\pipe\vmwm_ui_21740`，rdp 日志 `shm frame ring opened for ...`，浏览器画面正常
- 帧数据不再经过命名管道：管道只传 32B `VmWmEvtShmFrame` 通知；rdp 直接读映射区，跨进程零拷贝

### 已知限制

- rdp 解码成浏览器 JSON 仍有拷贝（SSE 文本输出必需），本任务只消除 wms→rdp 这一跳。
- 8 slot 循环覆盖；rdp 在通知回调内同步消费，不长期持有 payload 指针。若 rdp 卡住超过 8 帧会读到更新的 seq，当前直接丢弃该帧（日志），下次 fullframe/增量补上。
- `VM_WMS_FRAME_TRANSPORT=shm` 时 legacy `test_wms_rdp` 收不到帧（它不订阅 SHM 语义），所以 e2e 需在默认 both 模式跑；浏览器路径 shm-only 已单独验证。

## 风险与注意

- Windows 下共享内存对象名全局命名空间，pipe 名 hash 要足够分散；多实例并行需独立管道名（e2e 已用随机名）。
- rdp 可能比 wms 先启动：Open 失败时重试，直到 wms 创建。
- 不要 memcpy payload 到 rdp 本地缓冲：直接把映射区指针传给解码器，解码成 JSON 时才分配。
- 事件通知仍需走管道：只传 16~24 字节，开销可忽略。
- 改代码前停掉 `vm_*` 进程，避免 Windows 锁 exe。
