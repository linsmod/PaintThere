using System;
using System.Drawing;
using System.Windows.Forms;

public class TestForm : Form
{
    private readonly System.Windows.Forms.Timer _clockTimer = new();

    public TestForm()
    {
        Text = "图绘指令流 · 测试台（无推送业务）";
        Name = "TestForm";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(760, 520);
        BackColor = Color.White;

        var title = new Label
        {
            Name = "titleLabel", Text = "跨端图绘指令流测试", Location = new Point(30, 24),
            Size = new Size(500, 32), Font = new Font("Microsoft YaHei UI", 16F, FontStyle.Bold),
            ForeColor = Color.FromArgb(30, 90, 160)
        };
        Controls.Add(title);

        var hint = new Label
        {
            Name = "hintLabel",
            Text = "浏览器打开 http://localhost:8080/index.html 查看实时画面；点击按钮/勾选可测试事件回传",
            Location = new Point(30, 62), Size = new Size(700, 24),
            Font = new Font("Microsoft YaHei UI", 9F), ForeColor = Color.Gray
        };
        Controls.Add(hint);

        var clockLabel = new Label
        {
            Name = "clockLabel", Text = "计数: 0", Location = new Point(30, 110),
            Size = new Size(300, 28), Font = new Font("Microsoft YaHei UI", 11F),
            ForeColor = Color.DarkSlateGray
        };
        Controls.Add(clockLabel);

        var nameLabel = new Label
        {
            Name = "nameLabel", Text = "姓名:", Location = new Point(30, 155), Size = new Size(60, 28)
        };
        Controls.Add(nameLabel);

        var nameBox = new TextBox
        {
            Name = "nameBox", Text = "张三", PlaceholderText = "请输入姓名",
            Location = new Point(95, 152), Size = new Size(220, 30)
        };
        Controls.Add(nameBox);

        var agreeCheck = new CheckBox
        {
            Name = "agreeCheck", Text = "同意协议", Checked = true,
            Location = new Point(30, 200), Size = new Size(160, 28)
        };
        Controls.Add(agreeCheck);

        var link = new LinkLabel
        {
            Name = "detailLink", Text = "查看详情", Location = new Point(30, 240), Size = new Size(120, 28)
        };
        link.Links.Add(0, 4);
        Controls.Add(link);

        var submitButton = new Button
        {
            Name = "submitButton", Text = "提交 (0)", Location = new Point(30, 290),
            Size = new Size(160, 42), BackColor = Color.FromArgb(45, 130, 210), ForeColor = Color.White
        };
        submitButton.Click += (s, e) =>
        {
            _clicks++;
            submitButton.Text = $"提交 ({_clicks})";
        };
        Controls.Add(submitButton);

        var panel = new Panel
        {
            Name = "infoPanel", Location = new Point(340, 110), Size = new Size(390, 300),
            BackColor = Color.FromArgb(245, 248, 252), BorderStyle = BorderStyle.FixedSingle
        };
        panel.Controls.Add(new Label
        {
            Name = "panelLabel", Text = "嵌套面板内容", Location = new Point(20, 20),
            Size = new Size(300, 26), Font = new Font("Microsoft YaHei UI", 11F, FontStyle.Bold)
        });
        panel.Controls.Add(new Button
        {
            Name = "panelButton", Text = "面板内按钮", Location = new Point(20, 60), Size = new Size(140, 38)
        });
        panel.Controls.Add(new CheckBox
        {
            Name = "panelCheck", Text = "面板内复选", Location = new Point(20, 120), Size = new Size(160, 28)
        });
        Controls.Add(panel);

        int ticks = 0;
        _clockTimer.Interval = 500;
        _clockTimer.Tick += (s, e) =>
        {
            ticks++;
            clockLabel.Text = $"计数: {ticks}  时间: {DateTime.Now:HH:mm:ss}";
        };
        _clockTimer.Start();

        Load += async (s, e) => await Program.StartStaticServerAsync();
        FormClosing += (s, e) => _clockTimer.Stop();
    }

    private int _clicks;

    protected override void Dispose(bool disposing)
    {
        if (disposing)
            _clockTimer.Dispose();
        base.Dispose(disposing);
    }
}
