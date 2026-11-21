using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using System.Text;
using Aps反向代理;

public static class CanvasLayoutGenerator
{
    public static string Generate(string xmlFilePath)
    {
        var doc = XDocument.Load(xmlFilePath);
        var formElement = doc.Element("Form");

        // 获取表单属性
        string sizeStr = (string)formElement.Attribute("Size");
        var sizeParts = sizeStr.Split(',');
        int width = int.Parse(sizeParts[0]);
        int height = int.Parse(sizeParts[1]);
        string backColor = (string)formElement.Attribute("BackColor") ?? "transparent";

        StringBuilder html = new StringBuilder();
        html.AppendLine("<!DOCTYPE html>");
        html.AppendLine("<html lang=\"en\">");
        html.AppendLine("<head>");
        html.AppendLine("    <meta charset=\"UTF-8\">");
        html.AppendLine($"    <title>{formElement.Attribute("Text")?.Value ?? "Form"} Preview</title>");
        html.AppendLine("    <style>");
        html.AppendLine("        body { margin: 0; padding: 0; overflow: hidden; }");
        html.AppendLine("        #form-container { position: relative; }");
        html.AppendLine("        #formCanvas { display: block; background-color: " + backColor + "; }");
        html.AppendLine("        @font-face {");
        html.AppendLine("            font-family: 'Microsoft YaHei UI';");
        html.AppendLine("            src: local('Microsoft YaHei UI'), local('MicrosoftYaHei_UI');");
        html.AppendLine("        }");
        html.AppendLine("    </style>");
        html.AppendLine("</head>");
        html.AppendLine("<body>");
        html.AppendLine("    <div id=\"form-container\">");
        html.AppendLine($"        <canvas id=\"formCanvas\"></canvas>");
        html.AppendLine("    </div>");

        // 引用外部 JavaScript 文件（模块化方式）
        html.AppendLine("    <script type=\"module\">");
        html.AppendLine("        import { initCanvas, drawLabel,drawButton,drawCheckBox,drawLinkLabel,drawEnhancedTextBox,drawControl} from './app.js';");
        html.AppendLine($"        initCanvas('formCanvas','{backColor}',{width},{height})");

        // 渲染所有控件
        foreach (var controlEl in formElement.Elements("Control"))
        {
            html.Append(RenderControl(controlEl));
        }

        html.AppendLine("    </script>");
        html.AppendLine("</body>");
        html.AppendLine("</html>");

        return html.ToString();
    }

    private static string RenderControl(XElement el)
    {
        var sb = new StringBuilder();
        string type = (string)el.Attribute("Type") ?? "Unknown";
        string name = (string)el.Attribute("Name") ?? "";
        string text = (string)el.Attribute("Text") ?? "";
        string locationStr = (string)el.Attribute("Location") ?? "0,0";
        string sizeStr = (string)el.Attribute("Size") ?? "100,20";
        bool enabled = (bool?)el.Attribute("Enabled") ?? true;
        string foreColor = (string)el.Attribute("ForeColor") ?? "#000000";
        string backColor = (string)el.Attribute("BackColor") ?? "#ffffff";
        string fontStr = (string)el.Attribute("Font") ?? "9pt 'Microsoft YaHei UI'";

        // 解析位置和尺寸
        Point location = Utility.ParsePoint(locationStr);
        Size size = Utility.ParseSize(sizeStr);

        // 构建字体字符串
        string font = Utility.BuildFontString(fontStr);

        // 转义文本中的特殊字符
        text = EscapeJsString(text);

        // 根据控件类型调用不同的渲染函数
        switch (type.ToLower())
        {
            case "button":
                sb.AppendLine($"drawButton('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\");");
                break;

            case "label":
                sb.AppendLine($"drawLabel('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\");");
                break;

            case "textbox":
                bool multiline = (bool?)el.Attribute("Multiline") ?? false;
                bool readOnly = (bool?)el.Attribute("ReadOnly") ?? false;
                string borderStyle = (string)el.Attribute("BorderStyle") ?? "Fixed3D";
                bool useSystemPasswordChar = (bool?)el.Attribute("UseSystemPasswordChar") ?? false;
                string textAlign = (string)el.Attribute("TextAlign") ?? "Left";
                string passwordChar = (string)el.Attribute("PasswordChar") ?? "*";
                string characterCasing = (string)el.Attribute("CharacterCasing") ?? "Normal";
                bool acceptsReturn = (bool?)el.Attribute("AcceptsReturn") ?? false;
                string placeholderText = (string)el.Attribute("PlaceholderText") ?? "";

                sb.AppendLine($"drawEnhancedTextBox('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\", " +
                             $"{multiline.ToString().ToLower()}, {readOnly.ToString().ToLower()}, '{borderStyle}', " +
                             $"{useSystemPasswordChar.ToString().ToLower()}, '{textAlign.ToLower()}', " +
                             $"'{EscapeJsString(passwordChar)}', '{characterCasing.ToLower()}', " +
                             $"{acceptsReturn.ToString().ToLower()}, '{EscapeJsString(placeholderText)}',{enabled.ToString().ToLower()});");
                break;

            case "linklabel":
                int linkStart = 0;
                int linkLength = text.Length;
                string linkColor = "#0000ff";
                string visitedLinkColor = "#800080";

                string linkAreaStr = (string)el.Attribute("LinkArea");
                if (!string.IsNullOrEmpty(linkAreaStr))
                {
                    var linkAreaParts = linkAreaStr.Split(',');
                    if (int.TryParse(linkAreaParts[0], out linkStart) && int.TryParse(linkAreaParts[1], out linkLength))
                    {
                        linkStart = Math.Max(0, linkStart);
                        linkLength = Math.Min(linkLength, text.Length - linkStart);
                    }
                }

                string linkColorAttr = (string)el.Attribute("LinkColor");
                if (!string.IsNullOrEmpty(linkColorAttr))
                {
                    linkColor = Utility.ColorToHtml(linkColorAttr);
                }

                string visitedLinkColorAttr = (string)el.Attribute("VisitedLinkColor");
                if (!string.IsNullOrEmpty(visitedLinkColorAttr))
                {
                    visitedLinkColor = Utility.ColorToHtml(visitedLinkColorAttr);
                }

                sb.AppendLine($"drawLinkLabel('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"{linkStart}, {linkLength}, '{linkColor}', '{visitedLinkColor}', \"{font}\");");
                break;

            case "checkbox":
                bool isChecked = (bool?)el.Attribute("Checked") ?? false;
                sb.AppendLine($"drawCheckBox('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\", " +
                             $"{isChecked.ToString().ToLower()});");
                break;

            default:
                sb.AppendLine($"drawControl('{type.ToLower()}', '{text}', {location.X}, {location.Y}, " +
                             $"{size.Width}, {size.Height}, '{Utility.ColorToHtml(foreColor)}', " +
                             $"'{Utility.ColorToHtml(backColor)}', \"{font}\");");
                break;
        }

        // 递归处理子控件
        foreach (var childEl in el.Elements("Control"))
        {
            sb.Append(RenderControl(childEl));
        }

        return sb.ToString();
    }

    private static string EscapeJsString(string input)
    {
        if (string.IsNullOrEmpty(input))
            return input;

        return input.Replace("\\", "\\\\")
                   .Replace("'", "\\'")
                   .Replace("\"", "\\\"")
                   .Replace("\r", "\\r")
                   .Replace("\n", "\\n");
    }

    public static void SaveToFile(string xmlPath, string htmlPath)
    {
        string html = Generate(xmlPath);
        File.WriteAllText(htmlPath, html);
    }
}