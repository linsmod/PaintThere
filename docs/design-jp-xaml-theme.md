# 设计：jp_xaml ResourceDictionary 主题体系

> **这不是现状。** 本文冻结于 2026-11-23 的决策，只写「要改成什么」与边界。
> 执行进度见 `docs/task-jp-xaml-theme.md`。
> 参考对象：RetroBar `Themes/*.xaml`（WPF ResourceDictionary + DynamicResource）。

## 1. 要解决的问题

RetroBar 的主题是完整的 `ResourceDictionary`：单个 `Windows XP Blue.xaml` 有 155 个 `x:Key`，其中 116 个 Brush（80 Solid、23 LinearGradient、5 RadialGradient、4 DrawingBrush、4 ImageBrush），控件 XAML 里有 249 处 `{DynamicResource}` 引用；切换主题靠替换 `Application.Current.Resources.MergedDictionaries`（`DictionaryManager.cs:59-86`）。

现有 jp_xaml 没有资源字典概念，主题走 `VmTheme`/`JpxTheme` 少量 token，且 resources 在连接时一次性下发。要让 XAML 主题真正驱动 WM、shell 与控件，必须补三样东西：**资源字典解析与查找**、**多 stop 渐变的跨端表达**、**资源热更新**。

## 2. 边界（冻结）

| 项 | 决策 | 理由 |
|---|---|---|
| `VM_CMD_*` 枚举 | **不改** | 渐变仍是 `definePattern`，只扩 descriptor 内容 |
| `VmDrawCommand` | **不改** | 资源命令在外置数组，扩容不影响绘制命令尺寸 |
| 绘制原语 | **不改** | 渐变仍通过 `drawRect` 的 paint 参数引用 patternId |
| `vm_display.h` | **改** `VmDefinePatternArgs`、`VM_DISPLAY_MAX_RESOURCES` | 多 stop + 容量，见 §4 |
| `assets/app.js` | **改** `applyPattern` 与 `definePattern` | 多 stop 渐变必须在前端可绘制 |
| `vm_server.c` | **改** 新增 `resources-update` 广播 | 一次性 resources 无法热切换 |
| 前端输入协议 | **不改** | 主题不涉及输入 |
| 动画 | **不做** | 沿用 jp_xaml v1 边界 |

禁止项：

- 不实现 WPF 的 `ControlTemplate` / `Style` / `Setter` / `DataTrigger`（那是控件模板，不是配色资源）。
- 不引入依赖属性的运行时反射；资源查找是 key → 定长值的线性查表。
- 不把 DrawingBrush 的矢量几何下发；v1 只做 Solid / LinearGradient（多 stop）与数值资源。
- 不让 XAML 直接写 CSS 颜色到控件；控件只引用资源 key。

## 3. 分层

```text
主题文件 .xaml（ResourceDictionary 子集）
  → jp_xaml 解析器扩展：x:Key + Brush/GradientStop/Double
      → VmResourceTable：key → 类型化资源（定长，POD）
          → 三条消费路径：
             1. definePattern/defineStyle → VmDisplayList.resources → 跨端
             2. VmTheme 派生 → VmWm_SetChrome（WM 装饰）+ VmShell_SetTheme（任务栏）
             3. JpxTheme 派生 → JPX 控件
          → 主题切换：重建表 → 广播 resources-update + 全量帧
```

责任边界：

| 组件 | 负责 | 不负责 |
|---|---|---|
| jp_xaml 解析器 | 资源字典标签、x:Key、GradientStop | 颜色/渐变的跨端语义 |
| VmResourceTable | key → 类型化值的查表与生命周期 | XAML 语法、前端绘制 |
| VmTheme 派生 | 从资源表取色填 WM/shell/控件 theme | 资源热更新时机 |
| vm_server | `resources-update` 事件广播 | 资源内容生成 |
| 前端 | 覆盖缓存 + 多 stop 渐变绘制 | 资源表结构 |

## 4. 协议扩展（最小）

### 4.1 `VmDefinePatternArgs`

现状 `{patternId, type, color, from, to, direction, imgId, tiling}`——两 stop、方向被前端忽略。扩展为：

```c
#define VM_PATTERN_MAX_STOPS 8

typedef struct {
    char color[VM_DISPLAY_MAX_STR];
    float offset;               /* 0..1 */
} VmGradientStop;

typedef struct {
    char patternId[VM_DISPLAY_MAX_STR];
    VmPatternType type;         /* 新增 VM_PATTERN_RADIAL */
    char color[VM_DISPLAY_MAX_STR];          /* solid */
    VmGradientStop stops[VM_PATTERN_MAX_STOPS];
    int stopCount;
    char direction[VM_DISPLAY_MAX_STR];      /* "0,0->0,1" 这类端点对 */
    char imgId[VM_DISPLAY_MAX_STR];
    char tiling[VM_DISPLAY_MAX_STR];
} VmDefinePatternArgs;
```

- `from`/`to` 不删除：老的 `VmTheme_AppendResources` 继续写这两个字段，前端保留兼容分支（两 stop 时走原路径）。
- `stopCount >= 2` 时前端使用 `stops[]`，忽略 `from`/`to`。
- `direction` 改为实际端点字符串（如 `"0,0,0,1"`），前端 `createLinearGradient(x0,y0,x1,y1)` 按比例映射；解析失败退回对角。
- `VM_PATTERN_RADIAL`：前端 `createRadialGradient`，`direction` 承载 `cx,cy,r` 比例。
- **序列化格式**：`definePattern` 的 descriptor 对象新增可选 `stops` 数组与 `direction`；老字段保持不变，前端按存在与否分支。

### 4.2 资源容量

`VM_DISPLAY_MAX_RESOURCES` 从 16 提到 256。理由：单个 RetroBar 主题有 155 个 key，加上 jp_xaml 自身的 8 条固定资源。

**尺寸实测修正（2026-11-23）**：原估「增大约 200 KB」低估了。实测 `sizeof(VmDisplayList)` = 1,022,880 B（约 1.02 MB），`sizeof(VmResourceCommand)` = 2,912 B，`sizeof(VmDefinePatternArgs)` = 896 B。若 stops 的 color 沿用 `VM_DISPLAY_MAX_STR`（128 B），8 个 stop 会让每个 pattern 再增 ~1 KB，256 资源下 `VmDisplayList` 将超过 1.8 MB——而 `test_frame.c`、`jpx_app.c` 里有栈上实例，会爆默认栈。

对策（实现必须同时做）：

- `VmGradientStop.color` 用专用紧凑长度 `VM_DISPLAY_MAX_COLOR 32`（CSS 颜色最长 `rgba(255,255,255,0.999)` 仅 24 字符），不用 128。
- 所有栈上 `VmDisplayList` 实例（`test_frame.c:149/376/432`、`demo_frame.c:21`、`demo_wm.c:30`、`jpx_app.c:73`）改为 `static` 或堆分配。
- `VmDrawCommand` 尺寸不变，tile 差分不受影响（资源不进 tiles）。

### 4.3 资源热更新

新增 SSE 事件：

```text
event: resources-update
data: [ {definePattern/defineStyle/loadImg 命令数组} ]
```

- guest 侧 `VmServer_BroadcastResources(updateJson)`：向所有已连接客户端推一次。
- 前端 `EventSource` 监听 `resources-update` → 对每条命令调 `definePattern/defineStyle/loadImg` 覆盖缓存 → 标记需要全量重绘 → 下个 `tiles` 帧用 `fullframe` 语义重发所有 tile。
- 老客户端（不监听该事件）不会崩，只是主题不更新；不引入协议版本号。

## 5. XAML 资源字典子集

支持：

```xml
<ResourceDictionary>
  <SolidColorBrush x:Key="TaskbarBackground" Color="#3888e9" />
  <LinearGradientBrush x:Key="TaskbarGradient" StartPoint="0,0" EndPoint="0,1">
    <GradientStop Color="#3888e9" Offset="0.0" />
    <GradientStop Color="#1941a5" Offset="1.0" />
  </LinearGradientBrush>
  <RadialGradientBrush x:Key="TrayToggle" GradientOrigin="0.15,0.15" RadiusX="0.75" RadiusY="0.7">
    <GradientStop Color="#8ed6f9" Offset="0.0" />
    <GradientStop Color="#0671eb" Offset="0.9" />
  </RadialGradientBrush>
  <system:Double x:Key="TaskbarHeight">30</system:Double>
</ResourceDictionary>
```

资源查找表达式：`{StaticResource key}` 与 `{DynamicResource key}` 在 v1 **等价**（都做一次查表；热更新靠资源广播 + 全量帧，不做前端层面的引用重解析）。不支持 `{Binding}` 与资源引用的嵌套。

映射规则：

| XAML | 内部资源 | 跨端命令 |
|---|---|---|
| `SolidColorBrush` | `VM_PATTERN_SOLID` + color | `definePattern` |
| `LinearGradientBrush` + `GradientStop` | `VM_PATTERN_GRADIENT` + stops + direction | `definePattern` |
| `RadialGradientBrush` | `VM_PATTERN_RADIAL` + stops + origin/radius | `definePattern` |
| `system:Double` | 数值 token | 不跨端（guest 布局用） |
| `Style` / `ControlTemplate` | 不解析 | 不跨端 |

## 6. 派生主题

资源表提供按 key 查询：`VmRes_GetColor(table, "TaskbarBackground", fallback)` 返回 CSS 字符串。

`VmTheme` 派生：

- `VmTheme.chrome.titleFocusColor` ← `AccentColor` 或 `TaskButtonBackground`
- `VmTheme.chrome.titleIdleColor` ← `TaskbarBackground`
- `VmTheme.bgColor` ← `WindowBackground`
- `VmTheme.panelColor` ← `TaskbarBackground`
- `VmTheme.fgColor` ← `TaskTextColor`
- `VmTheme.accentColor` ← `AccentColor`
- 其余 token 按语义取固定 key，缺失则用默认值

`VmShell_SetTheme(shell, theme)`：shell 的 12 处硬编码颜色改为从 `VmTheme` 读；需要 `VmColor_Parse()` 共享函数（从 `Jpx__Color` 提到 `vm_theme.c`）。

## 7. 明确不做（v1 非目标）

- Style / ControlTemplate / Setter / DataTrigger / BasedOn
- DrawingBrush 矢量几何、ImageBrush 图片路径解析
- 19 个 RetroBar 主题的完整兼容（v1 以 Windows XP Blue 为验收样本，能做多少算多少）
- 运行时创建/删除资源；资源表在主题加载时固化
- 主题编辑器、主题文件格式校验器

## 8. 验收定义

- `test_jpx_xaml_theme`：解析 `Windows XP Blue.xaml` 子集，断言 key 数量、Solid/Gradient/Radial 解析、Stop 顺序与 offset、`{StaticResource}` 展开。
- `test_frame`：断言资源命令里 `definePattern` 带 `stops` 数组、多 stop 渐变序列化正确、资源容量 > 155。
- 前端：多 stop 渐变在 Canvas 上渲染为多色过渡，方向按 `direction` 生效。
- 主题切换：运行时加载第二个主题，所有客户端收 `resources-update`，画面颜色变化，无需重连。
- WM/shell/控件：标题栏、任务栏开始按钮、条目高亮、控件强调色随主题变化。
