using System;
using System.Threading.Tasks;
using System.Windows.Forms;

internal static class Program
{
    private static StaticFileServer _staticServer =
        new StaticFileServer("http://localhost:8080/", AppDomain.CurrentDomain.BaseDirectory);

    // 图绘推送控制器：只做控件树 -> 指令流 -> WebSocket
    private static SurfaceController _controller;

    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        _controller = new SurfaceController();

        Application.Run(new TestForm());

        _staticServer.Stop();
    }

    public static Task StartStaticServerAsync() => _staticServer.StartAsync();
}
