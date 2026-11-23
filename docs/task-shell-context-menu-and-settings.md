# 任务：任务栏右键菜单与设置面板（对标 RetroBar 交互）

状态：进行中（2026-11-23 启动）
恢复点：见「执行记录」末尾。

## 背景

任务栏（`vm-desktop/vm_shell.c`）目前只有左键交互。RetroBar 的核心交互差距：
任务条目右键菜单、任务栏空白右键菜单（属性 → 设置）、设置对话框。

用户已确认的切分：
- 阶段 0：WM 右键通道（本任务）
- 阶段 1：菜单组件 + 右键菜单（本任务）
- 阶段 2：设置模型 + jp_xaml 设置窗口 + 持久化（本任务）
- 阶段 3：动态托盘图标（IPC 协议扩展）→ 不在本任务，登记到 `todo-`

用户要求：公共控件做抽象，方便复用。

## 现状事实（已核实）

- 输入协议上行已带 `button` 字段（`assets/vm_renderer.js` mousedown 上报 `e.button`），
  `vm_wm.c` 的 `VmWm_HandleInput` 已解析（`button = extract_int(jsonBody, "button", 0)`），
  但只放行 `button == 0`（`vm_wm.c:674`），右键直接丢弃。
- `VmWindowClickHandler` 签名无按键参数；唯一实现方是 `vm_shell.c` 的 `shell_click`。
- `VM_WM_MAX_WINDOWS = 8`：demo_jpx 常开 6 窗（任务栏+桌面+3 应用+开始菜单），
  加设置窗口 + 右键弹窗正好顶格。本任务把上限提到 10。
- 已有「无装饰/置顶/不抢焦点」窗口范式：`vm_shell.c`（手绘 paint + click handler）。
- 开始菜单目前是 demo 层普通装饰窗口（`demo_jpx.c:514`），非 popup。
- jp_xaml 表单能力已验证（`demo_xaml.c`）：CheckBox/ListBox/Button/绑定/handler。
- 主题预编译目录：`assets/themes/<name>/`（19 个），`JpxXamlTheme_LoadPrecompiled`
  + `JpxXamlTheme_ResolveImages` + `JpxXamlTheme_Apply` 链路已在 demo_xaml 验证。
- 「弹窗后任务栏消失」bug 用户确认已修复（2026-11-23，issues/INDEX 已相应归档）。

## 设计

### 新增 1：`vm_menu` WM 级通用 popup 菜单控件（可复用）

新文件 `vm-desktop/vm_menu.h` / `vm_menu.c`。

```c
#define VM_MENU_WINDOW_ID 0x7F01   /* 菜单窗口 id，避开 0x7F00（任务栏） */
typedef struct { const char *text; int enabled; } VmMenuItem;  /* text==NULL: 分隔线 */
int  VmMenu_Open(VmWm *wm, const VmTheme *theme, int x, int y,
                 const VmMenuItem *items, int count,
                 void (*onSelect)(int index, void *userData), void *userData);
void VmMenu_Close(void);
int  VmMenu_IsOpen(void);
```

- 单例（同一时刻一个菜单，与 Windows 行为一致）。
- 自建 VmWindow：`UNDECORATED|TOPMOST|PINNED|NOFOCUS`，手绘 paint + click handler，
  与任务栏同一范式。
- 菜单项文本宽度用 `VmFont_Width` 量（调用方需已加载字体；任务栏路径已保证）。
- 位置可溢出时钳制进画布（用 wm->width/height）。
- 命中菜单项 → onSelect(index) → 自动关闭。悬停高亮用 `selectedColor`，
  文字用 `fgColor`/`accentTextColor`，边框用 `borderColor`（v1 复用现有 token，
  不加主题格式）。
- 禁用项画 `disabledColor`，不可点。

### 新增 2：WM popup 外点关闭基础设施（通用，不绑死 vm_menu）

`VmWm` 增加：

```c
/* mousedown 落在 popup 之外时先关 popup 并消费本次按下（Windows 行为）。
   popupWindowId <= 0 表示没有 popup。 */
void VmWm_SetPopup(VmWm *wm, int windowId,
                   void (*dismiss)(void *userData), void *userData);
```

- `VmWm_HandleInput`：hit-test 之后、装饰按钮/焦点处理之前，若
  `isDown && wm->popupWindowId > 0 && (!hit || hit->id != popupWindowId)`
  → 调 dismiss → 消费本次按下并 return 1。
- `VmMenu_Open` 注册自己为 popup；`VmMenu_Close` 清除。

### 变更 3：右键通道

- `VmWindowClickHandler` 签名加 `int button`（全部调用点同步改）。
- `vm_wm.c:674` 放行 `(button == 0 || button == 2)`。
- 右键不改拖动/双击逻辑（那两处本来就只认 button==0）。

### 变更 4：`vm_shell` 接入右键与选项

- `shell_click` 加 button 参数；左/右键按下沿分别记录（`lastLeftSeq`/`lastRightSeq`）。
- 任务条目右键 → `VmMenu`：
  - 最小化 → 「还原」
  - 最大化 → 「还原」（去最大化）
  - 均非 → 「最大化」「最小化」
  - 分隔线
  - 「关闭」：走 `win->closeHandler`（与装饰 × 同路径）；无 handler 时 `VmWm_RemoveWindow`。
- 任务栏空白右键 → 「属性」。
- 新 API：
  - `VmShell_SetSettingsHandler(shell, handler, userData)`：属性菜单项触发。
  - `VmShell_SetShowClock(shell, int)` / `VmShell_SetShowTrayIcon(shell, int)`
    （默认均 1；paint 按开关绘制）。
- 左键语义不变。

### 变更 5：jpx 应用框架补口子

- `Jpx_ShellSetShowClock(app, show)` / `Jpx_ShellSetShowTrayIcon(app, show)`。
- `Jpx_SetShellSettingsHandler(app, handler, userData)`（转发给 VmShell）。

### 新增 6：`jpx_shell_settings` 设置组件（可复用）

新文件 `vm-desktop/jpx_xaml/jpx_shell_settings.h` / `.c`（依赖 jpx_xaml 与 jpx）。

```c
typedef struct {
    int showClock;      /* 任务栏显示时钟，默认 1 */
    int showTrayIcon;   /* 任务栏显示托盘图标，默认 1 */
    char theme[64];     /* 预编译主题目录名（assets/themes/ 下），空 = 不换 */
} JpxShellSettings;

/* 读配置文件（默认 <webRoot>/shell.ini，可传自定义路径）并应用主题与选项。
   配置不存在时用默认值，不算失败。返回 0 = 文件读取/解析失败但仍应用了默认。 */
int JpxShellSettings_Init(JpxApp *app, const char *webRoot,
                          const char *configPath, JpxShellSettings *cfg);
/* 打开设置窗口（jp_xaml 表单：时钟/托盘开关 + 主题列表 + 应用/确定/取消）。
   返回 winId，0 = 失败（窗口数满）。同一时刻一个实例。 */
int JpxShellSettings_Open(JpxApp *app, const char *webRoot, JpxShellSettings *cfg);
```

- 配置格式：`key=value` 行（`showClock` / `showTrayIcon` / `theme`）。
- 主题列表：`VmFs_ListDir` 扫描 `<webRoot>/themes/` 下的目录（isDir=1），
  按名字排序展示；应用时 `LoadPrecompiled(dir)` → `ResolveImages(dir)` → `Apply`。
  组件长期持有 `JpxXamlResourceTable`（颜色指针借给 VmTheme）与图片 provider。
- 窗口布局用 jp_xaml（CheckBox/ListBox/Button），绑定 `JpxShellSettings` 字段。
- 「应用」= 立即生效 + 写配置；「确定」= 应用 + 关窗；「取消」= 关窗不写。
- 测试可用 `configPath` 指向临时目录，避免污染 assets。

### 变更 7：demo_jpx 接线

- `JpxShellSettings_Init` 于启动调用；`Jpx_SetShellSettingsHandler` → 开设置窗。
- CMakeLists：demo_jpx 目标加 jpx_xaml 与 jpx_shell_settings 源文件、include 路径。

### 变更 8：`VM_WM_MAX_WINDOWS` 8 → 10

理由：任务栏+桌面+3 应用+开始菜单+设置窗口+右键弹窗 = 8 正好顶格，
弹窗失败比内存代价更伤（一个 VmWindow 约 400KB 级，+2 可接受）。

### 测试

新增 `test_menu.c`（不开服务/浏览器，直连 VmWm_HandleInput + VmMenu）：
1. 右键 mousedown（button=2）到达窗口 clickHandler 且 button 参数正确。
2. VmMenu_Open 后 IsOpen=1；点击菜单项命中 onSelect 且菜单自动关闭。
3. 菜单外 mousedown 触发 dismiss 且按下被消费。
4. 左键行为回归（button=0 原语义）。
5. 分隔线/禁用项不产生可选命中。

加入 CMakeLists；运行时间登记 `docs/test-timing.md`。

## 不做（明确排除）

- 快速启动栏（下个任务）。
- 动态托盘图标 API（IPC 扩展，走协议修订）。
- 开始菜单 popup 化（菜单组件可复用，另行任务）。
- 时钟 tooltip/日历。
- 任务条目拖拽重排、分组、溢出。

## 执行记录

### 2026-11-23 阶段 A–F 全部完成，端到端验证通过

实现（全部按设计落地，一处偏离见下）：
- `vm_wm.h/.c`：`VmWindowClickHandler` 加 `button` 参数；`VmWm_SetPopup` +
  HandleInput 中 popup 外点关闭（在装饰按钮/焦点处理之前消费）；窗口上限 8→10。
- `vm_menu.h/.c`：通用 popup 菜单控件（单例、钳制进画布、分隔线/禁用项、
  悬停高亮、选中即关）。
- `vm_shell.h/.c`：右键按下沿与左键分离（`lastRightSeq`）；任务条目右键菜单
  （还原/最大化/最小化/关闭，按窗口状态动态生成，「关闭」走 closeHandler）；
  空白右键「属性」；`VmShell_SetSettingsHandler`；`showClock`/`showTrayIcon` 选项。
- `jpx.h/jpx_app.c`：`Jpx_SetShellSettingsHandler`、`Jpx_ShellSetShowClock/TrayIcon`。
- `jpx_xaml/jpx_shell_settings.h/.c`：设置组件（XAML 动态拼装表单、主题预设扫描
  排序、LoadPrecompiled→Apply、shell.ini 持久化、ui 影子结构 + backup 回滚）。
- `jpx_xaml/jpx_xaml_view.c`：ListBox 支持 `Rows` 属性（走 `Jpx_ListBegin`
  可滚动变体；`rawSelected` 允许 -1 无初始选中）。
- `jpx_widget.c`：勾选标记 ✓→√（font.ttf 无 U+2713 字形，fontTools 查 cmap 证实）。
- `demo_jpx.c` + CMakeLists：接线；`assets/vm_renderer.js` 抑制 contextmenu。
- `test_menu.c`：30 断言全过；test_frame 回归 120 断言全过；test_fs 从仓库根运行
  19 断言全过（曾在 build 目录跑出 8 失败，系 cwd 依赖，非回归）。

端到端（agent-browser 合成输入，demo_jpx 9011）：任务条目右键菜单 ✓、
外点关闭不穿透 ✓、空白右键→属性→设置窗口 ✓、勾选/应用/落盘 ✓、主题切换
（xp-blue 应用后任务栏变 Luna 蓝）✓、启动时读 ini 应用主题与开关 ✓、
取消/确定/× 三条关闭路径 ✓。

执行中发现并修复的缺陷（已登记 issues/INDEX.md）：
1. 绑定偏移基址错误 → 点复选框段错误（无日志静默崩）。
2. layout 回调里同步销毁窗口 → Clay out of bounds 刷屏崩溃；改为意图 + 定时器。
3. `do_apply` 未把 ui 开关同步回 cfg，落盘旧值。
4. `Jpx_ListRow` 无选中（-1）被 `selected < 0` 钳成 0 首行误高亮。
5. 设置窗口 470 高（原 420 装不下按钮行）；ListBox 19 项全展开 → `Rows='8'`。
6. ✓ 字形缺失 → √。

遗留：无阻塞项。后续路线登记 `docs/todo-shell-roadmap.md`（托盘 IPC 扩展、
开始菜单 popup 化、快速启动栏、时钟 tooltip、条目拖拽）。

状态：完成，待提交。
