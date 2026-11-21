using System;
using System.IO;
using System.Net;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Concurrent;

public class StaticFileServer : IDisposable
{
    // ... 其他成员保持不变 ...

    private readonly CancellationTokenSource _cts = new();
    private readonly string _rootDirectory;
    private readonly HttpListener _listener;
    private readonly string _url;
    private readonly ConcurrentDictionary<string, byte[]> _fileCache = new();
    private readonly SemaphoreSlim _concurrentLimiter = new(Environment.ProcessorCount * 2);
    public StaticFileServer(string url, string rootDirectory)
    {
        _url = url;
        _rootDirectory = Path.GetFullPath(rootDirectory);
        _listener = new HttpListener();
        _listener.Prefixes.Add(url);
    }
    public async Task StartAsync()
    {
        _listener.Start();
        Console.WriteLine($"Server started (Max concurrency: {_concurrentLimiter.CurrentCount})");

        try
        {
            while (!_cts.IsCancellationRequested)
            {
                var context = await _listener.GetContextAsync().WaitAsync(_cts.Token);
                _ = ProcessRequestAsync(context, _cts.Token); // 传递取消令牌
            }
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Server shutdown requested");
        }
    }

    private async Task ProcessRequestAsync(HttpListenerContext context, CancellationToken ct)
    {
        await _concurrentLimiter.WaitAsync(ct);
        try
        {
            var request = context.Request;
            var response = context.Response;

            // 检查客户端是否已断开连接
            if (ct.IsCancellationRequested || !context.Request.IsLocal)
            {
                response.StatusCode = 499; // Client Closed Request
                response.Close();
                return;
            }

            // 绑定客户端断开事件
            using var registration = ct.Register(() => response.Abort());

            try
            {
                var path = request.Url!.AbsolutePath.TrimStart('/');
                var physicalPath = Path.Combine(_rootDirectory, path);

                // 异步检查文件存在
                if (!File.Exists(physicalPath))
                {
                    response.StatusCode = 404;
                    response.Close();
                    return;
                }

                // 使用FileStream异步读取（支持取消）
                await using (var fs = new FileStream(
                    physicalPath,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.Read,
                    bufferSize: 4096,
                    useAsync: true))
                {
                    response.ContentType = GetContentType(Path.GetExtension(physicalPath));
                    response.ContentLength64 = fs.Length;

                    // 分段读取并检查取消状态
                    var buffer = new byte[64 * 1024];
                    int bytesRead;
                    while ((bytesRead = await fs.ReadAsync(buffer, ct)) > 0)
                    {
                        await response.OutputStream.WriteAsync(buffer.AsMemory(0, bytesRead), ct);
                        await response.OutputStream.FlushAsync(ct);
                    }
                }
            }
            catch (OperationCanceledException)
            {
                Console.WriteLine($"Request cancelled: {request.Url}");
                response.Abort();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error processing {request.Url}: {ex.Message}");
                response.StatusCode = 500;
            }
        }
        finally
        {
            _concurrentLimiter.Release();
        }
    }
    private string GetContentType(string extension) => extension switch
    {
        ".js" => "application/javascript",
        ".html" => "text/html",
        ".css" => "text/css",
        _ => "application/octet-stream"
    };

    public void Stop()
    {
        _cts.Cancel(); // 触发所有请求取消
        _listener.Stop();
    }

    public void Dispose()
    {
        _cts.Dispose();
        _listener.Close();
        _concurrentLimiter.Dispose();
    }
}