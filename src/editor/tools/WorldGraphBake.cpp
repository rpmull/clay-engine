#include "WorldGraphBake.h"
#include "editor/Project.h"
#include "core/serialization/Serializer.h"
#include "core/world/WorldGraph.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include "core/ecs/EntityData.h"
#include "core/managed/ScriptSystem.h"
#include "core/managed/ScriptReflection.h"
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>

namespace cm::editor::worldgraph {

struct PortalLookupEntry {
    ClaymoreGUID Guid{};
    std::string Path;
    glm::vec3 EntryPosition{0.0f};
    glm::vec3 ExitPosition{0.0f};
};

struct PortalBakeInfo {
    std::string ScenePath;
    ClaymoreGUID PortalGuid{};
    std::string PortalPath;
    glm::vec3 EntryPosition{0.0f};
    std::string TargetScenePath;
    ClaymoreGUID TargetPortalGuid{};
    std::string TargetPortalPath;
    glm::vec3 ExitPosition{0.0f};
    bool Resolved = false;
};

struct PoiBakeInfo {
    std::string ScenePath;
    ClaymoreGUID EntityGuid{};
    std::string EntityPath;
    std::string ScriptClass;
    std::string NodeName;
    std::string NodeType;
    bool IsPortalHint = false;
    glm::vec3 Position{0.0f};
};

enum class LocalNodeKind : int {
    Portal = 0,
    Poi = 1
};

struct LocalDistanceBakeInfo {
    std::string ScenePath;
    LocalNodeKind FromKind = LocalNodeKind::Portal;
    ClaymoreGUID FromGuid{};
    std::string FromPath;
    LocalNodeKind ToKind = LocalNodeKind::Portal;
    ClaymoreGUID ToGuid{};
    std::string ToPath;
    float Distance = 0.0f;
};

static std::string BuildEntityPath(Scene& scene, EntityID id)
{
    std::vector<std::string> parts;
    EntityID cur = id;
    while (cur != INVALID_ENTITY_ID) {
        auto* data = scene.GetEntityData(cur);
        if (!data) break;
        parts.push_back(data->Name);
        cur = data->Parent;
    }
    std::reverse(parts.begin(), parts.end());
    std::string path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) path += "/";
        path += parts[i];
    }
    return path;
}

static glm::vec3 GetWorldPosition(const TransformComponent& t, const glm::vec3& localOffset)
{
    glm::vec4 world = t.WorldMatrix * glm::vec4(localOffset, 1.0f);
    return glm::vec3(world.x, world.y, world.z);
}

static float Distance3D(const glm::vec3& a, const glm::vec3& b)
{
    return glm::length(b - a);
}

static std::string NormalizeVPath(const std::filesystem::path& path, const std::filesystem::path& root)
{
    std::error_code ec;
    std::filesystem::path rel = std::filesystem::relative(path, root, ec);
    std::string vpath = ec ? path.string() : rel.string();
    std::replace(vpath.begin(), vpath.end(), '\\', '/');
    try { vpath = IVirtualFS::NormalizePath(vpath); } catch(...) {}
    return vpath;
}

static std::string NormalizeScenePath(const std::string& path, const std::filesystem::path& projectRoot)
{
    if (path.empty()) return std::string();
    std::filesystem::path p(path);
    std::string vpath = path;
    if (p.is_absolute()) {
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(p, projectRoot, ec);
        vpath = ec ? p.string() : rel.string();
    }
    std::replace(vpath.begin(), vpath.end(), '\\', '/');
    try { vpath = IVirtualFS::NormalizePath(vpath); } catch(...) {}
    return vpath;
}

static nlohmann::json WriteVec3(const glm::vec3& v)
{
    return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
}

static const char* KindToString(LocalNodeKind kind)
{
    return kind == LocalNodeKind::Poi ? "poi" : "portal";
}

static bool ShouldBakeScriptClass(const std::string& className)
{
    if (className.empty()) return false;
    // Backward-compat fallback for existing project script while users migrate to attribute.
    if (className == "LocationPOI") return true;
    return ScriptSystem::Instance().HasScriptFlag(className, ScriptSystem::ScriptType_BakeInWorldGraph);
}

static bool TryGetScriptString(const ScriptInstance& script, const char* key, std::string& out)
{
    auto it = script.Values.find(key);
    if (it == script.Values.end()) return false;
    if (std::holds_alternative<std::string>(it->second)) {
        out = std::get<std::string>(it->second);
        return true;
    }
    if (std::holds_alternative<int>(it->second)) {
        out = std::to_string(std::get<int>(it->second));
        return true;
    }
    if (std::holds_alternative<float>(it->second)) {
        out = std::to_string(std::get<float>(it->second));
        return true;
    }
    if (std::holds_alternative<bool>(it->second)) {
        out = std::get<bool>(it->second) ? "true" : "false";
        return true;
    }
    return false;
}

static bool TryGetScriptBool(const ScriptInstance& script, const char* key, bool& out)
{
    auto it = script.Values.find(key);
    if (it == script.Values.end()) return false;
    if (std::holds_alternative<bool>(it->second)) {
        out = std::get<bool>(it->second);
        return true;
    }
    if (std::holds_alternative<int>(it->second)) {
        out = std::get<int>(it->second) != 0;
        return true;
    }
    if (std::holds_alternative<std::string>(it->second)) {
        const std::string& s = std::get<std::string>(it->second);
        out = (s == "1" || s == "true" || s == "True" || s == "TRUE");
        return true;
    }
    return false;
}

static void ScanForScenes(const std::filesystem::path& root, const std::filesystem::path& projectDir, std::vector<std::string>& outScenes)
{
    namespace fs = std::filesystem;
    if (!fs::exists(root)) return;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() == ".scene") {
            outScenes.push_back(NormalizeVPath(e.path(), projectDir));
        }
    }
}

static std::vector<std::string> CollectAllProjectScenes(const std::filesystem::path& projectDir)
{
    namespace fs = std::filesystem;
    fs::path assetsDir = Project::GetAssetDirectory();
    if (assetsDir.empty()) assetsDir = projectDir / "assets";

    std::vector<std::string> scenes;
    ScanForScenes(assetsDir, projectDir, scenes);
    if (scenes.empty()) {
        ScanForScenes(projectDir / "scenes", projectDir, scenes);
        if (scenes.empty()) ScanForScenes(projectDir, projectDir, scenes);
    }
    std::sort(scenes.begin(), scenes.end());
    scenes.erase(std::unique(scenes.begin(), scenes.end()), scenes.end());
    return scenes;
}

static bool ReadExistingWorldGraph(const std::filesystem::path& path, nlohmann::json& out)
{
    out = nlohmann::json::object();
    if (!std::filesystem::exists(path)) return true;
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path.string(), text)) return false;
    try {
        out = nlohmann::json::parse(text);
        if (!out.is_object()) out = nlohmann::json::object();
        return true;
    } catch (...) {
        return false;
    }
}

static void AppendPortalJson(const PortalBakeInfo& portal, nlohmann::json& portalsArray)
{
    nlohmann::json jp;
    jp["scenePath"] = portal.ScenePath;
    jp["portalGuid"] = portal.PortalGuid;
    if (!portal.PortalPath.empty()) jp["portalPath"] = portal.PortalPath;
    jp["entry"] = WriteVec3(portal.EntryPosition);
    jp["targetScenePath"] = portal.TargetScenePath;
    if (portal.TargetPortalGuid != ClaymoreGUID()) jp["targetPortalGuid"] = portal.TargetPortalGuid;
    if (!portal.TargetPortalPath.empty()) jp["targetPortalPath"] = portal.TargetPortalPath;
    if (portal.Resolved) jp["exit"] = WriteVec3(portal.ExitPosition);
    jp["resolved"] = portal.Resolved;
    portalsArray.push_back(std::move(jp));
}

static void AppendPoiJson(const PoiBakeInfo& poi, nlohmann::json& poisArray)
{
    nlohmann::json jp;
    jp["scenePath"] = poi.ScenePath;
    jp["entityGuid"] = poi.EntityGuid;
    jp["entityPath"] = poi.EntityPath;
    jp["scriptClass"] = poi.ScriptClass;
    jp["nodeName"] = poi.NodeName;
    jp["nodeType"] = poi.NodeType;
    jp["isPortal"] = poi.IsPortalHint;
    jp["position"] = WriteVec3(poi.Position);
    poisArray.push_back(std::move(jp));
}

static void AppendDistanceJson(const LocalDistanceBakeInfo& edge, nlohmann::json& edgesArray)
{
    nlohmann::json je;
    je["scenePath"] = edge.ScenePath;
    je["fromKind"] = KindToString(edge.FromKind);
    je["fromGuid"] = edge.FromGuid;
    if (!edge.FromPath.empty()) je["fromPath"] = edge.FromPath;
    je["toKind"] = KindToString(edge.ToKind);
    je["toGuid"] = edge.ToGuid;
    if (!edge.ToPath.empty()) je["toPath"] = edge.ToPath;
    je["distance"] = edge.Distance;
    edgesArray.push_back(std::move(je));
}

static void BuildLocalDistances(
    const std::vector<PortalBakeInfo>& portals,
    const std::vector<PoiBakeInfo>& pois,
    std::vector<LocalDistanceBakeInfo>& outEdges)
{
    std::unordered_map<std::string, std::vector<size_t>> portalsByScene;
    std::unordered_map<std::string, std::vector<size_t>> poisByScene;

    for (size_t i = 0; i < portals.size(); ++i) {
        portalsByScene[portals[i].ScenePath].push_back(i);
    }
    for (size_t i = 0; i < pois.size(); ++i) {
        poisByScene[pois[i].ScenePath].push_back(i);
    }

    for (auto& [scenePath, portalIndices] : portalsByScene) {
        // Portal-portal local distances in same scene.
        for (size_t a = 0; a < portalIndices.size(); ++a) {
            const PortalBakeInfo& pa = portals[portalIndices[a]];
            for (size_t b = 0; b < portalIndices.size(); ++b) {
                if (a == b) continue;
                const PortalBakeInfo& pb = portals[portalIndices[b]];

                LocalDistanceBakeInfo edge;
                edge.ScenePath = scenePath;
                edge.FromKind = LocalNodeKind::Portal;
                edge.FromGuid = pa.PortalGuid;
                edge.FromPath = pa.PortalPath;
                edge.ToKind = LocalNodeKind::Portal;
                edge.ToGuid = pb.PortalGuid;
                edge.ToPath = pb.PortalPath;
                edge.Distance = Distance3D(pa.EntryPosition, pb.EntryPosition);
                outEdges.push_back(std::move(edge));
            }
        }

        auto itPois = poisByScene.find(scenePath);
        if (itPois == poisByScene.end()) continue;

        // Poi-portal and portal-poi local distances.
        for (size_t poiIndex : itPois->second) {
            const PoiBakeInfo& poi = pois[poiIndex];
            for (size_t portalIndex : portalIndices) {
                const PortalBakeInfo& portal = portals[portalIndex];
                const float d = Distance3D(poi.Position, portal.EntryPosition);

                LocalDistanceBakeInfo poiToPortal;
                poiToPortal.ScenePath = scenePath;
                poiToPortal.FromKind = LocalNodeKind::Poi;
                poiToPortal.FromGuid = poi.EntityGuid;
                poiToPortal.FromPath = poi.EntityPath;
                poiToPortal.ToKind = LocalNodeKind::Portal;
                poiToPortal.ToGuid = portal.PortalGuid;
                poiToPortal.ToPath = portal.PortalPath;
                poiToPortal.Distance = d;
                outEdges.push_back(std::move(poiToPortal));

                LocalDistanceBakeInfo portalToPoi;
                portalToPoi.ScenePath = scenePath;
                portalToPoi.FromKind = LocalNodeKind::Portal;
                portalToPoi.FromGuid = portal.PortalGuid;
                portalToPoi.FromPath = portal.PortalPath;
                portalToPoi.ToKind = LocalNodeKind::Poi;
                portalToPoi.ToGuid = poi.EntityGuid;
                portalToPoi.ToPath = poi.EntityPath;
                portalToPoi.Distance = d;
                outEdges.push_back(std::move(portalToPoi));
            }
        }
    }
}

static void ResolvePortalLinks(std::vector<PortalBakeInfo>& portals)
{
    std::unordered_map<std::string, std::unordered_map<ClaymoreGUID, PortalLookupEntry>> portalsByGuid;
    std::unordered_map<std::string, std::unordered_map<std::string, PortalLookupEntry>> portalsByPath;

    for (const auto& portal : portals) {
        PortalLookupEntry lookup;
        lookup.Guid = portal.PortalGuid;
        lookup.Path = portal.PortalPath;
        lookup.EntryPosition = portal.EntryPosition;
        lookup.ExitPosition = portal.ExitPosition;
        portalsByGuid[portal.ScenePath][lookup.Guid] = lookup;
        if (!lookup.Path.empty()) portalsByPath[portal.ScenePath][lookup.Path] = lookup;
    }

    for (auto& portal : portals) {
        portal.Resolved = false;
        if (portal.TargetScenePath.empty()) continue;

        const PortalLookupEntry* target = nullptr;
        auto itScene = portalsByGuid.find(portal.TargetScenePath);
        if (itScene == portalsByGuid.end()) continue;

        if (portal.TargetPortalGuid != ClaymoreGUID()) {
            auto itGuid = itScene->second.find(portal.TargetPortalGuid);
            if (itGuid != itScene->second.end()) target = &itGuid->second;
        }
        if (!target && !portal.TargetPortalPath.empty()) {
            auto itPathMap = portalsByPath.find(portal.TargetScenePath);
            if (itPathMap != portalsByPath.end()) {
                auto itPath = itPathMap->second.find(portal.TargetPortalPath);
                if (itPath != itPathMap->second.end()) target = &itPath->second;
            }
        }

        if (target) {
            portal.ExitPosition = target->ExitPosition;
            portal.Resolved = true;
        }
    }
}

static bool GatherSceneData(
    const std::string& scenePath,
    const std::filesystem::path& projectDir,
    std::vector<PortalBakeInfo>& outPortals,
    std::vector<PoiBakeInfo>& outPois,
    std::vector<std::string>* outConnectedScenes)
{
    Scene temp;
    if (!Serializer::LoadSceneFromFile(scenePath, temp)) {
        std::cerr << "[WorldGraphBake] Failed to load scene: " << scenePath << "\n";
        return false;
    }

    temp.UpdateTransforms();

    int portalCount = 0;
    int enabledPortalCount = 0;

    for (const auto& e : temp.GetEntities()) {
        auto* d = temp.GetEntityData(e.GetID());
        if (!d) continue;

        const std::string entityPath = BuildEntityPath(temp, e.GetID());

        if (d->Portal && d->Portal->Enabled) {
            PortalBakeInfo info;
            info.ScenePath = scenePath;
            info.PortalGuid = d->EntityGuid;
            info.PortalPath = entityPath;
            info.EntryPosition = GetWorldPosition(d->Transform, d->Portal->EntryOffset);
            info.TargetScenePath = NormalizeScenePath(d->Portal->TargetScenePath, projectDir);
            info.TargetPortalGuid = d->Portal->TargetPortalGuid;
            info.TargetPortalPath = d->Portal->TargetPortalPath;
            info.ExitPosition = GetWorldPosition(d->Transform, d->Portal->ExitOffset);
            outPortals.push_back(std::move(info));

            if (outConnectedScenes && !d->Portal->TargetScenePath.empty()) {
                const std::string normalizedTarget = NormalizeScenePath(d->Portal->TargetScenePath, projectDir);
                if (!normalizedTarget.empty()) outConnectedScenes->push_back(normalizedTarget);
            }
            enabledPortalCount++;
        }
        if (d->Portal) portalCount++;

        // Ensure script instances are available during bake if serializer left gaps.
        for (auto& script : d->Scripts) {
            if (!script.Instance) {
                auto recreated = ScriptSystem::Instance().Create(script.ClassName);
                if (recreated) script.Instance = recreated;
            }
            if (!ShouldBakeScriptClass(script.ClassName)) continue;

            PoiBakeInfo poi;
            poi.ScenePath = scenePath;
            poi.EntityGuid = d->EntityGuid;
            poi.EntityPath = entityPath;
            poi.ScriptClass = script.ClassName;
            poi.Position = GetWorldPosition(d->Transform, glm::vec3(0.0f));
            poi.NodeName = d->Name;
            poi.NodeType = "Other";

            std::string locationNodeName;
            if (TryGetScriptString(script, "locationNodeName", locationNodeName) && !locationNodeName.empty()) {
                poi.NodeName = locationNodeName;
            }

            std::string nodeType;
            if (TryGetScriptString(script, "nodeType", nodeType) && !nodeType.empty()) {
                poi.NodeType = nodeType;
            }

            bool isPortalHint = false;
            if (TryGetScriptBool(script, "isPortal", isPortalHint)) {
                poi.IsPortalHint = isPortalHint;
            }

            outPois.push_back(std::move(poi));
        }
    }

    std::cout << "[WorldGraphBake] Scene: " << scenePath
              << " portals=" << portalCount
              << " enabled=" << enabledPortalCount << "\n";
    return true;
}

static nlohmann::json BuildMergedWorldGraphJson(
    const nlohmann::json& existing,
    const std::unordered_set<std::string>& bakedScenes,
    const std::vector<PortalBakeInfo>& portals,
    const std::vector<PoiBakeInfo>& pois,
    const std::vector<LocalDistanceBakeInfo>& localDistances)
{
    nlohmann::json result = nlohmann::json::object();
    result["version"] = 2;
    result["portals"] = nlohmann::json::array();
    result["pois"] = nlohmann::json::array();
    result["localDistances"] = nlohmann::json::array();

    auto keepEntryForScene = [&](const nlohmann::json& entry) -> bool {
        if (!entry.is_object()) return false;
        const std::string scenePath = entry.value("scenePath", std::string());
        return bakedScenes.find(scenePath) == bakedScenes.end();
    };

    if (existing.contains("portals") && existing["portals"].is_array()) {
        for (const auto& entry : existing["portals"]) {
            if (keepEntryForScene(entry)) result["portals"].push_back(entry);
        }
    }
    if (existing.contains("pois") && existing["pois"].is_array()) {
        for (const auto& entry : existing["pois"]) {
            if (keepEntryForScene(entry)) result["pois"].push_back(entry);
        }
    }
    if (existing.contains("localDistances") && existing["localDistances"].is_array()) {
        for (const auto& entry : existing["localDistances"]) {
            if (keepEntryForScene(entry)) result["localDistances"].push_back(entry);
        }
    }

    for (const auto& portal : portals) AppendPortalJson(portal, result["portals"]);
    for (const auto& poi : pois) AppendPoiJson(poi, result["pois"]);
    for (const auto& edge : localDistances) AppendDistanceJson(edge, result["localDistances"]);

    return result;
}

static bool BakeWorldGraphInternal(const std::string* seedScenePath)
{
    namespace fs = std::filesystem;
    const fs::path projectDir = Project::GetProjectDirectory();
    if (projectDir.empty()) {
        std::cerr << "[WorldGraphBake] No project directory set.\n";
        return false;
    }

    std::vector<PortalBakeInfo> bakedPortals;
    std::vector<PoiBakeInfo> bakedPois;
    std::vector<LocalDistanceBakeInfo> bakedLocalDistances;
    std::unordered_set<std::string> bakedScenes;

    if (seedScenePath == nullptr) {
        auto allScenes = CollectAllProjectScenes(projectDir);
        std::cout << "[WorldGraphBake] Full bake, scene count=" << allScenes.size() << "\n";
        for (const std::string& scenePath : allScenes) {
            GatherSceneData(scenePath, projectDir, bakedPortals, bakedPois, nullptr);
            bakedScenes.insert(scenePath);
        }
    } else {
        const std::string normalizedSeed = NormalizeScenePath(*seedScenePath, projectDir);
        if (normalizedSeed.empty()) {
            std::cerr << "[WorldGraphBake] Invalid seed scene path.\n";
            return false;
        }

        std::queue<std::string> pending;
        pending.push(normalizedSeed);

        std::cout << "[WorldGraphBake] Seed bake from: " << normalizedSeed << "\n";
        while (!pending.empty()) {
            const std::string scenePath = pending.front();
            pending.pop();
            if (bakedScenes.find(scenePath) != bakedScenes.end()) continue;

            std::vector<std::string> connected;
            if (!GatherSceneData(scenePath, projectDir, bakedPortals, bakedPois, &connected)) {
                continue;
            }
            bakedScenes.insert(scenePath);
            for (const std::string& next : connected) {
                if (!next.empty() && bakedScenes.find(next) == bakedScenes.end()) {
                    pending.push(next);
                }
            }
        }
    }

    ResolvePortalLinks(bakedPortals);
    BuildLocalDistances(bakedPortals, bakedPois, bakedLocalDistances);

    fs::path outFile = projectDir / ".bin" / "world" / "worldgraph.json";
    fs::create_directories(outFile.parent_path());

    nlohmann::json existing;
    if (!ReadExistingWorldGraph(outFile, existing)) {
        std::cerr << "[WorldGraphBake] Failed to read existing world graph. Aborting.\n";
        return false;
    }

    nlohmann::json merged = BuildMergedWorldGraphJson(existing, bakedScenes, bakedPortals, bakedPois, bakedLocalDistances);
    const std::string payload = merged.dump(4);
    if (!FileSystem::Instance().WriteTextFile(outFile.string(), payload)) {
        std::cerr << "[WorldGraphBake] Failed to write: " << outFile.string() << "\n";
        return false;
    }

    cm::world::WorldGraph::Get().LoadFromJson(merged);
    std::cout << "[WorldGraphBake] Wrote world graph to: " << outFile.string()
              << " portals=" << bakedPortals.size()
              << " pois=" << bakedPois.size()
              << " localDistances=" << bakedLocalDistances.size()
              << " updatedScenes=" << bakedScenes.size() << "\n";
    return true;
}

bool BakeWorldGraph()
{
    return BakeWorldGraphInternal(nullptr);
}

bool BakeWorldGraphFromScene(const std::string& seedScenePath)
{
    return BakeWorldGraphInternal(&seedScenePath);
}

} // namespace cm::editor::worldgraph
