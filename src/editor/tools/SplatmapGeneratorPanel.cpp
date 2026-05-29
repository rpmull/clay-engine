#include "editor/tools/SplatmapGeneratorPanel.h"

#include "imgui.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/rendering/Terrain.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace
{
    constexpr float kFloatEpsilon = 1e-6f;
    
    const char* kPresetNames[] = {
        "Custom",
        "Standard",
        "Desert",
        "Alpine",
        "Tropical",
        "Volcanic",
        "Grassland"
    };
    static_assert(sizeof(kPresetNames) / sizeof(kPresetNames[0]) == static_cast<int>(SplatPreset::Count),
                  "Preset names must match enum");
    
    const char* kLayerChannelNames[] = { 
        "R (Layer 0)", "G (Layer 1)", "B (Layer 2)", "A (Layer 3)",
        "R2 (Layer 4)", "G2 (Layer 5)", "B2 (Layer 6)", "A2 (Layer 7)"
    };
    
    // Noise helpers
    uint32_t HashUInt(uint32_t x, uint32_t y, uint32_t seed)
    {
        uint32_t h = x * 374761393u + y * 668265263u + seed * 362437u;
        h = (h ^ (h >> 13u)) * 1274126177u;
        h ^= (h >> 16u);
        return h;
    }

    float Fade(float t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
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

    float FBM(const glm::vec2& p, int octaves, uint32_t seed)
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
            frequency *= 2.0f;
            amplitude *= 0.5f;
        }
        return weight > kFloatEpsilon ? (0.5f + 0.5f * (sum / weight)) : 0.5f;
    }
    
    ImU32 GetLayerColor(int index)
    {
        switch (index)
        {
        case 0: return IM_COL32(255, 100, 100, 255);  // Red
        case 1: return IM_COL32(100, 255, 100, 255);  // Green
        case 2: return IM_COL32(100, 100, 255, 255);  // Blue
        case 3: return IM_COL32(255, 255, 100, 255);  // Yellow (Alpha)
        default: return IM_COL32(200, 200, 200, 255);
        }
    }

    uint8_t SplatWeightToByte(float value)
    {
        return static_cast<uint8_t>(glm::clamp(value, 0.0f, 1.0f) * 255.0f);
    }

    float ReadSplatLayerWeight(const glm::u8vec4& splat0, const glm::u8vec4& splat1, int layer)
    {
        if (layer < 4)
        {
            return static_cast<float>(splat0[layer]) / 255.0f;
        }
        return static_cast<float>(splat1[layer - 4]) / 255.0f;
    }

    void WriteSplatLayerWeight(glm::u8vec4& splat0, glm::u8vec4& splat1, int layer, float value)
    {
        if (layer < 4)
        {
            splat0[layer] = SplatWeightToByte(value);
        }
        else
        {
            splat1[layer - 4] = SplatWeightToByte(value);
        }
    }
} // namespace

SplatmapGeneratorPanel::SplatmapGeneratorPanel(Scene* scene, EntityID* selectedEntity)
    : m_SelectedEntity(selectedEntity)
{
    SetContext(scene);
    ApplyPreset(SplatPreset::Standard);
}

void SplatmapGeneratorPanel::Open()
{
    SyncSelectionTarget();
    m_Open = true;
}

void SplatmapGeneratorPanel::SyncSelectionTarget()
{
    if (m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID && *m_SelectedEntity != 0)
    {
        m_TargetTerrain = *m_SelectedEntity;
    }
}

bool SplatmapGeneratorPanel::HasValidTarget() const
{
    if (!m_Context)
        return false;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return false;
    EntityData* data = m_Context->GetEntityData(m_TargetTerrain);
    return data && data->Terrain;
}

EntityData* SplatmapGeneratorPanel::GetTargetEntityData() const
{
    if (!m_Context)
        return nullptr;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return nullptr;
    return m_Context->GetEntityData(m_TargetTerrain);
}

void SplatmapGeneratorPanel::OnImGuiRender()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(450.0f, 600.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Splatmap Generator", &m_Open))
    {
        ImGui::End();
        return;
    }

    DrawTargetSelector();
    ImGui::Spacing();
    DrawPresetSelector();
    ImGui::Spacing();
    DrawLayerRules();
    
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canApply = HasValidTarget();
    
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("Generate Splatmap", ImVec2(150, 28)))
    {
        SaveUndoState();
        GenerateSplatmap();
    }
    ImGui::EndDisabled();
    
    ImGui::SameLine();
    
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

void SplatmapGeneratorPanel::DrawTargetSelector()
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

void SplatmapGeneratorPanel::DrawPresetSelector()
{
    ImGui::SeparatorText("Preset");
    
    int presetIdx = static_cast<int>(m_CurrentPreset);
    if (ImGui::Combo("Distribution Preset", &presetIdx, kPresetNames, static_cast<int>(SplatPreset::Count)))
    {
        m_CurrentPreset = static_cast<SplatPreset>(presetIdx);
        if (m_CurrentPreset != SplatPreset::Custom)
        {
            ApplyPreset(m_CurrentPreset);
        }
    }
    
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    switch (m_CurrentPreset)
    {
    case SplatPreset::Standard:
        ImGui::TextWrapped("Grass on flat areas, rock on cliffs, sand near water, snow at peaks.");
        break;
    case SplatPreset::Desert:
        ImGui::TextWrapped("Sand on flat areas, rock on slopes, gravel transitions.");
        break;
    case SplatPreset::Alpine:
        ImGui::TextWrapped("Grass at low elevations, rock mid-range, snow and ice at peaks.");
        break;
    case SplatPreset::Tropical:
        ImGui::TextWrapped("Jungle grass, mud in valleys, sand at beaches, rock on cliffs.");
        break;
    case SplatPreset::Volcanic:
        ImGui::TextWrapped("Ash on flat areas, lava rock on slopes, obsidian at peaks.");
        break;
    case SplatPreset::Grassland:
        ImGui::TextWrapped("Multiple grass variations based on height and moisture.");
        break;
    case SplatPreset::Custom:
        ImGui::TextWrapped("Manually configured layer rules.");
        break;
    default:
        break;
    }
    ImGui::PopStyleColor();
}

void SplatmapGeneratorPanel::DrawLayerRules()
{
    ImGui::SeparatorText("Layer Rules");
    
    if (ImGui::Button("Add Rule"))
    {
        SplatLayerRule rule;
        rule.LayerName = "Layer " + std::to_string(m_Rules.size());
        rule.LayerIndex = static_cast<int>(m_Rules.size()) % 4;
        rule.Priority = static_cast<int>(m_Rules.size());
        m_Rules.push_back(rule);
        m_CurrentPreset = SplatPreset::Custom;
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Clear All"))
    {
        m_Rules.clear();
        m_CurrentPreset = SplatPreset::Custom;
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Sort by Priority"))
    {
        std::sort(m_Rules.begin(), m_Rules.end(), [](const SplatLayerRule& a, const SplatLayerRule& b) {
            return a.Priority < b.Priority;
        });
    }
    
    // Draw each rule
    int deleteIdx = -1;
    for (size_t i = 0; i < m_Rules.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        
        bool ruleOpen = ImGui::CollapsingHeader(m_Rules[i].LayerName.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        
        // Show layer color indicator
        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        drawList->AddRectFilled(p, ImVec2(p.x + 16, p.y + 16), GetLayerColor(m_Rules[i].LayerIndex));
        ImGui::Dummy(ImVec2(20, 16));
        
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
        {
            deleteIdx = static_cast<int>(i);
        }
        
        if (ruleOpen)
        {
            DrawLayerRuleEditor(m_Rules[i], static_cast<int>(i));
        }
        
        ImGui::PopID();
    }
    
    if (deleteIdx >= 0)
    {
        m_Rules.erase(m_Rules.begin() + deleteIdx);
        m_CurrentPreset = SplatPreset::Custom;
    }
}

void SplatmapGeneratorPanel::DrawLayerRuleEditor(SplatLayerRule& rule, int index)
{
    ImGui::Indent();
    
    // Basic settings
    char nameBuf[64];
    strncpy(nameBuf, rule.LayerName.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
    {
        rule.LayerName = nameBuf;
        m_CurrentPreset = SplatPreset::Custom;
    }
    
    if (ImGui::Combo("Channel", &rule.LayerIndex, kLayerChannelNames, kMaxTerrainLayers))
    {
        m_CurrentPreset = SplatPreset::Custom;
    }
    
    ImGui::SliderInt("Priority", &rule.Priority, 0, 10);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Higher priority layers paint over lower priority");
    
    ImGui::Checkbox("Enabled", &rule.Enabled);
    
    ImGui::Spacing();
    
    // Height-based
    if (ImGui::Checkbox("Use Height", &rule.UseHeight))
        m_CurrentPreset = SplatPreset::Custom;
    
    if (rule.UseHeight)
    {
        ImGui::Indent();
        ImGui::SliderFloat("Height Min", &rule.HeightMin, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Height Max", &rule.HeightMax, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Height Falloff", &rule.HeightFalloff, 0.0f, 0.3f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Soft transition width at height boundaries");
        ImGui::Unindent();
    }
    
    // Slope-based
    if (ImGui::Checkbox("Use Slope", &rule.UseSlope))
        m_CurrentPreset = SplatPreset::Custom;
    
    if (rule.UseSlope)
    {
        ImGui::Indent();
        ImGui::SliderFloat("Slope Min", &rule.SlopeMin, 0.0f, 90.0f, "%.1f°");
        ImGui::SliderFloat("Slope Max", &rule.SlopeMax, 0.0f, 90.0f, "%.1f°");
        ImGui::SliderFloat("Slope Falloff", &rule.SlopeFalloff, 0.0f, 20.0f, "%.1f°");
        ImGui::Unindent();
    }
    
    // Curvature-based
    if (ImGui::Checkbox("Use Curvature", &rule.UseCurvature))
        m_CurrentPreset = SplatPreset::Custom;
    
    if (rule.UseCurvature)
    {
        ImGui::Indent();
        ImGui::SliderFloat("Curvature Min", &rule.CurvatureMin, -1.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Curvature Max", &rule.CurvatureMax, -1.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("-1 = concave (valleys), +1 = convex (ridges)");
        ImGui::Unindent();
    }
    
    // Water distance
    if (ImGui::Checkbox("Use Water Distance", &rule.UseWaterDistance))
        m_CurrentPreset = SplatPreset::Custom;
    
    if (rule.UseWaterDistance)
    {
        ImGui::Indent();
        ImGui::SliderFloat("Water Level", &rule.WaterLevel, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Distance Min", &rule.WaterDistanceMin, 0.0f, 0.3f, "%.3f");
        ImGui::SliderFloat("Distance Max", &rule.WaterDistanceMax, 0.0f, 0.3f, "%.3f");
        ImGui::Unindent();
    }
    
    // Noise modulation
    if (ImGui::Checkbox("Use Noise", &rule.UseNoise))
        m_CurrentPreset = SplatPreset::Custom;
    
    if (rule.UseNoise)
    {
        ImGui::Indent();
        ImGui::SliderFloat("Noise Strength", &rule.NoiseStrength, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Noise Frequency", &rule.NoiseFrequency, 0.5f, 20.0f, "%.1f");
        ImGui::InputScalar("Noise Seed", ImGuiDataType_U32, &rule.NoiseSeed);
        ImGui::Unindent();
    }
    
    ImGui::Unindent();
    ImGui::Spacing();
}

void SplatmapGeneratorPanel::ApplyPreset(SplatPreset preset)
{
    m_Rules.clear();
    
    switch (preset)
    {
    case SplatPreset::Standard:
    {
        // Layer 0 (R): Grass - flat low/mid areas
        SplatLayerRule grass;
        grass.LayerName = "Grass";
        grass.LayerIndex = 0;
        grass.UseHeight = true;
        grass.HeightMin = 0.2f;
        grass.HeightMax = 0.7f;
        grass.HeightFalloff = 0.1f;
        grass.UseSlope = true;
        grass.SlopeMin = 0.0f;
        grass.SlopeMax = 30.0f;
        grass.SlopeFalloff = 10.0f;
        grass.Priority = 1;
        m_Rules.push_back(grass);
        
        // Layer 1 (G): Rock - steep slopes
        SplatLayerRule rock;
        rock.LayerName = "Rock/Cliff";
        rock.LayerIndex = 1;
        rock.UseSlope = true;
        rock.SlopeMin = 25.0f;
        rock.SlopeMax = 90.0f;
        rock.SlopeFalloff = 8.0f;
        rock.Priority = 3;
        m_Rules.push_back(rock);
        
        // Layer 2 (B): Sand - near water
        SplatLayerRule sand;
        sand.LayerName = "Sand/Beach";
        sand.LayerIndex = 2;
        sand.UseHeight = true;
        sand.HeightMin = 0.0f;
        sand.HeightMax = 0.25f;
        sand.HeightFalloff = 0.05f;
        sand.UseSlope = true;
        sand.SlopeMin = 0.0f;
        sand.SlopeMax = 20.0f;
        sand.SlopeFalloff = 5.0f;
        sand.Priority = 2;
        m_Rules.push_back(sand);
        
        // Layer 3 (A): Snow - high elevations
        SplatLayerRule snow;
        snow.LayerName = "Snow";
        snow.LayerIndex = 3;
        snow.UseHeight = true;
        snow.HeightMin = 0.7f;
        snow.HeightMax = 1.0f;
        snow.HeightFalloff = 0.08f;
        snow.UseNoise = true;
        snow.NoiseStrength = 0.15f;
        snow.NoiseFrequency = 6.0f;
        snow.Priority = 4;
        m_Rules.push_back(snow);
        break;
    }
    
    case SplatPreset::Desert:
    {
        SplatLayerRule sand;
        sand.LayerName = "Sand";
        sand.LayerIndex = 0;
        sand.UseSlope = true;
        sand.SlopeMin = 0.0f;
        sand.SlopeMax = 25.0f;
        sand.SlopeFalloff = 8.0f;
        sand.Priority = 1;
        m_Rules.push_back(sand);
        
        SplatLayerRule rock;
        rock.LayerName = "Desert Rock";
        rock.LayerIndex = 1;
        rock.UseSlope = true;
        rock.SlopeMin = 20.0f;
        rock.SlopeMax = 90.0f;
        rock.SlopeFalloff = 10.0f;
        rock.Priority = 2;
        m_Rules.push_back(rock);
        
        SplatLayerRule gravel;
        gravel.LayerName = "Gravel";
        gravel.LayerIndex = 2;
        gravel.UseSlope = true;
        gravel.SlopeMin = 10.0f;
        gravel.SlopeMax = 35.0f;
        gravel.SlopeFalloff = 8.0f;
        gravel.UseNoise = true;
        gravel.NoiseStrength = 0.4f;
        gravel.NoiseFrequency = 8.0f;
        gravel.Priority = 1;
        m_Rules.push_back(gravel);
        break;
    }
    
    case SplatPreset::Alpine:
    {
        SplatLayerRule grass;
        grass.LayerName = "Alpine Grass";
        grass.LayerIndex = 0;
        grass.UseHeight = true;
        grass.HeightMin = 0.15f;
        grass.HeightMax = 0.5f;
        grass.HeightFalloff = 0.1f;
        grass.UseSlope = true;
        grass.SlopeMin = 0.0f;
        grass.SlopeMax = 35.0f;
        grass.SlopeFalloff = 10.0f;
        grass.Priority = 1;
        m_Rules.push_back(grass);
        
        SplatLayerRule rock;
        rock.LayerName = "Mountain Rock";
        rock.LayerIndex = 1;
        rock.UseHeight = true;
        rock.HeightMin = 0.4f;
        rock.HeightMax = 0.85f;
        rock.HeightFalloff = 0.1f;
        rock.UseSlope = true;
        rock.SlopeMin = 20.0f;
        rock.SlopeMax = 90.0f;
        rock.SlopeFalloff = 8.0f;
        rock.Priority = 2;
        m_Rules.push_back(rock);
        
        SplatLayerRule snow;
        snow.LayerName = "Snow";
        snow.LayerIndex = 2;
        snow.UseHeight = true;
        snow.HeightMin = 0.75f;
        snow.HeightMax = 1.0f;
        snow.HeightFalloff = 0.1f;
        snow.UseSlope = true;
        snow.SlopeMin = 0.0f;
        snow.SlopeMax = 50.0f;
        snow.SlopeFalloff = 15.0f;
        snow.Priority = 3;
        m_Rules.push_back(snow);
        
        SplatLayerRule ice;
        ice.LayerName = "Ice";
        ice.LayerIndex = 3;
        ice.UseHeight = true;
        ice.HeightMin = 0.85f;
        ice.HeightMax = 1.0f;
        ice.HeightFalloff = 0.05f;
        ice.UseSlope = true;
        ice.SlopeMin = 30.0f;
        ice.SlopeMax = 90.0f;
        ice.SlopeFalloff = 10.0f;
        ice.Priority = 4;
        m_Rules.push_back(ice);
        break;
    }
    
    case SplatPreset::Tropical:
    {
        SplatLayerRule jungle;
        jungle.LayerName = "Jungle Grass";
        jungle.LayerIndex = 0;
        jungle.UseHeight = true;
        jungle.HeightMin = 0.2f;
        jungle.HeightMax = 0.6f;
        jungle.HeightFalloff = 0.1f;
        jungle.UseSlope = true;
        jungle.SlopeMin = 0.0f;
        jungle.SlopeMax = 40.0f;
        jungle.SlopeFalloff = 12.0f;
        jungle.Priority = 1;
        m_Rules.push_back(jungle);
        
        SplatLayerRule mud;
        mud.LayerName = "Mud";
        mud.LayerIndex = 1;
        mud.UseHeight = true;
        mud.HeightMin = 0.15f;
        mud.HeightMax = 0.35f;
        mud.HeightFalloff = 0.08f;
        mud.UseCurvature = true;
        mud.CurvatureMin = -1.0f;
        mud.CurvatureMax = 0.0f;
        mud.Priority = 2;
        m_Rules.push_back(mud);
        
        SplatLayerRule sand;
        sand.LayerName = "Beach Sand";
        sand.LayerIndex = 2;
        sand.UseHeight = true;
        sand.HeightMin = 0.0f;
        sand.HeightMax = 0.22f;
        sand.HeightFalloff = 0.05f;
        sand.UseSlope = true;
        sand.SlopeMin = 0.0f;
        sand.SlopeMax = 15.0f;
        sand.SlopeFalloff = 5.0f;
        sand.Priority = 3;
        m_Rules.push_back(sand);
        
        SplatLayerRule rock;
        rock.LayerName = "Tropical Rock";
        rock.LayerIndex = 3;
        rock.UseSlope = true;
        rock.SlopeMin = 35.0f;
        rock.SlopeMax = 90.0f;
        rock.SlopeFalloff = 10.0f;
        rock.Priority = 4;
        m_Rules.push_back(rock);
        break;
    }
    
    case SplatPreset::Volcanic:
    {
        SplatLayerRule ash;
        ash.LayerName = "Volcanic Ash";
        ash.LayerIndex = 0;
        ash.UseSlope = true;
        ash.SlopeMin = 0.0f;
        ash.SlopeMax = 30.0f;
        ash.SlopeFalloff = 10.0f;
        ash.UseNoise = true;
        ash.NoiseStrength = 0.3f;
        ash.NoiseFrequency = 5.0f;
        ash.Priority = 1;
        m_Rules.push_back(ash);
        
        SplatLayerRule lavaRock;
        lavaRock.LayerName = "Lava Rock";
        lavaRock.LayerIndex = 1;
        lavaRock.UseSlope = true;
        lavaRock.SlopeMin = 20.0f;
        lavaRock.SlopeMax = 90.0f;
        lavaRock.SlopeFalloff = 10.0f;
        lavaRock.Priority = 2;
        m_Rules.push_back(lavaRock);
        
        SplatLayerRule obsidian;
        obsidian.LayerName = "Obsidian";
        obsidian.LayerIndex = 2;
        obsidian.UseHeight = true;
        obsidian.HeightMin = 0.7f;
        obsidian.HeightMax = 1.0f;
        obsidian.HeightFalloff = 0.1f;
        obsidian.UseNoise = true;
        obsidian.NoiseStrength = 0.4f;
        obsidian.NoiseFrequency = 8.0f;
        obsidian.Priority = 3;
        m_Rules.push_back(obsidian);
        break;
    }
    
    case SplatPreset::Grassland:
    {
        SplatLayerRule shortGrass;
        shortGrass.LayerName = "Short Grass";
        shortGrass.LayerIndex = 0;
        shortGrass.UseHeight = true;
        shortGrass.HeightMin = 0.35f;
        shortGrass.HeightMax = 0.6f;
        shortGrass.HeightFalloff = 0.1f;
        shortGrass.UseSlope = true;
        shortGrass.SlopeMin = 0.0f;
        shortGrass.SlopeMax = 25.0f;
        shortGrass.SlopeFalloff = 8.0f;
        shortGrass.Priority = 1;
        m_Rules.push_back(shortGrass);
        
        SplatLayerRule tallGrass;
        tallGrass.LayerName = "Tall Grass";
        tallGrass.LayerIndex = 1;
        tallGrass.UseHeight = true;
        tallGrass.HeightMin = 0.25f;
        tallGrass.HeightMax = 0.45f;
        tallGrass.HeightFalloff = 0.08f;
        tallGrass.UseCurvature = true;
        tallGrass.CurvatureMin = -1.0f;
        tallGrass.CurvatureMax = 0.2f;
        tallGrass.Priority = 2;
        m_Rules.push_back(tallGrass);
        
        SplatLayerRule dirt;
        dirt.LayerName = "Dirt Path";
        dirt.LayerIndex = 2;
        dirt.UseSlope = true;
        dirt.SlopeMin = 15.0f;
        dirt.SlopeMax = 45.0f;
        dirt.SlopeFalloff = 10.0f;
        dirt.UseNoise = true;
        dirt.NoiseStrength = 0.5f;
        dirt.NoiseFrequency = 6.0f;
        dirt.Priority = 1;
        m_Rules.push_back(dirt);
        
        SplatLayerRule flowers;
        flowers.LayerName = "Wildflowers";
        flowers.LayerIndex = 3;
        flowers.UseHeight = true;
        flowers.HeightMin = 0.3f;
        flowers.HeightMax = 0.55f;
        flowers.HeightFalloff = 0.1f;
        flowers.UseNoise = true;
        flowers.NoiseStrength = 0.7f;
        flowers.NoiseFrequency = 10.0f;
        flowers.Priority = 3;
        m_Rules.push_back(flowers);
        break;
    }
    
    default:
        break;
    }
    
    m_CurrentPreset = preset;
}

void SplatmapGeneratorPanel::SaveUndoState()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    m_UndoBuffer = data->Terrain->SplatMap;
    m_UndoBuffer2 = data->Terrain->SplatMap2;
    m_HasUndo = true;
}

void SplatmapGeneratorPanel::UndoLastOperation()
{
    if (!m_HasUndo) return;
    
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    terrain.SplatMap = m_UndoBuffer;
    terrain.SplatMap2 = m_UndoBuffer2;
    
    int res = terrain.GridResolution;
    Terrain::MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    m_Context->MarkDirty();
    
    m_HasUndo = false;
    m_StatusLine = "Undo applied.";
}

float SplatmapGeneratorPanel::ComputeSlope(int x, int y, const std::vector<uint16_t>& heightMap, 
                                           int res, float maxHeight, const glm::vec2& worldSize) const
{
    auto getHeight = [&](int px, int py) -> float {
        px = std::clamp(px, 0, res - 1);
        py = std::clamp(py, 0, res - 1);
        return heightMap[py * res + px] / 65535.0f;
    };
    
    float hL = getHeight(x - 1, y);
    float hR = getHeight(x + 1, y);
    float hD = getHeight(x, y - 1);
    float hU = getHeight(x, y + 1);
    
    glm::vec2 cellSize(worldSize.x / (res - 1), worldSize.y / (res - 1));
    
    float dzdx = (hR - hL) * maxHeight / (2.0f * cellSize.x);
    float dzdy = (hU - hD) * maxHeight / (2.0f * cellSize.y);
    
    float slopeRad = std::atan(std::sqrt(dzdx * dzdx + dzdy * dzdy));
    return glm::degrees(slopeRad);
}

float SplatmapGeneratorPanel::ComputeCurvature(int x, int y, const std::vector<uint16_t>& heightMap, int res) const
{
    auto getHeight = [&](int px, int py) -> float {
        px = std::clamp(px, 0, res - 1);
        py = std::clamp(py, 0, res - 1);
        return heightMap[py * res + px] / 65535.0f;
    };
    
    float h = getHeight(x, y);
    float hL = getHeight(x - 1, y);
    float hR = getHeight(x + 1, y);
    float hD = getHeight(x, y - 1);
    float hU = getHeight(x, y + 1);
    
    // Laplacian approximation - positive = convex (ridge), negative = concave (valley)
    float laplacian = (hL + hR + hD + hU) / 4.0f - h;
    return glm::clamp(laplacian * 50.0f, -1.0f, 1.0f);  // Scale and clamp
}

float SplatmapGeneratorPanel::ComputeLayerWeight(const SplatLayerRule& rule, float height, float slope, 
                                                  float curvature, float waterDist, const glm::vec2& uv) const
{
    if (!rule.Enabled)
        return 0.0f;
    
    float weight = 1.0f;
    
    // Height contribution
    if (rule.UseHeight)
    {
        float heightWeight = 1.0f;
        if (height < rule.HeightMin)
        {
            heightWeight = 1.0f - glm::clamp((rule.HeightMin - height) / glm::max(rule.HeightFalloff, kFloatEpsilon), 0.0f, 1.0f);
        }
        else if (height > rule.HeightMax)
        {
            heightWeight = 1.0f - glm::clamp((height - rule.HeightMax) / glm::max(rule.HeightFalloff, kFloatEpsilon), 0.0f, 1.0f);
        }
        weight *= heightWeight;
    }
    
    // Slope contribution
    if (rule.UseSlope)
    {
        float slopeWeight = 1.0f;
        if (slope < rule.SlopeMin)
        {
            slopeWeight = 1.0f - glm::clamp((rule.SlopeMin - slope) / glm::max(rule.SlopeFalloff, kFloatEpsilon), 0.0f, 1.0f);
        }
        else if (slope > rule.SlopeMax)
        {
            slopeWeight = 1.0f - glm::clamp((slope - rule.SlopeMax) / glm::max(rule.SlopeFalloff, kFloatEpsilon), 0.0f, 1.0f);
        }
        weight *= slopeWeight;
    }
    
    // Curvature contribution
    if (rule.UseCurvature)
    {
        float curvWeight = 0.0f;
        if (curvature >= rule.CurvatureMin && curvature <= rule.CurvatureMax)
        {
            curvWeight = 1.0f;
        }
        weight *= curvWeight;
    }
    
    // Water distance contribution
    if (rule.UseWaterDistance)
    {
        float waterWeight = 0.0f;
        if (waterDist >= rule.WaterDistanceMin && waterDist <= rule.WaterDistanceMax)
        {
            waterWeight = 1.0f;
        }
        else if (waterDist < rule.WaterDistanceMin)
        {
            waterWeight = 1.0f - glm::clamp((rule.WaterDistanceMin - waterDist) / 0.02f, 0.0f, 1.0f);
        }
        else if (waterDist > rule.WaterDistanceMax)
        {
            waterWeight = 1.0f - glm::clamp((waterDist - rule.WaterDistanceMax) / 0.02f, 0.0f, 1.0f);
        }
        weight *= waterWeight;
    }
    
    // Noise modulation
    if (rule.UseNoise && weight > kFloatEpsilon)
    {
        float noise = FBM(uv * rule.NoiseFrequency, 3, rule.NoiseSeed);
        float noiseWeight = glm::mix(1.0f - rule.NoiseStrength, 1.0f, noise);
        weight *= noiseWeight;
    }
    
    return glm::clamp(weight, 0.0f, 1.0f);
}

void SplatmapGeneratorPanel::GenerateSplatmap()
{
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain) return;
    
    TerrainComponent& terrain = *data->Terrain;
    const int res = terrain.GridResolution;
    const size_t count = static_cast<size_t>(res) * res;
    
    if (m_Rules.empty())
    {
        m_StatusLine = "No layer rules defined.";
        return;
    }
    
    auto start = std::chrono::steady_clock::now();
    
    // Sort rules by priority
    std::vector<SplatLayerRule> sortedRules = m_Rules;
    std::sort(sortedRules.begin(), sortedRules.end(), [](const SplatLayerRule& a, const SplatLayerRule& b) {
        return a.Priority < b.Priority;
    });

    const int terrainLayerCount = std::clamp(static_cast<int>(terrain.Layers.empty()
        ? 1
        : terrain.Layers.size()), 1, static_cast<int>(kMaxTerrainLayers));
    bool controlledLayers[kMaxTerrainLayers] = {};
    int enabledRuleCount = 0;
    int ignoredRuleCount = 0;
    for (const SplatLayerRule& rule : sortedRules)
    {
        if (!rule.Enabled)
            continue;

        const int layer = std::clamp(rule.LayerIndex, 0, static_cast<int>(kMaxTerrainLayers) - 1);
        if (layer >= terrainLayerCount)
        {
            ++ignoredRuleCount;
            continue;
        }

        controlledLayers[layer] = true;
        ++enabledRuleCount;
    }

    if (enabledRuleCount == 0)
    {
        m_StatusLine = "No enabled layer rules target existing terrain layers.";
        return;
    }

    int protectedLayerCount = 0;
    for (int layer = 0; layer < terrainLayerCount; ++layer)
    {
        if (!controlledLayers[layer])
            ++protectedLayerCount;
    }
    
    // Ensure splatmap is sized correctly
    if (terrain.SplatMap.size() != count)
    {
        terrain.SplatMap.assign(count, glm::u8vec4(255, 0, 0, 0));
    }
    if (terrainLayerCount > 4 || !terrain.SplatMap2.empty())
    {
        terrain.EnsureSplatMap2();
    }
    
    float invRes = 1.0f / static_cast<float>(res - 1);
    
    // Find water level from rules (use first rule that has water distance enabled)
    float waterLevel = 0.37f;
    for (const auto& rule : sortedRules)
    {
        if (rule.UseWaterDistance)
        {
            waterLevel = rule.WaterLevel;
            break;
        }
    }
    
    // Generate splatmap
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + x;
            glm::vec2 uv(x * invRes, y * invRes);
            
            float height = terrain.HeightMap[idx] / 65535.0f;
            float slope = ComputeSlope(x, y, terrain.HeightMap, res, terrain.MaxHeight, terrain.WorldSize);
            float curvature = ComputeCurvature(x, y, terrain.HeightMap, res);
            float waterDist = height - waterLevel;
            
            // Accumulate weights per channel (8 layers max)
            float channelWeights[kMaxTerrainLayers] = { 0.0f };
            
            // Apply rules in priority order
            for (const auto& rule : sortedRules)
            {
                float weight = ComputeLayerWeight(rule, height, slope, curvature, waterDist, uv);
                if (weight > kFloatEpsilon)
                {
                    int ch = std::clamp(rule.LayerIndex, 0, static_cast<int>(kMaxTerrainLayers) - 1);
                    if (ch >= terrainLayerCount || !controlledLayers[ch])
                        continue;
                    channelWeights[ch] = glm::max(channelWeights[ch], weight);
                }
            }

            const glm::u8vec4 originalSplat0 = terrain.SplatMap[idx];
            const glm::u8vec4 originalSplat1 = (!terrain.SplatMap2.empty() && idx < terrain.SplatMap2.size())
                ? terrain.SplatMap2[idx]
                : glm::u8vec4(0, 0, 0, 0);

            float protectedWeight = 0.0f;
            for (int layer = 0; layer < terrainLayerCount; ++layer)
            {
                if (!controlledLayers[layer])
                {
                    protectedWeight += ReadSplatLayerWeight(originalSplat0, originalSplat1, layer);
                }
            }
            const float writableWeight = glm::clamp(1.0f - protectedWeight, 0.0f, 1.0f);

            // Normalize generated weights only within rule-controlled layers.
            float totalWeight = 0.0f;
            for (int i = 0; i < terrainLayerCount; ++i) {
                if (controlledLayers[i])
                {
                    totalWeight += channelWeights[i];
                }
            }
            if (totalWeight > kFloatEpsilon)
            {
                for (int i = 0; i < terrainLayerCount; ++i)
                {
                    channelWeights[i] = controlledLayers[i]
                        ? (channelWeights[i] / totalWeight) * writableWeight
                        : 0.0f;
                }
            }
            else
            {
                for (int i = 0; i < terrainLayerCount; ++i)
                {
                    if (controlledLayers[i])
                    {
                        channelWeights[i] = writableWeight;
                        break;
                    }
                }
            }

            glm::u8vec4 packed0 = originalSplat0;
            glm::u8vec4 packed1 = originalSplat1;
            for (int layer = 0; layer < terrainLayerCount; ++layer)
            {
                if (controlledLayers[layer])
                {
                    WriteSplatLayerWeight(packed0, packed1, layer, channelWeights[layer]);
                }
            }

            terrain.SplatMap[idx] = packed0;
            if (!terrain.SplatMap2.empty())
            {
                terrain.SplatMap2[idx] = packed1;
            }
        }
    }
    
    Terrain::MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastOperationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream oss;
    oss << "Generated splatmap with " << enabledRuleCount << " enabled rules";
    if (protectedLayerCount > 0)
    {
        oss << "; preserved " << protectedLayerCount << " non-rule terrain layers";
    }
    if (ignoredRuleCount > 0)
    {
        oss << "; ignored " << ignoredRuleCount << " rules targeting missing terrain layers";
    }
    oss << ".";
    m_StatusLine = oss.str();
}

