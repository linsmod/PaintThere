# 设计：UI 生产层解耦（displayList 为契约）

> **这不是现状。** 本文冻结于 2026-11-24 的决策讨论，只写「要改成什么」与边界。
> 当前实现见 `docs/arch-vmdesk.md`；执行记录见 `docs/task-shell-xaml-framework.md`；
> 进程化本身见 `docs/task-vm-wms-merge.md`。

## 1. 一句话

**UI 架构的核心是产生 displayList（`VmDisplayList` / `VmCmdBuffer`），渲染与命令生成解耦。**
Clay 是可选后端，不是架构核心。开发者可以用 Clay、XAML、三方 HTML+CSS 类库、从 GTK 抽的 UI 库，
也可以用即时模式或保留模式编写，只要最终产出同一份命令流。

## 2. 分层

```text
声明源（任选其一，互不感知）
  C 直接调用 Jpx_*  |  XAML  |  HTML+CSS 适配  |  GTK 适配  |  直接写 VmCmdBuffer
        │
        ▼
保留模式层 jp_xaml：元素树 + 绑定 + 状态 + 模板层次
        │  （parser / state 与后端无关）
        ├── 后端 A：Clay   → Jpx_* 控件 → clay_frame_adapter ─┐
        └── 后端 B：Direct → 绝对定位/模板层 → VmCmdBuffer ───┤
                                                              ▼
                                        窗口内容契约 = VmCmdBuffer
                                                              │
                              WMS：装饰 + 合成 + VmDisplay_Diff + tiles + respack
                                                              │
                                                    rdp → 浏览器/C 渲染器
```

## 3. 决策

| 编号 | 决策 | 理由 |
|---|---|---|
| **U1** | 窗口内容的唯一契约是 `VmCmdBuffer`（命令数组）。`VmWindow.layout`（返回 `Clay_RenderCommandArray`）与 `VmWindow.clayCtx` 从 WM 核心移除，Clay 布局改由 producer 侧适配 | 现在 `vm_wm.h` include `clay.h`，任何非 Clay 宿主都必须链接 Clay 头；M3 已把 `vm_wms` 逼成编译期依赖 `CLAY_DIR` |
| **U2** | Clay 降级为「jp_xaml 的后端之一」+「JPX 控件库的实现」。它仍可被任何进程链接，但 WM/合成进程不链接 | 降级 ≠ 删除。任务栏要画多 stop 渐变与位图切片，Clay 的 `backgroundColor` 只吃单色（`vm_shell.c:13` 已记录），因此某些 producer 本来就不该走 Clay |
| **U3** | `jp_xaml` 增加绘制后端接口（vtable）。`Jpx_*` 回放只是其中一个实现 | 否则「任务栏用 XAML 描述」无法实现；也给将来 HTML+CSS / GTK 适配留出唯一落点 |
| **U4** | 保留模式的代价留在 producer 侧：框架不提供控件树抽象、不做跨进程控件状态 | 把 Clay/XAML/GTK 的差异抬进核心会重演「Clay 地位过高」的问题 |
| **U5** | 跨端协议不变：仍然只有绘制原语与原始输入 | 浏览器不解释界面结构，窗口语义只在 guest |
| **U6** | 应用层不改造旧 `jpx`，从零写 producer（`vm_app`）。**不新增 local/remote 两种语义**；旧 app 标 DEPRECATED、保留代码，新 app 验收后删除 | 给旧代码加“远程模式”会把两套驱动（每帧拉 / 事件推）和两种权威（自有窗口表 / wms 客户端）混在一个 `JpxApp` 里；新写只依赖 `VmCmdBuffer` 与 wms 协议，边界干净 |
| **U7** | 控件与焦点抽成独立库 `vm_ui_core`，不随 app / 后端重复实现 | 焦点表、文本编辑、列表状态、命中规则与布局引擎无关；retain / imm 共用，第三方 app 可直接链接。`jpx_widget.c` 把这三层与 `JpxApp` 单例绑死，只能重写接口、搬运实现 |

## 4. 后端接口（形状冻结，字段可在实现时微调）

```c
typedef struct {
    /* 叶子 */
    void (*rect)(void *b, const char *id, VmRect r, const char *paint, float radius);
    void (*border)(void *b, const char *id, VmRect r, const char *paint, const int w[4]);
    void (*text)(void *b, const char *id, VmRect r, const char *s,
                 int fontId, int size, const char *color);
    void (*image)(void *b, const char *id, VmRect r, const char *imgRef);
    void (*path)(void *b, const char *id, VmRect r, const char *svgPath, const char *paint);
    /* 文本度量：必须由后端提供，不能共用 Clay 的 measure */
    int  (*text_width)(void *b, int fontId, int size, const char *s);
    int  (*line_height)(void *b, int fontId, int size);
    /* 容器 */
    void (*frame_begin)(void *b, const char *tag, const JpxXamlNode *node);
    void (*frame_end)(void *b, const char *tag);
} JpxXamlBackend;
```

两个后端的分工差异是本质的，不试图统一：

| 能力 | Clay 后端 | Direct 后端 |
|---|---|---|
| 布局 | `Jpx_*` 声明 → Clay measure/arrange（flex） | 自己算几何：绝对定位 + `DockPanel` 子集 |
| 叶子绘制 | 由 Clay 生成 render command，`rect/text/image` 多为 no-op | 直接产出 `VmDrawCommand` |
| 文本度量 | `Clay_SetMeasureTextFunction` | `VmFont_Width` / `VmFont_LineHeight` |
| 多 stop 渐变 / 位图切片 | 不支持（Clay bg 单色） | 支持（pattern id / `imgId#slice`） |
| 控件交互（焦点链、文本编辑） | `Jpx_*` 提供 | 不提供，由 producer 自己做命中与状态 |

## 5. 能力矩阵（谁是范本）

| 使用方 | 写法 | 后端 | 说明 |
|---|---|---|---|
| 普通应用（表单类） | 新 `vm_app`：手写命令 / Clay / XAML | 各自 | 旧 `demo_jpx` 标弃用，M7 删除 |
| 任何 app 的控件与焦点 | `vm_ui_core`（焦点/Tab/编辑/列表/命中） | 无 | 跨 retain/imm 共用；不随后端重复 |
| 桌面外壳（任务栏/菜单） | XAML + 模板层 | Direct | **保留模式范本**，不依赖 Clay |
| 三方 HTML+CSS 库 | 自有排版 → 命令 | Direct（或其等价接口） | 未来接入点 |
| GTK 抽出的 UI 库 | 自有 widget → 命令 | Direct | 未来接入点 |
| 极简 / 测试 | 直接写 `VmCmdBuffer` | 无 | `test_wms_app` 已这么干 |

## 6. 边界与禁止项

- 不让浏览器/前端成为命令生成端：DOM 渲染会把窗口层叠、命中、焦点、滚动搬回前端，破坏「语义只在 guest」。
- 不在 WM 层引入 UI 库类型（Clay 或其它）：窗口内容只认 `VmCmdBuffer`。
- 不为「范本」另造第二套元素树：shell 必须复用 `jp_xaml` 的树与绑定，否则成型后还要再改一次。
- 不做通用布局引擎：Direct 后端 v1 只支持绝对定位 + `DockPanel`（Left/Right/Fill）。
- 不把 XAML 标记、元素树、绑定表达式下发到前端。

## 7. 未实现与已知缺口

| 项 | 状态 | 归属 |
|---|---|---|
| WM 去 Clay 依赖（U1） | **未实现**（M5.2 执行） | `task-vm-wms-merge.md` §8 M5 |
| `jp_xaml` 后端接口 + Direct 后端（U3） | **未实现** | `task-shell-xaml-framework.md` P1 |
| 任务栏由 XAML 描述 | **未实现** | `task-shell-xaml-framework.md` P3 |
| 原语降级契约（CSS/GTK 特性 → 支持/降级/不支持三列） | 未写 | 接第一个三方库前必须补齐 |
| 应用私有资源（respack 只由宿主生成） | 已知限制 | 三方库接入的实际门槛 |
| 命令 `id` 跨帧稳定性契约 | 未文档化 | 差分与带宽完全依赖它；需写入 producer 契约 |
| 颜色为 CSS 字符串（非 typed RGBA） | 已知耦合 | P3 级，不阻塞 |
