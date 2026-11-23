# 任务：任务栏按钮模板层次化 —— 对齐 RetroBar TaskButton 模板

状态：进行中（2026-11-23 启动）

## 1. 目标与证据

当前任务栏条目只是一层两 stop 渐变 + 圆角 + 4 边 bevel，与 RetroBar 的视觉差距
主要来自模板层次被压扁。RetroBar `Windows XP Blue.xaml` 的 `TaskButton`
模板（行 804-848）是 8 层嵌套 Border，绘制顺序（外→内）：

1. 外圈 4 边：`TaskButton{Top,Left,Bottom,Right}Border`（各 1px，SolidColorBrush）
2. 主渐变底：`TaskButtonBackground`（LinearGradientBrush，**带 stop offset**，
   XP Blue 是 `#4892f7@0.0 → #3980f4@0.17`，17% 高度后平色）
3. 内圈 3 边：`TaskButtonInner{Left,Right,Bottom}Border`（各 1px）
4. 高光层：`TaskButtonBackgroundHighlight`（对角渐变 `0,0 → 0.03,1`，
   `#70aafa@0 → 透明@0.3`，Luna 玻璃感来源）

三个状态各有完整一套 key，命名规则 `{Base}{Suffix}`，Suffix ∈ `""` / `"Hover"` /
`"Pressed"`（XP Blue 还定义 PressedHover 与 Flashing，v1 不做）。

现有瓶颈（都已核实）：

- 解析层**无需改动**：`JpxXamlResource` 已存 `stops[]`（color+offset）与
  `direction`（jpx_xaml_theme.h:40-42），预编译产物同样携带。
- 协议**无需改动**：`VmDefinePatternArgs` 支持 `stops[8]` + 任意线性方向
  `"x0,y0,x1,y1"`；前端 `gradientFill` 按比例端点 + offset 渲染。
- 差距在：`VmTheme` 只有 From/To 成对字段（`gradient_ends` 丢 offset）；
  `vm_theme.c append_gradient_pair` 固定两 stop 全高插值；`vm_shell.c` 只画
  单层渐变 + bevel，无内圈边与高光层。

## 2. 方案

### A. token 层（vm_theme.h）

新增（指针借自资源表，与 VmTheme 既有约定一致）：

```c
#define VM_THEME_TASK_STOPS 8
typedef struct { const char *color; float offset; } VmThemeStop;

/* RetroBar TaskButton 模板层集：stopCount >= 2 时 shell 按层绘制，
   否则退回 From/To 单层渐变 + bevel（经典主题无这些 key，走降级）。 */
typedef struct {
    VmThemeStop stops[VM_THEME_TASK_STOPS]; int stopCount;
    const char *direction;                 /* NULL = "0,0,0,1" */
    VmThemeStop hlStops[VM_THEME_TASK_STOPS]; int hlStopCount;
    const char *hlDirection;               /* NULL = 沿用 direction */
    const char *borderTop, *borderLeft, *borderBottom, *borderRight;
    const char *innerLeft, *innerRight, *innerBottom;
} VmTaskButtonLayer;
```

`VmTheme` 新增 `taskNormal / taskHover / taskActive` 三个 `VmTaskButtonLayer`。
既有 From/To 字段与 bevel 字段保留：作为无层集主题的降级路径。

### B. 资源生成（vm_theme.c）

新增 `append_task_layers(out, name, layer)`：stopCount>=2 时发出
`p_<name>`（stops+direction）+ `s_<name>`；hlStopCount>=2 时加发
`p_<name>_hl` + `s_<name>_hl`。`VmTheme_AppendResources` 中三个状态改为
「层集优先，否则退回 append_gradient_pair」。highlight 全透明（Pressed 态）
也照发，前端画透明层无副作用。

### C. 派生（jpx_xaml_theme.c）

`derive_task_layers(table, st, suffix)`：按 suffix（""/"Hover"/"Pressed"）拼
key 采集——主渐变 stops+direction、高光 stops+direction、外 4 边与内 3 边
SolidColorBrush。key 缺失即留空（stopCount=0），不造 shade 兜底（降级由
legacy 字段承担）。在 DeriveTheme 尾部对三个状态各调一次。
`accentColor = TaskButtonBackground 首色` 的既有逻辑不变。

### D. shell 绘制（vm_shell.c）

条目内缩盒（ix,iy,iw,ih）上，层集可用（对应状态 stopCount>=2）时按序：

1. 主渐变 `s_task / s_task_hover / s_task_active`（全盒，radius）
2. 外 4 边 1px 条（既有 tb_item_top/left/bottom/right id 复用）
3. 内 3 边 1px 条（inset 1px；新 id tb_item_il/ir/ib_<winId>）
4. 高光层 `s_task_hl / s_task_hover_hl / s_task_active_hl`：
   `(ix+2, iy+1, iw-4, ih-3)`（对角玻璃）
5. 文字不变

状态选择：focused → active 层集；hovered 且非 focused → hover 层集（缺则
normal 层集 + 叠加旧 s_task_hover）；否则 normal。层集不可用走现有
「单层渐变 + bevel 反转」路径，一字不改。层集路径不再做 bevel 颜色反转
（各状态有自己的边色）。

### E. 字体实验（优先级 2，单独评估）

XP 中文界面是宋体（小字号为内嵌点阵）。stb 只渲染轮廓，SimSun 12px 轮廓
观感存疑。用 `tools/extract_system_font.py simsun --subset` 提取子集，
渲染探针对比 simhei/simsun 12-14px 后再决定是否替换 `assets/font.ttf`
（全局影响：guest 度量 + 前端光栅 + test_frame 几何断言都可能变）。
结论与截图证据写回本文档；不盲目替换。

## 3. 验证

- `test_jpx_xaml_theme --probe`：新增层集采集断言（stops/offset/direction/
  内外边色/Hover/Pressed 后缀/缺 key 留空）+ AppendResources 产物 JSON 含
  stops。基线 115 项。
- `test_frame --probe`：几何不受影响（无 paint 断言），全量回归。
- `precompile_themes.py` 重扫 19 主题 0 失败（理论上无需重扫——采集层未变，
  跑一遍确认）。
- `demo_xaml --preset xp-blue` 冒烟：inspect_vm_frame 检查 tb_item 层命令序
  （bg → 外边 → 内边 → 高光），浏览器截图目视玻璃高光。

## 4. 恢复点与进度

阶段 A/B（token+资源）→ C（派生）→ D（shell）→ 测试 → 冒烟 → 字体实验 → 收尾。

- [x] A token 层（2026-11-23）
  - `VmThemeStop` / `VmTaskButtonLayer`（VM_THEME_TASK_STOPS=8），VmTheme 新增
    `taskNormal / taskHover / taskActive`；From/To 与 bevel 字段保留为降级路径。
- [x] B 资源生成（2026-11-23）
  - `append_task_layers`：stopCount>=2 发 `p_<name>`（stops+direction）与
    `p_<name>_hl` 高光 pattern + 对应 s_* style；否则退回 `append_gradient_pair`。
- [x] C 派生采集（2026-11-23）
  - `derive_task_layers(table, st, suffix)`：""/"Hover"/"Pressed" 三态，采
    TaskButtonBackground(Highlight){Suffix} 渐变 stops+offset+direction 与
    外 4 边/内 3 边 SolidColorBrush；key 缺失留空，不做 shade 兜底。
- [x] D shell 层次绘制（2026-11-23）
  - 状态选择 focused→active、hovered→hover（缺则 normal + 旧悬停叠加）；
    绘制序：主渐变 → 外 4 边 → 内 3 边（inset 1px，新 id tb_item_il/ir/ib）
    → 高光 (ix+2, iy+1, iw-4, ih-3)。旧路径原样保留。
- [x] 测试与预编译确认（2026-11-23）
  - `test_jpx_xaml_theme --probe` 137 项 0 失败（新增 test_task_layers 22 项：
    offset 0/0.17 保留、对角方向 0.03、内外边色、Hover 3 stop、Pressed 缺失降级、
    线路含 stops/offset/p_task_hl、无 pressed key 不发高光）。
    中途 1 次断言写错（legacy 降级仍发 s_task_active 属设计内），修正后全过。
  - `test_frame --probe` 120 项 0 失败（换宋体后同样全过）。
  - `precompile_themes.py` 19 主题 0 失败（采集层未变，理论上无需重扫）。
- [x] demo 冒烟 + 截图（2026-11-23）
  - demo_jpx + xp-blue：SSE 帧含 tb_item_hl/il/ir/ib；respack 容器含
    p_task_hl（stops offset 0.0/0.3、direction 0.0000,0.0000,0.0300,1.0000）、
    p_task stops offset 0.17；浏览器截图 Luna 蓝条目与深蓝 active 态成立。
- [x] 字体实验与结论（2026-11-23）
  - `extract_system_font.py simsun --subset` → 2.5MB 宋体子集，替换
    assets/font.ttf（原 simhei 9.7MB 在 git 历史）。宋体是 XP 中文 UI 真实
    用字，12px 轮廓渲染清晰度可接受，test_frame/test_jpx_xaml_theme 全过。
    stb 不用 SimSun 内嵌点阵（只渲染轮廓），真 Windows 的点阵小字无法复刻，
    这是与 RetroBar（GDI/ClearType）文字观感的残余差距来源。
- [x] 收尾：INDEX / debug 文档登记（2026-11-23）
