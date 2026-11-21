using System;
using Newtonsoft.Json;

namespace RemoteForm.Paint
{
    /// <summary>
    /// 结构化绘图指令：cmd 为渲染函数名，args 为参数，id 用于增量比较，rect 用于脏矩形。
    /// 相比原来的 JS 字符串指令，省掉 eval 解析，并支持逐控件 diff。
    /// </summary>
    public class PaintCommand : IEquatable<PaintCommand>
    {
        [JsonProperty("id", NullValueHandling = NullValueHandling.Ignore)]
        public string Id { get; set; }

        [JsonProperty("cmd")]
        public string Cmd { get; set; }

        [JsonProperty("args")]
        public object[] Args { get; set; }

        [JsonProperty("rect", NullValueHandling = NullValueHandling.Ignore)]
        public int[] Rect { get; set; }

        public bool Equals(PaintCommand other)
        {
            if (other is null)
                return false;
            if (!string.Equals(Id, other.Id, StringComparison.Ordinal))
                return false;
            if (!string.Equals(Cmd, other.Cmd, StringComparison.Ordinal))
                return false;
            if (Args is null || other.Args is null)
                return ReferenceEquals(Args, other.Args);
            if (Args.Length != other.Args.Length)
                return false;
            for (int i = 0; i < Args.Length; i++)
            {
                if (!Equals(Args[i], other.Args[i]))
                    return false;
            }
            return true;
        }

        public override bool Equals(object obj) => Equals(obj as PaintCommand);

        public override int GetHashCode() => Cmd?.GetHashCode(StringComparison.Ordinal) ?? 0;
    }
}
