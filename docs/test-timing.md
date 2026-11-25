# 测试耗时记录

最后核对：2026-11-24（新增 `test_wms_ipc` / `test_wms_peer`，修正 `test_jpx_xaml_theme` 断言数）。耗时为本机（Windows 11 / PowerShell 7）单次进程启动到退出的实测值，含进程启动开销（约 10 ms），取连跑中的稳定值。

| 测试 | 命令 | 断言数 | 实测耗时 | 依赖 |
|---|---|---|---|---|
| `test_frame` | `.\vm-desktop\build\test_frame.exe --probe` | 120 | 30 ms | `assets/font.ttf`（默认 `../assets/font.ttf`） |
| `test_menu` | `.\vm-desktop\build\test_menu.exe --probe` | 30 | 38 ms | `assets/font.ttf`（右键路由/菜单命中/popup 外点关闭） |
| `test_sys` | `.\vm-desktop\build\test_sys.exe --probe` | 16 | 53 ms | 无（进程表、内存、CPU 差分、错误分类） |
| `test_text_edit` | `.\vm-desktop\build\test_text_edit.exe --probe` | 66 | 13 ms | 无 |
| `test_jpx_xaml` | `.\vm-desktop\build\test_jpx_xaml.exe --probe` | 67 | 49 ms | 无（纯解析/绑定/handler 逻辑 + Direct 渲染/交互） |
| `test_ui` | `.\vm-desktop\build\test_ui.exe --probe` | 26 | 3 ms | 无（焦点/文本编辑/命中纯状态机） |
| `test_jpx_xaml_theme` | `.\vm-desktop\build\test_jpx_xaml_theme.exe --probe` | 137 | 114 ms | 无（合并/解析/查找/派生/预编译往返/css 编译）；`--file`、`--dir`、`--precompile <xaml> <outDir>`、`--compile-css <css> <outDir>` |
| `test_fs` | `.\vm-desktop\build\test_fs.exe --probe` | 19（1 项既有失败，见 `issues/INDEX.md`） | 13 ms | cwd 必须是仓库根（读 `docs/INDEX.md`） |
| `test_wms_ipc` | `.\vm-desktop\build\test_wms_ipc.exe --probe` | 36 | 10 ms | 无（协议编解码往返、非法 magic、超上限长度、变长尾随；不链 socket） |
| `test_wms_peer` | `.\vm-desktop\build\test_wms_peer.exe --probe` | 7 | 33 ms | 无（进程身份抽象在 Windows 下走真实管道句柄） |

## 说明

- 九个测试都支持 `--probe`：只输出失败项与 `N checks, M failures` 汇总；不加参数时逐项打印 PASS 与诊断值。
- 都不开服务、不占端口、不读写共享文件，可以并发运行。
- `test_frame` / `test_menu` 需要字体文件；按参数扫描，第一个非 `--` 开头的参数作为字体路径。
- `test_fs` 依赖 cwd：在 `vm-desktop/build` 里跑会因找不到 `docs/INDEX.md` 出现 6 项失败，必须从仓库根运行。
- 新增测试时应在此登记实测耗时；耗时明显超出（例如超过 1 秒）需在对应测试文档中说明原因。
