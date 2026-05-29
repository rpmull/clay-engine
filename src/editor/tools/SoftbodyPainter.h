#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"

#include <glm/glm.hpp>
#include <vector>

class Camera;
struct EntityData;

class SoftbodyPainter : public EditorPanel
{
public:
   enum class PaintMode {
      Weight = 0,
      Anchor = 1,
      Select = 2
   };

   SoftbodyPainter(Scene* scene, EntityID* selectedEntity);

   void Update(bool viewportHovered, bool playMode, Camera* viewportCamera);
   void DrawVisualization(Camera* viewportCamera);

   void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }
   void SyncSelectionTarget();

   void StartPaintMode();
   void StopPaintMode();
   bool IsPaintModeActive() const { return m_PaintModeActive; }
   bool HasValidTarget() const;
   bool IsTarget(EntityID entity) const { return m_TargetEntity == entity; }

   PaintMode GetPaintMode() const { return m_Mode; }
   void SetPaintMode(PaintMode mode) { m_Mode = mode; }

   float GetBrushRadius() const { return m_BrushRadius; }
   void SetBrushRadius(float radius) { m_BrushRadius = radius; }

   float GetPaintWeight() const { return m_PaintWeight; }
   void SetPaintWeight(float weight) { m_PaintWeight = weight; }

   bool GetShowAllVertices() const { return m_ShowAllVertices; }
   void SetShowAllVertices(bool show) { m_ShowAllVertices = show; }

   int GetHoveredVertex() const { return m_HoveredVertex; }
   size_t GetSelectedCount() const { return m_SelectedVertices.size(); }

   void ResetWeights();
   void ClearAnchors();
   void SetSelectedAnchored(bool anchored);
   void SetSelectedWeight(float weight);

private:
   EntityData* GetTargetEntityData() const;
   bool RaycastTargetMesh(const struct Ray& ray, glm::vec3& outHitPos) const;
   void BuildWorldVertexPositions(const EntityData& data, std::vector<glm::vec3>& outPositions) const;
   int FindNearestVertex(const std::vector<glm::vec3>& worldPositions, const glm::vec3& worldPoint, float radius) const;
   bool ApplyBrush(const std::vector<glm::vec3>& worldPositions, const glm::vec3& worldPoint, bool erase);

private:
   EntityID* m_SelectedEntity = nullptr;
   EntityID m_TargetEntity = INVALID_ENTITY_ID;
   bool m_PaintModeActive = false;
   PaintMode m_Mode = PaintMode::Weight;
   float m_BrushRadius = 0.2f;
   float m_PaintWeight = 1.0f;
   bool m_ShowAllVertices = true;
   int m_HoveredVertex = -1;
   std::vector<uint32_t> m_SelectedVertices;
   bool m_LastHitValid = false;
   glm::vec3 m_LastHitWorldPos{ 0.0f };
   glm::vec3 m_LastDrawNormal{ 0.0f, 1.0f, 0.0f };
};
