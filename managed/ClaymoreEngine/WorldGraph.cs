using System;
using System.Collections.Generic;
using System.IO;
using System.Numerics;

namespace ClaymoreEngine
{
    public static class WorldGraph
    {
        /// <summary>
        /// Returns true if two scene identifiers match.
        /// Accepts full paths (e.g. "scenes/TestScene.scene", "assets/scenes/Main.sceneb")
        /// or stripped names (e.g. "TestScene", "Main").
        /// </summary>
        public static bool SceneNamesMatch(string a, string b)
        {
            if (string.IsNullOrEmpty(a) || string.IsNullOrEmpty(b)) return false;
            string strippedA = StripSceneName(a);
            string strippedB = StripSceneName(b);
            return string.Equals(strippedA, strippedB, StringComparison.OrdinalIgnoreCase)
                || string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        /// Extracts the scene name without path or extension (e.g. "TestScene" from "scenes/TestScene.scene").
        /// </summary>
        public static string StripSceneName(string scenePathOrName)
        {
            if (string.IsNullOrEmpty(scenePathOrName)) return string.Empty;
            string name = Path.GetFileName(scenePathOrName);
            return string.IsNullOrEmpty(name) ? scenePathOrName : Path.GetFileNameWithoutExtension(name);
        }

        public struct PortalInfo
        {
            public string ScenePath;
            public string PortalGuid;
            public string PortalPath;
            public Vector3 EntryPosition;
            public string TargetScenePath;
            public string TargetPortalGuid;
            public string TargetPortalPath;
            public Vector3 ExitPosition;
            public bool Resolved;
            public float Distance;
        }

        public struct PoiInfo
        {
            public string ScenePath;
            public string PoiGuid;
            public string PoiPath;
            public string ScriptClass;
            public string NodeName;
            public string NodeType;
            public bool IsPortal;
            public Vector3 Position;
        }

        public static bool LoadProjectGraph()
        {
            return WorldGraphInterop.LoadProject != null && WorldGraphInterop.LoadProject();
        }

        public static int PortalCount => WorldGraphInterop.GetPortalCount != null ? WorldGraphInterop.GetPortalCount() : 0;
        public static int PoiCount => WorldGraphInterop.GetPoiCount != null ? WorldGraphInterop.GetPoiCount() : 0;

        public static PortalInfo GetPortal(int index)
        {
            PortalInfo info = new PortalInfo();
            info.ScenePath = WorldGraphInterop.GetPortalScenePath(index);
            info.PortalGuid = WorldGraphInterop.GetPortalGuid(index);
            info.PortalPath = WorldGraphInterop.GetPortalPath(index);
            info.TargetScenePath = WorldGraphInterop.GetPortalTargetScenePath(index);
            info.TargetPortalGuid = WorldGraphInterop.GetPortalTargetGuid(index);
            info.TargetPortalPath = WorldGraphInterop.GetPortalTargetPath(index);
            info.Resolved = WorldGraphInterop.IsPortalResolved != null && WorldGraphInterop.IsPortalResolved(index);
            info.Distance = WorldGraphInterop.GetPortalDistance != null ? WorldGraphInterop.GetPortalDistance(index) : 0.0f;

            float x, y, z;
            if (WorldGraphInterop.GetPortalEntryPosition != null)
            {
                WorldGraphInterop.GetPortalEntryPosition(index, out x, out y, out z);
                info.EntryPosition = new Vector3(x, y, z);
            }
            if (WorldGraphInterop.GetPortalExitPosition != null)
            {
                WorldGraphInterop.GetPortalExitPosition(index, out x, out y, out z);
                info.ExitPosition = new Vector3(x, y, z);
            }

            return info;
        }

        /// <summary>
        /// Returns all portals in the given scene from the baked WorldGraph data.
        /// Does not require the scene to be loaded. sceneName can be full path (e.g. "scenes/TestScene.scene")
        /// or stripped name (e.g. "TestScene").
        /// </summary>
        public static List<PortalInfo> GetPortalsByScene(string sceneName)
        {
            var result = new List<PortalInfo>();
            if (string.IsNullOrEmpty(sceneName)) return result;

            int count = PortalCount;
            for (int i = 0; i < count; i++)
            {
                var info = GetPortal(i);
                if (SceneNamesMatch(info.ScenePath, sceneName))
                    result.Add(info);
            }
            return result;
        }

        /// <summary>
        /// Returns portals that connect startScene to endScene from the baked WorldGraph data.
        /// Does not require either scene to be loaded. Returns portals where (ScenePath→TargetScenePath)
        /// matches either (startScene→endScene) or (endScene→startScene).
        /// Scene names can be full paths or stripped names (e.g. "TestScene").
        /// </summary>
        public static List<PortalInfo> GetPortalsBetweenScenes(string startScene, string endScene)
        {
            var result = new List<PortalInfo>();
            if (string.IsNullOrEmpty(startScene) || string.IsNullOrEmpty(endScene)) return result;

            int count = PortalCount;
            for (int i = 0; i < count; i++)
            {
                var info = GetPortal(i);
                bool startToEnd = SceneNamesMatch(info.ScenePath, startScene) && SceneNamesMatch(info.TargetScenePath, endScene);
                bool endToStart = SceneNamesMatch(info.ScenePath, endScene) && SceneNamesMatch(info.TargetScenePath, startScene);
                if (startToEnd || endToStart)
                    result.Add(info);
            }
            return result;
        }

        /// <summary>
        /// Finds a path of scene names from fromScene to toScene using BFS on the portal graph.
        /// Returns a list of scene names (e.g. [A, B, C]) or empty list if no path exists.
        /// Includes both start and end scenes. Single-scene path [A] means fromScene == toScene.
        /// </summary>
        public static List<string> FindScenePath(string fromScene, string toScene)
        {
            var path = new List<string>();
            if (string.IsNullOrEmpty(fromScene) || string.IsNullOrEmpty(toScene))
                return path;
            if (SceneNamesMatch(fromScene, toScene))
            {
                path.Add(fromScene);
                return path;
            }

            // Build adjacency using canonical scene names (first occurrence from portals)
            var canonical = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var adj = new Dictionary<string, HashSet<string>>(StringComparer.OrdinalIgnoreCase);
            int count = PortalCount;
            for (int i = 0; i < count; i++)
            {
                var p = GetPortal(i);
                string a = p.ScenePath ?? string.Empty;
                string b = p.TargetScenePath ?? string.Empty;
                if (string.IsNullOrEmpty(a) || string.IsNullOrEmpty(b)) continue;
                string ca = StripSceneName(a);
                string cb = StripSceneName(b);
                if (!canonical.ContainsKey(ca)) canonical[ca] = a;
                if (!canonical.ContainsKey(cb)) canonical[cb] = b;
                if (!adj.TryGetValue(ca, out var setA)) { setA = new HashSet<string>(StringComparer.OrdinalIgnoreCase); adj[ca] = setA; }
                setA.Add(cb);
                if (!adj.TryGetValue(cb, out var setB)) { setB = new HashSet<string>(StringComparer.OrdinalIgnoreCase); adj[cb] = setB; }
                setB.Add(ca);
            }

            string fromKey = StripSceneName(fromScene);
            string toKey = StripSceneName(toScene);

            // BFS
            var queue = new Queue<string>();
            var parent = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            queue.Enqueue(fromKey);
            parent[fromKey] = null;

            while (queue.Count > 0)
            {
                string cur = queue.Dequeue();
                if (string.Equals(cur, toKey, StringComparison.OrdinalIgnoreCase))
                {
                    var rev = new List<string>();
                    for (string n = cur; n != null; n = parent.TryGetValue(n, out var p) ? p : null)
                        rev.Add(canonical.TryGetValue(n, out var can) ? can : n);
                    rev.Reverse();
                    return rev;
                }
                if (!adj.TryGetValue(cur, out var neighbors)) continue;
                foreach (string next in neighbors)
                {
                    if (parent.ContainsKey(next)) continue;
                    parent[next] = cur;
                    queue.Enqueue(next);
                }
            }
            return path;
        }

        /// <summary>
        /// Returns the per-segment transit distances for a path of scenes.
        /// For path [A,B,C]: segment[0]=fromPoi to exit portal in A; segment[1]=entry to exit in B; segment[2]=entry to toPoi in C.
        /// Returns empty list if path is invalid or has fewer than 2 scenes.
        /// </summary>
        public static List<float> GetPathSegmentDistances(List<string> path, string fromPoiGuid, string toPoiGuid)
        {
            var segments = new List<float>();
            if (path == null || path.Count < 2)
                return segments;

            string prevEntryGuid = null;
            for (int i = 0; i < path.Count - 1; i++)
            {
                string fromScene = path[i];
                string toScene = path[i + 1];
                var portals = GetPortalsBetweenScenes(fromScene, toScene);
                if (portals == null || portals.Count == 0)
                {
                    segments.Add(5f);
                    prevEntryGuid = null;
                    continue;
                }

                PortalInfo? exitPortal = null;
                foreach (var p in portals)
                {
                    if (SceneNamesMatch(p.ScenePath, fromScene) && SceneNamesMatch(p.TargetScenePath, toScene))
                    {
                        exitPortal = p;
                        break;
                    }
                }
                if (exitPortal == null)
                {
                    segments.Add(5f);
                    prevEntryGuid = null;
                    continue;
                }

                var pInfo = exitPortal.Value;
                string exitGuid = pInfo.PortalGuid ?? string.Empty;
                string entryGuid = pInfo.TargetPortalGuid ?? string.Empty;

                float dist = 0f;
                if (i == 0)
                {
                    if (!string.IsNullOrEmpty(fromPoiGuid))
                    {
                        float d = GetPoiToPortalDistance(fromScene, fromPoiGuid, exitGuid);
                        if (d >= 0f) dist = d;
                    }
                }
                else if (!string.IsNullOrEmpty(prevEntryGuid))
                {
                    float d = GetPortalToPortalDistance(fromScene, prevEntryGuid, exitGuid);
                    if (d >= 0f) dist = d;
                }

                prevEntryGuid = entryGuid;

                if (i == path.Count - 2 && !string.IsNullOrEmpty(toPoiGuid))
                {
                    float d = GetPortalToPoiDistance(toScene, entryGuid, toPoiGuid);
                    if (d >= 0f)
                        dist += d;
                    else if (dist <= 0f)
                        dist = 5f;
                }

                if (dist <= 0f) dist = 5f;
                segments.Add(dist);
            }
            return segments;
        }

        public static PoiInfo GetPoi(int index)
        {
            PoiInfo info = new PoiInfo();
            info.ScenePath = WorldGraphInterop.GetPoiScenePath(index);
            info.PoiGuid = WorldGraphInterop.GetPoiGuid(index);
            info.PoiPath = WorldGraphInterop.GetPoiPath(index);
            info.ScriptClass = WorldGraphInterop.GetPoiScriptClass(index);
            info.NodeName = WorldGraphInterop.GetPoiNodeName(index);
            info.NodeType = WorldGraphInterop.GetPoiNodeType(index);
            info.IsPortal = WorldGraphInterop.GetPoiIsPortal != null && WorldGraphInterop.GetPoiIsPortal(index);

            float x, y, z;
            if (WorldGraphInterop.GetPoiPosition != null)
            {
                WorldGraphInterop.GetPoiPosition(index, out x, out y, out z);
                info.Position = new Vector3(x, y, z);
            }

            return info;
        }

        public static int FindPoiIndex(string scenePath, string poiGuid)
        {
            return WorldGraphInterop.FindPoiIndex != null
                ? WorldGraphInterop.FindPoiIndex(scenePath ?? string.Empty, poiGuid ?? string.Empty)
                : -1;
        }

        public static float GetPoiToPortalDistance(string scenePath, string poiGuid, string portalGuid)
        {
            return WorldGraphInterop.GetPoiToPortalDistance != null
                ? WorldGraphInterop.GetPoiToPortalDistance(scenePath ?? string.Empty, poiGuid ?? string.Empty, portalGuid ?? string.Empty)
                : -1.0f;
        }

        public static float GetPortalToPoiDistance(string scenePath, string portalGuid, string poiGuid)
        {
            return WorldGraphInterop.GetPortalToPoiDistance != null
                ? WorldGraphInterop.GetPortalToPoiDistance(scenePath ?? string.Empty, portalGuid ?? string.Empty, poiGuid ?? string.Empty)
                : -1.0f;
        }

        public static float GetPortalToPortalDistance(string scenePath, string fromPortalGuid, string toPortalGuid)
        {
            return WorldGraphInterop.GetPortalToPortalDistance != null
                ? WorldGraphInterop.GetPortalToPortalDistance(scenePath ?? string.Empty, fromPortalGuid ?? string.Empty, toPortalGuid ?? string.Empty)
                : -1.0f;
        }

        /// <summary>
        /// Finds the first POI GUID in the given scene matching nodeType (e.g. "House", "Tavern").
        /// If nameFilter is non-empty, NodeName must contain it (case-insensitive).
        /// Returns empty string if no match.
        /// </summary>
        public static string FindFirstPoiGuidInScene(string scenePath, string nodeType, string nameFilter)
        {
            if (string.IsNullOrEmpty(scenePath)) return string.Empty;
            int count = PoiCount;
            for (int i = 0; i < count; i++)
            {
                var poi = GetPoi(i);
                if (!SceneNamesMatch(poi.ScenePath, scenePath)) continue;
                if (!string.IsNullOrEmpty(nodeType) && !string.Equals(poi.NodeType ?? string.Empty, nodeType, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (!string.IsNullOrEmpty(nameFilter) && (poi.NodeName == null || poi.NodeName.IndexOf(nameFilter, StringComparison.OrdinalIgnoreCase) < 0))
                    continue;
                return poi.PoiGuid ?? string.Empty;
            }
            return string.Empty;
        }

        /// <summary>
        /// Returns the total transit distance for inter-scene travel, supporting multi-hop paths.
        /// Uses FindScenePath for pathfinding; sums all segment distances.
        /// If fromPoiGuid or toPoiGuid is empty, those legs contribute 0 (or fallback).
        /// Returns -1 if no path exists between scenes.
        /// </summary>
        public static float GetInterSceneTransitDistance(string fromScene, string toScene, string fromPoiGuid, string toPoiGuid)
        {
            var path = FindScenePath(fromScene, toScene);
            if (path == null || path.Count < 2)
                return -1.0f;

            var segments = GetPathSegmentDistances(path, fromPoiGuid, toPoiGuid);
            if (segments == null || segments.Count == 0)
                return -1.0f;

            float total = 0f;
            foreach (float s in segments)
                total += s;
            return total > 0f ? total : 5f;
        }
    }
}
