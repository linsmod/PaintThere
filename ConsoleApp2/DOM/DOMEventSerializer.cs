using Newtonsoft.Json;
using RemoteForm.DOM;

public static class DomEventSerializer
{
    private static readonly JsonSerializerSettings Settings = new JsonSerializerSettings
    {
        Converters = new List<JsonConverter> { new DomEventConverter() },
        NullValueHandling = NullValueHandling.Ignore,
        MissingMemberHandling = MissingMemberHandling.Ignore
    };

    public static DomEvent Deserialize(string json)
    {
        return JsonConvert.DeserializeObject<DomEvent>(json, Settings);
    }

    public static T Deserialize<T>(string json) where T : DomEvent
    {
        return JsonConvert.DeserializeObject<T>(json, Settings);
    }
}