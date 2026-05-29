#pragma once
#include <cstdint>

extern "C" {
    __declspec(dllexport) bool WorldGraph_LoadProject();
    __declspec(dllexport) bool WorldGraph_IsLoaded();
    __declspec(dllexport) int WorldGraph_GetPortalCount();
    __declspec(dllexport) const char* WorldGraph_GetPortalScenePath(int index);
    __declspec(dllexport) const char* WorldGraph_GetPortalGuid(int index);
    __declspec(dllexport) const char* WorldGraph_GetPortalTargetScenePath(int index);
    __declspec(dllexport) const char* WorldGraph_GetPortalTargetGuid(int index);
    __declspec(dllexport) const char* WorldGraph_GetPortalPath(int index);
    __declspec(dllexport) const char* WorldGraph_GetPortalTargetPath(int index);
    __declspec(dllexport) void WorldGraph_GetPortalEntryPosition(int index, float* x, float* y, float* z);
    __declspec(dllexport) void WorldGraph_GetPortalExitPosition(int index, float* x, float* y, float* z);
    __declspec(dllexport) bool WorldGraph_IsPortalResolved(int index);
    __declspec(dllexport) float WorldGraph_GetPortalDistance(int index);
    __declspec(dllexport) int WorldGraph_FindPortalIndex(const char* scenePath, const char* portalGuid);
    __declspec(dllexport) int WorldGraph_GetPoiCount();
    __declspec(dllexport) const char* WorldGraph_GetPoiScenePath(int index);
    __declspec(dllexport) const char* WorldGraph_GetPoiGuid(int index);
    __declspec(dllexport) const char* WorldGraph_GetPoiPath(int index);
    __declspec(dllexport) const char* WorldGraph_GetPoiScriptClass(int index);
    __declspec(dllexport) const char* WorldGraph_GetPoiNodeName(int index);
    __declspec(dllexport) const char* WorldGraph_GetPoiNodeType(int index);
    __declspec(dllexport) bool WorldGraph_GetPoiIsPortal(int index);
    __declspec(dllexport) void WorldGraph_GetPoiPosition(int index, float* x, float* y, float* z);
    __declspec(dllexport) int WorldGraph_FindPoiIndex(const char* scenePath, const char* poiGuid);
    __declspec(dllexport) float WorldGraph_GetPoiToPortalDistance(const char* scenePath, const char* poiGuid, const char* portalGuid);
    __declspec(dllexport) float WorldGraph_GetPortalToPoiDistance(const char* scenePath, const char* portalGuid, const char* poiGuid);
    __declspec(dllexport) float WorldGraph_GetPortalToPortalDistance(const char* scenePath, const char* fromPortalGuid, const char* toPortalGuid);
}

extern "C" __declspec(dllexport) void* Get_WorldGraph_LoadProject_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_IsLoaded_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalCount_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalScenePath_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalGuid_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalTargetScenePath_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalTargetGuid_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalPath_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalTargetPath_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalEntryPosition_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalExitPosition_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_IsPortalResolved_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalDistance_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_FindPortalIndex_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiCount_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiScenePath_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiGuid_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiPath_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiScriptClass_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiNodeName_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiNodeType_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiIsPortal_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiPosition_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_FindPoiIndex_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPoiToPortalDistance_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalToPoiDistance_Ptr();
extern "C" __declspec(dllexport) void* Get_WorldGraph_GetPortalToPortalDistance_Ptr();
