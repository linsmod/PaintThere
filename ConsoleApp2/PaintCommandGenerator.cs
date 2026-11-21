using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Xml.Linq;
using System.Text;
using Aps反向代理;
public static class PaintCommandGenerator
{
    public static List<string> Generate(string xmlFilePath)
    {
        List<string> paintCommands = new List<string>();
        var doc = XDocument.Load(xmlFilePath);
        var formElement = doc.Element("Form");

        // 获取表单属性
        string sizeStr = (string)formElement.Attribute("Size");
        var sizeParts = sizeStr.Split(',');
        int width = int.Parse(sizeParts[0]);
        int height = int.Parse(sizeParts[1]);
        string backColor = (string)formElement.Attribute("BackColor") ?? "transparent";
        // 渲染所有控件
        foreach (var controlEl in formElement.Elements("Control"))
        {
            RenderControl(controlEl, paintCommands);
        }

        return paintCommands;
    }
    public static List<string> Generate(XElement formElement)
    {
        List<string> paintCommands = new List<string>();
        // 获取表单属性
        string sizeStr = (string)formElement.Attribute("Size");
        string locationStr = (string)formElement.Attribute("Location");
        var sizeParts = sizeStr.Split(',');
        int width = int.Parse(sizeParts[0]);
        int height = int.Parse(sizeParts[1]);
        if (width == 0 || height == 0)
        {
            return paintCommands;
        }
        string backColor = (string)formElement.Attribute("BackColor") ?? "transparent";
        paintCommands.Add("setBounds(" + sizeStr + "," + locationStr + ")");
        paintCommands.Add("drawBackground('" + backColor + "')");
        // 渲染所有控件
        foreach (var controlEl in formElement.Elements("Control"))
        {
            RenderControl(controlEl, paintCommands);
        }

        return paintCommands;
    }

    private static string RenderControl(XElement el, List<string> paintCommands)
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
                paintCommands.Add($"drawButton('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\");");
                break;

            case "label":
                paintCommands.Add($"drawLabel('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\");");
                break;

            case "textbox":
                bool multiline = (bool?)el.Attribute("Multiline") ?? false;
                bool readOnly = (bool?)el.Attribute("ReadOnly") ?? false;
                string borderStyle = (string)el.Attribute("BorderStyle") ?? "Fixed3D";
                bool useSystemPasswordChar = (bool?)el.Attribute("UseSystemPasswordChar") ?? false;
                string textAlign = (string)el.Attribute("TextAlign") ?? "Left";
                string passwordChar = (string)el.Attribute("PasswordChar") ?? null;
                string characterCasing = (string)el.Attribute("CharacterCasing") ?? "Normal";
                bool acceptsReturn = (bool?)el.Attribute("AcceptsReturn") ?? false;
                string placeholderText = (string)el.Attribute("PlaceholderText") ?? "";

                string passwordCharParam = passwordChar == null ? "null" : "'" + EscapeJsString(passwordChar) + "'";

                paintCommands.Add($"drawEnhancedTextBox('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\", " +
                             $"{multiline.ToString().ToLower()}, {readOnly.ToString().ToLower()}, '{borderStyle}', " +
                             $"{useSystemPasswordChar.ToString().ToLower()}, '{textAlign.ToLower()}', " +
                             $"{passwordCharParam}, '{characterCasing.ToLower()}', " +
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

                paintCommands.Add($"drawLinkLabel('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"{linkStart}, {linkLength}, '{linkColor}', '{visitedLinkColor}', \"{font}\");");
                break;

            case "checkbox":
                bool isChecked = (bool?)el.Attribute("Checked") ?? false;
                paintCommands.Add($"drawCheckBox('{text}', {location.X}, {location.Y}, {size.Width}, {size.Height}, " +
                             $"'{Utility.ColorToHtml(foreColor)}', '{Utility.ColorToHtml(backColor)}', \"{font}\", " +
                             $"{isChecked.ToString().ToLower()});");
                break;

            default:
                paintCommands.Add($"drawControl('{type.ToLower()}', '{text}', {location.X}, {location.Y}, " +
                             $"{size.Width}, {size.Height}, '{Utility.ColorToHtml(foreColor)}', " +
                             $"'{Utility.ColorToHtml(backColor)}', \"{font}\");");
                break;
        }

        // 递归处理子控件
        foreach (var childEl in el.Elements("Control"))
        {
            RenderControl(childEl, paintCommands);
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
}