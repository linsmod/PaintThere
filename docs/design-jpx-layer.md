# 窗口图层化 · 设计记录

> **这不是现状。** 本文冻结于 2026-11-22 的决策讨论，只写「要改成什么」以及为什么。当前实现见 `arch-vmdesk.md` 与 `vm-desktop/vm_wm.c`。

## 1. 要解决的问题

`VmWm_BuildFrame` 按 z 序把每个窗口的装饰与内容**直接 append 进同一条** `VmDisplayList`：

```672:681:vm-desktop/vm_wm.c
    for (int pass = 0; pass < 2; pass++) {
        int wantTop = pass;
        for (int z = 1; z < wm->nextZ; z++) {
            for (int i = 0; i < wm->count; i++) {
                VmWindow *win = &wm->windows[i];
                if (win->z != z || win->minimized) continue;
                int top = (win->flags & VM_WIN_TOPMOST) ? 1 : 0;
                if (top != wantTop) continue;
                build_one_window(wm, out, win, ch, fontResolver);
```

结果是窗口只有「矩形 + z + 裁剪」，没有「内容单元」：

- 任一窗口变化 → 所有窗口重新 layout、所有 Clay 命令重生成
- 窗口之间没有可独立失效、独立交付的边界
- 没有「窗口属于哪个应用」的归属，多进程演进没有可依托的接口

目标：让窗口真正成为图层——**自己的内容、自己的矩形、自己的失效粒度**。

## 2. 边界：图层不越出 guest

这是本设计最硬的约束，由「不向远端泄露窗口结构」决定：

| 段 | 图层概念可见性 | 理由 |
|---|---|---|
| 应用进程 → WM 进程（将来） | **可见** | 本机 IPC，双方都在 guest 侧 |
| WM 内部 | **可见** | 本设计的作用域 |
| WM → 浏览器 | **不可见** | 线上只有扁平指令流 + tile 差分 |

由此得出的禁止项：

- 不向线上协议引入「图层偏移」「图层 id」「按图层的增量更新」
- 不让前端按图层缓存内容
- 不改 `VmDisplay_Diff`、不改 tile 差分语义、不改帧 JSON 结构

一句话：**图层是 guest 内部的重构，对外协议零变化。**

## 3. 数据结构

```c
/* 每个窗口一份；窗口销毁时释放。 */
typedef struct {
    VmDisplayList list;     /* 装饰 + 内容，坐标已是画布坐标 */
    int contentDirty;       /* 需要重新 layout + 重生成命令 */
    int chromeDirty;        /* 仅装饰变化（标题/边框/按钮着色） */
    uint32_t builtFrame;    /* 上次生成时的帧号，便于诊断复用 */
} VmWindowLayer;
```

`VmWindow` 增加 `VmWindowLayer *layer;`（指针而非内嵌，与 `clayCtx` 一致，避免 `VmWindow` 数组膨胀）。

### 内存代价与池化（实测，2026-11-22）

```
                     改造前            改造后（S0）
sizeof(VmDrawCommand)   992 B    →      496 B
sizeof(VmDisplayList)   496 KB    →     263 KB   (269,584 B)
sizeof(VmDisplayDelta)  560 KB    →   待 P2 处理
```

撑大结构的是**资源类 args**，不是绘制类：

| union 成员 | 大小 | 构成 |
|---|---|---|
| `VmDefinePatternArgs` | 896 B | 7 个 `char[128]` + enum |
| `VmDefineStyleArgs` | 768 B | 6 个 `char[128]` |
| `VmLoadImgArgs` | 640 B | `imgId[128] + dataUrl[512]` |
| `VmDrawTextArgs` | 408 B | `text[256] + color[128]` |

实测一帧（`demo_jpx`，两窗口 + 任务栏）资源命令只占约 4%：`definePattern` 3、`defineStyle` 3、`loadImg` 1、`defineProgram` 1。**这 8 条命令让另外 200 多条每条都背 992 字节**——这就是固定开销的来源。

### 三层池化

| 层 | 做法 | 命令大小 | 每帧 list |
|---|---|---|---|
| **P1 资源外置**（已完成） | `loadImg`/`definePattern`/`defineStyle`/`defineProgram`/`programControl` 移出 `VmDrawCommand`，进入独立的 `VmResourceCommand[16]` | 992 → **496** | 496 → **263 KB** |
| **P2 list 池化** | `VmDisplayList` 改为「池中视图」（指针 + 配额 + count），全进程一份命令池，每窗口固定配额 | 496 | 全进程约 250 KB，不再是每窗口一份 |
| **P3 文本驻留** | `drawText.text`/`color` 移到字符串池，命令留 offset | 496 → ~104 | 248 → 53 KB |

三层的取舍：

- **P1 已完成（S0）**。`programControl` 也一并外置：START/PAUSE/RESUME 与定义类命令在资源通道里是连续顺序，分开会让输出顺序变化。
- **P2 与 S1 合并做**：S1 本来就要给 `BuildFrame` 换成两阶段，顺带把 list 改成视图，避免二次改动。收益 8 倍（8 × 263 KB → 250 KB）。
- **P3 视需求**，文本是最大的单一成员，但会让 `VmDisplay_CommandEqual` 与序列化都要经池查表，复杂度明显上升。

**对外序列化不变**：`emit_resource` 的输出格式与原 `emit_command` 的资源分支逐字节一致，`serialize()` 先输出资源、再输出命令，顺序与现状（资源命令在前）一致。S0 用「改造前后 `resources` 事件逐字节相同」验证过。

### 容量风险（比内存更值得警惕）

现状是「全局 512 条」。改成每窗口一份 512 之后，合成阶段的总量是：

```
Σ(窗口 layer) + 任务栏 + 指针 + 桌面资源
```

很容易超过 512。`VmDisplay_Append` 超限后的行为必须显式化：

- 合成时逐窗口 append，`VmDisplay_Append` 返回 NULL 即计数溢出
- 溢出时置 `wm->overflow = 1` 并写日志，**不静默丢弃**
- 不自动扩容：线上协议与前端的上限按 512 设计，扩容要单独决策

现有 `append_*` 系列大多不检查返回值，这是本设计要顺手补掉的隐患。

## 4. 脏标记来源

| 事件 | contentDirty | chromeDirty | 说明 |
|---|---|---|---|
| 指针输入命中窗口 w | 1 | 0 | 不知道应用内部谁脏，按命中窗口近似 |
| `Jpx__Invalidate(app)` | 1 | 0 | 目前是 app 级，取「当前正在 layout 的窗口」 |
| caret 闪烁 timer | 1 | 0 | 只影响有焦点窗口 |
| 移动 / resize | **1** | 1 | 内容坐标已偏移，必须重建 |
| z 序变化 | 0 | 1 | 内容不含 z，只需重排 |
| 聚焦 / 失焦 | 0 | 1 | 只改标题与边框颜色 |
| 最小化 / 恢复 | 1 | 1 | 可见性变化 |
| 主题变化 | 1 | 1 | 全部窗口 |
| 窗口增删 | — | — | 直接置全局 dirty |

**关键约束：移动窗口必须重建内容。** 内容命令在生成时就带上了 body 偏移（`ClayFrame_AppendCommands(out, cmds, len, bodyX, bodyY, ...)`），位置一变，每条坐标都过期。想复用就必须给线上协议加「图层偏移」——正是第 2 节禁止的事。这一条写在这里，避免后人误以为可以优化。

## 5. `BuildFrame` 两阶段改造

```c
void VmWm_BuildFrame(VmWm *wm, VmDisplayList *out, uint32_t frameId, ...) {
    /* 阶段 1：只为脏窗口重建它自己的 layer.list —— 唯一接触 Clay 的地方 */
    for (int i = 0; i < wm->count; i++) {
        VmWindow *win = &wm->windows[i];
        if (!win->layer) continue;
        if (!win->layer->contentDirty && !win->layer->chromeDirty) continue;
        VmDisplay_Clear(&win->layer->list);
        build_one_window(wm, &win->layer->list, win, ch, fontResolver);
        win->layer->contentDirty = 0;
        win->layer->chromeDirty = 0;
        win->layer->builtFrame = frameId;
    }

    /* 阶段 2：按 z 序合并 —— 纯命令拷贝，不跑布局 */
    VmDisplay_Clear(out);
    out->frameId = frameId;
    for (int pass = 0; pass < 2; pass++) {
        for (int z = 1; z < wm->nextZ; z++) {
            for (int i = 0; i < wm->count; i++) {
                /* 与现状相同的筛选条件 */
                append_layer(out, &wm->windows[i].layer->list);
            }
        }
    }
    append_cursor_commands(wm, out);
}
```

要点：

- 只有阶段 1 碰 Clay 上下文；阶段 2 只是逐条拷贝命令（`VM_DISPLAY_MAX_COMMANDS` 内的线性复制）
- 输出 `out` 仍是全局 list，**顺序、id、坐标与改造前完全一致**
- `VmDisplay_Diff`、`VmTileGrid_Build`、`build_tiles` 全部无需改动

## 6. 明确不承诺的收益

- 不减少带宽：线上仍是全帧或 tile 差分，tile 差分本来就在做局部更新
- 不减少前端重绘：同上
- 拖窗口不重画内容：见第 4 节的坐标约束，做不到
- 不引入跨进程能力：本轮完全不碰 IPC

**唯一确定的收益**：未脏窗口跳过 Clay 布局与命令生成；窗口成为可独立失效、可独立交付的单元，为「应用归属」与「将来本机 IPC」提供依托。

## 7. 迁移步骤（各自可验收）

- **S0 资源外置（P1）—— 已完成（2026-11-22）**：`loadImg`/`definePattern`/`defineStyle`/`defineProgram`/`programControl` 移到 `VmResourceCommand[16]`，`VmDisplayList` 增加 `resources`/`resourceCount`；新增 `VmDisplay_AppendResource`，序列化新增 `emit_resource`，`serialize()` 与 `VmSerialize_DeltaFrameJson` 先资源后命令。
  - 证据：`sizeof(VmDrawCommand)` 992 → **496**，`sizeof(VmDisplayList)` 496 → **263 KB**；`test_frame --probe` 97 项 0 失败（含 `sizeof(VmDrawCommand) <= 512` 预算断言）；`demo_jpx` 的 `resources` 事件改造前后**逐字节相同**（1398 B）；tile 帧内 `loadImg` 计数 0、resources 通道 1；`demo_jpx` 输入 `hi` 与 `test_text_edit` 66 项均通过。
- **S1 图层骨架 —— 已完成（2026-11-22）**：`VmWindowLayer`（每窗口 64 条命令配额）+ `VmCmdBuffer` 写入目标 + `BuildFrame` 两阶段；`ClayFrame_AppendCommands` 与窗口装饰的 `append_*` 辅助全部改写成 `VmCmdBuffer`。
  - 脏源已接：新建、移动、聚焦（旧+新窗口）、最小化/恢复、主题与装饰参数、指针命中的新旧窗口；`Raise` 只置 `dirty`、不作废内容（z 序变化只需重排）。
  - `VmWm_Invalidate` 仍全量作废内容（jpx 层还定位不到具体窗口），因此**本步不承诺 CPU 收益**，只是把结构立起来。
  - 证据：`test_frame --probe` 99 项 0 失败（新增「clean layer is reused」「no layer overflow」）；`demo_jpx` 的 `fullframe` 事件改造前后**逐字节相同**（48137 B）；输入 `hi` 与 caret 闪烁回归通过。
  - 内存代价：`8 × 64 × 496 B ≈ 254 KB` 常驻（`VmWindow` 内嵌 layer）。
- **S2 单窗口失效 —— 已完成（2026-11-22）**：新增 `VmWm_InvalidateWindow(wm, id)`；`JpxApp` 增加 `layoutWinId`（layout 期间由 `window_layout_cb` 设置），`Jpx__Invalidate` 据此只作废当前窗口；`caret_blink`、`Jpx_WindowSetTitle`、`VmShell_Refresh` 各自带上自己的窗口 id；WM 的键盘分支把焦点窗口标脏。
  - 证据：`test_frame --probe` 104 项 0 失败（新增「adding a window does not rebuild others」「invalidated window rebuilt」「untouched window still reused」）；`layoutCount` 实测——S1 时每次按键 `+3`（三窗口全量），S2 后 `+1`；输入 `abc` 与 caret 闪烁回归通过。
  - 剩余全量入口：`Jpx_SetTheme`（chrome 影响所有窗口，本就该全量）与 layout 之外的 `Jpx_Invalidate` 公共 API（调用方没给窗口 id）。
- **S3 存储池化（P2）**：`VmDisplayList` 改视图 + 全进程一份命令池，消掉「输出 list 263 KB + 每窗口 layer 254 KB」的双份，并让 `app->frame` 不再占 263 KB。断言：帧 JSON 与 S1 逐字节相同。

每步都要跑 `test_frame --probe` 与 `test_text_edit --probe`，并在 `demo_jpx` 上验三种场景：拖窗口、按钮 hover、caret 闪烁。

## 8. 未决问题

- **已决**：`VmDefineProgramArgs.commands` 指向的程序内容属于桌面级资源，随 S0 一起留在资源池（`VmResourceCommand`），不进任何窗口 layer。
- **已决**：`VM_DISPLAY_MAX_COMMANDS` 改为双约束——每窗口配额 64、全进程命令池 512。实测单窗口约 40–80 条（两窗口 + 任务栏合计不到 200 条），64 留出余量；超过配额的窗口在 S3 报错而不是静默截断。若后续窗口内容变复杂，优先调高单窗口配额，而不是退回「每窗口 512」。
- **已决**：S1 把 layer 做成 `VmWindow` 的内嵌值而非指针，`VmWm_RemoveWindow` 不需要释放，也就没有泄漏路径；代价是 `VmWm` 结构体增大 254 KB，由 `VmWm_Create` 的 `calloc` 承担（不要放栈上）。
