#pragma once

#include <cstdint>
#include <vector>
#include <utility>
#include "core/navigation/NavTypes.h"

namespace nav { class NavMeshRuntime; struct NavMeshComponent; struct NavAgentComponent; }

namespace nav::debug
{
    void SetMask(NavDrawMask mask);
    NavDrawMask GetMask();
    
    // Debug draw vertical offset for visibility above terrain
    void SetOffset(float offset);
    float GetOffset();
    
    // Set max distance for debug drawing (0 = unlimited)
    void SetDrawDistance(float distance);
    float GetDrawDistance();
    
    // Call when a navmesh is destroyed to free cached GPU buffers
    void InvalidateCache(const NavMeshRuntime* rt);

    // Draw navmesh with camera-based culling
    // cameraPos: used to cull triangles beyond draw distance
    void DrawRuntime(const NavMeshRuntime& rt, const glm::vec3& cameraPos, uint16_t viewId = 3);
    void DrawPath(const NavPath& path, uint16_t viewId = 3);
    void DrawAgent(const NavAgentComponent& agent, const glm::vec3& pos, const glm::vec3& vel, uint16_t viewId = 3);
    void DrawPortals(const std::vector<std::pair<glm::vec3, glm::vec3>>& portals, uint16_t viewId = 3);
}


