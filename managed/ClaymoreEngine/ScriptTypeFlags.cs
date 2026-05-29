using System;

namespace ClaymoreEngine
{
    [Flags]
    public enum ScriptTypeFlags : uint
    {
        None = 0,
        DontDestroyOnLoad = 1 << 0,
        BakeInWorldGraph = 1 << 1,
        PreAnimationUpdate = 1 << 2
    }
}
