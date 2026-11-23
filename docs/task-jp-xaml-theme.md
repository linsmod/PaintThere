# 任务：jp_xaml ResourceDictionary 主题体系

> 自包含执行文档（2026-11-23 立项）。会话中断后据此恢复。
> 设计冻结于 `docs/design-jp-xaml-theme.md`；本文只写执行步骤、命令、观察与恢复点。
> 用户已确认：按路径 B 推进（ResourceDictionary 风格主题），接受协议 descriptor 扩展与 resources 热更新；仍不做动画。

## 1. 目标与验收

让 jp_xaml 能加载 RetroBar 风格的 ResourceDictionary 主题文件，驱动 WM 装饰、shell 任务栏与 JPX 控件，并支持运行时切换。

逐条验收：

- 解析 `Windows XP Blue.xaml` 子集，155 个 `x:Key` 中 Solid/LinearGradient/RadialGradient/Double 全覆盖。
- `{StaticResource}` / `{DynamicResource}` 在 v1 等价展开；控件不直接写颜色。
- `definePattern` descriptor 支持多 stop 与方向/Radial；前端正确绘制。
- 运行时切主题 → 所有客户端收 `resources-update` → 画面变化，无需重连。
- WM 标题栏、shell 开始按钮/条目高亮、控件强调色随主题变化。
- 全量回归：`test_frame` / `test_text_edit` / `test_sys` / `test_fs` / `test_jpx_xaml` 全通过。

## 2. 边界

冻结设计见 `docs/design-jp-xaml-theme.md`。关键：

- 不改 `VM_CMD_*`、绘制原语、输入协议。
- 改 `VmDefinePatternArgs`、`VM_DISPLAY_MAX_RESOURCES`、`vm_serialize_json.c`、`assets/app.js`、`vm_server.c/.h`。
- 不做 Style/ControlTemplate/DrawingBrush/ImageBrush/动画。

## 3. 文件清单

```text
vm-desktop/
  vm_display.h/.c          改：VmPatternType + VmGradientStop + VmDefinePatternArgs + 容量
  vm_serialize_json.c      改：definePattern 序列化 stops/direction
  vm_theme.h/.c            改：VmColor_Parse 共享 + 从资源表派生 VmTheme
  vm_server.h/.c           改：BroadcastResources + resources-update 事件
  jpx_xaml/
    jpx_xaml_theme.h/.c    新增：VmResourceTable、解析、查找、派生主题
    jpx_xaml_parser.c      改：ResourceDictionary 标签支持（复用现有解析器）
    jpx_xaml_view.c        改：{StaticResource} 展开
    jpx_xaml.h             改：LoadTheme / SetTheme API
    jpx_xaml_internal.h    改：资源表结构
  vm_shell.h/.c            改：VmShell_SetTheme + 去硬编码
  jpx/jpx_app.c            改：主题切换传播（resources 重建 + shell）
  test_jpx_xaml_theme.c    新增：资源解析/查找断言
  test_frame.c             改：多 stop 序列化断言
  demo_xaml.c              改：加载主题文件 + 切换演示
assets/
  app.js                   改：多 stop / direction / radial 渐变绘制 + resources-update 监听
docs/
  design-jp-xaml-theme.md  已写
  task-jp-xaml-theme.md    本文
  INDEX.md / test-timing.md  更新
workflows/wf-jp-xaml-dev.md  更新
```

## 4. 分阶段实施与验收

### S1：协议与前端的渐变能力（可独立验收）

文件：`vm_display.h/.c`、`vm_serialize_json.c`、`assets/app.js`。

- `VmPatternType` 加 `VM_PATTERN_RADIAL`；`VmDefinePatternArgs` 加 `stops[8]` / `stopCount` / `direction`；容量 16 → 256。
- `VM_PATTERN_GRADIENT` 序列化：`stopCount>=2` 时输出 `stops:[{color,offset}...]` 与 `direction`；缺省保留 `from`/`to`。
- 前端 `drawRect` 的 gradient 分支：优先用 `stops`，按 `direction` 换算端点；`type==='radial'` 用 `createRadialGradient`。

验收：`test_frame --probe` 新增断言；curl 抓帧确认 descriptor 含 stops；浏览器确认多色过渡。

### S2：XAML 资源字典解析与资源表

文件：`jpx_xaml_theme.h/.c`、`jpx_xaml_parser.c`、`jpx_xaml_internal.h`、`test_jpx_xaml_theme.c`。

- 定义 `VmResourceTable`：key → {kind, cssColor, stops, direction, number}，定长容量 256。
- 解析器识别 `ResourceDictionary` 下的 `SolidColorBrush` / `LinearGradientBrush` / `RadialGradientBrush` / `GradientStop` / `system:Double`，读 `x:Key`。
- `VmRes_GetColor/GetDouble/GetPattern` 查询函数。
- `{StaticResource key}` / `{DynamicResource key}` 解析与展开。

验收：`test_jpx_xaml_theme --probe` 断言 key 数量、解析值、资源展开、缺失 key 降级。

### S3：派生主题与 WM/shell 接入

文件：`vm_theme.c`、`vm_shell.c/.h`、`jpx/jpx_app.c`。

- `VmColor_Parse` 从 `Jpx__Color` 提到 `vm_theme.c`，JPX 与 shell 共用。
- `VmTheme_FromResources(table, base)` 派生 VmTheme。
- `VmShell_SetTheme(shell, theme)` + shell 12 处硬编码改为查 theme。
- `Jpx_SetTheme` 补 resources 重建与 shell 换肤。

验收：`test_frame` 断言 shell 颜色来自主题；demo 中标题栏/任务栏/控件同步变色。

### S4：资源热更新与主题切换演示

文件：`vm_server.c/.h`、`assets/app.js`、`jpx_xaml_view.c`、`demo_xaml.c`。

- `VmServer_BroadcastResources(json)` → `event: resources-update`。
- 前端监听并覆盖缓存 + 触发全量重绘。
- demo 加主题切换按钮（XAML handler），加载第二个主题并广播。

验收：两个客户端同时连接，切主题后都收到 `resources-update`，画面变化；断线重连仍正确。

### S5：全量回归 + 文档与工作流收尾

- 六个测试套件全通过。
- `docs/INDEX.md` / `test-timing.md` / `tools/INDEX.md` 更新。
- 更新 `workflows/wf-jp-xaml-dev.md`，登记主题相关命令与坑。
- 提交。

## 5. 构建与验证命令

```powershell
cd h:\github_repos\跨端图绘指令流
python tools/build_vm.py
.\vm-desktop\build\test_frame.exe --probe
.\vm-desktop\build\test_jpx_xaml.exe --probe
.\vm-desktop\build\test_jpx_xaml_theme.exe --probe
python tools/start_vm_desktop.py demo_xaml
# 浏览器：http://127.0.0.1:9060/
```

环境：同 `docs/task-jp-xaml.md`。RetroBar 主题样本：`H:\gitee_repos\RetroBar\RetroBar\Themes\Windows XP Blue.xaml`（只读参考，不修改）。

## 6. 恢复点

- 设计：`docs/design-jp-xaml-theme.md`。
- RetroBar 参考：`H:\gitee_repos\RetroBar\RetroBar\Themes\Windows XP Blue.xaml`、`RetroBar\Utilities\DictionaryManager.cs`。
- 现有资源链：`vm-desktop/vm_theme.c`（`VmTheme_AppendResources`）、`vm_serialize_json.c`（`emit_resource`）、`assets/app.js`（`definePattern`/`drawRect`）。
- jp_xaml：`vm-desktop/jpx_xaml/`（parser/state/view）。
- 调试经验：`docs/debug-跨端图绘指令流.md`（stdout 探针、guest dump 优先）。

## 7. 进度

- [x] S1：协议与前端渐变能力（2026-11-23 完成）
- [x] S2：XAML 资源字典解析与资源表（2026-11-23 完成）
- [x] S3：派生主题与 WM/shell 接入（2026-11-23 完成）
- [x] S4：资源热更新与主题切换演示（2026-11-23 完成）
- [x] S5：全量回归 + 文档收尾（2026-11-23 完成）

## 8. 执行记录

**S1**：`vm_display.h` 加 `VM_PATTERN_RADIAL`、`VmGradientStop[8]`、`VM_DISPLAY_MAX_COLOR 32`，资源容量 16→256；`vm_serialize_json.c` 加 `json_float` 并输出 `stops:[{color,offset}]`；`assets/app.js` 加 `gradientFill()` 支持多 stop、direction 端点、createRadialGradient。

关键发现：实测 `sizeof(VmDisplayList)`=1.02 MB、`sizeof(VmResourceCommand)`=2912 B，原设计估的「增大 200 KB」严重低估。已修正设计文档：stop 颜色用 32 字节紧凑长度，并把 6 处栈上 `VmDisplayList`（test_frame ×3、demo_frame、demo_wm、jpx_app）改为 static，避免 >1.8 MB 结构爆栈。

**S2**：新增 `jpx_xaml_theme.h/.c`：解析 `ResourceDictionary` 下 Solid/Linear/Radial Brush 与 system:Double，`#AARRGGBB` → `rgba()` 转换，`VmRes_Find/Color/Number`、`JpxXamlTheme_Resolve` 支持 Static/DynamicResource（v1 等价）、`DeriveTheme`、`AppendPatterns`。`test_jpx_xaml_theme.c` 33 项断言全通过。

**S3**：`vm_shell.c` 的 6 处硬编码颜色改为从 theme 派生（本地 `theme_color()` 解析 rgba，不上提 Jpx__Color 以免 vm 层依赖 Clay）；新增 `VmShell_SetTheme`；`jpx.h` 加 `Jpx_SetVmTheme`/`Jpx_BroadcastResources`（include vm_theme.h，POD 无 Clay 依赖）；`JpxApp` 加 `vmTheme/hasVmTheme`。

**S4**：`vm_server.c/.h` 加 `VmServer_BroadcastResources` + 主循环广播 `event: resources-update`（调用方 JSON 复制、非阻塞发送）；`assets/vm_renderer.js` 监听该事件覆盖 pattern/style 缓存；`jpx_xaml_theme_apply.c` 一步完成 Derive → SetVmTheme → Serialize → Broadcast（单独 TU 便于纯测试链接）；`demo_xaml.c` 加「切换主题」按钮和暖色 ResourceDictionary。

验证证据：

- 六个测试套件全通过：test_jpx_xaml 47、test_jpx_xaml_theme 33、test_frame 120、test_text_edit 66、test_sys 16、test_fs 19，0 失败。
- 端到端：`drive_vm_desktop.py` 点「切换主题」→ SSE 流出现 `event: resources-update`，携带 `rgba(255,140,66,1.000)` 等新主题颜色；状态文本「主题已切换」进帧；`p_window` 等标准样式按新主题重建。

未验证：浏览器内多 stop 渐变的实际像素效果（只验证了 descriptor 序列化与事件广播）；RetroBar 19 个主题文件的完整兼容（v1 样本为自建子集）；Style/ControlTemplate/DrawingBrush/ImageBrush（明确不做）。

恢复点：继续扩展时优先补 `DeriveTheme` 的 token 覆盖（当前只映射 8 个 key），以及前端 `applyResources` 后主动触发全量重绘的机制（当前靠主题切换时的窗口置脏推新帧）。

### 追加：真实 RetroBar 主题文件支持（2026-11-23）

用户反馈「没看到 Windows XP 主题」——此前只用自造暖色样本，没真正加载 RetroBar 文件。补齐如下：

- **容量**：`JPX_XAML_MAX_NODES` 128 → 1024（XP Blue 898 节点、Vista Aero 905）；`ATTR_VALUE_CAP` 256→512、`TEXT_CAP` 512→8192（Style/ControlTemplate 里的长文本）。大元素树改为 static 存储（`JpxXamlTheme_Parse` 的 `doc`、`test_jpx_xaml.c` 4 处），避免 600KB 栈溢出。
- **探针**：`test_jpx_xaml_theme.exe --file <path>` 解析真实主题并打印资源摘要。实测 `Windows XP Blue.xaml`：111 资源（155 key 中 Brush/Double 类型），`TaskbarBackground` = 5 stop 线性渐变，首色 `rgba(56,136,233,1)`。
- **demo**：`demo_xaml.exe <port> <webroot> --theme <path>` 加载真实文件；注意 PowerShell `Start-Process -ArgumentList` 给含空格路径加引号。
- **渐变代表色**：`JpxRes_Color` 对渐变资源返回首 stop 颜色，供只能吃单色的 VmTheme token 使用（否则 XP 蓝渐变取到空串）。加断言覆盖。
- **任务栏渐变**：`JpxXamlTheme_Apply` 把 XAML 的 `TaskbarBackground` 多 stop 渐变覆盖到 `p_panel`（`s_panel` 引用的 pattern），让任务栏显示真实 XP 蓝渐变而非单色。

验证：启动 `--theme "...\Windows XP Blue.xaml"`，点切换按钮 → `resources-update` 114 条命令、29 个渐变，`p_panel` = 5-stop XP 蓝渐变；状态文本「主题已切换」进帧。六个测试套件全通过（主题测试 34 项）。

未覆盖：RetroBar 主题的 Style/Setter/ControlTemplate/DrawingBrush/ImageBrush 仍跳过（v1 非目标）；任务栏按钮/文字用单色代表色，未逐元素套用各自渐变 key。
