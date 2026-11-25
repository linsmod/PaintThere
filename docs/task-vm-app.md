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
| A5 | Clay 控件层（M7.3） | 新写 `app/ui_clay.c`：表单控件、Tab/caret/列表滚轮；协议与输入行为不变 |
| A6 | XAML 与主题（M7.4） | 表单与主题能力对齐 `demo_xaml`；19 个预编译主题可切换 |

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
| A1 骨架与单窗口 | 已完成（2026-11-25） |
| A2 两实例并发 | 已完成（2026-11-25） |
| A3 崩溃隔离 | 已完成（2026-11-25） |
| A4 几何与 epoch | 已完成（2026-11-25） |
| A5 Clay 后端（M7） | 未开始 |
| A6 XAML 后端（M7.4） | 已完成（2026-11-25）：XAML 表单 + 主题加载/热切换 |

## 9. 执行记录：A1–A4（2026-11-25）

**做了什么**

1. `app/main.c`：`WmsClient(APP)` → 建窗 → 订阅 `EVT_LAYOUT_REQ｜EVT_INPUT｜EVT_WINDOW_CLOSE｜EVT_THEME_CHANGED`；
   首个 layout_req 认领窗口（create ACK 之前到达）；`push_content` 用最近 epoch；
   caret 500ms timer；`--x/--y/--slow-layout/--resize-after` 四个测试参数。
2. `app/ui_hello.c/.h`：9 条手写命令（bg / label / btn / btn_label / input / input_border /
   input_text / caret / hint），按钮 toggle，输入框尾部截断 + caret。
3. CMake target `vm_app`：`app/main.c app/ui_hello.c vm_display.c vm_font.c` + `wms_client`；
   不链 `vm_wm` / `vm_server` / `vm_shell` / Clay。

**验收证据**

| 项 | 实测 |
|---|---|
| A1 body | `layout epoch=1 body=418x249`（420 − 2×1 边框，280 − 30 标题 − 1） |
| A1 内容 | 帧文本 `vm_app / A1 (no Clay)` @ (77,102) = 窗口(40,60)+body(1,30)+内容(16,12) |
| A1 点击 | 画布 (120,160) → body (79,70) → `button -> pressed=1 clicks=1`；帧 `Clicked: 1  [on]` @ (89,156) |
| A1 输入 | `key h` / `key i` → 帧文本 `hi` @ (85,209)，caret 由 500ms 相位控制 |
| A2 并发 | 两实例各一窗；任务栏两条目 `Hello` / `Second`；内容互不覆盖 |
| A3 隔离 | kill Second → wms `remove window id=3`；帧无 Second；Hello 仍 `button -> pressed=1 clicks=1` |
| A4 epoch | `resize -> 520x340` → `layout epoch=2 body=518x309` → `push epoch=2 accepted=1`；hint y 从 221 → 368 |
| A4 不卡 | Slow app `--slow-layout 300` 期间，另一窗口的帧照常更新（60009 B 增量批次） |
| e2e | `app` 场景 8 条断言，总计 **61/61** |
| 截图 | `temp/m6-a1.png`：两窗口 + 任务栏两条目 + 时钟 |

**观察**

- **建窗竞态**：后启动的进程可能先拿到 `win_id`（实测 Second=2、Hello=3）。任务栏按
  `created_seq` 排序，所以条目不按启动命令的先后排列，这是预期行为。
- **修复 M5.1 遗留**：默认主题没有条目渐变时，`vm_shell` 用 `fgColor`（文字色）当条目底色，
  出现浅底白字。改为 `bgColor`，`focused` 仍用 `accentColor`。该修复会改变 `demo_jpx`
  的任务栏帧，取代旧「166531 逐字节一致」基线。
- `Wms_PushContent` 在 app 侧是同步等 ACK；`--slow-layout` 的延迟只卡本进程，
  wms 合成与其他 app 不受影响（A4 已验证）。

**恢复点**：`vm_app.exe` 可独立跑两实例，e2e `app` 场景通过。

## 9.1 执行记录：A6 XAML 表单（2026-11-25）

**做了什么**

1. 新增 `app/ui_xaml.c/.h`：内置表单 XAML（两个 TextBox、两个 CheckBox、一个 ListBox、一个 Button），
   复用 `jpx_xaml` 的 parser/state 与 Direct 后端；`--xaml <file>` 可替换为外部文件。
2. `vm_app` 增加 `--ui xaml` / `--xaml <file>`；`push_content` 按模式分派；
   `EVT_INPUT` 的 down/up 都交给 `JpxXaml_DirectPointer`，键盘走 `JpxXaml_DirectKey`；
   caret 相位由 500ms timer 翻转。
3. CMake：`vm_app` 改链 `jpx_xaml_direct`（库自带 `vm_display` / `vm_font`），源加 `app/ui_xaml.c`。

**验收证据**

| 项 | 实测 |
|---|---|
| 表单渲染 | 19 条命令；标题 `jp_xaml form (Direct backend)` @ (61,90) |
| 文本输入 | 点 TextBox 后输入 `a`/`b`，帧文本 `ab` @ (69,122) |
| 复选框 | 点 notify → `form ok ... notify=1` |
| 列表/按钮 | OK 按钮读回 `name='ab' email='' notify=1 remember=0 sel=0` |
| 回归 | 8 个单测不变；e2e 61/61 |

**主题（同日补齐）**

- `JpxXamlResourceTable` 改为命名 struct，`jpx_xaml.h` 可前向声明；新增
  `JpxXamlView_SetResources`，Direct 渲染遇到 `{StaticResource X}` 时查表取颜色。
- `jpx_xaml_direct` 库加入 `jpx_xaml_theme.c` + `vm_theme.c`（`Apply` 已在独立 TU，不引入 `JpxApp`）。
- `vm_app` 增加 `--theme <slug>`、`--themes-root <dir>`、`--check-theme <slug>`（自检后退出）。
- `EVT_THEME_CHANGED` → 重新 `LoadPrecompiled` 并重推内容。

**主题验收**：19 个预编译主题逐个 `--check-theme`，全部加载成功
（`2000`=29、`xp-silver`=507、`xp-zune-style`=514 …，`failed=0`）；
运行中由 `test_wms_shell --theme xp-blue` 触发热切换，app 日志出现
`theme changed -> 'xp-blue'` 与 `theme loaded ... resources=491`。

## 10. 下一步（M7.3 / M7.4 / M7.5）

| 步 | 内容 | 归属 |
|---|---|---|
| M7.1 | `JpxXamlBackend` + Direct 后端 + 最小 XAML 子集 | `docs/task-shell-xaml-framework.md` P1/P2 |
| M7.2 | shell 补开始菜单 / 设置面板 / 右键菜单 | `task-vm-wms-merge.md` §8 M7.2 |
| A5 | 新 app 接 Clay 控件层 | M7.3 |
| A6 | 新 app 接 XAML 与 19 主题 | M7.4 |
| 删除 | `jpx` / `jpx_xaml` 执行层 / `demo_jpx` / `demo_xaml` | M7.5 |

**A5 的决策**：不复用 `jpx_widget.c` 的绘制代码（与 `JpxApp` 单例、focusable 表深度耦合）。
新写 `app/ui_clay.c`，只复用纯逻辑 `JpxTextEdit` 与 `JpxList`。

旧 app 在 A5/A6 完成并验收后删除；删除前必须完成 M7.2（shell 的菜单/设置），
否则会连带删掉尚未迁走的桌面功能。
