using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.CompilerServices;
using System.Windows.Forms;
using Aps反向代理;
using RemoteForm.Paint;

namespace RemoteForm.Paint
{
    /// <summary>
    /// 直接遍历 WinForms 控件树生成结构化指令，跳过 XML 序列化/解析中间层。
    /// 颜色、字体解析结果做缓存，省掉每帧重复的字符串解析。
    /// </summary>
    public static class FastPaintCommandGenerator
    {
        private static readonly ConditionalWeakTable<Font, string> FontCache = new();

        public static List<PaintCommand> Generate(Form form)
        {
            var commands = new List<PaintCommand>();
            var client = form.ClientRectangle;

            commands.Add(new PaintCommand
            {
                Cmd = "setBounds",
                Args = new object[] { client.Width, client.Height, form.Left, form.Top }
            });
            commands.Add(new PaintCommand
            {
                Cmd = "drawBackground",
                Args = new object[] { ToHtml(form.BackColor) },
                Rect = new[] { 0, 0, client.Width, client.Height }
            });

            int index = 0;
            foreach (Control control in EnumerateControls(form))
            {
                var command = Render(control, "c" + index++);
                if (command != null)
                    commands.Add(command);
            }
            return commands;
        }

        private static IEnumerable<Control> EnumerateControls(Control parent)
        {
            foreach (Control control in parent.Controls)
            {
                if (control is OverlayPanel || control.IsDisposed || control.Disposing)
                    continue;
                yield return control;
                foreach (var child in EnumerateControls(control))
                    yield return child;
            }
        }

        private static PaintCommand Render(Control control, string id)
        {
            int x = control.Left;
            int y = control.Top;
            int w = control.Width;
            int h = control.Height;
            if (w <= 0 || h <= 0)
                return null;

            // 结构化指令走 JSON 序列化，文本无需再做 JS 转义
            string text = control.Text ?? string.Empty;
            string fore = ToHtml(control.ForeColor);
            string back = ToHtml(control.BackColor);
            string font = GetFont(control.Font);
            var rect = new[] { x, y, w, h };

            switch (control)
            {
                case Button:
                    return new PaintCommand
                    {
                        Id = id, Cmd = "drawButton", Rect = rect,
                        Args = new object[] { text, x, y, w, h, fore, back, font }
                    };
                case LinkLabel link:
                    return new PaintCommand
                    {
                        Id = id, Cmd = "drawLinkLabel", Rect = rect,
                        Args = new object[]
                        {
                            text, x, y, w, h,
                            link.LinkArea.Start, link.LinkArea.Length,
                            ToHtml(link.LinkColor), ToHtml(link.VisitedLinkColor), font
                        }
                    };
                case Label:
                    return new PaintCommand
                    {
                        Id = id, Cmd = "drawLabel", Rect = rect,
                        Args = new object[] { text, x, y, w, h, fore, back, font }
                    };
                case CheckBox check:
                    return new PaintCommand
                    {
                        Id = id, Cmd = "drawCheckBox", Rect = rect,
                        Args = new object[] { text, x, y, w, h, fore, back, font, check.Checked }
                    };
                case TextBox tb:
                    return new PaintCommand
                    {
                        Id = id, Cmd = "drawEnhancedTextBox", Rect = rect,
                        Args = new object[]
                        {
                            text, x, y, w, h, fore, back, font,
                            tb.Multiline, tb.ReadOnly, tb.BorderStyle.ToString(),
                            tb.UseSystemPasswordChar, tb.TextAlign.ToString().ToLowerInvariant(),
                            tb.PasswordChar == '\0' ? null : tb.PasswordChar.ToString(),
                            tb.CharacterCasing.ToString().ToLowerInvariant(),
                            tb.AcceptsReturn, tb.PlaceholderText ?? string.Empty, control.Enabled
                        }
                    };
                default:
                    return new PaintCommand
                    {
                        Id = id, Cmd = "drawControl", Rect = rect,
                        Args = new object[]
                        {
                            control.GetType().Name.ToLowerInvariant(), text, x, y, w, h, fore, back, font
                        }
                    };
            }
        }

        private static string GetFont(Font font)
        {
            if (font == null)
                return string.Empty;
            if (FontCache.TryGetValue(font, out var cached))
                return cached;
            var value = Utility.BuildFontString(Utility.FontToString(font));
            FontCache.Add(font, value);
            return value;
        }

        private static string ToHtml(Color color)
        {
            if (color.IsSystemColor)
                return "--color-" + color.Name;
            if (color.A < 255)
                return $"rgba({color.R}, {color.G}, {color.B}, {color.A / 255.0:F2})";
            return "#" + color.ToArgb().ToString("X6").Substring(2);
        }
    }
}
