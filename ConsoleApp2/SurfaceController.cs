using Aps反向代理;
using Newtonsoft.Json;
using RemoteForm.DOM;
using RemoteForm.Paint;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using System.Xml;
using System.Xml.Linq;
using static System.Runtime.InteropServices.JavaScript.JSType;
using Timer = System.Timers.Timer;

public class SurfaceController
{
    private Form form;
    private static ServerWebSocket _webSocketServer = new ServerWebSocket(8081);
    private Timer _debounceTimer;
    private const int DesiredFrameRate = 60; // 目标帧率（优化版，压力大可降回 30）
    private const int MillisecondsPerFrame = 1000 / DesiredFrameRate; // 每帧的时间间隔
    private int _isUpdating; // 防止定时器重入

    // 事件分发器
    private readonly EventDispatcher _eventDispatcher = new EventDispatcher();

    public SurfaceController()
    {
        // 初始化防抖计时器
        _debounceTimer = new System.Timers.Timer(MillisecondsPerFrame);
        _debounceTimer.Elapsed += DebounceTimer_Elapsed;

        InitializeOverlay();
        // 初始化事件处理器
        InitializeEventHandlers();
        _debounceTimer.Start();


        Task.Run(() => _webSocketServer.StartAsync(this));
    }
    private async void DebounceTimer_Elapsed(object sender, EventArgs e)
    {
        // 上一帧未处理完则丢弃本次触发，避免 async void 重入导致任务堆积
        if (Interlocked.Exchange(ref _isUpdating, 1) == 1)
            return;
        try
        {
            await UpdateRemote();
        }
        finally
        {
            Interlocked.Exchange(ref _isUpdating, 0);
        }
    }

    private async Task UpdateRemote()
    {
        if (!_webSocketServer.IsRunning)
            return;

        try
        {
            Form form = Utility.GetTopMostForm();
            if (form == null)
            {
                await _webSocketServer.SendCommandsToClient(new List<PaintCommand>(), Guid.Empty);
                return;
            }

            Guid formId = GetFormID(form);
            // 直接遍历控件树生成结构化指令，跳过 XML 中间层
            var paintCommands = FastPaintCommandGenerator.Generate(form);
            // 值相等才跳过（原引用比较永远为 false，导致空转发送）
            if (this.commands != null && paintCommands.SequenceEqual(this.commands))
                return;

            this.commands = paintCommands;
            await _webSocketServer.SendCommandsToClient(paintCommands, formId);
        }
        catch (Exception ex)
        {
            Debug.WriteLine(ex.ToString());
        }
    }

    public static Dictionary<Form, Guid> FormIds = new Dictionary<Form, Guid>();
    private static readonly Dictionary<Form, CursorOverlayPanel> CursorOverlays = new();

    public Guid GetFormID(Form form)
    {
        lock (FormIds)
        {
            if (!FormIds.ContainsKey(form))
            {
                FormIds.Add(form, Guid.NewGuid());
                EnsureCursorOverlay(form);
                //form.VisibleChanged += async (s, e) => await UpdateRemote();
                //form.Activated += async (s, e) => await UpdateRemote();
            }
            return FormIds[form];
        }
    }

    // 为每个窗体创建一个 Win32 分层透明窗口作为指针层，跟随窗体且鼠标穿透
    private void EnsureCursorOverlay(Form form)
    {
        if (CursorOverlays.ContainsKey(form))
            return;
        // GetFormID 可能在后台线程（Timer）调用，窗口必须关联 UI 线程的窗体
        if (form.InvokeRequired)
        {
            form.Invoke(new Action(() => EnsureCursorOverlay(form)));
            return;
        }
        CursorOverlays[form] = new CursorOverlayPanel(form);
    }

    private void InitializeEventHandlers()
    {
        // 注册事件分发器
        _eventDispatcher.AddEventListener("click", HandleMouseEvent);
        _eventDispatcher.AddEventListener("dblclick", HandleMouseEvent);
        _eventDispatcher.AddEventListener("mousemove", HandleMouseEvent);
        _eventDispatcher.AddEventListener("mousedown", HandleMouseEvent);
        _eventDispatcher.AddEventListener("mouseup", HandleMouseEvent);
        _eventDispatcher.AddEventListener("mouseenter", HandleMouseEvent);
        _eventDispatcher.AddEventListener("mouseleave", HandleMouseEvent);
        _eventDispatcher.AddEventListener("contextmenu", HandleMouseEvent);
        _eventDispatcher.AddEventListener("wheel", HandleWheelEvent);
        _eventDispatcher.AddEventListener("keydown", HandleKeyEvent);
        _eventDispatcher.AddEventListener("keyup", HandleKeyEvent);
    }

    // 处理从WebSocket接收的事件
    public void HandleWebSocketEvent(string jsonEvent)
    {

        try
        {
            // 显示远程指针
            ShowRemoteCursor();

            var currentForm = Utility.GetTopMostForm();
            if (currentForm == null)
            {
                return;
            }

            var domEvent = DomEventSerializer.Deserialize(jsonEvent);
            var currentFormId = GetFormID(currentForm);
            if (currentFormId != domEvent.winFormId)
            {
                return;
            }
            _eventDispatcher.DispatchEvent(currentForm, domEvent);


            // 设置3秒后自动隐藏指针（可选）
            //Task.Delay(3000).ContinueWith(t => HideRemoteCursor());
        }
        catch (Exception ex)
        {
            //MessageBox.Show(ex.ToString());
            Debug.WriteLine(ex.ToString());
            //Console.WriteLine($"Failed to handle WebSocket event: {ex.Message}");OnKeyUp
        }
    }

    // 处理鼠标事件（统一处理）
    public void HandleMouseEvent(object currentForm, DomEvent eventData)
    {
        if (!(eventData is MouseEvent mouseEvent)) return;

        form = Utility.GetTopMostForm();
        if (form == null)
            return;
        Guid formId = GetFormID(form);
        if (formId != eventData.winFormId)
        {
            return;
        }
        if (form.InvokeRequired)
        {
            form.Invoke(() => HandleMouseEvent(currentForm, eventData));
            return;
        }
        int x = (int)mouseEvent.ClientX;
        int y = (int)mouseEvent.ClientY;
        int button = (int)mouseEvent.Button; // 0表示左键，1中键，2右键
        var eventType = eventData.Type;
        switch (eventType.ToLower())
        {
            case "click":
                OnClick(x, y, button);
                break;
            case "dblclick":
                OnDoubleClick(x, y, button);
                break;
            case "mousemove":
                form.Invalidate();
                OnMouseMove(x, y);
                break;
            case "mousedown":
                OnMouseDown(x, y, button);
                break;
            case "mouseup":
                OnMouseUp(x, y, button);
                break;
            case "mouseenter":
                OnMouseEnter(x, y);
                break;
            case "mouseleave":
                OnMouseLeave(x, y);
                break;
            case "contextmenu":
                OnContextMenu(x, y, button);
                break;
            default:
                break;
        }
    }

    // 处理滚轮事件
    public void HandleWheelEvent(object currentForm, DomEvent eventData)
    {
        if (!(eventData is WheelEvent wheelEvent)) return;

        int x = (int)wheelEvent.ClientX;
        int y = (int)wheelEvent.ClientY;
        OnMouseWheel(x, y, (int)wheelEvent.DeltaX, (int)wheelEvent.DeltaY);
    }

    // 处理键盘事件
    // 处理键盘事件
    public void HandleKeyEvent(object currentForm, DomEvent eventData)
    {
        if (!(eventData is KeyboardEvent keyboardEvent)) return;

        var keys = new List<string>();

        // 优先使用 key 属性，如果为空则使用 code
        var mainKey = !string.IsNullOrEmpty(keyboardEvent.Key)
            ? keyboardEvent.Key
            : keyboardEvent.Code;

        if (!string.IsNullOrEmpty(mainKey))
        {
            keys.Add(mainKey);
        }

        // 添加修饰键 - 确保正确处理所有键盘事件
        if (keyboardEvent.CtrlKey) keys.Add("Control");
        if (keyboardEvent.ShiftKey) keys.Add("Shift");
        if (keyboardEvent.AltKey) keys.Add("Alt");
        if (keyboardEvent.MetaKey) keys.Add("Win");

        // 处理 keyCode 作为备用方案
        if (keys.Count == 0 && keyboardEvent.KeyCode > 0)
        {
            keys.Add(((Keys)keyboardEvent.KeyCode).ToString());
        }
        string eventType = eventData.Type.ToLower();
        if (eventType == "keydown")
        {
            OnKeyDown(form, keys);
        }
        else if (eventType == "keyup")
        {
            OnKeyUp(form, keys);
        }
    }

    // 标准 WinForm 事件
    public MethodInfo MouseClick;
    private MethodInfo LinkClick;
    public MethodInfo Click;
    public MethodInfo DoubleClick;
    public MethodInfo MouseMove;
    public MethodInfo MouseDown;
    public MethodInfo MouseUp;
    public MethodInfo MouseEnter;
    public MethodInfo MouseLeave;
    public MethodInfo ContextMenu;
    public MethodInfo MouseWheel;
    public MethodInfo KeyDown;
    public MethodInfo KeyUp;

    private Bitmap _cursorBitmap; // 鼠标指针图像
    private Point _cursorHotSpot;
    private Point _lastRemotePoint = Point.Empty; // 上一次远程鼠标位置

    // 命中测试：跳过指针覆盖层等透明覆盖控件，返回最深的真实控件
    private Control HitTest(Point clientPoint)
    {
        if (form == null)
            return null;
        return HitTestRecursive(form, clientPoint);
    }

    private Control HitTestRecursive(Control parent, Point pointInParent)
    {
        // 从最上层（Controls 索引从大到小）向下遍历
        for (int i = parent.Controls.Count - 1; i >= 0; i--)
        {
            var child = parent.Controls[i];
            if (child is OverlayPanel)
                continue;
            if (!child.Visible || !child.Enabled)
                continue;
            // Bounds 相对父容器，pointInParent 也是父容器坐标，两者可直接比较
            if (!child.Bounds.Contains(pointInParent))
                continue;
            var local = new Point(pointInParent.X - child.Left, pointInParent.Y - child.Top);
            var deeper = HitTestRecursive(child, local);
            return deeper ?? child;
        }
        return null;
    }

    // 以下方法由外部调用，触发对应的 WinForm 事件
    protected virtual void OnClick(int x, int y, int button)
    {
        var e = CreateScaledPoint(x, y);
        Control target = HitTest(e);
        if (target == null)
        {
            Click?.Invoke(form, CreateMouseEventArgs(null, x, y, button));
            return;
        }

        Point childPoint = new Point(e.X - target.Left, e.Y - target.Top);
        var mb = button == 0 ? MouseButtons.Left : button == 1 ? MouseButtons.Middle : MouseButtons.Right;

        if (target is LinkLabel link && link.Links.Count > 0)
        {
            InvokeOnMethod(link, "OnClick", new LinkLabelLinkClickedEventArgs(link.Links[0], mb));
        }
        else
        {
            // 调用控件的 OnClick 虚方法，由 WinForms 分发所有订阅的处理器（含闭包 lambda）
            InvokeOnMethod(target, "OnClick", EventArgs.Empty);
        }
    }

    private void InvokeOnMethod(Control target, string methodName, EventArgs args)
    {
        var method = target.GetType().GetMethod(methodName, BindingFlags.NonPublic | BindingFlags.Instance);
        if (method == null)
        {
            Debug.WriteLine($"No {methodName} on {target.GetType().Name}");
            return;
        }
        try
        {
            method.Invoke(target, new object[] { args });
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Invoke {methodName} failed: " + ex.Message);
        }
    }

    protected virtual void OnDoubleClick(int x, int y, int button)
    {
        DoubleClick?.Invoke(form, CreateMouseEventArgs(null, x, y, button));
    }

    private void InitializeOverlay()
    {

        // 加载鼠标指针图像
        _cursorBitmap = new Bitmap("cursor.png"); // 从资源加载或创建
        _cursorHotSpot = new Point(_cursorBitmap.Width / 2, _cursorBitmap.Height / 2); // 设置热点位于图像中心


        var type = typeof(Form);
        this.MouseClick = type.GetMethod("OnMouseClick", BindingFlags.NonPublic | BindingFlags.Instance);
        this.Click = type.GetMethod("OnClick", BindingFlags.NonPublic | BindingFlags.Instance);
        this.DoubleClick = type.GetMethod("OnDoubleClick", BindingFlags.NonPublic | BindingFlags.Instance);
        this.MouseMove = type.GetMethod("OnMouseMove", BindingFlags.NonPublic | BindingFlags.Instance);
        this.MouseDown = type.GetMethod("OnMouseDown", BindingFlags.NonPublic | BindingFlags.Instance);
        this.MouseUp = type.GetMethod("OnMouseUp", BindingFlags.NonPublic | BindingFlags.Instance);
        this.MouseEnter = type.GetMethod("OnMouseEnter", BindingFlags.NonPublic | BindingFlags.Instance);
        this.MouseLeave = type.GetMethod("OnMouseLeave", BindingFlags.NonPublic | BindingFlags.Instance);
        //this.ContextMenu = type.GetMethod("OnContextMenu",BindingFlags.NonPublic|BindingFlags.Instance);
        this.MouseWheel = type.GetMethod("OnMouseWheel", BindingFlags.NonPublic | BindingFlags.Instance);
        this.KeyDown = type.GetMethod("OnKeyDown", BindingFlags.NonPublic | BindingFlags.Instance);
        this.KeyUp = type.GetMethod("OnKeyUp", BindingFlags.NonPublic | BindingFlags.Instance);
    }

    private void InvalidateCursorOverlay()
    {
        if (form == null || !CursorOverlays.TryGetValue(form, out var overlay))
            return;
        overlay.SetCursor(_lastRemotePoint, _showRemoteCursor);
    }

    protected virtual void OnMouseMove(int x, int y)
    {
        // 更新远程鼠标位置
        _lastRemotePoint = CreateScaledPoint(x, y);
        InvalidateCursorOverlay();
        // 触发原生事件
        MouseMove?.Invoke(form, CreateMouseEventArgs(null, x, y, 0));
    }

    public void ShowRemoteCursor()
    {
        this._showRemoteCursor = true;
        InvalidateCursorOverlay();
    }

    public void HideRemoteCursor()
    {
        _showRemoteCursor = false;
        InvalidateCursorOverlay();
    }
    protected virtual void OnMouseDown(int x, int y, int button)
    {
        MouseDown?.Invoke(form, CreateMouseEventArgs(null, x, y, button));
    }

    protected virtual void OnMouseUp(int x, int y, int button)
    {
        MouseUp?.Invoke(form, CreateMouseEventArgs(null, x, y, button));
    }

    protected virtual void OnMouseEnter(int x, int y)
    {
        var target = ControlAt(x, y);
        if (target == null)
        {
            MouseEnter?.Invoke(form, [EventArgs.Empty]);
            return;
        }
        // 触发命中控件自身订阅的 MouseEnter（含 lambda），而非仅窗体
        InvokeOnMethod(target, "OnMouseEnter", EventArgs.Empty);
    }

    protected virtual void OnMouseLeave(int x, int y)
    {
        var target = ControlAt(x, y);
        if (target == null)
        {
            MouseLeave?.Invoke(form, [EventArgs.Empty]);
            return;
        }
        InvokeOnMethod(target, "OnMouseLeave", EventArgs.Empty);
    }

    private Control ControlAt(int x, int y)
    {
        return HitTest(CreateScaledPoint(x, y));
    }

    protected virtual void OnContextMenu(int x, int y, int button)
    {
        ContextMenu?.Invoke(form, CreateMouseEventArgs(form, x, y, button));
    }

    protected virtual void OnMouseWheel(int x, int y, int deltaX, int deltaY)
    {
        var localX = (int)Math.Round(x * dpiScale);
        var localY = (int)Math.Round(y * dpiScale);
        var localDeltaX = (int)Math.Round(deltaX * dpiScale);
        var localDeltaY = (int)Math.Round(deltaY * dpiScale);
        MouseWheel?.Invoke(form, [new MouseEventArgs(MouseButtons.None, 0, localX, localY, localDeltaY)]);
    }

    protected virtual void OnKeyDown(Form form, List<string> keys)
    {
        foreach (var key in keys.Distinct()) // 去重
        {
            if (Enum.TryParse(key, out Keys keyCode))
            {
                KeyDown?.Invoke(form, [new KeyEventArgs(keyCode)]);
            }
        }
    }

    protected virtual void OnKeyUp(Form form, List<string> keys)
    {
        foreach (var key in keys.Distinct()) // 去重
        {
            if (Enum.TryParse(key, out Keys keyCode))
            {
                KeyUp?.Invoke(form, [new KeyEventArgs(keyCode)]);
            }
        }
    }
    public double dpiScale = 1.5;
    private bool _showRemoteCursor;
    private List<PaintCommand> commands;

    private object[] CreateLinkLabelLinkClickedEventArgs(object sender, int button, LinkLabel.Link link)
    {
        var mb = MouseButtons.None;
        if (button == 0)
        {
            mb = MouseButtons.Left;
        }
        else if (button == 1)
        {
            mb = MouseButtons.Middle;
        }
        else
        {
            mb = MouseButtons.Right;
        }
        return [sender, new LinkLabelLinkClickedEventArgs(link, mb)];
    }
    private object[] CreateMouseEventArgs(object target, int x, int y, int button, int delta = 0)
    {
        var mb = MouseButtons.None;
        if (button == 0)
        {
            mb = MouseButtons.Left;
        }
        else if (button == 1)
        {
            mb = MouseButtons.Middle;
        }
        else
        {
            mb = MouseButtons.Right;
        }
        var localX = (int)Math.Round(x * dpiScale);
        var localY = (int)Math.Round(y * dpiScale);
        if (target == null)
            return [new MouseEventArgs(mb, 1, localX, localY, delta)];
        return [target, new MouseEventArgs(mb, 1, localX, localY, delta)];
    }
    public Point CreateScaledPoint(int x, int y)
    {
        var localX = (int)Math.Round(x * dpiScale);
        var localY = (int)Math.Round(y * dpiScale);
        return new Point(localX, localY);
    }
}