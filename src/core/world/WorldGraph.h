#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include "core/assets/AssetReference.h"

namespace cm::world {

struct WorldGraphPortal
{
    std::string ScenePath;
    ClaymoreGUID PortalGuid{};
    std::string PortalPath;
    glm::vec3 EntryPosition{0.0f};

    std::string TargetScenePath;
    ClaymoreGUID TargetPortalGuid{};
    std::string TargetPortalPath;
    glm::vec3 ExitPosition{0.0f};

    bool Resolved = false;
    float Distance = 0.0f;
    bool HasDistance = false;
    bool Runtime = false;
    bool RuntimeTouched = false;
};

enum class WorldGraphNodeKind : int
{
    Portal = 0,
    Poi = 1
};

struct WorldGraphPoi
{
    std::string ScenePath;
    ClaymoreGUID EntityGuid{};
    std::string EntityPath;
    std::string ScriptClass;
    std::string NodeName;
    std::string NodeType;
    bool IsPortal = false;
    glm::vec3 Position{0.0f};
};

struct WorldGraphLocalDistance
{
    std::string ScenePath;
    WorldGraphNodeKind FromKind = WorldGraphNodeKind::Portal;
    ClaymoreGUID FromGuid{};
    std::string FromPath;
    WorldGraphNodeKind ToKind = WorldGraphNodeKind::Portal;
    ClaymoreGUID ToGuid{};
    std::string ToPath;
    float Distance = 0.0f;
};

class WorldGraph
{
public:
    static WorldGraph& Get()
    {
        static WorldGraph s_Instance;
        return s_Instance;
    }

    void Clear();
    void ClearRuntime();
    bool LoadFromJson(const nlohmann::json& j);
    bool LoadFromFile(const std::string& path);
    bool LoadProjectGraph();

    const std::vector<WorldGraphPortal>& GetPortals() const { return m_Portals; }
    const std::vector<WorldGraphPortal>& GetRuntimePortals() const { return m_RuntimePortals; }
    int GetPortalCount() const { return static_cast<int>(m_Portals.size() + m_RuntimePortals.size()); }
    const std::vector<WorldGraphPoi>& GetPois() const { return m_Pois; }
    int GetPoiCount() const { return static_cast<int>(m_Pois.size()); }
    const WorldGraphPoi* GetPoi(int index) const;
    int FindPoiIndex(const std::string& scenePath, const ClaymoreGUID& poiGuid) const;
    float FindLocalDistance(const std::string& scenePath, WorldGraphNodeKind fromKind, const ClaymoreGUID& fromGuid,
                            WorldGraphNodeKind toKind, const ClaymoreGUID& toGuid) const;
    float FindPoiToPortalDistance(const std::string& scenePath, const ClaymoreGUID& poiGuid, const ClaymoreGUID& portalGuid) const;
    float FindPortalToPoiDistance(const std::string& scenePath, const ClaymoreGUID& portalGuid, const ClaymoreGUID& poiGuid) const;
    float FindPortalToPortalDistance(const std::string& scenePath, const ClaymoreGUID& fromPortalGuid, const ClaymoreGUID& toPortalGuid) const;
    bool IsLoaded() const { return m_Loaded; }
    const WorldGraphPortal* GetPortal(int index) const;
    const WorldGraphPortal* FindPortal(const std::string& scenePath, const ClaymoreGUID& portalGuid) const;
    const WorldGraphPortal* FindPortalByPath(const std::string& scenePath, const std::string& portalPath) const;

    void BeginRuntimeUpdate();
    void UpsertRuntimePortal(WorldGraphPortal portal);
    void FinalizeRuntimeUpdate();

    static std::string BuildDefaultAssetPath();

private:
    static std::string BuildRuntimeKey(const std::string& scenePath, const ClaymoreGUID& guid, const std::string& portalPath);
    const WorldGraphPortal* FindPortalInternal(const std::string& scenePath, const ClaymoreGUID& portalGuid, const std::string& portalPath) const;
    void ResolvePortalLink(WorldGraphPortal& portal);
    void UpdatePortalDistance(WorldGraphPortal& portal);

    std::vector<WorldGraphPortal> m_Portals;
    std::vector<WorldGraphPoi> m_Pois;
    std::vector<WorldGraphLocalDistance> m_LocalDistances;
    std::vector<WorldGraphPortal> m_RuntimePortals;
    std::unordered_map<std::string, size_t> m_RuntimeLookup;
    bool m_Loaded = false;
};

} // namespace cm::world
