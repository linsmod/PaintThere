using Newtonsoft.Json;
using RemoteForm.Paint;
using System.Collections.Generic;
using System.Net.WebSockets;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;
using System.Diagnostics;
using Aps反向代理;

public class ServerWebSocket
{
    private SurfaceController _controller;
    private readonly int _port;
    WebSocketServerImpl impl = new WebSocketServerImpl();

    public bool IsRunning { get { return impl.IsRunning; } }

    public ServerWebSocket(int port)
    {
        _port = port;
    }

    public async Task StartAsync(SurfaceController formOverlay)
    {
        //function drawLabel(text, x, y, width, height, foreColor, backColor, font) {}
        var loadingSplash = JsonConvert.SerializeObject(new string[] {
            $"drawLabel('Loading',10,10,100,100,'#333333','transparent','Microsoft YaHei UI,16,Regular')"
        });
        _controller = formOverlay;
        impl.MessageReceived += (websocket, message) => _controller.HandleWebSocketEvent(message);
        await impl.StartAsync(_port, loadingSplash, CancellationToken.None);
    }

    public async Task SendCommandsToClient(List<PaintCommand> commands, Guid formId)
    {
        await impl.BroadcastAsync(commands, formId, CancellationToken.None);
    }

    public Task BroadcastCursorsAsync()
    {
        return impl.BroadcastCursorsAsync();
    }

    public void Stop()
    {
        impl.Dispose();
    }
}