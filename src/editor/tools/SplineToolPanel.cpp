#include "editor/tools/SplineToolPanel.h"

#include "imgui.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/rendering/Renderer.h"
#include "core/input/Input.h"
#include "core/platform/KeyCodes.h"
#include "core/physics/Physics.h"
#include "core/rendering/Terrain.h"
#include "editor/rendering/Picking.h"
#include "editor/undo/EditorSceneUndoStack.h"

#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <limits>

SplineToolPanel::SplineToolPanel(Scene* scene, EntityID* selectedEntity)
    : m_SelectedEntity(selectedEntity)
{
    SetContext(scene);
}

void SplineToolPanel::Open()
{
    SyncSelectionTarget();
    m_Open = true;
}

void SplineToolPanel::Close()
{
    StopDrawMode();
}

void SplineToolPanel::StopDrawMode()
{
    if (m_UndoActionActive) {
        EditorSceneUndoStack::Get().EndScopedAction(m_Context);
        m_UndoActionActive = false;
    }
    m_DrawModeActive = false;
    m_DraggingPointIndex = -1;
    m_Open = false;
}

void SplineToolPanel::SyncSelectionTarget()
{
    if (m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID && *m_SelectedEntity != 0)
    {
        EntityData* data = m_Context->GetEntityData(*m_SelectedEntity);
        if (data && data->Spline)
        {
            m_TargetSplineEntity = *m_SelectedEntity;
        }
    }
}

bool SplineToolPanel::HasValidTarget() const
{
    if (!m_Context) return false;
    if (m_TargetSplineEntity == INVALID_ENTITY_ID || m_TargetSplineEntity == 0) return false;
    EntityData* data = m_Context->GetEntityData(m_TargetSplineEntity);
    return data && data->Spline;
}

EntityData* SplineToolPanel::GetTargetEntityData() const
{
    if (!m_Context) return nullptr;
    if (m_TargetSplineEntity == INVALID_ENTITY_ID || m_TargetSplineEntity == 0) return nullptr;
    return m_Context->GetEntityData(m_TargetSplineEntity);
}

bool SplineToolPanel::RaycastWorld(const Ray& ray, glm::vec3& outPos, glm::vec3& outNormal)
{
    if (!m_Context) return false;

    Physics::RaycastHit physicsHit;
    if (Physics::Raycast(ray.Origin, ray.Direction, 1000.0f, physicsHit))
    {
        outPos = physicsHit.point;
        outNormal = (glm::dot(physicsHit.normal, physicsHit.normal) > 1e-6f)
            ? glm::normalize(physicsHit.normal) : glm::vec3(0.0f, 1.0f, 0.0f);
        return true;
    }

    float closest = std::numeric_limits<float>::max();
    bool hit = false;
    for (const auto& entity : m_Context->GetEntities())
    {
        auto* data = m_Context->GetEntityData(entity.GetID());
        if (!data || !data->Terrain) continue;
        glm::vec3 worldPos, worldNormal;
        if (Terrain::Raycast(data->Transform, *data->Terrain, ray.Origin, ray.Direction,
                             &worldPos, &worldNormal, nullptr, nullptr))
        {
            float dist = glm::length(worldPos - ray.Origin);
            if (dist < closest)
            {
                closest = dist;
                outPos = worldPos;
                outNormal = (glm::dot(worldNormal, worldNormal) > 1e-6f)
                    ? glm::normalize(worldNormal) : glm::vec3(0.0f, 1.0f, 0.0f);
                hit = true;
            }
        }
    }
    if (hit) return true;

    float closestWorldDist = std::numeric_limits<float>::max();
    bool anyMeshHit = false;
    for (const auto& entity : m_Context->GetEntities())
    {
        auto* data = m_Context->GetEntityData(entity.GetID());
        if (!data || !data->Visible || !data->Active || !data->Mesh || !data->Mesh->mesh) continue;
        float hitT = 0.0f;
        if (Picking::RayIntersectsMesh(ray, *data->Mesh->mesh, data->Transform.WorldMatrix, hitT) && hitT > 0.0f)
        {
            glm::vec3 worldHit = ray.Origin + ray.Direction * hitT;
            if (hitT < closestWorldDist)
            {
                closestWorldDist = hitT;
                outPos = worldHit;
                outNormal = glm::vec3(0.0f, 1.0f, 0.0f);
                anyMeshHit = true;
            }
        }
    }
    return anyMeshHit;
}

float SplineToolPanel::PointToRayDistance(const glm::vec3& worldPoint, const Ray& ray) const
{
    glm::vec3 diff = worldPoint - ray.Origin;
    float t = glm::dot(diff, ray.Direction);
    if (t <= 0.0f) return glm::length(diff);
    glm::vec3 closest = ray.Origin + ray.Direction * t;
    return glm::length(worldPoint - closest);
}

int SplineToolPanel::GetHoveredPointIndex(const glm::vec3& worldCursorPos, float pickRadius) const
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline) return -1;

    const glm::mat4& worldMat = data->Transform.WorldMatrix;
    int closest = -1;
    float closestDist = pickRadius;

    for (size_t i = 0; i < data->Spline->ControlPoints.size(); ++i)
    {
        glm::vec3 worldPt = glm::vec3(worldMat * glm::vec4(data->Spline->ControlPoints[i].Position, 1.0f));
        float d = glm::length(worldPt - worldCursorPos);
        if (d < closestDist)
        {
            closestDist = d;
            closest = static_cast<int>(i);
        }
    }
    return closest;
}

void SplineToolPanel::AddControlPoint(const glm::vec3& worldPos, const glm::vec3& normal)
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline) return;

    glm::mat4 invWorld = glm::inverse(data->Transform.WorldMatrix);
    glm::vec3 localPos = glm::vec3(invWorld * glm::vec4(worldPos, 1.0f));

    SplinePathPoint pt;
    pt.Position = localPos;
    pt.Normal = glm::normalize(glm::vec3(invWorld * glm::vec4(normal, 0.0f)));
    if (glm::dot(pt.Normal, pt.Normal) < 1e-6f) pt.Normal = glm::vec3(0.0f, 1.0f, 0.0f);

    data->Spline->ControlPoints.push_back(pt);
    if (data->Instancer) data->Instancer->NeedsRegeneration = true;
    m_Context->MarkDirty();
}

void SplineToolPanel::RemovePoint(size_t index)
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline || index >= data->Spline->ControlPoints.size()) return;
    data->Spline->ControlPoints.erase(data->Spline->ControlPoints.begin() + static_cast<ptrdiff_t>(index));
    if (data->Instancer) data->Instancer->NeedsRegeneration = true;
    m_Context->MarkDirty();
}

void SplineToolPanel::MovePoint(size_t index, const glm::vec3& newWorldPos)
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline || index >= data->Spline->ControlPoints.size()) return;

    glm::mat4 invWorld = glm::inverse(data->Transform.WorldMatrix);
    data->Spline->ControlPoints[index].Position = glm::vec3(invWorld * glm::vec4(newWorldPos, 1.0f));
    if (data->Instancer) data->Instancer->NeedsRegeneration = true;
    m_Context->MarkDirty();
}

std::vector<glm::vec3> SplineToolPanel::GetSampledWorldPoints() const
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline) return {};

    auto sampled = cm::spline::SampleSpline(
        data->Spline->ControlPoints,
        data->Spline->SplineSubdivision,
        data->Spline->Closed);

    const glm::mat4& worldMat = data->Transform.WorldMatrix;
    std::vector<glm::vec3> world;
    world.reserve(sampled.size());
    for (const auto& p : sampled)
        world.push_back(glm::vec3(worldMat * glm::vec4(p, 1.0f)));
    return world;
}

void SplineToolPanel::Update(bool viewportHovered, bool playMode, Camera* viewportCamera)
{
    EditorSceneUndoStack& undoStack = EditorSceneUndoStack::Get();
    auto endUndoAction = [&]() {
        if (m_UndoActionActive) {
            undoStack.EndScopedAction(m_Context);
            m_UndoActionActive = false;
        }
    };
    auto finishDragIfReleased = [&]() {
        if (m_DraggingPointIndex >= 0 && !Input::IsMouseButtonPressed(MouseButton::Left)) {
            endUndoAction();
            m_DraggingPointIndex = -1;
        }
    };

    if (!m_Open || !m_DrawModeActive || playMode) {
        endUndoAction();
        return;
    }
    if (!HasValidTarget()) {
        endUndoAction();
        return;
    }

    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline) {
        endUndoAction();
        return;
    }

    Renderer& renderer = Renderer::Get();
    float mouseNX = 0.0f, mouseNY = 0.0f;
    if (!renderer.GetUIMouseNormalized(mouseNX, mouseNY)) {
        finishDragIfReleased();
        return;
    }

    Camera* cam = viewportCamera ? viewportCamera : renderer.GetCamera();
    if (!cam) {
        finishDragIfReleased();
        return;
    }

    Ray ray = Picking::ScreenPointToRay(mouseNX, mouseNY, cam);
    glm::vec3 hitPos, hitNormal;
    if (!RaycastWorld(ray, hitPos, hitNormal)) {
        finishDragIfReleased();
        return;
    }

    if (m_DraggingPointIndex >= 0)
    {
        if (Input::IsMouseButtonPressed(MouseButton::Left))
        {
            if (!m_UndoActionActive) {
                undoStack.BeginScopedAction(m_Context, "Edit Spline");
                m_UndoActionActive = undoStack.IsBoundTo(m_Context);
            }
            MovePoint(static_cast<size_t>(m_DraggingPointIndex), hitPos);
        }
        else
        {
            endUndoAction();
            m_DraggingPointIndex = -1;
        }
    }
    else
    {
        int hovered = GetHoveredPointIndex(hitPos, m_PointPickRadius);
        bool leftPressed = Input::WasMouseButtonPressedThisFrame(MouseButton::Left);
        bool rightPressed = Input::WasMouseButtonPressedThisFrame(MouseButton::Right);
        bool escapePressed = Input::IsKeyPressed(KeyCode::Escape);

        if (viewportHovered && leftPressed)
        {
            if (hovered >= 0)
            {
                m_DraggingPointIndex = hovered;
            }
            else
            {
                undoStack.BeginScopedAction(m_Context, "Edit Spline");
                AddControlPoint(hitPos, hitNormal);
                undoStack.EndScopedAction(m_Context);
            }
        }

        if (rightPressed || escapePressed)
        {
            if (escapePressed)
            {
                m_DrawModeActive = false;
            }
            else if (hovered >= 0)
            {
                undoStack.BeginScopedAction(m_Context, "Edit Spline");
                RemovePoint(static_cast<size_t>(hovered));
                undoStack.EndScopedAction(m_Context);
            }
            else if (!data->Spline->ControlPoints.empty())
            {
                undoStack.BeginScopedAction(m_Context, "Edit Spline");
                RemovePoint(data->Spline->ControlPoints.size() - 1);
                undoStack.EndScopedAction(m_Context);
            }
        }
    }

    uint32_t cursorColor = 0xFFFFAA00u;
    renderer.DrawFilledCircle(hitPos, hitNormal, 0.2f, cursorColor);

#ifndef CLAYMORE_CORE
    if (viewportHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_None);
#endif
}

void SplineToolPanel::DrawSplineVisualization()
{
    if (!m_Open || !HasValidTarget()) return;

    EntityData* data = GetTargetEntityData();
    if (!data || !data->Spline || data->Spline->ControlPoints.empty()) return;

    Renderer& renderer = Renderer::Get();
    const glm::mat4& worldMat = data->Transform.WorldMatrix;

    float mouseNX = 0.0f, mouseNY = 0.0f;
    glm::vec3 cursorWorld(0.0f);
    if (renderer.GetUIMouseNormalized(mouseNX, mouseNY))
    {
        Camera* cam = renderer.GetCamera();
        if (cam)
        {
            Ray ray = Picking::ScreenPointToRay(mouseNX, mouseNY, cam);
            if (RaycastWorld(ray, cursorWorld, glm::vec3(0,1,0))) { /* have cursor */ }
        }
    }

    int hovered = GetHoveredPointIndex(cursorWorld, m_PointPickRadius);
    bool dragging = m_DraggingPointIndex >= 0;

    for (size_t i = 0; i < data->Spline->ControlPoints.size(); ++i)
    {
        glm::vec3 worldPt = glm::vec3(worldMat * glm::vec4(data->Spline->ControlPoints[i].Position, 1.0f));
        glm::vec3 normal = glm::normalize(glm::vec3(worldMat * glm::vec4(data->Spline->ControlPoints[i].Normal, 0.0f)));
        if (glm::dot(normal, normal) < 1e-6f) normal = glm::vec3(0.0f, 1.0f, 0.0f);

        uint32_t color;
        if (static_cast<int>(i) == m_DraggingPointIndex)
            color = 0xFF00FF00u;
        else if (static_cast<int>(i) == hovered)
            color = 0xFFFFFF00u;
        else if (i == 0)
            color = 0xFF00FF00u;
        else if (i == data->Spline->ControlPoints.size() - 1)
            color = 0xFF0000FFu;
        else
            color = 0xFF00FFFFu;

        float radius = (static_cast<int>(i) == hovered || static_cast<int>(i) == m_DraggingPointIndex) ? 0.25f : 0.18f;
        renderer.DrawFilledCircle(worldPt, normal, radius, color);
    }

    if (data->Spline->ControlPoints.size() >= 2)
    {
        if (data->Spline->ControlPoints.size() >= 4)
        {
            std::vector<glm::vec3> sampled = GetSampledWorldPoints();
            for (size_t i = 0; i + 1 < sampled.size(); ++i)
                renderer.DrawDebugLineColored(sampled[i], sampled[i + 1], 0xFFFFFF00u);
        }
        else
        {
            for (size_t i = 0; i + 1 < data->Spline->ControlPoints.size(); ++i)
            {
                glm::vec3 a = glm::vec3(worldMat * glm::vec4(data->Spline->ControlPoints[i].Position, 1.0f));
                glm::vec3 b = glm::vec3(worldMat * glm::vec4(data->Spline->ControlPoints[i + 1].Position, 1.0f));
                renderer.DrawDebugLineColored(a, b, 0xFFFFFF00u);
            }
        }
    }
}

void SplineToolPanel::OnImGuiRender()
{
    // Spline tool config is in the Inspector panel; no standalone window
}
