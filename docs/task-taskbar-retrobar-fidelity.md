# 任务：任务栏对齐 RetroBar —— 几何全部主题数据驱动

状态：进行中（2026-11-23 启动）

## 1. 目标

按「框架提供能力、数据进主题包」的原则，消除任务栏移植的视觉还原度差距。
对照对象：`H:\gitee_repos\RetroBar\RetroBar\`（只读参考）。

问题清单（2026-11-23 对照结论，见会话记录）：

1. `TaskButton.Width` / `Tray.Width` 是不存在的 key，条目宽 110、托盘宽 76 是
   `DeriveTheme` 硬编码兜底；RetroBar 真实值：`TaskButtonWidth=160`、
   `TaskButtonMinWidth=45`、`TaskButtonMargin.Right=3`（System.xaml），
   条目宽随窗口数动态伸缩铺满整行（`TaskList.xaml.cs` `SetTaskButtonWidth`）。
2. 解析层 `metric_prop` 不收 Margin/Padding/BorderThickness，而 RetroBar 的关键
   间距全在这些属性里（StartButton Margin `0,0,1,-3`、StartIcon 25×20 Margin
   `10,0,0,0`、StartLabel FontSize 19 右 Margin 26 带阴影、Tray Margin `10,0,0,0`、
   Clock Margin `8,1,11,6`、TaskButton 模板 Padding `0,3,0,2` + Content `8,3`、
   ShowDesktopButton Content `5,1,3,0`）。
3. `shell_button_height` 的 `bh > h` 钳制与注释矛盾；XP 是 32 高按钮放进 30 高
   栏（顶对齐、底部溢出画布外）。
4. 「显示桌面」按钮应独立于托盘（RetroBar 的 ShowDesktopButton 在托盘左侧、
   全高、内容驱动宽），目前画在托盘内部且无点击行为。
5. 开始按钮图标 22×22 居中排版 vs StartIcon 25×20 左 10px；StartLabel 12px 正体
   vs 19px 斜体带阴影。

## 2. 方案

### A. 解析层新增采集（vm-desktop/jpx_xaml/jpx_xaml_theme.c）

- `parse_thickness4`：解析 WPF Thickness 文本 `"4"` / `"4,8"` / `"l,t,r,b"`
  （含负值）成四值。
- Thickness 资源元素（属性形式与文本形式）→ 4 条 NUMBER：
  `<Key>.{Left,Top,Right,Bottom}`。
- Style Setter 的 Margin/Padding/BorderThickness：字面量直接解析；
  `{DynamicResource K}` 解析 Thickness 资源 K；收成
  `<StyleKey>.<Prop>.{Left,Top,Right,Bottom}`。
- ControlTemplate 内带 `x:Name` 的元素的 Margin/Padding/BorderThickness →
  `<StyleKey>.<x:Name>.<Prop>.<Side>`；CornerRadius（取单值）→
  `<StyleKey>.<x:Name>.CornerRadius`。这是模板支持的最小子集：只取命名元素的
  几何，不执行模板。
- `JPX_XAML_MAX_RESOURCES` 256 → 512（厚度条目增量估算 ~150；NUMBER 不进
  下发通道，只有内存代价）。
- 不收：DataTrigger.Setters 内的 Setter、未命名模板元素（v1 边界）。

### B. token 层（vm_theme.h / vm_theme.c）

删除 `panelItemWidth`、`panelTrayWidth`、`panelStartIconSize`（语义被取代）。
新增（全为 int；0 = shell 兜底）：

| 字段 | 来源 key | 兜底 = System.xaml/XP 惯例 |
|---|---|---|
| panelStartMarginRight | `StartButton.Margin.Right` | 1 |
| panelStartIconW / H | `StartIcon.Width/Height` | 25 / 20 |
| panelStartIconMarginLeft | `StartIcon.Margin.Left` | 10 |
| panelStartLabelFontSize | `StartLabel.FontSize` | 0（沿用通用 fontSize） |
| panelStartLabelMarginRight | `StartLabel.Margin.Right` | 26 |
| panelStartLabelShadow | `StartLabelShadow` 资源存在 | 0 |
| panelItemWidthDefault | `TaskButtonWidth` | 160 |
| panelItemWidthMin | `TaskButtonMinWidth` | 45 |
| panelItemMarginRight | `TaskButtonMargin.Right` | 3 |
| panelTaskListMarginLeft | `TaskListScrollViewer.Margin.Left` | 4 |
| panelTaskTextInsetX | `TaskButton.Content.Margin.Left` | 8 |
| panelTrayMarginLeft | `Tray.Margin.Left` | 10 |
| clockMarginL/T/R/B | `Clock.Margin.*` | 8/1/11/6 |
| panelShowDesktopIconW / H | `ShowDesktopIcon.Width/Height` | 16 / 16 |
| panelShowDesktopMarginL / R | `ShowDesktopButton.Content.Margin.Left/Right` | 5 / 3 |
| panelRadius（改来源） | `TaskButton.ButtonTopBorder.CornerRadius` | 2 |

`TaskbarHeight`、`StartButton.MinWidth/Height` 等既有读取不变。
默认主题（现代深色）维持自身观感值，仅按新字段语义填数。

### C. shell 动态布局（vm_shell.c）

- 条目宽三段式（照抄 `SetTaskButtonWidth` 横向语义）：
  `avail = width - startW - startMR - listML - trayZone`；
  `n*(defaultW+mr) <= avail` → defaultW；
  否则 `floor(avail/n)-mr`，余数像素分给前 k 个条目；
  低于 min → min（无滚动，v1 不做溢出箭头）。
- `trayZone` 内容驱动 = showDesktop 按钮（iconW + margins）+ trayMarginLeft +
  clock 内容宽（Clock margins + 字体实测宽）。无时钟/图标时逐项收缩。
- 按钮高：去掉 `bh > h` 钳制；`bh >= h` 时顶对齐（`by = y`），否则居中。
  30 高栏 + 32 高按钮 = 底部 2px 溢出画布外（与 XP 一致）。
- 开始按钮：图标 W/H 分离 + 左 margin；label 字号取 panelStartLabelFontSize，
  右 margin 26；阴影 = 同文深色副本偏移 (+1,+1) 先画（框架不做字体合成，
  伪阴影与伪粗体同一手法）。斜体无字体能力，登记 todo 不做。
- 条目文字缩进 panelTaskTextInsetX；条目间距 = panelItemMarginRight。
- 点击命中与绘制共用同一几何（ShellLayout）；「显示桌面」按钮点击 =
  全部最小化 / 再次点击还原（XP 语义，toggle）。

### D. 验证

- `test_jpx_xaml_theme --probe`：新增厚度/模板采集与派生断言。
- `test_frame --probe`：任务栏点击几何按新公式更新。
- `python tools/precompile_themes.py` 重扫 19 主题 0 失败（采集层变化自动
  进预编译产物，二进制格式不变）。
- `demo_xaml --preset xp-blue` 冒烟：任务栏几何像素抽查（开始按钮 32 高、
  条目宽随窗口数变化、托盘贴右缘）。

## 3. 恢复点

- 阶段 A（解析层）→ 阶段 B（token）→ 阶段 C（shell）→ 阶段 D（测试/预编译）
  → 文档收尾。
- 基线：实施前 `test_jpx_xaml_theme` 66 项、`test_frame` 104 项全过。

## 4. 进度

- [x] A 解析层厚度/模板几何采集（2026-11-23）
  - `parse_thickness4`（1/2/4 值，负值合法）+ `add_thickness_res`（`<Key>.<Side>` 四条
    NUMBER）+ `resolve_thickness_ref`（`{DynamicResource}` 引用 Thickness 资源）。
  - Style Setter 的 Margin/Padding/BorderThickness 收成 `<StyleKey>.<Prop>.<Side>`；
    ControlTemplate 内带 x:Name 的元素收成 `<StyleKey>.<x:Name>.<Prop>[.<Side>]`，
    CornerRadius 取单值。组合 key 超 `VM_DISPLAY_MAX_ID` 跳过（防截断碰撞）。
  - 新增 `DropShadowEffect` 的 Color 采集（`<Key>.Brush`）。
  - `JPX_XAML_MAX_RESOURCES` 256 → 768（厚度/模板几何让 XP Silver 达 510 条，
    512 余量不足；NUMBER 只驻 guest 内存）。
- [x] B VmTheme token 扩展与派生（2026-11-23）
  - 删除 `panelItemWidth`/`panelTrayWidth`/`panelStartIconSize`；新增开始按钮
    margin/图标 W/H/字号/阴影、条目默认宽/下限/间距、任务列表左距、文字缩进、
    托盘左距、时钟四边 margin、显示桌面图标 W/H 与左右距、显示桌面底纹
    `showDesktopFrom/To`（`TaskListScrollButtonBackground` → 托盘色降级）、
    `startLabelShadowColor`。`panelRadius` 改源（模板 CornerRadius）。
  - 兜底值 = System.xaml（TaskButtonWidth 160 / MinWidth 45 / margin 3）与
    XP Blue 惯例。
- [x] C shell 动态布局（2026-11-23）
  - 条目宽三段式：默认 160 放得下就默认（尾留空）→ 收缩铺满整行（余数摊给
    前 N 条）→ 下限 45（无滚动，放不下的条目截断不画）。
  - 托盘内容驱动：时钟实测宽 + Clock Margin + Tray.Margin.Left；显示桌面按钮
    独立于托盘（全高、图标 16 + 左右 margin），点击 = 全部最小化/还原 toggle。
  - 按钮高去掉 `bh > h` 钳制；`bh >= h` 时顶对齐（XP 32 入 30，底部溢出画布外）。
  - 开始按钮：图标 W/H 分离左 10px，label 左对齐跟图标、右留 26px，字号取
    `StartLabel.FontSize`，阴影 = 同文深色副本偏移 (1,1)。
- [x] D 测试 + 预编译 + 冒烟（2026-11-23）
  - `test_jpx_xaml_theme --probe` 115 项 0 失败（新增 `test_geometry_collection`
    11 项：Thickness 属性/文本形式、负值、DynamicResource 解析、模板 Padding/
    BorderThickness/CornerRadius、阴影色）。
  - `test_frame --probe` 120 项 0 失败（任务栏点击几何公式随新 token 更新）；
    `test_menu` 30、`test_text_edit` 66 项均过。
  - `precompile_themes.py` 重扫 RetroBar 19 主题 0 失败（资源数 ~46-510）。
  - `demo_xaml --preset xp-blue` 冒烟（temp/xpblue_smoke.txt，tile 局部坐标 +
    行偏移还原）：tb_start (0,570,99,32) 顶对齐溢出 2px ✓、图标 (10,+,25,20) ✓、
    标签 x=35 含阴影/伪粗体 ✓、条目 (104,570,160,32) 文字缩进 8 ✓、显示桌面
    (17,+,24,30) ✓、托盘 (751,+,49,30) 贴右缘 ✓、时钟 (759,576) ✓。
  - 一次瞬时链接失败（exe 被上一轮测试进程占用），重跑构建即恢复，非代码问题。
- [x] 收尾：todo 登记、INDEX（2026-11-23）
