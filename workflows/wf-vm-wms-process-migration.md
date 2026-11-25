# WMS 进程化迁移：构建、验证与排错

适用：`vm_wms + vm_rdp + vm_shell + vm_app` 四进程链路的开发与回归（M0–M7 的产物）。
单进程桌面已移除；需要最小复现时用 `test_frame` / `test_menu` 或 `vm_renderer`。

## 1. 环境与构建

```powershell
# 构建（工作区根目录执行；产物都在 vm-desktop/build/）
C:\msys64\mingw64\bin\cmake.exe --build vm-desktop/build -j 8
```

- 工具链固定 mingw64，与 `tools/build_vm.py` 一致；不要混用 MSVC 或其它 gcc。
- **改代码前先停掉正在运行的 exe**：Windows 会锁住 `build/*.exe`，链接会报
  `Permission denied` 或静默保留旧文件。停止命令：

```powershell
Get-Process vm_wms,vm_rdp,vm_shell,vm_app -ErrorAction SilentlyContinue | Stop-Process -Force
```

- 只用 `-j 8` 全量构建；单 target 构建（`--target vm_wms`）适合看链接错误，但别用它当验收。

## 2. 四进程启动模板

一键启动（默认多进程，Ctrl+C 收全部）：

```powershell
python tools/start_vm_desktop.py            # 端口 9050，日志 temp/desktop-<name>.err.txt
python tools/start_vm_renderer.py           # 同一套 + C 渲染器
```

需要精确控制（多套并行、指定管道/坐标/参数）时用下面的手工模板。
每套实验用**独立管道名**与**独立端口**，可与正在运行的实例并存：

```powershell
$pipe = '\\.\pipe\vmwm_dev1'          # 每次换名
$port = 19120                          # 19000–19999 区间随便挑
$font = 'assets/font.ttf'

Start-Process .\vm-desktop\build\vm_wms.exe   -ArgumentList '--pipe',$pipe,'--screen','800x600' `
    -RedirectStandardError temp\wms.err.txt -WindowStyle Hidden
Start-Sleep -Milliseconds 600
Start-Process .\vm-desktop\build\vm_shell.exe -ArgumentList '--pipe',$pipe,'--font',$font `
    -RedirectStandardError temp\shell.err.txt -WindowStyle Hidden
Start-Sleep -Milliseconds 300
Start-Process .\vm-desktop\build\vm_app.exe   -ArgumentList '--pipe',$pipe,'--title','Hello',`
    '--x','60','--y','60','--font',$font -RedirectStandardError temp\app.err.txt -WindowStyle Hidden
Start-Sleep -Milliseconds 300
Start-Process .\vm-desktop\build\vm_rdp.exe   -ArgumentList '--port',$port,'--pipe',$pipe,`
    '--web-root','assets','--font',$font -RedirectStandardError temp\rdp.err.txt -WindowStyle Hidden
Start-Sleep -Milliseconds 900
```

顺序有依赖：`vm_rdp` 连上时会向上游请求 fullframe；`vm_wms` 必须先起。
`vm_shell` / `vm_app` 与 `vm_rdp` 的先后不敏感。

## 3. 验证三件套

### 3.1 抓帧 + 反查坐标（不依赖浏览器）

```powershell
python tools/drive_vm_desktop.py 19120 wait 1200 snap s1
python tools/inspect_vm_frame.py temp\s1.txt --text 开始
python tools/inspect_vm_frame.py temp\s1.txt --tiles
```

`drive_vm_desktop.py` 的 `down X Y` **内部先发一次 mousemove**，这是必须的：
Clay 的 `Clay_Hovered()` 依赖上一帧 `pointerInfo`，只发 `mousedown` 会命中失败。
手写 `curl POST /input` 时同样要先发 `mousemove`。

坐标反查顺序：先拿到控件文本的 `abs(x,y)`，再减窗口原点与 body 偏移
（有装饰窗口 body 偏移约为 `(1, 30)`），最后传给 `drive`。

### 3.2 浏览器视觉

```powershell
agent-browser open http://127.0.0.1:19120/
agent-browser wait --load load
agent-browser screenshot temp/vis.png
agent-browser close
```

截图看三件事：任务栏在底部且无标题栏、窗口装饰完整、控件文字可读。
**浅底白字**是 `vm_shell` 条目配色问题的典型症状（见 §5）。

### 3.3 单元测试与 e2e

```powershell
# 9 个纯逻辑测试（当前断言数）
cd vm-desktop\build
$t=@('test_frame','test_menu','test_wms_ipc','test_wms_peer','test_text_edit',
     'test_jpx_xaml','test_jpx_xaml_theme','test_sys','test_ui')
foreach($n in $t){ $o = & ".\$n.exe" --probe 2>&1 | Select-Object -Last 1; "$n : $o" }

# 跨进程：7 个场景 61 条断言
pwsh tools/e2e_wms.ps1 -SkipBuild
```

预期计数：120 / 30 / 36 / 7 / 66 / 67 / 137 / 16 / 26；e2e `61/61`。
计数变化时必须同步 `docs/test-timing.md`，否则回归口径会漂。

## 4. 排错顺序（自上而下）

| 现象 | 先看 | 常见原因 |
|---|---|---|
| 浏览器黑屏 / 只有背景 | `temp\rdp.err.txt` | rdp 没连上 wms（管道名错、wms 未起） |
| 窗口出现但内容空 | `temp\app.err.txt` | 没收到 `EVT_LAYOUT_REQ`；或 `push` 未被采纳（epoch 旧） |
| 任务栏缺失 | `temp\shell.err.txt` | `push content accepted=0`；或 shell 名下窗口被回收 |
| 点击无反应 | app 的 `input kind=...` | 坐标是画布还是 body 局部；窗口是否聚焦（键盘事件只给焦点窗口） |
| 点一次触发两次 | app 日志重复 `button ->` | 把 down 当 click；用按下沿 + 抬起命中同一控件 |
| resize 后内容不重排 | app 的 `layout epoch=` | 只更新了 body 尺寸没重跑布局；或 push 用了旧 epoch |

日志惯例：每个进程都 `-RedirectStandardError temp\xxx.err.txt`，关键路径都有一行
`[wms]/[shell]/[app]/[rdp]` 前缀日志。**不要靠猜**，先读对应进程最后 10 行。

## 5. 已踩过的坑（按代价排序）

1. **事件回调里发同步 WMS 请求 → libuv 重入**。
   `Wms_CreateWindow` / `Wms_PushContent` 内部跑 `uv_run`；在 `on_wms_event` 里直接调用会
   嵌套 `uv_run`，ACK 永远读不到（表现为 `create failed (timeout)`）。
   规则：**回调只记意图，所有同步请求在主循环里执行**（`shell/main.c` 的 `g_pending*` 模式）。
2. **`EVT_LAYOUT_REQ` 早于 `Wms_CreateWindow` 的 ACK 到达**。
   服务端在 create 处理里就发了 layout。此时调用方还没拿到 `win_id`。
   单窗口进程可「认领第一条 layout_req」；多窗口必须用「本次 create 期间未认领」的状态机。
3. **外壳断开不应保留其窗口**。曾按「SHELL 掉线保留窗口、owner 置 0」实现，结果 kill 外壳后
   任务栏以无主窗口留在画面上。改为**断开即回收名下窗口**，工作区保留最后值。
4. **`CLAY_SIZING_FIT` 在空容器上算 0 高**。空文本输入框高度为 0，背景与命中区被 tile 切分
   静默丢弃，控件「看不见」。输入框/按钮一律给固定高度。
5. **合成点击必须带 `mousemove`**。只发 `mousedown` 时 `Clay_Hovered()` 为假，点击被吞。
6. **无条目渐变的主题不能拿 `fgColor` 当条目底色**。`fgColor` 是文字色，会出现浅底白字；
   改 `bgColor`，focused 仍用 `accentColor`。
7. **`vm_host` 丢弃 `cmd_size` 不匹配的内容**。改 `VmDrawCommand` 必须同时改所有 producer 与
   消费端，否则内容静默消失（日志有 `cmd_size=... != ...`）。
8. **`uv_close` 在 `uv_write` 排空前调用会丢数据**（M4）。需要 `closeAfterFlush`。
9. **`on_read` 里先 `free(buf->base)` 再解析** 是 use-after-free（M4）。先复制/解析，再释放。

## 6. 新增一个 producer（不写 C 也能接）

契约只有两条：

1. 内容：`CMD_WINDOW_CONTENT(win_id, epoch, cmd_size, count, bytes)`，字节流是
   `VmDrawCommand[]`，坐标是 **body 局部**。
2. 输入：收 `EVT_INPUT`（body 局部），把「按下/抬起/移动/键盘/滚轮」映射成自己的状态机。

隐式要求：

- **命令 id 必须跨帧稳定**。diff 与 tile 差分都靠 id；随机 id 不会报错，只会退化成全量下发。
- **颜色是 CSS 字符串**（`rgba(...)`）。非 C 的 producer 要自己生成该语法。
- **epoch 必须原样回传**。wms 用它丢弃过期内容。

最小验证：Python 用 `wms_client` 的等价协议推两条矩形命令，再用 `inspect_vm_frame.py`
确认它们进了帧。这一步能同时验证 id 稳定与 body 坐标。

## 7. 当前代码布局（M7.5 之后）

```text
vm-desktop/
  wms/          wms_ipc.h · wms_client.c · wms_server.c · vm_host.c
  rdp/          HTTP / SSE / WS / 静态资源 / respack
  shell/        任务栏 · 开始菜单 · 右键菜单 · 设置面板
  app/          宿主（main.c）· ui_hello.c · ui_xaml.c · ui_clay_demo.c
  ui/           vm_ui_core（焦点/编辑/命中）· ui/clay（vm_ui_clay）
  jpx_xaml/     parser/state · Direct 渲染/交互 · ResourceDictionary 主题
  vm_wm.c       宿主渲染副本 + test_frame/test_menu 夹具；窗口内容契约是 VmCmdBuffer
  vm_shell.c    任务栏纯绘制层（单进程适配在 vm_shell_local.c）
```

已删除：`jpx/`、`demo_jpx.c`、`demo_xaml.c`、`jpx_xaml_view.c`、`jpx_xaml_theme_apply.c`、
`jpx_shell_settings.*`。历史文档里的旧路径只作记录。

## 8. 提交前检查单

- [ ] 停掉所有 `vm_*` 进程后重新全量构建，exit 0。
- [ ] 9 个测试计数与 `docs/test-timing.md` 一致。
- [ ] `pwsh tools/e2e_wms.ps1 -SkipBuild` → 61/61。
- [ ] 改到 UI 时补一张浏览器截图，确认不是「能跑但看不见」。
- [ ] 文档里的可执行体列表与 `CMakeLists.txt` 一致。
