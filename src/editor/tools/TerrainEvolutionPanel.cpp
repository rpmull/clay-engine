#include "editor/tools/TerrainEvolutionPanel.h"

#include "imgui.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/rendering/Terrain.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>

namespace
{
    constexpr float kFloatEpsilon = 1e-6f;
    
    const char* kOperationNames[] = {
        "Hydraulic Erosion",
        "Thermal Erosion",
        "Wind Erosion",
        "Tectonic Uplift",
        "Smoothing",
        "Roughening",
        "River Carving",
        "Coastal Erosion",
        "Terracing"
    };
    static_assert(sizeof(kOperationNames) / sizeof(kOperationNames[0]) == static_cast<int>(EvolutionOperation::Count), 
                  "Operation names must match enum");

    float Fade(float t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    uint32_t HashUInt(uint32_t x, uint32_t y, uint32_t seed)
    {
        uint32_t h = x * 374761393u + y * 668265263u + seed * 362437u;
        h = (h ^ (h >> 13u)) * 1274126177u;
        h ^= (h >> 16u);
        return h;
    }

    glm::vec2 RandomGradient(int ix, int iy, uint32_t seed)
    {
        uint32_t h = HashUInt(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), seed);
        float angle = (h & 1023u) / 1023.0f * glm::two_pi<float>();
        return glm::vec2(std::cos(angle), std::sin(angle));
    }

    float GradientNoise(const glm::vec2& p, uint32_t seed)
    {
        int x0 = static_cast<int>(std::floor(p.x));
        int y0 = static_cast<int>(std::floor(p.y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float tx = p.x - static_cast<float>(x0);
        float ty = p.y - static_cast<float>(y0);
        glm::vec2 d00(tx, ty);
        glm::vec2 d10(tx - 1.0f, ty);
        glm::vec2 d01(tx, ty - 1.0f);
        glm::vec2 d11(tx - 1.0f, ty - 1.0f);
        float v00 = glm::dot(RandomGradient(x0, y0, seed), d00);
        float v10 = glm::dot(RandomGradient(x1, y0, seed), d10);
        float v01 = glm::dot(RandomGradient(x0, y1, seed), d01);
        float v11 = glm::dot(RandomGradient(x1, y1, seed), d11);
        float u = Fade(tx);
        float v = Fade(ty);
        float nx0 = glm::mix(v00, v10, u);
        float nx1 = glm::mix(v01, v11, u);
        return glm::mix(nx0, nx1, v);
    }

    float FBM(const glm::vec2& p, int octaves, float lacunarity, float gain, uint32_t seed)
    {
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float sum = 0.0f;
        float weight = 0.0f;
        for (int i = 0; i < octaves; ++i)
        {
            float n = GradientNoise(p * frequency, seed + static_cast<uint32_t>(i) * 7919u);
            sum += n * amplitude;
            weight += amplitude;
            frequency *= lacunarity;
            amplitude *= gain;
        }
        if (weight <= kFloatEpsilon)
            return 0.5f;
        float normalized = sum / weight;
        return 0.5f + 0.5f * glm::clamp(normalized, -1.0f, 1.0f);
    }

    // Height field helper that works on terrain heightmap directly
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
        
        glm::vec2 Gradient(float fx, float fy) const
        {
            float left = Sample(fx - 1.0f, fy);
            float right = Sample(fx + 1.0f, fy);
            float down = Sample(fx, fy - 1.0f);
            float up = Sample(fx, fy + 1.0f);
            return glm::vec2((right - left) * 0.5f, (up - down) * 0.5f);
        }
    };
} // namespace

TerrainEvolutionPanel::TerrainEvolutionPanel(Scene* scene, EntityID* selectedEntity)
    : m_SelectedEntity(selectedEntity)
{
    SetContext(scene);
}

void TerrainEvolutionPanel::Open()
{
    SyncSelectionTarget();
    m_Open = true;
}

void TerrainEvolutionPanel::SyncSelectionTarget()
{
    if (m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID && *m_SelectedEntity != 0)
    {
        m_TargetTerrain = *m_SelectedEntity;
    }
}

bool TerrainEvolutionPanel::HasValidTarget() const
{
    if (!m_Context)
        return false;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return false;
    EntityData* data = m_Context->GetEntityData(m_TargetTerrain);
    return data && data->Terrain;
}

EntityData* TerrainEvolutionPanel::GetTargetEntityData() const
{
    if (!m_Context)
        return nullptr;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return nullptr;
    return m_Context->GetEntityData(m_TargetTerrain);
}

void TerrainEvolutionPanel::OnImGuiRender()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(420.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terrain Evolution", &m_Open))
    {
        ImGui::End();
        return;
    }

    DrawTargetSelector();
    ImGui::Spacing();
    DrawOperationSelector();
    ImGui::Spacing();
    
    // Draw operation-specific UI
    switch (m_CurrentOperation)
    {
    case EvolutionOperation::HydraulicErosion:
        DrawHydraulicUI();
        break;
    case EvolutionOperation::ThermalErosion:
        DrawThermalUI();
        break;
    case EvolutionOperation::WindErosion:
        DrawWindUI();
        break;
    case EvolutionOperation::TectonicUplift:
        DrawTectonicUI();
        break;
    case EvolutionOperation::Smoothing:
        DrawSmoothUI();
        break;
    case EvolutionOperation::Roughening:
        DrawRoughenUI();
        break;
    case EvolutionOperation::RiverCarving:
        DrawRiverUI();
        break;
    case EvolutionOperation::CoastalErosion:
        DrawCoastalUI();
        break;
    case EvolutionOperation::Terracing:
        DrawTerraceUI();
        break;
    default:
        break;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canApply = HasValidTarget();
    
    // Apply button
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("Apply", ImVec2(120, 28)))
    {
        SaveUndoState();
        
        switch (m_CurrentOperation)
        {
        case EvolutionOperation::HydraulicErosion:
            ApplyHydraulicErosion();
            break;
        case EvolutionOperation::ThermalErosion:
            ApplyThermalErosion();
            break;
        case EvolutionOperation::WindErosion:
            ApplyWindErosion();
            break;
        case EvolutionOperation::TectonicUplift:
            ApplyTectonicUplift();
            break;
        case EvolutionOperation::Smoothing:
            ApplySmoothing();
            break;
        case EvolutionOperation::Roughening:
            ApplyRoughening();
            break;
        case EvolutionOperation::RiverCarving:
            ApplyRiverCarving();
            break;
        case EvolutionOperation::CoastalErosion:
            ApplyCoastalErosion();
            break;
        case EvolutionOperation::Terracing:
            ApplyTerracing();
            break;
        default:
            break;
        }
    }
    ImGui::EndDisabled();
    
    ImGui::SameLine();
    
    // Undo button
    ImGui::BeginDisabled(!m_HasUndo || !canApply);
    if (ImGui::Button("Undo", ImVec2(80, 28)))
    {
        UndoLastOperation();
    }
    ImGui::EndDisabled();

    if (!canApply)
    {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "Select an entity with a Terrain component.");
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
}

void TerrainEvolutionPanel::DrawTargetSelector()
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
                m_HasUndo = false; // Clear undo when changing target
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
    }
}

void TerrainEvolutionPanel::DrawOperationSelector()
{
    ImGui::SeparatorText("Operation");
    
    int opIdx = static_cast<int>(m_CurrentOperation);
    if (ImGui::Combo("Type", &opIdx, kOperationNames, static_cast<int>(EvolutionOperation::Count)))
    {
        m_CurrentOperation = static_cast<EvolutionOperation>(opIdx);
    }
    
    // Operation description
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    switch (m_CurrentOperation)
    {
    case EvolutionOperation::HydraulicErosion:
        ImGui::TextWrapped("Simulates water droplets eroding and depositing sediment. Creates realistic valleys and gullies.");
        break;
    case EvolutionOperation::ThermalErosion:
        ImGui::TextWrapped("Simulates material sliding down steep slopes. Creates talus slopes and softens harsh edges.");
        break;
    case EvolutionOperation::WindErosion:
        ImGui::TextWrapped("Simulates wind carrying and depositing particles. Creates dunes and wind-swept features.");
        break;
    case EvolutionOperation::TectonicUplift:
        ImGui::TextWrapped("Raises or lowers regions of terrain. Can add fault lines for dramatic ridges.");
        break;
    case EvolutionOperation::Smoothing:
        ImGui::TextWrapped("Blurs the terrain to remove harsh details. Good for creating gentle rolling hills.");
        break;
    case EvolutionOperation::Roughening:
        ImGui::TextWrapped("Adds noise detail to terrain. Good for adding micro-variation.");
        break;
    case EvolutionOperation::RiverCarving:
        ImGui::TextWrapped("Analyzes water flow and carves river channels based on drainage patterns.");
        break;
    case EvolutionOperation::CoastalErosion:
        ImGui::TextWrapped("Erodes terrain near water level. Creates beaches, cliffs, and shoreline detail.");
        break;
    case EvolutionOperation::Terracing:
        ImGui::TextWrapped("Adds terrace/step effects to slopes. Creates mesa or rice-paddy like terrain.");
        break;
    default:
        break;
    }
    ImGui::PopStyleColor();
}

void TerrainEvolutionPanel::DrawHydraulicUI()
{
    ImGui::SeparatorText("Hydraulic Erosion Settings");
    
    ImGui::SliderInt("Iterations", &m_Hydraulic.Iterations, 1000, 100000);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Number of water droplets to simulate");
    
    ImGui::SliderInt("Max Steps", &m_Hydraulic.MaxSteps, 16, 128);
    ImGui::SliderFloat("Inertia", &m_Hydraulic.Inertia, 0.0f, 0.2f, "%.3f");
    ImGui::SliderFloat("Capacity", &m_Hydraulic.Capacity, 1.0f, 10.0f, "%.1f");
    ImGui::SliderFloat("Min Slope", &m_Hydraulic.MinSlope, 0.0001f, 0.01f, "%.4f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Deposit Speed", &m_Hydraulic.DepositSpeed, 0.1f, 0.5f, "%.2f");
    ImGui::SliderFloat("Erode Speed", &m_Hydraulic.ErodeSpeed, 0.1f, 0.5f, "%.2f");
    ImGui::SliderFloat("Evaporation", &m_Hydraulic.EvaporateSpeed, 0.0f, 0.1f, "%.3f");
    ImGui::SliderFloat("Gravity", &m_Hydraulic.Gravity, 1.0f, 10.0f, "%.1f");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_Hydraulic.Seed);
}

void TerrainEvolutionPanel::DrawThermalUI()
{
    ImGui::SeparatorText("Thermal Erosion Settings");
    
    ImGui::SliderInt("Iterations", &m_Thermal.Iterations, 1, 200);
    ImGui::SliderFloat("Talus Angle", &m_Thermal.TalusAngle, 15.0f, 60.0f, "%.1f°");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Angle of repose - steeper slopes will erode");
    
    ImGui::SliderFloat("Erosion Rate", &m_Thermal.ErosionRate, 0.1f, 1.0f, "%.2f");
    ImGui::SliderFloat("Min Slope", &m_Thermal.MinSlope, 0.001f, 0.1f, "%.3f");
}

void TerrainEvolutionPanel::DrawWindUI()
{
    ImGui::SeparatorText("Wind Erosion Settings");
    
    ImGui::SliderInt("Iterations", &m_Wind.Iterations, 100, 10000);
    ImGui::SliderFloat("Wind Strength", &m_Wind.WindStrength, 0.1f, 1.0f, "%.2f");
    ImGui::SliderFloat2("Wind Direction", glm::value_ptr(m_Wind.WindDirection), -1.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Suspension Height", &m_Wind.SuspensionHeight, 0.1f, 0.5f, "%.2f");
    ImGui::SliderFloat("Deposition Rate", &m_Wind.DepositionRate, 0.1f, 0.8f, "%.2f");
    ImGui::SliderFloat("Abrasion Strength", &m_Wind.AbrasionStrength, 0.0f, 0.5f, "%.2f");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_Wind.Seed);
}

void TerrainEvolutionPanel::DrawTectonicUI()
{
    ImGui::SeparatorText("Tectonic Settings");
    
    ImGui::SliderFloat("Uplift Strength", &m_Tectonic.UpliftStrength, -0.3f, 0.3f, "%.3f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Positive = raise, Negative = lower");
    
    ImGui::SliderFloat("Uplift Radius", &m_Tectonic.UpliftRadius, 0.1f, 0.8f, "%.2f");
    ImGui::SliderFloat2("Uplift Center", glm::value_ptr(m_Tectonic.UpliftCenter), 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Falloff Power", &m_Tectonic.FalloffPower, 0.5f, 4.0f, "%.1f");
    
    ImGui::Separator();
    ImGui::Checkbox("Add Fault Lines", &m_Tectonic.AddFaultLines);
    if (m_Tectonic.AddFaultLines)
    {
        ImGui::SliderInt("Fault Count", &m_Tectonic.FaultCount, 1, 10);
        ImGui::SliderFloat("Fault Displacement", &m_Tectonic.FaultDisplacement, 0.01f, 0.2f, "%.3f");
    }
}

void TerrainEvolutionPanel::DrawSmoothUI()
{
    ImGui::SeparatorText("Smoothing Settings");
    
    ImGui::SliderInt("Iterations", &m_Smooth.Iterations, 1, 20);
    ImGui::SliderInt("Kernel Radius", &m_Smooth.KernelRadius, 1, 8);
    ImGui::SliderFloat("Strength", &m_Smooth.Strength, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blend between original and smoothed");
    
    ImGui::Checkbox("Preserve Edges", &m_Smooth.PreserveEdges);
    if (m_Smooth.PreserveEdges)
    {
        ImGui::SliderFloat("Edge Threshold", &m_Smooth.EdgeThreshold, 0.01f, 0.3f, "%.3f");
    }
}

void TerrainEvolutionPanel::DrawRoughenUI()
{
    ImGui::SeparatorText("Roughening Settings");
    
    ImGui::SliderFloat("Noise Strength", &m_Roughen.NoiseStrength, 0.0f, 0.3f, "%.3f");
    ImGui::SliderFloat("Noise Frequency", &m_Roughen.NoiseFrequency, 1.0f, 20.0f, "%.1f");
    ImGui::SliderInt("Octaves", &m_Roughen.NoiseOctaves, 1, 8);
    ImGui::SliderFloat("Lacunarity", &m_Roughen.NoiseLacunarity, 1.5f, 3.0f, "%.2f");
    ImGui::SliderFloat("Gain", &m_Roughen.NoiseGain, 0.3f, 0.7f, "%.2f");
    ImGui::Checkbox("Additive", &m_Roughen.Additive);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Additive adds noise, Multiplicative modulates existing height");
    ImGui::InputScalar("Seed", ImGuiDataType_U32, &m_Roughen.Seed);
}

void TerrainEvolutionPanel::DrawRiverUI()
{
    ImGui::SeparatorText("River Carving Settings");
    
    ImGui::SliderFloat("Density", &m_River.Density, 0.05f, 0.5f, "%.2f");
    ImGui::SliderFloat("Depth", &m_River.Depth, 0.01f, 0.15f, "%.3f");
    ImGui::SliderFloat("Bank Width (m)", &m_River.BankWidth, 10.0f, 100.0f, "%.0f");
    ImGui::SliderFloat("Bank Smoothing", &m_River.BankSmoothing, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Min Flow Threshold", &m_River.MinFlowThreshold, 10.0f, 100.0f, "%.0f");
}

void TerrainEvolutionPanel::DrawCoastalUI()
{
    ImGui::SeparatorText("Coastal Erosion Settings");
    
    ImGui::SliderFloat("Sea Level", &m_Coastal.SeaLevel, 0.1f, 0.6f, "%.2f");
    ImGui::SliderFloat("Erosion Strength", &m_Coastal.ErosionStrength, 0.1f, 0.8f, "%.2f");
    ImGui::SliderFloat("Erosion Width", &m_Coastal.ErosionWidth, 0.01f, 0.15f, "%.3f");
    ImGui::SliderInt("Iterations", &m_Coastal.Iterations, 1, 50);
    ImGui::Checkbox("Create Cliffs", &m_Coastal.CreateCliffs);
    if (m_Coastal.CreateCliffs)
    {
        ImGui::SliderFloat("Cliff Height", &m_Coastal.CliffHeight, 0.02f, 0.2f, "%.3f");
    }
}

void TerrainEvolutionPanel::DrawTerraceUI()
{
    ImGui::SeparatorText("Terracing Settings");
    
    ImGui::SliderFloat("Step Height", &m_Terrace.StepHeight, 0.02f, 0.15f, "%.3f");
    ImGui::SliderFloat("Sharpness", &m_Terrace.Sharpness, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Min Height", &m_Terrace.MinHeight, 0.0f, 0.5f, "%.2f");
    ImGui::SliderFloat("Max Height", &m_Terrace.MaxHeight, 0.5f, 1.0f, "%.2f");
}

void TerrainEvolutionPanel::SaveUndoState()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    m_UndoBuffer = data->Terrain->HeightMap;
    m_HasUndo = true;
}

void TerrainEvolutionPanel::UndoLastOperation()
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

void TerrainEvolutionPanel::ApplyHydraulicErosion()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    
    std::mt19937 rng(m_Hydraulic.Seed);
    std::uniform_real_distribution<float> randPos(0.0f, static_cast<float>(res - 1));
    
    for (int drop = 0; drop < m_Hydraulic.Iterations; ++drop)
    {
        float posX = randPos(rng);
        float posY = randPos(rng);
        float dirX = 0.0f;
        float dirY = 0.0f;
        float speed = m_Hydraulic.StartSpeed;
        float water = m_Hydraulic.StartWater;
        float sediment = 0.0f;
        
        for (int step = 0; step < m_Hydraulic.MaxSteps; ++step)
        {
            float height = field.Sample(posX, posY);
            glm::vec2 gradient = field.Gradient(posX, posY);
            
            dirX = dirX * m_Hydraulic.Inertia - gradient.x * (1.0f - m_Hydraulic.Inertia);
            dirY = dirY * m_Hydraulic.Inertia - gradient.y * (1.0f - m_Hydraulic.Inertia);
            
            float dirLength = std::sqrt(dirX * dirX + dirY * dirY);
            if (dirLength > kFloatEpsilon)
            {
                dirX /= dirLength;
                dirY /= dirLength;
            }
            
            posX += dirX;
            posY += dirY;
            
            if (dirLength <= kFloatEpsilon || posX < 0.0f || posX >= res - 1 || posY < 0.0f || posY >= res - 1)
                break;
            
            float newHeight = field.Sample(posX, posY);
            float deltaH = newHeight - height;
            
            float slope = std::max(-deltaH, m_Hydraulic.MinSlope);
            float capacity = slope * speed * water * m_Hydraulic.Capacity;
            
            if (sediment > capacity || deltaH > 0.0f)
            {
                float amount = (sediment - capacity) * m_Hydraulic.DepositSpeed;
                amount = glm::max(amount, deltaH > 0.0f ? std::min(deltaH, sediment) : 0.0f);
                amount = glm::min(amount, sediment);
                if (amount > 0.0f)
                {
                    // Deposit
                    int x0 = static_cast<int>(std::floor(posX));
                    int y0 = static_cast<int>(std::floor(posY));
                    float tx = posX - x0;
                    float ty = posY - y0;
                    field.Set(x0, y0, field.Get(x0, y0) + amount * (1-tx) * (1-ty));
                    field.Set(x0+1, y0, field.Get(x0+1, y0) + amount * tx * (1-ty));
                    field.Set(x0, y0+1, field.Get(x0, y0+1) + amount * (1-tx) * ty);
                    field.Set(x0+1, y0+1, field.Get(x0+1, y0+1) + amount * tx * ty);
                    sediment -= amount;
                }
            }
            else
            {
                float amount = (capacity - sediment) * m_Hydraulic.ErodeSpeed;
                if (amount > 0.0f)
                {
                    // Erode
                    int x0 = static_cast<int>(std::floor(posX));
                    int y0 = static_cast<int>(std::floor(posY));
                    float tx = posX - x0;
                    float ty = posY - y0;
                    float w00 = (1-tx) * (1-ty);
                    float w10 = tx * (1-ty);
                    float w01 = (1-tx) * ty;
                    float w11 = tx * ty;
                    float eroded = 0.0f;
                    auto erodeAt = [&](int x, int y, float w) {
                        if (w <= 0) return;
                        float take = std::min(field.Get(x, y), amount * w);
                        field.Set(x, y, field.Get(x, y) - take);
                        eroded += take;
                    };
                    erodeAt(x0, y0, w00);
                    erodeAt(x0+1, y0, w10);
                    erodeAt(x0, y0+1, w01);
                    erodeAt(x0+1, y0+1, w11);
                    sediment += eroded;
                }
            }
            
            speed = std::sqrt(std::max(0.0f, speed * speed + deltaH * m_Hydraulic.Gravity));
            water *= (1.0f - m_Hydraulic.EvaporateSpeed);
            if (water < 0.05f)
                break;
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream oss;
    oss << "Applied hydraulic erosion (" << m_Hydraulic.Iterations << " droplets).";
    m_StatusLine = oss.str();
}

void TerrainEvolutionPanel::ApplyThermalErosion()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    float talusSlope = std::tan(glm::radians(m_Thermal.TalusAngle));
    
    const int offsets[8][2] = {
        {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,-1}, {1,-1}, {-1,1}
    };
    
    for (int iter = 0; iter < m_Thermal.Iterations; ++iter)
    {
        for (int y = 1; y < res - 1; ++y)
        {
            for (int x = 1; x < res - 1; ++x)
            {
                float h = field.Get(x, y);
                float totalDiff = 0.0f;
                float maxDiff = 0.0f;
                
                for (const auto& off : offsets)
                {
                    float nh = field.Get(x + off[0], y + off[1]);
                    float diff = h - nh;
                    if (diff > m_Thermal.MinSlope && diff > talusSlope)
                    {
                        totalDiff += diff - talusSlope;
                        maxDiff = std::max(maxDiff, diff);
                    }
                }
                
                if (totalDiff > kFloatEpsilon)
                {
                    float amount = (maxDiff - talusSlope) * 0.5f * m_Thermal.ErosionRate;
                    field.Set(x, y, h - amount);
                    
                    // Distribute to lower neighbors
                    for (const auto& off : offsets)
                    {
                        float nh = field.Get(x + off[0], y + off[1]);
                        float diff = h - nh;
                        if (diff > m_Thermal.MinSlope && diff > talusSlope)
                        {
                            float weight = (diff - talusSlope) / totalDiff;
                            field.Set(x + off[0], y + off[1], nh + amount * weight);
                        }
                    }
                }
            }
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied thermal erosion.";
}

void TerrainEvolutionPanel::ApplyWindErosion()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    glm::vec2 windDir = glm::normalize(m_Wind.WindDirection);
    if (glm::length(m_Wind.WindDirection) < kFloatEpsilon)
        windDir = glm::vec2(1.0f, 0.0f);
    
    std::mt19937 rng(m_Wind.Seed);
    std::uniform_real_distribution<float> randPos(0.0f, static_cast<float>(res - 1));
    std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
    
    for (int iter = 0; iter < m_Wind.Iterations; ++iter)
    {
        // Start particle on upwind edge
        float posX, posY;
        if (std::abs(windDir.x) > std::abs(windDir.y))
        {
            posX = windDir.x > 0 ? 0.0f : static_cast<float>(res - 1);
            posY = randPos(rng);
        }
        else
        {
            posX = randPos(rng);
            posY = windDir.y > 0 ? 0.0f : static_cast<float>(res - 1);
        }
        
        float sediment = 0.0f;
        
        for (int step = 0; step < res * 2; ++step)
        {
            if (posX < 0 || posX >= res - 1 || posY < 0 || posY >= res - 1)
                break;
            
            float height = field.Sample(posX, posY);
            glm::vec2 gradient = field.Gradient(posX, posY);
            
            // Wind-shadow effect - deposit in lee of slopes
            float windward = glm::dot(gradient, windDir);
            
            if (windward < 0 && sediment > 0)
            {
                // In wind shadow - deposit
                float deposit = sediment * m_Wind.DepositionRate * (-windward);
                int x = static_cast<int>(posX);
                int y = static_cast<int>(posY);
                field.Set(x, y, field.Get(x, y) + deposit);
                sediment -= deposit;
            }
            else if (windward > 0.01f)
            {
                // Windward slope - erode
                float erode = m_Wind.AbrasionStrength * windward * m_Wind.WindStrength;
                int x = static_cast<int>(posX);
                int y = static_cast<int>(posY);
                float take = std::min(field.Get(x, y), erode);
                field.Set(x, y, field.Get(x, y) - take);
                sediment += take;
            }
            
            // Move with wind (with some randomness)
            posX += windDir.x * 2.0f + (rand01(rng) - 0.5f) * 0.5f;
            posY += windDir.y * 2.0f + (rand01(rng) - 0.5f) * 0.5f;
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied wind erosion.";
}

void TerrainEvolutionPanel::ApplyTectonicUplift()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    float invRes = 1.0f / static_cast<float>(res - 1);
    
    // Apply radial uplift/subsidence
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            glm::vec2 uv(x * invRes, y * invRes);
            float dist = glm::length(uv - m_Tectonic.UpliftCenter);
            float falloff = 1.0f - glm::smoothstep(0.0f, m_Tectonic.UpliftRadius, dist);
            falloff = std::pow(falloff, m_Tectonic.FalloffPower);
            
            float h = field.Get(x, y);
            h += m_Tectonic.UpliftStrength * falloff;
            field.Set(x, y, glm::clamp(h, 0.0f, 1.0f));
        }
    }
    
    // Add fault lines if enabled
    if (m_Tectonic.AddFaultLines)
    {
        std::mt19937 rng(42u);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        
        for (int f = 0; f < m_Tectonic.FaultCount; ++f)
        {
            glm::vec2 normal(rand01(rng) - 0.5f, rand01(rng) - 0.5f);
            if (glm::length(normal) < kFloatEpsilon) continue;
            normal = glm::normalize(normal);
            glm::vec2 point(rand01(rng), rand01(rng));
            
            for (int y = 0; y < res; ++y)
            {
                for (int x = 0; x < res; ++x)
                {
                    glm::vec2 uv(x * invRes, y * invRes);
                    float d = glm::dot(uv - point, normal);
                    float delta = m_Tectonic.FaultDisplacement * std::tanh(d * 10.0f);
                    
                    float h = field.Get(x, y);
                    field.Set(x, y, glm::clamp(h + delta, 0.0f, 1.0f));
                }
            }
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied tectonic uplift.";
}

void TerrainEvolutionPanel::ApplySmoothing()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    std::vector<uint16_t> temp = terrain.HeightMap;
    TerrainHeightField field(terrain.HeightMap, res);
    TerrainHeightField original(temp, res);
    
    int radius = m_Smooth.KernelRadius;
    
    for (int iter = 0; iter < m_Smooth.Iterations; ++iter)
    {
        temp = terrain.HeightMap;
        TerrainHeightField source(temp, res);
        
        for (int y = radius; y < res - radius; ++y)
        {
            for (int x = radius; x < res - radius; ++x)
            {
                float sum = 0.0f;
                int count = 0;
                
                for (int dy = -radius; dy <= radius; ++dy)
                {
                    for (int dx = -radius; dx <= radius; ++dx)
                    {
                        sum += source.Get(x + dx, y + dy);
                        ++count;
                    }
                }
                
                float smoothed = sum / static_cast<float>(count);
                float originalVal = source.Get(x, y);
                
                if (m_Smooth.PreserveEdges)
                {
                    // Check if this is an edge (high gradient)
                    glm::vec2 grad = source.Gradient(static_cast<float>(x), static_cast<float>(y));
                    float gradMag = glm::length(grad);
                    float edgeFactor = glm::smoothstep(0.0f, m_Smooth.EdgeThreshold, gradMag);
                    smoothed = glm::mix(smoothed, originalVal, edgeFactor);
                }
                
                field.Set(x, y, glm::mix(originalVal, smoothed, m_Smooth.Strength));
            }
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied smoothing.";
}

void TerrainEvolutionPanel::ApplyRoughening()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    float invRes = 1.0f / static_cast<float>(res - 1);
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            glm::vec2 uv(x * invRes, y * invRes);
            float noise = FBM(uv * m_Roughen.NoiseFrequency, 
                             m_Roughen.NoiseOctaves,
                             m_Roughen.NoiseLacunarity,
                             m_Roughen.NoiseGain,
                             m_Roughen.Seed);
            
            float h = field.Get(x, y);
            
            if (m_Roughen.Additive)
            {
                h += (noise - 0.5f) * 2.0f * m_Roughen.NoiseStrength;
            }
            else
            {
                h *= (1.0f + (noise - 0.5f) * m_Roughen.NoiseStrength);
            }
            
            field.Set(x, y, glm::clamp(h, 0.0f, 1.0f));
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied roughening.";
}

void TerrainEvolutionPanel::ApplyRiverCarving()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    const size_t count = static_cast<size_t>(res) * res;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    
    // Compute flow accumulation
    std::vector<float> flow(count, 1.0f);
    std::vector<int> receiver(count, -1);
    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    
    // Sort by height (highest first)
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return field.Get(a % res, a / res) > field.Get(b % res, b / res);
    });
    
    const int offsets[8][2] = {
        {1,0}, {-1,0}, {0,1}, {0,-1}, {1,1}, {-1,-1}, {1,-1}, {-1,1}
    };
    
    // Find flow direction
    for (size_t idx : order)
    {
        int x = static_cast<int>(idx % res);
        int y = static_cast<int>(idx / res);
        float h = field.Get(x, y);
        
        float bestDrop = 0.0f;
        int bestIdx = -1;
        
        for (const auto& off : offsets)
        {
            int nx = x + off[0];
            int ny = y + off[1];
            if (nx < 0 || ny < 0 || nx >= res || ny >= res) continue;
            
            size_t nIdx = static_cast<size_t>(ny) * res + nx;
            float drop = h - field.Get(nx, ny);
            if (drop > bestDrop)
            {
                bestDrop = drop;
                bestIdx = static_cast<int>(nIdx);
            }
        }
        receiver[idx] = bestIdx;
    }
    
    // Accumulate flow
    for (size_t idx : order)
    {
        int downstream = receiver[idx];
        if (downstream >= 0)
        {
            flow[downstream] += flow[idx];
        }
    }
    
    // Find threshold for rivers
    std::vector<float> sortedFlow = flow;
    std::sort(sortedFlow.begin(), sortedFlow.end());
    size_t threshIdx = static_cast<size_t>((1.0f - m_River.Density) * (count - 1));
    float threshold = std::max(m_River.MinFlowThreshold, sortedFlow[threshIdx]);
    
    // Carve rivers
    for (size_t i = 0; i < count; ++i)
    {
        if (flow[i] > threshold)
        {
            int x = static_cast<int>(i % res);
            int y = static_cast<int>(i / res);
            float riverStrength = glm::clamp((flow[i] - threshold) / threshold, 0.0f, 1.0f);
            float h = field.Get(x, y);
            field.Set(x, y, glm::clamp(h - riverStrength * m_River.Depth, 0.0f, 1.0f));
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied river carving.";
}

void TerrainEvolutionPanel::ApplyCoastalErosion()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    
    for (int iter = 0; iter < m_Coastal.Iterations; ++iter)
    {
        std::vector<uint16_t> temp = terrain.HeightMap;
        TerrainHeightField source(temp, res);
        
        for (int y = 1; y < res - 1; ++y)
        {
            for (int x = 1; x < res - 1; ++x)
            {
                float h = source.Get(x, y);
                
                // Only affect terrain near sea level
                float distFromSea = std::abs(h - m_Coastal.SeaLevel);
                if (distFromSea > m_Coastal.ErosionWidth) continue;
                
                float erosionFactor = 1.0f - distFromSea / m_Coastal.ErosionWidth;
                erosionFactor *= m_Coastal.ErosionStrength;
                
                // Check if we're on land above sea level
                if (h > m_Coastal.SeaLevel)
                {
                    // Check for nearby water
                    bool nearWater = false;
                    for (int dy = -2; dy <= 2; ++dy)
                    {
                        for (int dx = -2; dx <= 2; ++dx)
                        {
                            if (source.Get(x + dx, y + dy) < m_Coastal.SeaLevel)
                            {
                                nearWater = true;
                                break;
                            }
                        }
                        if (nearWater) break;
                    }
                    
                    if (nearWater)
                    {
                        if (m_Coastal.CreateCliffs)
                        {
                            // Create cliff - sharp drop then flat beach
                            if (h > m_Coastal.SeaLevel + m_Coastal.CliffHeight)
                            {
                                // Above cliff - minor erosion
                                h -= erosionFactor * 0.1f;
                            }
                            else
                            {
                                // Cliff face - strong erosion toward sea level
                                h = glm::mix(h, m_Coastal.SeaLevel + 0.01f, erosionFactor * 0.3f);
                            }
                        }
                        else
                        {
                            // Gradual beach
                            h = glm::mix(h, m_Coastal.SeaLevel + 0.02f, erosionFactor * 0.2f);
                        }
                    }
                }
                
                field.Set(x, y, glm::clamp(h, 0.0f, 1.0f));
            }
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied coastal erosion.";
}

void TerrainEvolutionPanel::ApplyTerracing()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    
    auto start = std::chrono::steady_clock::now();
    
    TerrainHeightField field(terrain.HeightMap, res);
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            float h = field.Get(x, y);
            
            // Only terrace within height range
            if (h < m_Terrace.MinHeight || h > m_Terrace.MaxHeight)
                continue;
            
            // Quantize to steps
            float step = m_Terrace.StepHeight;
            float terraced = std::round(h / step) * step;
            
            // Blend based on sharpness
            float blend = m_Terrace.Sharpness;
            field.Set(x, y, glm::mix(h, terraced, blend));
        }
    }
    
    terrain.MarkDataDirty();
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    m_StatusLine = "Applied terracing.";
}

