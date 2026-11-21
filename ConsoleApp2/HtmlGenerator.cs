using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using System.Text;

public static class HtmlLayoutGenerator
{
    public static string Generate(string xmlFilePath)
    {
        var doc = XDocument.Load(xmlFilePath);
        var formElement = doc.Element("Form");

        StringBuilder html = new StringBuilder();
        html.AppendLine("<!DOCTYPE html>");
        html.AppendLine("<html lang=\"en\">");
        html.AppendLine("<head>");
        html.AppendLine("    <meta charset=\"UTF-8\">");
        html.AppendLine("    <title>Form Layout Preview</title>");
        html.AppendLine("    <style>");
        html.AppendLine("        body { position: relative; margin: 0; padding: 0; }");
        html.AppendLine("        .control { box-sizing: border-box; padding: 4px; overflow: hidden; }");
        html.AppendLine("        .button { border: 1px solid #ccc; text-align: center; line-height: 28px; cursor: default; }");
        html.AppendLine("        .textbox { border: 1px solid #ccc; padding: 4px; }");
        html.AppendLine("        .label { font-weight: bold; }");
        html.AppendLine("        .panel { border: 1px solid #ccc; padding: 4px; background-color: #f9f9f9; }");
        html.AppendLine("    </style>");
        html.AppendLine("</head>");
        html.AppendLine("<body>");

        foreach (var controlEl in formElement.Elements("Control"))
        {
            html.Append(RenderControl(controlEl));
        }
        html.Append("<script type=\"application/javascript\" src=\"systemColors.js\"></script>");
        html.AppendLine("</body>");
        html.AppendLine("</html>");

        return html.ToString();
    }

    private static string RenderControl(XElement el)
    {
        var sb = new StringBuilder();
        string type = (string)el.Attribute("Type") ?? "Unknown";
        string name = (string)el.Attribute("Name") ?? "Unnamed";
        string text = (string)el.Attribute("Text") ?? "";
        string locationStr = (string)el.Attribute("Location") ?? "0,0";
        string sizeStr = (string)el.Attribute("Size") ?? "100,20";
        bool enabled = (bool?)el.Attribute("Enabled") ?? true;
        string foreColor = (string)el.Attribute("ForeColor") ?? "#000000";
        string backColor = (string)el.Attribute("BackColor") ?? "#ffffff";
        string fontStr = (string)el.Attribute("Font") ?? "";

        // 解析位置和尺寸
        Point location = ParsePoint(locationStr);
        Size size = ParseSize(sizeStr);

        // 构建样式字符串
        StringBuilder style = new StringBuilder();
        style.AppendFormat("position: absolute; ");
        style.AppendFormat("left: {0}px; top: {1}px; ", location.X, location.Y);
        style.AppendFormat("width: {0}px; height: {1}px; ", size.Width, size.Height);

        if (!string.IsNullOrEmpty(backColor))
            style.AppendFormat("background-color: {0}; ", ColorToHtml(backColor));

        if (!string.IsNullOrEmpty(foreColor))
            style.AppendFormat("color: {0}; ", ColorToHtml(foreColor));

        if (!string.IsNullOrEmpty(fontStr))
        {
            var fontParts = fontStr.Split(',');
            if (fontParts.Length >= 3)
            {
                style.AppendFormat("font-family: '{0}'; font-size: {1}pt; ", fontParts[0], fontParts[1]);
                if (fontParts.Length > 2 && !string.IsNullOrEmpty(fontParts[2]))
                {
                    style.AppendFormat("font-weight: {0}; ", fontParts[2].ToLower().Contains("bold") ? "bold" : "normal");
                    style.AppendFormat("font-style: {0}; ", fontParts[2].ToLower().Contains("italic") ? "italic" : "normal");
                }
            }
        }

        // 构建 HTML 元素
        string tagName = "div";
        string cssClass = "control";

        switch (type.ToLower())
        {
            case "button":
                cssClass += " button";
                tagName = "button";
                break;
            case "textbox":
                tagName = "input";
                cssClass += " textbox";
                break;
            case "label":
                tagName = "label";
                cssClass += " label";
                break;
            case "panel":
                cssClass += " panel";
                break;
            default:
                cssClass += " unknown";
                break;
        }

        sb.AppendFormat("<{0} class=\"{1}\" style=\"{2}\"", tagName, cssClass, style.ToString().TrimEnd(' ', ';'));

        if (tagName == "input")
        {
            sb.AppendFormat(" type=\"text\" value=\"{0}\" />", text);
        }
        else if (tagName == "button")
        {
            sb.Append(">");
            sb.Append(text);
            sb.AppendFormat("</{0}>", tagName);
        }
        else
        {
            sb.Append(">");
            sb.Append(text);
            sb.AppendFormat("</{0}>", tagName);
        }

        sb.AppendLine();

        // 子控件递归
        foreach (var childEl in el.Elements("Control"))
        {
            sb.Append(RenderControl(childEl));
        }

        return sb.ToString();
    }

    private static Point ParsePoint(string value)
    {
        if (string.IsNullOrEmpty(value))
            return new Point(0, 0);

        var parts = value.Split(',');
        return new Point(int.Parse(parts[0]), int.Parse(parts[1]));
    }

    private static Size ParseSize(string value)
    {
        if (string.IsNullOrEmpty(value))
            return new Size(100, 20);

        var parts = value.Split(',');
        return new Size(int.Parse(parts[0]), int.Parse(parts[1]));
    }

    private static string ColorToHtml(string value)
    {
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
                return $"rgba({color.R}, {color.G}, {color.B}, {color.A / 255.0:F2})";
            }
            else
            {
                // 是系统颜色，使用 CSS 变量引用
                return $"var(--color-{value})";
            }
        }
        catch
        {
            // 解析失败，默认黑色
            return "#000000";
        }
    }

    public static void SaveToFile(string xmlPath, string htmlPath)
    {
        string html = Generate(xmlPath);
        File.WriteAllText(htmlPath, html);
    }
}