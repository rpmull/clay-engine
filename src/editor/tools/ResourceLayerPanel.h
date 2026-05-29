#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/resourcelayer/ClimateTypes.h"
#include "core/resourcelayer/EligibilityFilter.h"

#include <glm/glm.hpp>
#include <string>
#include <vector>

class Scene;
class Camera;
struct EntityData;

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// ResourceLayerPanel - Modern editor panel for resource layer configuration
//------------------------------------------------------------------------------
class ResourceLayerPanel : public EditorPanel {
public:
    ResourceLayerPanel(Scene* scene, EntityID* selectedEntity);
    
    // Call every frame from editor
    void Update(bool viewportHovered, bool playMode, Camera* viewportCamera);
    
    // ImGui panel rendering
    void OnImGuiRender();
    
    // Panel state
    void Open();
    void SetOpen(bool open) { m_Open = open; }
    void Close();
    bool IsOpen() const { return m_Open; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }
    
    // Draw eligibility preview overlay in viewport
    void DrawEligibilityPreview();
    
private:
    //--------------------------------------------------------------------------
    // UI Sections
    //--------------------------------------------------------------------------
    void DrawHeader();
    void DrawTargetSelector();
    void DrawClimateSection();
    void DrawClimateGradientEditor(ClimateGradient& gradient, const char* label);
    void DrawLayerList();
    void DrawLayerSettings();
    void DrawEligibilityFilters();
    void DrawFilterInspector(IEligibilityFilter* filter, size_t index);
    void DrawDistributionSettings();
    void DrawLODSettings();
    void DrawStatistics();
    void DrawFooter();
    
    //--------------------------------------------------------------------------
    // Layer Management
    //--------------------------------------------------------------------------
    void AddNewLayer();
    void DuplicateLayer(size_t index);
    void RemoveLayer(size_t index);
    void MoveLayerUp(size_t index);
    void MoveLayerDown(size_t index);
    
    //--------------------------------------------------------------------------
    // Filter Management
    //--------------------------------------------------------------------------
    void AddFilter(const std::string& typeName);
    void RemoveFilter(size_t index);
    void MoveFilterUp(size_t index);
    void MoveFilterDown(size_t index);
    
    //--------------------------------------------------------------------------
    // Generation
    //--------------------------------------------------------------------------
    void RegenerateAll();
    void RegenerateSelectedLayer();
    void BakeImposters();
    
    //--------------------------------------------------------------------------
    // Helper Functions
    //--------------------------------------------------------------------------
    bool HasValidTarget() const;
    EntityData* GetTargetEntityData() const;
    ResourceLayerComponent* GetResourceLayerComponent() const;
    void SyncSelectionTarget();
    
    //--------------------------------------------------------------------------
    // State
    //--------------------------------------------------------------------------
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_TargetTerrain = INVALID_ENTITY_ID;
    bool m_Open = false;
    
    // Selection
    int m_SelectedLayerIndex = -1;
    int m_SelectedFilterIndex = -1;
    
    // Preview
    bool m_ShowEligibilityPreview = false;
    int m_PreviewLayerIndex = -1;
    float m_PreviewOpacity = 0.5f;
    
    // UI state
    bool m_ShowClimateSection = true;
    bool m_ShowLayerList = true;
    bool m_ShowLayerSettings = true;
    bool m_ShowStatistics = true;
    
    // Add filter popup
    bool m_ShowAddFilterPopup = false;
    
    // Status
    std::string m_StatusMessage;
    float m_StatusTimer = 0.0f;
    
    // Camera for viewport calculations
    Camera* m_ViewportCamera = nullptr;
};

} // namespace resourcelayer
} // namespace cm


