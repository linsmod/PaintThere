# 任务：新架构 app（vm_app，从零写）

> 自包含执行文档（2026-11-25 立项，未开工）。
> 上游设计：`docs/design-ui-producer-decoupling.md`（displayList 为契约）。
> 进程模型与 M6/M7 归属：`docs/task-vm-wms-merge.md`。
> 本任务取代原 M6 的「改造 jpx 加远程模式」方案。

## 1. 决策与边界

- 不新增 local/remote 两种语义，不给 jpx 抽宿主抽象。
- `jpx/`、`jpx_xaml/`、`demo_jpx`、`demo_xaml` 是 **old infra**：保留代码、标 DEPRECATED、不重构。
- 新 app 从零写，只依赖 `VmCmdBuffer` 与 wms 协议；验收通过后（M7）整体删除旧 app。
- 第一版**不引入 Clay / XAML**，手写命令。理由：先测协议（坐标、epoch、驱动方式），
  再叠 UI 库；否则出问题分不清是协议还是布局引擎。

## 2. 目标与验收

| 编号 | 验收 | 判据 |
|---|---|---|
| **A1** | 单窗口闭环 | 窗口出现；按钮可按下（两个方向都变）；可打字、Backspace 生效；caret 500ms 闪；空闲零帧 |
| **A2** | 两实例并发 | 两个窗口、任务栏两条目；点条目能切焦点；内容互不覆盖 |
| **A3** | 崩溃隔离 | kill 一个实例：只有它的窗口与条目消失；另一个窗口、浏览器连接不断 |
| **A4** | 几何与 epoch | 拖动/resize 后 `epoch+1` 重排；期间旧内容保留不白屏；app 延迟 300ms 回内容时桌面不卡 |
| A5 | Clay 后端（M7） | 同一个窗口内容换 Clay 产出，协议与输入行为不变 |
| A6 | XAML 后端（M7） | 表单 demo 能力对齐 `demo_xaml`；19 主题可切换 |

A1–A4 是本任务的验收；A5/A6 属 M7。

## 3. 进程契约

| 项 | 约定 |
|---|---|
| 角色 | `VMWM_CLIENT_APP`（能力 `OWN_WINDOWS｜CONTENT`，另需 `INPUT` 收自己的输入） |
| 建窗 | `Wms_CreateWindow(x,y,w,h, flags=0, &win)`；flags 为 0 = 普通装饰窗口 |
| 内容 | `Wms_PushContent(win, epoch, cmds, count, sizeof(VmDrawCommand), &accepted, &overflow)` |
| 坐标 | 内容与 `EVT_INPUT` 都是 **body 局部**；app 不知道装饰厚度 |
| 布局驱动 | 只由 `EVT_LAYOUT_REQ(win, epoch, w, h)` 触发；几何变化时 `epoch+1` |
| 旧内容 | epoch 不匹配的内容被服务端丢弃；app 在新布局完成前应继续保留旧内容（不主动清屏） |
| 关闭 | wms 移除窗口后发 `EVT_WINDOW_CLOSE`；app 销毁本地窗口，不尝试恢复 |
| 主题 | `EVT_THEME_CHANGED` 先只记日志；主题解析在 M7 接 |
| 资源 | app 不生成 respack；直接引用宿主主题的 pattern id（如 `s_desktop`） |

输入 kind（`VmWmReqInput.kind`）：`0` move / `1` down / `2` up / `3` wheel / `4` key；
`is_down` 只在 down/keydown 为 1；键盘 `key` 是字符或 `Backspace`/`Tab`/`Enter`；
修饰键在 `mods`（bit0 ctrl、bit1 shift、bit2 alt）。

## 4. 文件规划

```text
vm-desktop/app/
  main.c         连接、事件分发、主循环、窗口生命周期、输入分发
  ui_hello.c/.h  第一版手写内容：标题条、按钮、文本、caret
  （M7）ui_clay.c  / ui_xaml.c
```

CMake target `vm_app`：`app/main.c app/ui_hello.c vm_display.c vm_font.c` + `wms_client`；
不链 `vm_wm` / `vm_server` / `vm_shell` / Clay。

## 5. 步骤

### A1 骨架与单窗口

**做什么**

1. `main.c`：参数 `--pipe`、`--title`、`--font`；libuv loop + `Wms_ClientCreate(pipe, APP)`。
2. 连上后 `Wms_CreateWindow(60, 60, 420, 280, 0, &g_win)`，订阅
   `EVT_LAYOUT_REQ｜EVT_INPUT｜EVT_WINDOW_CLOSE｜EVT_THEME_CHANGED`。
3. 收 `EVT_LAYOUT_REQ`：记 `g_epoch/g_bodyW/g_bodyH`，调 `HelloPaint` 产出命令并推送。
4. 收 `EVT_INPUT`：
   - `kind=1 && is_down`：命中按钮矩形 → 切换 `pressed`，重推；命中输入框 → 置焦点。
   - `kind=4 && is_down`：可打印字符追加、`Backspace` 删除、`Enter` 换行。
   - `kind=3`：忽略（第一版无滚动）。
5. caret：500ms timer 翻转 `caretOn`，只重推本窗口。
6. 空闲不推：只有状态变化或 `layout_req` 才 `push`。

**验收**：A1 全条；`vm_wms` 日志能看到 `create window` / `accepted=1`；
rdp 抓帧（`tools/drive_vm_desktop.py` + `inspect_vm_frame.py`）能看到按钮与文本。

**恢复点**：`vm_app.exe --pipe ... --title Hello` 单独跑，窗口可见、可交互。

### A2 两实例并发

**做什么**

1. 不做代码改动，只起两份（`--title A` / `--title B`）。
2. 任务栏两条目由 `vm_shell` 自动出现；点击条目切焦点由 wms 仲裁。

**验收**：A2 全条；两个窗口内容分别正确，不串。

**恢复点**：两实例同时在线。

### A3 崩溃隔离

**做什么**

1. kill 其中一个实例。
2. 确认另一个实例继续工作（点击/输入仍生效），浏览器 SSE 不断。
3. 重启被 kill 的实例：拿新 `win_id`，内容重新建（不恢复旧状态）。

**验收**：A3 全条。

**恢复点**：`taskkill /F /IM vm_app.exe` 只影响被 kill 的窗口。

### A4 几何与 epoch

**做什么**

1. 用 `test_wms_shell` 或直接 `Wms_ConfigureWindow` 改窗口尺寸。
2. app 收 `layout_req(epoch+1)` → 用新 body 尺寸重排 → 推送。
3. 人为在 app 里加一段 300ms 延迟（仅测试用 `--slow-layout` 参数）再回内容：
   期间旧内容必须保留，其他窗口的输入与帧不受影响。

**验收**：A4 全条；`--slow-layout` 下桌面不卡。

**恢复点**：resize 前后内容正确重排。

### A5/A6（M7，本任务不执行）

接 Clay 与 XAML 后端；前置 `docs/task-shell-xaml-framework.md` P1/P2。

## 6. 测试与验证命令

```powershell
C:\msys64\mingw64\bin\cmake.exe --build vm-desktop/build -j 8
.\vm-desktop\build\vm_wms.exe --pipe '\\.\pipe\vmwm_dev' --screen 800x600
.\vm-desktop\build\vm_rdp.exe --port 19090 --pipe '\\.\pipe\vmwm_dev' --web-root assets --font assets/font.ttf
.\vm-desktop\build\vm_shell.exe --pipe '\\.\pipe\vmwm_dev' --font assets/font.ttf
.\vm-desktop\build\vm_app.exe --pipe '\\.\pipe\vmwm_dev' --title A
python tools\drive_vm_desktop.py 19090 wait 1200 snap appA
python tools\inspect_vm_frame.py temp\appA.txt --text Hello
```

e2e（A1–A3 稳定后加入）：`tools/e2e_wms.ps1` 新增 `app` 场景，断言窗口内容、
点击坐标、kill 隔离。

## 7. 风险与不做

| 风险 | 处理 |
|---|---|
| 首个 `layout_req` 到达前窗口空白 | 正常；app 不发内容，宿主只画装饰 |
| `epoch` 变化导致内容被丢弃 | 每次推送都用最近一次 `layout_req` 的 epoch；失败时等下一次 |
| 新旧 app 并存 | 旧 app 不改不删；新 app 独立 target，互不引用 |
| wmwms 无最大化状态 | `vm_app` 第一版不做最大化 |
| 关闭按钮语义 | wms 移除窗口后 app 收 `EVT_WINDOW_CLOSE`；不做「请求关闭」 |

**不做**：动画、主题热切换、应用私有资源、跨进程剪贴板、最大化、多窗口单实例。

## 8. 进度

| 步 | 状态 |
|---|---|
| A1 骨架与单窗口 | 未开始 |
| A2 两实例并发 | 未开始 |
| A3 崩溃隔离 | 未开始 |
| A4 几何与 epoch | 未开始 |
| A5 Clay 后端（M7） | 未开始 |
| A6 XAML 后端（M7） | 未开始 |
