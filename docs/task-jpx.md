# 任务：JPX 应用框架与桌面外壳

> 自包含执行文档（2026-11-22 立项）。可在会话中断或上下文丢失后据此恢复。
> 设计决策已冻结；进度见文末「进度」章节，每完成一步更新勾选。

## 1. 目标与验收定义

JPX（Just Paint Things）是开发 PT 协议 GUI 程序的库。目标可以逐条验证：

- 应用源码只 `#include "jpx.h"`，不出现 `clay.h` / `vm_display.h` / `vm_serialize_json.h` / `vm_tiles.h` / `vm_server.h` / JSON / tiles / frameId / dirty 等任何 PT 词汇。
- 一个「表单程序」（输入框 ×3、勾选框 ×2、按钮 ×2、滚动列表 ×1）能用普通 GUI 逻辑写完：声明控件、读返回值、改状态、`Jpx_Invalidate`。
- 桌面具备基本能力：窗口装饰与按钮、层级、焦点、拖动、任务栏、剪贴板、多客户端指针。
- 跨端协议零改动：仍只有绘制原语 + 原始输入 + tiles 管理三类内容。

## 2. 边界（沿用既定冻结决策）

来源：`docs/design-vm-draw-protocol.md`（协议冻结）、`docs/task-vm-instruction-desktop.md`（WM 边界）、`docs/task-remote-tiles.md`（tiles 模型）。

- 协议不暴露任何窗口/桌面 API，不传像素，不传窗口图层。
- 窗口树、z 序、焦点、命中、事件分发、脏区、合成全部在 guest。
- 多客户端共享同一画面；彩色指针是绘制层叠加，不是 WM API。
- JPX 不自研布局引擎，站在 Clay 上。Clay 的短板（窗口树、焦点链、键盘、文本输入、IME）由 JPX 补。

## 3. 分层与归属

```text
应用 app            布局回调 + 状态 + 回调，只 include jpx.h
  ───────────────────────────────────────────────────────
JPX                 应用宿主：窗口句柄、控件、控件焦点链、文本编辑、主题应用
  ───────────────────────────────────────────────────────
桌面外壳 vm_shell   【新增】任务栏（特殊窗口）+ 窗口列表
  ───────────────────────────────────────────────────────
窗口管理 vm_wm      窗口树、z、焦点、拖动、装饰、命中、指针表、合成
  ───────────────────────────────────────────────────────
共享基础            【新增】vm_font（度量）、vm_theme（样式 token）、vm_clipboard
  ───────────────────────────────────────────────────────
交付层              vm_display / vm_serialize_json / vm_tiles / vm_server
```

| 组件 | 知道什么 | 绝不能知道 |
|---|---|---|
| app | 自己的控件与状态 | 窗口全局坐标、z、tiles、协议 |
| JPX | 窗口内局部坐标、控件焦点 | 窗口全局位置与 z、其它窗口、协议 |
| vm_shell | 窗口列表、标题、状态 | 应用内部状态、协议 |
| vm_wm | 窗口矩形/z/焦点/装饰/指针 | 控件、应用状态、协议 |
| 共享基础 | 字体度量、样式 id、剪贴板文本 | 任何窗口或控件 |
| 交付层 | 命令列表与 JSON | 窗口与控件的存在 |

### 3.1 WM 与 JPX 的三条硬规则

1. **坐标系单向**：JPX 只在窗口局部坐标布局；WM 合成时加 `(x+2, y+titleH+2)` 偏移（现状见 `vm-desktop/vm_wm.c` 的 `VmWm_BuildFrame`）。JPX 不得获取窗口全局坐标。
2. **命中分层**：WM 只做窗口级命中（标题栏、边框、缩放边），内容区整块把局部坐标推进该窗口 Clay 上下文（现状见 `sync_pointer_state`）；控件级命中、hover、点击判定全在 JPX。
3. **脏标记汇合、单点出帧**：WM 装饰变化与 JPX 内容变化都只置全局 dirty；帧由 WM 单点合成、单点切块。JPX 不碰 `vm_display` / `vm_tiles` / `vm_server`。

配套责任边界：

- **窗口生杀由 JPX 发起、WM 执行**：JPX 调 `VmWm_AddWindow/RemoveWindow`；装饰上的 × 走「关闭请求 → 应用回调 → 应用决定 → Remove」。
- **WM 不解释应用语义**：WM 不认识按钮/输入框，只认识矩形和指针。

### 3.2 决策记录

| 决策 | 结论 | 理由 |
|---|---|---|
| 任务栏形态 | 无装饰、置顶、固定位置、不抢焦点的**特殊窗口** | 完全复用现有 z/命中/Clay 指针/键盘路径；WM 只需新增三个窗口标志与一个枚举接口。不做 panel 区/struts/工作区，因此暂不支持最大化 |
| 剪贴板 | 本轮做（guest 内单例） | 纯 guest 模块，三个函数；不做则文本输入「能打字不能复制」第一时间被用户踩到 |
| 应用模型 | 单进程 = 一组窗口，一个 `JpxApp` | 多进程共享桌面需要 IPC，登记为限制 |
| 输入法 | 不实现 IME 状态机 | 既定决策：客户端把最终字符作为 `key` 上报 |
| 任意绘制逃生口 | v1 不提供 | 避免把 Clay/vm_display 类型泄进公开头；后续用「每窗口附加命令列表 + `Jpx_Paint*` 包装」实现，见 §10 |

## 4. 新增与修改的组件

### 4.1 `vm_font`（共享文本度量，最高优先）

前置条件：guest 侧度量必须与前端 stb_truetype 逐像素同源。现状 `demo_wm.c` 用 `length × fontSize × 0.6` 估算，与前端光栅宽度不一致，导致文本盒与贴图错位。

```c
/* vm_font.h */
int VmFont_Load(const char *ttfPath, int fontId);                 /* 0 成功；支持 fontId 0/1 */
int VmFont_Measure(int fontId, int px, const char *s, int len);   /* px 宽，与 wasm 完全一致 */
int VmFont_Ascent(int fontId, int px);                            /* 垂直居中与基线 */
int VmFont_LineHeight(int fontId, int px);
int VmFont_Wrap(int fontId, int px, const char *s, int len, int maxW,
                int *lineBreaks, int cap);                        /* 返回行数；供 JPX 换行/省略 */
```

实现约束：

- `STB_TRUETYPE_IMPLEMENTATION` 只在 `vm_font.c` 定义一次；`stb_font_wasm.c` 只参与 emcc 构建，两者不进同一二进制。
- 必须复刻 wasm 的取整方式（逐 glyph 截断 `x += (int)(advance * scale)`、kern 同样截断），否则会出现 1px 漂移，验收按逐字节相等判定。
- 字体来源：`assets/font.ttf`（445KB），由 `JpxOptions.fontPath` 指定，不从网络取。

### 4.2 `vm_theme`（共享样式 token）

WM 装饰与 JPX 控件必须同源，消除「窗口背景必须叫 `s_window`、标题写死 fontId 0/14px」的硬编码。

```c
/* vm_theme.h */
typedef struct {
    int titleHeight;              /* 默认 28 */
    int borderWidth;              /* 默认 2 */
    const char *windowStyleId;    /* 默认 "s_window" */
    const char *titleFocusColor, *titleIdleColor;
    const char *borderFocusColor, *borderIdleColor;
    int titleFontId, titleFontSize;
} VmChrome;

typedef struct {
    VmChrome chrome;
    const char *desktopFrom, *desktopTo;        /* 桌面背景渐变 */
    const char *panelStyleId;
    /* 控件 token：bg/fg/border/accent/inputBg/disabled/selected、radius/padding/gap、fontId/fontSize */
    const char *bgColor, *fgColor, *borderColor, *accentColor, *accentTextColor;
    const char *inputBgColor, *disabledColor, *selectedColor;
    int radius, padding, gap, fontId, fontSize;
} VmTheme;

const VmTheme *VmTheme_Default(void);
void VmTheme_AppendResources(VmDisplayList *out, const VmTheme *t);  /* 唯一 resources 生成者 */
```

### 4.3 `vm_shell`（桌面外壳）

```c
/* vm_shell.h */
int  VmShell_Init(VmWm *wm, const VmTheme *t);   /* 建任务栏特殊窗口，注册布局 */
void VmShell_Refresh(VmWm *wm);                  /* 窗口打开/关闭/标题变化时调用 */
```

任务栏内容：每个窗口一个条目（标题 + 焦点/最小化状态色），点击条目 raise + focus，点最小化的条目恢复。任务栏自身不参与焦点。

### 4.4 `vm_clipboard`（guest 内单例）

```c
/* vm_clipboard.h */
void VmClipboard_SetText(const char *utf8);
int  VmClipboard_GetText(char *out, int cap);   /* 返回写入长度 */
```

### 4.5 WM 增补

```c
/* vm_wm.h 新增 */
enum { VM_WIN_UNDECORATED = 1, VM_WIN_TOPMOST = 2, VM_WIN_PINNED = 4, VM_WIN_NOFOCUS = 8 };

typedef void (*VmWindowCloseHandler)(int windowId, void *userData);

void VmWm_SetChrome(VmWm *wm, const VmTheme *theme);
void VmWm_SetWindowFlags(VmWm *wm, int id, unsigned flags);
void VmWm_SetCloseHandler(VmWm *wm, int id, VmWindowCloseHandler handler, void *userData);
int  VmWm_Enumerate(VmWm *wm, int *ids, int cap);          /* 返回数量，按 z 升序 */
void VmWm_Minimize(VmWm *wm, int id);
void VmWm_Restore(VmWm *wm, int id);
void VmWm_SetContentClip(VmWm *wm, int enabled);           /* 默认开 */
void VmWm_Invalidate(VmWm *wm);                            /* 与 Jpx_Invalidate 同一 dirty */
```

行为要求：

- 内容裁剪：`VmWm_BuildFrame` 在每个窗口内容前后各发一对 `clipPush/clipPop`（body 矩形，全局坐标）。
- 装饰按钮：`×` 在标题栏右端命中后调用 closeHandler；`—` 调用 `VmWm_Minimize`。后者不直接生效，由应用决定。
- 标志：`UNDECORATED` 不发标题/边框、不参与拖动；`TOPMOST` 的 z 恒高于普通窗口；`PINNED` 不响应拖动；`NOFOCUS` 不抢焦点。
- 句柄安全：`VmWm_AddWindow` 继续返回指针但**不得跨帧持有**；JPX 一律用 `int id`，每次用 `VmWm_FindWindow` 取。`RemoveWindow` 后所有缓存指针失效。

### 4.6 `vm_server` 增补

```c
typedef void (*VmTickHandler)(void *userData);
void VmServer_SetTickHandler(VmServer *server, int intervalMs, VmTickHandler handler, void *userData);
```

`VmServer_Run` 已是 20ms 的 `select` 轮询，在每轮 `vm_poll_frames` 之前按 intervalMs 调用 tick。用于动画、光标闪烁、指针 3 秒超时。注意：tick 本身不置 dirty，动画场景由应用回调里调 `Jpx_Invalidate`。

### 4.7 前端输入补齐（`assets/vm_renderer.js`）

- 键盘白名单：可打印单字符、`Backspace`、`Enter`、`Tab`、`Escape`、`Delete`、`ArrowUp/Down/Left/Right`、`Home`、`End`、`PageUp`、`PageDown`；同时上报 `ctrlKey/altKey/shiftKey/metaKey`。
- 新增 `wheel`：`{kind:"wheel", x, y, deltaY, clientId, color}`，`preventDefault` 防止页面滚动。
- 所有 input 事件带 `frameId`（客户端记录最近一次收到的帧号），设计文档已定义、当前实现遗漏。
- 不改 `vm_wm` 对未知 kind 的容错：wheel 只透传给焦点窗口。

## 5. `jpx.h` 公开接口

```c
#ifndef JPX_H
#define JPX_H
#include <stddef.h>

typedef struct JpxApp JpxApp;

typedef struct {
    int width, height;        /* 逻辑桌面，如 800×600 */
    int tileW, tileH;         /* 远端瓦片尺寸，如 100×100；网格由 JPX 推导 */
    const char *fontPath;     /* 与前端同一份 TTF */
    const char *webRoot;
    unsigned short port;
    int logLevel;             /* 0 静默 / 1 告警 / 2 信息 */
} JpxOptions;

typedef struct {
    const char *bgColor, *fgColor, *borderColor, *accentColor, *accentTextColor;
    const char *inputBgColor, *disabledColor;
    int fontId, fontSize, radius, padding, gap;
} JpxTheme;

/* ---- 窗口 ---- */
typedef void (*JpxLayoutFn)(int winId, void *user);
typedef void (*JpxCloseFn)(int winId, void *user);
typedef int  (*JpxKeyFn)(int winId, const char *key, const char *code,
                         int isDown, int ctrl, int shift, int alt, void *user);

typedef struct {
    const char *title;
    int x, y, w, h;
    JpxLayoutFn layout;
    void *user;
    JpxCloseFn onClose;   /* 被点 × 时调用；应用自行调 Jpx_WindowClose 才真正关闭 */
    JpxKeyFn onKey;       /* 控件未消费的键；返回 1 表示已处理 */
} JpxWindowDesc;

/* ---- 布局 ---- */
void Jpx_RowBegin(int win);
void Jpx_RowEnd(int win);
void Jpx_ColumnBegin(int win);
void Jpx_ColumnEnd(int win);
void Jpx_PanelBegin(int win);
void Jpx_PanelEnd(int win);
void Jpx_ScrollBegin(int win, const char *id, int height);
void Jpx_ScrollEnd(int win);
void Jpx_Spacer(int win, int w, int h);

/* ---- 控件（返回值 = 本帧是否发生该交互） ---- */
typedef struct { int wrap; int align; } JpxTextStyle;   /* align: 0 左 / 1 中 / 2 右 */
void Jpx_Label(int win, const char *text);
void Jpx_LabelEx(int win, const char *text, const JpxTextStyle *style);
int  Jpx_Button(int win, const char *id, const char *text);
int  Jpx_Checkbox(int win, const char *id, const char *label, int *checked);
int  Jpx_TextInput(int win, const char *id, char *buf, size_t cap, size_t *len);
int  Jpx_ListBox(int win, const char *id, const char *const *items, int count,
                 int *selected, int visibleRows);
void Jpx_Separator(int win);

/* ---- 运行时 ---- */
JpxApp *Jpx_Create(const JpxOptions *opt);
void    Jpx_Destroy(JpxApp *app);
int     Jpx_Run(JpxApp *app);                  /* 阻塞；0 正常退出 */
void    Jpx_Stop(JpxApp *app);
void    Jpx_SetTheme(JpxApp *app, const JpxTheme *theme);

int  Jpx_WindowOpen(JpxApp *app, const JpxWindowDesc *desc);   /* 返回 winId > 0 */
void Jpx_WindowClose(JpxApp *app, int winId);
void Jpx_WindowSetTitle(JpxApp *app, int winId, const char *title);
void Jpx_Invalidate(JpxApp *app);

int  Jpx_TimerEvery(JpxApp *app, int ms, void (*fn)(void *), void *user);
void Jpx_TimerCancel(JpxApp *app, int timerId);

void Jpx_ClipboardSet(const char *utf8);
int  Jpx_ClipboardGet(char *out, int cap);

void Jpx_Log(JpxApp *app, int level, const char *fmt, ...);
#endif
```

### 5.1 控件规范

- **id 必填且稳定**：`Jpx_Button(win, "ok", ...)` 内部拼成 `w<winId>/ok` 作为 Clay 元素 id。不得用递增序号，否则增删控件会让 retained diff 整屏重画。
- **返回值语义**：返回 1 表示本帧完成一次交互（按钮按下、勾选翻转、列表选择变化）。应用据此改状态；JPX 自动 invalidate。
- **视觉状态**：normal / hover / pressed / focused / disabled 五态，用颜色与边框区分，全部由 theme token 推导，应用不传颜色。禁用态由 `Jpx_WindowDisable`？——v1 不提供，控件不接收输入即视为可用。
- **焦点链**：窗口内 Tab / Shift+Tab 顺序为控件声明顺序；点击可聚焦控件同时把键盘焦点给该窗口。
- **文本编辑**：caret 用 1px `drawRect`，闪烁周期 500ms 由 timer 驱动；选区用半透明 `drawRect`，文本画在其上；Home/End/Delete/Backspace/方向键/Shift 选区/Ctrl+C/V/X 全部由 JPX 处理。
- **文本绘制**：多行与省略号必须用 `VmFont_Wrap` 自己算，然后拆成多条单行 `drawText`（客户端 `drawText` 不换行、不裁剪、忽略 `lineHeight`）。每行位置由 `VmFont_Ascent` 计算。
- **滚动**：`Jpx_ScrollBegin/End` 走 Clay 的裁剪容器；滚轮增量来自 WM 透传的 `wheel`，只作用于指针所在窗口的滚动区。

### 5.2 帧与脏标记

- `Jpx_Invalidate` 与 `VmWm_Invalidate` 置同一个 dirty 位；帧 provider 在 dirty 为假时返回 NULL（沿用现状自适应帧率）。
- 输入到达 → WM 更新状态 → JPX 处理控件交互 → 任意一方置 dirty → 服务端在本轮轮询出帧。`VmServer_Run` 已在每条输入后立即 poll（现状 `vm_server.c` 注释说明 Clay `PRESSED_THIS_FRAME` 需要分帧）。
- 指针移动一律置 dirty：hover 变化无法在 WM 层精确判定；代价由 tile 差异吸收（实测一次移动 1–2 块）。

## 6. 目录与构建

```text
vm-desktop/
  vm_font.c/.h           新增：度量
  vm_theme.c/.h          新增：样式 token 与 resources
  vm_clipboard.c/.h      新增：剪贴板
  vm_shell.c/.h          新增：任务栏
  vm_wm.c/.h             修改：装饰参数化、裁剪、标志、枚举、关闭请求、dirty
  vm_server.c/.h         修改：tick
  test_frame.c           新增：帧断言测试（不开服务、不开浏览器）
  demo_jpx.c             新增：等价双窗口示例
  jpx/jpx.h              公开头（唯一入口）
  jpx/jpx_app.c          宿主：create/run/provider 接线/tiles 生命周期/timer
  jpx/jpx_window.c       窗口封装：id 句柄、装饰回调、标题
  jpx/jpx_theme.c        默认主题
  jpx/jpx_widget.c       控件
  jpx/jpx_text_edit.c    文本编辑模型（caret/选区/剪贴板）
  jpx/jpx_layout.c       Row/Column/Panel/Scroll/Spacer 封装
assets/vm_renderer.js    修改：键盘白名单、wheel、frameId
```

CMake 新增目标 `demo_jpx`、`test_frame`；`demo_wm` 保留为最小样例不动。`vm_font.c` 编译需要 `-I<H:/github_repos/mystbfont_c>`。

## 7. 分阶段实施与验收

### 阶段 A：共享基础 + 前置修复

改动：`vm_font`、`vm_theme`、`vm_wm`（裁剪/Chrome/dirty）、`vm_server`（tick）、`vm_renderer.js`（输入）、新增 `test_frame`。

验收断言：

- `test_frame` 里 `VmFont_Measure` 与 wasm `measureText` 对 10 段文本（中英混排、空格、标点、长英文）宽度**逐字节相等**。
- 全量帧中标题文本盒宽度等于 `VmFont_Measure(fontId, 14, "Window A")`。
- 构造一个宽度超出窗口的子控件，窗口外采样像素不被污染（内容裁剪生效）。
- `curl` 发送 `{"kind":"keydown","key":"Tab"}` 到焦点窗口，服务端打印 `changed=1`。
- 无输入时 tick 能驱动出帧（临时 demo 用 timer 改变标题颜色，观察 frameId 递增）。
- `python tools/build_vm.py` 一次通过。

### 阶段 B：WM 接口补齐 + 桌面外壳

改动：`vm_wm` 标志/枚举/最小化/关闭请求/装饰按钮、`vm_shell`、`vm_clipboard`。

验收断言：

- 任务栏显示两个窗口条目；点击条目后对应窗口 z 升到最高（帧中命令顺序变化）。
- 点 `×` 后应用 `onClose` 被调用；应用不调 `Jpx_WindowClose` 则窗口仍在。
- 最小化后该窗口内容不再出现在帧中，任务栏条目仍在且可恢复。
- 任务栏自身点击不改变窗口焦点。

### 阶段 C：JPX 宿主

改动：`jpx/jpx.h`、`jpx_app.c`、`jpx_window.c`、`jpx_theme.c`、`demo_jpx.c`；`demo_wm.c` 不动。

验收断言：

- `demo_jpx` 的 tiles 数量与 `demo_wm` 相同（800×600 / 100×100 = 48）。
- 按钮 hover 变橙、单击计数精确 +1；焦点切换标题蓝灰互换；标题栏拖动位移与鼠标一致。
- 双客户端彩色指针互见；非焦点客户端输入不改变窗口状态。
- 空闲 2 秒 frameId 不变；一次输入 +1。
- 应用源码只 include `jpx.h`，`grep -c "clay\|vm_display\|vm_tiles\|json"` 为 0。

### 阶段 D：控件、文本编辑、剪贴板接入

改动：`jpx_widget.c`、`jpx_text_edit.c`、`jpx_layout.c`、`jpx_clipboard` 接入。

验收断言（form demo）：

- 三个输入框 Tab/Shift+Tab 循环焦点，点击可直接聚焦。
- 中英文输入、Backspace/Delete/Home/End/方向键/Shift 选区行为正确，caret 每 500ms 闪烁。
- 在一个输入框 Ctrl+C、另一个 Ctrl+V 粘贴成功；Ctrl+X 剪切后源框清空。
- 勾选框点击翻转；滚动列表滚轮生效且只影响指针所在滚动区。
- 反复开关窗口 100 次，进程 RSS 无明显增长（无泄漏）。

### 阶段 E：首个真实程序 + guest 部署

改动：真实应用代码、`tools/cross_vm.py`（riscv64-linux-musl 交叉编译）、guest 启动脚本。

验收断言：

- guest 内启动后浏览器端完整渲染并交互闭环，console 无错误。
- 空闲零帧；输入后仅变化 tile 下发。
- 记录 guest 进程 RSS 与帧耗时。

预计：A 1 天，B 1–2 天，C 1–2 天，D 3–5 天，E 1–2 天。

## 8. 构建与验证命令（当前 Windows host）

```powershell
cd h:\github_repos\跨端图绘指令流
python tools/build_vm.py
# 产物：vm-desktop/build/demo_wm.exe、demo_jpx.exe、test_frame.exe、vm_renderer.exe

# 帧断言（不开服务）
.\vm-desktop\build\test_frame.exe

# 服务端验证
.\vm-desktop\build\demo_jpx.exe 9000 ..\assets
curl -s --max-time 2 -N http://127.0.0.1:9000/events
curl -s -X POST -H "Content-Type: application/json" -d '{"kind":"mousemove","x":300,"y":250,"color":"#F44336","clientId":"A"}' http://127.0.0.1:9000/input

# 浏览器验证
agent-browser --session jpx open http://127.0.0.1:9000/
agent-browser --session jpx wait 1500
agent-browser --session jpx console
agent-browser --session jpx screenshot jpx_check.png
agent-browser --session jpx close
```

环境：Clay 源码 `H:\github_repos\clay`；stb_truetype `H:\github_repos\mystbfont_c`；MSYS2 gcc `C:\msys64\mingw64\bin\gcc.exe`；端口默认 9000（避开用户可能占用的 8080/8081）。

## 9. 恢复点

- 协议：`docs/design-vm-draw-protocol.md`（原语、tiles、输入 schema）。
- WM 现状：`vm-desktop/vm_wm.c/.h`（窗口数组、z 序、焦点、拖动、指针表、合成）。
- 样板来源：`vm-desktop/demo_wm.c`（阶段 C 要整体吸收的 200 行）。
- 交付链：`vm-desktop/vm_tiles.c`（切块）、`vm_serialize_json.c`（唯一 JSON 出口）、`vm_server.c`（SSE/输入/tick 挂点）。
- 前端原语：`assets/app.js`（`drawText` 不换行不裁剪、`resolvePaint` 支持 styleId）；`assets/vm_renderer.js`（tiles 与输入上报）。
- 度量对照：`vm-desktop/stb_font_wasm.c`（wasm 侧实现，必须复刻其取整方式）、`vm-desktop/font_probe.c`（native 对照）。

## 10. 已知限制与后续触发条件

- 单进程模型：一个 `JpxApp` 一组窗口；多进程共享桌面需要 IPC，不做。
- 无最大化：不做 panel 区/struts/工作区；将来要做最大化时先加 `VmWm_SetWorkArea`。
- 无窗口吸附、虚拟桌面、窗口动画、拖放、通知中心、系统托盘、多显示器、Per-Monitor DPI。
- 无 IME 组合输入：客户端只上报最终字符。
- 无任意绘制逃生口：将来若确需，方案为「WM 合成时在窗口内容之后追加应用提供的命令列表」，由 JPX 以 `Jpx_PaintRect/Text/Image/ClipPush/OverlayPush` 包装，`vm_display.h` 仍不对应用公开。
- 命令预算 512 条、单条文本 256 字节：JPX 在超限时打告警并降级（优先丢弃不可见窗口内容），不静默截断。
- `drawText` 的 `letterSpacing` / `lineHeight` 当前客户端未实现：JPX 的换行与行距全部在 guest 侧算好后拆成多条单行文本。

## 11. 相关缺陷

见 `issues/INDEX.md` 中标记本轮带入的条目：文本度量失真、键盘集合过窄、窗口内容不裁剪、窗口句柄悬空、overlay 颜色环形缓冲串色、Clay 元素 id 跨窗口撞号。阶段 A/C 修掉的条目在结案时归档。

## 12. 进度

- [x] 阶段 A：共享基础 + 前置修复（2026-11-22 完成）
- [x] 阶段 B：WM 接口补齐 + 桌面外壳（2026-11-22 完成）
- [ ] 阶段 C：JPX 宿主
- [ ] 阶段 D：控件、文本编辑、剪贴板接入
- [ ] 阶段 E：首个真实程序 + guest 部署

### 阶段 A 执行记录

新增：`vm_font.c/.h`（度量 + Wrap）、`vm_theme.c/.h`（`VmChrome` + token + resources）、`test_frame.c`（帧断言）。
修改：`vm_wm.c/.h`（装饰参数化、内容裁剪、`dirty`/`Invalidate`/`ConsumeDirty`、wheel 处理器、`VmWm_SetChrome`/`SetContentClip`）、`vm_server.c/.h`（`VmServer_SetTickHandler`）、`assets/vm_renderer.js`（键白名单 + 修饰键 + wheel + `frameId`）、`CMakeLists.txt`（`MSTB_DIR`、`test_frame` 目标）。

验证证据：

- `test_frame.exe ..\assets\font.ttf` → 51 项断言全通过；`"Window A"@14 → w=48 h=14 baseline=10 lineStep=14`；`"跨端图绘指令流"@16 → w=49`；Wrap 行首单调递增。
- `node test_wasm_font.mjs ../assets/font.ttf 14 "Window A"` → `outW=48 outH=14`，与 native 完全一致（同一次输出里 `measureText=45`，确认了 measure/raster 口径差，见 issues）。
- 9000 实例 SSE：49 个 `setTile`，`clipPush=98` / `clipPop=98`，标题文本 `[70,68,340,14]`（y 由 `VmFont_LineHeight` 居中得出）。
- `curl POST /input`：`mousedown` `changed=1`；`keydown Tab` `changed=1`；`wheel` `changed=0`（demo 未注册 wheel handler）但使 frameId 递增到 7。
- `python tools/build_vm.py` 通过（需先停掉占用 `demo_wm.exe` 的旧实例，否则链接报 Permission denied）。

未验证：浏览器内按键与滚轮的实际交互路径（改用人工浏览器检查，未做自动化）。

恢复点：阶段 B 起，从 `vm_wm.c` 的窗口标志（undecorated/topmost/pinned/noFocus）、`VmWm_Enumerate`、最小化与关闭请求、装饰按钮命中开始；然后是 `vm_shell.c`（任务栏特殊窗口）与 `vm_clipboard.c`。

### 阶段 B 执行记录

新增：`vm_shell.c/.h`（任务栏特殊窗口，Clay 布局列出窗口条目，点击切换焦点/恢复）、`vm_clipboard.c/.h`（guest 单例，4KB UTF-8 缓冲）。
修改：`vm_wm.c/.h`（`VM_WIN_*` 标志、`VmWm_SetWindowFlags/WindowFlags/SetCloseHandler/Enumerate/Minimize/Restore/IsMinimized/FocusId/WindowBody`、标题栏 ×/— 按钮命中与绘制、顶层窗口两遍合成、press 改投给指针命中的窗口、`bodyStyleId`）、`vm_theme.c/.h`（`panelColor` + `p_panel`/`s_panel`）、`demo_wm.c`（资源改用 `VmTheme_AppendResources`、度量改用 `VmFont_Measure`、加载 `font.ttf`、建任务栏、两个窗口挂关闭回调）、`test_frame.c`（+38 项断言）。

验证证据：

- `test_frame.exe` → **89 项断言 0 失败**。新增覆盖：× 触发 closeHandler 且不自动关窗、— 最小化并清焦点、Restore 重新聚焦、NOFOCUS 抢不到焦点、TOPMOST 合成在普通窗口之后、UNDECORATED 无标题/边框且 body 等于整窗、Enumerate 按 z 升序、任务栏窗口标志/位置/body 宽度、任务栏用 `s_panel` 且合成在普通窗口之后、点条目可恢复并聚焦、剪贴板 round-trip/截断/清空。
- 9000 实例 SSE：`w32512_body` = `[0,64,800,36,"s_panel"]`；任务栏条目文本 `[56,76.5,40,11,"Window A",0,12,...]`（tile 局部坐标，对应全局 x≈156、y≈576.5，即第 1 个条目内边距 8 处）。
- 端到端：点 B 的 × 后 `w2_body` 与 `"Window B"` 计数归 0；点 A 的 — 后 `w1_body` 归 0 而任务栏条目仍在；点任务栏条目后 `w1_body` 恢复。

期间修掉的偏差：`VmWm_WindowBody` 统一后，窗口正文原点从 `(x+bw, y+th+bw)` 改为 `(x+bw, y+th)`，与 body 矩形一致（旧代码正文比 body 低 2px）。test_frame 的 wheel 局部坐标期望值随之由 10 改为 12。

已知行为：Clay 的 `PRESSED_THIS_FRAME` 需先有一帧 hover。浏览器总会先发 mousemove，所以真实交互无问题；用 `curl` 合成点击验证时必须先 POST 一次 `mousemove`，否则任务栏条目不会响应。

恢复点：阶段 C 从 `jpx/jpx.h` 与 `jpx_app.c` 开始（吸收 demo_wm 的样板：Clay 上下文、tile 网格、四个 provider、dirty、frameId），用 `demo_jpx.c` 与 `demo_wm` 做等价对照。
