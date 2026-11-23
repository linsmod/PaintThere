# 工作流：jp_xaml 声明式 UI 层开发

## 适用场景

在 guest 侧新增或扩展 XAML 风格声明式 UI 层（jp_xaml），或调试「XAML 声明的控件没进帧 / 输入没生效 / handler 没触发」时。适用于修改 `vm-desktop/jpx_xaml/*`、`vm-desktop/demo_xaml.c`、相关 CMake 目标时。

## 前置条件

- 设计冻结于 `docs/design-jp-xaml.md`；进度见 `docs/task-jp-xaml.md`。
- 边界：XAML 是外层编写模型，Clay 仍是内部布局引擎；协议/WM/前端零改动；v1 不做动画。
- 环境与 VM 桌面相同：Clay `H:\github_repos\clay`、mystbfont `H:\github_repos\mystbfont_c`、MSYS2 gcc、构建 `python tools/build_vm.py`。

## 相关文件

| 路径 | 作用 |
|---|---|
| `vm-desktop/jpx_xaml/jpx_xaml.h` | 公开 API：Create/Bind/On/Render/Node 查询 |
| `vm-desktop/jpx_xaml/jpx_xaml_parser.c` | XAML 子集 → 定长元素树（索引表达父子/兄弟，哨兵 -1） |
| `vm-desktop/jpx_xaml/jpx_xaml_state.c` | 视图/绑定表/handler 表，纯逻辑，不调 JPX，可独立测试 |
| `vm-desktop/jpx_xaml/jpx_xaml_view.c` | 唯一调用 `Jpx_*` 的 TU：每帧把树回放为控件调用 |
| `vm-desktop/demo_xaml.c` | 表单 demo：XAML 内嵌，应用只写 state + handler |
| `vm-desktop/test_jpx_xaml.c` | 解析/绑定/handler 断言，支持 `--probe` |
| `tools/drive_vm_desktop.py` | HTTP `/input` 合成输入，同连接累积 SSE 帧流 |
| `tools/inspect_vm_frame.py` | 回放 SSE 流，按 tile 还原绝对坐标列出文本 |

## 常用命令

```powershell
cd h:\github_repos\跨端图绘指令流
python tools/build_vm.py
.\vm-desktop\build\test_jpx_xaml.exe --probe

# 启动（不带浏览器，端口独立）
$env:JPX_TRACE='1'
$p = Start-Process -FilePath '.\vm-desktop\build\demo_xaml.exe' `
  -ArgumentList '9060','..\assets' -PassThru -WindowStyle Hidden `
  -RedirectStandardOutput 'vm-desktop\build\xaml_stdout.log' `
  -RedirectStandardError 'vm-desktop\build\xaml_stderr.log'

# 合成输入 + 抓帧
$env:VM_DRIVE_OUT='vm-desktop/build'
python tools/drive_vm_desktop.py 9060 move X Y down X Y up X Y key A snap name
python tools/inspect_vm_frame.py vm-desktop/build/name.txt --text '关键词'
Get-Content vm-desktop\build\xaml_stdout.log -Tail 50

# 收尾
Stop-Process -Id $p.Id
```

## 调试经验（针对本场景）

- **诊断打印走 stdout**：`Start-Process -RedirectStandardError` 在 PowerShell 下拿不到实时 `fprintf(stderr,...)`；`VmWm_Dump` 与临时探针都用 `printf` 到 stdout。排查「代码没生效」前先确认观测通道通。
- **guest 侧观测优先于抓帧反推**：`JPX_TRACE=1` 的 `[wm]` dump 给出窗口集合、z、焦点、每窗 `cmds / dirty(原因) / built`。控件没进帧先看这里，比解 SSE JSON 快。
- **面板上的坐标要按实际帧反查**：合成点击前用 `inspect_vm_frame.py` 列出控件文本的 `abs(x,y)`，不要按 XAML 声明顺序估算。窗口 body 会裁掉超出高度的控件——按钮、状态行「看不见」先查窗口高度和 `body` 矩形。
- **合成点击必须先 move 再 down**：`drive_vm_desktop.py` 的 `down` 动作内部已先发一次 `mousemove`；手写 curl 时若只发 `mousedown`，`Clay_Hovered()` 为假，点击被吞。
- **文本框输入链路有三个独立环节**：焦点（`Jpx__FocusKey` 命中）、编辑状态（`Jpx__TextEditKey` 写 buf）、视图同步（回放器把 buf 写回 model）。任何一环断了都表现为「输入没进帧」。逐环加 stdout 探针，不要连续猜。
- **`Jpx_TextInput` 的 len 必须传 buf 的实际长度**：传缓存旧 len 会让 `ed.len != *len` 成立，触发 `Jpx__TextEditInit` 重建并抹掉刚输入的字符。症状是按键瞬间 buf 有值、下一帧又空。
- **回放器不能无条件置脏**：`JpxXaml_Render` 结尾调 `Jpx_Invalidate` 会破坏空闲零帧；由 handler 在状态真变化时显式置脏。
- **`test_jpx_xaml` 故意不链接 JPX**：纯逻辑测试只链接 parser + state；view 回放器调用 `Jpx_*`，要测它必须链接完整 JPX 依赖（用 `demo_xaml` 做端到端）。
- **`--probe` 模式**：`test_jpx_xaml` 支持，只输出失败与汇总。
- **绑定偏移必须相对 model 基址**：`JpxXamlView_Bind` 的 offset 是「model 指针 + offset」。绑定嵌套在 ctx 里的子结构时写 `offsetof(Ctx, sub) + offsetof(Sub, field)`；直接传 `offsetof(Sub, field)` 会把 1 字节写进 ctx 开头的指针里，点控件即段错误且无任何日志。症状特征：崩溃点在 handler 解引用 model 内指针处，stderr 干净。
- **layout 回调里禁止同步销毁自己的窗口**：`Jpx_Button` 等 handler 在 `JpxXaml_Render`（即 build_one_window）内部执行，此时 `Jpx_WindowClose` 会 memmove/释放窗口数组，Clay 继续布局直接越界（报 `out of bounds array access` 循环刷屏）。窗口开关一律走「意图 + 定时器」（demo_jpx `launcher_tick`、`jpx_shell_settings.c` `settings_tick` 是模板）。
- **font.ttf 没有 U+2713（✓）字形**：勾选框选中态会画成空白。用 U+221A（√），字形存在。给字体加字形前先用 fontTools 查 cmap。
- **浏览器端到端验证右键**：`vm_renderer.js` 已 `preventDefault` 掉 canvas 的 `contextmenu`；合成右键要对 `#c` 元素 dispatch `MouseEvent('mousedown', {button: 2})`——页面上有两个 canvas（`#bg` 在前），`querySelector('canvas')` 会选错。

## 主题（ResourceDictionary）扩展要点

- **尺寸先行**：改 `VmDisplayList`/`VmDefinePatternArgs` 前先用 `gcc -B C:\msys64\mingw64\bin -c` + `objdump -t` 读符号大小（链接探针在本机 collect2 报错，用编译期符号法）。当前 `VmDisplayList` 约 1.02 MB，扩容资源或 stop 颜色用 128 字节会爆栈；stop 颜色固定用 `VM_DISPLAY_MAX_COLOR 32`，栈上 list 改 static。
- **测试 TU 隔离**：`jpx_xaml_theme.c`（纯解析）与 `jpx_xaml_theme_apply.c`（需 JPX/server 依赖）分开，纯测试只链接前者，避免为了一个 Apply 函数拖入整条 JPX 依赖链。
- **主题色生命周期**：`VmTheme`/`JpxTheme` 的颜色是 `const char *`，DeriveTheme 只搬指针、不拷贝。资源表必须比 JpxApp 活得久（demo 用 static）。
- **shell 不上提 Jpx__Color**：vm 层不能依赖 Clay，shell 直接本地解析 rgba 字符串（`theme_color()`）。
- **资源包协议（现行）**：资源本体不进 SSE。服务端只发 `loadResPack(id)`，client 自己 `GET /respack/<id>` 并按 id 缓存。
  验证顺序：`curl /respack/<id>` 看命令数组 → 抓 SSE 首事件确认 `resources` 里只有一条 loadResPack →
  抓帧看 `w1_title` 的 paint 是 style id（`s_title_active`）而不是 rgba。
- **热更新验证**：点主题按钮后抓 SSE 流，确认 `event: resources-update` 里只有 `loadResPack("theme-N")`；
  新 id 递增说明注册生效。只断言画面文本不够，要直接看事件 payload。
- **DeriveTheme 映射要对着真实主题**：RetroBar `Windows XP Blue.xaml` 只有任务栏资源（155 key 全是
  Taskbar/Tray/TaskButton/StartButton/Clock）；`AccentColor`/`WindowBackground`/`TaskTextColor`/`DesktopFrom`
  这些「看起来合理」的 key 在里面**并不存在**，`Select-String 'x:Key="AccentColor"'` 计数为 0 即可证实。
  新增映射前先确认 key 存在，否则整份 token 静默 fallback 回默认暗色。
- **渐变的两个通道**：`drawRect`/`drawBorder` 的 paint 命中 `styleCache` 才走渐变，所以 WM 的 chrome 颜色字段
  要填 `s_*` style id；Clay 布局的 shell 只支持单色 `backgroundColor`，渐变填不进去。
- **主题要能在刷新后恢复**：旧实现把资源写在首连接的 `resources` 事件里，主题切换后新连接拿不到；
  现行用 respack id + HTTP 拉取，刷新即恢复。改动资源路径后必须刷新页面复验一次。

## 退出条件

六个测试套件（`test_jpx_xaml` / `test_jpx_xaml_theme` / `test_frame` / `test_text_edit` / `test_sys` / `test_fs`，
全部从仓库根运行）全通过、`demo_xaml` 端到端验证四类交互（输入/勾选/列表/按钮）与刷新后主题恢复、文档与索引更新后，停止本任务启动的 demo 进程。
