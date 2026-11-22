# docs 索引

- `arch-跨端图绘指令流.md` — 当前架构与实现现状：进程模型、双向数据流、关键设计点与已知限制。
- `debug-跨端图绘指令流.md` — 调试经验：启动验证方法、深挖过的坑与结论、构建环境信息。
- `task-multi-client-cursor.md` — 多客户端彩色指针与资源命令任务的执行记录与验证结果。
- `design-vm-draw-protocol.md` — VM 跨端协议冻结设计：绘制原语、frame envelope、原始输入 schema、Clay 映射。
- `task-vm-instruction-desktop.md` — VM 指令化桌面与窗口管理器的方案定义、Clay 映射与分阶段计划。
- `task-remote-tiles.md` — Remote tiles 协议替换分层合成指令的执行记录、协议 schema 与验证结果。
- `task-jpx.md` — JPX 应用框架与桌面外壳：分层归属、WM/JPX 分界规则、新增组件接口、`jpx.h` 公开接口、分阶段验收。
- `test-timing.md` — 各测试程序的命令、断言数与实测耗时，新增测试时同步登记。
- `design-jpx-layer.md` — 窗口图层化设计（冻结于 2026-11-22）：图层只在 guest 内、每窗口内容缓冲、脏标记表、`BuildFrame` 两阶段、容量与内存代价、不承诺的收益。
- `design-vm-ipc.md` — WMS 与应用进程的线路协议（冻结于 2026-11-22）：责任边界、TCP 帧格式、9+6 条消息、异步内容拉取与 epoch、ProcMan 接口、失败模式、落地步骤。
- `design-vm-sys.md` — 平台能力抽象层（冻结于 2026-11-22）：系统事实与桌面语义的分界、CPU 差分的有状态接口、错误三分类、512 容量依据。
- `design-vm-fs.md` — 文件能力抽象层（冻结于 2026-11-22）：UTF-8 路径硬约束、W 系列 API、定长 POD、错误分类、时间口径、与 IPC 的关系。
