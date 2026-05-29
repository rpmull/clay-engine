#include "ResourceLayerPanel.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Components.h"
#include "core/resourcelayer/ImposterManager.h"
#include "core/rendering/Renderer.h"
#include "editor/EditorIcons.h"
#include "editor/pipeline/AssetLibrary.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace cm {
namespace resourcelayer {

// UI Colors
namespace Colors {
    const ImVec4 HeaderBg = ImVec4(0.15f, 0.15f, 0.18f, 1.0f);
    const ImVec4 CardBg = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    const ImVec4 CardBgHover = ImVec4(0.16f, 0.16f, 0.19f, 1.0f);
    const ImVec4 Accent = ImVec4(0.3f, 0.7f, 0.4f, 1.0f);
    const ImVec4 AccentDark = ImVec4(0.2f, 0.5f, 0.3f, 1.0f);
    const ImVec4 Warning = ImVec4(0.9f, 0.7f, 0.2f, 1.0f);
    const ImVec4 Error = ImVec4(0.9f, 0.3f, 0.3f, 1.0f);
    const ImVec4 TextMuted = ImVec4(0.5f, 0.5f, 0.55f, 1.0f);
}

ResourceLayerPanel::ResourceLayerPanel(Scene* scene, EntityID* selectedEntity)
    : EditorPanel()
    , m_SelectedEntity(selectedEntity)
{
    SetContext(scene);
}

void ResourceLayerPanel::Open() {
    m_Open = true;
    SyncSelectionTarget();
}

void ResourceLayerPanel::Close() {
    m_Open = false;
}

void ResourceLayerPanel::Update(bool viewportHovered, bool playMode, Camera* viewportCamera) {
    (void)viewportHovered;
    (void)playMode;
    m_ViewportCamera = viewportCamera;
    
    if (m_StatusTimer > 0.0f) {
        m_StatusTimer -= 0.016f;
    }
}

void ResourceLayerPanel::OnImGuiRender() {
    if (!m_Open) return;
    
    ImGui::SetNextWindowSize(ImVec2(450, 700), ImGuiCond_FirstUseEver);
    
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (!ImGui::Begin("Resource Layers", &m_Open, flags)) {
        ImGui::End();
        return;
    }
    
    DrawHeader();
    
    ImGui::Separator();
    
    DrawTargetSelector();
    
    if (HasValidTarget()) {
        ImGui::Spacing();
        
        // Main content in two columns
        float contentHeight = ImGui::GetContentRegionAvail().y - 60.0f; // Reserve space for footer
        
        if (ImGui::BeginChild("MainContent", ImVec2(0, contentHeight), false)) {
            DrawClimateSection();
            ImGui::Spacing();
            DrawLayerList();
            ImGui::Spacing();
            DrawLayerSettings();
            ImGui::Spacing();
            DrawStatistics();
        }
        ImGui::EndChild();
        
        DrawFooter();
    } else {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextMuted);
        ImGui::TextWrapped("Select a terrain entity to configure resource layers.");
        ImGui::PopStyleColor();
        
        ImGui::Spacing();
        
        if (ImGui::Button("Create Resource Layer Component", ImVec2(-1, 30))) {
            if (m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID) {
                EntityData* data = m_Context->GetEntityData(*m_SelectedEntity);
                if (data && data->Terrain && !data->ResourceLayers) {
                    data->ResourceLayers = std::make_unique<ResourceLayerComponent>();
                    m_TargetTerrain = *m_SelectedEntity;
                    m_StatusMessage = "Resource Layer component created!";
                    m_StatusTimer = 3.0f;
                }
            }
        }
    }
    
    ImGui::End();
}

void ResourceLayerPanel::DrawHeader() {
    // Title with icon
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::Accent);
    ImGui::Text(ICON_FA_LAYER_GROUP " Resource Layers");
    ImGui::PopStyleColor();
    
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    
    if (ImGui::Button(ICON_FA_SYNC " Regenerate")) {
        RegenerateAll();
    }
    
    // Status message
    if (m_StatusTimer > 0.0f && !m_StatusMessage.empty()) {
        ImGui::TextColored(Colors::Accent, "%s", m_StatusMessage.c_str());
    }
}

void ResourceLayerPanel::DrawTargetSelector() {
    ImGui::Text("Target Terrain:");
    ImGui::SameLine();
    
    if (m_TargetTerrain != INVALID_ENTITY_ID) {
        EntityData* data = m_Context->GetEntityData(m_TargetTerrain);
        if (data) {
            ImGui::TextColored(Colors::Accent, "%s", data->Name.c_str());
        } else {
            ImGui::TextColored(Colors::Error, "(Invalid)");
            m_TargetTerrain = INVALID_ENTITY_ID;
        }
    } else {
        ImGui::TextColored(Colors::TextMuted, "(None selected)");
    }
    
    ImGui::SameLine();
    if (ImGui::SmallButton("Use Selected")) {
        SyncSelectionTarget();
    }
}

void ResourceLayerPanel::DrawClimateSection() {
    ImGui::PushStyleColor(ImGuiCol_Header, Colors::HeaderBg);
    
    if (ImGui::CollapsingHeader(ICON_FA_THERMOMETER_HALF " Climate Gradients", 
                                m_ShowClimateSection ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        m_ShowClimateSection = true;
        
        auto* comp = GetResourceLayerComponent();
        if (!comp) {
            ImGui::PopStyleColor();
            return;
        }
        
        ImGui::Indent();
        
        ImGui::Checkbox("Enable Climate System", &comp->UseClimateGradients);
        
        if (comp->UseClimateGradients) {
            ImGui::Spacing();
            
            // Preset dropdown
            if (ImGui::BeginCombo("Preset", "Select...")) {
                for (const auto& name : ClimateConfig::GetPresetNames()) {
                    if (ImGui::Selectable(name.c_str())) {
                        comp->Climate.ApplyPreset(name);
                        m_StatusMessage = "Applied " + name + " preset";
                        m_StatusTimer = 2.0f;
                    }
                }
                ImGui::EndCombo();
            }
            
            ImGui::Spacing();
            
            // World bounds
            if (ImGui::TreeNode("World Bounds")) {
                ImGui::DragFloat("Min Altitude", &comp->Climate.MinAltitude, 1.0f, -1000.0f, 10000.0f, "%.0f m");
                ImGui::DragFloat("Max Altitude", &comp->Climate.MaxAltitude, 1.0f, -1000.0f, 10000.0f, "%.0f m");
                ImGui::DragFloat("Min Longitude", &comp->Climate.MinLongitude, 10.0f, 0.0f, 100000.0f, "%.0f m");
                ImGui::DragFloat("Max Longitude", &comp->Climate.MaxLongitude, 10.0f, 0.0f, 100000.0f, "%.0f m");
                ImGui::TreePop();
            }
            
            // Vertical gradient
            if (ImGui::TreeNode("Vertical Gradient (Altitude)")) {
                DrawClimateGradientEditor(comp->Climate.VerticalGradient, "vertical");
                ImGui::TreePop();
            }
            
            // Longitudinal gradient
            if (ImGui::TreeNode("Longitudinal Gradient (Latitude)")) {
                DrawClimateGradientEditor(comp->Climate.LongitudinalGradient, "longitudinal");
                ImGui::TreePop();
            }
        }
        
        ImGui::Unindent();
    } else {
        m_ShowClimateSection = false;
    }
    
    ImGui::PopStyleColor();
}

void ResourceLayerPanel::DrawClimateGradientEditor(ClimateGradient& gradient, const char* label) {
    ImGui::PushID(label);
    
    // Simple table view of control points
    if (ImGui::BeginTable("GradientPoints", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Pos", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Temp", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Moist", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Wind", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        
        int toRemove = -1;
        
        for (size_t i = 0; i < gradient.Points.size(); ++i) {
            auto& pt = gradient.Points[i];
            ImGui::PushID(static_cast<int>(i));
            
            ImGui::TableNextRow();
            
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##pos", &pt.Position, 0.01f, 0.0f, 1.0f, "%.2f");
            
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##temp", &pt.Temperature, 0.5f, -40.0f, 50.0f, "%.0f°C");
            
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float moistPct = pt.Moisture * 100.0f;
            if (ImGui::DragFloat("##moist", &moistPct, 1.0f, 0.0f, 100.0f, "%.0f%%")) {
                pt.Moisture = moistPct / 100.0f;
            }
            
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            float windPct = pt.WindExposure * 100.0f;
            if (ImGui::DragFloat("##wind", &windPct, 1.0f, 0.0f, 100.0f, "%.0f%%")) {
                pt.WindExposure = windPct / 100.0f;
            }
            
            ImGui::TableNextColumn();
            if (gradient.Points.size() > 2) {
                if (ImGui::SmallButton(ICON_FA_TIMES)) {
                    toRemove = static_cast<int>(i);
                }
            }
            
            ImGui::PopID();
        }
        
        ImGui::EndTable();
        
        if (toRemove >= 0) {
            gradient.RemovePoint(toRemove);
        }
    }
    
    if (ImGui::Button(ICON_FA_PLUS " Add Point")) {
        ClimateGradient::ControlPoint pt;
        pt.Position = 0.5f;
        pt.Temperature = 15.0f;
        pt.Moisture = 0.5f;
        pt.WindExposure = 0.5f;
        gradient.AddPoint(pt);
    }
    
    ImGui::PopID();
}

void ResourceLayerPanel::DrawLayerList() {
    auto* comp = GetResourceLayerComponent();
    if (!comp) return;
    
    ImGui::PushStyleColor(ImGuiCol_Header, Colors::HeaderBg);
    
    if (ImGui::CollapsingHeader(ICON_FA_LIST " Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        
        // Layer cards
        for (size_t i = 0; i < comp->Layers.size(); ++i) {
            auto& layer = comp->Layers[i];
            ImGui::PushID(static_cast<int>(i));
            
            bool isSelected = (m_SelectedLayerIndex == static_cast<int>(i));
            
            // Card style
            ImVec4 cardColor = isSelected ? Colors::AccentDark : Colors::CardBg;
            ImGui::PushStyleColor(ImGuiCol_ChildBg, cardColor);
            
            ImGui::BeginChild("LayerCard", ImVec2(-1, 45), true, ImGuiWindowFlags_NoScrollbar);
            
            // Enable checkbox
            ImGui::Checkbox("##enabled", &layer.Enabled);
            ImGui::SameLine();
            
            // Layer name (clickable to select)
            if (ImGui::Selectable(layer.Name.c_str(), isSelected, 0, ImVec2(150, 0))) {
                m_SelectedLayerIndex = static_cast<int>(i);
                m_SelectedFilterIndex = -1;
            }
            
            ImGui::SameLine();
            
            // Instance count
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::TextMuted);
            size_t instanceCount = 0;
            for (const auto& inst : comp->Runtime.Instances) {
                if (inst.LayerIndex == i) instanceCount++;
            }
            ImGui::Text("(%zu)", instanceCount);
            ImGui::PopStyleColor();
            
            ImGui::SameLine(ImGui::GetWindowWidth() - 80);
            
            // Move buttons
            if (i > 0 && ImGui::SmallButton(ICON_FA_ARROW_UP)) {
                MoveLayerUp(i);
            }
            ImGui::SameLine();
            if (i < comp->Layers.size() - 1 && ImGui::SmallButton(ICON_FA_ARROW_DOWN)) {
                MoveLayerDown(i);
            }
            ImGui::SameLine();
            
            // Preview color indicator
            ImGui::ColorButton("##color", ImVec4(layer.PreviewColor.r, layer.PreviewColor.g, layer.PreviewColor.b, 1.0f),
                              ImGuiColorEditFlags_NoTooltip, ImVec2(16, 16));
            
            ImGui::EndChild();
            ImGui::PopStyleColor();
            
            ImGui::PopID();
        }
        
        ImGui::Spacing();
        
        // Add layer button
        if (ImGui::Button(ICON_FA_PLUS " Add Layer", ImVec2(-1, 30))) {
            AddNewLayer();
        }
    }
    
    ImGui::PopStyleColor();
}

void ResourceLayerPanel::DrawLayerSettings() {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0 || m_SelectedLayerIndex >= static_cast<int>(comp->Layers.size())) {
        return;
    }
    
    auto& layer = comp->Layers[m_SelectedLayerIndex];
    
    ImGui::PushStyleColor(ImGuiCol_Header, Colors::HeaderBg);
    
    if (ImGui::CollapsingHeader(ICON_FA_COG " Layer Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Indent();
        
        // Layer name
        char nameBuf[256];
        strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf) - 1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            layer.Name = nameBuf;
        }
        
        // Preview color
        ImGui::ColorEdit3("Preview Color", &layer.PreviewColor.x);
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        // Prefab selection
        ImGui::Text(ICON_FA_CUBE " Prefab");
        
        // Get all prefabs from asset library
        auto allAssets = AssetLibrary::Instance().GetAllAssets();
        struct PrefabEntry {
            std::string path;
            std::string displayName;
            ClaymoreGUID guid;
        };
        std::vector<PrefabEntry> prefabs;
        prefabs.push_back({"", "(None)", ClaymoreGUID()}); // Allow clearing
        
        int currentIdx = 0;
        for (const auto& [path, guid, type] : allAssets) {
            if (type == AssetType::Prefab) {
                std::string displayName = std::filesystem::path(path).stem().string();
                if (displayName.empty()) displayName = path;
                prefabs.push_back({path, displayName, guid});
                if (path == layer.PrefabPath || guid == layer.PrefabAsset.guid) {
                    currentIdx = static_cast<int>(prefabs.size()) - 1;
                }
            }
        }
        
        // Show combo dropdown
        ImGui::SetNextItemWidth(-1);
        const char* previewValue = (currentIdx < static_cast<int>(prefabs.size())) ? prefabs[currentIdx].displayName.c_str() : "(None)";
        if (ImGui::BeginCombo("##prefab", previewValue)) {
            for (int i = 0; i < static_cast<int>(prefabs.size()); ++i) {
                bool selected = (i == currentIdx);
                if (ImGui::Selectable(prefabs[i].displayName.c_str(), selected)) {
                    layer.PrefabPath = prefabs[i].path;
                    layer.PrefabAsset.guid = prefabs[i].guid;
                    layer.InvalidateCache();
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
                // Show path tooltip on hover
                if (ImGui::IsItemHovered() && !prefabs[i].path.empty()) {
                    ImGui::SetTooltip("%s", prefabs[i].path.c_str());
                }
            }
            ImGui::EndCombo();
        }
        
        // Show current path as tooltip
        if (ImGui::IsItemHovered() && !layer.PrefabPath.empty()) {
            ImGui::SetTooltip("Path: %s", layer.PrefabPath.c_str());
        }
        
        ImGui::Spacing();
        
        // Tabs for different settings
        if (ImGui::BeginTabBar("LayerSettingsTabs")) {
            
            if (ImGui::BeginTabItem("Distribution")) {
                DrawDistributionSettings();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("Eligibility")) {
                DrawEligibilityFilters();
                ImGui::EndTabItem();
            }
            
            if (ImGui::BeginTabItem("LOD")) {
                DrawLODSettings();
                ImGui::EndTabItem();
            }
            
            ImGui::EndTabBar();
        }
        
        ImGui::Unindent();
        
        // Actions
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        if (ImGui::Button(ICON_FA_SYNC " Regenerate Layer", ImVec2(150, 0))) {
            RegenerateSelectedLayer();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_COPY " Duplicate", ImVec2(100, 0))) {
            DuplicateLayer(m_SelectedLayerIndex);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Error);
        if (ImGui::Button(ICON_FA_TRASH " Delete", ImVec2(80, 0))) {
            RemoveLayer(m_SelectedLayerIndex);
        }
        ImGui::PopStyleColor();
    }
    
    ImGui::PopStyleColor();
}

void ResourceLayerPanel::DrawDistributionSettings() {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0) return;
    
    auto& layer = comp->Layers[m_SelectedLayerIndex];
    
    ImGui::Text("Density & Spacing");
    ImGui::DragFloat("Density (per m²)", &layer.DensityPerSquareMeter, 0.001f, 0.0f, 10.0f, "%.3f");
    ImGui::DragFloat("Min Spacing (m)", &layer.MinSpacing, 0.1f, 0.1f, 50.0f, "%.1f");
    
    ImGui::Spacing();
    ImGui::Text("Scale Variation");
    ImGui::Checkbox("Non-Uniform Scale", &layer.NonUniformScale);
    if (layer.NonUniformScale) {
        ImGui::DragFloat3("Min Scale", &layer.MinScaleVec.x, 0.01f, 0.1f, 5.0f);
        ImGui::DragFloat3("Max Scale", &layer.MaxScaleVec.x, 0.01f, 0.1f, 5.0f);
    } else {
        ImGui::DragFloatRange2("Scale Range", &layer.MinScale, &layer.MaxScale, 0.01f, 0.1f, 5.0f);
    }
    
    ImGui::Spacing();
    ImGui::Text("Rotation");
    ImGui::DragFloat("Yaw Variance (°)", &layer.YawVarianceDegrees, 1.0f, 0.0f, 360.0f);
    ImGui::DragFloat("Pitch Variance (°)", &layer.PitchVarianceDegrees, 0.5f, 0.0f, 45.0f);
    ImGui::DragFloat("Roll Variance (°)", &layer.RollVarianceDegrees, 0.5f, 0.0f, 45.0f);
    
    ImGui::Checkbox("Align to Slope", &layer.AlignToSlope);
    if (layer.AlignToSlope) {
        ImGui::SliderFloat("Alignment Factor", &layer.SlopeAlignmentFactor, 0.0f, 1.0f);
    }
    
    ImGui::Spacing();
    ImGui::Text("Height Offset");
    ImGui::DragFloat("Offset (m)", &layer.HeightOffset, 0.01f, -10.0f, 10.0f);
    ImGui::DragFloat("Variance (m)", &layer.HeightOffsetVariance, 0.01f, 0.0f, 5.0f);
    
    ImGui::Spacing();
    if (ImGui::TreeNode("Clustering")) {
        ImGui::Checkbox("Enable Clustering", &layer.EnableClustering);
        if (layer.EnableClustering) {
            ImGui::DragFloat("Cluster Radius", &layer.ClusterRadius, 0.5f, 1.0f, 100.0f);
            ImGui::DragInt("Min Count", &layer.ClusterMinCount, 0.1f, 1, 50);
            ImGui::DragInt("Max Count", &layer.ClusterMaxCount, 0.1f, 1, 100);
            ImGui::DragFloat("Cluster Spacing", &layer.ClusterSpacing, 1.0f, 5.0f, 500.0f);
            ImGui::SliderFloat("Falloff", &layer.ClusterFalloff, 0.0f, 1.0f);
        }
        ImGui::TreePop();
    }
}

void ResourceLayerPanel::DrawEligibilityFilters() {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0) return;
    
    auto& layer = comp->Layers[m_SelectedLayerIndex];
    auto& eligibility = layer.Eligibility;
    
    // Combine mode
    const char* modes[] = { "Multiply (AND)", "Minimum", "Maximum", "Average" };
    int modeInt = static_cast<int>(eligibility.Mode);
    if (ImGui::Combo("Combine Mode", &modeInt, modes, 4)) {
        eligibility.Mode = static_cast<CompositeEligibilityMap::CombineMode>(modeInt);
    }
    
    ImGui::Spacing();
    ImGui::Text("Filters:");
    
    // Filter list
    for (size_t i = 0; i < eligibility.Filters.size(); ++i) {
        auto& entry = eligibility.Filters[i];
        if (!entry.Filter) continue;
        
        ImGui::PushID(static_cast<int>(i));
        
        bool isSelected = (m_SelectedFilterIndex == static_cast<int>(i));
        ImVec4 cardColor = isSelected ? Colors::AccentDark : Colors::CardBg;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, cardColor);
        
        float filterHeight = isSelected ? 120.0f : 35.0f;
        ImGui::BeginChild("FilterCard", ImVec2(-1, filterHeight), true);
        
        // Header row
        ImGui::Checkbox("##enabled", &entry.Enabled);
        ImGui::SameLine();
        
        if (ImGui::Selectable(entry.Filter->GetTypeName(), isSelected, 0, ImVec2(120, 0))) {
            m_SelectedFilterIndex = isSelected ? -1 : static_cast<int>(i);
        }
        
        ImGui::SameLine(ImGui::GetWindowWidth() - 130);
        
        ImGui::SetNextItemWidth(50);
        ImGui::DragFloat("##weight", &entry.Weight, 0.1f, 0.0f, 10.0f, "%.1f");
        
        ImGui::SameLine();
        if (i > 0 && ImGui::SmallButton(ICON_FA_ARROW_UP "##up")) {
            MoveFilterUp(i);
        }
        ImGui::SameLine();
        if (i < eligibility.Filters.size() - 1 && ImGui::SmallButton(ICON_FA_ARROW_DOWN "##down")) {
            MoveFilterDown(i);
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, Colors::Error);
        if (ImGui::SmallButton(ICON_FA_TIMES "##remove")) {
            RemoveFilter(i);
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
            break;
        }
        ImGui::PopStyleColor();
        
        // Expanded settings
        if (isSelected) {
            ImGui::Separator();
            entry.Filter->DrawInspector();
        }
        
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    
    ImGui::Spacing();
    
    // Add filter dropdown
    if (ImGui::Button(ICON_FA_PLUS " Add Filter", ImVec2(-1, 28))) {
        ImGui::OpenPopup("AddFilterPopup");
    }
    
    if (ImGui::BeginPopup("AddFilterPopup")) {
        for (const auto& typeName : IEligibilityFilter::GetRegisteredTypes()) {
            if (ImGui::MenuItem(typeName.c_str())) {
                AddFilter(typeName);
            }
        }
        ImGui::EndPopup();
    }
}

void ResourceLayerPanel::DrawLODSettings() {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0) return;
    
    auto& layer = comp->Layers[m_SelectedLayerIndex];
    
    ImGui::Checkbox("Use Imposter System", &layer.UseImposter);
    
    if (layer.UseImposter) {
        ImGui::DragFloat("Imposter Distance (m)", &layer.ImposterDistance, 1.0f, 1.0f, 500.0f);
        ImGui::DragFloat("Cull Distance (m)", &layer.CullDistance, 5.0f, 10.0f, 2000.0f);
        ImGui::DragFloat("Crossfade Range (m)", &layer.CrossfadeRange, 0.5f, 0.0f, 20.0f);
        
        ImGui::Spacing();
        
        // Imposter baking status
        bool hasImposter = ImposterManager::Instance().HasImposter(layer.PrefabAsset.guid);
        if (hasImposter) {
            ImGui::TextColored(Colors::Accent, ICON_FA_CHECK " Imposter baked");
        } else {
            ImGui::TextColored(Colors::Warning, ICON_FA_EXCLAMATION_TRIANGLE " Imposter not baked");
        }
        
        if (ImGui::Button("Bake Imposter", ImVec2(120, 0))) {
            // Clear existing cache to force fresh bake
            ImposterManager::Instance().ClearImposter(layer.PrefabAsset.guid);
            ImposterCache cache;
            if (ImposterManager::Instance().BakeImposter(layer.PrefabAsset.guid, cache)) {
                m_StatusMessage = "Imposter baked successfully!";
            } else {
                m_StatusMessage = "Failed to bake imposter";
            }
            m_StatusTimer = 3.0f;
        }
    }
    
    // Interaction settings
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    ImGui::TextColored(Colors::Accent, ICON_FA_HAND_POINTER " Interaction");
    ImGui::Spacing();
    
    if (ImGui::Checkbox("Interactable", &layer.Interactable)) {
        // If making interactable, enable physics preservation by default
        if (layer.Interactable && !layer.PreservePhysics) {
            layer.PreservePhysics = true;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Allow player to interact with these resources (foraging, mining, etc.)");
    }
    
    if (layer.Interactable) {
        ImGui::Indent();
        
        ImGui::Checkbox("Preserve Physics", &layer.PreservePhysics);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Keep colliders and physics components when spawned.\n"
                              "Enable for items that need collision detection.");
        }
        
        ImGui::DragFloat("Interaction Radius (m)", &layer.InteractionRadius, 0.1f, 0.5f, 10.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("How close the player must be to interact with this resource");
        }
        
        // Interaction tag
        char tagBuffer[128];
        strncpy(tagBuffer, layer.InteractionTag.c_str(), sizeof(tagBuffer) - 1);
        tagBuffer[sizeof(tagBuffer) - 1] = '\0';
        if (ImGui::InputText("Interaction Tag", tagBuffer, sizeof(tagBuffer))) {
            layer.InteractionTag = tagBuffer;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Tag for gameplay systems (e.g., 'forageable', 'mineable', 'harvestable')");
        }
        
        // Quick tag presets
        ImGui::SameLine();
        if (ImGui::BeginCombo("##TagPresets", "", ImGuiComboFlags_NoPreview)) {
            if (ImGui::Selectable("forageable")) { layer.InteractionTag = "forageable"; }
            if (ImGui::Selectable("mineable")) { layer.InteractionTag = "mineable"; }
            if (ImGui::Selectable("harvestable")) { layer.InteractionTag = "harvestable"; }
            if (ImGui::Selectable("choppable")) { layer.InteractionTag = "choppable"; }
            if (ImGui::Selectable("collectible")) { layer.InteractionTag = "collectible"; }
            ImGui::EndCombo();
        }
        
        ImGui::Unindent();
    }
}

void ResourceLayerPanel::DrawStatistics() {
    auto* comp = GetResourceLayerComponent();
    if (!comp) return;
    
    ImGui::PushStyleColor(ImGuiCol_Header, Colors::HeaderBg);
    
    if (ImGui::CollapsingHeader(ICON_FA_CHART_BAR " Statistics")) {
        ImGui::Indent();
        
        // Show play mode status (swap system only works in play mode)
        bool isPlaying = m_Context && m_Context->m_IsPlaying;
        if (isPlaying) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.9f, 0.2f, 1.0f));
            ImGui::Text(ICON_FA_CHECK " Play Mode Active - Prefab swap enabled");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.2f, 1.0f));
            ImGui::Text(ICON_FA_EXCLAMATION_TRIANGLE " Editor Mode - Prefab swap disabled");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Enter Play mode to enable imposter/prefab swapping.");
        }
        
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        ImGui::Text("Total Instances: %u", comp->Stats.TotalInstances);
        ImGui::Text("Visible Imposters: %u", comp->Stats.VisibleImposters);
        ImGui::Text("Active Prefabs: %u / %u", comp->Stats.ActivePrefabs, comp->MaxActivePrefabs);
        ImGui::Text("Culled: %u", comp->Stats.CulledInstances);
        ImGui::Text("Generation Time: %.1f ms", comp->Stats.GenerationTimeMs);
        
        ImGui::Spacing();
        
        // Swap system settings
        if (ImGui::TreeNode("Swap Settings")) {
            ImGui::DragFloat("Swap Distance (m)", &comp->GlobalSwapDistance, 1.0f, 1.0f, 200.0f,
                "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Distance at which imposters swap to full prefabs in play mode");
            
            ImGui::DragFloat("Hysteresis (m)", &comp->SwapHysteresis, 0.5f, 0.0f, 20.0f);
            ImGui::SetItemTooltip("Prevents rapid back-and-forth swapping at boundary");
            
            ImGui::DragInt("Max Prefabs", reinterpret_cast<int*>(&comp->MaxActivePrefabs), 
                1.0f, 1, 1000);
            ImGui::SetItemTooltip("Budget for simultaneously active prefab instances");
            
            ImGui::TreePop();
        }
        
        // Per-layer stats
        if (ImGui::TreeNode("Per Layer")) {
            for (size_t i = 0; i < comp->Layers.size(); ++i) {
                const auto& layer = comp->Layers[i];
                size_t totalCount = 0;
                size_t activeCount = 0;
                for (const auto& inst : comp->Runtime.Instances) {
                    if (inst.LayerIndex == i) {
                        totalCount++;
                        if (inst.State == ResourceState::Active) activeCount++;
                    }
                }
                ImGui::Text("%s: %zu (%zu active)", layer.Name.c_str(), totalCount, activeCount);
            }
            ImGui::TreePop();
        }
        
        ImGui::Unindent();
    }
    
    ImGui::PopStyleColor();
}

void ResourceLayerPanel::DrawFooter() {
    ImGui::Separator();
    ImGui::Spacing();
    
    // Global settings
    auto* comp = GetResourceLayerComponent();
    if (comp) {
        ImGui::SetNextItemWidth(100);
        ImGui::DragInt("Seed", reinterpret_cast<int*>(&comp->GlobalSeed));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::DragFloat("Density×", &comp->GlobalDensityMultiplier, 0.1f, 0.0f, 10.0f, "%.1f");
    }
    
    ImGui::SameLine(ImGui::GetWindowWidth() - 120);
    
    if (ImGui::Button(ICON_FA_SYNC_ALT " Regenerate All", ImVec2(-1, 0))) {
        RegenerateAll();
    }
}

//------------------------------------------------------------------------------
// Layer Management
//------------------------------------------------------------------------------

void ResourceLayerPanel::AddNewLayer() {
    auto* comp = GetResourceLayerComponent();
    if (!comp) return;
    
    ProceduralResourceLayer layer;
    layer.Name = "New Layer " + std::to_string(comp->Layers.size() + 1);
    layer.PreviewColor = glm::vec3(
        0.3f + 0.4f * (comp->Layers.size() % 3) / 2.0f,
        0.5f + 0.3f * ((comp->Layers.size() + 1) % 3) / 2.0f,
        0.4f + 0.4f * ((comp->Layers.size() + 2) % 3) / 2.0f
    );
    
    // Add default slope filter
    layer.Eligibility.AddFilter(std::make_unique<SlopeFilter>());
    
    comp->Layers.push_back(std::move(layer));
    m_SelectedLayerIndex = static_cast<int>(comp->Layers.size()) - 1;
    
    m_StatusMessage = "Layer added";
    m_StatusTimer = 2.0f;
}

void ResourceLayerPanel::DuplicateLayer(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || index >= comp->Layers.size()) return;
    
    ProceduralResourceLayer copy = comp->Layers[index];
    copy.Name += " (Copy)";
    copy.Guid = ClaymoreGUID::Generate();
    
    comp->Layers.insert(comp->Layers.begin() + index + 1, std::move(copy));
    m_SelectedLayerIndex = static_cast<int>(index) + 1;
}

void ResourceLayerPanel::RemoveLayer(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || index >= comp->Layers.size()) return;
    
    comp->RemoveLayerAt(index);
    
    if (m_SelectedLayerIndex >= static_cast<int>(comp->Layers.size())) {
        m_SelectedLayerIndex = static_cast<int>(comp->Layers.size()) - 1;
    }
    
    comp->NeedsFullRegeneration = true;
}

void ResourceLayerPanel::MoveLayerUp(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || index == 0 || index >= comp->Layers.size()) return;
    
    std::swap(comp->Layers[index], comp->Layers[index - 1]);
    if (m_SelectedLayerIndex == static_cast<int>(index)) {
        m_SelectedLayerIndex--;
    }
}

void ResourceLayerPanel::MoveLayerDown(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || index >= comp->Layers.size() - 1) return;
    
    std::swap(comp->Layers[index], comp->Layers[index + 1]);
    if (m_SelectedLayerIndex == static_cast<int>(index)) {
        m_SelectedLayerIndex++;
    }
}

//------------------------------------------------------------------------------
// Filter Management
//------------------------------------------------------------------------------

void ResourceLayerPanel::AddFilter(const std::string& typeName) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0) return;
    
    auto filter = IEligibilityFilter::Create(typeName);
    if (filter) {
        comp->Layers[m_SelectedLayerIndex].Eligibility.AddFilter(std::move(filter));
    }
}

void ResourceLayerPanel::RemoveFilter(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0) return;
    
    comp->Layers[m_SelectedLayerIndex].Eligibility.RemoveFilter(index);
    m_SelectedFilterIndex = -1;
}

void ResourceLayerPanel::MoveFilterUp(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0 || index == 0) return;
    
    comp->Layers[m_SelectedLayerIndex].Eligibility.MoveFilter(index, index - 1);
}

void ResourceLayerPanel::MoveFilterDown(size_t index) {
    auto* comp = GetResourceLayerComponent();
    if (!comp || m_SelectedLayerIndex < 0) return;
    
    auto& filters = comp->Layers[m_SelectedLayerIndex].Eligibility.Filters;
    if (index >= filters.size() - 1) return;
    
    comp->Layers[m_SelectedLayerIndex].Eligibility.MoveFilter(index, index + 1);
}

//------------------------------------------------------------------------------
// Generation
//------------------------------------------------------------------------------

void ResourceLayerPanel::RegenerateAll() {
    auto* comp = GetResourceLayerComponent();
    EntityData* data = GetTargetEntityData();
    if (!comp || !data || !data->Terrain) return;
    
    // Get terrain world transform
    glm::mat4 terrainTransform = data->Transform.WorldMatrix;
    
    auto start = std::chrono::high_resolution_clock::now();
    comp->Regenerate(*data->Terrain, terrainTransform);
    auto end = std::chrono::high_resolution_clock::now();
    
    float ms = std::chrono::duration<float, std::milli>(end - start).count();
    m_StatusMessage = "Regenerated " + std::to_string(comp->Stats.TotalInstances) + 
                      " instances in " + std::to_string(static_cast<int>(ms)) + "ms";
    m_StatusTimer = 3.0f;
}

void ResourceLayerPanel::RegenerateSelectedLayer() {
    auto* comp = GetResourceLayerComponent();
    EntityData* data = GetTargetEntityData();
    if (!comp || !data || !data->Terrain || m_SelectedLayerIndex < 0) return;
    
    // Get terrain world transform
    glm::mat4 terrainTransform = data->Transform.WorldMatrix;
    
    comp->RegenerateLayer(m_SelectedLayerIndex, *data->Terrain, terrainTransform);
    
    m_StatusMessage = "Layer regenerated";
    m_StatusTimer = 2.0f;
}

void ResourceLayerPanel::BakeImposters() {
    auto* comp = GetResourceLayerComponent();
    if (!comp) return;
    
    int baked = 0;
    for (auto& layer : comp->Layers) {
        if (layer.PrefabAsset.guid.high != 0 || layer.PrefabAsset.guid.low != 0) {
            // Clear existing cache to force fresh bake
            ImposterManager::Instance().ClearImposter(layer.PrefabAsset.guid);
            ImposterCache cache;
            if (ImposterManager::Instance().BakeImposter(layer.PrefabAsset.guid, cache)) {
                baked++;
            }
        }
    }
    
    m_StatusMessage = "Baked " + std::to_string(baked) + " imposters";
    m_StatusTimer = 3.0f;
}

//------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------

bool ResourceLayerPanel::HasValidTarget() const {
    if (m_TargetTerrain == INVALID_ENTITY_ID) return false;
    
    EntityData* data = m_Context->GetEntityData(m_TargetTerrain);
    return data && data->Terrain && data->ResourceLayers;
}

EntityData* ResourceLayerPanel::GetTargetEntityData() const {
    if (m_TargetTerrain == INVALID_ENTITY_ID) return nullptr;
    return m_Context->GetEntityData(m_TargetTerrain);
}

ResourceLayerComponent* ResourceLayerPanel::GetResourceLayerComponent() const {
    EntityData* data = GetTargetEntityData();
    return data ? data->ResourceLayers.get() : nullptr;
}

void ResourceLayerPanel::SyncSelectionTarget() {
    if (!m_SelectedEntity || *m_SelectedEntity == INVALID_ENTITY_ID) return;
    
    EntityData* data = m_Context->GetEntityData(*m_SelectedEntity);
    if (data && data->Terrain) {
        m_TargetTerrain = *m_SelectedEntity;
    }
}

void ResourceLayerPanel::DrawEligibilityPreview() {
    // TODO: Implement viewport overlay showing eligibility values as color gradient
}

} // namespace resourcelayer
} // namespace cm

