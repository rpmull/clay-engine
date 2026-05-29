#include "WorldGraph.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#ifdef CLAYMORE_EDITOR
#include "editor/Project.h"
#endif

namespace cm::world {

static glm::vec3 ReadVec3(const nlohmann::json& j)
{
    glm::vec3 v{0.0f};
    if (!j.is_object()) return v;
    v.x = j.value("x", 0.0f);
    v.y = j.value("y", 0.0f);
    v.z = j.value("z", 0.0f);
    return v;
}

static WorldGraphNodeKind ReadNodeKind(const nlohmann::json& j, const char* key, WorldGraphNodeKind fallback)
{
    if (!j.contains(key)) return fallback;
    if (j[key].is_string()) {
        const std::string value = j.value(key, std::string("portal"));
        if (value == "poi") return WorldGraphNodeKind::Poi;
        return WorldGraphNodeKind::Portal;
    }
    if (j[key].is_number_integer()) {
        int v = j.value(key, static_cast<int>(fallback));
        return (v == static_cast<int>(WorldGraphNodeKind::Poi)) ? WorldGraphNodeKind::Poi : WorldGraphNodeKind::Portal;
    }
    return fallback;
}

void WorldGraph::Clear()
{
    m_Portals.clear();
    m_Pois.clear();
    m_LocalDistances.clear();
    m_RuntimePortals.clear();
    m_RuntimeLookup.clear();
    m_Loaded = false;
}

void WorldGraph::ClearRuntime()
{
    m_RuntimePortals.clear();
    m_RuntimeLookup.clear();
}

const WorldGraphPoi* WorldGraph::GetPoi(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Pois.size())) return nullptr;
    return &m_Pois[index];
}

int WorldGraph::FindPoiIndex(const std::string& scenePath, const ClaymoreGUID& poiGuid) const
{
    if (poiGuid == ClaymoreGUID()) return -1;
    for (int i = 0; i < static_cast<int>(m_Pois.size()); ++i) {
        const auto& poi = m_Pois[i];
        if (!scenePath.empty() && poi.ScenePath != scenePath) continue;
        if (poi.EntityGuid == poiGuid) return i;
    }
    return -1;
}

float WorldGraph::FindLocalDistance(const std::string& scenePath, WorldGraphNodeKind fromKind, const ClaymoreGUID& fromGuid,
                                    WorldGraphNodeKind toKind, const ClaymoreGUID& toGuid) const
{
    if (fromGuid == ClaymoreGUID() || toGuid == ClaymoreGUID()) return -1.0f;
    for (const auto& edge : m_LocalDistances) {
        if (!scenePath.empty() && edge.ScenePath != scenePath) continue;
        if (edge.FromKind != fromKind || edge.ToKind != toKind) continue;
        if (edge.FromGuid != fromGuid || edge.ToGuid != toGuid) continue;
        return edge.Distance;
    }
    return -1.0f;
}

float WorldGraph::FindPoiToPortalDistance(const std::string& scenePath, const ClaymoreGUID& poiGuid, const ClaymoreGUID& portalGuid) const
{
    return FindLocalDistance(scenePath, WorldGraphNodeKind::Poi, poiGuid, WorldGraphNodeKind::Portal, portalGuid);
}

float WorldGraph::FindPortalToPoiDistance(const std::string& scenePath, const ClaymoreGUID& portalGuid, const ClaymoreGUID& poiGuid) const
{
    return FindLocalDistance(scenePath, WorldGraphNodeKind::Portal, portalGuid, WorldGraphNodeKind::Poi, poiGuid);
}

float WorldGraph::FindPortalToPortalDistance(const std::string& scenePath, const ClaymoreGUID& fromPortalGuid, const ClaymoreGUID& toPortalGuid) const
{
    return FindLocalDistance(scenePath, WorldGraphNodeKind::Portal, fromPortalGuid, WorldGraphNodeKind::Portal, toPortalGuid);
}

const WorldGraphPortal* WorldGraph::GetPortal(int index) const
{
    if (index < 0) return nullptr;
    if (index < static_cast<int>(m_Portals.size())) {
        return &m_Portals[index];
    }
    int runtimeIndex = index - static_cast<int>(m_Portals.size());
    if (runtimeIndex < 0 || runtimeIndex >= static_cast<int>(m_RuntimePortals.size())) return nullptr;
    return &m_RuntimePortals[runtimeIndex];
}

const WorldGraphPortal* WorldGraph::FindPortal(const std::string& scenePath, const ClaymoreGUID& portalGuid) const
{
    return FindPortalInternal(scenePath, portalGuid, std::string());
}

const WorldGraphPortal* WorldGraph::FindPortalByPath(const std::string& scenePath, const std::string& portalPath) const
{
    return FindPortalInternal(scenePath, ClaymoreGUID(), portalPath);
}

std::string WorldGraph::BuildDefaultAssetPath()
{
    std::filesystem::path p = ".bin";
    p /= "world";
    p /= "worldgraph.json";
    return p.string();
}

bool WorldGraph::LoadFromJson(const nlohmann::json& j)
{
    if (!j.is_object()) return false;
    m_Portals.clear();
    m_Pois.clear();
    m_LocalDistances.clear();
    ClearRuntime();

    if (j.contains("portals") && j["portals"].is_array()) {
        for (const auto& entry : j["portals"]) {
            if (!entry.is_object()) continue;
            WorldGraphPortal portal;
            portal.ScenePath = entry.value("scenePath", std::string());
            if (!portal.ScenePath.empty()) {
                try { portal.ScenePath = IVirtualFS::NormalizePath(portal.ScenePath); } catch(...) {}
            }
            if (entry.contains("portalGuid")) {
                try { entry.at("portalGuid").get_to(portal.PortalGuid); } catch(...) {}
            }
            portal.PortalPath = entry.value("portalPath", std::string());
            if (entry.contains("entry")) portal.EntryPosition = ReadVec3(entry["entry"]);

            portal.TargetScenePath = entry.value("targetScenePath", std::string());
            if (!portal.TargetScenePath.empty()) {
                try { portal.TargetScenePath = IVirtualFS::NormalizePath(portal.TargetScenePath); } catch(...) {}
            }
            if (entry.contains("targetPortalGuid")) {
                try { entry.at("targetPortalGuid").get_to(portal.TargetPortalGuid); } catch(...) {}
            }
            portal.TargetPortalPath = entry.value("targetPortalPath", std::string());
            if (entry.contains("exit")) portal.ExitPosition = ReadVec3(entry["exit"]);
            portal.Resolved = entry.value("resolved", false);
            portal.Runtime = false;
            portal.RuntimeTouched = false;
            UpdatePortalDistance(portal);

            m_Portals.push_back(std::move(portal));
        }
    }

    if (j.contains("pois") && j["pois"].is_array()) {
        for (const auto& entry : j["pois"]) {
            if (!entry.is_object()) continue;
            WorldGraphPoi poi;
            poi.ScenePath = entry.value("scenePath", std::string());
            if (!poi.ScenePath.empty()) {
                try { poi.ScenePath = IVirtualFS::NormalizePath(poi.ScenePath); } catch(...) {}
            }
            if (entry.contains("entityGuid")) {
                try { entry.at("entityGuid").get_to(poi.EntityGuid); } catch(...) {}
            }
            poi.EntityPath = entry.value("entityPath", std::string());
            poi.ScriptClass = entry.value("scriptClass", std::string());
            poi.NodeName = entry.value("nodeName", std::string());
            poi.NodeType = entry.value("nodeType", std::string());
            poi.IsPortal = entry.value("isPortal", false);
            if (entry.contains("position")) poi.Position = ReadVec3(entry["position"]);
            m_Pois.push_back(std::move(poi));
        }
    }

    if (j.contains("localDistances") && j["localDistances"].is_array()) {
        for (const auto& entry : j["localDistances"]) {
            if (!entry.is_object()) continue;
            WorldGraphLocalDistance edge;
            edge.ScenePath = entry.value("scenePath", std::string());
            if (!edge.ScenePath.empty()) {
                try { edge.ScenePath = IVirtualFS::NormalizePath(edge.ScenePath); } catch(...) {}
            }
            edge.FromKind = ReadNodeKind(entry, "fromKind", WorldGraphNodeKind::Portal);
            if (entry.contains("fromGuid")) {
                try { entry.at("fromGuid").get_to(edge.FromGuid); } catch(...) {}
            }
            edge.FromPath = entry.value("fromPath", std::string());
            edge.ToKind = ReadNodeKind(entry, "toKind", WorldGraphNodeKind::Portal);
            if (entry.contains("toGuid")) {
                try { entry.at("toGuid").get_to(edge.ToGuid); } catch(...) {}
            }
            edge.ToPath = entry.value("toPath", std::string());
            edge.Distance = entry.value("distance", 0.0f);
            m_LocalDistances.push_back(std::move(edge));
        }
    }

    m_Loaded = true;
    return true;
}

bool WorldGraph::LoadFromFile(const std::string& path)
{
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        std::vector<uint8_t> bytes;
        if (!FileSystem::Instance().ReadFile(path, bytes)) {
            return false;
        }
        text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    try {
        nlohmann::json j = nlohmann::json::parse(text);
        return LoadFromJson(j);
    } catch (...) {
        return false;
    }
}

bool WorldGraph::LoadProjectGraph()
{
    const std::string path = BuildDefaultAssetPath();
    if (LoadFromFile(path)) {
        return true;
    }
#ifdef CLAYMORE_EDITOR
    try {
        const std::filesystem::path projectDir = Project::GetProjectDirectory();
        if (!projectDir.empty()) {
            const std::filesystem::path projectPath = projectDir / path;
            if (LoadFromFile(projectPath.string())) {
                return true;
            }
        }
    } catch (...) {}
#endif
    return false;
}

std::string WorldGraph::BuildRuntimeKey(const std::string& scenePath, const ClaymoreGUID& guid, const std::string& portalPath)
{
    std::string key = scenePath;
    key.push_back('|');
    if (guid != ClaymoreGUID()) {
        key += guid.ToString();
    } else {
        key += portalPath;
    }
    return key;
}

const WorldGraphPortal* WorldGraph::FindPortalInternal(const std::string& scenePath, const ClaymoreGUID& portalGuid, const std::string& portalPath) const
{
    auto matchesScene = [&](const std::string& candidate) {
        if (scenePath.empty()) return true;
        return candidate == scenePath;
    };

    if (portalGuid != ClaymoreGUID()) {
        for (const auto& portal : m_RuntimePortals) {
            if (matchesScene(portal.ScenePath) && portal.PortalGuid == portalGuid) return &portal;
        }
        for (const auto& portal : m_Portals) {
            if (matchesScene(portal.ScenePath) && portal.PortalGuid == portalGuid) return &portal;
        }
    }

    if (!portalPath.empty()) {
        for (const auto& portal : m_RuntimePortals) {
            if (matchesScene(portal.ScenePath) && portal.PortalPath == portalPath) return &portal;
        }
        for (const auto& portal : m_Portals) {
            if (matchesScene(portal.ScenePath) && portal.PortalPath == portalPath) return &portal;
        }
    }

    return nullptr;
}

void WorldGraph::ResolvePortalLink(WorldGraphPortal& portal)
{
    portal.Resolved = false;
    portal.ExitPosition = glm::vec3(0.0f);

    if (portal.TargetScenePath.empty() && portal.TargetPortalGuid == ClaymoreGUID() && portal.TargetPortalPath.empty()) {
        UpdatePortalDistance(portal);
        return;
    }

    const WorldGraphPortal* target = FindPortalInternal(portal.TargetScenePath, portal.TargetPortalGuid, portal.TargetPortalPath);
    if (target) {
        portal.ExitPosition = target->ExitPosition;
        portal.Resolved = true;
    }

    UpdatePortalDistance(portal);
}

void WorldGraph::UpdatePortalDistance(WorldGraphPortal& portal)
{
    portal.HasDistance = portal.Resolved;
    portal.Distance = portal.Resolved ? glm::length(portal.ExitPosition - portal.EntryPosition) : 0.0f;
}

void WorldGraph::BeginRuntimeUpdate()
{
    for (auto& portal : m_RuntimePortals) {
        portal.RuntimeTouched = false;
    }
}

void WorldGraph::UpsertRuntimePortal(WorldGraphPortal portal)
{
    m_Loaded = true;
    portal.Runtime = true;
    portal.RuntimeTouched = true;

    const std::string key = BuildRuntimeKey(portal.ScenePath, portal.PortalGuid, portal.PortalPath);
    if (!key.empty()) {
        auto it = m_RuntimeLookup.find(key);
        if (it != m_RuntimeLookup.end() && it->second < m_RuntimePortals.size()) {
            WorldGraphPortal& existing = m_RuntimePortals[it->second];
            portal.Runtime = true;
            portal.RuntimeTouched = true;
            ResolvePortalLink(portal);
            existing = std::move(portal);
            return;
        }
    }

    ResolvePortalLink(portal);
    m_RuntimePortals.push_back(std::move(portal));
    if (!key.empty()) {
        m_RuntimeLookup[key] = m_RuntimePortals.size() - 1;
    }
}

void WorldGraph::FinalizeRuntimeUpdate()
{
    if (m_RuntimePortals.empty()) return;

    std::vector<WorldGraphPortal> kept;
    kept.reserve(m_RuntimePortals.size());
    m_RuntimeLookup.clear();

    for (auto& portal : m_RuntimePortals) {
        if (!portal.RuntimeTouched) continue;
        const std::string key = BuildRuntimeKey(portal.ScenePath, portal.PortalGuid, portal.PortalPath);
        kept.push_back(std::move(portal));
        if (!key.empty()) {
            m_RuntimeLookup[key] = kept.size() - 1;
        }
    }

    m_RuntimePortals.swap(kept);
}

} // namespace cm::world
