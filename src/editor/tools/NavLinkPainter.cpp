#include "editor/tools/NavLinkPainter.h"
#include "core/rendering/Renderer.h"
#include "core/ecs/Entity.h"
#include "core/ecs/Components.h"
#include "core/ecs/EntityData.h"
#include "core/physics/Physics.h"
#include "core/rendering/Terrain.h"
#include "editor/rendering/Picking.h"
#include "core/input/Input.h"
#include "core/platform/KeyCodes.h"
#ifndef CLAYMORE_CORE
#include <imgui.h>
#endif
#include <limits>
#include <algorithm>
#include <cmath>

namespace {
    struct NavLinkPainterState {
        bool PaintModeEnabled = false;
        bool CursorValid = false;
        glm::vec3 CursorWorldPos{0.0f};
        glm::vec3 CursorWorldNormal{0.0f, 1.0f, 0.0f};
        bool HasStart = false;
        glm::vec3 StartWorld{0.0f};
        glm::vec3 StartNormal{0.0f, 1.0f, 0.0f};
        bool WasClickedLastFrame = false;
        float Radius = 0.5f;
        float Cost = 1.0f;
        bool Bidirectional = true;
        uint32_t Flags = 0;
    };

    NavLinkPainterState& GetState() {
        static NavLinkPainterState s;
        return s;
    }

    bool RaycastWorld(Scene& scene, const Ray& ray, glm::vec3& outPos, glm::vec3& outNormal)
    {
        // Priority 1: Physics raycast
        Physics::RaycastHit physicsHit;
        if (Physics::Raycast(ray.Origin, ray.Direction, 1000.0f, physicsHit)) {
            outPos = physicsHit.point;
            if (glm::dot(physicsHit.normal, physicsHit.normal) > 1e-6f) {
                outNormal = glm::normalize(physicsHit.normal);
            } else {
                outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            return true;
        }

        // Priority 2: Terrain raycast
        float closest = std::numeric_limits<float>::max();
        bool hit = false;
        for (const auto& entity : scene.GetEntities()) {
            EntityID id = entity.GetID();
            auto* data = scene.GetEntityData(id);
            if (!data || !data->Terrain) continue;
            glm::vec3 worldPos, worldNormal;
            if (Terrain::Raycast(data->Transform, *data->Terrain, ray.Origin, ray.Direction, &worldPos, &worldNormal, nullptr, nullptr)) {
                float dist = glm::length(worldPos - ray.Origin);
                if (dist < closest) {
                    closest = dist;
                    outPos = worldPos;
                    outNormal = (glm::dot(worldNormal, worldNormal) > 1e-6f) ? glm::normalize(worldNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                    hit = true;
                }
            }
        }
        if (hit) return true;

        // Priority 3: Mesh intersection (approx normal with up)
        float closestWorldDist = std::numeric_limits<float>::max();
        bool anyMeshHit = false;
        glm::vec3 meshHitPos;
        for (const auto& entity : scene.GetEntities()) {
            EntityID id = entity.GetID();
            auto* data = scene.GetEntityData(id);
            if (!data || !data->Visible || !data->Active) continue;
            if (!data->Mesh || !data->Mesh->mesh) continue;
            float hitT = 0.0f;
            if (Picking::RayIntersectsMesh(ray, *data->Mesh->mesh, data->Transform.WorldMatrix, hitT)) {
                if (hitT > 0.0f) {
                    glm::vec3 worldHit = ray.Origin + ray.Direction * hitT;
                    if (hitT < closestWorldDist) {
                        closestWorldDist = hitT;
                        meshHitPos = worldHit;
                        anyMeshHit = true;
                    }
                }
            }
        }
        if (anyMeshHit) {
            outPos = meshHitPos;
            outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
            return true;
        }

        return false;
    }

    void CreateNavLinkEntity(Scene& scene, EntityID parent, const glm::vec3& start, const glm::vec3& end, const NavLinkPainterState& state)
    {
        Entity linkEntity = scene.CreateEntity("NavLink");
        EntityID linkId = linkEntity.GetID();
        auto* data = scene.GetEntityData(linkId);
        if (!data) return;

        data->NavLink = std::make_unique<nav::NavLinkComponent>();
        auto& link = *data->NavLink;
        link.Enabled = true;
        link.Start = start;
        link.End = end;
        link.Radius = state.Radius;
        link.Cost = state.Cost;
        link.Flags = state.Flags;
        link.Bidirectional = state.Bidirectional;
        link.UseWorldSpace = true;

        data->Transform.Position = (start + end) * 0.5f;
        data->Transform.TransformDirty = true;

        if (parent != INVALID_ENTITY_ID && parent != 0) {
            scene.SetParent(linkId, parent, true);
        }
    }
}

void NavLinkPainter::Update(Scene& scene, EntityID selectedEntity, bool playMode, bool viewportHovered, bool /*allowSelectionChanges*/, Camera* viewportCamera)
{
    NavLinkPainterState& state = GetState();
    state.CursorValid = false;

    if (!state.PaintModeEnabled) {
#ifndef CLAYMORE_CORE
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
#endif
        return;
    }

    if (!viewportHovered) {
#ifndef CLAYMORE_CORE
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
#endif
        return;
    }

#ifndef CLAYMORE_CORE
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
#endif

    if (playMode || selectedEntity == 0 || selectedEntity == INVALID_ENTITY_ID) return;
    auto* navEntityData = scene.GetEntityData(selectedEntity);
    if (!navEntityData || !navEntityData->Navigation) return;

    Renderer& renderer = Renderer::Get();
    float mouseNX = 0.0f;
    float mouseNY = 0.0f;
    if (!renderer.GetUIMouseNormalized(mouseNX, mouseNY)) return;

    Camera* cam = viewportCamera ? viewportCamera : renderer.GetCamera();
    if (!cam) return;

    Ray ray = Picking::ScreenPointToRay(mouseNX, mouseNY, cam);
    glm::vec3 hitPos, hitNormal;
    if (!RaycastWorld(scene, ray, hitPos, hitNormal)) return;

    state.CursorValid = true;
    state.CursorWorldPos = hitPos;
    state.CursorWorldNormal = hitNormal;

    float scroll = Input::GetScrollDelta();
    if (std::abs(scroll) > 0.0f) {
        state.Radius = std::clamp(state.Radius + scroll * 0.25f, 0.1f, 25.0f);
    }

    const uint32_t cursorColor = state.HasStart ? 0xFF00AAFFu : 0xFFFFAA00u;
    renderer.DrawRing(hitPos, hitNormal, state.Radius, cursorColor);

    if (state.HasStart) {
        renderer.DrawRing(state.StartWorld, state.StartNormal, state.Radius, 0xFF00FF00u);
        renderer.DrawDebugLineColored(state.StartWorld, hitPos, 0xFF00FFFFu);
    }

    bool leftMouseDown = Input::IsMouseButtonPressed(MouseButton::Left);
    bool rightMouseDown = Input::IsMouseButtonPressed(MouseButton::Right);
    bool escapePressed = Input::IsKeyPressed(KeyCode::Escape);

    if (viewportHovered && leftMouseDown && !state.WasClickedLastFrame) {
        if (!state.HasStart) {
            state.HasStart = true;
            state.StartWorld = hitPos;
            state.StartNormal = hitNormal;
        } else {
            CreateNavLinkEntity(scene, selectedEntity, state.StartWorld, hitPos, state);
            state.HasStart = false;
        }
    }

    if ((rightMouseDown || escapePressed) && !state.WasClickedLastFrame) {
        if (escapePressed) {
            state.PaintModeEnabled = false;
            state.HasStart = false;
        } else if (state.HasStart) {
            state.HasStart = false;
        }
    }

    state.WasClickedLastFrame = leftMouseDown || rightMouseDown;
}

bool NavLinkPainter::IsPaintModeEnabled()
{
    return GetState().PaintModeEnabled;
}

void NavLinkPainter::SetPaintModeEnabled(bool enabled)
{
    NavLinkPainterState& state = GetState();
    state.PaintModeEnabled = enabled;
    if (!enabled) {
        state.HasStart = false;
    }
}

bool NavLinkPainter::TogglePaintMode()
{
    NavLinkPainterState& state = GetState();
    state.PaintModeEnabled = !state.PaintModeEnabled;
    if (!state.PaintModeEnabled) {
        state.HasStart = false;
    }
    return state.PaintModeEnabled;
}
