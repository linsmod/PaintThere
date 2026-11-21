using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing.Imaging;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading.Tasks;

namespace Aps反向代理
{
    internal class Utility
    {

        #region Helper Classes for Parsing

        public static class PointConverter
        {
            public static bool TryParse(string value, out Point point)
            {
                try
                {
                    var parts = value.Split(',');
                    point = new Point(int.Parse(parts[0]), int.Parse(parts[1]));
                    return true;
                }
                catch
                {
                    point = Point.Empty;
                    return false;
                }
            }
        }

        public static class SizeConverter
        {
            public static bool TryParse(string value, out Size size)
            {
                try
                {
                    var parts = value.Split(',');
                    size = new Size(int.Parse(parts[0]), int.Parse(parts[1]));
                    return true;
                }
                catch
                {
                    size = Size.Empty;
                    return false;
                }
            }
        }

        public static class LinkAreaConverter
        {
            public static bool TryParse(string value, out LinkArea area)
            {
                try
                {
                    var parts = value.Split(',');
                    area = new LinkArea(int.Parse(parts[0]), int.Parse(parts[1]));
                    return true;
                }
                catch
                {
                    area = new LinkArea();
                    return false;
                }
            }
        }

        #endregion

        #region Utility Methods for Fonts and Colors

        public static string ColorToRgba(Color color)
        {
            float alpha = color.A / 255f;
            return $"rgba({color.R}, {color.G}, {color.B}, {alpha:F2})";
        }

        public static string ColorToString(Color color)
        {
            if (color.IsNamedColor)
                return color.Name;
            else
            {
                float alpha = color.A / 255f;
                return $"rgba({color.R}, {color.G}, {color.B}, {alpha:F2})";
            }
        }

        public static Color ColorFromString(string value)
        {
            if (value.StartsWith("#"))
                return ColorTranslator.FromHtml(value);

            if (value.Contains(","))
            {
                var parts = value.Substring(5, value.Length - 6).Split(',');
                byte r = byte.Parse(parts[0]);
                byte g = byte.Parse(parts[1]);
                byte b = byte.Parse(parts[2]);
                float a = float.Parse(parts[3]) * 255;
                return Color.FromArgb((int)a, r, g, b);
            }

            return Color.FromName(value);
        }

        public static string FontToString(Font font)
        {
            return $"{font.FontFamily.Name},{font.SizeInPoints},{font.Style}";
        }

        public static Font FontFromString(string value)
        {
            var parts = value.Split(',');
            string fontFamily = parts[0];
            float fontSize = float.Parse(parts[1]);
            FontStyle fontStyle = FontStyle.Regular;

            if (parts.Length > 2)
            {
                fontStyle = (FontStyle)Enum.Parse(typeof(FontStyle), parts[2]);
            }

            return new Font(fontFamily, fontSize, fontStyle);
        }

        #endregion



        public static Point ParsePoint(string value)
        {
            if (string.IsNullOrEmpty(value))
                return new Point(0, 0);

            var parts = value.Split(',');
            return new Point(int.Parse(parts[0]), int.Parse(parts[1]));
        }

        public static Size ParseSize(string value)
        {
            if (string.IsNullOrEmpty(value))
                return new Size(100, 20);

            var parts = value.Split(',');
            return new Size(int.Parse(parts[0]), int.Parse(parts[1]));
        }

        public static string BuildFontString(string fontStr)
        {
            if (string.IsNullOrEmpty(fontStr))
                return "";

            var fontParts = fontStr.Split(',');
            if (fontParts.Length >= 3)
            {
                string fontWeight = "normal";
                string fontStyle = "normal";
                string fontFamily = fontParts[0];
                double fontSizePx = double.Parse(fontParts[1]);

                // Determine font weight and style based on the third part
                string[] styles = fontParts[2].Split(new char[] { ' ', ',' }, StringSplitOptions.RemoveEmptyEntries);
                foreach (var style in styles)
                {
                    if (style.Equals("Bold", StringComparison.OrdinalIgnoreCase))
                        fontWeight = "bold";
                    else if (style.Equals("Italic", StringComparison.OrdinalIgnoreCase))
                        fontStyle = "italic";
                }

                // Ensure the font family is enclosed in single quotes
                if (!fontFamily.StartsWith("'") && !fontFamily.EndsWith("'"))
                {
                    fontFamily = $"'{fontFamily}'";
                }

                // Convert font size from pt to px (1pt ≈ 1.33px)
                double fontSizePt = Math.Max(12, Math.Round(fontSizePx * 1, 2)); // Minimum 12pt
                double fontSizePxConverted = Math.Round(fontSizePt * 1.5);

                // Only include one 'normal' if both are not needed
                if (fontWeight == "normal" && fontStyle == "normal")
                {
                    return $"{fontSizePxConverted}pt {fontFamily}";
                }
                else if (fontWeight == "normal")
                {
                    return $"{fontStyle} {fontSizePxConverted}pt {fontFamily}";
                }
                else if (fontStyle == "normal")
                {
                    return $"{fontWeight} {fontSizePxConverted}pt {fontFamily}";
                }
                else
                {
                    return $"{fontWeight} {fontStyle} {fontSizePxConverted}pt {fontFamily}";
                }
            }
            return "";
        }

        public static string ColorToHtml(string value)
        {
            // 检查是否是 CSS 变量
            if (value.StartsWith("--"))
            {
                return value;
            }

            // 已是十六进制颜色，直接返回
            if (value.StartsWith("#"))
                return value;
            if (value.StartsWith("rgba"))
                return value;

            // 是 RGBA 数字字符串：如 "255,0,0,128"
            if (value.Contains(","))
            {
                var parts = value.Split(',')
                    .Select(p => p.Trim())
                    .Where(p => !string.IsNullOrEmpty(p))
                    .ToList();

                if (parts.Count == 4 &&
                    byte.TryParse(parts[0], out byte a) &&
                    byte.TryParse(parts[1], out byte r) &&
                    byte.TryParse(parts[2], out byte g) &&
                    byte.TryParse(parts[3], out byte b))
                {
                    return $"rgba({r}, {g}, {b}, {a / 255.0:F2})";
                }
            }

            // 检查是否是已知的系统颜色名（比如 ControlText、ButtonFace 等）
            try
            {
                // 尝试解析为系统颜色名
                var color = Color.FromName(value);
                if (!color.IsSystemColor && color.A < 255)
                {
                    // 非系统色且有透明度，转为 rgba
                    return $"rgba({color.R}, {color.G}, {color.B}, {color.A / 255.0:F2})";
                }
                else if (!color.IsSystemColor)
                {
                    // 非系统颜色但无透明度，转为 HEX
                    return "#" + color.ToArgb().ToString("X6").Substring(2);
                }
                else
                {
                    // 是系统颜色，使用 CSS 变量引用
                    return $"--color-{value}";
                }
            }
            catch
            {
                // 解析失败，默认黑色
                return "#000000";
            }
        }
        /// <summary>
        /// 保存图片并返回哈希文件名（格式：图片哈希.扩展名）
        /// </summary>
        public static string SaveImageToFile(Image image, string outputDirectory)
        {
            if (image == null || string.IsNullOrEmpty(outputDirectory))
                return null;

            // 确保目录存在
            if (!Directory.Exists(outputDirectory))
                Directory.CreateDirectory(outputDirectory);

            // 计算图片的MD5哈希值
            string hash;
            using (var ms = new MemoryStream())
            {
                image.Save(ms, image.RawFormat);
                byte[] imageData = ms.ToArray();
                hash = BitConverter.ToString(System.Security.Cryptography.MD5.Create().ComputeHash(imageData))
                    .Replace("-", "").ToLower(); // 32位小写哈希（如 "5d41402abc4b2a76b9719d911017c592"）
            }

            // 生成文件名（哈希 + 扩展名）
            string extension = image.RawFormat.Equals(ImageFormat.Png) ? ".png" : ".jpg";
            string fileName = $"{hash}{extension}";
            string filePath = Path.Combine(outputDirectory, fileName);

            // 如果文件不存在，则保存
            if (!File.Exists(filePath))
            {
                image.Save(filePath, image.RawFormat);
            }

            return fileName;
        }
        public static Delegate[] GetEventHandlers(Control control)
        {
            PropertyInfo _eventsProp = typeof(Component).GetProperty("Events", BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.GetProperty);
            if (_eventsProp == null) return [];

            object _events = _eventsProp.GetValue(control);
            FieldInfo _entryField = typeof(EventHandlerList).GetField("_head", BindingFlags.NonPublic | BindingFlags.Instance);
            object _entry = _entryField.GetValue(_events);
            if (_entry == null)
                return [];
            FieldInfo _handlerField = _entry.GetType().GetField("_handler", BindingFlags.NonPublic | BindingFlags.Instance);
            Delegate d = (Delegate)_handlerField.GetValue(_entry);
            if (d != null)
            {
                // 返回 Delegate 本身（保留 Target），闭包 lambda 的 Target 是编译器生成的闭包实例
                return d.GetInvocationList();
            }
            return [];
        }

        public static Form GetTopMostForm()
        {
            // 获取所有打开的窗口
            var openForms = Application.OpenForms.Cast<Form>().ToList();
            if (openForms.Count == 1)
                return openForms[0];
            var activeForm = openForms.FirstOrDefault(f => IsActived(f));
            if (activeForm != null)
            {
                return activeForm;
            }

            return null;
        }
        private static PropertyInfo activeField = typeof(Form).GetProperty("Active", BindingFlags.NonPublic | BindingFlags.Instance);
        private static bool IsActived(Form f)
        {
            if (f.InvokeRequired)
            {
                return (bool)f.Invoke(IsActived, f);
            }
            return f.Visible &&  (bool)activeField.GetValue(f);
        }
    }
}

