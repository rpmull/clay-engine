#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"
#include "core/spline/SplineUtils.h"
#include "editor/rendering/Picking.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

class Scene;
class Camera;
struct EntityData;

class SplineToolPanel : public EditorPanel
{
public:
    SplineToolPanel(Scene* scene, EntityID* selectedEntity);

    void Update(bool viewportHovered, bool playMode, Camera* viewportCamera);
    void OnImGuiRender();
    void Open();
    void Close();
    void StopDrawMode();
    void StartDrawMode() { m_DrawModeActive = true; m_Open = true; }
    bool IsOpen() const { return m_Open; }
    bool IsDrawModeActive() const { return m_DrawModeActive; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }
    void DrawSplineVisualization();
    float GetPointPickRadius() const { return m_PointPickRadius; }
    void SetPointPickRadius(float r) { m_PointPickRadius = r; }
    void SyncSelectionTarget();

private:
    bool HasValidTarget() const;
    EntityData* GetTargetEntityData() const;

    bool RaycastWorld(const Ray& ray, glm::vec3& outPos, glm::vec3& outNormal);
    int GetHoveredPointIndex(const glm::vec3& worldCursorPos, float pickRadius) const;
    float PointToRayDistance(const glm::vec3& worldPoint, const Ray& ray) const;

    void AddControlPoint(const glm::vec3& worldPos, const glm::vec3& normal);
    void RemovePoint(size_t index);
    void MovePoint(size_t index, const glm::vec3& newWorldPos);

    std::vector<glm::vec3> GetSampledWorldPoints() const;

private:
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_TargetSplineEntity = INVALID_ENTITY_ID;
    bool m_Open = false;
    bool m_DrawModeActive = false;

    int m_DraggingPointIndex = -1;
    float m_PointPickRadius = 1.0f;
    bool m_UndoActionActive = false;
};
