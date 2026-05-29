#include "WorldGraphInterop.h"
#include "WorldGraph.h"
#include "core/vfs/VirtualFS.h"

namespace {
    static constexpr int kNumStringBuffers = 8;
    std::string& GetRotatingStringBuffer() {
        thread_local std::string s_Buffers[kNumStringBuffers];
        thread_local int s_Current = 0;
        s_Current = (s_Current + 1) % kNumStringBuffers;
        return s_Buffers[s_Current];
    }

    void EnsureLoaded() {
        auto& graph = cm::world::WorldGraph::Get();
        if (!graph.IsLoaded()) {
            graph.LoadProjectGraph();
        }
    }
}

bool WorldGraph_LoadProject()
{
    return cm::world::WorldGraph::Get().LoadProjectGraph();
}

bool WorldGraph_IsLoaded()
{
    return cm::world::WorldGraph::Get().IsLoaded();
}

int WorldGraph_GetPortalCount()
{
    auto& graph = cm::world::WorldGraph::Get();
    if (!graph.IsLoaded()) {
        graph.LoadProjectGraph();
    }
    return graph.GetPortalCount();
}

const char* WorldGraph_GetPortalScenePath(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = portal->ScenePath;
    return buf.c_str();
}

const char* WorldGraph_GetPortalGuid(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = portal->PortalGuid.ToString();
    return buf.c_str();
}

const char* WorldGraph_GetPortalTargetScenePath(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = portal->TargetScenePath;
    return buf.c_str();
}

const char* WorldGraph_GetPortalTargetGuid(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = portal->TargetPortalGuid.ToString();
    return buf.c_str();
}

const char* WorldGraph_GetPortalPath(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = portal->PortalPath;
    return buf.c_str();
}

const char* WorldGraph_GetPortalTargetPath(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = portal->TargetPortalPath;
    return buf.c_str();
}

void WorldGraph_GetPortalEntryPosition(int index, float* x, float* y, float* z)
{
    EnsureLoaded();
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (z) *z = 0.0f;
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return;
    if (x) *x = portal->EntryPosition.x;
    if (y) *y = portal->EntryPosition.y;
    if (z) *z = portal->EntryPosition.z;
}

void WorldGraph_GetPortalExitPosition(int index, float* x, float* y, float* z)
{
    EnsureLoaded();
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (z) *z = 0.0f;
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return;
    if (x) *x = portal->ExitPosition.x;
    if (y) *y = portal->ExitPosition.y;
    if (z) *z = portal->ExitPosition.z;
}

bool WorldGraph_IsPortalResolved(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    if (!portal) return false;
    return portal->Resolved;
}

float WorldGraph_GetPortalDistance(int index)
{
    EnsureLoaded();
    auto* portal = cm::world::WorldGraph::Get().GetPortal(index);
    return (portal && portal->HasDistance) ? portal->Distance : 0.0f;
}

int WorldGraph_FindPortalIndex(const char* scenePath, const char* portalGuid)
{
    EnsureLoaded();
    if (!scenePath || !portalGuid) return -1;
    std::string scene = scenePath;
    std::string guidStr = portalGuid;
    if (guidStr.empty()) return -1;
    if (!scene.empty()) {
        try { scene = IVirtualFS::NormalizePath(scene); } catch(...) {}
    }
    ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
    auto& graph = cm::world::WorldGraph::Get();
    const int count = graph.GetPortalCount();
    for (int i = 0; i < count; ++i) {
        const auto* portal = graph.GetPortal(i);
        if (!portal) continue;
        if (!scene.empty() && portal->ScenePath != scene) continue;
        if (portal->PortalGuid == guid) return i;
    }
    return -1;
}

int WorldGraph_GetPoiCount()
{
    EnsureLoaded();
    return cm::world::WorldGraph::Get().GetPoiCount();
}

const char* WorldGraph_GetPoiScenePath(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = poi->ScenePath;
    return buf.c_str();
}

const char* WorldGraph_GetPoiGuid(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = poi->EntityGuid.ToString();
    return buf.c_str();
}

const char* WorldGraph_GetPoiPath(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = poi->EntityPath;
    return buf.c_str();
}

const char* WorldGraph_GetPoiScriptClass(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = poi->ScriptClass;
    return buf.c_str();
}

const char* WorldGraph_GetPoiNodeName(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = poi->NodeName;
    return buf.c_str();
}

const char* WorldGraph_GetPoiNodeType(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = poi->NodeType;
    return buf.c_str();
}

bool WorldGraph_GetPoiIsPortal(int index)
{
    EnsureLoaded();
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return false;
    return poi->IsPortal;
}

void WorldGraph_GetPoiPosition(int index, float* x, float* y, float* z)
{
    EnsureLoaded();
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (z) *z = 0.0f;
    auto* poi = cm::world::WorldGraph::Get().GetPoi(index);
    if (!poi) return;
    if (x) *x = poi->Position.x;
    if (y) *y = poi->Position.y;
    if (z) *z = poi->Position.z;
}

int WorldGraph_FindPoiIndex(const char* scenePath, const char* poiGuid)
{
    EnsureLoaded();
    if (!poiGuid) return -1;
    std::string scene = scenePath ? scenePath : "";
    if (!scene.empty()) {
        try { scene = IVirtualFS::NormalizePath(scene); } catch(...) {}
    }
    ClaymoreGUID guid = ClaymoreGUID::FromString(std::string(poiGuid));
    return cm::world::WorldGraph::Get().FindPoiIndex(scene, guid);
}

float WorldGraph_GetPoiToPortalDistance(const char* scenePath, const char* poiGuid, const char* portalGuid)
{
    EnsureLoaded();
    if (!poiGuid || !portalGuid) return -1.0f;
    std::string scene = scenePath ? scenePath : "";
    if (!scene.empty()) {
        try { scene = IVirtualFS::NormalizePath(scene); } catch(...) {}
    }
    ClaymoreGUID poi = ClaymoreGUID::FromString(std::string(poiGuid));
    ClaymoreGUID portal = ClaymoreGUID::FromString(std::string(portalGuid));
    return cm::world::WorldGraph::Get().FindPoiToPortalDistance(scene, poi, portal);
}

float WorldGraph_GetPortalToPoiDistance(const char* scenePath, const char* portalGuid, const char* poiGuid)
{
    EnsureLoaded();
    if (!portalGuid || !poiGuid) return -1.0f;
    std::string scene = scenePath ? scenePath : "";
    if (!scene.empty()) {
        try { scene = IVirtualFS::NormalizePath(scene); } catch(...) {}
    }
    ClaymoreGUID portal = ClaymoreGUID::FromString(std::string(portalGuid));
    ClaymoreGUID poi = ClaymoreGUID::FromString(std::string(poiGuid));
    return cm::world::WorldGraph::Get().FindPortalToPoiDistance(scene, portal, poi);
}

float WorldGraph_GetPortalToPortalDistance(const char* scenePath, const char* fromPortalGuid, const char* toPortalGuid)
{
    EnsureLoaded();
    if (!fromPortalGuid || !toPortalGuid) return -1.0f;
    std::string scene = scenePath ? scenePath : "";
    if (!scene.empty()) {
        try { scene = IVirtualFS::NormalizePath(scene); } catch(...) {}
    }
    ClaymoreGUID from = ClaymoreGUID::FromString(std::string(fromPortalGuid));
    ClaymoreGUID to = ClaymoreGUID::FromString(std::string(toPortalGuid));
    return cm::world::WorldGraph::Get().FindPortalToPortalDistance(scene, from, to);
}

extern "C" __declspec(dllexport) void* Get_WorldGraph_LoadProject_Ptr() { return (void*)WorldGraph_LoadProject; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_IsLoaded_Ptr() { return (void*)WorldGraph_IsLoaded; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalCount_Ptr() { return (void*)WorldGraph_GetPortalCount; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalScenePath_Ptr() { return (void*)WorldGraph_GetPortalScenePath; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalGuid_Ptr() { return (void*)WorldGraph_GetPortalGuid; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalTargetScenePath_Ptr() { return (void*)WorldGraph_GetPortalTargetScenePath; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalTargetGuid_Ptr() { return (void*)WorldGraph_GetPortalTargetGuid; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalPath_Ptr() { return (void*)WorldGraph_GetPortalPath; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalTargetPath_Ptr() { return (void*)WorldGraph_GetPortalTargetPath; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalEntryPosition_Ptr() { return (void*)WorldGraph_GetPortalEntryPosition; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalExitPosition_Ptr() { return (void*)WorldGraph_GetPortalExitPosition; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_IsPortalResolved_Ptr() { return (void*)WorldGraph_IsPortalResolved; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalDistance_Ptr() { return (void*)WorldGraph_GetPortalDistance; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_FindPortalIndex_Ptr() { return (void*)WorldGraph_FindPortalIndex; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiCount_Ptr() { return (void*)WorldGraph_GetPoiCount; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiScenePath_Ptr() { return (void*)WorldGraph_GetPoiScenePath; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiGuid_Ptr() { return (void*)WorldGraph_GetPoiGuid; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiPath_Ptr() { return (void*)WorldGraph_GetPoiPath; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiScriptClass_Ptr() { return (void*)WorldGraph_GetPoiScriptClass; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiNodeName_Ptr() { return (void*)WorldGraph_GetPoiNodeName; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiNodeType_Ptr() { return (void*)WorldGraph_GetPoiNodeType; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiIsPortal_Ptr() { return (void*)WorldGraph_GetPoiIsPortal; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiPosition_Ptr() { return (void*)WorldGraph_GetPoiPosition; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_FindPoiIndex_Ptr() { return (void*)WorldGraph_FindPoiIndex; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiToPortalDistance_Ptr() { return (void*)WorldGraph_GetPoiToPortalDistance; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalToPoiDistance_Ptr() { return (void*)WorldGraph_GetPortalToPoiDistance; }
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalToPortalDistance_Ptr() { return (void*)WorldGraph_GetPortalToPortalDistance; }
