#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Scene;
struct EntityData;

// A single layer distribution rule
struct SplatLayerRule
{
    std::string LayerName = "Untitled";
    int LayerIndex = 0;                  // Which splat channel (0-3 for RGBA)
    bool Enabled = true;
    
    // Height-based distribution
    bool UseHeight = true;
    float HeightMin = 0.0f;              // Normalized 0-1
    float HeightMax = 1.0f;
    float HeightFalloff = 0.1f;          // Soft transition at edges
    
    // Slope-based distribution
    bool UseSlope = false;
    float SlopeMin = 0.0f;               // In degrees
    float SlopeMax = 90.0f;
    float SlopeFalloff = 5.0f;           // Soft transition in degrees
    
    // Noise modulation
    bool UseNoise = false;
    float NoiseStrength = 0.3f;
    float NoiseFrequency = 4.0f;
    uint32_t NoiseSeed = 12345u;
    
    // Priority - higher priority layers paint over lower
    int Priority = 0;
    
    // Curvature-based (concave vs convex)
    bool UseCurvature = false;
    float CurvatureMin = -1.0f;          // -1 = concave, +1 = convex
    float CurvatureMax = 1.0f;
    
    // Distance from water/shore
    bool UseWaterDistance = false;
    float WaterDistanceMin = 0.0f;
    float WaterDistanceMax = 0.1f;
    float WaterLevel = 0.37f;
};

// Preset rule configurations
enum class SplatPreset
{
    Custom,
    Standard,           // Grass, rock cliffs, sand beaches, snow peaks
    Desert,             // Sand, rock, gravel
    Alpine,             // Grass, rock, snow, ice
    Tropical,           // Jungle grass, mud, sand, rock
    Volcanic,           // Ash, lava rock, obsidian
    Grassland,          // Multiple grass types by height
    Count
};

class SplatmapGeneratorPanel : public EditorPanel
{
public:
    SplatmapGeneratorPanel(Scene* scene, EntityID* selectedEntity);

    void OnImGuiRender();
    void Open();
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }

private:
    void DrawTargetSelector();
    void DrawPresetSelector();
    void DrawLayerRules();
    void DrawLayerRuleEditor(SplatLayerRule& rule, int index);
    void DrawPreview();
    
    bool HasValidTarget() const;
    EntityData* GetTargetEntityData() const;
    void SyncSelectionTarget();
    
    void ApplyPreset(SplatPreset preset);
    void GenerateSplatmap();
    void UndoLastOperation();
    void SaveUndoState();
    
    // Computation helpers
    float ComputeSlope(int x, int y, const std::vector<uint16_t>& heightMap, int res, float maxHeight, const glm::vec2& worldSize) const;
    float ComputeCurvature(int x, int y, const std::vector<uint16_t>& heightMap, int res) const;
    float ComputeLayerWeight(const SplatLayerRule& rule, float height, float slope, float curvature, float waterDist, const glm::vec2& uv) const;

private:
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_TargetTerrain = INVALID_ENTITY_ID;
    bool m_Open = false;
    
    SplatPreset m_CurrentPreset = SplatPreset::Standard;
    std::vector<SplatLayerRule> m_Rules;
    
    // Preview settings
    bool m_ShowPreview = true;
    int m_PreviewLayer = -1;             // -1 = show all, 0-3 = show specific channel
    
    double m_LastOperationMs = 0.0;
    std::string m_StatusLine;
    
    // Undo support
    std::vector<glm::u8vec4> m_UndoBuffer;
    std::vector<glm::u8vec4> m_UndoBuffer2;
    bool m_HasUndo = false;
};

