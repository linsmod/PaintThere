# 设计：VM 指令化桌面跨端协议

> 本文是冻结在决策时的设计记录，不是现状。落地进度见 `docs/task-vm-instruction-desktop.md`；现状落地后另写 `arch-vm-instruction-desktop.md`。

## 设计边界

跨端协议只有两类内容：**绘制原语**和**原始输入事件**。不暴露任何窗口/桌面 API（`createWindow` / `raise` / `focus` 等禁止）。窗口树、层叠、焦点、命中、事件分发全部在 guest 内部完成。

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
- `id` 用于 retained diff；`rect` 用于脏矩形，可省略（省略时按整屏处理）。

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

`loadImg(imgId, dataUrl)` 沿用已有资源命令，不放在帧内。

### 字体加载（决策更新：wasm 光栅化）

新增资源命令 `loadFont(fontId, ttfBase64)`：

- `fontId` 为 0..7 的整数，与 guest 侧 `Clay_SetMeasureTextFunction` 使用的字体索引一致。
- `ttfBase64` 为 TTF 文件内容的 base64；前端解码后写入 wasm 线性内存，调用 `loadFont(fontId, ptr, len)`。
- wasm 模块 `stb_font_wasm.js/.wasm` 由 `vm-desktop/stb_font_wasm.c` 编译，直接基于 stb_truetype，导出 `loadFont` / `measureText` / `rasterizeText` / `alloc` / `freeMem`。体积约 32KB。
- `drawText` 执行时调用 `rasterizeText` 得到 RGBA 位图，用离屏 canvas + `putImageData` 生成 ImageBitmap，再按颜色 tint 画到目标 canvas；不使用浏览器字体引擎。
- 已实测：同一份 stb_truetype C 代码在 native（`font_probe`）与 wasm 下对三段文本度量完全一致（91 / 70 / 247），无需 Canvas `measureText` 对比。
- 阶段 2 剩余验证：浏览器像素级渲染一致性（中文/英文/换行）。

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
  "frameId": 42,
  "kind": "mouse",
  "x": 120,
  "y": 80,
  "button": 0,
  "buttons": 0,
  "key": null,
  "code": null,
  "deltaX": 0,
  "deltaY": 0
}
```

- `clientId` 复用多客户端指针的连接 id。
- `frameId` 为客户端处理该输入时所见的最新帧号，guest 可据此判断“所见即所操作”；不阻塞输入，只作标记。
- `kind`：`mouse` / `key` / `wheel` / `touch`。
- `x/y` 为 canvas 内部坐标（guest 逻辑桌面坐标），浏览器按 canvas 实际尺寸换算后填入。
- guest 收到后由 WM 做命中、焦点与分发；跨端不解释事件语义。

## 与现有协议的关系

- 现有 WinForms 帧、welcome/cursors 消息、DOM 事件回传全部保持原样，VM 协议作为新增分支共存。
- 复用 `PaintCommand` 结构、签名去重、脏矩形、rAF 渲染循环、图片缓存与多客户端指针层。
- 本设计不引入任何窗口级指令，WM 状态不跨端传输。
