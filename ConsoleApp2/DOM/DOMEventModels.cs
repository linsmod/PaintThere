using System;
using System.Collections.Generic;

namespace RemoteForm.DOM
{
    /// <summary>
    /// 基础事件类 - 对应Web标准的Event
    /// </summary>
    public class DomEvent
    {
        public string Type { get; set; }
        public EventTarget Target { get; set; }
        public EventTarget CurrentTarget { get; set; }
        public DateTime TimeStamp { get; set; } = DateTime.Now;
        public bool Bubbles { get; set; }
        public bool Cancelable { get; set; }
        public bool Composed { get; set; }
        public bool DefaultPrevented { get; set; }

        public void PreventDefault() => DefaultPrevented = true;
        public void StopPropagation() => _stopPropagation = true;
        public void StopImmediatePropagation() => _stopImmediatePropagation = true;

        private bool _stopPropagation;
        private bool _stopImmediatePropagation;

        public Guid winFormId { get; set; }
    }

    /// <summary>
    /// UI事件 - 对应Web标准的UIEvent
    /// </summary>
    public class UIEvent : DomEvent
    {
        public int Detail { get; set; }
        public Window View { get; set; }
    }

    /// <summary>
    /// 鼠标事件 - 对应Web标准的MouseEvent
    /// </summary>
    public class MouseEvent : UIEvent
    {
        public double ScreenX { get; set; }
        public double ScreenY { get; set; }
        public double ClientX { get; set; }
        public double ClientY { get; set; }
        public double PageX { get; set; }
        public double PageY { get; set; }
        public int Button { get; set; }
        public int Buttons { get; set; }
        public bool CtrlKey { get; set; }
        public bool ShiftKey { get; set; }
        public bool AltKey { get; set; }
        public bool MetaKey { get; set; }
        public EventTarget RelatedTarget { get; set; }
    }

    /// <summary>
    /// 键盘事件 - 对应Web标准的KeyboardEvent
    /// </summary>
    public class KeyboardEvent : UIEvent
    {
        public string Key { get; set; }
        public string Code { get; set; }
        public int KeyCode { get; set; }
        public int CharCode { get; set; }
        public int Location { get; set; }
        public bool Repeat { get; set; }
        public bool IsComposing { get; set; }
        public bool CtrlKey { get; set; }
        public bool ShiftKey { get; set; }
        public bool AltKey { get; set; }
        public bool MetaKey { get; set; }
    }

    /// <summary>
    /// 滚轮事件 - 对应Web标准的WheelEvent
    /// </summary>
    public class WheelEvent : MouseEvent
    {
        public double DeltaX { get; set; }
        public double DeltaY { get; set; }
        public double DeltaZ { get; set; }
        public WheelDeltaMode DeltaMode { get; set; }
    }

    /// <summary>
    /// 焦点事件 - 对应Web标准的FocusEvent
    /// </summary>
    public class FocusEvent : UIEvent
    {
        public EventTarget RelatedTarget { get; set; }
    }

    /// <summary>
    /// 输入事件 - 对应Web标准的InputEvent
    /// </summary>
    public class InputEvent : UIEvent
    {
        public string Data { get; set; }
        public bool IsComposing { get; set; }
    }

    /// <summary>
    /// 触摸事件 - 对应Web标准的TouchEvent
    /// </summary>
    public class TouchEvent : UIEvent
    {
        public List<Touch> Touches { get; set; } = new List<Touch>();
        public List<Touch> TargetTouches { get; set; } = new List<Touch>();
        public List<Touch> ChangedTouches { get; set; } = new List<Touch>();
        public bool AltKey { get; set; }
        public bool CtrlKey { get; set; }
        public bool ShiftKey { get; set; }
        public bool MetaKey { get; set; }
    }

    /// <summary>
    /// 触摸点 - 对应Web标准的Touch
    /// </summary>
    public class Touch
    {
        public int Identifier { get; set; }
        public EventTarget Target { get; set; }
        public double ScreenX { get; set; }
        public double ScreenY { get; set; }
        public double ClientX { get; set; }
        public double ClientY { get; set; }
        public double PageX { get; set; }
        public double PageY { get; set; }
    }

    /// <summary>
    /// 事件目标 - 对应Web标准的EventTarget
    /// </summary>
    public class EventTarget
    {
        public string TagName { get; set; }
        public string Id { get; set; }
        public string ClassName { get; set; }
        public Dictionary<string, string> Attributes { get; set; } = new Dictionary<string, string>();
    }

    /// <summary>
    /// 窗口对象 - 对应Web标准的Window
    /// </summary>
    public class Window
    {
        public double InnerWidth { get; set; }
        public double InnerHeight { get; set; }
        public double ScrollX { get; set; }
        public double ScrollY { get; set; }
    }

    public enum WheelDeltaMode
    {
        Pixel = 0,
        Line = 1,
        Page = 2
    }

    /// <summary>
    /// 拖放事件
    /// </summary>
    public class DragEvent : MouseEvent
    {
        public DataTransfer DataTransfer { get; set; }
    }

    public class DataTransfer
    {
        public string DropEffect { get; set; }
        public string EffectAllowed { get; set; }
        public List<DataTransferItem> Items { get; set; } = new List<DataTransferItem>();

        public void SetData(string format, string data)
        {
            // 设置拖拽数据的逻辑
        }

        public string GetData(string format)
        {
            // 获取拖拽数据的逻辑
            return string.Empty;
        }
    }

    public class DataTransferItem
    {
        public string Kind { get; set; }
        public string Type { get; set; }
    }

    /// <summary>
    /// 自定义事件分发器
    /// </summary>
    public class EventDispatcher
    {
        private readonly Dictionary<string, List<EventHandler<DomEvent>>> _eventListeners =
            new Dictionary<string, List<EventHandler<DomEvent>>>();

        public void AddEventListener(string type, EventHandler<DomEvent> listener)
        {
            if (!_eventListeners.ContainsKey(type))
            {
                _eventListeners[type] = new List<EventHandler<DomEvent>>();
            }
            _eventListeners[type].Add(listener);
        }

        public void RemoveEventListener(string type, EventHandler<DomEvent> listener)
        {
            if (_eventListeners.ContainsKey(type))
            {
                _eventListeners[type].Remove(listener);
            }
        }

        public void DispatchEvent(Form currentForm, DomEvent evt)
        {
            if (_eventListeners.ContainsKey(evt.Type))
            {
                foreach (var listener in _eventListeners[evt.Type])
                {
                    listener.Invoke(currentForm, evt);
                }
            }
        }
    }
}