#pragma once

#include "editor/ui/panels/EditorPanel.h"
#include "core/ecs/Entity.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

class Scene;
struct EntityData;

// Types of evolution/erosion operations
enum class EvolutionOperation
{
    HydraulicErosion,   // Water-based erosion (rivers, rain)
    ThermalErosion,     // Material falling down steep slopes
    WindErosion,        // Aeolian erosion and deposition
    TectonicUplift,     // Raise/lower regions
    Smoothing,          // Gaussian blur
    Roughening,         // Add noise/detail
    RiverCarving,       // Carve river channels
    CoastalErosion,     // Erode shorelines
    Terracing,          // Add terrace steps
    Count
};

class TerrainEvolutionPanel : public EditorPanel
{
public:
    // Hydraulic erosion settings (water-based)
    struct HydraulicSettings
    {
        int Iterations = 25000;
        int MaxSteps = 64;
        float Inertia = 0.05f;
        float Capacity = 4.0f;
        float MinSlope = 0.001f;
        float DepositSpeed = 0.3f;
        float ErodeSpeed = 0.3f;
        float EvaporateSpeed = 0.02f;
        float Gravity = 4.0f;
        float StartWater = 1.0f;
        float StartSpeed = 1.0f;
        uint32_t Seed = 12345u;
    };
    
    // Thermal erosion settings (talus/scree)
    struct ThermalSettings
    {
        int Iterations = 50;
        float TalusAngle = 35.0f;       // Angle of repose in degrees
        float ErosionRate = 0.5f;        // How much material moves per iteration
        float MinSlope = 0.01f;
    };
    
    // Wind erosion settings
    struct WindSettings
    {
        int Iterations = 1000;
        float WindStrength = 0.5f;
        glm::vec2 WindDirection = glm::vec2(1.0f, 0.0f);
        float SuspensionHeight = 0.3f;   // Height particles can be carried
        float DepositionRate = 0.4f;
        float AbrasionStrength = 0.2f;
        uint32_t Seed = 54321u;
    };
    
    // Tectonic settings
    struct TectonicSettings
    {
        float UpliftStrength = 0.1f;     // Positive = raise, negative = lower
        float UpliftRadius = 0.3f;       // Size of affected area (UV space)
        glm::vec2 UpliftCenter = glm::vec2(0.5f, 0.5f);
        float FalloffPower = 2.0f;       // How sharply it falls off
        bool AddFaultLines = false;
        int FaultCount = 3;
        float FaultDisplacement = 0.05f;
    };
    
    // Smoothing settings
    struct SmoothSettings
    {
        int Iterations = 3;
        int KernelRadius = 2;
        float Strength = 0.5f;           // Blend with original
        bool PreserveEdges = true;
        float EdgeThreshold = 0.1f;
    };
    
    // Roughening/noise settings
    struct RoughenSettings
    {
        float NoiseStrength = 0.1f;
        float NoiseFrequency = 8.0f;
        int NoiseOctaves = 4;
        float NoiseLacunarity = 2.0f;
        float NoiseGain = 0.5f;
        bool Additive = true;            // true = add noise, false = multiply
        uint32_t Seed = 99999u;
    };
    
    // River carving settings
    struct RiverSettings
    {
        float Density = 0.2f;
        float Depth = 0.04f;
        float BankWidth = 40.0f;
        float BankSmoothing = 0.5f;
        float MinFlowThreshold = 50.0f;
    };
    
    // Coastal erosion settings
    struct CoastalSettings
    {
        float SeaLevel = 0.37f;
        float ErosionStrength = 0.3f;
        float ErosionWidth = 0.05f;      // How far inland erosion reaches
        int Iterations = 10;
        bool CreateCliffs = true;
        float CliffHeight = 0.08f;
    };
    
    // Terracing settings
    struct TerraceSettings
    {
        float StepHeight = 0.05f;
        float Sharpness = 0.7f;          // How sharp the step edges are
        float MinHeight = 0.2f;          // Don't terrace below this
        float MaxHeight = 0.9f;          // Don't terrace above this
    };

    TerrainEvolutionPanel(Scene* scene, EntityID* selectedEntity);

    void OnImGuiRender();
    void Open();
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    void SetSelectedEntityPtr(EntityID* ptr) { m_SelectedEntity = ptr; }

private:
    void DrawTargetSelector();
    void DrawOperationSelector();
    void DrawHydraulicUI();
    void DrawThermalUI();
    void DrawWindUI();
    void DrawTectonicUI();
    void DrawSmoothUI();
    void DrawRoughenUI();
    void DrawRiverUI();
    void DrawCoastalUI();
    void DrawTerraceUI();
    
    bool HasValidTarget() const;
    EntityData* GetTargetEntityData() const;
    void SyncSelectionTarget();
    
    // Evolution operations
    void ApplyHydraulicErosion();
    void ApplyThermalErosion();
    void ApplyWindErosion();
    void ApplyTectonicUplift();
    void ApplySmoothing();
    void ApplyRoughening();
    void ApplyRiverCarving();
    void ApplyCoastalErosion();
    void ApplyTerracing();
    
    void UndoLastOperation();
    void SaveUndoState();

private:
    EntityID* m_SelectedEntity = nullptr;
    EntityID m_TargetTerrain = INVALID_ENTITY_ID;
    bool m_Open = false;
    
    EvolutionOperation m_CurrentOperation = EvolutionOperation::HydraulicErosion;
    
    HydraulicSettings m_Hydraulic;
    ThermalSettings m_Thermal;
    WindSettings m_Wind;
    TectonicSettings m_Tectonic;
    SmoothSettings m_Smooth;
    RoughenSettings m_Roughen;
    RiverSettings m_River;
    CoastalSettings m_Coastal;
    TerraceSettings m_Terrace;
    
    double m_LastOperationMs = 0.0;
    std::string m_StatusLine;
    
    // Undo support - store previous heightmap
    std::vector<uint16_t> m_UndoBuffer;
    bool m_HasUndo = false;
};

