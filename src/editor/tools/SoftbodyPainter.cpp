#include "editor/tools/SoftbodyPainter.h"

#include "core/ecs/Components.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Scene.h"
#include "core/ecs/SoftbodySystem.h"
#include "core/input/Input.h"
#include "core/platform/KeyCodes.h"
#include "core/rendering/Camera.h"
#include "core/rendering/Mesh.h"
#include "core/rendering/Renderer.h"
#include "editor/rendering/Picking.h"

#ifndef CLAYMORE_CORE
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr float kVertexRadiusMin = 0.0125f;
constexpr float kVertexRadiusMax = 0.08f;
constexpr float kBrushStrengthEpsilon = 1.0e-4f;

uint32_t ToABGR(const glm::vec4& color)
{
   const uint8_t r = static_cast<uint8_t>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
   const uint8_t g = static_cast<uint8_t>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
   const uint8_t b = static_cast<uint8_t>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
   const uint8_t a = static_cast<uint8_t>(glm::clamp(color.a, 0.0f, 1.0f) * 255.0f);
   return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
}

glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
{
   const float lenSq = glm::dot(value, value);
   if (lenSq <= 1.0e-8f) {
      return fallback;
   }
   return value / std::sqrt(lenSq);
}

bool HasModifier(KeyCode a, KeyCode b)
{
   return Input::IsKeyPressed(a) || Input::IsKeyPressed(b);
}

bool ContainsVertex(const std::vector<uint32_t>& vertices, uint32_t vertex)
{
   return std::find(vertices.begin(), vertices.end(), vertex) != vertices.end();
}

void RemoveVertex(std::vector<uint32_t>& vertices, uint32_t vertex)
{
   auto it = std::find(vertices.begin(), vertices.end(), vertex);
   if (it != vertices.end()) {
      vertices.erase(it);
   }
}

float ComputeMarkerRadius(const SoftbodyPainter& painter,
                          const std::vector<glm::vec3>& worldPositions)
{
   if (worldPositions.empty()) {
      return kVertexRadiusMin;
   }

   glm::vec3 minBounds(std::numeric_limits<float>::max());
   glm::vec3 maxBounds(std::numeric_limits<float>::lowest());
   for (const glm::vec3& pos : worldPositions) {
      minBounds = glm::min(minBounds, pos);
      maxBounds = glm::max(maxBounds, pos);
   }

   const float diag = glm::length(maxBounds - minBounds);
   const float brushDriven = painter.GetBrushRadius() * 0.08f;
   const float diagDriven = diag * 0.0025f;
   return glm::clamp(std::max(brushDriven, diagDriven), kVertexRadiusMin, kVertexRadiusMax);
}

glm::vec4 WeightColor(float weight)
{
   const glm::vec3 locked(0.13f, 0.35f, 0.92f);
   const glm::vec3 free(0.14f, 0.92f, 0.34f);
   return glm::vec4(glm::mix(locked, free, glm::clamp(weight, 0.0f, 1.0f)), 0.95f);
}

} // namespace

SoftbodyPainter::SoftbodyPainter(Scene* scene, EntityID* selectedEntity)
   : m_SelectedEntity(selectedEntity)
{
   SetContext(scene);
}

void SoftbodyPainter::SyncSelectionTarget()
{
   EntityID newTarget = INVALID_ENTITY_ID;
   if (m_Context && m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID && *m_SelectedEntity != 0) {
      EntityData* data = m_Context->GetEntityData(*m_SelectedEntity);
      if (data && data->Softbody) {
         newTarget = *m_SelectedEntity;
      }
   }

   if (newTarget != m_TargetEntity) {
      m_TargetEntity = newTarget;
      m_SelectedVertices.clear();
      m_HoveredVertex = -1;
      m_LastHitValid = false;
   }

   if (m_TargetEntity == INVALID_ENTITY_ID && m_PaintModeActive) {
      StopPaintMode();
   }
}

void SoftbodyPainter::StartPaintMode()
{
   SyncSelectionTarget();
   if (!HasValidTarget()) {
      return;
   }

   if (EntityData* data = GetTargetEntityData()) {
      SoftbodySystem::EnsureAuthoringData(*data);
   }

   m_PaintModeActive = true;
   m_LastHitValid = false;
   m_HoveredVertex = -1;
}

void SoftbodyPainter::StopPaintMode()
{
   m_PaintModeActive = false;
   m_HoveredVertex = -1;
   m_LastHitValid = false;
}

bool SoftbodyPainter::HasValidTarget() const
{
   if (!m_Context) {
      return false;
   }
   if (m_TargetEntity == INVALID_ENTITY_ID || m_TargetEntity == 0) {
      return false;
   }

   EntityData* data = m_Context->GetEntityData(m_TargetEntity);
   return data && data->Softbody && data->Mesh && data->Mesh->mesh;
}

EntityData* SoftbodyPainter::GetTargetEntityData() const
{
   if (!m_Context || m_TargetEntity == INVALID_ENTITY_ID || m_TargetEntity == 0) {
      return nullptr;
   }
   return m_Context->GetEntityData(m_TargetEntity);
}

bool SoftbodyPainter::RaycastTargetMesh(const Ray& ray, glm::vec3& outHitPos) const
{
   EntityData* data = GetTargetEntityData();
   if (!data || !data->Mesh || !data->Mesh->mesh) {
      return false;
   }

   const Mesh& mesh = *data->Mesh->mesh;
   if (mesh.Vertices.empty() || mesh.Indices.size() < 3) {
      return false;
   }

   const glm::mat4 invWorld = glm::inverse(data->Transform.WorldMatrix);
   const glm::vec3 localOrigin = glm::vec3(invWorld * glm::vec4(ray.Origin, 1.0f));
   const glm::vec3 localDir = SafeNormalize(
      glm::vec3(invWorld * glm::vec4(ray.Direction, 0.0f)),
      glm::vec3(0.0f, 0.0f, -1.0f));

   float closestT = std::numeric_limits<float>::max();
   glm::vec3 bestNormal(0.0f, 1.0f, 0.0f);
   bool hit = false;

   for (size_t i = 0; i + 2 < mesh.Indices.size(); i += 3) {
      const uint32_t i0 = mesh.Indices[i + 0];
      const uint32_t i1 = mesh.Indices[i + 1];
      const uint32_t i2 = mesh.Indices[i + 2];
      if (i0 >= mesh.Vertices.size() || i1 >= mesh.Vertices.size() || i2 >= mesh.Vertices.size()) {
         continue;
      }

      float localT = 0.0f;
      if (!Picking::RayIntersectsTriangle(localOrigin, localDir, mesh.Vertices[i0], mesh.Vertices[i1], mesh.Vertices[i2], localT)) {
         continue;
      }
      if (localT <= 0.0f || localT >= closestT) {
         continue;
      }

      const glm::vec3 faceNormal = glm::cross(
         mesh.Vertices[i1] - mesh.Vertices[i0],
         mesh.Vertices[i2] - mesh.Vertices[i0]);
      closestT = localT;
      bestNormal = faceNormal;
      hit = true;
   }

   if (!hit) {
      return false;
   }

   const glm::vec3 localHit = localOrigin + localDir * closestT;
   outHitPos = glm::vec3(data->Transform.WorldMatrix * glm::vec4(localHit, 1.0f));

   const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(data->Transform.WorldMatrix)));
   const glm::vec3 worldNormal = SafeNormalize(
      normalMatrix * bestNormal,
      glm::vec3(0.0f, 1.0f, 0.0f));

   const_cast<SoftbodyPainter*>(this)->m_LastDrawNormal = worldNormal;
   return true;
}

void SoftbodyPainter::BuildWorldVertexPositions(const EntityData& data, std::vector<glm::vec3>& outPositions) const
{
   outPositions.clear();
   if (!data.Mesh || !data.Mesh->mesh) {
      return;
   }

   const Mesh& mesh = *data.Mesh->mesh;
   outPositions.reserve(mesh.Vertices.size());
   for (const glm::vec3& vertex : mesh.Vertices) {
      outPositions.push_back(glm::vec3(data.Transform.WorldMatrix * glm::vec4(vertex, 1.0f)));
   }
}

int SoftbodyPainter::FindNearestVertex(const std::vector<glm::vec3>& worldPositions,
                                       const glm::vec3& worldPoint,
                                       float radius) const
{
   const float radiusSq = radius * radius;
   int closestIndex = -1;
   float closestDistSq = radiusSq;

   for (size_t i = 0; i < worldPositions.size(); ++i) {
      const glm::vec3 delta = worldPositions[i] - worldPoint;
      const float distSq = glm::dot(delta, delta);
      if (distSq <= closestDistSq) {
         closestDistSq = distSq;
         closestIndex = static_cast<int>(i);
      }
   }

   return closestIndex;
}

bool SoftbodyPainter::ApplyBrush(const std::vector<glm::vec3>& worldPositions,
                                 const glm::vec3& worldPoint,
                                 bool erase)
{
   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return false;
   }

   SoftbodyComponent& softbody = *data->Softbody;
   SoftbodySystem::EnsureAuthoringData(*data);

   const float radius = std::max(m_BrushRadius, 0.001f);
   const float radiusSq = radius * radius;
   bool changed = false;

   if (m_Mode == PaintMode::Weight) {
      const float targetWeight = erase ? 1.0f : glm::clamp(m_PaintWeight, 0.0f, 1.0f);
      for (size_t i = 0; i < worldPositions.size(); ++i) {
         const glm::vec3 delta = worldPositions[i] - worldPoint;
         const float distSq = glm::dot(delta, delta);
         if (distSq > radiusSq) {
            continue;
         }

         const float dist = std::sqrt(std::max(distSq, 0.0f));
         const float falloff = 1.0f - glm::clamp(dist / radius, 0.0f, 1.0f);
         const float blend = falloff * falloff;
         const float current = softbody.VertexWeights[i];
         const float updated = glm::mix(current, targetWeight, blend);
         if (std::abs(updated - current) > kBrushStrengthEpsilon) {
            softbody.VertexWeights[i] = glm::clamp(updated, 0.0f, 1.0f);
            changed = true;
         }
      }
   }
   else if (m_Mode == PaintMode::Anchor) {
      const uint8_t targetValue = erase ? 0 : 1;
      for (size_t i = 0; i < worldPositions.size(); ++i) {
         const glm::vec3 delta = worldPositions[i] - worldPoint;
         const float distSq = glm::dot(delta, delta);
         if (distSq > radiusSq) {
            continue;
         }
         if (softbody.AnchorVertices[i] != targetValue) {
            softbody.AnchorVertices[i] = targetValue;
            changed = true;
         }
      }
   }

   if (changed && m_Context) {
      m_Context->MarkDirty();
   }
   return changed;
}

void SoftbodyPainter::Update(bool viewportHovered, bool playMode, Camera* viewportCamera)
{
   SyncSelectionTarget();

   if (!m_PaintModeActive) {
#ifndef CLAYMORE_CORE
      ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
#endif
      return;
   }

   if (playMode || !HasValidTarget()) {
      StopPaintMode();
      return;
   }

   Renderer& renderer = Renderer::Get();
   if (renderer.WasUIInputConsumedThisFrame()) {
      return;
   }

   float mouseNX = 0.0f;
   float mouseNY = 0.0f;
   if (!renderer.GetUIMouseNormalized(mouseNX, mouseNY)) {
      return;
   }

   Camera* camera = viewportCamera ? viewportCamera : renderer.GetCamera();
   if (!camera) {
      return;
   }

   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return;
   }

   std::string supportReason;
   if (!SoftbodySystem::SupportsMesh(*data, &supportReason)) {
      StopPaintMode();
      return;
   }

   std::vector<glm::vec3> worldPositions;
   BuildWorldVertexPositions(*data, worldPositions);

   const Ray ray = Picking::ScreenPointToRay(mouseNX, mouseNY, camera);
   glm::vec3 hitWorld(0.0f);
   m_LastHitValid = RaycastTargetMesh(ray, hitWorld);
   m_HoveredVertex = -1;

   if (m_LastHitValid) {
      m_LastHitWorldPos = hitWorld;
      m_HoveredVertex = FindNearestVertex(worldPositions, hitWorld, m_BrushRadius);
   }

#ifndef CLAYMORE_CORE
   if (viewportHovered) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_None);
   }
   else {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
   }
#endif

   if (!viewportHovered) {
      return;
   }

   if (Input::WasKeyPressedThisFrame(KeyCode::Escape)) {
      StopPaintMode();
      return;
   }

   if (!m_LastHitValid) {
      if (m_Mode == PaintMode::Select && Input::WasMouseButtonPressedThisFrame(MouseButton::Left) &&
          !HasModifier(KeyCode::LeftShift, KeyCode::RightShift) &&
          !HasModifier(KeyCode::LeftControl, KeyCode::RightControl)) {
         m_SelectedVertices.clear();
      }
      return;
   }

   const bool leftClicked = Input::WasMouseButtonPressedThisFrame(MouseButton::Left);
   const bool rightClicked = Input::WasMouseButtonPressedThisFrame(MouseButton::Right);
   const bool leftHeld = Input::IsMouseButtonPressed(MouseButton::Left);
   const bool rightHeld = Input::IsMouseButtonPressed(MouseButton::Right);

   if (m_Mode == PaintMode::Select) {
      const bool addToSelection = HasModifier(KeyCode::LeftShift, KeyCode::RightShift);
      const bool subtractFromSelection = HasModifier(KeyCode::LeftControl, KeyCode::RightControl);

      if (leftClicked) {
         if (m_HoveredVertex >= 0) {
            const uint32_t hovered = static_cast<uint32_t>(m_HoveredVertex);
            if (!addToSelection && !subtractFromSelection) {
               m_SelectedVertices.clear();
            }

            if (subtractFromSelection) {
               RemoveVertex(m_SelectedVertices, hovered);
            }
            else if (!ContainsVertex(m_SelectedVertices, hovered)) {
               m_SelectedVertices.push_back(hovered);
            }
         }
         else if (!addToSelection && !subtractFromSelection) {
            m_SelectedVertices.clear();
         }
      }

      if (rightClicked) {
         if (m_HoveredVertex >= 0) {
            RemoveVertex(m_SelectedVertices, static_cast<uint32_t>(m_HoveredVertex));
         }
         else {
            m_SelectedVertices.clear();
         }
      }
      return;
   }

   if (leftHeld) {
      ApplyBrush(worldPositions, m_LastHitWorldPos, false);
   }
   if (rightHeld) {
      ApplyBrush(worldPositions, m_LastHitWorldPos, true);
   }
}

void SoftbodyPainter::DrawVisualization(Camera* viewportCamera)
{
   if (!m_PaintModeActive || !HasValidTarget()) {
      return;
   }

   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return;
   }

   Renderer& renderer = Renderer::Get();
   Camera* camera = viewportCamera ? viewportCamera : renderer.GetCamera();

   std::vector<glm::vec3> worldPositions;
   BuildWorldVertexPositions(*data, worldPositions);
   if (worldPositions.empty()) {
      return;
   }

   const SoftbodyComponent& softbody = *data->Softbody;
   const float markerRadius = ComputeMarkerRadius(*this, worldPositions);

   if (m_LastHitValid) {
      const uint32_t brushColor = (m_Mode == PaintMode::Anchor)
         ? ToABGR(glm::vec4(1.0f, 0.48f, 0.15f, 0.95f))
         : ToABGR(glm::vec4(0.18f, 0.82f, 1.0f, 0.95f));
      renderer.DrawRing(m_LastHitWorldPos, m_LastDrawNormal, m_BrushRadius, brushColor);
   }

   for (size_t i = 0; i < worldPositions.size(); ++i) {
      const bool anchored = i < softbody.AnchorVertices.size() && softbody.AnchorVertices[i] != 0;
      const bool selected = ContainsVertex(m_SelectedVertices, static_cast<uint32_t>(i));
      const bool hovered = static_cast<int>(i) == m_HoveredVertex;

      if (!m_ShowAllVertices && !selected && !hovered && !anchored) {
         continue;
      }

      glm::vec4 color = anchored
         ? glm::vec4(1.0f, 0.35f, 0.1f, 0.95f)
         : WeightColor(i < softbody.VertexWeights.size() ? softbody.VertexWeights[i] : 1.0f);

      float radius = markerRadius;
      if (selected) {
         color = glm::vec4(0.08f, 0.95f, 1.0f, 0.98f);
         radius *= 1.5f;
      }
      if (hovered) {
         color = glm::vec4(1.0f, 0.95f, 0.2f, 1.0f);
         radius *= 1.25f;
      }

      glm::vec3 normal = glm::vec3(0.0f, 1.0f, 0.0f);
      if (camera) {
         normal = SafeNormalize(camera->GetPosition() - worldPositions[i], glm::vec3(0.0f, 1.0f, 0.0f));
      }
      renderer.DrawFilledCircle(worldPositions[i], normal, radius, ToABGR(color));
   }
}

void SoftbodyPainter::ResetWeights()
{
   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return;
   }

   SoftbodySystem::EnsureAuthoringData(*data);
   std::fill(data->Softbody->VertexWeights.begin(), data->Softbody->VertexWeights.end(), 1.0f);
   if (m_Context) {
      m_Context->MarkDirty();
   }
}

void SoftbodyPainter::ClearAnchors()
{
   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return;
   }

   SoftbodySystem::EnsureAuthoringData(*data);
   std::fill(data->Softbody->AnchorVertices.begin(), data->Softbody->AnchorVertices.end(), uint8_t(0));
   if (m_Context) {
      m_Context->MarkDirty();
   }
}

void SoftbodyPainter::SetSelectedAnchored(bool anchored)
{
   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return;
   }

   SoftbodySystem::EnsureAuthoringData(*data);
   for (uint32_t index : m_SelectedVertices) {
      if (index < data->Softbody->AnchorVertices.size()) {
         data->Softbody->AnchorVertices[index] = anchored ? 1 : 0;
      }
   }
   if (m_Context && !m_SelectedVertices.empty()) {
      m_Context->MarkDirty();
   }
}

void SoftbodyPainter::SetSelectedWeight(float weight)
{
   EntityData* data = GetTargetEntityData();
   if (!data || !data->Softbody) {
      return;
   }

   SoftbodySystem::EnsureAuthoringData(*data);
   const float clamped = glm::clamp(weight, 0.0f, 1.0f);
   for (uint32_t index : m_SelectedVertices) {
      if (index < data->Softbody->VertexWeights.size()) {
         data->Softbody->VertexWeights[index] = clamped;
      }
   }
   if (m_Context && !m_SelectedVertices.empty()) {
      m_Context->MarkDirty();
   }
}
