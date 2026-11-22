# 任务：demo_jpx 现代深色界面与桌面功能升级

> 自包含执行文档（2026-11-22 立项）。会话中断后据此恢复。
> 用户已确认：L1–L4 全做；风格为现代深色（深色桌面 + 蓝紫强调 + 轻阴影圆角）；
> 边界为「可动架构」——允许调整 guest 内 WM/任务栏职责与 JPX 公开头，但跨端协议零改动。

## 1. 目标与验收

把 `demo_jpx` 从当前粗糙演示（截图：窗口挤在上方、大片空白、扁平灰白、Start 独立小窗）升级为现代深色桌面：

- 浏览器视口被桌面填满，无白边；800×600 逻辑画布等比居中缩放，输入坐标仍正确。
- 深色桌面渐变、蓝紫强调、窗口轻阴影与圆角、标题栏按钮 hover/pressed 态。
- 控件精修：按钮立体与按下态、输入框 focus 光晕与选区高亮、列表斑马纹与表头、勾选标记。
- 桌面功能：任务栏开始按钮 + 时钟、桌面快捷图标、ProcMan/Files 一键启动、无装饰弹出菜单。
- 空闲行为允许每秒一次任务栏时钟帧（行为变更，需在 `test_frame` 中钉住）；除时钟外仍零帧。
- 跨端协议（`vm_display.h` 命令集、tiles、输入 schema）零改动；所有效果用现有原语实现。

## 2. 不动与可动边界

**不动**：`VM_CMD_*` 枚举、`VmDrawCommand` 结构、tiles 协议、前端输入 schema、`vm_renderer.js` 的输入上报字段。

**可动**：
- `assets/vm_renderer.js` 的页面布局与 CSS 缩放（不改事件上报逻辑）。
- `vm_theme.c` / `jpx_theme.c` 的默认 token；`demo_jpx.c` 的主题覆盖。
- `vm_wm.c` 的窗口装饰生成（阴影、圆角、标题按钮 hover）与 `mousemove` 置脏。
- `vm_shell.c` 任务栏职责：开始按钮回调、时钟、过滤无装饰窗口。
- `jpx.h` 新增 `JPX_WIN_*` 窗口标志；`jpx_window.c` 映射到 `VM_WIN_*`。
- `jpx_widget.c` 控件视觉与选区。
- `test_frame.c` 断言扩展；`docs/INDEX.md` 与 `workflows/INDEX.md` 更新。

## 3. 分层实施

### L1 画布适配（`assets/vm_renderer.js`）

- `boot()` 里 body 背景设为深色渐变，填满视口，去掉白边。
- stage 居中，canvas 物理尺寸保持 800×600（`tile` 网格依赖），CSS 宽高按 `min(vw/800, vh/600)` 等比缩放。
- 用 resize 监听重算 CSS 尺寸；`toCanvasPoint(e)` 已按 `getBoundingClientRect()` 换算，缩放后输入坐标不变。
- 验证：浏览器截图无白边；点击按钮仍精确命中；resize 后坐标仍正确。

### L2 主题与窗口装饰（`vm_theme.c`、`vm_theme.h`、`jpx_theme.c`、`demo_jpx.c`、`vm_wm.c`）

深色 token 草案（实现时可微调，但保持蓝紫强调）：

| token | 值 |
|---|---|
| desktopFrom / desktopTo | `rgba(13,15,24,1)` → `rgba(26,30,46,1)` |
| panelColor | `rgba(16,18,28,1)` |
| bgColor | `rgba(26,29,42,1)` |
| fgColor | `rgba(226,231,240,1)` |
| borderColor | `rgba(72,80,104,1)` |
| accentColor | `rgba(124,122,255,1)` |
| accentTextColor | `rgba(255,255,255,1)` |
| inputBgColor | `rgba(18,21,32,1)` |
| disabledColor | `rgba(74,80,98,1)` |
| selectedColor | `rgba(124,122,255,0.30)` |
| chrome.titleFocusColor | `rgba(86,84,176,1)` |
| chrome.titleIdleColor | `rgba(34,38,54,1)` |
| chrome.borderFocusColor | `rgba(124,122,255,1)` |
| chrome.borderIdleColor | `rgba(58,64,84,1)` |
| chrome.titleHeight / borderWidth | 30 / 1 |
| radius / padding / gap | 8 / 12 / 10 |

`demo_jpx.c` 覆盖 `hoverColor = rgba(156,152,255,1)`（替换原橙色）。

`vm_wm.c` 装饰生成（`build_one_window`）：
- 顺序：`w%d_shadow`（外扩 6px、深色半透明、圆角 10）→ `w%d_frame`（整窗圆角 8、边框色）→ `w%d_title`（缩进 1px、圆角 7）→ `w%d_ttext` → 按钮 hover 背景 → 按钮符号 → `w%d_border` → body → clip → 内容。
- 标题按钮 hover：用 `wm->pointerX/Y/pointerValid` 对 `chrome_button_rects` 做命中，hover 时画圆角高亮块；关闭按钮 hover 用红色系。
- `mousemove` 必须置脏，否则 hover 不刷新。

命令预算：每窗口装饰最多 +2 条，单窗口 64 条上限仍需满足；`test_frame` 钉住 `layerOverflow == 0`。

### L3 控件精修（`jpx_widget.c`）

- 按钮：normal/hover/pressed 三态；1px 深色 border；圆角 6。pressed 由 `pointerDown && Clay_Hovered()` 判定。
- TextInput：focus 时 2px accent 边框 + 外圈光晕（用两层 CLAY 元素模拟）；有选区时把文本拆三段，选区段带 `selectedColor` 背景；caret 保持。
- List：偶数行淡背景（斑马纹）；选中行蓝紫；hover 行提亮；表头由 `demo_jpx.c` 用 Label 补。
- Checkbox：勾选状态画 `✓` 文本（Noto Sans 支持）；box 背景深色。
- Panel/Separator：深色面板 + 柔和分隔线。

### L4 桌面功能（`demo_jpx.c`、`vm_shell.c`、`vm_shell.h`、`jpx.h`、`jpx_window.c`）

- `jpx.h` 的 `JpxWindowDesc` 增加 `unsigned flags`；定义 `JPX_WIN_UNDECORATED/TOPMOST/PINNED/NOFOCUS` 与 `VM_WIN_*` 同值；`jpx_window.c` 创建后调用 `VmWm_SetWindowFlags`。
- 任务栏：`vm_shell.h` 新增 `VmShell_SetStartHandler(shell, fn, user)`；`shell_layout` 左侧绘「开始」按钮，右侧绘 `HH:MM` 时钟；过滤 `UNDECORATED|NOFOCUS` 窗口，避免桌面图标占用任务栏条目。
- `demo_jpx.c`：
  - 去掉独立 Start 窗口；注册开始按钮回调切换菜单窗口。
  - 菜单改为无装饰弹出窗口，位于开始按钮上方；含 ProcMan / Files / 关闭。
  - 新增桌面快捷图标窗口（无装饰、置底、不抢焦点），两个大按钮直接启动 ProcMan / Files。
  - 窗口重排：A(24,24,240,230)、B(280,24,240,230)、List(536,24,240,230)、ProcMan(60,160,420,320)、Files(340,180,420,320)。
  - 每秒 timer 调 `VmShell_Refresh` + `Jpx_Invalidate` 驱动时钟。
- 空闲行为：除时钟帧外仍零帧；`test_frame` 断言时钟刷新只作废任务栏窗口。

## 4. 验证命令

```powershell
cd h:\github_repos\跨端图绘指令流
python tools/build_vm.py
.\vm-desktop\build\test_frame.exe --probe
.\vm-desktop\build\test_text_edit.exe --probe
.\vm-desktop\build\test_sys.exe
.\vm-desktop\build\test_fs.exe
python tools/start_vm_desktop.py demo_jpx
# 浏览器截图：agent-browser --session ui open http://127.0.0.1:9050/
```

预期：`test_frame` 原断言不回退；新增断言覆盖主题 token、阴影命令、hover 按钮、任务栏开始/时钟、无装饰窗口不入任务栏、时钟只作废 shell 窗口。

## 5. 恢复点

- L1 完成：`vm_renderer.js` 布局适配；浏览器截图验证。
- L2 完成：深色 token + 窗口阴影/圆角/hover；`test_frame` 装饰断言更新。
- L3 完成：控件视觉；浏览器截图验证选区/按钮态。
- L4 完成：任务栏开始/时钟 + 桌面图标 + 菜单；全量测试与截图。

## 6. 进度

- [x] L1 画布适配
- [x] L2 主题与窗口装饰
- [x] L3 控件精修
- [x] L4 桌面功能
- [x] 全量测试与截图验收
- [ ] 文档与工作流更新

## 7. 执行记录（2026-11-22）

**L1**：`vm_renderer.js` 页面背景改深色渐变，stage 按 `min(vw/800, vh/600)` 等比缩放居中；canvas 物理尺寸仍 800×600，`getBoundingClientRect()` 换算保证输入坐标正确；resize 监听重算。浏览器实测：视口 1262×568 时 canvas 757×568，比例一致，无白边。

**L2**：`vm_theme.c` / `jpx_theme.c` 默认 token 换现代深色（蓝紫强调 `rgba(124,122,255,1)`，圆角 8，标题高 30，边框 1）。`vm_wm.c` 装饰加阴影 `w%d_shadow`、圆角标题与窗口体、标题按钮 hover 高亮（关闭为红色）。`test_frame` 更新尺寸/颜色断言并新增阴影与圆角断言。

**L3**：`jpx_widget.c` 按钮加按下态与描边；输入框 focus 2px 边框、hover 过渡与选区高亮（`JpxTheme` 新增 `selectedColor`）；列表斑马纹与行尾按钮深色底；勾选框 ✓ 标记。

**L4**：`jpx.h` 新增 `JPX_WIN_*`、`selectedColor`、`Jpx_SetStartHandler`、`Jpx_ShellRefresh`；`vm_shell.c` 任务栏加开始按钮回调、HH:MM 时钟、过滤无装饰窗口；`demo_jpx.c` 窗口重排与中文化、桌面快捷图标层、开始菜单（无装饰）、每秒时钟 timer。

**字体**：原 `assets/font.ttf`（Noto Sans 拉丁子集）不含中文字形，中文显示为方块。替换为 `C:\Windows\Fonts\simhei.ttf`（9.7MB，含中文），`vm_server.c` 静态资源上限从 4MB 提到 16MB（HTTP 413）。wasm 度量对照：`"Window A"@14 → 56px`，与 guest 侧一致。`test_frame` 111 项、`test_text_edit` 66 项、`test_sys` 16 项、`test_fs` 19 项全部通过。

**期间修掉的缺陷**：
- 任务栏时钟用 `char clock[16]` 栈变量，Clay 保留指针到 EndLayout 之后导致乱码；改 `static`。
- 开始菜单意图用 `-2`，被 `launcher_tick` 的 `if (g_pendingLaunch < 0) return;` 拦截；改为 `2`。
- 桌面图标层原 y=210 被窗口 A 遮挡标签；下移到 y=272。
- 用户通过不同窗口边框色诊断确认命中判定正常，根因在意图值拦截。

**浏览器验证**（agent-browser，跳过 `wait --load`，SSE 长连接会卡住）：
- 中文正确显示；深色桌面、蓝紫按钮、阴影圆角、任务栏开始/时钟均正常。
- 点任务栏开始按钮 → 菜单弹出；点菜单 ProcMan → 菜单关闭且进程窗口打开。
- 字体加载需约 5 秒（9.7MB base64），期间 `window.__vmRenderer.ready=false`，属预期。

**未验证**：窗口拖动后的控件命中、列表滚轮、输入框选区高亮的浏览器端实际效果（仅 guest 侧命令与测试断言覆盖）。
