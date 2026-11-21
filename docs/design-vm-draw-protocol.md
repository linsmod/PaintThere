# 设计：VM 指令化桌面跨端协议

> 本文是冻结在决策时的设计记录，不是现状。落地进度见 `docs/task-vm-instruction-desktop.md`；现状落地后另写 `arch-vm-instruction-desktop.md`。

## 设计边界

跨端协议只有两类内容：**绘制原语**和**原始输入事件**。不暴露任何窗口/桌面 API（`createWindow` / `raise` / `focus` 等禁止）。窗口树、层叠、焦点、命中、事件分发全部在 guest 内部完成。

## 内部模型与序列化分层（冻结）

**内部不用 JSON**。JSON 只是序列化格式之一，未来可对接其它协议（二进制、共享内存等）。

```text
Clay RenderCommandArray
  → VmDisplayList（内部结构体，协议无关）
  → diff（结构体字段比较）
  → serializer：JSON / 二进制 / 其它
  → transport：SSE / WebSocket / 其它
```

- `vm_display.h/.c`：定义 `VmCmdType` / `VmDrawCommand` / `VmDisplayList` 及构造、追加、比较工具。所有命令参数是 typed 字段，不是 JSON 字符串。
- `vm_display_diff.c`：按 id + typed 字段做 retained diff，产出变化命令与脏矩形。
- `vm_serialize_json.c`：唯一负责把 `VmDisplayList` 序列化成 JSON 的地方。新增协议时只加 serializer，不动内部模型。
- `clay_frame_adapter`：职责改为 Clay 命令 → `VmDisplayList`（填充结构体），不再直接生成 JSON。
- `vm_wm`：合成结果是 `VmDisplayList`；首帧全量与后续增量都在结构体层表达。

## 传输 envelope

下行帧（guest → 浏览器）：

```json
{
  "type": "frame",
  "frameId": 42,
  "commands": [
    { "id": "bg", "cmd": "clear", "args": [], "rect": [0, 0, 1024, 768] },
    { "id": "btn1", "cmd": "drawRect", "args": [10, 20, 100, 40, "#336699", 4], "rect": [10, 20, 100, 40] }
  ]
}
```

- `frameId`：单调递增整数，仅用于帧同步，不含窗口语义。
- `commands`：`PaintCommand` 同形 `{id, cmd, args, rect}`，顺序即 z 序（与 Clay `RenderCommandArray` 一致）。
- `resources`（可选，仅首帧或资源变更帧携带）：一次性下发的 `definePattern` / `defineStyle` / `defineDrawProgram` 命令，客户端按 id 缓存，不随普通帧重复。
- `id` 用于 retained diff；`rect` 用于脏矩形，可省略（省略时按整屏处理）。

### 帧率策略：自适应，不固定

桌面不是游戏，不追求恒定帧率：

- **有变化才出帧**：guest 只在窗口状态、指针位置、hover/click 发生变化时递增 `frameId` 并广播；无变化时静默。
- **限流只作上限保护**：输入高峰（如快速 mousemove）可做节流上限（如 60Hz），但不是固定节拍；空闲时零帧。
- **首帧必达**：新客户端连接时 `lastSentFrameId=0`，服务端立即推送当前帧 + `resources`，保证状态完整。
- **retained diff 与脏矩形**：前端按 `id` 比较命令，只重画变化区域；背景层由 draw program 本地重放，不走每帧指令流。

现有 WinForms 控件帧（纯数组）与多客户端指针帧（`type:'cursors'`）保持原样；VM 帧用 `type:'frame'` 区分，三者共存于同一 WebSocket。

## 绘制原语清单

与 `ConsoleApp2/app.js` 的 `commandHandlers` 对齐。`color` 为 CSS 颜色字符串（Clay 的 RGBA 由 adapter 转成 `rgba(r,g,b,a)`），`resolveColor` 不接受数组。

| cmd | args | 语义 |
|---|---|---|
| `clear` | `(x?, y?, w?, h?)` | 无参清整屏；有参清矩形 |
| `drawRect` | `(x, y, w, h, color, cornerRadius?)` | 填充矩形，可选圆角 |
| `drawBorder` | `(x, y, w, h, color, widths[4], cornerRadius?)` | 四边框，`widths = [left, right, top, bottom]` |
| `drawText` | `(x, y, w, h, text, fontId, fontSize, color, letterSpacing?, lineHeight?)` | 文本；由 wasm 中的 stb_truetype 光栅化为 RGBA 位图后绘制，不使用 Canvas `fillText` |
| `drawImg` | `(imgId, x, y, w, h, tint?, cornerRadius?)` | 图片；复用已有 `loadImg` 缓存 |
| `clipPush` | `(x, y, w, h)` | 压栈并裁剪后续绘制 |
| `clipPop` | `()` | 弹栈恢复裁剪 |
| `overlayPush` | `(color)` | 压栈叠加色，期间每个原语绘制后叠一层 |
| `overlayPop` | `()` | 弹栈结束叠加 |

**样式参数约定**：`drawRect` / `drawBorder` / `drawText` 的颜色参数允许传 `styleId` 字符串（如 `"s1"`），表示引用客户端缓存的样式，而非字面颜色。未定义则按字面 CSS 颜色处理。这样每帧只发几何与 id，不重复传颜色/字体。

**资源与绘制程序命令**（一次性下发，客户端缓存，不随每帧重复）：

| cmd | args | 语义 |
|---|---|---|
| `loadImg` | `(imgId, dataUrl)` | 已有，加载图片 |
| `definePattern` | `(patternId, descriptor)` | 声明式图样：`{type:'solid',color}` / `{type:'gradient',from,to,direction}` / `{type:'image',imgId,tiling}` |
| `defineStyle` | `(styleId, {bgPattern?,border?,fontId?,fontSize?,textColor?})` | 样式表，供绘制原语按 id 引用 |
| `defineDrawProgram` | `(programId, commands[], layerId)` | 定义一段可复用绘制指令，绑定到图层 |
| `startDrawProgram` | `(programId, mode)` | `mode`: `once`（默认）/ `loop` |
| `pauseDrawProgram` | `(programId)` | 暂停该程序的自动重绘 |
| `resumeDrawProgram` | `(programId)` | 恢复 |

**图样算法边界**：`definePattern` 只接受声明式 descriptor，不传可执行代码。复杂图案若确需自定义算法，走 wasm 模块通道（与字体方案一致），不嵌入帧指令。

`loadImg` / `definePattern` / `defineStyle` / `defineDrawProgram` 不放在普通帧内重复下发；由 guest 在连接建立或资源变更时发一次，客户端按 id 缓存。

### 字体加载（决策更新：wasm 光栅化）

新增资源命令 `loadFont(fontId, ttfBase64)`：

- `fontId` 为 0..7 的整数，与 guest 侧 `Clay_SetMeasureTextFunction` 使用的字体索引一致。
- `ttfBase64` 为 TTF 文件内容的 base64；前端解码后写入 wasm 线性内存，调用 `loadFont(fontId, ptr, len)`。
- wasm 模块 `stb_font_wasm.js/.wasm` 由 `vm-desktop/stb_font_wasm.c` 编译，直接基于 stb_truetype，导出 `loadFont` / `measureText` / `rasterizeText` / `alloc` / `freeMem`。体积约 32KB。
- `drawText` 执行时调用 `rasterizeText` 得到 RGBA 位图，用离屏 canvas + `putImageData` 生成 ImageBitmap，再按颜色 tint 画到目标 canvas；不使用浏览器字体引擎。
- 已实测：同一份 stb_truetype C 代码在 native（`font_probe`）与 wasm 下对三段文本度量完全一致（91 / 70 / 247），无需 Canvas `measureText` 对比。
- 阶段 2 剩余验证：浏览器像素级渲染一致性（中文/英文/换行）。

## 图层模型

跨端画面分三层，自底向上合成：

| layerId | 名称 | 绘制方 | 更新方式 |
|---|---|---|---|
| `bg` | 背景层 | 客户端本地 | 由 draw program 绑定，静态或循环，不随每帧指令流更新 |
| `windows` | 窗口层 | guest 指令帧 | 每帧 `drawRect` / `drawText` / `drawImg` 等原语 |
| `cursors` | 指针层 | guest 指令帧（帧尾） | 复用 `loadImg` + `drawImg` 画各客户端彩色箭头 |

- `defineDrawProgram(programId, commands[], layerId)` 把绘制程序绑定到具体图层。`startDrawProgram(..., 'loop')` 只在该图层内循环，不干扰窗口层。
- `pauseDrawProgram` / `resumeDrawProgram` 的语义是暂停/恢复某层的自动重绘，不改变其他层。
- 背景层与窗口背景都用 draw program 本地绘制：桌面背景绑 `bg` 层，窗口背景作为窗口层内的静态 program（窗口移动时由 guest 重定位，不重传颜色/图样）。
- 图层顺序固定，跨端不暴露图层创建/销毁 API；layerId 只有上述三个取值。

## Clay 命令映射

| Clay `RenderCommandType` | 跨端 cmd |
|---|---|
| `RECTANGLE` | `drawRect` |
| `BORDER` | `drawBorder` |
| `TEXT` | `drawText` |
| `IMAGE` | `drawImg` |
| `SCISSOR_START` / `SCISSOR_END` | `clipPush` / `clipPop` |
| `OVERLAY_COLOR_START/END` | `overlayPush` / `overlayPop` |
| `CUSTOM` | adapter 决定，不进主协议 |
| `NONE` | 跳过 |

## 原始输入事件

上行事件（浏览器 → guest）：

```json
{
  "type": "input",
  "clientId": "ab12...",
  "color": "#F44336",
  "frameId": 42,
  "kind": "keydown",
  "key": "A",
  "code": "KeyA"
}
```

- `clientId` 复用多客户端指针身份；`color` 为该客户端指针颜色。
- `frameId` 为客户端处理该输入时所见的最新帧号，guest 可据此判断“所见即所操作”；不阻塞输入，只作标记。
- `kind`：`mousedown` / `mousemove` / `mouseup` / `click` / `wheel` / `keydown` / `keyup` / `touch`。
- 鼠标事件带 `x/y`（canvas 内部坐标，guest 逻辑桌面坐标）；键盘事件带 `key`（可读字符或 `Backspace` / `Enter` 等）与 `code`（物理键码）。
- guest 收到后由 WM 做命中、焦点与分发；跨端只传原始事件，不解释语义。焦点仲裁只在 guest：仅焦点客户端的输入能改变窗口状态或键盘焦点，其他客户端只更新指针。
- 键盘派发路由：WM → 焦点窗口 → 窗口注册的 key handler。IME/组合输入由客户端把最终字符以 `key` 字段上报，guest 不实现 IME 状态机。

## SDK 前瞻性

资源 / 样式 / 绘制程序三层抽象是为了不阻塞未来的窗口与控件 SDK：

- 未来控件的视觉属性（背景、边框、字体、图样）统一走 `defineStyle` + `definePattern`，与窗口背景、桌面背景同源，不出现“背景一套机制、控件另一套机制”的分裂。
- 控件的静态外观用 `defineDrawProgram` 缓存到客户端，hover/click 只改少量状态参数，不重发整段外观指令。
- 图层固定三层（bg/windows/cursors），SDK 不跨端创建图层；若未来需要新层，作为协议版本升级处理，不在现有 SDK 里动态申请。
- `drawLoop` 类的命名不采用——静态背景用 `mode:once`，动画用 `mode:loop`，避免“Loop”语义限制控件复用。

## 与现有协议的关系

- 现有 WinForms 帧、welcome/cursors 消息、DOM 事件回传全部保持原样，VM 协议作为新增分支共存。
- 复用 `PaintCommand` 结构、签名去重、脏矩形、rAF 渲染循环、图片缓存与多客户端指针层。
- 本设计不引入任何窗口级指令，WM 状态不跨端传输。
