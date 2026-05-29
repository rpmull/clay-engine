using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    internal static class SceneInterop
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate bool Scene_LoadSceneFn([MarshalAs(UnmanagedType.LPStr)] string path);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate bool Scene_LoadSceneExFn([MarshalAs(UnmanagedType.LPStr)] string path, [MarshalAs(UnmanagedType.I1)] bool async);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void Scene_UnloadSceneFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate float Scene_GetLoadProgressFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate bool Scene_IsSceneLoadingFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate bool Scene_IsSceneLoadedFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate IntPtr Scene_GetCurrentScenePathFn();

        // Bound by native init; we piggyback on EntityInterop table expansion if desired.
        internal static Scene_LoadSceneFn LoadScene;
        internal static Scene_LoadSceneExFn LoadSceneEx;
        internal static Scene_UnloadSceneFn UnloadScene;
        internal static Scene_GetLoadProgressFn GetLoadProgress;
        internal static Scene_IsSceneLoadingFn IsSceneLoading;
        internal static Scene_IsSceneLoadedFn IsSceneLoaded;
        internal static Scene_GetCurrentScenePathFn GetCurrentScenePath;
    }

    public static class SceneManager
    {
        public static bool Load(string path) => LoadAsync(path);
        public static bool LoadAsync(string path)
        {
            if (SceneInterop.LoadSceneEx != null) return SceneInterop.LoadSceneEx(path, true);
            return SceneInterop.LoadScene != null && SceneInterop.LoadScene(path);
        }

        public static bool LoadSync(string path)
        {
            if (SceneInterop.LoadSceneEx != null) return SceneInterop.LoadSceneEx(path, false);
            return SceneInterop.LoadScene != null && SceneInterop.LoadScene(path);
        }

        public static void Unload() => SceneInterop.UnloadScene?.Invoke();

        public static float LoadProgress => SceneInterop.GetLoadProgress != null
            ? SceneInterop.GetLoadProgress()
            : (IsLoaded ? 1.0f : 0.0f);

        public static bool IsLoading => SceneInterop.IsSceneLoading != null && SceneInterop.IsSceneLoading();
        public static bool IsLoaded => SceneInterop.IsSceneLoaded == null || SceneInterop.IsSceneLoaded();

        /// <summary>
        /// Gets the path of the currently loaded scene (e.g. "scenes/TestScene.scene").
        /// Returns empty string if no scene is loaded or path is unknown.
        /// </summary>
        public static string CurrentScenePath
        {
            get
            {
                if (SceneInterop.GetCurrentScenePath == null) return string.Empty;
                var ptr = SceneInterop.GetCurrentScenePath();
                return ptr != IntPtr.Zero ? System.Runtime.InteropServices.Marshal.PtrToStringAnsi(ptr) ?? string.Empty : string.Empty;
            }
        }

        /// <summary>
        /// Gets the current scene name without path or extension (e.g. "TestScene" from "scenes/TestScene.scene").
        /// Use this for inter-scene transport, portal detection, and comparing with destinationScene/currentScene
        /// in GlobalScheduler, TimeBlock, and other scripts. Matches the format used by WorldGraph.SceneNamesMatch.
        /// </summary>
        public static string CurrentScene => WorldGraph.StripSceneName(CurrentScenePath);
    }
}


