# 测试耗时记录

最后核对：2026-11-23。耗时为本机（Windows 11 / PowerShell 7）单次进程启动到退出的实测值，含进程启动开销（约 10 ms），取连跑中的稳定值。

| 测试 | 命令 | 断言数 | 实测耗时 | 依赖 |
|---|---|---|---|---|
| `test_frame` | `.\vm-desktop\build\test_frame.exe --probe` | 120 | 30 ms | `assets/font.ttf`（默认 `../assets/font.ttf`） |
| `test_sys` | `.\vm-desktop\build\test_sys.exe --probe` | 16 | 53 ms | 无（进程表、内存、CPU 差分、错误分类） |
| `test_text_edit` | `.\vm-desktop\build\test_text_edit.exe --probe` | 66 | 13 ms | 无 |
| `test_jpx_xaml` | `.\vm-desktop\build\test_jpx_xaml.exe --probe` | 47 | 47 ms | 无（纯解析/绑定/handler 逻辑） |
| `test_jpx_xaml_theme` | `.\vm-desktop\build\test_jpx_xaml_theme.exe --probe` | 86 | 71 ms | 无（合并/解析/查找/派生/预编译往返）；`--file <path>` 解析真实主题、`--dir <dir>` 扫目录、`--precompile <xaml> <outDir>` 写预编译产物 |
| `test_fs` | `.\vm-desktop\build\test_fs.exe --probe` | 19 | 9 ms | cwd 必须是仓库根（读 `docs/INDEX.md`） |

## 说明

- 六个测试都支持 `--probe`：只输出失败项与 `N checks, M failures` 汇总；不加参数时逐项打印 PASS 与诊断值。
- 都不开服务、不占端口、不读写共享文件，可以并发运行。
- `test_frame` 需要字体文件；它按参数扫描，第一个非 `--` 开头的参数作为字体路径。
- `test_fs` 依赖 cwd：在 `vm-desktop/build` 里跑会因找不到 `docs/INDEX.md` 出现 6 项失败，必须从仓库根运行。
- 新增测试时应在此登记实测耗时；耗时明显超出（例如超过 1 秒）需在对应测试文档中说明原因。
