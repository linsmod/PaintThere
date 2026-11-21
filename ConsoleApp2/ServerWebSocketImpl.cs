using Newtonsoft.Json;
using RemoteForm.Paint;
using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

public class WebSocketServerImpl : IDisposable
{
    private const double DesiredMaxFps = 60;
    private readonly ConcurrentDictionary<WebSocket, ClientContext> _clients = new();
    private readonly ArrayPool<byte> _bufferPool = ArrayPool<byte>.Shared;
    private readonly int _receiveBufferSize = 1024 * 4; // 4KB
    private readonly int _maxMessageSize = 1024 * 1024; // 1MB
    private readonly TimeSpan _keepAliveInterval = TimeSpan.FromSeconds(120);
    private static readonly string[] CursorColors = { "#F44336", "#2196F3", "#4CAF50", "#FF9800", "#9C27B0", "#00BCD4", "#FF5722", "#3F51B5" };
    private const string CursorSvg = "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='24' viewBox='0 0 24 24'><path d='M4,2 L4,20 L9,15 L12.5,22 L15.5,20.5 L12,13.5 L20,13 Z' fill='#fff' stroke='#000' stroke-width='1'/></svg>";
    private static readonly TimeSpan CursorTimeout = TimeSpan.FromSeconds(3);
    private bool _disposed;
    private int _nextClientIndex;
    private string _lastCursorPayload = string.Empty;
    private readonly object _cursorLock = new object();
    private string serverHello = null;
    private string lastMessage = null;

    public async Task StartAsync(int port, string serverHello, CancellationToken cancellationToken = default)
    {
        this.serverHello = serverHello;
        using var listener = new HttpListener();
        listener.Prefixes.Add($"http://localhost:{port}/");
        listener.Start();
        IsRunning = true;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var context = await listener.GetContextAsync();
                if (context.Request.IsWebSocketRequest)
                {
                    _ = HandleWebSocketConnectionAsync(context, cancellationToken);
                }
                else
                {
                    context.Response.StatusCode = 400;
                    context.Response.Close();
                }
            }
        }
        finally
        {
            IsRunning = false;
            listener.Stop();
        }
    }

    public bool IsRunning { get; set; }
    public Action<WebSocket, string> MessageReceived { get; internal set; }

    private async Task HandleWebSocketConnectionAsync(HttpListenerContext httpContext, CancellationToken cancellationToken)
    {
        WebSocket webSocket = null;
        try
        {
            var wsContext = await httpContext.AcceptWebSocketAsync(subProtocol: null);
            webSocket = wsContext.WebSocket;

            var clientContext = new ClientContext(_receiveBufferSize);
            var index = Interlocked.Increment(ref _nextClientIndex) - 1;
            clientContext.Id = Guid.NewGuid().ToString("N");
            clientContext.Color = CursorColors[index % CursorColors.Length];
            if (!_clients.TryAdd(webSocket, clientContext))
            {
                await webSocket.CloseAsync(WebSocketCloseStatus.InternalServerError, "Unable to register client", CancellationToken.None);
                return;
            }

            await ProcessClientAsync(webSocket, clientContext, cancellationToken);
        }
        catch (Exception ex) when (ex is WebSocketException || ex is OperationCanceledException)
        {
            MessageBox.Show(ex.ToString());
            // 正常关闭或WebSocket错误
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.ToString());
            Console.WriteLine($"Unexpected error: {ex}");
        }
        finally
        {
            if (webSocket != null)
            {
                _clients.TryRemove(webSocket, out _);
                webSocket.Dispose();
            }
        }
    }

    private async Task ProcessClientAsync(WebSocket webSocket, ClientContext context, CancellationToken cancellationToken)
    {
        var buffer = _bufferPool.Rent(_receiveBufferSize);
        try
        {
            if (webSocket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
            {
                await SendTextAsync(webSocket, serverHello, cancellationToken);
                var welcome = new
                {
                    type = "welcome",
                    clientId = context.Id,
                    color = context.Color,
                    cursorImage = "data:image/svg+xml;base64," + Convert.ToBase64String(Encoding.UTF8.GetBytes(CursorSvg))
                };
                await SendTextAsync(webSocket, JsonConvert.SerializeObject(welcome), cancellationToken);
            }
            while (webSocket.State == WebSocketState.Open && !cancellationToken.IsCancellationRequested)
            {
                var result = await ReceiveMessageAsync(webSocket, context, buffer, cancellationToken);
                if (result.CloseStatus.HasValue)
                {
                    await webSocket.CloseAsync(result.CloseStatus.Value, result.CloseStatusDescription, CancellationToken.None);
                    break;
                }

                // 处理消息
                if (result.MessageType == WebSocketMessageType.Text && context.ReceiveBuffer.Count > 0)
                {
                    var message = Encoding.UTF8.GetString(context.ReceiveBuffer.ToArray());
                    context.ReceiveBuffer.Clear();
                    if (context.LastTask.Status == TaskStatus.Running)
                    {
                        continue;
                    }
                    context.LastTask = ProcessMessageAsync(webSocket, message, cancellationToken);
                }
            }
        }
        finally
        {
            _bufferPool.Return(buffer);
        }
    }

    private async Task<WebSocketReceiveResult> ReceiveMessageAsync(
        WebSocket webSocket,
        ClientContext context,
        byte[] buffer,
        CancellationToken cancellationToken)
    {
        WebSocketReceiveResult result;
        do
        {
            result = await webSocket.ReceiveAsync(new ArraySegment<byte>(buffer), cancellationToken);

            if (result.MessageType == WebSocketMessageType.Close)
            {
                return result;
            }

            if (result.MessageType != WebSocketMessageType.Text &&
                result.MessageType != WebSocketMessageType.Binary)
            {
                return new WebSocketReceiveResult(
                    0, WebSocketMessageType.Close, true,
                    WebSocketCloseStatus.InvalidMessageType,
                    "Only text or binary messages are supported");
            }

            if (context.ReceiveBuffer.Count + result.Count > _maxMessageSize)
            {
                return new WebSocketReceiveResult(
                    0, WebSocketMessageType.Close, true,
                    WebSocketCloseStatus.MessageTooBig,
                    $"Message exceeds maximum size of {_maxMessageSize} bytes");
            }

            context.ReceiveBuffer.AddRange(new ArraySegment<byte>(buffer, 0, result.Count));

        } while (!result.EndOfMessage);

        return result;
    }
    private void OnMessageReceived(WebSocket webSocket, string message)
    {
        var handler = this.MessageReceived;
        if (handler != null) { handler.Invoke(webSocket, message); }
    }
    private Task ProcessMessageAsync(WebSocket webSocket, string message, CancellationToken cancellationToken)
    {
        try
        {
            if (message.IndexOf("\"type\":\"cursor\"", StringComparison.Ordinal) >= 0)
            {
                var cursor = JsonConvert.DeserializeObject<CursorMessage>(message);
                if (cursor != null && _clients.TryGetValue(webSocket, out var context))
                {
                    context.CursorX = cursor.x;
                    context.CursorY = cursor.y;
                    context.CursorVisible = cursor.visible;
                    context.LastCursorTime = DateTime.UtcNow;
                }
                return Task.CompletedTask;
            }
            OnMessageReceived(webSocket, message);
            return Task.CompletedTask;
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.ToString());
            Console.WriteLine($"Error processing message: {ex}");
            return webSocket.CloseAsync(
                WebSocketCloseStatus.InternalServerError,
                "Error processing message",
                CancellationToken.None);
        }
    }

    private sealed class CursorMessage
    {
        public double x { get; set; }
        public double y { get; set; }
        public bool visible { get; set; }
    }

    public Task BroadcastCursorsAsync()
    {
        var now = DateTime.UtcNow;
        var list = new List<object>();
        foreach (var kv in _clients)
        {
            var c = kv.Value;
            if (string.IsNullOrEmpty(c.Id)) continue;
            list.Add(new
            {
                clientId = c.Id,
                color = c.Color,
                x = c.CursorX,
                y = c.CursorY,
                visible = c.CursorVisible && (now - c.LastCursorTime) < CursorTimeout
            });
        }
        var payload = JsonConvert.SerializeObject(new { type = "cursors", list });
        lock (_cursorLock)
        {
            if (payload == _lastCursorPayload) return Task.CompletedTask;
            _lastCursorPayload = payload;
        }
        var segment = new ArraySegment<byte>(Encoding.UTF8.GetBytes(payload));
        var tasks = new List<Task>();
        foreach (var kv in _clients)
        {
            if (kv.Key.State != WebSocketState.Open) continue;
            tasks.Add(SendCursorSnapshotAsync(kv.Key, kv.Value, segment));
        }
        return Task.WhenAll(tasks);
    }

    private async Task SendCursorSnapshotAsync(WebSocket ws, ClientContext context, ArraySegment<byte> segment)
    {
        await context.CursorSendLock.WaitAsync();
        try
        {
            if (ws.State == WebSocketState.Open)
                await ws.SendAsync(segment, WebSocketMessageType.Text, true, CancellationToken.None);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"Cursor broadcast failed: {ex.Message}");
        }
        finally
        {
            context.CursorSendLock.Release();
        }
    }

    public async Task SendTextAsync(WebSocket webSocket, string text, CancellationToken cancellationToken)
    {
        if (webSocket.State != WebSocketState.Open)
            return;

        _clients.TryGetValue(webSocket, out var context);
        if (context != null)
            await context.CursorSendLock.WaitAsync();
        try
        {
            if (webSocket.State != WebSocketState.Open)
                return;
            var buffer = Encoding.UTF8.GetBytes(text);
            await webSocket.SendAsync(
                new ArraySegment<byte>(buffer),
                WebSocketMessageType.Text,
                true,
                cancellationToken);
        }
        finally
        {
            if (context != null)
                context.CursorSendLock.Release();
        }
    }
    public async Task BroadcastAsync(List<PaintCommand> commands, Guid formId, CancellationToken cancellationToken)
    {
        var now = DateTime.UtcNow;
        var payload = WrapWithUpdateCommands(commands, formId);
        var buffer = Encoding.UTF8.GetBytes(payload);
        var segment = new ArraySegment<byte>(buffer);
        var serialized = new Serialized(formId, segment);

        foreach (var client in _clients)
        {
            if (client.Key.State != WebSocketState.Open)
                continue;

            var context = client.Value;
            // 原实现用 TotalMicroseconds < 100 且 Now/UtcNow 混用，限流几乎失效
            var isRateLimited = (now - context.LastActivity).TotalMilliseconds < 1000.0 / DesiredMaxFps;
            if (isRateLimited)
                continue;
            if (context.LastSent.Equals(serialized))
                continue;
            try
            {
                if (context.LastSendCts != null)
                {
                    context.LastSendCts.Cancel();
                }
                context.LastActivity = now;
                context.LastSent = serialized;
                await SendAsync(formId, segment, true, serialized, client, (context.LastSendCts = new CancellationTokenSource()).Token);
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.ToString());
                Console.WriteLine($"Error broadcasting to client: {ex}");
                _ = RemoveClientAsync(client.Key);
            }
        }
    }

    private string WrapWithUpdateCommands(List<PaintCommand> commands, Guid formId)
    {
        // 直接构造对象后序列化一次；原实现先反序列化再序列化，白做两遍 JSON
        var payload = new List<PaintCommand>(commands.Count + 2)
        {
            new PaintCommand { Cmd = "#beginUpdate", Args = new object[] { formId.ToString() } }
        };
        payload.AddRange(commands);
        payload.Add(new PaintCommand { Cmd = "#endUpdate", Args = new object[] { formId.ToString() } });
        return JsonConvert.SerializeObject(payload);
    }

    private async Task SendAsync(Guid formId, ArraySegment<byte> segment, bool endOfMessage, Serialized serialized, KeyValuePair<WebSocket, ClientContext> client, CancellationToken cancellationToken)
    {
        if (client.Key.State == WebSocketState.Open)
        {
            try
            {
                await client.Key.SendAsync(
                    segment,
                    WebSocketMessageType.Text,
                    endOfMessage,
                    cancellationToken);
                client.Value.LastSent = serialized;
            }
            catch (Exception ex)
            {
                if (client.Key.State == WebSocketState.Aborted)
                {
                    return;
                }
                MessageBox.Show(ex.ToString());
                Console.WriteLine($"Error broadcasting to client: {ex}");
                _ = RemoveClientAsync(client.Key);
            }
        }
    }

    private async Task RemoveClientAsync(WebSocket webSocket)
    {
        _clients.TryRemove(webSocket, out _);
        try
        {
            if (webSocket.State == WebSocketState.Aborted)
                return;
            await webSocket.CloseAsync(
                WebSocketCloseStatus.NormalClosure,
                "Client removed",
                CancellationToken.None);
        }
        finally
        {
            webSocket.Dispose();
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        foreach (var client in _clients)
        {
            client.Key.Dispose();
        }
        _clients.Clear();
    }

    private class ClientContext
    {
        public List<byte> ReceiveBuffer { get; }
        public DateTime LastActivity { get; set; }
        public CancellationTokenSource LastSendCts = new CancellationTokenSource();

        public Task LastTask = Task.CompletedTask;

        public string Id { get; set; }
        public string Color { get; set; }
        public double CursorX { get; set; }
        public double CursorY { get; set; }
        public bool CursorVisible { get; set; }
        public DateTime LastCursorTime { get; set; }
        public readonly SemaphoreSlim CursorSendLock = new SemaphoreSlim(1, 1);

        public Serialized LastSent = new Serialized(Guid.Empty, ArraySegment<byte>.Empty);
        public ClientContext(int initialBufferSize)
        {
            ReceiveBuffer = new List<byte>(initialBufferSize);
            LastActivity = DateTime.UtcNow;
        }
    }

    public class Serialized : IEquatable<Serialized>
    {
        public Serialized(Guid formId, ArraySegment<byte> segment)
        {
            FormID = formId;
            LastSegment = segment;
        }

        public IEnumerable<byte> LastSegment { get; }
        public Guid FormID { get; }

        // 实现 IEquatable<T>
        public bool Equals(Serialized other)
        {
            if (other is null)
                return false;

            return FormID == other.FormID && SequencesEqual(LastSegment, other.LastSegment);
        }

        // 重写 Object.Equals
        public override bool Equals(object obj)
        {
            return Equals(obj as Serialized);
        }

        // 重写 GetHashCode
        public override int GetHashCode()
        {
            unchecked // 忽略溢出
            {
                int hash = 17;
                hash = hash * 23 + FormID.GetHashCode();

                foreach (var b in LastSegment)
                {
                    hash = hash * 23 + b.GetHashCode();
                }

                return hash;
            }
        }

        // 比较两个 byte 序列是否相等
        private static bool SequencesEqual(IEnumerable<byte> first, IEnumerable<byte> second)
        {
            if (first is null || second is null)
                return first == second;

            var e1 = first.GetEnumerator();
            var e2 = second.GetEnumerator();

            while (e1.MoveNext() && e2.MoveNext())
            {
                if (e1.Current != e2.Current)
                    return false;
            }

            return !(e1.MoveNext() || e2.MoveNext());
        }

        // 可选：重载 == / != 运算符
        public static bool operator ==(Serialized left, Serialized right)
        {
            return Equals(left, right);
        }

        public static bool operator !=(Serialized left, Serialized right)
        {
            return !Equals(left, right);
        }
    }
}