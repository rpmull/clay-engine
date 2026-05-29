#include "editor/tools/RiverCutterPanel.h"

#include "imgui.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/rendering/Terrain.h"
#include "core/rendering/Renderer.h"
#include "core/input/Input.h"
#include "core/platform/KeyCodes.h"
#include "core/utils/TerrainPainter.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/River.h"
#include "editor/rendering/Picking.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace
{
    constexpr float kFloatEpsilon = 1e-6f;
    
    // Catmull-Rom spline interpolation
    glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t)
    {
        float t2 = t * t;
        float t3 = t2 * t;
        
        return 0.5f * ((2.0f * p1) +
                       (-p0 + p2) * t +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }
    
    // Height field helper
    struct TerrainHeightField
    {
        std::vector<uint16_t>& HeightMap;
        int Resolution;
        
        TerrainHeightField(std::vector<uint16_t>& heightMap, int res)
            : HeightMap(heightMap), Resolution(res) {}
        
        size_t Index(int x, int y) const
        {
            x = std::clamp(x, 0, Resolution - 1);
            y = std::clamp(y, 0, Resolution - 1);
            return static_cast<size_t>(y) * Resolution + static_cast<size_t>(x);
        }
        
        float Get(int x, int y) const
        {
            return HeightMap[Index(x, y)] / 65535.0f;
        }
        
        void Set(int x, int y, float value)
        {
            HeightMap[Index(x, y)] = static_cast<uint16_t>(glm::clamp(value, 0.0f, 1.0f) * 65535.0f);
        }
        
        float Sample(float fx, float fy) const
        {
            fx = glm::clamp(fx, 0.0f, static_cast<float>(Resolution - 1));
            fy = glm::clamp(fy, 0.0f, static_cast<float>(Resolution - 1));
            int x0 = static_cast<int>(std::floor(fx));
            int y0 = static_cast<int>(std::floor(fy));
            int x1 = std::min(x0 + 1, Resolution - 1);
            int y1 = std::min(y0 + 1, Resolution - 1);
            float tx = fx - static_cast<float>(x0);
            float ty = fy - static_cast<float>(y0);
            float v00 = Get(x0, y0);
            float v10 = Get(x1, y0);
            float v01 = Get(x0, y1);
            float v11 = Get(x1, y1);
            float a = glm::mix(v00, v10, tx);
            float b = glm::mix(v01, v11, tx);
            return glm::mix(a, b, ty);
        }
    };
} // namespace

RiverCutterPanel::RiverCutterPanel(Scene* scene, EntityID* selectedEntity)
    : m_SelectedEntity(selectedEntity)
{
    SetContext(scene);
}

void RiverCutterPanel::Open()
{
    SyncSelectionTarget();
    m_Open = true;
}

void RiverCutterPanel::Close()
{
    m_DrawModeActive = false;
    m_Open = false;
}

void RiverCutterPanel::SyncSelectionTarget()
{
    if (m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID && *m_SelectedEntity != 0)
    {
        m_TargetTerrain = *m_SelectedEntity;
    }
}

bool RiverCutterPanel::HasValidTarget() const
{
    if (!m_Context)
        return false;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return false;
    EntityData* data = m_Context->GetEntityData(m_TargetTerrain);
    return data && data->Terrain;
}

EntityData* RiverCutterPanel::GetTargetEntityData() const
{
    if (!m_Context)
        return nullptr;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return nullptr;
    return m_Context->GetEntityData(m_TargetTerrain);
}

void RiverCutterPanel::Update(bool viewportHovered, bool playMode, Camera* viewportCamera)
{
    if (!m_Open || !m_DrawModeActive || playMode)
        return;
    
    if (!HasValidTarget())
        return;
    
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain)
        return;
    
    TerrainComponent& terrain = *data->Terrain;
    Terrain::EnsureChunkLayout(terrain);
    
    Renderer& renderer = Renderer::Get();
    
    // Get mouse position
    float mouseNX = 0.0f;
    float mouseNY = 0.0f;
    if (!renderer.GetUIMouseNormalized(mouseNX, mouseNY))
        return;
    
    Camera* cam = viewportCamera ? viewportCamera : renderer.GetCamera();
    if (!cam)
        return;
    
    Ray ray = Picking::ScreenPointToRay(mouseNX, mouseNY, cam);
    
    glm::vec3 worldPos, worldNormal, localPos, localNormal;
    if (!Terrain::Raycast(data->Transform, terrain, ray.Origin, ray.Direction, &worldPos, &worldNormal, &localPos, &localNormal))
        return;
    
    // Draw cursor ring at current mouse position using terrain brush radius
    uint32_t cursorColor = 0xFFFFAA00u;  // Cyan
    renderer.DrawRing(worldPos, worldNormal, terrain.Brush.Radius, cursorColor);
    
    // Handle click to add point
    bool leftMouseDown = Input::IsMouseButtonPressed(MouseButton::Left);
    
    if (viewportHovered && leftMouseDown && !m_WasClickedLastFrame)
    {
        AddPathPoint(worldPos, localPos, worldNormal);
    }
    
    // Handle right click or Escape to remove last point
    bool rightMouseDown = Input::IsMouseButtonPressed(MouseButton::Right);
    bool escapePressed = Input::IsKeyPressed(KeyCode::Escape);
    
    if ((rightMouseDown || escapePressed) && !m_PathPoints.empty())
    {
        if (escapePressed)
        {
            // Exit draw mode on Escape
            m_DrawModeActive = false;
        }
        else if (!m_WasClickedLastFrame)
        {
            RemoveLastPoint();
        }
    }
    
    m_WasClickedLastFrame = leftMouseDown || rightMouseDown;
    
#ifndef CLAYMORE_CORE
    if (viewportHovered)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
#endif
}

void RiverCutterPanel::DrawPathVisualization()
{
    if (!m_Open || m_PathPoints.empty())
        return;
    
    Renderer& renderer = Renderer::Get();
    
    // Draw control points as rings
    for (size_t i = 0; i < m_PathPoints.size(); ++i)
    {
        const RiverEditorPathPoint& pt = m_PathPoints[i];
        
        // First point is green, last is red, others are yellow
        uint32_t color;
        if (i == 0)
            color = 0xFF00FF00u;  // Green (start)
        else if (i == m_PathPoints.size() - 1)
            color = 0xFF0000FFu;  // Red (end)
        else
            color = 0xFF00FFFFu;  // Yellow (waypoint)
        
        // Draw a small ring at each control point
        renderer.DrawRing(pt.WorldPosition, pt.Normal, 0.5f, color);
    }
    
    // Draw lines connecting the points (and spline if enough points)
    if (m_PathPoints.size() >= 2)
    {
        if (m_PathPoints.size() >= 4)
        {
            // Draw smooth spline
            std::vector<RiverEditorPathPoint> spline = GenerateSplinePath();
            for (size_t i = 0; i < spline.size() - 1; ++i)
            {
                renderer.DrawDebugLineColored(spline[i].WorldPosition, spline[i + 1].WorldPosition, 0xFFFFFF00u); // Cyan line
            }
        }
        else
        {
            // Draw straight lines between control points
            for (size_t i = 0; i < m_PathPoints.size() - 1; ++i)
            {
                renderer.DrawDebugLineColored(m_PathPoints[i].WorldPosition, m_PathPoints[i + 1].WorldPosition, 0xFFFFFF00u);
            }
        }
    }
    
    // If draw mode is active, show terrain brush radius on target terrain
    if (m_DrawModeActive && HasValidTarget())
    {
        EntityData* data = GetTargetEntityData();
        if (data && data->Terrain)
        {
            // Brush radius is already shown during Update when hovering
        }
    }
}

void RiverCutterPanel::OnImGuiRender()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(380.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("River Cutter", &m_Open))
    {
        if (!m_Open)
        {
            m_DrawModeActive = false;
        }
        ImGui::End();
        return;
    }

    DrawTargetSelector();
    ImGui::Spacing();
    DrawPathControls();
    ImGui::Spacing();
    DrawSettingsUI();
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canGenerate = HasValidTarget() && m_PathPoints.size() >= 2;
    
    // Generate button
    ImGui::BeginDisabled(!canGenerate);
    if (ImGui::Button("Generate River", ImVec2(140, 32)))
    {
        SaveUndoState();
        GenerateRiver();
    }
    ImGui::EndDisabled();
    
    ImGui::SameLine();
    
    // Undo button
    ImGui::BeginDisabled(!m_HasUndo || !HasValidTarget());
    if (ImGui::Button("Undo", ImVec2(80, 32)))
    {
        UndoLastOperation();
    }
    ImGui::EndDisabled();

    if (!HasValidTarget())
    {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "Select an entity with a Terrain component.");
    }
    else if (m_PathPoints.size() < 2)
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.3f, 1), "Add at least 2 path points to generate a river.");
    }

    if (!m_StatusLine.empty())
    {
        ImGui::TextWrapped("%s", m_StatusLine.c_str());
    }
    if (m_LastOperationMs > 0.0)
    {
        ImGui::Text("Last operation: %.1f ms", m_LastOperationMs);
    }

    ImGui::End();
    
    // Handle window close
    if (!m_Open)
    {
        m_DrawModeActive = false;
    }
}

void RiverCutterPanel::DrawTargetSelector()
{
    if (!m_Context)
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Scene context unavailable.");
        return;
    }

    std::vector<std::pair<EntityID, std::string>> terrains;
    for (const Entity& entity : m_Context->GetEntities())
    {
        EntityData* data = m_Context->GetEntityData(entity.GetID());
        if (data && data->Terrain)
        {
            terrains.emplace_back(entity.GetID(), data->Name);
        }
    }

    if (terrains.empty())
    {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "No terrain entities in this scene.");
        m_TargetTerrain = INVALID_ENTITY_ID;
        return;
    }

    const char* currentLabel = "<none>";
    bool foundMatch = false;
    for (auto& entry : terrains)
    {
        if (entry.first == m_TargetTerrain)
        {
            currentLabel = entry.second.c_str();
            foundMatch = true;
            break;
        }
    }
    if (!foundMatch && !terrains.empty())
    {
        m_TargetTerrain = terrains.front().first;
        currentLabel = terrains.front().second.c_str();
    }

    if (ImGui::BeginCombo("Terrain Entity", currentLabel))
    {
        for (auto& entry : terrains)
        {
            bool selected = entry.first == m_TargetTerrain;
            if (ImGui::Selectable(entry.second.c_str(), selected))
            {
                m_TargetTerrain = entry.first;
                m_HasUndo = false;
                ClearPath();
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Use Selected") && m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID)
    {
        m_TargetTerrain = *m_SelectedEntity;
        m_HasUndo = false;
        ClearPath();
    }
}

void RiverCutterPanel::DrawPathControls()
{
    ImGui::SeparatorText("Path Drawing");
    
    // Draw mode toggle button
    if (m_DrawModeActive)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.7f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.5f, 0.15f, 1.0f));
        
        if (ImGui::Button("Stop Draw Mode", ImVec2(160, 28)))
        {
            m_DrawModeActive = false;
        }
        
        ImGui::PopStyleColor(3);
        
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "ACTIVE");
    }
    else
    {
        ImGui::BeginDisabled(!HasValidTarget());
        if (ImGui::Button("Start Draw Mode", ImVec2(160, 28)))
        {
            // Disable terrain painter brush mode when entering river cutter draw mode
            if (TerrainPainter::IsBrushModeEnabled())
            {
                TerrainPainter::SetBrushModeEnabled(false);
            }
            m_DrawModeActive = true;
        }
        ImGui::EndDisabled();
    }
    
    ImGui::Spacing();
    
    // Path info
    ImGui::Text("Path Points: %zu", m_PathPoints.size());
    
    ImGui::SameLine(200);
    ImGui::BeginDisabled(m_PathPoints.empty());
    if (ImGui::Button("Clear Path"))
    {
        ClearPath();
    }
    ImGui::EndDisabled();
    
    ImGui::SameLine();
    ImGui::BeginDisabled(m_PathPoints.empty());
    if (ImGui::Button("Remove Last"))
    {
        RemoveLastPoint();
    }
    ImGui::EndDisabled();
    
    // Instructions
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    if (m_DrawModeActive)
    {
        ImGui::TextWrapped("Left-click on terrain to add path points. Right-click to remove last point. Press Escape to exit draw mode.");
    }
    else
    {
        ImGui::TextWrapped("Click 'Start Draw Mode' to begin placing path points on the terrain.");
    }
    ImGui::PopStyleColor();
    
    // Show brush radius info
    if (HasValidTarget())
    {
        EntityData* data = GetTargetEntityData();
        if (data && data->Terrain)
        {
            ImGui::Spacing();
            ImGui::Text("Using Brush Radius: %.2f", data->Terrain->Brush.Radius);
            ImGui::Text("Using Brush Strength: %.2f", data->Terrain->Brush.Strength);
        }
    }
}

void RiverCutterPanel::DrawSettingsUI()
{
    ImGui::SeparatorText("Carving Settings");
    
    ImGui::SliderFloat("Depth", &m_Settings.Depth, 0.1f, 10.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Maximum depth of the river channel at its center");
    
    ImGui::SliderFloat("Bank Smoothing", &m_Settings.BankSmoothing, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How smoothly the riverbanks transition to the terrain");
    
    ImGui::SliderInt("Spline Subdivision", &m_Settings.SplineSubdivision, 1, 16);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Number of subdivisions between control points for smooth curves");
    
    ImGui::Checkbox("Smooth After Carve", &m_Settings.SmoothAfterCarve);
    if (m_Settings.SmoothAfterCarve)
    {
        ImGui::SliderInt("Smooth Iterations", &m_Settings.SmoothIterations, 1, 10);
    }
    
    ImGui::Spacing();
    ImGui::SeparatorText("Water Mesh Settings");
    
    ImGui::Checkbox("Create Water Mesh", &m_Settings.CreateWaterMesh);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Generate a water surface mesh following the river path");
    
    if (m_Settings.CreateWaterMesh)
    {
        ImGui::SliderFloat("Water Height Offset", &m_Settings.WaterHeightOffset, -0.5f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Height of water surface above the carved riverbed");
        
        ImGui::ColorEdit4("Water Color", &m_Settings.WaterColor.x);
        
        ImGui::SliderFloat("Flow Speed", &m_Settings.FlowSpeed, 0.0f, 5.0f, "%.1f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Speed of water flow animation (UV scrolling)");
    }
}

void RiverCutterPanel::ClearPath()
{
    m_PathPoints.clear();
    m_StatusLine.clear();
}

void RiverCutterPanel::AddPathPoint(const glm::vec3& worldPos, const glm::vec3& localPos, const glm::vec3& normal)
{
    RiverEditorPathPoint pt;
    pt.WorldPosition = worldPos;
    pt.LocalPosition = localPos;
    pt.Normal = normal;
    m_PathPoints.push_back(pt);
}

void RiverCutterPanel::RemoveLastPoint()
{
    if (!m_PathPoints.empty())
    {
        m_PathPoints.pop_back();
    }
}

std::vector<RiverEditorPathPoint> RiverCutterPanel::GenerateSplinePath() const
{
    std::vector<RiverEditorPathPoint> result;
    
    if (m_PathPoints.size() < 2)
        return result;
    
    if (m_PathPoints.size() < 4)
    {
        // Not enough points for Catmull-Rom, just return linear interpolation
        for (size_t i = 0; i < m_PathPoints.size() - 1; ++i)
        {
            for (int j = 0; j <= m_Settings.SplineSubdivision; ++j)
            {
                float t = static_cast<float>(j) / static_cast<float>(m_Settings.SplineSubdivision);
                RiverEditorPathPoint pt;
                pt.WorldPosition = glm::mix(m_PathPoints[i].WorldPosition, m_PathPoints[i + 1].WorldPosition, t);
                pt.LocalPosition = glm::mix(m_PathPoints[i].LocalPosition, m_PathPoints[i + 1].LocalPosition, t);
                pt.Normal = glm::normalize(glm::mix(m_PathPoints[i].Normal, m_PathPoints[i + 1].Normal, t));
                result.push_back(pt);
            }
        }
        return result;
    }
    
    // Catmull-Rom spline interpolation
    for (size_t i = 0; i < m_PathPoints.size() - 1; ++i)
    {
        // Get four control points (clamping at boundaries)
        size_t i0 = (i == 0) ? 0 : i - 1;
        size_t i1 = i;
        size_t i2 = i + 1;
        size_t i3 = std::min(i + 2, m_PathPoints.size() - 1);
        
        const RiverEditorPathPoint& p0 = m_PathPoints[i0];
        const RiverEditorPathPoint& p1 = m_PathPoints[i1];
        const RiverEditorPathPoint& p2 = m_PathPoints[i2];
        const RiverEditorPathPoint& p3 = m_PathPoints[i3];
        
        for (int j = 0; j <= m_Settings.SplineSubdivision; ++j)
        {
            // Skip the last point of each segment except for the final segment
            if (j == m_Settings.SplineSubdivision && i < m_PathPoints.size() - 2)
                continue;
            
            float t = static_cast<float>(j) / static_cast<float>(m_Settings.SplineSubdivision);
            
            RiverEditorPathPoint pt;
            pt.WorldPosition = CatmullRom(p0.WorldPosition, p1.WorldPosition, p2.WorldPosition, p3.WorldPosition, t);
            pt.LocalPosition = CatmullRom(p0.LocalPosition, p1.LocalPosition, p2.LocalPosition, p3.LocalPosition, t);
            pt.Normal = glm::normalize(CatmullRom(p0.Normal, p1.Normal, p2.Normal, p3.Normal, t));
            
            result.push_back(pt);
        }
    }
    
    return result;
}

void RiverCutterPanel::SaveUndoState()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    m_UndoBuffer = data->Terrain->HeightMap;
    m_HasUndo = true;
}

void RiverCutterPanel::UndoLastOperation()
{
    if (!m_HasUndo) return;
    
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    terrain.HeightMap = m_UndoBuffer;
    
    int res = terrain.GridResolution;
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    Terrain::MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    m_HasUndo = false;
    m_StatusLine = "Undo applied.";
}

void RiverCutterPanel::GenerateRiver()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain)
        return;
    
    if (m_PathPoints.size() < 2)
        return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    
    // Get brush radius from terrain component
    float brushRadius = terrain.Brush.Radius;
    float brushStrength = terrain.Brush.Strength;
    
    // Get cell size for local->grid conversion
    glm::vec2 cellSize = Terrain::GetCellSize(terrain);
    if (cellSize.x <= 0.0f || cellSize.y <= 0.0f)
    {
        m_StatusLine = "Invalid terrain cell size.";
        return;
    }
    
    // Generate the smooth spline path
    std::vector<RiverEditorPathPoint> splinePath = GenerateSplinePath();
    
    if (splinePath.empty())
    {
        m_StatusLine = "Failed to generate spline path.";
        return;
    }
    
    // Calculate the depth in normalized height units
    float maxHeight = glm::max(0.001f, terrain.MaxHeight);
    float depthNormalized = m_Settings.Depth / maxHeight;
    
    // Step size for continuous carving - use a fraction of brush radius for smooth overlap
    float stepSize = brushRadius * 0.25f;  // 25% of brush radius ensures good overlap
    
    // Helper lambda to carve at a specific local position
    auto carveAtPosition = [&](const glm::vec3& localPos) {
        // Convert local position to grid coordinates
        float gx = localPos.x / cellSize.x;
        float gz = localPos.z / cellSize.y;
        
        // Calculate brush area in grid coordinates
        float radiusInCells = brushRadius / cellSize.x;
        int minX = static_cast<int>(std::floor(gx - radiusInCells));
        int maxX = static_cast<int>(std::ceil(gx + radiusInCells));
        int minZ = static_cast<int>(std::floor(gz - radiusInCells));
        int maxZ = static_cast<int>(std::ceil(gz + radiusInCells));
        
        minX = std::clamp(minX, 0, res - 1);
        maxX = std::clamp(maxX, 0, res - 1);
        minZ = std::clamp(minZ, 0, res - 1);
        maxZ = std::clamp(maxZ, 0, res - 1);
        
        // Carve within the brush area
        for (int z = minZ; z <= maxZ; ++z)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                // Calculate distance from this cell to the path point
                float dx = (static_cast<float>(x) - gx) * cellSize.x;
                float dz = (static_cast<float>(z) - gz) * cellSize.y;
                float dist = std::sqrt(dx * dx + dz * dz);
                
                if (dist > brushRadius)
                    continue;
                
                // Calculate falloff based on distance from center
                float normalizedDist = dist / brushRadius;
                
                // Smooth falloff using smoothstep-like curve
                float falloff = 1.0f - normalizedDist;
                falloff = falloff * falloff * (3.0f - 2.0f * falloff);  // smoothstep
                falloff = glm::mix(falloff, falloff * falloff, m_Settings.BankSmoothing);
                
                // Calculate the amount to lower
                float lowerAmount = depthNormalized * falloff * brushStrength * 0.1f;
                
                // Apply the carving
                float currentHeight = field.Get(x, z);
                float newHeight = currentHeight - lowerAmount;
                field.Set(x, z, glm::clamp(newHeight, 0.0f, 1.0f));
            }
        }
    };
    
    // Carve the river along the spline with dense sampling for continuity
    int totalCarvePoints = 0;
    for (size_t i = 0; i < splinePath.size(); ++i)
    {
        const RiverEditorPathPoint& pt = splinePath[i];
        
        // Always carve at this spline point
        carveAtPosition(pt.LocalPosition);
        ++totalCarvePoints;
        
        // If there's a next point, interpolate between them for continuous coverage
        if (i < splinePath.size() - 1)
        {
            const RiverEditorPathPoint& nextPt = splinePath[i + 1];
            
            // Calculate distance between consecutive spline points
            glm::vec3 delta = nextPt.LocalPosition - pt.LocalPosition;
            float segmentLength = glm::length(delta);
            
            // If segment is longer than step size, add intermediate carve points
            if (segmentLength > stepSize)
            {
                int numSteps = static_cast<int>(std::ceil(segmentLength / stepSize));
                for (int step = 1; step < numSteps; ++step)
                {
                    float t = static_cast<float>(step) / static_cast<float>(numSteps);
                    glm::vec3 interpPos = glm::mix(pt.LocalPosition, nextPt.LocalPosition, t);
                    carveAtPosition(interpPos);
                    ++totalCarvePoints;
                }
            }
        }
    }
    
    // Optional smoothing pass
    if (m_Settings.SmoothAfterCarve && m_Settings.SmoothIterations > 0)
    {
        std::vector<uint16_t> temp = terrain.HeightMap;
        
        for (int iter = 0; iter < m_Settings.SmoothIterations; ++iter)
        {
            temp = terrain.HeightMap;
            TerrainHeightField source(temp, res);
            
            // Only smooth cells near the river path
            for (const RiverEditorPathPoint& pt : splinePath)
            {
                float gx = pt.LocalPosition.x / cellSize.x;
                float gz = pt.LocalPosition.z / cellSize.y;
                
                float radiusInCells = (brushRadius * 1.5f) / cellSize.x;
                int minX = static_cast<int>(std::floor(gx - radiusInCells));
                int maxX = static_cast<int>(std::ceil(gx + radiusInCells));
                int minZ = static_cast<int>(std::floor(gz - radiusInCells));
                int maxZ = static_cast<int>(std::ceil(gz + radiusInCells));
                
                minX = std::clamp(minX, 1, res - 2);
                maxX = std::clamp(maxX, 1, res - 2);
                minZ = std::clamp(minZ, 1, res - 2);
                maxZ = std::clamp(maxZ, 1, res - 2);
                
                for (int z = minZ; z <= maxZ; ++z)
                {
                    for (int x = minX; x <= maxX; ++x)
                    {
                        float sum = 0.0f;
                        int count = 0;
                        
                        for (int dz = -1; dz <= 1; ++dz)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                sum += source.Get(x + dx, z + dz);
                                ++count;
                            }
                        }
                        
                        float smoothed = sum / static_cast<float>(count);
                        float original = source.Get(x, z);
                        field.Set(x, z, glm::mix(original, smoothed, 0.3f));
                    }
                }
            }
        }
    }
    
    // Mark terrain as dirty
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    // Generate water mesh if enabled
    if (m_Settings.CreateWaterMesh)
    {
        GenerateWaterMesh();
    }
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream oss;
    oss << "River carved with " << m_PathPoints.size() << " control points, " 
        << totalCarvePoints << " carve samples.";
    if (m_Settings.CreateWaterMesh)
    {
        oss << " Water mesh created.";
    }
    m_StatusLine = oss.str();
}

void RiverCutterPanel::GenerateWaterMesh()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain)
        return;
    
    if (m_PathPoints.size() < 2)
        return;
    
    TerrainComponent& terrain = *data->Terrain;
    float brushRadius = terrain.Brush.Radius;
    
    // Get cell size for local->world conversion
    glm::vec2 cellSize = Terrain::GetCellSize(terrain);
    if (cellSize.x <= 0.0f || cellSize.y <= 0.0f)
        return;
    
    // Generate the smooth spline path
    std::vector<RiverEditorPathPoint> splinePath = GenerateSplinePath();
    if (splinePath.size() < 2)
        return;
    
    // Build mesh data
    std::vector<glm::vec3> vertices;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> indices;
    
    float totalLength = 0.0f;
    std::vector<float> segmentLengths;
    segmentLengths.push_back(0.0f);
    
    // Calculate total path length for UV mapping
    for (size_t i = 1; i < splinePath.size(); ++i)
    {
        float segLen = glm::length(splinePath[i].LocalPosition - splinePath[i - 1].LocalPosition);
        totalLength += segLen;
        segmentLengths.push_back(totalLength);
    }
    
    // Generate vertices along the path
    for (size_t i = 0; i < splinePath.size(); ++i)
    {
        const RiverEditorPathPoint& pt = splinePath[i];
        
        // Calculate tangent (direction of flow)
        glm::vec3 tangent;
        if (i == 0)
        {
            tangent = glm::normalize(splinePath[1].LocalPosition - splinePath[0].LocalPosition);
        }
        else if (i == splinePath.size() - 1)
        {
            tangent = glm::normalize(splinePath[i].LocalPosition - splinePath[i - 1].LocalPosition);
        }
        else
        {
            tangent = glm::normalize(splinePath[i + 1].LocalPosition - splinePath[i - 1].LocalPosition);
        }
        
        // Calculate perpendicular (river width direction)
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        glm::vec3 perpendicular = glm::normalize(glm::cross(up, tangent));
        
        // Sample terrain height at this point for water surface
        float terrainHeight = Terrain::SampleHeightWorld(terrain, pt.LocalPosition.x, pt.LocalPosition.z);
        float waterY = terrainHeight + m_Settings.WaterHeightOffset;
        
        // Create left and right vertices
        glm::vec3 leftPos = pt.LocalPosition + perpendicular * brushRadius;
        glm::vec3 rightPos = pt.LocalPosition - perpendicular * brushRadius;
        leftPos.y = waterY;
        rightPos.y = waterY;
        
        vertices.push_back(leftPos);
        vertices.push_back(rightPos);
        
        // Normals point up
        normals.push_back(up);
        normals.push_back(up);
        
        // UVs: U goes across river width (0 to 1), V goes along flow direction
        float v = (totalLength > 0.0f) ? segmentLengths[i] / totalLength : 0.0f;
        // Scale V by river length for proper tiling
        float vScaled = v * (totalLength / brushRadius);
        uvs.push_back(glm::vec2(0.0f, vScaled));
        uvs.push_back(glm::vec2(1.0f, vScaled));
    }
    
    // Generate indices (triangle strip as quads)
    for (size_t i = 0; i < splinePath.size() - 1; ++i)
    {
        uint32_t bl = static_cast<uint32_t>(i * 2);       // Bottom-left
        uint32_t br = static_cast<uint32_t>(i * 2 + 1);   // Bottom-right
        uint32_t tl = static_cast<uint32_t>((i + 1) * 2); // Top-left
        uint32_t tr = static_cast<uint32_t>((i + 1) * 2 + 1); // Top-right
        
        // Two triangles per quad (counter-clockwise winding for top face)
        indices.push_back(bl);
        indices.push_back(tl);
        indices.push_back(br);
        
        indices.push_back(br);
        indices.push_back(tl);
        indices.push_back(tr);
    }
    
    if (vertices.empty() || indices.empty())
        return;
    
    // Create or update the river mesh entity
    CreateRiverMeshEntity(vertices, normals, uvs, indices);
}

EntityID RiverCutterPanel::CreateRiverMeshEntity(
    const std::vector<glm::vec3>& vertices,
    const std::vector<glm::vec3>& normals,
    const std::vector<glm::vec2>& uvs,
    const std::vector<uint32_t>& indices)
{
    if (!m_Context || !HasValidTarget())
        return INVALID_ENTITY_ID;
    
    EntityData* terrainData = GetTargetEntityData();
    if (!terrainData)
        return INVALID_ENTITY_ID;
    
    // Build GPU buffers
    std::vector<PBRVertex> pbrVertices;
    pbrVertices.reserve(vertices.size());
    
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        PBRVertex v;
        v.x = vertices[i].x;
        v.y = vertices[i].y;
        v.z = vertices[i].z;
        v.nx = normals[i].x;
        v.ny = normals[i].y;
        v.nz = normals[i].z;
        v.u = uvs[i].x;
        v.v = uvs[i].y;
        pbrVertices.push_back(v);
    }
    
    // Create the mesh
    auto riverMesh = std::make_shared<Mesh>();
    riverMesh->Vertices = vertices;
    riverMesh->Normals = normals;
    riverMesh->UVs = uvs;
    riverMesh->Indices = indices;
    riverMesh->numVertices = static_cast<uint32_t>(vertices.size());
    riverMesh->numIndices = static_cast<uint32_t>(indices.size());
    riverMesh->ComputeBounds();
    
    // Create GPU buffers
    PBRVertex::Init();
    const bgfx::Memory* vMem = bgfx::copy(pbrVertices.data(), 
        static_cast<uint32_t>(pbrVertices.size() * sizeof(PBRVertex)));
    riverMesh->vbh = bgfx::createVertexBuffer(vMem, PBRVertex::layout);
    
    const bgfx::Memory* iMem = bgfx::copy(indices.data(), 
        static_cast<uint32_t>(indices.size() * sizeof(uint32_t)));
    riverMesh->ibh = bgfx::createIndexBuffer(iMem, BGFX_BUFFER_INDEX32);
    
    // Create a default PBR material for the water mesh
    auto material = MaterialManager::Instance().CreateDefaultPBRMaterial();
    
    // Look for existing river entity as child of terrain, or create new one
    EntityID riverEntityId = INVALID_ENTITY_ID;
    std::string riverName = terrainData->Name + "_River";
    
    // Check if terrain already has a River component with a mesh entity
    if (terrainData->River && terrainData->River->MeshEntity != INVALID_ENTITY_ID)
    {
        riverEntityId = terrainData->River->MeshEntity;
        EntityData* existingRiver = m_Context->GetEntityData(riverEntityId);
        if (existingRiver && existingRiver->Mesh)
        {
            // Update existing mesh
            // Destroy old GPU resources
            if (bgfx::isValid(existingRiver->Mesh->mesh->vbh))
                bgfx::destroy(existingRiver->Mesh->mesh->vbh);
            if (bgfx::isValid(existingRiver->Mesh->mesh->ibh))
                bgfx::destroy(existingRiver->Mesh->mesh->ibh);
            
            existingRiver->Mesh->mesh = riverMesh;
            existingRiver->Mesh->MeshName = "RiverMesh";
        }
    }
    else
    {
        // Create new entity
        Entity riverEntity = m_Context->CreateEntity(riverName);
        riverEntityId = riverEntity.GetID();
        
        EntityData* riverData = m_Context->GetEntityData(riverEntityId);
        if (riverData)
        {
            // Set up as child of terrain
            riverData->Parent = m_TargetTerrain;
            terrainData->Children.push_back(riverEntityId);
            
            // Add mesh component
            riverData->Mesh = std::make_unique<MeshComponent>(riverMesh, "RiverMesh", material);
            riverData->Mesh->ShowBackfaces = true;  // Show both sides of water
            
            // Transform is at origin relative to terrain (mesh uses local coords)
            riverData->Transform.Position = glm::vec3(0.0f);
            riverData->Transform.TransformDirty = true;
        }
    }
    
    // Create or update River component on terrain
    if (!terrainData->River)
    {
        terrainData->River = std::make_unique<RiverComponent>();
    }
    
    // Store path data for serialization
    terrainData->River->PathPoints.clear();
    for (const auto& pt : m_PathPoints)
    {
        ::RiverPathPoint rpt;
        rpt.Position = pt.LocalPosition;
        rpt.Normal = pt.Normal;
        terrainData->River->PathPoints.push_back(rpt);
    }
    
    terrainData->River->Width = GetTargetEntityData()->Terrain->Brush.Radius;
    terrainData->River->Depth = m_Settings.Depth;
    terrainData->River->BankSmoothing = m_Settings.BankSmoothing;
    terrainData->River->SplineSubdivision = m_Settings.SplineSubdivision;
    terrainData->River->WaterHeight = m_Settings.WaterHeightOffset;
    terrainData->River->WaterColor = m_Settings.WaterColor;
    terrainData->River->FlowSpeed = m_Settings.FlowSpeed;
    terrainData->River->MeshEntity = riverEntityId;
    
    // Save the mesh asset for runtime loading
    River::SaveRiverMeshAsset(*terrainData->River, vertices, normals, uvs, indices);
    
    terrainData->River->MeshGenerated = true;
    terrainData->River->NeedsRegeneration = false;
    
    m_Context->MarkDirty();
    
    return riverEntityId;
}

