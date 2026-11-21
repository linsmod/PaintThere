using System;
using System.Drawing;
using System.Windows.Forms;

/// <summary>
/// 透明、鼠标穿透的覆盖层，用于在 WinForms 窗体最上层绘制远程鼠标指针。
/// 继承 OverlayPanel 以被指令生成器跳过（不推送到浏览器）。
/// </summary>
public class CursorOverlayPanel : OverlayPanel
{
    public CursorOverlayPanel()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer, true);
        Dock = DockStyle.Fill;
        BackColor = Color.Transparent;
    }

    protected override CreateParams CreateParams
    {
        get
        {
            var cp = base.CreateParams;
            // 去掉 WS_EX_TRANSPARENT：它会让本控件绘制在兄弟控件之下，
            // 我们要绘制在最上层；鼠标穿透已由 OverlayPanel.WndProc 的 HTTRANSPARENT 实现
            cp.ExStyle &= ~0x20;
            return cp;
        }
    }

    protected override void OnPaintBackground(PaintEventArgs e)
    {
        // 保持透明，不填充背景
    }
}
