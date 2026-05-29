#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Scene;
class Camera;
struct EntityData;

// A point along the river spline (editor-side with both world and local coords)
struct RiverEditorPathPoint
{
    glm::vec3 WorldPosition{ 0.0f };
    glm::vec3 LocalPosition{ 0.0f };
    glm::vec3 Normal{ 0.0f, 1.0f, 0.0f };
};

class RiverCutterPanel : public EditorPanel
{
public:
    // River carving settings
    struct RiverSettings
    {
        float Depth = 1.5f;               // Depth at center of river (world units)
        float BankSmoothing = 0.7f;       // How smoothly the banks transition
        int SplineSubdivision = 4;        // Subdivisions between control points for smooth curves
        bool SmoothAfterCarve = true;     // Apply smoothing pass after carving
        int SmoothIterations = 2;
        
        // Water mesh settings
        bool CreateWaterMesh = true;      // Create a water surface mesh
        float WaterHeightOffset = 0.05f;  // Height above carved terrain
        glm::vec4 WaterColor{ 0.15f, 0.35f, 0.55f, 0.85f };
        float FlowSpeed = 1.0f;
    };

    RiverCutterPanel(Scene* scene, EntityID* selectedEntity);

    // Call every frame from editor - handles drawing mode input
    void Update(bool viewportHovered, bool playMode, Camera* viewportCamera);
    
    void OnImGuiRender();
    void Open();
    void SetOpen(bool open) { m_Open = open; }
    void Close();
    void StopDrawMode() { m_DrawModeActive = false; }
    bool IsOpen() const { return m_Open; }
    bool IsDrawModeActive() const { return m_DrawModeActive; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }
    
    // Draw the spline path visualization
    void DrawPathVisualization();

private:
    void DrawTargetSelector();
    void DrawPathControls();
    void DrawSettingsUI();
    
    bool HasValidTarget() const;
    EntityData* GetTargetEntityData() const;
    void SyncSelectionTarget();
    
    // Path management
    void ClearPath();
    void AddPathPoint(const glm::vec3& worldPos, const glm::vec3& localPos, const glm::vec3& normal);
    void RemoveLastPoint();
    
    // Carving operations
    void GenerateRiver();
    void UndoLastOperation();
    void SaveUndoState();
    
    // Generate interpolated spline from control points
    std::vector<RiverEditorPathPoint> GenerateSplinePath() const;
    
    // Water mesh generation
    void GenerateWaterMesh();
    EntityID CreateRiverMeshEntity(const std::vector<glm::vec3>& vertices, 
                                    const std::vector<glm::vec3>& normals,
                                    const std::vector<glm::vec2>& uvs,
                                    const std::vector<uint32_t>& indices);

private:
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_TargetTerrain = INVALID_ENTITY_ID;
    bool m_Open = false;
    bool m_DrawModeActive = false;
    bool m_WasClickedLastFrame = false;
    
    // Path points (control points of the spline)
    std::vector<RiverEditorPathPoint> m_PathPoints;
    
    // Settings
    RiverSettings m_Settings;
    
    double m_LastOperationMs = 0.0;
    std::string m_StatusLine;
    
    // Undo support
    std::vector<uint16_t> m_UndoBuffer;
    bool m_HasUndo = false;
};

