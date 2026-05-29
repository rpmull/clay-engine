using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine.Scripting.Scriptable
{
    internal static class FieldAdapters
    {
        public static ManagedValue Box<T>(T value)
        {
            ManagedValue mv = default;
            if (value is int i) { mv.type = 2; mv.data = Marshal.AllocHGlobal(sizeof(int)); Marshal.WriteInt32(mv.data, i); return mv; }
            if (value is float f) { mv.type = 4; mv.data = Marshal.AllocHGlobal(sizeof(float)); Marshal.StructureToPtr(f, mv.data, false); return mv; }
            if (value is bool b) { mv.type = 1; mv.data = Marshal.AllocHGlobal(1); Marshal.WriteByte(mv.data, (byte)(b ? 1 : 0)); return mv; }
            if (value is string s) { mv.type = 6; mv.data = Marshal.StringToCoTaskMemUTF8(s); return mv; }
            return mv;
        }
    }
}


