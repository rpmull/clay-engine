using System;
using System.IO;

namespace ClaymoreEngine
{
    public delegate int EntryPointDelegate(IntPtr args, int size);

    public static class EngineEntry
    {
        public static int ManagedStart(IntPtr args, int size)
    {
         Console.WriteLine("[C#] ManagedStart invoked!");


         string envPath = Environment.GetEnvironmentVariable("CLAYMORE_SCRIPTS_DLL") ?? string.Empty;
         string scriptsPath = !string.IsNullOrWhiteSpace(envPath) && File.Exists(envPath)
            ? envPath
            : Path.Combine(AppContext.BaseDirectory, "GameScripts.dll");
         try
         {
            if (File.Exists(scriptsPath))
            {
               ScriptDomain.LoadScripts(scriptsPath);
            }
            else
            {
               Console.WriteLine("[C#] GameScripts.dll not found at: " + scriptsPath);
            }
         }
         catch (BadImageFormatException bif)
         {
            // Signal native to attempt a rebuild on Bad IL / corrupted DLL
            Console.WriteLine("[C#] BadImageFormatException while loading GameScripts.dll: " + bif.Message);
            return -1; // non-zero signals failure to native host
         }
         catch (Exception ex)
         {
            Console.WriteLine("[C#] Unexpected exception during ManagedStart: " + ex);
            return -2;
         }

         // Pump UI component events (e.g., Button) each frame from the engine loop.
         // Engine calls EngineSyncContext.Flush() every frame; here we just ensure a lightweight updater exists if needed.
         return 0;
        }
    }
}
