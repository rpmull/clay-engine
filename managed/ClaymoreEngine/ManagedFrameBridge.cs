using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public delegate void ManagedFrameUpdateDelegate(float dt);

    public static class ManagedFrameBridge
    {
        public static void UpdateExport(float dt)
        {
            ManagedFrameServices.Update(dt);
        }
    }
}
