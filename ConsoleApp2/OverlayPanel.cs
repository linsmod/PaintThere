using System;
using System.Drawing;
using System.Windows.Forms;

public class OverlayPanel : Panel
{
    public OverlayPanel()
    {
        // 初始化样式
        SetStyle(
            ControlStyles.AllPaintingInWmPaint |
            ControlStyles.UserPaint |
            ControlStyles.DoubleBuffer |
            ControlStyles.ResizeRedraw |
            ControlStyles.OptimizedDoubleBuffer,
            true);

        UpdateStyles();

        this.Visible = true;
        //this.Dock = DockStyle.Fill; // 默认填充整个父容器

        // 设置位置与尺寸
        this.Size = new Size(800, 600); // 根据需要调整初始大小
        this.Location = Point.Empty;

        // 设置背景为透明
        this.BackColor = Color.Transparent;
    }
    // 在添加到父容器时自动置于最前面
    protected override void OnParentChanged(EventArgs e)
    {
        base.OnParentChanged(e);
        if (this.Parent != null)
        {
            this.Parent.SizeChanged += (s, e) => { AdjustOverlaySize(); };
            //this.Parent.Controls.SetChildIndex(this, 0);
            AdjustOverlaySize();
        }
    }
    private void AdjustOverlaySize()
    {
        if (this.Parent != null)
        {
            // 调整覆盖层的大小以匹配父容器
            this.Size = this.Parent.ClientSize;
            this.Invalidate(); // 强制重绘
        }
    }
    #region Layered Transparency & Mouse穿透

    protected override CreateParams CreateParams
    {
        get
        {
            CreateParams cp = base.CreateParams;
            //cp.ExStyle |= 0x80000; // WS_EX_LAYERED
            cp.ExStyle |= 0x20;    // WS_EX_TRANSPARENT（仅影响WndProc）
            return cp;
        }
    }

    protected override void WndProc(ref Message m)
    {
        const int WM_NCHITTEST = 0x0084;
        const int HTTRANSPARENT = -1;

        if (m.Msg == WM_NCHITTEST)
        {
            m.Result = (IntPtr)HTTRANSPARENT;
            return;
        }

        base.WndProc(ref m);
    }

    #endregion

    #region 绘制逻辑

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);

        //// 示例：绘制一个半透明的红色矩形覆盖层
        //using (var brush = new SolidBrush(Color.FromArgb(10, Color.Black)))
        //{
        //    e.Graphics.FillRectangle(brush, ClientRectangle);
        //}
    }

    #endregion

    #region 资源清理

    protected override void Dispose(bool disposing)
    {
        base.Dispose(disposing);
    }

    #endregion
}