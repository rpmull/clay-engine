#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

class Scene;
struct EntityData;

struct TerrainChunkSummary
{
    int chunkX = 0;
    int chunkY = 0;
    glm::ivec2 start = glm::ivec2(0);
    glm::ivec2 size = glm::ivec2(0);
    float minHeight = 0.0f;
    float maxHeight = 0.0f;
    float avgSlope = 0.0f;
    float landCoverage = 0.0f;
    float riverCoverage = 0.0f;
};

// Terrain zone types - what kind of terrain a region produces
enum class TerrainZone
{
    Mountains,      // High ridged peaks with snow caps
    Highlands,      // Elevated rolling hills
    Plains,         // Flat grassland with gentle variation
    Valley,         // Low depression, fertile lowlands
    Cliffs,         // Sharp vertical drops, dramatic edges
    Beach,          // Sandy gradual slope toward water
    Mesa,           // Flat-topped elevated formations
    Canyon,         // Deep carved channels and pits
    Wetlands,       // Low flat terrain with river influence
    Plateau,        // Elevated flat regions with terracing
    Count
};

// World template presets for quick setup
enum class WorldTemplate
{
    Custom,             // User-defined regions
    Continental,        // Classic landmass - mountains N, valley center, coast W
    IslandKingdom,      // Central highlands, beaches all around
    MountainFortress,   // Mountains on all borders, valley center
    GrandCanyon,        // Massive central pit/canyon system
    CoastalRange,       // Mountains W, descending to beaches E
    RiftValley,         // Mountains N/S, deep valley corridor E-W
    Archipelago,        // Scattered highlands with water between
    HighlandPlains,     // Gentle rolling terrain throughout
    Count
};

// UI complexity mode
enum class GeneratorMode
{
    Simple,     // Just pick a template and generate
    Region,     // Define terrain types per cardinal zone
    Freeform,   // Draw polygon coastline with feature stamps
    Advanced    // Full parameter control (legacy)
};

// Feature stamp types for painting onto terrain
enum class FeatureStamp
{
    None,
    Mountain,
    Hill,
    Valley,
    River,
    Mesa,
    Plateau,
    Canyon,
    Lake,
    Forest,     // Just a biome hint, doesn't affect height
    Count
};

// A single stamped feature on the terrain
struct TerrainFeatureStamp
{
    FeatureStamp Type = FeatureStamp::None;
    glm::vec2 Position = glm::vec2(0.5f);   // UV coordinates 0-1
    float Radius = 0.1f;                     // Size in UV space
    float Intensity = 1.0f;                  // How strong the effect is
    float Rotation = 0.0f;                   // For elongated features
    float Aspect = 1.0f;                     // Width/height ratio
};

// Polygon for coastline definition
struct CoastlinePolygon
{
    std::vector<glm::vec2> Points;          // Vertices in UV space (0-1)
    bool IsClosed = false;
    
    void Clear() { Points.clear(); IsClosed = false; }
    bool IsValid() const { return IsClosed && Points.size() >= 3; }
};

class WorldGenerationPanel : public EditorPanel
{
public:
    enum class TerrainSizePreset
    {
        Small = 0,
        Medium,
        Large,
        Huge,
        Custom
    };

    // New region-based settings (simple and intuitive)
    struct RegionSettings
    {
        WorldTemplate Template = WorldTemplate::Continental;
        
        // Cardinal zone assignments (what terrain type per direction)
        TerrainZone North = TerrainZone::Mountains;
        TerrainZone South = TerrainZone::Cliffs;
        TerrainZone East = TerrainZone::Highlands;
        TerrainZone West = TerrainZone::Beach;
        TerrainZone Center = TerrainZone::Valley;
        
        // How much the center zone extends outward (0.0 = point, 0.5 = half the map)
        float CenterRadius = 0.28f;
        
        // How sharply regions transition (0 = very soft gradient, 1 = hard edge)
        float RegionBlendSharpness = 0.35f;
        
        // Overall terrain smoothness (0 = chaotic, 1 = very smooth base)
        float BaselineSmoothness = 0.6f;
        
        // How much local variation within each zone (0 = uniform, 1 = varied)
        float LocalVariation = 0.45f;
        
        // Feature intensity - how dramatic the terrain features are
        float FeatureIntensity = 0.5f;
        
        // Whether to add an ocean border around the continent
        bool OceanBorder = true;
        float OceanBorderWidth = 0.12f;
        
        // Coast irregularity (0 = smooth oval, 1 = complex fractal coast)
        float CoastComplexity = 0.4f;
        
        // River generation
        bool GenerateRivers = true;
        float RiverDensity = 0.3f;
    };
    
    // Freeform polygon-based coastline settings
    struct FreeformSettings
    {
        // The hand-drawn coastline polygon
        CoastlinePolygon Coastline;
        
        // Stamped features (mountains, valleys, etc.)
        std::vector<TerrainFeatureStamp> Stamps;
        
        // Base elevation for land inside polygon (0-1)
        float BaseElevation = 0.35f;
        
        // Shore/beach settings
        float ShoreWidth = 0.08f;           // How wide the beach gradient is (UV space)
        float ShoreNoiseStrength = 0.4f;    // Irregular shoreline detail
        float CliffChance = 0.2f;           // Probability of cliffs vs beaches
        
        // Interior terrain settings
        float TerrainRoughness = 0.3f;      // Base noise on interior
        float FeatureIntensity = 0.6f;      // How dramatic stamps are
        
        // Auto-generate some features
        bool AutoGenerateHills = true;
        int AutoHillCount = 8;
        bool AutoGenerateRivers = true;
        int AutoRiverCount = 3;
        
        // Currently selected stamp brush
        FeatureStamp CurrentBrush = FeatureStamp::Mountain;
        float BrushSize = 0.08f;
        float BrushIntensity = 0.8f;
    };

    // Legacy settings (all the detailed parameters, still available in Advanced mode)
    struct Settings
    {
        TerrainSizePreset SizePreset = TerrainSizePreset::Large;
        int GridResolution = 1024;
        int ChunkResolution = 128;
        glm::vec2 WorldSize = glm::vec2(3460.0f, 3460.0f);
        float MaxHeight = 260.0f;
        float SeaLevel = 0.37f;
        float OceanFloor = 0.04f;
        float BaseContinentFrequency = 0.68f;
        int BaseOctaves = 5;
        float BaseGain = 0.5f;
        float BaseLacunarity = 1.9f;
        float BaseWarpStrength = 0.22f;
        int MacroPlateCount = 3;
        float MacroMaskBlend = 0.78f;
        float MacroClusterStrength = 0.65f;
        float EdgeFadeStrength = 0.7f;
        float SeaShelfDepth = 0.09f;
        float InteriorHighlandsBoost = 0.23f;
        float BaseElevationMin = 0.16f;
        float BaseElevationMax = 0.78f;
        float MountainFrequency = 2.1f;
        float MountainIntensity = 0.62f;
        float MountainSharpness = 1.35f;
        int MountainOctaves = 4;
        float MountainLacunarity = 2.25f;
        float MountainGain = 0.46f;
        float MountainWarpStrength = 0.33f;
        float PlateauFrequency = 1.05f;
        float PlateauThreshold = 0.52f;
        float PlateauTerraceStrength = 0.72f;
        float PlateauPostTerraceStrength = 0.42f;
        float TerraceStep = 0.07f;
        float PlateauErosionDamping = 0.42f;
        float CanyonFrequency = 2.2f;
        float CanyonDepth = 0.06f;
        float CanyonWarpStrength = 0.32f;
        float CanyonSharpness = 1.45f;
        int FaultIterations = 56;
        float FaultDisplacement = 0.024f;
        float FaultFalloff = 0.6f;
        int ErosionDroplets = 22000;
        int ErosionMaxSteps = 65;
        float ErosionInertia = 0.06f;
        float ErosionCapacity = 4.4f;
        float ErosionMinSlope = 0.0008f;
        float ErosionDepositSpeed = 0.32f;
        float ErosionErodeSpeed = 0.42f;
        float ErosionEvaporateSpeed = 0.018f;
        float ErosionGravity = 4.3f;
        float ErosionStartWater = 1.1f;
        float ErosionStartSpeed = 1.05f;
        float HighlandErosionMultiplier = 0.36f;
        float RiverDensity = 0.23f;
        float RiverDepth = 0.05f;
        float RiverBankWidth = 55.0f;
        float RiverBankSmoothing = 0.55f;
        float RiverDistanceFalloff = 220.0f;
        float RiverFlowFloor = 35.0f;
        float CliffSlopeStart = 18.0f;
        float CliffSlopeMax = 58.0f;
        float SnowHeightStart = 0.7f;
        float SnowHeightFull = 0.9f;
        float SandHeightMax = 0.26f;
        float SandSlopeMax = 14.0f;
        float ValleyGrassBoost = 0.4f;
        uint32_t BaseSeed = 1337u;
        uint32_t MountainSeed = 9113u;
        uint32_t PlateauSeed = 60113u;
        uint32_t FaultSeed = 424242u;
        uint32_t ErosionSeed = 9901u;
        uint32_t RiverSeed = 771u;
    };

    WorldGenerationPanel(Scene* scene, EntityID* selectedEntity);

    void OnImGuiRender();
    void Open();
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }

    Settings& GetSettings() { return m_Settings; }
    const Settings& GetSettings() const { return m_Settings; }
    RegionSettings& GetRegionSettings() { return m_RegionSettings; }
    const RegionSettings& GetRegionSettings() const { return m_RegionSettings; }
    FreeformSettings& GetFreeformSettings() { return m_FreeformSettings; }
    const FreeformSettings& GetFreeformSettings() const { return m_FreeformSettings; }

private:
    void DrawTargetSelector();
    void DrawModeSelector();
    void DrawSimpleModeUI();
    void DrawRegionModeUI();
    void DrawFreeformModeUI();
    void DrawPolygonCanvas();
    void DrawFeatureStampControls();
    void DrawAdvancedModeUI();
    void DrawBaseSettings();
    void DrawFeatureSettings();
    void DrawRiverSettings();
    void DrawBiomeSettings();
    void DrawChunkSettings();
    void DrawChunkTable();
    void RandomizeSeeds();
    bool HasValidTarget() const;
    EntityData* GetTargetEntityData() const;
    void SyncSelectionTarget();
    void ApplyTemplate(WorldTemplate tmpl);
    void RunGeneration();
    void RunRegionBasedGeneration();
    void RunFreeformGeneration();
    void RunLegacyGeneration();
    
    // Polygon helpers
    void HandlePolygonDrawing(const glm::vec2& canvasMin, const glm::vec2& canvasSize);
    void AddStampAtPosition(const glm::vec2& uvPos);
    bool IsPointInPolygon(const glm::vec2& point, const CoastlinePolygon& poly) const;
    float DistanceToPolygonEdge(const glm::vec2& point, const CoastlinePolygon& poly) const;

private:
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_TargetTerrain = INVALID_ENTITY_ID;
    bool m_Open = false;
    GeneratorMode m_Mode = GeneratorMode::Region;
    Settings m_Settings;
    RegionSettings m_RegionSettings;
    FreeformSettings m_FreeformSettings;
    double m_LastGenerationMs = 0.0;
    std::string m_StatusLine;
    std::vector<TerrainChunkSummary> m_LastChunks;
    
    // Freeform drawing state
    bool m_IsDrawingPolygon = false;
    bool m_IsStamping = false;
    int m_SelectedVertex = -1;          // For dragging vertices
    int m_HoveredVertex = -1;
    int m_SelectedStamp = -1;           // For editing stamps
};




