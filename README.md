# PAINT-THERE
一种绘制协议，用于控制远端渲染器如何绘制内容。

# WM
vm-desktop 是基于PAINT-THERE协议实现的一种桌面环境。管理窗口的DisplayList和用于远端渲染的资源。


# 设计准则
- 协议不得显式暴露桌面结构与窗口的业务逻辑。
- 用单向协议提交`绘制`相关的一切事务，渲染器只是执行器。
- SSE下发，WS回传输入事件，各自独立通道。

# JPX
Just Paint Things, 用于开发支持远端渲染的库
本地Layout，生成DisplayList，提交到任何远端执行。


# 三方库
- Luja 一个能运行java子集的VM。基于Lua，作为脚本引擎。



