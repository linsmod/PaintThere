using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Xml.Linq;

namespace Aps反向代理
{
    public static class FormXmlValidator
    {
        public static List<string> ValidateXmlCharacters(Form form)
        {
            var xml = FormXmlSerializer.Serialize(form);
            var errors = new List<string>();

            // 检查表单属性
            ValidateElement(form.GetType().Name, form.Name, xml, errors);

            // 检查所有控件
            foreach (XElement controlElement in xml.Elements("Control"))
            {
                string controlType = (string)controlElement.Attribute("Type");
                string controlName = (string)controlElement.Attribute("Name");

                ValidateElement(controlType, controlName, controlElement, errors);
            }

            return errors;
        }

        private static void ValidateElement(string elementType, string elementName, XElement element, List<string> errors)
        {
            foreach (XAttribute attr in element.Attributes())
            {
                if (ContainsInvalidXmlChars(attr.Value))
                {
                    errors.Add($"无效字符 - {elementType}{(string.IsNullOrEmpty(elementName) ? "" : $" ({elementName})")} 的属性 '{attr.Name}': \"{EscapeControlChars(attr.Value)}\"");
                }
            }
        }

        private static bool ContainsInvalidXmlChars(string input)
        {
            return input.Any(ch =>
                char.IsControl(ch) &&
                ch != '\t' &&
                ch != '\n' &&
                ch != '\r');
        }

        private static string EscapeControlChars(string input)
        {
            return new string(input.Select(ch =>
                char.IsControl(ch) && ch != '\t' && ch != '\n' && ch != '\r' ? '�' : ch
            ).ToArray());
        }
    }
}
