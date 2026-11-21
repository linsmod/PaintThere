using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using RemoteForm.DOM;
using System;

public class DomEventConverter : JsonConverter
{
    public override bool CanConvert(Type objectType)
    {
        return typeof(DomEvent).IsAssignableFrom(objectType);
    }

    public override object ReadJson(JsonReader reader, Type objectType, object existingValue, JsonSerializer serializer)
    {
        JObject jsonObject = JObject.Load(reader);
        string type = jsonObject["type"]?.Value<string>();

        DomEvent domEvent = CreateEventInstance(type);
        serializer.Populate(jsonObject.CreateReader(), domEvent);

        return domEvent;
    }

    private DomEvent CreateEventInstance(string eventType)
    {
        switch (eventType?.ToLower())
        {
            case "click":
            case "dblclick":
            case "mousedown":
            case "mouseup":
            case "mousemove":
            case "mouseenter":
            case "mouseleave":
            case "mouseover":
            case "mouseout":
            case "contextmenu":
                return new MouseEvent();

            case "wheel":
                return new WheelEvent();

            case "keydown":
            case "keyup":
            case "keypress":
                return new KeyboardEvent();

            case "touchstart":
            case "touchmove":
            case "touchend":
            case "touchcancel":
                return new TouchEvent();

            case "focus":
            case "blur":
                return new FocusEvent();

            case "input":
                return new InputEvent();

            default:
                return new DomEvent();
        }
    }

    public override void WriteJson(JsonWriter writer, object value, JsonSerializer serializer)
    {
        throw new NotImplementedException();
    }
}