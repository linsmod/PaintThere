# 工作流：双浏览器客户端验证指针互见与事件回传

## 适用场景

验证「跨端图绘指令流」的多客户端指针通道：两个浏览器客户端各自拿到不同颜色，能看到彼此的光标，同时既有控件点击回传未被破坏。适用于修改 `main.js`、`app.js`、`ServerWebSocketImpl.cs` 或 `SurfaceController.cs` 后。

## 前置条件与环境

- 已执行 `dotnet build TestHarness/TestHarness.csproj`；前端资源会由 csproj 链接复制到 `TestHarness/bin/Debug/net9.0-windows/`。
- 本机 `dotnet` 10.0.404，目标框架 `net9.0-windows`；Node 22（原生 `WebSocket` 可用）；`agent-browser 0.27.0`。
- 端口 8080（静态文件）、8081（WebSocket）未被占用。检查：`netstat -ano | findstr "LISTENING" | findstr ":8081"`。

## 相关文件与组件

- 前端：`ConsoleApp2/main.js`（welcome/cursors 分流、`setupCursorReporting`、`renderCursors`）、`app.js`（`loadImg`/`drawImg`、染色缓存）、`index.html`（`cursorCanvas`）。
- 服务端：`ConsoleApp2/ServerWebSocketImpl.cs`（`ClientContext`、`BroadcastCursorsAsync`、`ProcessMessageAsync` 分流）、`ServerWebSocket.cs`、`SurfaceController.cs`（60fps 驱动广播）。
- 验证基准：`docs/debug-跨端图绘指令流.md`。

## 步骤

1. 启动测试台：`Start-Process -FilePath "TestHarness\bin\Debug\net9.0-windows\TestHarness.exe" -PassThru`，记下 PID；确认 8080/8081 监听。
2. 用隔离会话打开两个客户端：`agent-browser --session cursor-a open http://localhost:8080/index.html`、`agent-browser --session cursor-b open ...`。
3. `agent-browser --session cursor-a console`：应看到 `welcome client <id> #F44336`；B 应看到不同颜色（如 `#2196F3`），无错误日志。
4. 让 A 持续上报（单次事件会在 3 秒后因超时隐藏，导致误判）：
   `agent-browser --session cursor-a eval 'window.__t=setInterval(()=>document.dispatchEvent(new MouseEvent("mousemove",{clientX:270,clientY:200,bubbles:true})),200)'`
5. `agent-browser --session cursor-b wait 700`，然后 eval 统计 `cursorCanvas` 非透明像素数与取样颜色；应出现 A 的颜色。
6. 对 B 重复第 4 步，在 A 侧检查 B 的颜色。
7. 既有回传回归：记录 `formCanvas` 按钮区域 `(30,290,160,42)` 的 `getImageData` 像素和，向 `formCanvas` 派发 `click`（`clientX:73,clientY:207,button:0`），等 400ms 后再次统计；数值应变化（按钮文字 `提交 (0)` → `提交 (1)`）。
8. 收尾：分别 `agent-browser --session cursor-a close`、`...cursor-b close`，再用 `Stop-Process -Id <PID>` 停止本次启动的测试台。

## 快速排查经验

- 看不到光标先确认是否超过 3 秒未上报：`visible` 会置 false，必须持续上报。
- 判断消息断点：在页面 eval 注入 `WebSocket.prototype.send` 包装记录上行；注入 `JSON.parse` 包装记录下行非帧消息。
- 服务端独立探针：`node -e 'const ws=new WebSocket("ws://localhost:8081/");ws.onmessage=e=>{const d=JSON.parse(e.data);if(!Array.isArray(d))console.log(d.type,JSON.stringify(d))}'`，确认 welcome/cursors 是否正常。
- 像素统计比截图更可靠：用 `getImageData` 累加通道值或按颜色阈值计数，避免视觉误判。

## 退出条件

两侧像素检查通过、既有点击回传变化、console 无错误，即可关闭会话与进程；否则按 `docs/debug-跨端图绘指令流.md` 继续深挖。
