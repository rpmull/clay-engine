using System;

namespace ClaymoreEngine
{
    /// <summary>
    /// Managed logger that forwards messages to the native engine logger.
    /// Use this instead of Console.WriteLine to see messages in the editor.
    /// </summary>
    public static class ManagedLogger
    {
        private static void TryLog(int level, string message)
        {
            try
            {
                if (ComponentInterop.ManagedLog != null)
                {
                    ComponentInterop.ManagedLog(level, message);
                }
                else
                {
                    // Fall back to Console if native log isn't available yet
                    Console.WriteLine(message);
                }
            }
            catch (Exception ex)
            {
                // Never let logging throw inside exception handlers.
                try
                {
                    Console.WriteLine(message);
                    Console.WriteLine($"[ManagedLogger] Fallback logging due to logger failure: {ex.GetType().Name}: {ex.Message}");
                }
                catch
                {
                    // Intentionally swallow as final fallback.
                }
            }
        }

        public static void LogInfo(string message)
        {
            TryLog(0, message);
        }

        public static void LogWarning(string message)
        {
            TryLog(1, $"[Warning] {message}");
        }

        public static void LogError(string message)
        {
            TryLog(2, $"[Error] {message}");
        }

        public static void LogException(Exception ex, string context = "")
        {
            string prefix = string.IsNullOrEmpty(context) ? "" : $"[{context}] ";
            TryLog(2, $"{prefix}EXCEPTION: {ex.GetType().Name}: {ex.Message}");
            if (ex.StackTrace != null)
            {
                TryLog(2, $"Stack trace:\n{ex.StackTrace}");
            }
            if (ex.InnerException != null)
            {
                TryLog(2, $"Inner exception: {ex.InnerException.GetType().Name}: {ex.InnerException.Message}");
            }
        }
    }
}

