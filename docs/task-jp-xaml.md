# 任务：jp_xaml 声明式 UI 层

> 自包含执行文档（2026-11-23 立项）。会话中断或上下文丢失后据此恢复。
> 设计决策冻结于 `docs/design-jp-xaml.md`；本文只记录执行步骤、命令、观察与恢复点。
> 用户已确认：L1 方案（XAML 外层 + Clay 内部），**不做动画**。

## 1. 目标与验收定义

做一个 C 写的 XAML 子集解析与执行库，应用用声明式标记描述界面与业务连接，每帧回放为现有 JPX 调用。

逐条可验证的验收：

- 应用源码只 `#include "jpx.h"` 与 `#include "jpx_xaml.h"`，`grep -c "clay\|vm_display\|vm_tiles\|json"` 为 0。
- 表单 demo 含 3 个输入框、2 个勾选框、2 个按钮、1 个列表；点击、输入、Tab 切换、勾选、列表选择与手写 JPX 行为一致。
- `test_jpx_xaml` 覆盖解析、树结构、实体、错误定位、绑定读写、handler 触发、未注册事件降级，支持 `--probe`。
- 协议零改动：`test_frame` / `test_text_edit` / `test_sys` / `test_fs` 全部通过。
- 浏览器端到端：表单可交互；空闲零帧。

## 2. 边界

冻结设计见 `docs/design-jp-xaml.md`。关键约束：

- 不改 `vm_wm` / `vm_display` / `vm_tiles` / `vm_server` / `assets/*.js` / `jpx.h`。
- jp_xaml 只调 JPX 公开 API，不拿 `Clay_Context`，不碰 theme 之外的颜色。
- 不支持动画、Style/Template、属性路径绑定、网络加载、多窗口自动管理。

## 3. 文件清单

```text
vm-desktop/
  jpx_xaml/
    jpx_xaml.h            公开头：View/Bind/On/Render/Node 查询
    jpx_xaml_internal.h   内部：节点、属性、绑定与 handler 槽
    jpx_xaml_parser.c     XAML 子集解析 → 定长元素树
    jpx_xaml_view.c       绑定表、handler 表、每帧回放与事件分发
  demo_xaml.c             表单 demo（XAML 文本内嵌）
  test_jpx_xaml.c         解析/绑定/事件断言（不链接 Clay/WM/网络）
  CMakeLists.txt          新增 demo_xaml、test_jpx_xaml 目标
docs/
  design-jp-xaml.md       设计（已写）
  task-jp-xaml.md         本文
  INDEX.md                索引更新
  test-timing.md          新增测试耗时登记
tools/INDEX.md            新增文件登记
workflows/INDEX.md        完成后登记/更新工作流
```

## 4. 分阶段实施与验收

### S1：XAML 解析器 + 元素树 + `test_jpx_xaml` 解析断言

文件：`jpx_xaml_internal.h`、`jpx_xaml_parser.c`、`test_jpx_xaml.c`（解析部分）、`CMakeLists.txt`（`test_jpx_xaml` 目标）。

解析器职责：标签/属性/自闭合/注释/文本/五个实体；产出定长节点数组（父、首子、兄弟用下标）。

验收断言（`test_jpx_xaml --probe`）：

- 最小文档解析出根节点与父子/兄弟关系正确。
- 自闭合标签、注释、属性值单双引号、五个实体都正确。
- 属性值缺失引号、未闭合标签、未知实体定位到行号并报错。
- 节点/属性超容量时返回明确错误，不越界。

### S2：视图骨架 + 绑定表 + handler 表 + 回放器

文件：`jpx_xaml.h`、`jpx_xaml_view.c`、`test_jpx_xaml.c`（绑定与事件部分）。

回放器把元素树映射为 `Jpx_*` 调用。无 `Name` 的控件用节点下标生成稳定 id（如 `n12`），保证跨帧焦点与编辑槽匹配。

验收断言：

- `JpxXamlView_Bind` 按名字登记，偏移/cap/kind 正确；重复名字覆盖。
- `JpxXamlView_On` 按名字登记 handler；触发一次且只一次。
- 未注册事件名不报错、控件状态仍更新。
- 绑定字段不存在时回放忽略该属性，不静默截断其他属性。
- `{Binding name}` 只取单一字段名；含点号时按未绑定处理（登记为降级）。

### S3：表单 demo + 端到端交互

文件：`demo_xaml.c`、`CMakeLists.txt`（`demo_xaml` 目标）、`tools/start_vm_desktop.py`（若需新增 app 名分支）。

demo 的 XAML 内嵌为字符串；state 结构体含 3 个输入文本、2 个勾选、1 个列表选中项；handler 演示写回 state 与 `Jpx_Invalidate`。

验收：

- `python tools/build_vm.py` 通过，`demo_xaml.exe` 生成。
- `python tools/start_vm_desktop.py demo_xaml` 启动后浏览器可见完整表单。
- curl 合成输入：点击按钮 handler 触发、输入框可输入、勾选翻转、列表选择变化。
- 空闲 2 秒 frameId 不变（时钟帧除外）。
- 应用源码 `grep -c "clay\|vm_display\|vm_tiles\|json" demo_xaml.c` = 0。

### S4：全量回归 + 文档与工作流收尾

- `test_frame --probe` / `test_text_edit --probe` / `test_sys --probe` / `test_fs --probe` 全通过，断言数不回退。
- `docs/test-timing.md` 登记 `test_jpx_xaml` 实测耗时；`docs/INDEX.md`、`tools/INDEX.md` 更新。
- 按 `AGENTS.md` 整理或更新一条工作流（XAML 声明式 UI 层的开发验证管道）。

## 5. 构建与验证命令（Windows host）

```powershell

python tools/build_vm.py
.\vm-desktop\build\test_jpx_xaml.exe --probe
.\vm-desktop\build\test_frame.exe --probe
.\vm-desktop\build\test_text_edit.exe --probe
.\vm-desktop\build\test_sys.exe --probe
.\vm-desktop\build\test_fs.exe --probe
python tools/start_vm_desktop.py demo_xaml
# 浏览器：agent-browser --session xaml open http://127.0.0.1:<port>/
```

环境：Clay 源码 `H:\github_repos\clay`；stb_truetype `H:\github_repos\mystbfont_c`；MSYS2 gcc `C:\msys64\mingw64\bin\gcc.exe`；`demo_xaml` 用独立端口（默认 9060）避免与 `demo_wm`(9000)/`demo_jpx`(9050) 冲突。

## 6. 恢复点

- 设计：`docs/design-jp-xaml.md`（词汇表、绑定模型、事件流、非目标）。
- JPX 公开接口：`vm-desktop/jpx/jpx.h`（`Jpx_*` 控件签名、返回值语义）。
- JPX 实现参考：`vm-desktop/jpx/jpx_widget.c`（控件调用约定）、`jpx_window.c`（焦点链与按下沿）。
- 现有 demo：`vm-desktop/demo_jpx.c`（表单布局样板）。
- 测试样板：`vm-desktop/test_text_edit.c`（`--probe` 写法规整）。
- 构建与启动：`tools/build_vm.py`、`tools/start_vm_desktop.py`。

## 7. 进度

- [x] S1：XAML 解析器 + 元素树 + 解析断言（2026-11-23 完成）
- [x] S2：视图骨架 + 绑定/handler + 回放器（2026-11-23 完成）
- [x] S3：表单 demo + 端到端交互（2026-11-23 完成）
- [x] S4：全量回归 + 文档与工作流收尾（2026-11-23 完成）

## 8. 执行记录

**S1+S2**：新增 `jpx_xaml/jpx_xaml.h`（公开头）、`jpx_xaml_internal.h`（内部节点/绑定/handler 结构）、`jpx_xaml_parser.c`（标签/属性/自闭合/注释/五实体/行号错误）、`jpx_xaml_state.c`（视图生命周期/绑定表/handler 表，纯逻辑不调 JPX）、`jpx_xaml_view.c`（每帧回放为 `Jpx_*` 调用）、`test_jpx_xaml.c`（47 项断言）、`demo_xaml.c`（表单）。CMake 新增 `test_jpx_xaml` 与 `demo_xaml` 目标；`start_vm_desktop.py` 支持 `demo_xaml`（9060）。

期间修掉的偏差：

- 解析器的树指针初值用了 0（index 0 是有效节点），导致首子节点被当成「已存在」，兄弟关系错乱。改为 -1 哨兵。
- 空白绑定/事件名未被拒绝，改用 `blank_name()` 检查。
- `test_handler_table` 用了越界节点下标（Window 的直接子节点是 index 1，不是 2）。
- `jpx_xaml_view.c` 重写后漏了 `<stdio.h>`，`snprintf` 隐式声明导致 `demo_xaml` 链接失败；补上头文件。
- `JpxXaml_Render` 结尾曾无条件 `Jpx_Invalidate`，会破坏空闲零帧；移除，改由 handler 在状态变化时显式置脏。
- **文本框输入被自身抹掉**：回放时给 `Jpx_TextInput` 传的是缓存的 `slot->len`（旧值），按键写入后下一帧 `ed.len != *len` 触发 `Jpx__TextEditInit` 重建，把刚输入的字符清空。改为传 `strlen(slot->buf)` 的实际长度。定位过程用 stdout 探针（见 `docs/debug-vmdesk.md` 第 28/40 条：诊断打印走 stdout、guest 侧观测优先于抓帧反推）。
- demo 窗口高度 430 把按钮行与状态文本裁出 body（tile 里根本看不到）；改为 560 并上移窗口。

验证证据：

- `test_jpx_xaml --probe` 47 项 0 失败，耗时 28 ms（`docs/test-timing.md` 已登记）。
- `demo_xaml.exe 9060 ../assets` + `tools/drive_vm_desktop.py` 合成输入 + `tools/inspect_vm_frame.py` 回放帧：
  - 控件文本全在帧内：姓名/邮箱/备注/同意条款/订阅更新/项目/Alpha/Beta/Gamma/提交/清空。
  - 输入框：点 (250,180) 聚焦后输入 `ABC`，帧内 `abs(211,173) text='ABC'`，`Jpx__FocusKey` 命中焦点控件。
  - 勾选 handler：点同意条款后帧内 `同意: 是`。
  - 列表 handler：点 Gamma 行后 `选中: 2`。
  - 提交 handler：点提交后 `提交 #1: ...`。
- 全量回归：`test_frame --probe` 120 项、`test_text_edit --probe` 66 项、`test_sys --probe` 16 项、`test_fs --probe` 19 项，均 0 失败。
- 应用源码 `demo_xaml.c` 只 include `jpx.h` 与 `jpx_xaml.h`，不含 `clay/vm_display/vm_tiles/json` 词汇。

未验证：浏览器内的实际像素与真实键鼠交互（仅做 guest 侧命令与测试断言覆盖）；`x:Name` 前缀、单双引号混用、实体解码只覆盖解析断言，未在 demo 中逐一走查。

恢复点：若继续扩展，下一步应在 `jpx_xaml_view.c` 加 `Jpx_ListRow*` 的滚动列表映射（当前 `ListBox` 映射 `Jpx_ListBox` 只读前 N 项），并按 `docs/design-jp-xaml.md` §7 的边界补齐 Style/Template 之前先立新任务。
