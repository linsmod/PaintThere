# workflows 索引

- `wf-vm-instruction-desktop.md` — VM 指令化桌面端到端开发：emsdk/gcc/Clay/mystbfont 环境、构建命令（含 `vm_tiles.c`）、tiles 协议验证清单与全部踩坑。
- `wf-verify-dual-browser-cursor.md` — 已废弃（.NET/WinForms WebSocket 通道已移除），仅作历史记录；当前多客户端指针验证并入 `wf-vm-instruction-desktop.md`。
- `wf-jp-xaml-dev.md` — jp_xaml 声明式 UI 层开发与排错：构建/测试/合成输入/帧回放的工具组合，stdout 探针、`Jpx_TextInput` len 契约、坐标反查等本场景专有经验。
- `wf-vm-wms-process-migration.md` — WMS 进程化迁移（M0–M7）：`vm_wms + vm_rdp + vm_shell + vm_app` 的启动模板、抓帧/截图/单测/e2e 三件套、排错顺序表、9 条已踩坑、新增 producer 的契约、M7.5 后的代码布局与提交前检查单。

调试经验集中在 `docs/debug-vmdesk.md`，端到端构建/验证/排错的可复用流程见 `wf-vm-instruction-desktop.md`。
