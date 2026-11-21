using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Forms;

/// <summary>
/// Win32 分层透明窗口，作为远程鼠标指针层。
/// 顶层、鼠标穿透、背景全透明，跟随宿主窗体位置与大小，
/// 不影响宿主窗体自身渲染（不会像子控件那样把窗体盖黑）。
/// </summary>
public sealed class CursorOverlayPanel : IDisposable
{
    private const int WS_EX_LAYERED = 0x80000;
    private const int WS_EX_TRANSPARENT = 0x20;
    private const int WS_EX_TOOLWINDOW = 0x80;
    private const int WS_POPUP = unchecked((int)0x80000000);
    private const int SW_SHOWNOACTIVATE = 4;
    private const byte AC_SRC_OVER = 0;
    private const byte AC_SRC_ALPHA = 1;
    private const int ULW_ALPHA = 0x2;

    private IntPtr _hwnd = IntPtr.Zero;
    private readonly Form _host;
    private bool _disposed;
    private bool _visible;
    private Point _cursorPoint = Point.Empty;

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X; public int Y; public POINT(int x, int y) { X = x; Y = y; } }
    [StructLayout(LayoutKind.Sequential)]
    private struct SIZE { public int CX; public int CY; public SIZE(int cx, int cy) { CX = cx; CY = cy; } }
    [StructLayout(LayoutKind.Sequential)]
    private struct BLENDFUNCTION
    {
        public byte BlendOp; public byte BlendFlags; public byte SourceConstantAlpha; public byte AlphaFormat;
    }

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern IntPtr CreateWindowEx(int ex, string cls, string name, int style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr h);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint flags);
    [DllImport("user32.dll")] private static extern IntPtr DefWindowProc(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Auto)] private static extern ushort RegisterClass(ref WNDCLASS wc);
    [DllImport("user32.dll")] private static extern bool UpdateLayeredWindow(IntPtr h, IntPtr hdc, ref POINT pptDst, ref SIZE psize, IntPtr hdcSrc, ref POINT pptSrc, uint cr, ref BLENDFUNCTION blend, int dwFlags);
    [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private struct WNDCLASS
    {
        public uint style;
        public WndProcDelegate lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        public string lpszMenuName;
        public string lpszClassName;
    }

    private static WndProcDelegate _wndProcDelegate;
    private static bool _classRegistered;
    private static readonly object ClassLock = new object();

    public CursorOverlayPanel(Form host)
    {
        _host = host;
        CreateLayeredWindow();
        _host.LocationChanged += (s, e) => SyncToHost();
        _host.SizeChanged += (s, e) => SyncToHost();
        SyncToHost();
    }

    private void CreateLayeredWindow()
    {
        lock (ClassLock)
        {
            if (!_classRegistered)
            {
                _wndProcDelegate = WndProc;
                var wc = new WNDCLASS
                {
                    lpfnWndProc = _wndProcDelegate,
                    hInstance = Marshal.GetHINSTANCE(typeof(CursorOverlayPanel).Module),
                    lpszClassName = "CursorOverlayLayeredClass"
                };
                RegisterClass(ref wc);
                _classRegistered = true;
            }
        }

        // 作为宿主的 owner 窗口并去掉 WS_EX_TOPMOST：
        // 指针层跟随宿主的 z-order，宿主被其他窗口遮挡时指针也被遮挡
        _hwnd = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW,
            "CursorOverlayLayeredClass", "", WS_POPUP,
            0, 0, 1, 1, _host.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        ShowWindow(_hwnd, SW_SHOWNOACTIVATE);
    }

    private static IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    // 跟随宿主窗体的客户区位置与尺寸
    public void SyncToHost()
    {
        if (_hwnd == IntPtr.Zero || _host == null || _host.IsDisposed)
            return;
        // 宿主最小化或隐藏时，指针层也隐藏，避免遗留残影
        if (_host.WindowState == FormWindowState.Minimized || !_host.Visible)
        {
            ShowWindow(_hwnd, 0); // SW_HIDE
            return;
        }
        var screenPoint = _host.PointToScreen(Point.Empty);
        SetWindowPos(_hwnd, IntPtr.Zero,
            screenPoint.X, screenPoint.Y,
            _host.ClientSize.Width, _host.ClientSize.Height,
            0x0004); // SWP_NOACTIVATE
        ShowWindow(_hwnd, SW_SHOWNOACTIVATE);
        Render();
    }

    // 更新远程指针在宿主客户区中的位置
    public void SetCursor(Point clientPoint, bool visible)
    {
        _cursorPoint = clientPoint;
        _visible = visible;
        if (_host.WindowState == FormWindowState.Minimized || !_host.Visible)
        {
            ShowWindow(_hwnd, 0);
            return;
        }
        ShowWindow(_hwnd, SW_SHOWNOACTIVATE);
        Render();
    }

    private void Render()
    {
        if (_hwnd == IntPtr.Zero || _host == null || _host.IsDisposed)
            return;
        int w = Math.Max(1, _host.ClientSize.Width);
        int h = Math.Max(1, _host.ClientSize.Height);

        IntPtr screenDC = GetDC(IntPtr.Zero);
        IntPtr memDC = CreateCompatibleDC(screenDC);
        IntPtr hBitmap = CreateCompatibleBitmap(screenDC, w, h);
        IntPtr oldBmp = SelectObject(memDC, hBitmap);
        try
        {
            using (var g = Graphics.FromHdc(memDC))
            {
                g.Clear(Color.Transparent);
                if (_visible)
                {
                    var size = Cursors.Default.Size;
                    var rect = new Rectangle(_cursorPoint.X, _cursorPoint.Y, size.Width, size.Height);
                    Cursors.Default.Draw(g, rect);
                }
            }

            // pptDst 是分层窗口在屏幕上的位置，必须设为宿主客户区原点，
            // 传 (0,0) 会把窗口挪到屏幕左上角导致指针位置错位
            var origin = _host.PointToScreen(Point.Empty);
            var pDst = new POINT(origin.X, origin.Y);
            var pSize = new SIZE(w, h);
            var pSrc = new POINT(0, 0);
            var blend = new BLENDFUNCTION
            {
                BlendOp = AC_SRC_OVER,
                BlendFlags = 0,
                SourceConstantAlpha = 255,
                AlphaFormat = AC_SRC_ALPHA
            };
            UpdateLayeredWindow(_hwnd, screenDC, ref pDst, ref pSize, memDC, ref pSrc, 0, ref blend, ULW_ALPHA);
        }
        finally
        {
            SelectObject(memDC, oldBmp);
            DeleteObject(hBitmap);
            DeleteDC(memDC);
            ReleaseDC(IntPtr.Zero, screenDC);
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        if (_hwnd != IntPtr.Zero)
        {
            DestroyWindow(_hwnd);
            _hwnd = IntPtr.Zero;
        }
    }
}
