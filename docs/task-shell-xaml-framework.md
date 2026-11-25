# 任务：shell XAML 化（jp_xaml 双后端 + 任务栏声明式）

> 自包含执行文档（2026-11-24 立项，未开工）。可在会话中断或上下文丢失后据此恢复。
> 上游设计：`docs/design-ui-producer-decoupling.md`（U1–U5，冻结）。
> 前置任务：`docs/task-vm-wms-merge.md` M5（外壳进程化 + VM 去 Clay + 模板抽函数）。
> 本任务从 M5 完成后开始，不与 M5 混在同一次验收里。

## 1. 目标与验收

把桌面外壳（任务栏）从「C 硬编码模板层次」改为「XAML 声明 + Direct 后端产命令」，
使 `vm_shell` 成为**保留模式渲染的范本**，并让 `jp_xaml` 摆脱「只能回放为 `Jpx_*`」的单一后端。

验收（每条可复现）：

1. `jp_xaml` 的 parser 与 state 不感知后端；同一份 XAML 可分别由 Clay 后端与 Direct 后端回放。
2. Direct 后端能产出多 stop 渐变、位图切片、模板多层（4 边 + 内 3 边 + 高光），不经过 Clay。
3. 任务栏外观由 `shell/taskbar.xaml` 决定；`vm_shell.c` 里不再有「主渐变 → 外 4 边 → 内 3 边 → 对角高光」的绘制顺序代码。
4. 19 个预编译主题逐一比对：条目层序、内缩、圆角、三态选择与改造前一致（允许命令 id 变化，不允许视觉语义变化）。
5. `vm_shell` 的链接命令行不含 Clay；`vm_wms` 同样（M5.2 已保证）。
6. 既有测试全过：`test_jpx_xaml` 47 项只增不减、`test_jpx_xaml_theme` 137 项、`test_menu` 30 项、`test_frame` 120 项。

## 2. 现状基线（2026-11-24 核对）

| 事实 | 位置 |
|---|---|
| `jp_xaml` 执行器只有 `Jpx_*` 一条路 | `jpx_xaml/jpx_xaml_view.c`（198 行，10 个标签，文件头注释即声明「唯一调用 Jpx_* 的 TU」） |
| 元素树/绑定/handler 已是纯数据，与后端无关 | `jpx_xaml/jpx_xaml_state.c`、`jpx_xaml_internal.h` |
| 绑定只有 BOOL / INT / TEXT 三种标量 | `jpx_xaml.h:18-22` |
| 主题数据已完备：颜色、多 stop 线性/径向渐变、NUMBER、PATH、IMAGE | `jpx_xaml_theme.h:23-50` |
| 任务栏模板层次硬编码在 C | `vm_shell.c:363-448`（三态层集、4 边、内 3 边、高光） |
| 任务栏几何硬编码在 C | `vm_shell.c` 的 `shell_geometry` / `shell_item_rect` / 溢出截断 |
| Clay 只能单色背景是既有结论 | `vm_shell.c:13`、`vm_shell.h:34` |
| 既有测试断言数 | `test_jpx_xaml` 47、`test_jpx_xaml_theme` 137（见 `docs/test-timing.md`） |

## 3. 最小 XAML 子集（只加这些）

### 3.1 新标签

| 标签 | 作用 | Direct 后端 | Clay 后端 |
|---|---|---|---|
| `Canvas` | 绝对定位容器（`X`/`Y`/`Width`/`Height`） | 支持 | 不支持（返回错误，不静默降级） |
| `DockPanel` | `Dock="Left\|Right\|Fill"` | 支持（Top/Bottom v1 留空） | 部分（映射到 Row/Column） |
| `Layers` | 按序叠加的子层容器 | 支持 | 不支持 |
| `Layer` | 单层：`Paint`（资源 key）、`Inset="l,t,r,b"`、`State`、`Radius` | 支持 | 不支持 |
| `Repeat` | 按 ItemsSource 重复子树 | 支持 | 不支持 |

### 3.2 属性扩展

- `Border` 增加 `Background` / `BorderThickness` / `CornerRadius`，值必须是 `{StaticResource key}` 或 `{Binding field}`。
  不接受字面色值——颜色仍只能来自资源，避免应用层绕过主题（`design-jp-xaml.md` §4.1 的约束在应用侧保持；shell 通过资源拿到任意皮肤）。
- 图片切片：`Source="{StaticResource X}" Slice="0,0.3333,1,0.3333"` → 生成 `imgId#slice`（与 `vm_shell.c:267-272` 的写法等价）。

### 3.3 列表绑定（新数据结构，硬前置）

```c
/* 一个条目的只读字段集；v1 固定这几个，不做通用数据路径。 */
typedef struct {
    int id;
    const char *title;
    int focused;
    int minimized;
} JpxXamlItem;

typedef int (*JpxXamlItemSource)(void *user, int index, JpxXamlItem *out);

int JpxXamlView_SetItemSource(JpxXamlView *view, const char *name,
                              JpxXamlItemSource fn, void *user);
```

- XAML：`<Repeat ItemsSource="windows">` 的子节点即每项模板。
- 绑定作用域：`Repeat` 子树内 `{Binding Title}` / `Id` / `Focused` / `Minimized` 取 item；未命中则落回 model（与现有 `JpxXaml__BindingField` 同规则）。
- 计数由 `JpxXamlItemSource` 返回 -1 终止；不做增量更新，每次回放全量遍历（条目数 ≤ 10）。

### 3.4 三态

- 节点属性 `IsActive="{Binding focused}"`、`IsHover="{Binding hovered}"`。
- `Layer` 的 `State` 取 `normal` / `hover` / `active`；同时命中时 `active` 优先。
- 只支持这两个布尔，不做 Trigger 集合、不做优先级仲裁表。

## 4. 步骤

### P1 jp_xaml 后端接口 + Direct 后端骨架

**做什么**

1. 按 `design-ui-producer-decoupling.md` §4 定义 `JpxXamlBackend`（vtable）。
2. `jpx_xaml_view.c` 的 `render_node` 改为经 `view->backend` 调用；新增 `jpx_xaml_backend_jpx.c`（把 vtable 翻译成 `Jpx_*`）与 `jpx_xaml_backend_direct.c`（骨架：只支持现有 10 个标签的绝对定位映射）。
3. `JpxXamlView_Create` 增加后端参数；原签名保留一个 `JpxXamlView_CreateJpx` 包装，避免 `demo_xaml` 立即改动（若改动量小则直接改调用点，不留兼容层——本项目不做后向兼容转换）。
4. 文本度量走 vtable：Clay 后端用现有 `jpx_measure`，Direct 后端用 `VmFont_Width` / `VmFont_LineHeight`。

**验收**

- `test_jpx_xaml.exe --probe`：47 项全过，新增「Direct 后端产出命令序列」「两种后端对同一棵树产出的文本位置一致」断言（≥ 55 项）。
- `python tools/start_vm_desktop.py demo_xaml --port 9060`：表单行为与改造前一致（点击、输入、Tab、列表）。

**恢复点**：`jpx_xaml_backend_*.c` 可独立编译，`test_jpx_xaml --probe` 通过。

**结果（2026-11-25，已完成）**

- `jpx_xaml.h` 增加 `JpxXamlBackend`（rect/border/text/image/path/text_width/line_height/frame_begin/frame_end）与 `JpxXaml_RenderDirect`。
- 新增 `jpx_xaml_render_direct.c`：只遍历元素树，不调用任何 `Jpx_*`；v1 支持绝对定位与 `StackPanel` 纵/横流式布局。
- 新增 `jpx_xaml_backend_cmdbuf.c/.h`：`JpxXamlBackend_CmdBuf()` 把调用写成 `VmCmdBuffer`，文本度量走 `vm_font`。
- CMake 新增静态库 `jpx_xaml_direct`（parser/state/Direct 渲染/cmdbuf 后端/vm_display/vm_font）。
- `test_jpx_xaml` 47 → **58**（新增 11 条：两叶子文本、流式 y 位移、按钮矩形与显式宽高）。
- 旧路径未动：`demo_xaml` 抓帧 95827 B、表单文本正常；其余单测断言数不变。
- 布局规则（v1）：未给 `Height` 的流式项占 `line+8`；`StackPanel.Gap` 默认 6。

### P2 XAML 子集扩展（标签 + LIST + 三态）

**做什么**

1. parser 增加 `Canvas` / `DockPanel` / `Layers` / `Layer` / `Repeat` 五个标签与 `Dock` / `Inset` / `State` / `ItemsSource` / `Slice` / `IsActive` / `IsHover` 属性。
2. `Border` 的 `Background` / `BorderThickness` / `CornerRadius`。
3. `JpxXamlView_SetItemSource` + `Repeat` 展开；绑定作用域规则见 §3.3。
4. 三态选择见 §3.4；Direct 后端实现 `Layers` 的按序绘制与 `Inset` 内缩。

**验收**

- `test_jpx_xaml.exe --probe`：新增解析（5 标签、7 属性）、错误定位（未知标签 / `Repeat` 缺 `ItemsSource` / `Layer` 缺 `Paint`）、`Repeat` 展开 0/1/3/10 项、三态优先级断言（≥ 75 项）。
- Direct 后端对一份含 3 层 `Layers` + 4 边 + 内 3 边的测试 XAML，产出命令条数与层序符合预期（断言 id 顺序）。

**恢复点**：`test_jpx_xaml --probe` 通过；Direct 后端能画出一组测试模板命令。

### P3 taskbar.xaml 取代硬编码

**做什么**

1. 新增 `vm-desktop/shell/taskbar.xaml`：`DockPanel`（左开始 / 中 `Repeat` 条目 / 右托盘+时钟）+ 条目模板的 `Layers`（主渐变、外 4 边、内 3 边、高光）+ 三态。
2. `vm_shell.c` 的 `shell_paint` 改为：算几何（沿用 `shell_geometry`）→ 填 `JpxXamlItemSource` → 调 `JpxXaml_Render`（Direct 后端）。删除模板层次的绘制代码（`vm_shell.c:363-448`）。
3. 主题切换路径不变：`JpxXamlTheme_Apply` / `DeriveTheme` 仍产出资源与 `VmTheme`；`taskbar.xaml` 通过 `{StaticResource}` 取值。

**验收**

- 同主题下，改造前后 fullframe 的命令**语义**等价：每条目仍有 1 主层 + 至多 4 外边 + 至多 3 内边 + 至多 1 高光；内缩、圆角、文本位置一致。允许命令 id 与顺序内的等价替换，差异需逐条列出。
- 截图对比：`python tools/start_vm_desktop.py` + `agent-browser`，XP Silver 与现代深色各一张。
- `vm_shell` 链接命令行不含 Clay。

**恢复点**：任务栏由 `taskbar.xaml` 驱动；`shell_paint` 不再含模板绘制顺序。

### P4 19 主题逐一验证

**做什么**

对 `assets/themes/` 下全部 slug（当前 19 个）逐个启动 `vm_shell --theme <slug>`，抓帧并比对。

**验收**

- 每主题记录：条目数、每条目层数、内缩、圆角、三态选择。
- `python tools/inspect_vm_frame.py temp/<slug>.txt --tiles` 无缺层、无越界。
- 结果写入 `docs/test-timing.md` 与本文 §7。

**恢复点**：19 主题结果表落盘（`temp/shell-xaml-themes.md`，不进 Git）。

### P5 文档与工作流

1. 更新 `docs/arch-vmdesk.md`：`jp_xaml` 双后端、shell 为 Direct 后端使用者、`vm_shell` 不链 Clay。
2. 更新 `docs/design-jp-xaml.md` 顶部：标注「回放为 Jpx_* 不再是唯一执行器」，链接本文。
3. 更新 `docs/INDEX.md`、`tools/INDEX.md`。
4. 在 `workflows/` 新增或更新「UI producer 后端扩展」工作流（若已有相近流程则分叉）。

**验收**：`git grep -n "唯一调用 Jpx_\*"` 不再出现在 `jpx_xaml_view.c`；文档与实际链接依赖一致。

## 5. 测试与验证命令

```powershell
C:\msys64\mingw64\bin\cmake.exe --build vm-desktop/build -j 8
.\vm-desktop\build\test_jpx_xaml.exe --probe
.\vm-desktop\build\test_jpx_xaml_theme.exe --probe
.\vm-desktop\build\test_frame.exe --probe
pwsh tools/e2e_wms.ps1 -SkipBuild
python tools/start_vm_desktop.py demo_xaml --port 9060
```

主题比对补充：`python tools/precompile_themes.py assets/themes`（仅在主题源变更时需要）。

## 6. 风险与不做

| 风险 | 缓解 |
|---|---|
| 两后端行为分叉，应用出现两套渲染 | 双后端只服务不同使用方；同一份 XAML 的双后端一致性只对「共同子集」断言，不支持的能力显式报错 |
| Direct 后端被要求做 flex 布局 | v1 明确只做绝对定位 + Dock(Left/Right/Fill)；需要 flex 的界面走 Clay 后端 |
| 任务栏几何也被要求搬进 XAML | v1 不做；几何留在 C（`shell_geometry`），XAML 只描述视觉层与状态 |
| 19 主题逐条比对耗时 | 先做 3 个代表主题（XP Silver / 现代深色 / 一个位图皮肤），再全量 |

**本次不做**：通用布局引擎、动画/Storyboard、`Style`/`Template` 完整语义、属性路径绑定、应用侧任意字面色值、跨进程控件状态。

## 7. 进度

| 步 | 状态 |
|---|---|
| P1 后端接口 + Direct 骨架 | 已完成（2026-11-25） |
| P2 XAML 子集扩展 | 未开始 |
| P3 taskbar.xaml | 未开始 |
| P4 19 主题验证 | 未开始 |
| P5 文档与工作流 | 未开始 |

## 8. 依赖关系

```text
M5.1 外壳进程化 ──┐
M5.2 vm_wm 去 Clay ├──► 本任务 P1 ──► P2 ──► P3 ──► P4 ──► P5
M5.3 VmTemplate_Paint ─┘        │
                                └──► M7：新 app 的 XAML 后端（task-vm-app.md A6）
```

M5.3 的 `VmTemplate_Paint` 是本任务 P3 的现成积木：先有「按 `VmTaskButtonLayer` 描述绘制」的函数，再让 XAML 后端调用它。若 M5.3 未完成，P3 会退化成在 XAML 后端里重写一遍层绘制。

P1/P2（后端接口 + Direct 后端 + 最小 XAML 子集）同时是 M7 的公共前置：

- `docs/task-vm-app.md` A6（新 app 的 XAML 能力）；
- `docs/task-vm-wms-merge.md` M7.2（shell 的开始菜单与设置面板）。

旧 `jpx_xaml` 只有 parser/state 可复用，执行层（`Jpx_*`）随旧 app 一起删除，
新 app 与 shell 的 XAML 能力全靠这条后端。
