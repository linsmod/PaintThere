# 测试耗时记录

最后核对：2026-11-22。耗时为本机（Windows 11 / PowerShell 7）单次进程启动到退出的实测值，含进程启动开销（约 10 ms），取连跑中的稳定值。

| 测试 | 命令 | 断言数 | 实测耗时 | 依赖 |
|---|---|---|---|---|
| `test_frame` | `.\vm-desktop\build\test_frame.exe --probe` | 105 | 23 ms | `assets/font.ttf`（默认 `../assets/font.ttf`） |
| `test_sys` | `.\vm-desktop\build\test_sys.exe --probe` | 16 | 6 ms | 无（进程表、内存、CPU 差分、错误分类） |
| `test_text_edit` | `.\vm-desktop\build\test_text_edit.exe --probe` | 66 | 11 ms | 无 |
| `test_fs` | `.\vm-desktop\build\test_fs.exe --probe` | 19 | 6 ms | 无（cwd 路径、中文文件名、元数据、错误分类） |

## 说明

- 四个测试都支持 `--probe`：只输出失败项与 `N checks, M failures` 汇总；不加参数时逐项打印 PASS 与诊断值。
- 都不开服务、不占端口、不读写共享文件，可以并发运行。
- `test_frame` 需要字体文件；它按参数扫描，第一个非 `--` 开头的参数作为字体路径。
- 新增测试时应在此登记实测耗时；耗时明显超出（例如超过 1 秒）需在对应测试文档中说明原因。
