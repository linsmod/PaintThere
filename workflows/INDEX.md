# workflows 索引

- `wf-vm-instruction-desktop.md` — VM 指令化桌面端到端开发：emsdk/gcc/Clay/mystbfont 环境、构建命令（含 `vm_tiles.c`）、tiles 协议验证清单与全部踩坑。
- `wf-verify-dual-browser-cursor.md` — 用两个 agent-browser 会话验证多客户端指针互见与既有点击回传，含排查命令与 3 秒超时注意事项。
- `wf-jp-xaml-dev.md` — jp_xaml 声明式 UI 层开发与排错：构建/测试/合成输入/帧回放的工具组合，stdout 探针、`Jpx_TextInput` len 契约、坐标反查等本场景专有经验。

本次会话的调试经验已记录在 `docs/debug-跨端图绘指令流.md`，尚未抽象为可复用工作流。若后续需要反复"启动图绘测试台 + 浏览器验证事件回传/远程指针"，可从该文档提炼一条工作流。
