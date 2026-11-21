using Aps反向代理;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Windows.Forms;
using System.Xml.Linq;

public static class FormXmlSerializer
{
    public async static Task<XElement> SerializeAsync(Form form)
    {
        if (form.InvokeRequired)
        {
            return await form.InvokeAsync(() => Serialize(form));
        }
        return Serialize(form);
    }
    public static XElement Serialize(Form form)
    {
        var formElement = new XElement("Form",
            new XAttribute("Name", form.Name ?? ""),
            new XAttribute("Type", form.GetType().Name),
            new XAttribute("Text", form.Text),
            new XAttribute("Location", $"{form.Left},{form.Top}"),
            new XAttribute("Size", $"{form.ClientRectangle.Width},{form.ClientRectangle.Height}"),
            new XAttribute("Anchor", form.Anchor.ToString()),
            new XAttribute("Dock", form.Dock.ToString()),
            new XAttribute("Enabled", form.Enabled),
            new XAttribute("ForeColor", Utility.ColorToString(form.ForeColor)),
            new XAttribute("BackColor", Utility.ColorToString(form.BackColor)),
            new XAttribute("Font", Utility.FontToString(form.Font)),
            new XAttribute("Cursor", form.Cursor?.ToString() ?? "Default"),
            new XAttribute("TabIndex", form.TabIndex),
            new XAttribute("TabStop", form.TabStop),
            new XAttribute("AutoSize", form.AutoSize),
            new XAttribute("StartPosition", form.StartPosition.ToString())
        );

        foreach (Control control in GetAllControls(form))
        {
            if (form.Disposing || control.Disposing)
            {
                break;
            }
            if (form.IsDisposed || control.IsDisposed)
            {
                break;
            }
            if (control is OverlayPanel)
            {
                continue;
            }
            formElement.Add(SerializeControl(control));
        }
        
        //formElement.Add(new XAttribute("Click", String.Join(",", Utility.GetEventHandlerNames(form, "Click"))));
        return formElement;
    }

    private static XElement SerializeControl(Control control)
    {
        var element = new XElement("Control",
            new XAttribute("Name", control.Name ?? ""),
            new XAttribute("Type", control.GetType().Name),
            new XAttribute("Text", control.Text),
            new XAttribute("Location", $"{control.Left},{control.Top}"),
            new XAttribute("Size", $"{control.Width},{control.Height}"),
            new XAttribute("Anchor", control.Anchor.ToString()),
            new XAttribute("Dock", control.Dock.ToString()),
            new XAttribute("Enabled", control.Enabled),
            new XAttribute("ForeColor", Utility.ColorToString(control.ForeColor)),
            new XAttribute("BackColor", Utility.ColorToString(control.BackColor)),
            new XAttribute("Font", Utility.FontToString(control.Font)),
            new XAttribute("Cursor", control.Cursor?.ToString() ?? "Default"),
            new XAttribute("TabIndex", control.TabIndex),
            new XAttribute("TabStop", control.TabStop),
            new XAttribute("AutoSize", control.AutoSize)
        );

        // 特定控件拓展
        if (control is TextBox tb)
        {
            element.SetAttributeValue("Multiline", tb.Multiline);
            element.SetAttributeValue("ScrollBars", tb.ScrollBars.ToString());
            element.SetAttributeValue("WordWrap", tb.WordWrap);
            element.SetAttributeValue("ReadOnly", tb.ReadOnly);
            // 新增：
            element.SetAttributeValue("UseSystemPasswordChar", tb.UseSystemPasswordChar);
            element.SetAttributeValue("TextAlign", tb.TextAlign.ToString());
            element.SetAttributeValue("PasswordChar", tb.PasswordChar == '\0' ? null : tb.PasswordChar);
            element.SetAttributeValue("CharacterCasing", tb.CharacterCasing.ToString());
            element.SetAttributeValue("AutoCompleteSource", tb.AutoCompleteSource.ToString());
            element.SetAttributeValue("AutoCompleteMode", tb.AutoCompleteMode.ToString());

            // 特殊处理 AutoCompleteCustomSource（字符串集合）
            if (tb.AutoCompleteCustomSource != null && tb.AutoCompleteCustomSource.Count > 0)
            {
                var sourceElement = new XElement("AutoCompleteCustomSource");
                foreach (string item in tb.AutoCompleteCustomSource)
                {
                    sourceElement.Add(new XElement("Item", item));
                }
                element.Add(sourceElement);
            }

            element.SetAttributeValue("AcceptsReturn", tb.AcceptsReturn);
            element.SetAttributeValue("PlaceholderText", tb.PlaceholderText);
        }

        if (control is Button btn)
        {
            element.SetAttributeValue("FlatStyle", btn.FlatStyle.ToString());
        }
        if (control is CheckBox check)
        {
            element.SetAttributeValue("Checked", check.Checked.ToString());
        }

        if (control is LinkLabel linkLabel)
        {
            element.SetAttributeValue("LinkArea", $"{linkLabel.LinkArea.Start},{linkLabel.LinkArea.Length}");
            element.SetAttributeValue("LinkBehavior", linkLabel.LinkBehavior.ToString());
            element.SetAttributeValue("LinkColor", Utility.ColorToString(linkLabel.LinkColor));
            element.SetAttributeValue("VisitedLinkColor", Utility.ColorToString(linkLabel.VisitedLinkColor));
            element.SetAttributeValue("LinkVisited", linkLabel.LinkVisited);
        }
        else if (control is StatusStrip statusStrip)
        {
            var itemsElement = new XElement("Items");
            foreach (ToolStripItem item in statusStrip.Items)
            {
                itemsElement.Add(SerializeToolStripItem(item)); // 调用统一处理方法
            }
            element.Add(itemsElement);
        }

        return element;
    }

    private static XElement SerializeToolStripItem(ToolStripItem item)
    {
        XElement itemElement = new XElement(item.GetType().Name,
            new XAttribute("Name", item.Name ?? ""),
            new XAttribute("Text", item.Text),
            new XAttribute("Enabled", item.Enabled),
            new XAttribute("Visible", item.Visible),
            new XAttribute("ForeColor", Utility.ColorToString(item.ForeColor)),
            new XAttribute("BackColor", Utility.ColorToString(item.BackColor)),
            new XAttribute("Font", Utility.FontToString(item.Font)),
            new XAttribute("ToolTipText", item.ToolTipText ?? "")
        );
        // 处理图片（保存为文件并记录GUID）
        if (item.Image != null)
        {
            string imageFileName = Utility.SaveImageToFile(item.Image,AppDomain.CurrentDomain.BaseDirectory);
            itemElement.SetAttributeValue("Image", imageFileName); // 只存文件名
        }
        // 针对不同类型的 ToolStripItem 添加特有属性
        switch (item)
        {
            case ToolStripStatusLabel statusLabel:
                itemElement.SetAttributeValue("Alignment", statusLabel.Alignment.ToString());
                itemElement.SetAttributeValue("BorderSides", statusLabel.BorderSides.ToString());
                itemElement.SetAttributeValue("BorderStyle", statusLabel.BorderStyle.ToString());
                itemElement.SetAttributeValue("TextAlign", statusLabel.TextAlign.ToString());
                break;

            case ToolStripProgressBar progressBar:
                itemElement.SetAttributeValue("Value", progressBar.Value);
                itemElement.SetAttributeValue("Minimum", progressBar.Minimum);
                itemElement.SetAttributeValue("Maximum", progressBar.Maximum);
                itemElement.SetAttributeValue("Style", progressBar.Style.ToString());
                break;

            case ToolStripDropDownButton dropDownButton:
                if (dropDownButton.DropDownItems.Count > 0)
                {
                    var dropDownItemsElement = new XElement("DropDownItems");
                    foreach (ToolStripItem subItem in dropDownButton.DropDownItems)
                    {
                        dropDownItemsElement.Add(SerializeToolStripItem(subItem));
                    }
                    itemElement.Add(dropDownItemsElement);
                }
                break;

            case ToolStripSplitButton splitButton:
                if (splitButton.DropDownItems.Count > 0)
                {
                    var splitItemsElement = new XElement("DropDownItems");
                    foreach (ToolStripItem subItem in splitButton.DropDownItems)
                    {
                        splitItemsElement.Add(SerializeToolStripItem(subItem));
                    }
                    itemElement.Add(splitItemsElement);
                }
                break;

            case ToolStripComboBox comboBox:
                itemElement.SetAttributeValue("Items", string.Join("|", comboBox.Items.Cast<object>()));
                itemElement.SetAttributeValue("SelectedIndex", comboBox.SelectedIndex);
                itemElement.SetAttributeValue("DropDownStyle", comboBox.DropDownStyle.ToString());
                break;

            case ToolStripTextBox textBox:
                itemElement.SetAttributeValue("Text", textBox.Text);
                itemElement.SetAttributeValue("ReadOnly", textBox.ReadOnly);
                itemElement.SetAttributeValue("MaxLength", textBox.MaxLength);
                break;
        }

        return itemElement;
    }
    public static void SaveToFile(Form form, string filePath)
    {
        var validationErrors = FormXmlValidator.ValidateXmlCharacters(form);
        if (validationErrors.Any())
        {
            // 显示或记录错误信息
            foreach (var error in validationErrors)
            {
                //Console.WriteLine(error);
                MessageBox.Show(error);
            }

            // 可以选择不保存或继续保存
            return;
        }
        var xml = Serialize(form);
        xml.Save(filePath);
        CanvasLayoutGenerator.SaveToFile(filePath, form.Name + "_layout.html");

        SaveSystemColorsMap("SystemColors.xml");
    }

    public static void SaveSystemColorsMap(string filePath)
    {
        var doc = new XDocument(
            new XElement("SystemColors",
                from property in typeof(SystemColors).GetProperties()
                where property.PropertyType == typeof(Color)
                select new XElement("Color",
                    new XAttribute("Name", property.Name),
                    new XAttribute("Value", Utility.ColorToRgba((Color)property.GetValue(null))))));

        doc.Save(filePath);
    }

    public static Form Deserialize(string filePath)
    {
        var doc = XDocument.Load(filePath);
        return DeserializeForm(doc.Root);
    }

    private static Form DeserializeForm(XElement formElement)
    {
        var form = new Form();
        form.Name = (string)formElement.Attribute("Name");
        form.Text = (string)formElement.Attribute("Text");

        var sizeStr = (string)formElement.Attribute("Size");
        if (Utility.SizeConverter.TryParse(sizeStr, out var size))
            form.Size = size;

        var startPosStr = (string)formElement.Attribute("StartPosition");
        if (Enum.TryParse(typeof(FormStartPosition), startPosStr, out object startPos))
            form.StartPosition = (FormStartPosition)startPos;

        foreach (var el in formElement.Elements("Control"))
        {
            var control = DeserializeControl(el);
            form.Controls.Add(control);
        }

        return form;
    }

    private static Control DeserializeControl(XElement el)
    {
        string controlTypeName = (string)el.Attribute("Type");
        string name = (string)el.Attribute("Name");
        string text = (string)el.Attribute("Text");

        var controlType = Type.GetType(controlTypeName);
        var control = (Control)Activator.CreateInstance(controlType);

        control.Name = name;
        control.Text = text;

        var locStr = (string)el.Attribute("Location");
        var sizeStr = (string)el.Attribute("Size");

        if (Utility.PointConverter.TryParse(locStr, out var location))
            control.Location = location;

        if (Utility.SizeConverter.TryParse(sizeStr, out var size))
            control.Size = size;

        control.Anchor = ParseEnum<AnchorStyles>((string)el.Attribute("Anchor"));
        control.Dock = ParseEnum<DockStyle>((string)el.Attribute("Dock"));
        control.Enabled = (bool)el.Attribute("Enabled");

        // 颜色 & 字体
        control.ForeColor = Utility.ColorFromString((string)el.Attribute("ForeColor"));
        control.BackColor = Utility.ColorFromString((string)el.Attribute("BackColor"));
        control.Font = Utility.FontFromString((string)el.Attribute("Font"));

        // 枚举处理（通用）
        foreach (var enumEl in el.Elements("Enum"))
        {
            string enumTypeName = (string)enumEl.Attribute("Type");
            var enumType = Type.GetType(enumTypeName);

            object enumValue = 0;
            foreach (var item in enumEl.Elements("Item"))
            {
                int id = (int)item.Attribute("Id");
                enumValue = Convert.ChangeType(id, enumType);
            }

            var propInfo = controlType.GetProperty(enumType.Name, BindingFlags.Public | BindingFlags.Instance);
            if (propInfo != null && propInfo.CanWrite && Enum.IsDefined(enumType, enumValue))
            {
                propInfo.SetValue(control, enumValue);
            }
        }

        // 子控件递归
        foreach (var childEl in el.Elements("Control"))
        {
            var child = DeserializeControl(childEl);
            control.Controls.Add(child);
        }

        control.Cursor = (Cursor)typeof(Cursors).GetProperty(
                    (string)el.Attribute("Cursor"))?.GetValue(null) ?? Cursors.Default;

        control.TabIndex = (int)el.Attribute("TabIndex");
        control.TabStop = (bool)el.Attribute("TabStop");
        control.AutoSize = (bool)el.Attribute("AutoSize");

        if (control is TextBox textBox)
        {
            textBox.Multiline = (bool)el.Attribute("Multiline");
            textBox.ScrollBars = ParseEnum<ScrollBars>((string)el.Attribute("ScrollBars"));
            textBox.WordWrap = (bool)el.Attribute("WordWrap");
            textBox.ReadOnly = (bool)el.Attribute("ReadOnly");
            // 新增：
            textBox.UseSystemPasswordChar = (bool)el.Attribute("UseSystemPasswordChar");
            textBox.TextAlign = ParseEnum<HorizontalAlignment>((string)el.Attribute("TextAlign"));
            var passwordCharStr = (string)el.Attribute("PasswordChar");
            if (!string.IsNullOrEmpty(passwordCharStr))
            {
                if (passwordCharStr.Length == 1)
                {
                    textBox.PasswordChar = passwordCharStr[0];
                }
                else
                {
                    // 可选：记录警告日志
                    // Console.WriteLine("Invalid PasswordChar value: must be a single character.");
                    textBox.PasswordChar = '\0'; // 默认清空密码字符
                }
            }
            else
            {
                textBox.PasswordChar = '\0'; // 空值则不清除也可保留原控件默认行为
            }
            textBox.CharacterCasing = ParseEnum<CharacterCasing>((string)el.Attribute("CharacterCasing"));
            textBox.AutoCompleteSource = ParseEnum<AutoCompleteSource>((string)el.Attribute("AutoCompleteSource"));
            textBox.AutoCompleteMode = ParseEnum<AutoCompleteMode>((string)el.Attribute("AutoCompleteMode"));

            // 反序列化 AutoCompleteCustomSource
            var autoCompleteEl = el.Element("AutoCompleteCustomSource");
            if (autoCompleteEl != null)
            {
                var source = new AutoCompleteStringCollection();
                foreach (var item in autoCompleteEl.Elements("Item"))
                {
                    source.Add(item.Value);
                }
                textBox.AutoCompleteCustomSource = source;
            }

            textBox.AcceptsReturn = (bool)el.Attribute("AcceptsReturn");
            textBox.PlaceholderText = (string)el.Attribute("PlaceholderText");
        }

        if (control is Button button)
        {
            button.FlatStyle = ParseEnum<FlatStyle>((string)el.Attribute("FlatStyle"));
        }

        if (control is LinkLabel linkLabel)
        {
            var linkAreaStr = (string)el.Attribute("LinkArea");
            if (Utility.LinkAreaConverter.TryParse(linkAreaStr, out var linkArea))
                linkLabel.LinkArea = linkArea;

            linkLabel.LinkBehavior = ParseEnum<LinkBehavior>((string)el.Attribute("LinkBehavior"));
            linkLabel.LinkColor = Utility.ColorFromString((string)el.Attribute("LinkColor"));
            linkLabel.VisitedLinkColor = Utility.ColorFromString((string)el.Attribute("VisitedLinkColor"));
        }

        return control;
    }

    private static T ParseEnum<T>(string value)
    {
        if (Enum.TryParse(typeof(T), value, out object result))
            return (T)result;
        return default(T);
    }

    private static IEnumerable<Control> GetAllControls(Control parent)
    {
        foreach (Control control in parent.Controls)
        {
            yield return control;
            foreach (var child in GetAllControls(control))
                yield return child;
        }
    }

}