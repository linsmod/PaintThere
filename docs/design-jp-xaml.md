# 设计：jp_xaml 声明式 UI 层

> **这不是现状。** 本文冻结于 2026-11-23 的决策讨论，只写「要做什么」和边界。
> 执行进度与实测结果见 `docs/task-jp-xaml.md`；当前实现以代码为准。

## 1. 要解决的问题

现有 JPX 应用用 C 代码逐帧声明控件：每帧调 `Jpx_RowBegin` / `Jpx_Button` / `Jpx_TextInput`，控件 id、状态、事件回调都分散在布局函数里。应用越复杂，界面结构和业务状态越难分离。

jp_xaml 给应用一种声明式写法：XAML 描述界面结构与交互连接，C 写的解析器和执行器维护元素树与状态，每帧把树回放成现有 JPX 调用。布局仍由 Clay 完成，协议、WM、前端均不变。

一句话：**XAML 是外层编写模型，Clay 仍是内部布局引擎。**

## 2. 边界（冻结）

| 层 | 变化 | 理由 |
|---|---|---|
| `jpx.h` / JPX 内部 | 不改 | jp_xaml 只调用现有公开 API，不穿透到 JPX 内部 |
| Clay / `clay_frame_adapter.c` | 不改 | Clay 继续负责 measure/arrange 与 render command 生成 |
| `vm_wm` / `vm_display` / `vm_tiles` / `vm_server` | 不改 | 窗口管理、命令模型、切块、传输与 jp_xaml 无关 |
| 前端资源（`assets/*.js`） | 不改 | 跨端仍只有绘制原语与原始输入 |
| `jpx.h` 的 theme 体系 | 不改 | 控件颜色仍来自 theme token，XAML 不传颜色 |

由此得出的禁止项：

- 不改 `VM_CMD_*`、`VmDrawCommand`、tiles 协议、输入 schema。
- 不把 XAML 标记、元素树、绑定表达式下发到前端。
- 不引入 per-element 图层或 `defineDrawProgram mode:"loop"` 依赖（前端未实现，见 `assets/app.js` 的 `startDrawProgram`）。
- 不穿透 JPX 公开头去拿 `Clay_Context` 或 `VmWm`。

## 3. 分层

```text
应用：state 结构体 + handler 函数 + .xaml 文本/文件
  ───────────────────────────────────────────────────
jp_xaml：解析器（XAML 子集 → 元素树）
         视图（元素树 + 绑定表 + handler 表 + 状态）
         执行器（每帧回放为 JPX 调用、把控件返回值转成事件）
  ───────────────────────────────────────────────────
JPX：Jpx_* 控件 API、焦点链、文本编辑状态机、theme
  ───────────────────────────────────────────────────
Clay：布局与 render commands → ClayFrame_AppendCommands
  ───────────────────────────────────────────────────
交付层：vm_display / vm_tiles / vm_server（零改动）
```

责任边界：

| 组件 | 负责 | 不负责 |
|---|---|---|
| jp_xaml 解析器 | 标签、属性、文本、实体、错误定位 | 布局、业务语义 |
| jp_xaml 视图 | 元素树、绑定表、handler 表、XAML 属性到控件的映射 | Clay 布局、文本编辑状态机 |
| JPX | 控件交互、焦点链、caret/选区、theme | XAML 标记与业务状态 |
| 应用 | state 结构体、handler 实现、创建窗口 | XAML 解析细节、Clay 调用 |

## 4. XAML 子集（v1 词汇表）

只实现下列元素与属性；未列出的属性解析后忽略，不报错（便于后续扩展）。

| 元素 | 映射的 JPX 调用 | 支持属性 |
|---|---|---|
| `Window` | 可选根，仅提供 Title/尺寸元数据；窗口由应用 `Jpx_WindowOpen` 创建 | `Title`, `Width`, `Height` |
| `StackPanel` | `Jpx_ColumnBegin/End` 或 `Jpx_RowBegin/End` | `Orientation` = `Vertical`/`Horizontal` |
| `Border` | `Jpx_PanelBegin/End` | 无（内边距与背景走 theme） |
| `TextBlock` | `Jpx_Label` / `Jpx_LabelEx` | `Text`, `Wrap`, `Align` |
| `Button` | `Jpx_Button` | `Name`/`x:Name`, `Content`, `Click` |
| `CheckBox` | `Jpx_Checkbox` | `Name`/`x:Name`, `Content`, `IsChecked`, `Click` |
| `TextBox` | `Jpx_TextInput` | `Name`/`x:Name`, `Text`, `TextChanged` |
| `ListBox` | `Jpx_ListBox` | `Name`/`x:Name`, `Items`（分号分隔）, `SelectedIndex`, `SelectionChanged` |
| `Separator` | `Jpx_Separator` | 无 |
| `Spacer` | `Jpx_Spacer` | `Width`, `Height` |

语法约束：

- 属性值用单引号或双引号包裹；支持 `&amp; &lt; &gt; &quot; &apos;` 五个实体。
- 支持自闭合 `<Button />`、注释 `<!-- -->`、元素文本内容。
- 不支持：DTD、CDATA、命名空间声明、XAML 2009 语法、属性元素语法、资源字典、Style/Template、相对/绝对 URI 加载。
- `{Binding Field}` 只识别单一字段名，不做属性路径（不解析 `A.B`）。
- `Click` / `TextChanged` / `SelectionChanged` 的值是 handler 名，大小写敏感，绑定到 C 侧注册表。

### 4.1 为什么不做 Style/Template 与任意颜色

JPX 的既定设计是控件颜色来自 theme token，应用不传颜色（`docs/task-jpx.md` §5.1）。XAML 层引入 `Background="#rrggbb"` 会破坏这条边界，故 v1 只支持 theme。Style/Template 是 WPF 语义的大头，放进 v1 会让解析器与目标都失焦；如确需，另立任务，并仍落到 theme token 上。

## 5. 数据模型与绑定

元素树用**索引而非指针**表达，节点存在定长数组里：父节点下标、首子节点下标、兄弟下标。这样树在解析后稳定，不受数组重分配影响。

绑定表把 XAML 里的字段名映射到应用 state 结构体里的偏移：

```c
typedef enum {
    JPX_XAML_BIND_BOOL = 0,   /* 1 字节，用于 CheckBox */
    JPX_XAML_BIND_INT,        /* 4 字节，用于 ListBox SelectedIndex */
    JPX_XAML_BIND_TEXT        /* 定长 char 数组，需要 cap */
} JpxXamlBindKind;

int  JpxXamlView_Bind(JpxXamlView *view, const char *name,
                      JpxXamlBindKind kind, size_t offset, size_t cap);
```

- `JpxXamlView_Create` 接收 `void *model`，所有偏移相对该基址。
- `Text="{Binding name}"` → 回放时从 `model + offset` 读到 `Jpx_TextInput` 的临时 buf；`Jpx_TextInput` 返回 1 时把 buf 写回同一地址，再触发 `TextChanged`。
- `IsChecked="{Binding ok}"` → 同理读写 `int`/`bool` 槽。
- 绑定字段不存在时解析成功但回放忽略该属性；测试要钉住这个降级行为，不静默截断。

handler 注册表：

```c
typedef void (*JpxXamlHandler)(JpxXamlView *view, int nodeIndex, void *userData);

int  JpxXamlView_On(JpxXamlView *view, const char *name,
                    JpxXamlHandler fn, void *userData);
```

handler 签名只给视图与节点下标；应用通过 `JpxXamlView_Node` 取属性、通过 `JpxXamlView_Model` 取 state，不传 JPX/Clay 类型进 handler。

## 6. 帧回放与事件流

### 6.1 回放

应用把 `JpxXamlView_Render` 包装成 `JpxWindowDesc.layout` 回调。每帧：

1. 从根按深度优先遍历元素树。
2. 容器节点调 `Jpx_*Begin`，子节点回放完调 `Jpx_*End`。
3. 控件节点从绑定表读当前值，调对应 `Jpx_*`，拿到「本帧是否发生交互」。
4. 发生交互时写回绑定字段，查该节点的事件名，调用 handler。
5. handler 改状态后由视图调 `Jpx_Invalidate`；JPX 的下一次布局自然反映新状态。

因为 JPX 的可聚焦控件按**声明顺序**登记，XAML 树的深度优先回放顺序就是 Tab 顺序，不需要 jp_xaml 自己维护焦点链。

### 6.2 事件

- `Button`：`Jpx_Button` 返回 1 → 调 `Click`。
- `CheckBox`：`Jpx_Checkbox` 返回 1 → 写回绑定 → 调 `Click`。
- `TextBox`：`Jpx_TextInput` 返回 1 → 写回绑定 → 调 `TextChanged`。
- `ListBox`：`Jpx_ListBox` 返回 1 → 写回 `SelectedIndex` → 调 `SelectionChanged`。

事件名未注册时不报错，控件状态仍然更新。一个事件只触发一次，与帧重建次数无关（复用 JPX 的按下沿语义）。

## 7. 明确不做（v1 非目标）

- **动画 / 动效**：无 Storyboard、无属性插值、无动画 tick。扩展点保留在视图层（统一调度器位置），但不实现。
- 完整 WPF 依赖属性、视觉树渲染、模板化控件、数据触发器。
- 任意颜色与 per-control 样式；颜色一律走 theme。
- 网络/运行时重载 XAML；XAML 文本由应用内嵌或本地文件读入。
- 多窗口自动管理：一个 view 对应一个窗口，窗口生命周期仍由应用负责。
- 跨端协议扩展、新增绘制原语、前端改动。

## 8. 验收定义

- 应用源码只 include `jpx.h` 与 `jpx_xaml.h`，不出现 `clay` / `vm_display` / `vm_tiles` / `json` 词汇。
- 一个表单 demo 用 XAML 描述三个输入框、两个勾选框、两个按钮、一个列表，交互行为与手工 JPX 写法一致。
- `test_jpx_xaml`：解析、树结构、实体、错误定位、绑定读写、handler 触发、未注册事件降级均有断言，支持 `--probe`。
- 既有测试 `test_frame` / `test_text_edit` / `test_sys` / `test_fs` 全部通过，协议零改动。
- 跨端运行：浏览器内表单可点击、可输入、Tab 切换、勾选、列表选择；空闲零帧。
