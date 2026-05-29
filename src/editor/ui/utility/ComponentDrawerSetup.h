#pragma once
#include "ComponentDrawerRegistry.h"
#include "TextureSlotPicker.h"
#include "core/ecs/Components.h"
#include "core/ecs/AnimationComponents.h" // adjust path to where TransformComponent etc. live
#include "core/rendering/MaterialCache.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/Renderer.h"
#include "core/particles/SpriteLoader.h"
#include "editor/EnginePaths.h"
#include "core/physics/Physics.h"
#include "core/physics/PhysicsLayerManager.h"
#include "core/assets/AssetReference.h" // for ClaymoreGUID
#include "core/world/WorldGraph.h"
#include "core/world/RuntimeWorld.h"

#include <imgui.h>
#include <imgui_clay_inspector.h>
#include <glm/glm.hpp>
#include <functional>
#include <glm/gtx/euler_angles.hpp>
#include <filesystem>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <particles/ParticleSystem.h>
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimationAssetCache.h"
#include "ui/utility/AnimationAssetListCache.h"
#include "ui/utility/AudioAssetListCache.h"
#include "core/audio/AudioComponents.h"
#include <cstring>
#include <sstream>
#include "editor/Project.h"
#include "editor/pipeline/AssetLibrary.h"
#include "core/navigation/NavMesh.h"
#include "core/navigation/NavAgent.h"
#include "core/navigation/Navigation.h"
#include "core/navigation/NavDebugDraw.h"
#include "core/ecs/Scene.h"
#include "core/rendering/Terrain.h"
#include "core/rendering/GlobalShaderProperties.h"
#include "core/physics/area/AreaComponent.h"
#include <stb_image.h>

namespace
{
    inline bool TerrainInstancerDiagnosticsEnabled() {
        static const bool enabled = []() {
            const char* value = std::getenv("CLAYMORE_INSTANCER_DIAGNOSTICS");
            return value != nullptr && value[0] != '\0' && std::string(value) != "0";
        }();
        return enabled;
    }

    class ScopedTerrainInstancerUiTimer {
    public:
        explicit ScopedTerrainInstancerUiTimer(const char* label)
            : m_Label(label)
            , m_Enabled(TerrainInstancerDiagnosticsEnabled())
            , m_Start(std::chrono::high_resolution_clock::now())
        {
        }

        ~ScopedTerrainInstancerUiTimer()
        {
            if (!m_Enabled)
                return;

            const auto end = std::chrono::high_resolution_clock::now();
            const float elapsedMs = std::chrono::duration<float, std::milli>(end - m_Start).count();
            std::cout << "[InstancerDiag] " << m_Label << " " << elapsedMs << "ms\n";
        }

    private:
        const char* m_Label = "";
        bool m_Enabled = false;
        std::chrono::high_resolution_clock::time_point m_Start;
    };

    // Helper: Draw physics layer dropdown combo
    // Returns true if layer changed
    inline bool DrawPhysicsLayerCombo(const char* label, uint32_t& layerIndex, std::string& layerName) {
        auto& mgr = PhysicsLayers::PhysicsLayerManager::Get();
        const auto& layers = mgr.GetAllLayers();
        
        // Find current layer name for display
        std::string currentName = layerIndex < layers.size() ? layers[layerIndex] : "Unknown";
        if (!layerName.empty() && mgr.HasLayer(layerName)) {
            currentName = layerName;
            layerIndex = static_cast<uint32_t>(mgr.GetLayerIndex(layerName));
        }
        
        bool changed = false;
        if (ImGui::BeginCombo(label, currentName.c_str())) {
            for (uint32_t i = 0; i < layers.size(); ++i) {
                bool selected = (i == layerIndex);
                char buf[128];
                snprintf(buf, sizeof(buf), "[%u] %s", i, layers[i].c_str());
                if (ImGui::Selectable(buf, selected)) {
                    layerIndex = i;
                    layerName = layers[i];
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    inline std::string FormatPhysicsLayerMask(uint32_t mask) {
        auto& mgr = PhysicsLayers::PhysicsLayerManager::Get();
        const auto& layers = mgr.GetAllLayers();

        if (mask == 0xFFFFFFFFu) return "All";
        if (mask == 0u) return "None";

        std::string result;
        uint32_t visibleCount = 0;
        uint32_t totalCount = 0;
        for (uint32_t i = 0; i < layers.size() && i < MAX_PHYSICS_LAYERS; ++i) {
            if ((mask & (1u << i)) == 0u) {
                continue;
            }

            ++totalCount;
            if (visibleCount < 3) {
                if (!result.empty()) result += ", ";
                result += layers[i];
                ++visibleCount;
            }
        }

        if (result.empty()) return "None";
        if (totalCount > visibleCount) {
            result += " +";
            result += std::to_string(totalCount - visibleCount);
        }
        return result;
    }

    inline bool DrawPhysicsLayerMaskCombo(const char* label, uint32_t& mask) {
        auto& mgr = PhysicsLayers::PhysicsLayerManager::Get();
        const auto& layers = mgr.GetAllLayers();
        bool changed = false;
        const std::string summary = FormatPhysicsLayerMask(mask);

        if (ImGui::BeginCombo(label, summary.c_str())) {
            if (ImGui::Selectable("All", mask == 0xFFFFFFFFu)) {
                mask = 0xFFFFFFFFu;
                changed = true;
            }
            if (ImGui::Selectable("None", mask == 0u)) {
                mask = 0u;
                changed = true;
            }
            ImGui::Separator();

            for (uint32_t i = 0; i < layers.size() && i < MAX_PHYSICS_LAYERS; ++i) {
                bool enabled = (mask & (1u << i)) != 0u;
                char buf[128];
                snprintf(buf, sizeof(buf), "[%u] %s", i, layers[i].c_str());
                if (ImGui::Checkbox(buf, &enabled)) {
                    if (enabled) {
                        mask |= (1u << i);
                    } else {
                        mask &= ~(1u << i);
                    }
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }

        return changed;
    }

    void RemapTerrainSplatAfterLayerRemoval(TerrainComponent& terrain, int removedIndex)
    {
        if (terrain.SplatMap.empty())
            return;

        removedIndex = std::clamp(removedIndex, 0, 3);
        constexpr float kInv255 = 1.0f / 255.0f;
        constexpr float kEpsilon = 1e-4f;

        for (glm::u8vec4& px : terrain.SplatMap)
        {
            glm::vec4 weights = glm::vec4(px) * kInv255;
            weights[removedIndex] = 0.0f;
            float sum = weights.x + weights.y + weights.z + weights.w;
            if (sum > kEpsilon)
                weights /= sum;
            else
                weights = glm::vec4(0.25f);

            glm::vec4 clamped = glm::clamp(weights, glm::vec4(0.0f), glm::vec4(1.0f));
            glm::ivec4 ints = glm::ivec4(glm::round(clamped * 255.0f));
            ints = glm::clamp(ints, glm::ivec4(0), glm::ivec4(255));

            px.r = static_cast<uint8_t>(ints.x);
            px.g = static_cast<uint8_t>(ints.y);
            px.b = static_cast<uint8_t>(ints.z);
            int rgbSum = ints.x + ints.y + ints.z;
            px.a = static_cast<uint8_t>(std::clamp(255 - rgbSum, 0, 255));
        }

        for (TerrainChunk& chunk : terrain.Chunks)
        {
            chunk.SplatDirty = true;
            chunk.DirtyMin = glm::ivec2(0);
            chunk.DirtyMax = glm::ivec2(static_cast<int>(terrain.GridResolution) - 1);
        }
        terrain.SplatDataDirty = true;
        terrain.AssetDirty = true;
        terrain.GrassMasksDirty = true;
    }

    bool AnimationAssetHasSkeletalTracks(const cm::animation::AnimationAsset& asset)
    {
        for (const auto& track : asset.tracks)
        {
            if (!track) continue;
            if (track->type == cm::animation::TrackType::Bone ||
                track->type == cm::animation::TrackType::Avatar)
            {
                return true;
            }
        }
        return false;
    }

    EntityID FindNearestCanvasEntity(Scene& scene, EntityID ownerId)
    {
        EntityID current = ownerId;
        while (current != INVALID_ENTITY_ID) {
            auto* data = scene.GetEntityData(current);
            if (!data)
                break;
            if (data->Canvas)
                return current;
            current = data->Parent;
        }
        return INVALID_ENTITY_ID;
    }

    glm::ivec2 ComputeSuggestedWorldSpaceUICanvasSize(const EntityData* data)
    {
        float width = 0.0f;
        float height = 0.0f;

        if (data && data->Panel) {
            width = std::max(width, std::abs(data->Panel->Size.x * data->Panel->Scale.x));
            height = std::max(height, std::abs(data->Panel->Size.y * data->Panel->Scale.y));
        }

        if (data && data->Text && !data->Text->WorldSpace) {
            const float pixelSize = std::max(data->Text->PixelSize, 1.0f);
            const float fallbackWidth = data->Text->RectSize.x > 0.0f
                ? data->Text->RectSize.x
                : pixelSize * std::max<size_t>(data->Text->Text.empty() ? 8u : data->Text->Text.size(), 4u) * 0.6f;
            const float fallbackHeight = data->Text->RectSize.y > 0.0f
                ? data->Text->RectSize.y
                : pixelSize * 1.5f;
            width = std::max(width, fallbackWidth);
            height = std::max(height, fallbackHeight);
        }

        if (width <= 0.0f)
            width = 256.0f;
        if (height <= 0.0f)
            height = 256.0f;

        return glm::ivec2(
            std::max(1, static_cast<int>(std::ceil(width))),
            std::max(1, static_cast<int>(std::ceil(height))));
    }

    void EnsureWorldSpaceUICanvasSize(EntityData* data)
    {
        if (!data || !data->Canvas)
            return;

        glm::ivec2 suggested = ComputeSuggestedWorldSpaceUICanvasSize(data);
        if (data->Canvas->Width <= 0)
            data->Canvas->Width = suggested.x;
        if (data->Canvas->Height <= 0)
            data->Canvas->Height = suggested.y;
    }

    Scene* GetCurrentComponentDrawScene()
    {
        if (Scene* scene = ComponentDrawerRegistry::Instance().GetCurrentScene())
            return scene;
        return &Scene::Get();
    }

    EntityID GetCurrentComponentDrawEntity()
    {
        return ComponentDrawerRegistry::Instance().GetCurrentEntity();
    }

    EntityData* GetCurrentComponentEntityData()
    {
        Scene* scene = GetCurrentComponentDrawScene();
        const EntityID ownerId = GetCurrentComponentDrawEntity();
        if (!scene || ownerId == INVALID_ENTITY_ID)
            return nullptr;
        return scene->GetEntityData(ownerId);
    }

    void NotifyCurrentComponentChanged(cm::world::RuntimeDirtyBits bits)
    {
        Scene* scene = GetCurrentComponentDrawScene();
        const EntityID ownerId = GetCurrentComponentDrawEntity();
        if (!scene || ownerId == INVALID_ENTITY_ID)
            return;
        scene->NotifyComponentChanged(ownerId, bits);
    }

    void NotifyCurrentTransformChanged(TransformComponent& transform)
    {
        Scene* scene = GetCurrentComponentDrawScene();
        const EntityID ownerId = GetCurrentComponentDrawEntity();
        if (!scene || ownerId == INVALID_ENTITY_ID) {
            transform.TransformDirty = true;
            return;
        }
        scene->NotifyTransformChanged(ownerId);
    }

    bool EntityHasCanvasBackedUIComponents(const EntityData* data)
    {
        return data && (
            data->Canvas ||
            data->Panel ||
            data->Button ||
            data->Slider ||
            data->ProgressBar ||
            data->Toggle ||
            data->ScrollView ||
            data->LayoutGroup ||
            data->InputField ||
            data->Dropdown ||
            data->UIRect ||
            data->FitToContent ||
            data->UISceneCapture);
    }

    bool TextUsesCanvasBackedUI(const TextRendererComponent& text)
    {
        Scene* scene = GetCurrentComponentDrawScene();
        const EntityID ownerId = GetCurrentComponentDrawEntity();
        EntityData* ownerData = GetCurrentComponentEntityData();
        if (!scene || ownerId == INVALID_ENTITY_ID || !ownerData)
            return false;

        if (FindNearestCanvasEntity(*scene, ownerId) != INVALID_ENTITY_ID)
            return true;

        return EntityHasCanvasBackedUIComponents(ownerData);
    }

    void CopyCanvasDefaultsFromAncestor(CanvasComponent& destination, const CanvasComponent& source)
    {
        destination.DPIScale = source.DPIScale;
        destination.SortOrder = source.SortOrder;
        destination.Opacity = source.Opacity;
        destination.BlockSceneInput = source.BlockSceneInput;
        destination.Billboard = source.Billboard;
        destination.ReferenceWidth = source.ReferenceWidth;
        destination.ReferenceHeight = source.ReferenceHeight;
        destination.ReferenceScaleMode = source.ReferenceScaleMode;
    }

    CanvasComponent* EnsureLocalUICanvas(Scene& scene, EntityID ownerId, EntityData* ownerData, const CanvasComponent* inheritedCanvas)
    {
        if (!ownerData)
            return nullptr;
        if (!ownerData->Canvas) {
            ownerData->Canvas = std::make_unique<CanvasComponent>();
            if (inheritedCanvas)
                CopyCanvasDefaultsFromAncestor(*ownerData->Canvas, *inheritedCanvas);
            scene.MarkEntityStructureDirty(ownerId);
        }
        return ownerData->Canvas.get();
    }

    void NormalizeWorldSpaceUIRootLayout(EntityData* ownerData)
    {
        if (!ownerData)
            return;

        if (ownerData->Panel) {
            if (ownerData->UIRect && ownerData->UIRect->AnchorToParent) {
                ownerData->UIRect->Offset = glm::vec2(0.0f);
            } else {
                ownerData->Panel->AnchorEnabled = false;
                ownerData->Panel->AnchorToParentUI = false;
                ownerData->Panel->AnchorOffset = glm::vec2(0.0f);
                ownerData->Panel->Position = glm::vec2(0.0f);
            }
        }

        if (ownerData->Text && !ownerData->Text->WorldSpace) {
            if (ownerData->UIRect && ownerData->UIRect->AnchorToParent) {
                ownerData->UIRect->Offset = glm::vec2(0.0f);
            } else {
                ownerData->Text->AnchorEnabled = false;
                ownerData->Text->AnchorToParentUI = false;
                ownerData->Text->AnchorOffset = glm::vec2(0.0f);
                ownerData->Transform.Position.x = 0.0f;
                ownerData->Transform.Position.y = 0.0f;
            }
        }

        if (!ownerData->Panel && !ownerData->Text && ownerData->UIRect) {
            if (ownerData->UIRect->AnchorToParent) {
                ownerData->UIRect->Offset = glm::vec2(0.0f);
            } else {
                ownerData->Transform.Position.x = 0.0f;
                ownerData->Transform.Position.y = 0.0f;
            }
        }
    }

    void ApplyEntityUIRenderSpace(Scene& scene, EntityID ownerId, bool worldSpace)
    {
        auto* ownerData = scene.GetEntityData(ownerId);
        if (!ownerData)
            return;

        const EntityID effectiveCanvasEntityId = FindNearestCanvasEntity(scene, ownerId);
        auto* effectiveCanvasOwner = effectiveCanvasEntityId != INVALID_ENTITY_ID
            ? scene.GetEntityData(effectiveCanvasEntityId)
            : nullptr;
        CanvasComponent* inheritedCanvas = (effectiveCanvasOwner && effectiveCanvasOwner->Canvas && effectiveCanvasEntityId != ownerId)
            ? effectiveCanvasOwner->Canvas.get()
            : nullptr;

        if (!ownerData->Canvas) {
            const bool matchesInheritedSpace = inheritedCanvas
                && inheritedCanvas->Space == (worldSpace
                    ? CanvasComponent::RenderSpace::WorldSpace
                    : CanvasComponent::RenderSpace::ScreenSpace);
            if (!worldSpace && !inheritedCanvas)
                return;
            if (matchesInheritedSpace) {
                if (ownerData->Text && ownerData->Text->WorldSpace) {
                    ownerData->Text->WorldSpace = false;
                    scene.MarkDirty();
                    scene.MarkTransformDirty(ownerId);
                }
                return;
            }
        }

        const bool createdCanvas = ownerData->Canvas == nullptr;
        CanvasComponent* localCanvas = EnsureLocalUICanvas(scene, ownerId, ownerData, inheritedCanvas);
        if (!localCanvas)
            return;

        const bool wasWorldSpace = localCanvas->Space == CanvasComponent::RenderSpace::WorldSpace;

        localCanvas->Space = worldSpace
            ? CanvasComponent::RenderSpace::WorldSpace
            : CanvasComponent::RenderSpace::ScreenSpace;
        if (createdCanvas && worldSpace)
            localCanvas->Billboard = true;

        if (ownerData->Text)
            ownerData->Text->WorldSpace = false;
        if (worldSpace && !wasWorldSpace)
            NormalizeWorldSpaceUIRootLayout(ownerData);
        if (worldSpace)
            EnsureWorldSpaceUICanvasSize(ownerData);

        scene.MarkDirty();
        scene.MarkTransformDirty(ownerId);
    }

    void ApplyEntityUIBillboard(Scene& scene, EntityID ownerId, bool billboard)
    {
        auto* ownerData = scene.GetEntityData(ownerId);
        if (!ownerData)
            return;

        const EntityID effectiveCanvasEntityId = FindNearestCanvasEntity(scene, ownerId);
        auto* effectiveCanvasOwner = effectiveCanvasEntityId != INVALID_ENTITY_ID
            ? scene.GetEntityData(effectiveCanvasEntityId)
            : nullptr;
        CanvasComponent* inheritedCanvas = (effectiveCanvasOwner && effectiveCanvasOwner->Canvas && effectiveCanvasEntityId != ownerId)
            ? effectiveCanvasOwner->Canvas.get()
            : nullptr;

        if (!ownerData->Canvas) {
            const bool matchesInheritedBillboard = inheritedCanvas
                && inheritedCanvas->Space == CanvasComponent::RenderSpace::WorldSpace
                && inheritedCanvas->Billboard == billboard;
            if (matchesInheritedBillboard)
                return;
        }

        CanvasComponent* localCanvas = EnsureLocalUICanvas(scene, ownerId, ownerData, inheritedCanvas);
        if (!localCanvas)
            return;

        localCanvas->Space = CanvasComponent::RenderSpace::WorldSpace;
        localCanvas->Billboard = billboard;
        if (ownerData->Text)
            ownerData->Text->WorldSpace = false;
        EnsureWorldSpaceUICanvasSize(ownerData);

        scene.MarkDirty();
        scene.MarkTransformDirty(ownerId);
    }

    bool ShouldDrawUITextRenderSpaceControls(const TextRendererComponent& text)
    {
        return !text.WorldSpace || TextUsesCanvasBackedUI(text);
    }

    void DrawUIWorldSpaceControls()
    {
        Scene* scene = GetCurrentComponentDrawScene();
        const EntityID ownerId = GetCurrentComponentDrawEntity();
        if (!scene || ownerId == INVALID_ENTITY_ID)
            return;

        auto* ownerData = scene->GetEntityData(ownerId);
        if (!ownerData)
            return;

        EntityID effectiveCanvasEntityId = INVALID_ENTITY_ID;
        CanvasComponent* effectiveCanvas = nullptr;
        auto refreshCanvasState = [&]() {
            effectiveCanvasEntityId = FindNearestCanvasEntity(*scene, ownerId);
            auto* effectiveCanvasOwner = effectiveCanvasEntityId != INVALID_ENTITY_ID
                ? scene->GetEntityData(effectiveCanvasEntityId)
                : nullptr;
            effectiveCanvas = (effectiveCanvasOwner && effectiveCanvasOwner->Canvas)
                ? effectiveCanvasOwner->Canvas.get()
                : nullptr;
        };
        refreshCanvasState();

        bool worldSpace = effectiveCanvas
            && effectiveCanvas->Space == CanvasComponent::RenderSpace::WorldSpace;

        ImGui::Separator();
        ImGui::TextDisabled("Render Space");
        if (ImGui::Checkbox("World Space", &worldSpace)) {
            ApplyEntityUIRenderSpace(*scene, ownerId, worldSpace);
            refreshCanvasState();
            worldSpace = effectiveCanvas
                && effectiveCanvas->Space == CanvasComponent::RenderSpace::WorldSpace;
        }

        if (worldSpace) {
            bool billboard = effectiveCanvas ? effectiveCanvas->Billboard : true;
            if (ImGui::Checkbox("Billboard", &billboard)) {
                ApplyEntityUIBillboard(*scene, ownerId, billboard);
                refreshCanvasState();
            }
        }

        if (effectiveCanvas && effectiveCanvasEntityId != ownerId && !ownerData->Canvas) {
            ImGui::TextDisabled("Using ancestor Canvas until overridden on this entity");
        }
    }
}

inline void RegisterComponentDrawers() {
    auto& registry = ComponentDrawerRegistry::Instance();

    registry.Register<TransformComponent>("Transform", [](TransformComponent& t) {
        ImGui::ClayInspectorContentScope scope("TransformGrid");
        bool dirty = false;

        glm::vec3 pos = t.Position;
        if (ImGui::ClayFieldVec(scope, "Position", &pos.x, 3, 0.1f)) {
            t.Position = pos;
            dirty = true;
        }

        glm::vec3 euler = t.Rotation;
        if (ImGui::ClayFieldVec(scope, "Rotation", &euler.x, 3, 0.1f)) {
            t.Rotation = euler;
            if (t.UseQuatRotation) {
                glm::mat4 r = glm::yawPitchRoll(glm::radians(t.Rotation.y), glm::radians(t.Rotation.x), glm::radians(t.Rotation.z));
                t.RotationQ = glm::normalize(glm::quat_cast(r));
            }
            dirty = true;
        }

        bool useQuat = t.UseQuatRotation;
        if (ImGui::ClayFieldCheckbox(scope, "Quaternion Mode", &useQuat)) {
            if (useQuat && !t.UseQuatRotation) {
                glm::mat4 r = glm::yawPitchRoll(glm::radians(t.Rotation.y), glm::radians(t.Rotation.x), glm::radians(t.Rotation.z));
                t.RotationQ = glm::normalize(glm::quat_cast(r));
            } else if (!useQuat && t.UseQuatRotation) {
                glm::vec3 eulerDeg = glm::degrees(glm::eulerAngles(glm::normalize(t.RotationQ)));
                t.Rotation = eulerDeg;
            }
            t.UseQuatRotation = useQuat;
            dirty = true;
        }

        static bool s_LinkScale = false;
        glm::vec3 scale = t.Scale;
        bool scaleChanged = ImGui::ClayFieldVec(scope, "Scale", &scale.x, 3, 0.1f);
        if (scaleChanged) {
            if (s_LinkScale) {
                glm::vec3 before = t.Scale;
                glm::vec3 ratios(
                    before.x != 0.0f ? scale.x / before.x : 1.0f,
                    before.y != 0.0f ? scale.y / before.y : 1.0f,
                    before.z != 0.0f ? scale.z / before.z : 1.0f);
                float applied = ratios.x;
                if (fabsf(scale.y - before.y) > 1e-4f) applied = ratios.y;
                else if (fabsf(scale.z - before.z) > 1e-4f) applied = ratios.z;
                scale.x = before.x * applied;
                scale.y = before.y * applied;
                scale.z = before.z * applied;
            }
            t.Scale = scale;
            dirty = true;
        }

        scope.BeginRow("Scale Tools");
        ImGui::Checkbox("Link Axes", &s_LinkScale);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Scale")) {
            t.Scale = glm::vec3(1.0f);
            dirty = true;
        }
        scope.EndRow();

        if (dirty)
            NotifyCurrentTransformChanged(t);
    });

    registry.Register<MeshComponent>("Mesh", [](MeshComponent& m) {
        ImGui::Text("Mesh Name: %s", m.MeshName.c_str());
        if (!m.material && m.materials.empty()) return;
        auto drawTexSlot = [&](const char* label, bgfx::TextureHandle& tex) {
                ImGui::Separator();
                ImGui::Text("%s", label);
                ImGui::PushID(label);
                ImTextureID texId = TextureLoader::ToImGuiTextureID(tex);
                ImVec2 size(64, 64);
                bool requestPicker = false;
                // Bordered thumbnail box, even if empty
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f,0.35f,0.38f,1));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                if (texId) {
                    ImGui::Image(texId, size, ImVec2(0,0), ImVec2(1,1));
                } else {
                    if (ImGui::Button("##texslot", size)) {
                        requestPicker = true;
                    }
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                // Drag-drop target for texture assignment (no yellow outline)
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                        const char* path = (const char*)payload->Data;
                        std::string ext = std::filesystem::path(path).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") {
                            TextureSpecifier spec;
                            spec.Path = path;
                            bgfx::TextureHandle newTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                            if (bgfx::isValid(newTex)) {
                                tex = newTex;
                            }
                        }
                    }
                    if (ImGui::IsDragDropPayloadBeingAccepted()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    }
                    ImGui::EndDragDropTarget();
                }
                if (requestPicker) {
                    ImGui::OpenPopup("TexturePicker");
                }
                texturepicker::DrawTexturePickerPopup("TexturePicker",
                    [&](const std::string& path) {
                        TextureSpecifier spec;
                        spec.Path = path;
                        bgfx::TextureHandle newTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                        if (bgfx::isValid(newTex)) {
                            tex = newTex;
                        }
                    });
                ImGui::PopID();
            };

        auto drawEditableTextureSlot = [&](const char* label,
                                           const std::string& currentPath,
                                           bgfx::TextureHandle handle,
                                           const std::function<void(const std::string&)>& assignPath) {
            ImGui::Separator();
            ImGui::Text("%s", label);
            if (!currentPath.empty()) ImGui::TextDisabled("%s", currentPath.c_str());
            else ImGui::TextDisabled("Drag a texture from Project");
            ImGui::PushID(label);
            ImTextureID texId = TextureLoader::ToImGuiTextureID(handle);
            ImVec2 size(64,64);
            bool requestPicker = false;
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f,0.35f,0.38f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            if (texId) {
                ImGui::Image(texId, size, ImVec2(0,0), ImVec2(1,1));
            } else {
                if (ImGui::Button("##texpreview", size)) {
                    requestPicker = true;
                }
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    const char* path = static_cast<const char*>(payload->Data);
                    if (path) {
                        std::string ext = std::filesystem::path(path).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") {
                            assignPath(std::string(path));
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (requestPicker) {
                ImGui::OpenPopup("TexturePicker");
            }
            texturepicker::DrawTexturePickerPopup("TexturePicker", assignPath, currentPath);
            if (ImGui::Button("Clear##texclear")) assignPath(std::string());
            ImGui::PopID();
        };

        auto drawPBRControls = [&](const std::shared_ptr<PBRMaterial>& pbr) {
            drawEditableTextureSlot("Albedo", pbr->GetAlbedoPath(), pbr->m_AlbedoTex,
                [pbr](const std::string& path){ pbr->SetAlbedoTextureFromPath(path); });
            drawEditableTextureSlot("MetallicRoughness", pbr->GetMetallicRoughnessPath(), pbr->m_MetallicRoughnessTex,
                [pbr](const std::string& path){ pbr->SetMetallicRoughnessTextureFromPath(path); });
            drawEditableTextureSlot("Normal", pbr->GetNormalPath(), pbr->m_NormalTex,
                [pbr](const std::string& path){ pbr->SetNormalTextureFromPath(path); });
            drawEditableTextureSlot("Ambient Occlusion", pbr->GetAOPath(), pbr->m_AOTex,
                [pbr](const std::string& path){ pbr->SetAmbientOcclusionTextureFromPath(path); });
            drawEditableTextureSlot("Emission", pbr->GetEmissionPath(), pbr->m_EmissionTex,
                [pbr](const std::string& path){ pbr->SetEmissionTextureFromPath(path); });
            drawEditableTextureSlot("Displacement", pbr->GetDisplacementPath(), pbr->m_DisplacementTex,
                [pbr](const std::string& path){ pbr->SetDisplacementTextureFromPath(path); });

            float metallic = pbr->GetMetallic();
            if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f, "%.2f")) pbr->SetMetallic(metallic);
            float roughness = pbr->GetRoughness();
            if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f, "%.2f")) pbr->SetRoughness(roughness);
            float ao = pbr->GetAmbientOcclusion();
            if (ImGui::SliderFloat("AO", &ao, 0.0f, 1.0f, "%.2f")) pbr->SetAmbientOcclusion(ao);
            float normalScale = pbr->GetNormalScale();
            if (ImGui::SliderFloat("Normal Strength", &normalScale, 0.0f, 4.0f, "%.2f")) pbr->SetNormalScale(normalScale);
            float displacementScale = pbr->GetDisplacementScale();
            if (ImGui::SliderFloat("Displacement Scale", &displacementScale, 0.0f, 1.0f, "%.3f")) pbr->SetDisplacementScale(displacementScale);
            float emissionStrength = pbr->GetEmissionStrength();
            if (ImGui::SliderFloat("Emission Strength", &emissionStrength, 0.0f, 5.0f, "%.2f")) pbr->SetEmissionStrength(emissionStrength);
            glm::vec3 emissionColor = pbr->GetEmissionColor();
            if (ImGui::ColorEdit3("Emission Color", &emissionColor.x)) pbr->SetEmissionColor(emissionColor);
            glm::vec2 uvScale = pbr->GetUVScale();
            if (ImGui::DragFloat2("UV Scale", &uvScale.x, 0.01f, 0.01f, 10.0f)) pbr->SetUVScale(uvScale);
            glm::vec2 uvOffset = pbr->GetUVOffset();
            if (ImGui::DragFloat2("UV Offset", &uvOffset.x, 0.01f)) pbr->SetUVOffset(uvOffset);

            bool receiveOverride = pbr->GetReceiveShadowsOverride();
            if (ImGui::Checkbox("Receive Shadows Override", &receiveOverride)) {
                pbr->SetReceiveShadowsOverride(receiveOverride);
            }
            if (receiveOverride) {
                bool receive = pbr->GetReceiveShadows();
                if (ImGui::Checkbox("Receive Shadows", &receive)) {
                    pbr->SetReceiveShadows(receive);
                }
            }
        };

        // Sorting / draw control
        ImGui::Checkbox("Render On Top", &m.RenderOnTop);
        ImGui::Checkbox("Show Backfaces (Two-Sided)", &m.ShowBackfaces);
        bool runtimeBindingDirty = false;
        runtimeBindingDirty |= ImGui::Checkbox("Skip Frustum Culling", &m.SkipFrustumCulling);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Disable CPU frustum culling for this mesh.\n"
                              "Useful for first-person arms that would otherwise be culled.\n\n"
                              "NOTE: For characters with multiple mesh parts (Body, Gauntlets, etc.),\n"
                              "enable this on EACH mesh that contains arm geometry.");
        }
        runtimeBindingDirty |= ImGui::DragFloat("Bounds Padding", &m.BoundsPadding, 0.01f, 1.0f, 5.0f, "%.2f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Multiplier for AABB bounds used in frustum culling.\n"
                              "1.0 = exact mesh bounds (default for static meshes)\n"
                              "1.5-2.0 = recommended for skinned/animated meshes\n\n"
                              "Increase if animated meshes get culled incorrectly.");
        }
        if (runtimeBindingDirty) {
            NotifyCurrentComponentChanged(cm::world::RuntimeDirtyBits::RenderBinding |
                                          cm::world::RuntimeDirtyBits::Bounds);
        }
        ImGui::DragInt("Render Priority", &m.RenderOrder, 1, -1000, 1000);

        // Draw only the selected slot (InspectorPanel provides the selection index)
        int selectedSlot = 0;
        if (ImGui::GetStateStorage()) {
            // Use an ImGui state key to retrieve current slot from InspectorPanel
            ImGuiID key = ImGui::GetID("SelectedMaterialSlot");
            selectedSlot = ImGui::GetStateStorage()->GetInt(key, 0);
        }
        if (!m.materials.empty() && (size_t)selectedSlot < m.materials.size()) {
            auto mat = m.materials[selectedSlot];
            ImGui::TextDisabled("%s", mat ? mat->GetName().c_str() : "<none>");
            // Only allow direct material editing when UniqueMaterial is true.
            // When false, materials are shared and users should use the Property Block
            // system in InspectorPanel to apply per-instance overrides.
            if (m.UniqueMaterial) {
                if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(mat)) {
                    drawPBRControls(pbr);
                }
            } else {
                ImGui::TextDisabled("(Use Material Overrides below for per-instance textures)");
            }
            ImGui::TextDisabled("Alpha mode overrides are in the Inspector panel.");
        } else if (m.material) {
            ImGui::TextDisabled("%s", m.material->GetName().c_str());
            // Only allow direct material editing when UniqueMaterial is true.
            // When false, materials are shared and users should use the Property Block
            // system in InspectorPanel to apply per-instance overrides.
            if (m.UniqueMaterial) {
                if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(m.material)) {
                    drawPBRControls(pbr);
                }
            } else {
                ImGui::TextDisabled("(Use Material Overrides below for per-instance textures)");
            }
            ImGui::TextDisabled("Alpha mode overrides are in the Inspector panel.");
        }
        // Blend shape sliders
        if (m.BlendShapes) {
            if (ImGui::CollapsingHeader("Blend Shapes")) {
                for (auto& shape : m.BlendShapes->Shapes) {
                    if (ImGui::SliderFloat(shape.Name.c_str(), &shape.Weight, 0.0f, 1.0f)) {
                        m.BlendShapes->Dirty = true;
                    }
                }
            }
        }
    });

    // Unified Morphs drawer
    registry.Register<UnifiedMorphComponent>("Unified Morphs", [](UnifiedMorphComponent& u) {
        if (u.Names.empty()) { ImGui::TextDisabled("No shared morphs detected."); return; }
        bool anyChanged = false;
        for (size_t i = 0; i < u.Names.size(); ++i) {
            if (i >= u.Weights.size()) u.Weights.resize(u.Names.size(), 0.0f);
            float w = u.Weights[i];
            if (ImGui::SliderFloat(u.Names[i].c_str(), &w, 0.0f, 1.0f)) {
                u.Weights[i] = w;
                anyChanged = true;
            }
        }
        if (anyChanged) {
            auto& scene = Scene::Get();
            for (auto meshId : u.MemberMeshes) {
                auto* d = scene.GetEntityData(meshId); if (!d || !d->BlendShapes) continue;
                for (auto& shape : d->BlendShapes->Shapes) {
                    for (size_t i = 0; i < u.Names.size(); ++i) {
                        if (shape.Name == u.Names[i]) { shape.Weight = u.Weights[i]; d->BlendShapes->Dirty = true; }
                    }
                }
            }
        }
    });

    // BoneAttachment drawer - attach entity to skeleton bone
    registry.Register<BoneAttachmentComponent>("Bone Attachment", [](BoneAttachmentComponent& ba) {
        auto& scene = Scene::Get();
        
        ImGui::Checkbox("Enabled", &ba.Enabled);
        
        // Find owner entity and nearby skeleton for bone selection
        EntityID ownerEntity = INVALID_ENTITY_ID;
        EntityData* ownerData = nullptr;
        SkeletonComponent* skeleton = nullptr;
        EntityID skeletonOwnerEntity = INVALID_ENTITY_ID;
        EntityID boneEntity = INVALID_ENTITY_ID;
        
        for (auto& entity : scene.GetEntities()) {
            auto* data = scene.GetEntityData(entity.GetID());
            if (data && data->BoneAttachment.get() == &ba) {
                ownerEntity = entity.GetID();
                ownerData = data;
                
                // If explicit skeleton entity is set, use it directly
                if (ba.SkeletonEntity != INVALID_ENTITY_ID) {
                    auto* skelData = scene.GetEntityData(ba.SkeletonEntity);
                    if (skelData && skelData->Skeleton) {
                        skeleton = skelData->Skeleton.get();
                        skeletonOwnerEntity = ba.SkeletonEntity;
                    }
                }
                
                // Strategy 1: Walk up parent hierarchy to find skeleton directly on an ancestor
                if (!skeleton) {
                    EntityID cur = data->Parent;
                    while (cur != INVALID_ENTITY_ID) {
                        auto* pd = scene.GetEntityData(cur);
                        if (!pd) break;
                        if (pd->Skeleton && !pd->Skeleton->BoneEntities.empty()) {
                            skeleton = pd->Skeleton.get();
                            skeletonOwnerEntity = cur;
                            break;
                        }
                        cur = pd->Parent;
                    }
                }
                
                // Strategy 2: Check direct siblings for skeleton
                if (!skeleton && data->Parent != INVALID_ENTITY_ID) {
                    auto* parentData = scene.GetEntityData(data->Parent);
                    if (parentData) {
                        for (EntityID siblingId : parentData->Children) {
                            if (siblingId == ownerEntity) continue;
                            auto* siblingData = scene.GetEntityData(siblingId);
                            if (siblingData && siblingData->Skeleton && !siblingData->Skeleton->BoneEntities.empty()) {
                                skeleton = siblingData->Skeleton.get();
                                skeletonOwnerEntity = siblingId;
                                break;
                            }
                        }
                    }
                }
                
                // Strategy 3: Walk up to find model roots, check siblings and their direct children
                // This handles: base_human/SkeletonRoot (sibling model root of) Sword01/Sword_84
                if (!skeleton) {
                    EntityID cur = data->Parent;
                    while (cur != INVALID_ENTITY_ID) {
                        auto* pd = scene.GetEntityData(cur);
                        if (!pd) break;
                        
                        bool isModelRoot = (pd->ModelAssetGuid.high != 0 || pd->ModelAssetGuid.low != 0);
                        if (isModelRoot && pd->Parent != INVALID_ENTITY_ID) {
                            auto* grandparentData = scene.GetEntityData(pd->Parent);
                            if (grandparentData) {
                                // Check each sibling of this model root
                                for (EntityID siblingId : grandparentData->Children) {
                                    if (siblingId == cur) continue;  // Skip self
                                    auto* siblingData = scene.GetEntityData(siblingId);
                                    if (!siblingData) continue;
                                    
                                    // Check if sibling itself has skeleton
                                    if (siblingData->Skeleton && !siblingData->Skeleton->BoneEntities.empty()) {
                                        skeleton = siblingData->Skeleton.get();
                                        skeletonOwnerEntity = siblingId;
                                        break;
                                    }
                                    
                                    // Check sibling's direct children for skeleton
                                    for (EntityID nephewId : siblingData->Children) {
                                        auto* nephewData = scene.GetEntityData(nephewId);
                                        if (nephewData && nephewData->Skeleton && !nephewData->Skeleton->BoneEntities.empty()) {
                                            skeleton = nephewData->Skeleton.get();
                                            skeletonOwnerEntity = nephewId;
                                            break;
                                        }
                                    }
                                    if (skeleton) break;
                                }
                            }
                            if (skeleton) break;
                        }
                        cur = pd->Parent;
                    }
                }
                
                // Also find the bone entity if resolved
                if (ba.ResolvedBoneEntity != INVALID_ENTITY_ID) {
                    boneEntity = ba.ResolvedBoneEntity;
                } else if (skeleton && !ba.TargetBoneName.empty()) {
                    int idx = skeleton->GetBoneIndex(ba.TargetBoneName);
                    if (idx >= 0 && (size_t)idx < skeleton->BoneEntities.size()) {
                        boneEntity = skeleton->BoneEntities[idx];
                    }
                }
                break;
            }
        }
        
        // Bone name dropdown
        if (skeleton && !skeleton->BoneNames.empty()) {
            int currentIndex = skeleton->GetBoneIndex(ba.TargetBoneName);
            if (currentIndex < 0) currentIndex = 0;
            
            std::string preview = ba.TargetBoneName.empty() ? "(Select Bone)" : ba.TargetBoneName;
            if (ImGui::BeginCombo("Target Bone", preview.c_str())) {
                for (size_t i = 0; i < skeleton->BoneNames.size(); ++i) {
                    bool selected = ((size_t)currentIndex == i);
                    if (ImGui::Selectable(skeleton->BoneNames[i].c_str(), selected)) {
                        ba.TargetBoneName = skeleton->BoneNames[i];
                        ba.InvalidateResolution();
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        } else {
            // Manual text input fallback
            char buf[128];
            std::strncpy(buf, ba.TargetBoneName.c_str(), sizeof(buf)-1);
            buf[sizeof(buf)-1] = '\0';
            if (ImGui::InputText("Target Bone", buf, sizeof(buf))) {
                ba.TargetBoneName = buf;
                ba.InvalidateResolution();
            }
            if (!skeleton) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "No skeleton found in hierarchy");
            }
        }
        
        ImGui::Separator();
        
        // Capture current transform button - computes offset from current entity world position to bone
        if (ownerData && boneEntity != INVALID_ENTITY_ID) {
            if (ImGui::Button("Capture Current Transform")) {
                auto* boneData = scene.GetEntityData(boneEntity);
                if (boneData) {
                    // Get bone's world transform (without scale if not inheriting)
                    glm::mat4 boneWorld = boneData->Transform.WorldMatrix;
                    
                    if (!ba.InheritScale) {
                        // Strip scale from bone matrix
                        glm::vec3 bonePos = glm::vec3(boneWorld[3]);
                        glm::mat3 boneRot = glm::mat3(boneWorld);
                        for (int i = 0; i < 3; ++i) {
                            float len = glm::length(glm::vec3(boneRot[i]));
                            if (len > 1e-6f) boneRot[i] /= len;
                        }
                        boneWorld = glm::mat4(boneRot);
                        boneWorld[3] = glm::vec4(bonePos, 1.0f);
                    }
                    
                    // Compute entity's transform relative to bone
                    glm::mat4 entityWorld = ownerData->Transform.WorldMatrix;
                    glm::mat4 invBoneWorld = glm::inverse(boneWorld);
                    glm::mat4 relativeTransform = invBoneWorld * entityWorld;
                    
                    // Decompose into position, rotation, scale
                    ba.LocalPosition = glm::vec3(relativeTransform[3]);
                    
                    // Extract rotation (as Euler angles)
                    glm::vec3 X = glm::vec3(relativeTransform[0]);
                    glm::vec3 Y = glm::vec3(relativeTransform[1]);
                    glm::vec3 Z = glm::vec3(relativeTransform[2]);
                    ba.LocalScale = glm::vec3(glm::length(X), glm::length(Y), glm::length(Z));
                    if (ba.LocalScale.x > 1e-6f) X /= ba.LocalScale.x;
                    if (ba.LocalScale.y > 1e-6f) Y /= ba.LocalScale.y;
                    if (ba.LocalScale.z > 1e-6f) Z /= ba.LocalScale.z;
                    glm::mat3 rotMat(X, Y, Z);
                    glm::quat q = glm::quat_cast(rotMat);
                    ba.LocalRotation = glm::degrees(glm::eulerAngles(q));
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Captures the entity's current world position\nrelative to the target bone.\nUse this after positioning your mesh in the editor.");
            }
        } else {
            ImGui::BeginDisabled();
            ImGui::Button("Capture Current Transform");
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Select a target bone first");
            }
        }
        
        ImGui::Separator();
        ImGui::Text("Local Offset (relative to bone):");
        
        float pos[3] = { ba.LocalPosition.x, ba.LocalPosition.y, ba.LocalPosition.z };
        if (ImGui::DragFloat3("Position##ba", pos, 0.01f)) {
            ba.LocalPosition = glm::vec3(pos[0], pos[1], pos[2]);
        }
        
        float rot[3] = { ba.LocalRotation.x, ba.LocalRotation.y, ba.LocalRotation.z };
        if (ImGui::DragFloat3("Rotation##ba", rot, 1.0f)) {
            ba.LocalRotation = glm::vec3(rot[0], rot[1], rot[2]);
        }
        
        float scale[3] = { ba.LocalScale.x, ba.LocalScale.y, ba.LocalScale.z };
        if (ImGui::DragFloat3("Scale##ba", scale, 0.01f)) {
            ba.LocalScale = glm::vec3(scale[0], scale[1], scale[2]);
        }
        
        ImGui::Separator();
        ImGui::Text("Bone Transform Inheritance:");
        if (ImGui::Checkbox("Follow Bone Rotation", &ba.InheritRotation)) {
            // Default should be ON for typical use case
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled, the entity rotates WITH the bone.\nWhen disabled, entity keeps its own orientation.");
        }
        ImGui::Checkbox("Follow Bone Scale", &ba.InheritScale);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Usually disabled - bone scales can be weird.\nOnly enable if you need scale inheritance.");
        }
        
        // Show resolution status
        ImGui::Separator();
        if (ba.ResolvedBoneEntity != INVALID_ENTITY_ID) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Attached to bone (ID: %d)", (int)ba.ResolvedBoneEntity);
        } else if (ba.ResolutionAttempted) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed to resolve bone '%s'", ba.TargetBoneName.c_str());
        } else if (!ba.TargetBoneName.empty()) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Will resolve at runtime");
        }
    });

    // TintMaskController drawer - controls tints on child meshes with slot selection
    registry.Register<TintMaskController>("Tint Controller", [](TintMaskController& t) {
        auto& scene = Scene::Get();
        
        // Find the entity this component belongs to
        EntityID ownerEntity = (EntityID)-1;
        
        // Collect all child meshes with their slot counts
        struct MeshChildInfo {
            EntityID id;
            std::string name;
            int slotCount;
        };
        std::vector<MeshChildInfo> childMeshes;
        
        for (auto& entity : scene.GetEntities()) {
            auto* data = scene.GetEntityData(entity.GetID());
            if (data && data->TintController.get() == &t) {
                ownerEntity = entity.GetID();
                
                // Recursively collect child entities with meshes
                std::function<void(EntityID)> collectChildren = [&](EntityID parentId) {
                    auto* parentData = scene.GetEntityData(parentId);
                    if (!parentData) return;
                    
                    for (EntityID childId : parentData->Children) {
                        auto* childData = scene.GetEntityData(childId);
                        if (!childData) continue;
                        
                        if (childData->Mesh) {
                            int slots = std::max(1, static_cast<int>(childData->Mesh->materials.size()));
                            childMeshes.push_back({childId, childData->Name, slots});
                        }
                        collectChildren(childId);
                    }
                };
                collectChildren(ownerEntity);
                break;
            }
        }
        
        // Helper to check if a target exists for entity+slot
        auto findTarget = [&](EntityID entityId, int slot) -> TintTarget* {
            for (auto& target : t.Targets) {
                if (target.TargetEntity == entityId && target.MaterialSlot == slot) {
                    return &target;
                }
            }
            return nullptr;
        };

        auto findExactOrWildcardTarget = [&](EntityID entityId, int slot) -> TintTarget* {
            if (TintTarget* exact = findTarget(entityId, slot)) {
                return exact;
            }
            return findTarget(entityId, -1);
        };
        
        // Helper to add/remove targets
        auto toggleSlot = [&](EntityID entityId, int slot, bool enable) {
            if (enable) {
                if (!findTarget(entityId, slot)) {
                    TintTarget newTarget;
                    newTarget.TargetEntity = entityId;
                    newTarget.MaterialSlot = slot;
                    newTarget.BlendMode = TintBlendMode::Normal;
                    t.Targets.push_back(newTarget);
                }
            } else {
                TintTarget* wildcard = findTarget(entityId, -1);
                if (wildcard) {
                    int slotCount = 0;
                    for (const auto& meshInfo : childMeshes) {
                        if (meshInfo.id == entityId) {
                            slotCount = meshInfo.slotCount;
                            break;
                        }
                    }

                    TintTarget wildcardCopy = *wildcard;
                    t.Targets.erase(
                        std::remove_if(t.Targets.begin(), t.Targets.end(),
                            [entityId](const TintTarget& tgt) {
                                return tgt.TargetEntity == entityId && tgt.MaterialSlot == -1;
                            }),
                        t.Targets.end());

                    for (int s = 0; s < slotCount; ++s) {
                        if (s == slot) {
                            continue;
                        }

                        TintTarget cloned = wildcardCopy;
                        cloned.MaterialSlot = s;
                        t.Targets.push_back(cloned);
                    }
                    return;
                }

                t.Targets.erase(
                    std::remove_if(t.Targets.begin(), t.Targets.end(),
                        [entityId, slot](const TintTarget& tgt) {
                            return tgt.TargetEntity == entityId && tgt.MaterialSlot == slot;
                        }),
                    t.Targets.end());
            }
        };
        
        // Tint color settings
        if (ImGui::Checkbox("Use Tint Mask Mode", &t.UseTintMask)) {
            t.MarkDirty();
        }
        
        if (t.UseTintMask) {
            ImGui::TextDisabled("Multi-channel tint mask colors:");
            if (ImGui::ColorEdit4("Channel R", &t.TintColor0.x, ImGuiColorEditFlags_NoInputs)) t.MarkDirty();
            if (ImGui::ColorEdit4("Channel G", &t.TintColor1.x, ImGuiColorEditFlags_NoInputs)) t.MarkDirty();
            if (ImGui::ColorEdit4("Channel B", &t.TintColor2.x, ImGuiColorEditFlags_NoInputs)) t.MarkDirty();
            if (ImGui::ColorEdit4("Channel A", &t.TintColor3.x, ImGuiColorEditFlags_NoInputs)) t.MarkDirty();
        } else {
            if (ImGui::ColorEdit4("Tint Color", &t.BaseTint.x)) t.MarkDirty();
        }
        
        ImGui::Separator();
        ImGui::TextDisabled("PBR Overrides");
        if (ImGui::Checkbox("Override PBR Scalars", &t.UsePbrOverrides)) {
            t.MarkDirty();
        }
        if (t.UsePbrOverrides) {
            if (ImGui::SliderFloat("Metallic", &t.OverrideMetallic, 0.0f, 1.0f, "%.2f")) t.MarkDirty();
            if (ImGui::SliderFloat("Roughness", &t.OverrideRoughness, 0.0f, 1.0f, "%.2f")) t.MarkDirty();
            if (ImGui::SliderFloat("Emission Strength", &t.OverrideEmissionStrength, 0.0f, 5.0f, "%.2f")) t.MarkDirty();
            if (ImGui::ColorEdit3("Emission Color", &t.OverrideEmissionColor.x, ImGuiColorEditFlags_NoInputs)) t.MarkDirty();
        }
        
        ImGui::Separator();
        ImGui::TextDisabled("Mesh Targets");
        
        if (childMeshes.empty()) {
            ImGui::TextDisabled("  No child meshes found");
        }
        
        const char* blendModes[] = { "Normal", "Multiply", "Overlay", "Add", "Screen", "Soft Light", "Color Dodge", "Color Burn", "Difference", "Detail" };
        
        // Draw each mesh as a tree node with slot checkboxes
        for (const auto& meshInfo : childMeshes) {
            ImGui::PushID(static_cast<int>(meshInfo.id));
            
            // Count how many slots are enabled for this mesh
            int enabledSlots = 0;
            for (int s = 0; s < meshInfo.slotCount; ++s) {
                if (findExactOrWildcardTarget(meshInfo.id, s)) enabledSlots++;
            }
            
            // Mesh header with summary
            std::string headerLabel = meshInfo.name;
            if (enabledSlots > 0) {
                headerLabel += "  (" + std::to_string(enabledSlots) + "/" + std::to_string(meshInfo.slotCount) + " slots)";
            }
            
            bool meshOpen = ImGui::TreeNodeEx("##mesh", 
                ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_AllowOverlap,
                "%s", headerLabel.c_str());
            
            if (meshOpen) {
                ImGui::Indent(8.0f);
                
                // Draw each slot as a selectable row
                for (int s = 0; s < meshInfo.slotCount; ++s) {
                    ImGui::PushID(s);
                    
                    TintTarget* existingTarget = findExactOrWildcardTarget(meshInfo.id, s);
                    bool isEnabled = existingTarget != nullptr;
                    
                    // Slot checkbox
                    std::string slotLabel = "Slot " + std::to_string(s);
                    if (ImGui::Checkbox(slotLabel.c_str(), &isEnabled)) {
                        toggleSlot(meshInfo.id, s, isEnabled);
                        t.MarkDirty();
                    }
                    
                    // If enabled, show per-slot settings on same row
                    if (existingTarget) {
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(100.0f);
                        int modeIdx = static_cast<int>(existingTarget->BlendMode);
                        if (ImGui::Combo("##blend", &modeIdx, blendModes, IM_ARRAYSIZE(blendModes))) {
                            existingTarget->BlendMode = static_cast<TintBlendMode>(modeIdx);
                            t.MarkDirty();
                        }
                        
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Custom##color", &existingTarget->UseTargetColor)) {
                            t.MarkDirty();
                        }
                        
                        if (existingTarget->UseTargetColor) {
                            ImGui::SameLine();
                            ImGui::SetNextItemWidth(120.0f);
                            if (ImGui::ColorEdit4("##tint", &existingTarget->Color.x,
                                ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
                                t.MarkDirty();
                            }
                        }
                    }
                    
                    ImGui::PopID();
                }
                
                // Quick actions for this mesh
                ImGui::Spacing();
                if (ImGui::SmallButton("Enable All")) {
                    for (int s = 0; s < meshInfo.slotCount; ++s) {
                        toggleSlot(meshInfo.id, s, true);
                    }
                    t.MarkDirty();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Disable All")) {
                    for (int s = 0; s < meshInfo.slotCount; ++s) {
                        toggleSlot(meshInfo.id, s, false);
                    }
                    t.MarkDirty();
                }
                
                ImGui::Unindent(8.0f);
                ImGui::TreePop();
            }
            
            ImGui::PopID();
        }
        
        // Apply tints to all enabled targets
        for (const TintTarget& target : t.Targets) {
            auto* targetData = scene.GetEntityData(target.TargetEntity);
            if (!targetData || !targetData->Mesh) continue;
            
            auto* mesh = targetData->Mesh.get();
            if (mesh->SlotPropertyBlocks.size() < mesh->materials.size()) {
                mesh->SlotPropertyBlocks.resize(mesh->materials.size());
            }
            
            // Apply to the specific slot
            if (target.MaterialSlot >= 0 && target.MaterialSlot < static_cast<int>(mesh->materials.size())) {
                auto& pb = mesh->SlotPropertyBlocks[target.MaterialSlot];
                
                float blendModeValue = static_cast<float>(static_cast<int>(target.BlendMode));
                pb.SetVector("u_TintParams", glm::vec4(blendModeValue, 0.5f, 0.0f, 0.0f));
                
                glm::vec4 tintColor = target.UseTargetColor ? target.Color : t.BaseTint;
                
                if (t.UseTintMask) {
                    pb.SetVector("u_TintColor0", t.TintColor0);
                    pb.SetVector("u_TintColor1", t.TintColor1);
                    pb.SetVector("u_TintColor2", t.TintColor2);
                    pb.SetVector("u_TintColor3", t.TintColor3);
                } else {
                    pb.SetVector("u_ColorTint", tintColor);
                }
            }
        }
    });

    registry.Register<LightComponent>("Light", [](LightComponent& l) {
        int type = static_cast<int>(l.Type);
        const char* types[] = { "Directional", "Point" };
        bool dirty = false;
        if (ImGui::Combo("Type", &type, types, IM_ARRAYSIZE(types))) {
            l.Type = static_cast<LightType>(type);
            dirty = true;
        }

        dirty |= ImGui::ColorEdit3("Color", &l.Color.x);
        dirty |= ImGui::DragFloat("Intensity", &l.Intensity, 0.05f, 0.0f, 100.0f);
        if (l.Type == LightType::Point) {
            dirty |= ImGui::Checkbox("Cast Shadows (Opt-in)", &l.PointShadowsEnabled);
        }
        if (dirty) {
            NotifyCurrentComponentChanged(cm::world::RuntimeDirtyBits::Light);
        }
        });

    registry.Register<ColliderComponent>("Collider", [](ColliderComponent& c) {
        // Shape type dropdown
        int shapeType = static_cast<int>(c.ShapeType);
        const char* shapeTypes[] = { "Box", "Capsule", "Sphere", "Mesh" };
        if (ImGui::Combo("Shape Type", &shapeType, shapeTypes, IM_ARRAYSIZE(shapeTypes))) {
            c.ShapeType = static_cast<ColliderShape>(shapeType);
        }

        // Physics layer
        DrawPhysicsLayerCombo("Physics Layer##ColliderPhysicsLayer", c.PhysicsLayer, c.PhysicsLayerName);

        // Common properties
        ImGui::DragFloat3("Offset", &c.Offset.x, 0.1f);
        ImGui::Checkbox("Is Trigger", &c.IsTrigger);

        // Shape-specific properties
        switch (c.ShapeType) {
            case ColliderShape::Box:
                ImGui::DragFloat3("Size", &c.Size.x, 0.1f, 0.01f, 100.0f);
                break;
            case ColliderShape::Capsule:
                ImGui::DragFloat("Radius", &c.Radius, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("Height", &c.Height, 0.01f, 0.01f, 20.0f);
                break;
            case ColliderShape::Sphere:
                ImGui::DragFloat("Radius", &c.Radius, 0.01f, 0.01f, 100.0f);
                break;
            case ColliderShape::Mesh:
                ImGui::Text("Mesh Path: %s", c.MeshPath.empty() ? "(None)" : c.MeshPath.c_str());
                // TODO: Add mesh path selection
                break;
        }
        });

    // Area3D drawer
    registry.Register<cm::physics::AreaComponent>("Area", [](cm::physics::AreaComponent& a) {
        ImGui::Checkbox("Enabled", &a.Enabled);
        ImGui::Checkbox("Monitor Bodies", &a.MonitorBodies);
        ImGui::Checkbox("Monitor Areas", &a.MonitorAreas);
        // Shape selection similar to Collider
        int shapeType = static_cast<int>(a.ShapeType);
        const char* shapeTypes[] = { "Box", "Capsule", "Sphere" };
        if (ImGui::Combo("Shape Type", &shapeType, shapeTypes, IM_ARRAYSIZE(shapeTypes))) {
            a.ShapeType = static_cast<cm::physics::AreaShapeType>(shapeType);
        }
        ImGui::DragFloat3("Offset", &a.Offset.x, 0.1f);
        if (a.ShapeType == cm::physics::AreaShapeType::Box) {
            ImGui::DragFloat3("Size", &a.Size.x, 0.1f, 0.01f, 100.0f);
        } else if (a.ShapeType == cm::physics::AreaShapeType::Capsule) {
            ImGui::DragFloat("Radius", &a.Radius, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("Height", &a.Height, 0.01f, 0.01f, 20.0f);
        } else { // Sphere
            ImGui::DragFloat("Radius", &a.Radius, 0.01f, 0.01f, 100.0f);
        }
        ImGui::InputScalar("Collision Layer", ImGuiDataType_U32, &a.CollisionLayer);
        ImGui::InputScalar("Collision Mask", ImGuiDataType_U32, &a.CollisionMask);
        int effects = (int)(uint8_t)a.Effects;
        if (ImGui::InputInt("Effects (bitmask)", &effects)) a.Effects = (cm::physics::AreaSpaceEffect)(effects & 0xFF);
        ImGui::DragFloat("Gravity Override", &a.GravityOverride, 0.1f, -100.0f, 100.0f);
        ImGui::DragFloat("Linear Damp", &a.LinearDamp, 0.01f, 0.0f, 100.0f);
        ImGui::DragFloat("Angular Damp", &a.AngularDamp, 0.01f, 0.0f, 100.0f);
        ImGui::InputInt("Priority", &a.Priority);
    });

#if 0
    registry.Register<ParticleSystemComponent>("ParticleSystem", [](ParticleSystemComponent& ps) {
        ImGui::DragFloat("Emission Rate", &ps.EmissionRate, 1.0f, 0.0f, 100000.0f);
        ImGui::DragFloat2("Lifetime Min/Max", &ps.LifetimeMin, 0.1f, 0.0f, 10.0f);
        ImGui::DragFloat3("Gravity", &ps.Gravity.x, 0.1f, -100.0f, 100.0f);
        ImGui::DragFloat("Start Size", &ps.StartSize, 0.01f, 0.0f, 100.0f);
        ImGui::DragFloat("End Size", &ps.EndSize, 0.01f, 0.0f, 100.0f);
        ImGui::ColorEdit4("Start Color", &ps.StartColor.x);
        ImGui::ColorEdit4("End Color", &ps.EndColor.x);

        ImGui::Separator();
        ImGui::Text("Particle Texture");
        ImGui::SameLine();
        ImTextureID texId = TextureLoader::ToImGuiTextureID(ps.Texture);
        ImVec2 size(64, 64);
        ImGui::Image(texId, size);

        // Drag-drop target for assigning texture
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                const char* path = (const char*)payload->Data;
                std::string ext = std::filesystem::path(path).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                    TextureSpecifier spec;
                    spec.Path = path;
                    bgfx::TextureHandle newTex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                    if (bgfx::isValid(newTex)) {
                        ps.Texture = newTex;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    });

    // (disabled legacy block)
#endif

    // Navigation: NavMeshComponent inspector (Recast-only)
    registry.Register<nav::NavMeshComponent>("Nav Mesh", [](nav::NavMeshComponent& n) {
        ImGui::Checkbox("Enabled", &n.Enabled);

        ImGui::Separator();
        ImGui::Text("Recast Agent + Voxel");
        ImGui::DragFloat("Cell Size", &n.Bake.cellSize, 0.01f, 0.02f, 2.0f);
        ImGui::DragFloat("Cell Height", &n.Bake.cellHeight, 0.01f, 0.02f, 2.0f);
        ImGui::DragFloat("Agent Radius", &n.Bake.agentRadius, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Agent Height", &n.Bake.agentHeight, 0.01f, 0.0f, 6.0f);
        ImGui::DragFloat("Agent Max Climb", &n.Bake.agentMaxClimb, 0.01f, 0.0f, 4.0f);
        ImGui::DragFloat("Agent Max Slope", &n.Bake.agentMaxSlopeDeg, 0.1f, 0.0f, 89.0f, "%.1f deg");

        ImGui::Separator();
        ImGui::Text("Recast Regions");
        ImGui::DragFloat("Region Min Size", &n.Bake.regionMinSize, 0.1f, 0.0f, 512.0f);
        ImGui::DragFloat("Region Merge Size", &n.Bake.regionMergeSize, 0.1f, 0.0f, 2048.0f);

        ImGui::Separator();
        ImGui::Text("Recast Simplification");
        ImGui::DragFloat("Edge Max Length", &n.Bake.edgeMaxLen, 0.1f, 0.0f, 2048.0f);
        ImGui::DragFloat("Edge Max Error", &n.Bake.edgeMaxError, 0.01f, 0.01f, 16.0f);
        ImGui::DragFloat("Verts Per Poly", &n.Bake.vertsPerPoly, 1.0f, 3.0f, 6.0f);

        ImGui::Separator();
        ImGui::Text("Recast Detail Mesh");
        ImGui::DragFloat("Detail Sample Dist", &n.Bake.detailSampleDist, 0.1f, 0.0f, 64.0f);
        ImGui::DragFloat("Detail Max Error", &n.Bake.detailSampleMaxError, 0.01f, 0.0f, 16.0f);
        int step = static_cast<int>(n.TerrainSampleStep);
        if (ImGui::SliderInt("Terrain Sample Step", &step, 1, 8, "%d")) {
            n.TerrainSampleStep = static_cast<uint32_t>(std::max(1, step));
        }

        ImGui::Separator();
        ImGui::Text("Geometry Filtering");
        ImGui::Checkbox("Enable Include Name Regex", &n.GeometryIncludeRegexEnabled);
        if (n.GeometryIncludeRegexEnabled) {
            char regexBuf[512];
            std::snprintf(regexBuf, sizeof(regexBuf), "%s", n.GeometryIncludeRegexPattern.c_str());
            if (ImGui::InputText("Name Regex Pattern", regexBuf, sizeof(regexBuf))) {
                n.GeometryIncludeRegexPattern = regexBuf;
            }
            ImGui::TextDisabled("Only matching entity names are included in bake.");
        }

        ImGui::Separator();
        ImGui::Text("Chunked Output");
        ImGui::Checkbox("Chunked Nav Streaming", &n.ChunkedNavEnabled);
        ImGui::Checkbox("Bake Local View Only", &n.BakeVisibleChunksOnly);
        int visiblePad = static_cast<int>(n.BakeVisibleChunkPadding);
        if (ImGui::SliderInt("Local View Padding", &visiblePad, 0, 8)) {
            n.BakeVisibleChunkPadding = static_cast<uint32_t>(std::max(0, visiblePad));
        }
        ImGui::Checkbox("Fill Missing Chunks Only", &n.BakeMissingChunksOnly);
        if (n.ChunkedNavEnabled) {
            n.ChunkingMode = nav::NavChunkingMode::WorldGrid;
            ImGui::DragFloat("Chunk World Size", &n.ChunkWorldSize, 1.0f, 4.0f, 4096.0f, "%.1f");
            int bakePad = static_cast<int>(n.ChunkBakePadding);
            if (ImGui::SliderInt("Chunk Bake Padding", &bakePad, 0, 8)) {
                n.ChunkBakePadding = static_cast<uint32_t>(std::max(0, bakePad));
            }
            ImGui::DragFloat("Chunk Stream Radius", &n.ChunkStreamRadius, 10.0f, 0.0f, 10000.0f, "%.1f");
            char packBuf[256];
            std::snprintf(packBuf, sizeof(packBuf), "%s", n.NavPackPath.c_str());
            if (ImGui::InputText("NavPack Path", packBuf, sizeof(packBuf))) {
                n.NavPackPath = packBuf;
            }
        }

        ImGui::Separator();
        ImGui::Text("Debug");
        if (ImGui::DragFloat("Draw Offset", &n.DebugDrawOffset, 0.01f, 0.0f, 1.0f)) {
            nav::debug::SetOffset(n.DebugDrawOffset);
        }
        ImGui::DragFloat("Agent Height Offset", &n.AgentPlacementOffset, 0.01f, -4.0f, 4.0f, "%.2f m");
        ImGui::TextDisabled("Positive values lower grounded nav agents below the navmesh surface.");
        static float sDrawDistance = nav::debug::GetDrawDistance();
        if (ImGui::DragFloat("Draw Distance", &sDrawDistance, 1.0f, 10.0f, 500.0f, "%.0f m")) {
            nav::debug::SetDrawDistance(sDrawDistance);
        }
        {
            auto mask = nav::Navigation::Get().GetDebugMask();
            bool drawCosts = (static_cast<uint32_t>(mask) & static_cast<uint32_t>(nav::NavDrawMask::Costs)) != 0;
            if (ImGui::Checkbox("Draw Costs", &drawCosts)) {
                if (drawCosts) {
                    mask = mask | nav::NavDrawMask::Costs | nav::NavDrawMask::Polys;
                } else {
                    mask = static_cast<nav::NavDrawMask>(static_cast<uint32_t>(mask) &
                                                         ~static_cast<uint32_t>(nav::NavDrawMask::Costs));
                }
                nav::Navigation::Get().SetDebugMask(mask);
            }
        }

        ImGui::Separator();
        size_t displayVerts = 0;
        size_t displayPolys = 0;
        size_t loadedChunkCount = 0;
        uint32_t detourTiles = 0;
        uint32_t detourPolys = 0;
        if (n.ChunkedNavEnabled && n.ChunkManager) {
            std::vector<nav::NavChunkManager::LoadedChunkRuntime> loadedChunks;
            n.ChunkManager->GetLoadedChunkRuntimes(loadedChunks);
            loadedChunkCount = loadedChunks.size();
            for (const auto& chunk : loadedChunks) {
                if (!chunk.runtime) continue;
                displayVerts += chunk.runtime->m_Vertices.size();
                displayPolys += chunk.runtime->m_Polys.size();
            }
        } else if (n.Runtime) {
            displayVerts = n.Runtime->m_Vertices.size();
            displayPolys = n.Runtime->m_Polys.size();
        }
        if (n.Runtime && n.Runtime->HasDetour()) {
            n.Runtime->GetDetourDebugStats(detourTiles, detourPolys);
            if (n.ChunkedNavEnabled) {
                if (displayPolys == 0 && detourPolys > 0) displayPolys = detourPolys;
                if (loadedChunkCount == 0 && detourTiles > 0) loadedChunkCount = detourTiles;
            }
        }

        if (!n.IsBaking()) {
            if (ImGui::Button("Bake NavMesh")) {
                nav::debug::SetOffset(n.DebugDrawOffset);
                n.RequestBake(Scene::Get());
                nav::Navigation::Get().SetDebugMask(nav::NavDrawMask::TriMesh | nav::NavDrawMask::Polys);
            }
            ImGui::SameLine();
            if (!n.Runtime && (!n.AssetPath.empty() || !n.NavPackPath.empty())) {
                n.EnsureRuntimeLoaded(Scene::Get());
            }
            if (n.Runtime) {
                if (n.ChunkedNavEnabled) {
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f),
                                       "Baked (chunked: %zu loaded tris, %zu loaded chunks)",
                                       displayPolys, loadedChunkCount);
                } else {
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "Baked (%zu tris)", displayPolys);
                }
            } else if (!n.AssetPath.empty()) {
                ImGui::TextColored(ImVec4(0.8f, 0.3f, 0.3f, 1.0f), "Not loaded: %s", n.AssetPath.c_str());
            }
        } else {
            if (ImGui::Button("Cancel")) n.CancelBake();
            ImGui::SameLine();
            ImGui::ProgressBar(n.BakeProgress(), ImVec2(-1, 0));
            const char* stageLabels[] = {
                "Idle",
                "Gathering geometry",
                "Building Recast/Detour",
                "Writing nav output",
                "Finalizing",
                "Done"
            };
            uint32_t stage = n.BakingStage.load();
            const size_t stageCount = sizeof(stageLabels) / sizeof(stageLabels[0]);
            ImGui::Text("Stage: %s", (stage < stageCount) ? stageLabels[stage] : "Unknown");
        }

        if (n.Runtime) {
            if (n.ChunkedNavEnabled) {
                ImGui::Text("Loaded Chunks: %zu", loadedChunkCount);
                if (detourTiles > 0 || detourPolys > 0) {
                    ImGui::Text("Detour Tiles: %u, Detour Polys: %u", detourTiles, detourPolys);
                }
            }
            ImGui::Text("Vertices: %zu, Polys: %zu", displayVerts, displayPolys);
            ImGui::Text("Cost Range: %.2f - %.2f", n.LastBakeCostMin.load(), n.LastBakeCostMax.load());
            ImGui::Text("Islands: %u", n.LastBakeIslands.load());
        }
        if (!n.AssetPath.empty()) {
            ImGui::TextDisabled("Path: %s", n.AssetPath.c_str());
        }
    });

    // Navigation: NavAgent inspector (enabled)
    registry.Register<nav::NavAgentComponent>("Nav Agent", [](nav::NavAgentComponent& a) {
        ImGui::Checkbox("Enabled", &a.Enabled);
        ImGui::DragFloat("Radius", &a.Params.radius, 0.01f, 0.1f, 2.0f);
        ImGui::DragFloat("Height", &a.Params.height, 0.01f, 0.5f, 3.0f);
        ImGui::DragFloat("Max Speed", &a.Params.maxSpeed, 0.1f, 0.1f, 20.0f);
        ImGui::DragFloat("Max Accel", &a.Params.maxAccel, 0.1f, 0.1f, 50.0f);
        ImGui::DragInt("Preferred Domain", &a.Params.preferredDomainId, 1.0f, -1024, 1024);
        ImGui::SetItemTooltip("Bias pathing toward navmeshes with this domain id (0 = none)");
        if (ImGui::Button("Stop")) a.Stop();
        
        // Status message: show if navmesh is found
        auto& scene = Scene::Get();
        if (a.NavMeshEntity != 0 && a.NavMeshEntity != INVALID_ENTITY_ID) {
            // Check if navmesh entity exists and has Navigation component
            auto* navData = scene.GetEntityData(a.NavMeshEntity);
            if (navData && navData->Navigation) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f)); // Green
                ImGui::Text("NavMesh: Found (Entity %d)", a.NavMeshEntity);
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f)); // Red
                ImGui::Text("NavMesh: Invalid reference (Entity %d)", a.NavMeshEntity);
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f)); // Red
            ImGui::Text("NavMesh: Not found (auto-binding will search)");
            ImGui::PopStyleColor();
        }
        
        // Diagnostic info for movement debugging
        ImGui::Separator();
        ImGui::Text("Status:");
        ImGui::Text("  HasDestination: %s", a.HasDestination ? "Yes" : "No");
        ImGui::Text("  HasPath: %s", a.HasPath() ? "Yes" : "No");
        if (a.HasPath()) {
            ImGui::Text("  Path waypoints: %zu", a.CurrentPath.points.size());
            ImGui::Text("  Path cursor: %zu", a.PathCursor);
        }
        ImGui::Text("  PathRequested: %s", a.PathRequested ? "Yes" : "No");
        ImGui::Text("  PathFailCount: %d", a.PathFailCount);
        if (a.PathFailCount > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f)); // Orange
            ImGui::Text("  WARNING: Path requests are failing!");
            ImGui::PopStyleColor();
        }
    });

    // Navigation: NavLink inspector
    registry.Register<nav::NavLinkComponent>("Nav Link", [](nav::NavLinkComponent& l) {
        ImGui::Checkbox("Enabled", &l.Enabled);
        ImGui::Checkbox("World Space", &l.UseWorldSpace);
        ImGui::DragFloat3("Start", &l.Start.x, 0.05f);
        ImGui::DragFloat3("End", &l.End.x, 0.05f);
        ImGui::DragFloat("Radius", &l.Radius, 0.01f, 0.0f, 5.0f, "%.2f");
        ImGui::DragFloat("Cost Multiplier", &l.Cost, 0.05f, 0.1f, 20.0f, "%.2f");
        ImGui::Checkbox("Bidirectional", &l.Bidirectional);
    });

    // Portal: cross-scene traversal links
    registry.Register<PortalComponent>("Portal", [](PortalComponent& p) {
        ImGui::Checkbox("Enabled", &p.Enabled);

        // World graph picker (optional)
        std::string targetLabel = "None";
        if (!p.TargetScenePath.empty()) {
            targetLabel = p.TargetScenePath + " :: ";
            if (!p.TargetPortalPath.empty()) {
                targetLabel += p.TargetPortalPath;
            } else if (p.TargetPortalGuid != ClaymoreGUID()) {
                targetLabel += p.TargetPortalGuid.ToString();
            } else {
                targetLabel += "<portal>";
            }
        }

        if (ImGui::BeginCombo("Target Portal (World Graph)", targetLabel.c_str())) {
            if (ImGui::Selectable("(Clear Target)", false)) {
                p.TargetScenePath.clear();
                p.TargetPortalGuid = ClaymoreGUID();
                p.TargetPortalPath.clear();
            }

            auto& graph = cm::world::WorldGraph::Get();
            if (!graph.IsLoaded()) {
                graph.LoadProjectGraph();
            }

            const auto& portals = graph.GetPortals();
            if (portals.empty()) {
                ImGui::TextDisabled("No baked world graph found.");
            } else {
                std::unordered_map<std::string, std::vector<const cm::world::WorldGraphPortal*>> byScene;
                byScene.reserve(portals.size());
                for (const auto& portal : portals) {
                    std::string sceneKey = portal.ScenePath.empty() ? std::string("(Unspecified Scene)") : portal.ScenePath;
                    byScene[sceneKey].push_back(&portal);
                }

                std::vector<std::string> sceneKeys;
                sceneKeys.reserve(byScene.size());
                for (const auto& kv : byScene) sceneKeys.push_back(kv.first);
                std::sort(sceneKeys.begin(), sceneKeys.end());

                for (const auto& sceneKey : sceneKeys) {
                    if (!ImGui::TreeNodeEx(sceneKey.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                    auto& scenePortals = byScene[sceneKey];
                    std::sort(scenePortals.begin(), scenePortals.end(), [](const cm::world::WorldGraphPortal* a, const cm::world::WorldGraphPortal* b) {
                        return a->PortalPath < b->PortalPath;
                    });

                    for (const auto* portal : scenePortals) {
                        std::string label = !portal->PortalPath.empty() ? portal->PortalPath : portal->PortalGuid.ToString();
                        if (label.empty()) label = "<portal>";
                        bool selected = (p.TargetScenePath == portal->ScenePath) &&
                                        (p.TargetPortalGuid == portal->PortalGuid) &&
                                        (p.TargetPortalPath == portal->PortalPath);
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            p.TargetScenePath = portal->ScenePath;
                            p.TargetPortalGuid = portal->PortalGuid;
                            p.TargetPortalPath = portal->PortalPath;
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::EndCombo();
        }

        // Target scene path
        char sceneBuf[512] = {};
        std::snprintf(sceneBuf, sizeof(sceneBuf), "%s", p.TargetScenePath.c_str());
        if (ImGui::InputText("Target Scene", sceneBuf, sizeof(sceneBuf))) {
            p.TargetScenePath = sceneBuf;
        }

        // Drag/drop scene from Project panel
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const char* path = static_cast<const char*>(payload->Data);
                if (path) {
                    std::string ext = std::filesystem::path(path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".scene") {
                        std::error_code ec;
                        std::filesystem::path rel = std::filesystem::relative(path, Project::GetProjectDirectory(), ec);
                        std::string vpath = ec ? std::filesystem::path(path).string() : rel.string();
                        std::replace(vpath.begin(), vpath.end(), '\\', '/');
                        p.TargetScenePath = vpath;
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Target portal GUID
        char guidBuf[64] = {};
        const std::string guidStr = p.TargetPortalGuid.ToString();
        std::snprintf(guidBuf, sizeof(guidBuf), "%s", guidStr.c_str());
        if (ImGui::InputText("Target Portal GUID", guidBuf, sizeof(guidBuf))) {
            std::string value = guidBuf;
            if (value.empty()) {
                p.TargetPortalGuid = ClaymoreGUID();
            } else if (value.size() == 32) {
                p.TargetPortalGuid = ClaymoreGUID::FromString(value);
            }
        }

        // Target portal path
        char pathBuf[256] = {};
        std::snprintf(pathBuf, sizeof(pathBuf), "%s", p.TargetPortalPath.c_str());
        if (ImGui::InputText("Target Portal Path", pathBuf, sizeof(pathBuf))) {
            p.TargetPortalPath = pathBuf;
        }

        ImGui::Separator();
        ImGui::DragFloat3("Entry Offset", &p.EntryOffset.x, 0.01f);
        ImGui::DragFloat3("Exit Offset", &p.ExitOffset.x, 0.01f);

        ImGui::Separator();
        ImGui::Checkbox("Auto Detect", &p.AutoDetect);
        ImGui::DragFloat("Trigger Radius", &p.TriggerRadius, 0.05f, 0.0f, 50.0f);
        ImGui::Checkbox("Fire Exit Events", &p.FireExitEvents);
    });

    // Comprehensive ParticleEmitter drawer with modern particle system controls
    registry.Register<ParticleEmitterComponent>("ParticleEmitter", [](ParticleEmitterComponent& e) {
        ImGui::PushID(&e);
        
        // ===== Playback Controls =====
        {
            bool playing = e.IsPlaying;
            if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(60, 0)))
            {
                if (playing) e.Stop(); else e.Play();
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart", ImVec2(60, 0))) e.Restart();
            ImGui::SameLine();
            ImGui::Checkbox("Enabled", &e.Enabled);
            
            // Progress bar for duration
            if (!e.Looping && e.Duration > 0.0f)
            {
                float progress = e.ElapsedTime / e.Duration;
                ImGui::ProgressBar(glm::clamp(progress, 0.0f, 1.0f), ImVec2(-1, 0), 
                    (std::to_string(int(e.ElapsedTime)) + "s / " + std::to_string(int(e.Duration)) + "s").c_str());
            }
        }
        
        // ===== Duration & Looping =====
        if (ImGui::CollapsingHeader("Duration", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat("Duration##EmitterDuration", &e.Duration, 0.1f, 0.1f, 600.0f, "%.1f s");
            ImGui::Checkbox("Looping", &e.Looping);
            ImGui::SameLine();
            ImGui::Checkbox("Prewarm", &e.Prewarm);
            ImGui::Checkbox("Play On Awake", &e.PlayOnAwake);
            ImGui::Checkbox("Destroy On Complete", &e.DestroyOnComplete);
            ImGui::SetItemTooltip("Destroy the entity when emission completes (non-looping only)");
        }
        
        // ===== Emission =====
        if (ImGui::CollapsingHeader("Emission", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloat("Rate", &e.EmissionRate, 1.0f, 0.0f, 10000.0f, "%.0f /sec");
            ImGui::DragInt("Max Particles", (int*)&e.MaxParticles, 10, 1, 100000);
            
            ImGui::Separator();
            ImGui::Checkbox("Burst Mode", &e.BurstEnabled);
            if (e.BurstEnabled)
            {
                ImGui::Indent();
                ImGui::DragInt("Burst Count", &e.BurstCount, 1, 1, 10000);
                ImGui::DragFloat("Burst Time", &e.BurstTime, 0.01f, 0.0f, 10.0f, "%.2f s");
                ImGui::DragInt("Burst Cycles", &e.BurstCycles, 1, 0, 100);
                ImGui::SetItemTooltip("0 = infinite (when looping)");
                ImGui::DragFloat("Burst Interval", &e.BurstInterval, 0.01f, 0.01f, 10.0f, "%.2f s");
                ImGui::Unindent();
            }
        }
        
        // ===== Shape =====
        if (ImGui::CollapsingHeader("Shape", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* shapes[] = { "Point", "Sphere", "Hemisphere", "Cone", "Box", "Circle", "Disc", "Edge", "Rectangle" };
            int shapeIdx = static_cast<int>(e.Shape);
            if (ImGui::Combo("Type##EmissionShape", &shapeIdx, shapes, IM_ARRAYSIZE(shapes)))
            {
                e.Shape = static_cast<ParticleEmissionShape>(shapeIdx);
            }
            
            // Shape-specific parameters
            switch (e.Shape)
            {
                case ParticleEmissionShape::Sphere:
                case ParticleEmissionShape::Hemisphere:
                    ImGui::DragFloat("Radius", &e.ShapeRadius, 0.01f, 0.0f, 100.0f);
                    ImGui::SliderFloat("Radius Thickness", &e.ShapeRadiusThickness, 0.0f, 1.0f);
                    ImGui::SetItemTooltip("0 = emit from surface, 1 = emit from volume");
                    break;
                    
                case ParticleEmissionShape::Cone:
                    ImGui::DragFloat("Radius", &e.ShapeRadius, 0.01f, 0.0f, 100.0f);
                    ImGui::DragFloat("Angle", &e.ShapeAngle, 0.5f, 0.0f, 90.0f, "%.1f deg");
                    ImGui::DragFloat("Arc", &e.ShapeArc, 1.0f, 0.0f, 360.0f, "%.0f deg");
                    break;
                    
                case ParticleEmissionShape::Box:
                case ParticleEmissionShape::Rectangle:
                    ImGui::DragFloat3("Size", &e.ShapeScale.x, 0.1f, 0.0f, 100.0f);
                    break;
                    
                case ParticleEmissionShape::Circle:
                case ParticleEmissionShape::Disc:
                    ImGui::DragFloat("Radius", &e.ShapeRadius, 0.01f, 0.0f, 100.0f);
                    ImGui::DragFloat("Arc", &e.ShapeArc, 1.0f, 0.0f, 360.0f, "%.0f deg");
                    break;
                    
                case ParticleEmissionShape::Edge:
                    ImGui::DragFloat("Length", &e.ShapeLength, 0.1f, 0.0f, 100.0f);
                    break;
                    
                default:
                    break;
            }
            
            ImGui::Checkbox("Emit From Edge", &e.ShapeEmitFromEdge);
            ImGui::Checkbox("Randomize Direction", &e.ShapeRandomizeDirection);
        }
        
        // ===== Start Values =====
        if (ImGui::CollapsingHeader("Start Values", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::DragFloatRange2("Lifetime", &e.Lifetime.Min, &e.Lifetime.Max, 0.1f, 0.0f, 60.0f, "Min: %.1f s", "Max: %.1f s");
            ImGui::DragFloatRange2("Start Speed", &e.StartSpeed.Min, &e.StartSpeed.Max, 0.1f, 0.0f, 100.0f, "Min: %.1f", "Max: %.1f");
            ImGui::DragFloatRange2("Start Size", &e.StartSize.Min, &e.StartSize.Max, 0.01f, 0.0f, 10.0f, "Min: %.2f", "Max: %.2f");
            ImGui::DragFloatRange2("Start Rotation", &e.StartRotation.Min, &e.StartRotation.Max, 1.0f, 0.0f, 360.0f, "Min: %.0f°", "Max: %.0f°");
            
            ImGui::Separator();
            ImGui::ColorEdit4("Start Color", &e.StartColor.x, ImGuiColorEditFlags_Float);
            ImGui::Checkbox("Randomize Color", &e.StartColorRandom);
            if (e.StartColorRandom)
            {
                ImGui::ColorEdit4("Color Min", &e.StartColorMin.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
                ImGui::SameLine();
                ImGui::ColorEdit4("Color Max", &e.StartColorMax.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
            }
        }
        
        // ===== Physics =====
        if (ImGui::CollapsingHeader("Physics"))
        {
            ImGui::DragFloat("Gravity Modifier", &e.GravityModifier, 0.01f, -10.0f, 10.0f);
            ImGui::DragFloat("Drag", &e.DragCoefficient, 0.01f, 0.0f, 10.0f);
            ImGui::DragFloat("Inherit Velocity", &e.InheritVelocity, 0.01f, 0.0f, 1.0f);
            
            const char* simSpaces[] = { "Local", "World" };
            int simSpace = static_cast<int>(e.SimulationSpace);
            if (ImGui::Combo("Simulation Space", &simSpace, simSpaces, IM_ARRAYSIZE(simSpaces)))
            {
                e.SimulationSpace = static_cast<ParticleSimulationSpace>(simSpace);
            }
        }
        
        // ===== Velocity Over Lifetime =====
        if (ImGui::CollapsingHeader("Velocity Over Lifetime"))
        {
            ImGui::Checkbox("Enabled##VelOL", &e.VelocityOverLifetimeEnabled);
            if (e.VelocityOverLifetimeEnabled)
            {
                ImGui::DragFloat3("Linear Velocity", &e.LinearVelocity.x, 0.1f);
                ImGui::DragFloat("Orbital Velocity", &e.OrbitalVelocity, 0.1f, -100.0f, 100.0f);
                ImGui::DragFloat("Radial Velocity", &e.RadialVelocity, 0.1f, -100.0f, 100.0f);
            }
        }
        
        // ===== Size Over Lifetime =====
        if (ImGui::CollapsingHeader("Size Over Lifetime"))
        {
            ImGui::Checkbox("Enabled##SizeOL", &e.SizeOverLifetimeEnabled);
            if (e.SizeOverLifetimeEnabled)
            {
                const char* curves[] = { "Constant", "Linear", "Ease In", "Ease Out", "Ease In Out" };
                int curveIdx = static_cast<int>(e.SizeOverLifetime.CurveType);
                if (ImGui::Combo("Curve", &curveIdx, curves, IM_ARRAYSIZE(curves)))
                {
                    e.SizeOverLifetime.CurveType = static_cast<ParticleCurveType>(curveIdx);
                }
                ImGui::DragFloat("Start Multiplier", &e.SizeOverLifetime.StartValue, 0.01f, 0.0f, 10.0f);
                ImGui::DragFloat("End Multiplier", &e.SizeOverLifetime.EndValue, 0.01f, 0.0f, 10.0f);
            }
        }
        
        // ===== Color Over Lifetime =====
        if (ImGui::CollapsingHeader("Color Over Lifetime"))
        {
            ImGui::Checkbox("Enabled##ColorOL", &e.ColorOverLifetimeEnabled);
            if (e.ColorOverLifetimeEnabled)
            {
                // Simple gradient editor with up to 5 keys
                for (size_t i = 0; i < e.ColorGradient.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    auto& key = e.ColorGradient[i];
                    ImGui::DragFloat("Time", &key.Time, 0.01f, 0.0f, 1.0f);
                    ImGui::SameLine();
                    ImGui::ColorEdit4("##color", &key.Color.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
                    ImGui::SameLine();
                    if (e.ColorGradient.size() > 2 && ImGui::SmallButton("X"))
                    {
                        e.ColorGradient.erase(e.ColorGradient.begin() + i);
                        ImGui::PopID();
                        --i;
                        continue;
                    }
                    ImGui::PopID();
                }
                if (e.ColorGradient.size() < 5 && ImGui::SmallButton("Add Key"))
                {
                    e.ColorGradient.push_back({ 0.5f, glm::vec4(1.0f) });
                    // Sort by time
                    std::sort(e.ColorGradient.begin(), e.ColorGradient.end(), 
                        [](const ParticleColorKey& a, const ParticleColorKey& b) { return a.Time < b.Time; });
                }
            }
        }
        
        // ===== Rotation Over Lifetime =====
        if (ImGui::CollapsingHeader("Rotation Over Lifetime"))
        {
            ImGui::Checkbox("Enabled##RotOL", &e.RotationOverLifetimeEnabled);
            if (e.RotationOverLifetimeEnabled)
            {
                ImGui::DragFloat("Angular Velocity", &e.AngularVelocity, 1.0f, -1000.0f, 1000.0f, "%.0f deg/s");
            }
            ImGui::Checkbox("Align With Trajectory", &e.AlignWithTrajectory);
            ImGui::SetItemTooltip("Rotate particles to face the direction they are travelling.");
        }
        
        // ===== Rendering =====
        if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const char* blendModes[] = { "Alpha", "Additive", "Multiply" };
            int blend = static_cast<int>(e.BlendMode);
            if (ImGui::Combo("Blend Mode", &blend, blendModes, IM_ARRAYSIZE(blendModes)))
            {
                e.BlendMode = static_cast<ParticleBlendMode>(blend);
            }
            
            ImGui::DragInt("Render Order", &e.RenderOrder, 1, -1000, 1000);
            ImGui::Checkbox("Face Camera (Billboard)", &e.FaceCamera);
            
            ImGui::Separator();
            ImGui::Text("Sprite");
            ImVec2 preview(56, 56);
            bool requestPicker = false;
            auto assignSprite = [&e](const std::string& path) {
                if (path.empty()) return;
                if (path == e.SpritePath && ps::isValid(e.SpriteHandle)) return;
                auto sprite = particles::AcquireSprite(path);
                if (ps::isValid(sprite)) {
                    if (!e.SpritePath.empty() && ps::isValid(e.SpriteHandle)) {
                        particles::ReleaseSprite(e.SpriteHandle);
                    }
                    e.SpriteHandle = sprite;
                    e.Uniforms.m_handle = sprite;
                    e.SpritePath = path;
                }
            };

            ImTextureID imgID = 0;
            float uv[4];
            const bool hasSpritePreview = ps::isValid(e.SpriteHandle)
                && !ps::IsSpriteArrayBacked(e.SpriteHandle)
                && ps::GetSpriteUV(e.SpriteHandle, uv);
            if (hasSpritePreview)
            {
                bgfx::TextureHandle spriteTex = ps::GetSpriteTexture(e.SpriteHandle);
                imgID = TextureLoader::ToImGuiTextureID(spriteTex);
                if (ImGui::ImageButton("##particleSprite", imgID, preview, ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3])))
                {
                    requestPicker = true;
                }
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.22f, 0.25f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                const char* buttonLabel = ps::isValid(e.SpriteHandle) ? "Array\nSprite" : "Select\nSprite";
                if (ImGui::Button(buttonLabel, preview))
                {
                    requestPicker = true;
                }
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);

                if (ps::isValid(e.SpriteHandle) && ps::IsSpriteArrayBacked(e.SpriteHandle) && ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Array-backed particle sprites cannot be previewed in the inspector.");
                }
            }

            // Drag-drop support for sprites
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE"))
                {
                    const char* path = (const char*)payload->Data;
                    std::string ext = (std::filesystem::path(path).extension().string());
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga")
                    {
                        assignSprite(path);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Show current sprite path
            if (!e.SpritePath.empty())
            {
                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextDisabled("%s", std::filesystem::path(e.SpritePath).filename().string().c_str());
                if (ImGui::SmallButton("Clear"))
                {
                    if (!e.SpritePath.empty() && ps::isValid(e.SpriteHandle)) {
                        particles::ReleaseSprite(e.SpriteHandle);
                    }
                    e.SpritePath.clear();
                    e.SpriteHandle = { uint16_t{UINT16_MAX} };
                    e.Uniforms.m_handle = { uint16_t{UINT16_MAX} };
                }
                ImGui::EndGroup();
            }

            // Texture picker popup (includes both project and engine particle assets)
            if (requestPicker)
            {
                ImGui::OpenPopup("ParticleSpritePicker");
            }
            
            std::filesystem::path engineParticlesDir = EnginePaths::GetEngineAssetPath() / "particles";
            texturepicker::DrawTexturePickerPopup("ParticleSpritePicker",
                [&assignSprite](const std::string& selectedPath) { assignSprite(selectedPath); },
                e.SpritePath,
                { engineParticlesDir });
        }
        
        ImGui::PopID();
    });

    registry.Register<TerrainComponent>("Terrain", [](TerrainComponent& t) {
        auto markTerrainPlanarShapeDirty = [&t]() {
            t.MeshDirty = true;
            t.ChunkMeshDirty = true;
            t.PhysicsDirty = true;
            t.AssetDirty = true;
            t.GrassStructureDirty = true;
            Terrain::DestroyClipmapSystem(t);
        };

        auto markTerrainHeightScaleDirty = [&t]() {
            t.MeshDirty = true;
            t.PhysicsDirty = true;
            t.AssetDirty = true;
            t.GrassStructureDirty = true;
        };

        auto normalizeHeightmapStampRange = [&t]() {
            t.Brush.HeightmapStampMinY = std::clamp(t.Brush.HeightmapStampMinY, 0.0f, t.MaxHeight);
            t.Brush.HeightmapStampMaxY = std::clamp(t.Brush.HeightmapStampMaxY, t.Brush.HeightmapStampMinY, t.MaxHeight);
            t.Brush.HeightmapStampBaselineY = std::clamp(
                t.Brush.HeightmapStampBaselineY,
                t.Brush.HeightmapStampMinY,
                t.Brush.HeightmapStampMaxY);
        };

        auto assignHeightmapStampTexture = [&t](const std::string& newPath) {
            t.Brush.HeightmapStampTexturePath = newPath;
            t.Brush.HeightmapStampTexture = BGFX_INVALID_HANDLE;
            t.Brush.HeightmapStampSamples.clear();
            t.Brush.HeightmapStampWidth = 0;
            t.Brush.HeightmapStampHeight = 0;
            if (newPath.empty()) {
                return;
            }

            TextureSpecifier spec;
            spec.Path = newPath;
            t.Brush.HeightmapStampTexture = AcquireTextureHandle(spec, TextureColorSpace::Linear);
            Terrain::LoadHeightmapTextureSamples(
                newPath,
                t.Brush.HeightmapStampSamples,
                t.Brush.HeightmapStampWidth,
                t.Brush.HeightmapStampHeight);
        };

        if (ImGui::BeginTabBar("TerrainTabs")) {
        if (ImGui::BeginTabItem("Shape")) {
        // Grid resolution dropdown with power-of-2 presets (safely resamples terrain data)
        // Placed before Clay scope since raw Combo doesn't fit in the Clay field layout
        static const char* resOptions[] = { "64", "128", "256", "512", "1024", "2048", "4096" };
        static const uint32_t resValues[] = { 64, 128, 256, 512, 1024, 2048, 4096 };
        int resIdx = 2; // Default to 256
        for (int i = 0; i < IM_ARRAYSIZE(resValues); ++i) {
            if (t.GridResolution == resValues[i]) { resIdx = i; break; }
            if (t.GridResolution < resValues[i]) { resIdx = (i > 0) ? i - 1 : 0; break; }
            if (i == IM_ARRAYSIZE(resValues) - 1) { resIdx = i; }
        }
        if (ImGui::Combo("Grid Resolution", &resIdx, resOptions, IM_ARRAYSIZE(resOptions))) {
            t.ResizeWithResampling(resValues[resIdx]);
            Terrain::DestroyClipmapSystem(t);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Changing resolution will resample heightmap, splatmap, and grass masks.\nHigher resolution = more detail but more memory.");
        }

        {
            ImGui::ClayInspectorContentScope scope("TerrainComponentGrid");

            glm::vec2 worldSize = t.WorldSize;
            if (ImGui::ClayFieldVec(scope, "World Size", &worldSize.x, 2, 0.1f)) {
                worldSize.x = std::max(0.1f, worldSize.x);
                worldSize.y = std::max(0.1f, worldSize.y);
                if (worldSize != t.WorldSize) {
                    t.WorldSize = worldSize;
                    markTerrainPlanarShapeDirty();
                }
            }

            float maxHeight = t.MaxHeight;
            if (ImGui::ClayFieldFloat(scope, "Max Height", &maxHeight, 0.1f)) {
                const float clampedMaxHeight = std::max(0.1f, maxHeight);
                if (clampedMaxHeight != t.MaxHeight) {
                    t.MaxHeight = clampedMaxHeight;
                    normalizeHeightmapStampRange();
                    markTerrainHeightScaleDirty();
                }
            }
        }
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Import/Export")) {
        // Heightmap Import/Export
        if (ImGui::CollapsingHeader("Heightmap I/O")) {
            ImGui::TextDisabled("Import/Export heightmap as grayscale image");
            ImGui::TextDisabled("Path is relative to project root");
            
            static char heightmapPath[512] = "assets/terrain/heightmap.png";
            ImGui::InputText("Path", heightmapPath, sizeof(heightmapPath));
            
            if (ImGui::Button("Export Heightmap")) {
                // Resolve path relative to project directory
                std::filesystem::path fullPath = Project::GetProjectDirectory() / heightmapPath;
                // Ensure parent directory exists
                std::filesystem::create_directories(fullPath.parent_path());
                if (Terrain::ExportHeightmap(t, fullPath.string())) {
                    std::cout << "[TerrainUI] Heightmap exported to: " << fullPath.string() << "\n";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Import Heightmap")) {
                // Resolve path relative to project directory
                std::filesystem::path fullPath = Project::GetProjectDirectory() / heightmapPath;
                if (Terrain::ImportHeightmap(t, fullPath.string())) {
                    std::cout << "[TerrainUI] Heightmap imported from: " << fullPath.string() << "\n";
                }
            }
            
            ImGui::TextDisabled("8-bit and 16-bit grayscale PNG supported");
        }

        // Splatmap Import/Export
        if (ImGui::CollapsingHeader("Splatmap I/O")) {
            ImGui::TextDisabled("Import/Export splatmap as RGBA image");
            ImGui::TextDisabled("R=Layer0, G=Layer1, B=Layer2, A=Layer3");
            ImGui::TextDisabled("Path is relative to project root");
            
            static char splatmapPath[512] = "assets/terrain/splatmap.png";
            ImGui::InputText("Splat Path", splatmapPath, sizeof(splatmapPath));
            
            if (ImGui::Button("Export Splatmap")) {
                // Resolve path relative to project directory
                std::filesystem::path fullPath = Project::GetProjectDirectory() / splatmapPath;
                // Ensure parent directory exists
                std::filesystem::create_directories(fullPath.parent_path());
                if (Terrain::ExportSplatmap(t, fullPath.string())) {
                    std::cout << "[TerrainUI] Splatmap exported to: " << fullPath.string() << "\n";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Import Splatmap")) {
                // Resolve path relative to project directory
                std::filesystem::path fullPath = Project::GetProjectDirectory() / splatmapPath;
                if (Terrain::ImportSplatmap(t, fullPath.string())) {
                    std::cout << "[TerrainUI] Splatmap imported from: " << fullPath.string() << "\n";
                }
            }
            
            ImGui::TextDisabled("RGBA PNG - each channel is a layer weight");
        }
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("LOD")) {
        // Clipmap settings (LOD system) - legacy approach
        if (ImGui::CollapsingHeader("Clipmap LOD (Legacy)")) {
            bool useClipmaps = t.UseClipmaps;
            if (ImGui::Checkbox("Enable Clipmaps", &useClipmaps)) {
                t.UseClipmaps = useClipmaps;
                // Disable chunked terrain when enabling clipmaps
                if (useClipmaps) {
                    t.UseChunkedTerrain = false;
                }
            }
            ImGui::SetItemTooltip("Use geometry clipmaps for efficient LOD rendering\n(Legacy approach - prefer Chunked Terrain for new projects)");
            
            if (t.UseClipmaps) {
                int levels = static_cast<int>(t.ClipmapLevels);
                if (ImGui::SliderInt("Clipmap Levels", &levels, 2, 8)) {
                    t.ClipmapLevels = static_cast<uint32_t>(levels);
                    // Force clipmap system rebuild
                    if (t.ClipmapSystem) {
                        Terrain::DestroyClipmapSystem(t);
                    }
                }
                
                int gridSize = static_cast<int>(t.ClipmapGridSize);
                const char* gridSizes[] = { "32", "64", "128", "256" };
                int gridIdx = (gridSize <= 32) ? 0 : (gridSize <= 64) ? 1 : (gridSize <= 128) ? 2 : 3;
                if (ImGui::Combo("Grid Size", &gridIdx, gridSizes, IM_ARRAYSIZE(gridSizes))) {
                    static const uint32_t sizes[] = { 32, 64, 128, 256 };
                    t.ClipmapGridSize = sizes[gridIdx];
                    if (t.ClipmapSystem) {
                        Terrain::DestroyClipmapSystem(t);
                    }
                }
                
                bool morphing = t.ClipmapMorphing;
                if (ImGui::Checkbox("Enable Morphing", &morphing)) {
                    t.ClipmapMorphing = morphing;
                }
                ImGui::SetItemTooltip("Smooth transitions between LOD levels");
            }
        }
        
        // Chunked Terrain settings (Skyrim-style cells)
        if (ImGui::CollapsingHeader("Chunked Terrain (Recommended)", ImGuiTreeNodeFlags_DefaultOpen)) {
            bool useChunked = t.UseChunkedTerrain;
            if (ImGui::Checkbox("Enable Chunked Terrain", &useChunked)) {
                t.UseChunkedTerrain = useChunked;
                // Disable clipmaps when enabling chunked terrain
                if (useChunked) {
                    t.UseClipmaps = false;
                    if (t.ClipmapSystem) {
                        Terrain::DestroyClipmapSystem(t);
                    }
                }
                t.ChunkMeshDirty = true;
            }
            ImGui::SetItemTooltip("Skyrim-style terrain cells with per-chunk LOD and culling.\nRecommended for most projects.");
            
            if (t.UseChunkedTerrain) {
                // Chunk vertex size (power of 2 + 1)
                const char* chunkSizes[] = { "17 (16+1)", "33 (32+1)", "65 (64+1)", "129 (128+1)" };
                static const uint32_t chunkSizeValues[] = { 17, 33, 65, 129 };
                int chunkIdx = (t.ChunkVertexSize <= 17) ? 0 : 
                               (t.ChunkVertexSize <= 33) ? 1 : 
                               (t.ChunkVertexSize <= 65) ? 2 : 3;
                if (ImGui::Combo("Chunk Size", &chunkIdx, chunkSizes, IM_ARRAYSIZE(chunkSizes))) {
                    t.ChunkVertexSize = chunkSizeValues[chunkIdx];
                    t.ChunkMeshDirty = true;
                }
                ImGui::SetItemTooltip("Vertices per chunk edge. Smaller = more chunks, better culling.\nLarger = fewer draw calls.");
                
                // Display chunk count
                uint32_t effectiveChunkSize = t.ChunkVertexSize > 1 ? t.ChunkVertexSize - 1 : 1;
                uint32_t chunksPerSide = (t.GridResolution + effectiveChunkSize - 2) / effectiveChunkSize;
                chunksPerSide = std::max(1u, chunksPerSide);
                ImGui::Text("Chunk Grid: %u x %u (%u total)", chunksPerSide, chunksPerSide, chunksPerSide * chunksPerSide);
                
                ImGui::Spacing();
                
                // LOD Morphing
                bool chunkMorphing = t.ChunkMorphing;
                if (ImGui::Checkbox("Enable LOD Morphing", &chunkMorphing)) {
                    t.ChunkMorphing = chunkMorphing;
                }
                ImGui::SetItemTooltip("Smooth vertex transitions between LOD levels");
                
                if (t.ChunkMorphing) {
                    float morphRegion = t.ChunkMorphRegion;
                    if (ImGui::SliderFloat("Morph Region", &morphRegion, 0.1f, 0.5f, "%.2f")) {
                        t.ChunkMorphRegion = morphRegion;
                    }
                    ImGui::SetItemTooltip("Fraction of LOD distance band for morph transition.\nHigher = longer blend distance.");
                }
                
                ImGui::Spacing();
                
                // LOD Distances
                if (ImGui::TreeNode("LOD Distances")) {
                    bool lodChanged = false;
                    for (int i = 0; i < 4; ++i) {
                        char label[32];
                        snprintf(label, sizeof(label), "LOD %d Distance", i);
                        float dist = t.LODConfig.LODDistances[i];
                        if (ImGui::DragFloat(label, &dist, 1.0f, (i > 0 ? t.LODConfig.LODDistances[i-1] : 1.0f), 10000.0f, "%.0f")) {
                            t.LODConfig.LODDistances[i] = dist;
                            lodChanged = true;
                        }
                    }
                    if (lodChanged) {
                        // Ensure distances are sorted
                        for (int i = 1; i < 4; ++i) {
                            if (t.LODConfig.LODDistances[i] < t.LODConfig.LODDistances[i-1]) {
                                t.LODConfig.LODDistances[i] = t.LODConfig.LODDistances[i-1] + 10.0f;
                            }
                        }
                    }
                    ImGui::TreePop();
                }
                
                ImGui::Spacing();
                
                // Streaming settings (for large worlds)
                if (ImGui::TreeNode("Streaming (Large Worlds)")) {
                    bool streaming = t.ChunkStreaming;
                    if (ImGui::Checkbox("Enable Streaming", &streaming)) {
                        t.ChunkStreaming = streaming;
                    }
                    ImGui::SetItemTooltip("Background loading/unloading of terrain chunks.\nUseful for very large terrains (8km+).");
                    
                    if (t.ChunkStreaming) {
                        float loadRadius = t.StreamingLoadRadius;
                        if (ImGui::DragFloat("Load Radius", &loadRadius, 10.0f, 100.0f, 5000.0f, "%.0f")) {
                            t.StreamingLoadRadius = std::max(100.0f, loadRadius);
                            // Ensure unload radius is larger
                            if (t.StreamingUnloadRadius < t.StreamingLoadRadius + 50.0f) {
                                t.StreamingUnloadRadius = t.StreamingLoadRadius + 100.0f;
                            }
                        }
                        ImGui::SetItemTooltip("Distance from camera to start loading chunks");
                        
                        float unloadRadius = t.StreamingUnloadRadius;
                        if (ImGui::DragFloat("Unload Radius", &unloadRadius, 10.0f, t.StreamingLoadRadius + 50.0f, 6000.0f, "%.0f")) {
                            t.StreamingUnloadRadius = std::max(t.StreamingLoadRadius + 50.0f, unloadRadius);
                        }
                        ImGui::SetItemTooltip("Distance from camera to unload chunks");
                    }
                    ImGui::TreePop();
                }
            }
        }
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Paint")) {
        struct TerrainBrushModeOption {
            TerrainBrushMode Mode;
            const char* Label;
        };
        static const TerrainBrushModeOption brushModes[] = {
            { TerrainBrushMode::Height, "Height" },
            { TerrainBrushMode::Texture, "Texture" },
            { TerrainBrushMode::Grass, "Grass" },
            { TerrainBrushMode::Instancer, "Instancer" },
            { TerrainBrushMode::SmoothHeight, "Smooth Height" },
            { TerrainBrushMode::StampHeight, "Stamp Height" },
            { TerrainBrushMode::HeightmapStamp, "Heightmap Stamp" },
            { TerrainBrushMode::ErosionNoise, "Erosion Noise" },
            { TerrainBrushMode::FlattenHeight, "Flatten Height" },
            { TerrainBrushMode::CliffStamp, "Cliff Stamp" },
            { TerrainBrushMode::MountainStamp, "Mountain Stamp" },
            { TerrainBrushMode::Hole, "Hole" }
        };
        int mode = 0;
        for (int i = 0; i < IM_ARRAYSIZE(brushModes); ++i) {
            if (brushModes[i].Mode == t.Brush.Mode) {
                mode = i;
                break;
            }
        }
        if (ImGui::Combo("Brush Mode", &mode, [](void* data, int idx, const char** outText) {
            const auto* options = static_cast<const TerrainBrushModeOption*>(data);
            *outText = options[idx].Label;
            return true;
        }, const_cast<TerrainBrushModeOption*>(brushModes), IM_ARRAYSIZE(brushModes))) {
            mode = std::clamp(mode, 0, static_cast<int>(IM_ARRAYSIZE(brushModes)) - 1);
            t.Brush.Mode = brushModes[mode].Mode;
        }

        const bool isHeightmapStampMode = (t.Brush.Mode == TerrainBrushMode::HeightmapStamp);
        const bool isTextureMode = (t.Brush.Mode == TerrainBrushMode::Texture);
        const bool isHoleMode = (t.Brush.Mode == TerrainBrushMode::Hole);

        ImGui::SliderFloat(isHeightmapStampMode ? "Stamp Half Size" : "Brush Radius", &t.Brush.Radius, 0.1f, 256.0f, "%.2f");
        if (isHeightmapStampMode) {
            ImGui::SetItemTooltip("Heightmap Stamp uses a square footprint. This value is the square half-extent in world units.");
        }
        ImGui::SliderFloat("Brush Strength", &t.Brush.Strength, 0.01f, 10.0f, "%.2f");
        if (isTextureMode || isHoleMode) {
            ImGui::SliderFloat("Texture Strength", &t.Brush.TextureStrength, 0.0f, 1.0f, "%.2f");
        }
        if (t.Brush.Mode == TerrainBrushMode::Grass) {
            ImGui::SliderFloat("Grass Strength", &t.Brush.GrassStrength, 0.0f, 5.0f, "%.2f");
        }
        if (t.Brush.Mode == TerrainBrushMode::Instancer) {
            ImGui::SliderFloat("Instancer Strength", &t.Brush.InstancerStrength, 0.0f, 5.0f, "%.2f");
        }
        if (isHeightmapStampMode) {
            ImGui::SeparatorText("Heightmap Stamp");
            ImGui::Text("Heightmap Texture");
            if (!t.Brush.HeightmapStampTexturePath.empty()) {
                ImGui::TextDisabled("%s", t.Brush.HeightmapStampTexturePath.c_str());
            } else {
                ImGui::TextDisabled("Drop a grayscale heightmap here or pick one.");
            }

            ImGui::PushID("HeightmapStampTexture");
            ImTextureID texId = TextureLoader::ToImGuiTextureID(t.Brush.HeightmapStampTexture);
            ImVec2 preview(56.0f, 56.0f);
            if (texId) {
                ImGui::Image(texId, preview);
            } else {
                if (ImGui::Button("Assign", preview)) {
                    ImGui::OpenPopup("HeightmapStampTexturePicker");
                }
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    const char* dropPath = static_cast<const char*>(payload->Data);
                    if (dropPath) {
                        std::string ext = std::filesystem::path(dropPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") {
                            assignHeightmapStampTexture(dropPath);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (ImGui::Button("Pick")) {
                ImGui::OpenPopup("HeightmapStampTexturePicker");
            }
            texturepicker::DrawTexturePickerPopup("HeightmapStampTexturePicker", [&](const std::string& selectedPath) {
                assignHeightmapStampTexture(selectedPath);
            }, t.Brush.HeightmapStampTexturePath);
            ImGui::SameLine();
            if (ImGui::Button("Clear")) {
                assignHeightmapStampTexture(std::string());
            }
            ImGui::PopID();

            if (t.Brush.HeightmapStampWidth > 0 && t.Brush.HeightmapStampHeight > 0) {
                ImGui::TextDisabled("Loaded %dx%d grayscale samples", t.Brush.HeightmapStampWidth, t.Brush.HeightmapStampHeight);
            } else if (!t.Brush.HeightmapStampTexturePath.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f), "Couldn't decode height data from the selected texture.");
            }

            if (ImGui::Checkbox("Additive", &t.Brush.HeightmapStampAdditive) && t.Brush.HeightmapStampAdditive) {
                t.Brush.HeightmapStampSubtractive = false;
            }
            if (ImGui::Checkbox("Subtractive", &t.Brush.HeightmapStampSubtractive) && t.Brush.HeightmapStampSubtractive) {
                t.Brush.HeightmapStampAdditive = false;
            }

            bool stampRangeChanged = false;
            stampRangeChanged |= ImGui::DragFloat("Min Y", &t.Brush.HeightmapStampMinY, 0.1f, 0.0f, t.MaxHeight, "%.2f");
            stampRangeChanged |= ImGui::DragFloat("Baseline Y", &t.Brush.HeightmapStampBaselineY, 0.1f, 0.0f, t.MaxHeight, "%.2f");
            stampRangeChanged |= ImGui::DragFloat("Max Y", &t.Brush.HeightmapStampMaxY, 0.1f, 0.0f, t.MaxHeight, "%.2f");
            if (stampRangeChanged) {
                normalizeHeightmapStampRange();
            }
            ImGui::TextDisabled("Black maps to Min Y, white maps to Max Y. Baseline Y is the zero-reference for additive/subtractive stamps.");
        }
        if (t.Brush.Mode == TerrainBrushMode::ErosionNoise) {
            ImGui::SliderFloat("Noise Scale", &t.Brush.ErosionNoiseScale, 0.01f, 1.0f, "%.3f");
            ImGui::SetItemTooltip("Noise frequency - smaller values create larger erosion features");
            ImGui::SliderInt("Noise Octaves", &t.Brush.ErosionNoiseOctaves, 1, 8);
            ImGui::SetItemTooltip("Number of noise layers - more octaves add finer detail");
            ImGui::SliderFloat("Persistence", &t.Brush.ErosionNoisePersistence, 0.1f, 0.9f, "%.2f");
            ImGui::SetItemTooltip("How much each octave contributes - lower values create smoother erosion");
            ImGui::SliderFloat("Noise Strength", &t.Brush.ErosionNoiseStrength, 0.1f, 5.0f, "%.2f");
            ImGui::SetItemTooltip("Overall erosion intensity");
        }
        if (t.Brush.Mode == TerrainBrushMode::FlattenHeight) {
            ImGui::DragFloat("Target Height", &t.Brush.FlattenTargetHeight, 0.1f, 0.0f, t.MaxHeight, "%.2f");
            ImGui::SetItemTooltip("World height to flatten the terrain to when painting");
        }
        if (t.Brush.Mode == TerrainBrushMode::CliffStamp) {
            ImGui::DragFloat("Cliff Height", &t.Brush.CliffHeight, 0.5f, 1.0f, t.MaxHeight, "%.1f");
            ImGui::SetItemTooltip("Height of the cliff face in world units");
            ImGui::SliderFloat("Roughness", &t.Brush.CliffRoughness, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Amount of jagged rock detail on the cliff face");
            ImGui::SliderFloat("Layering", &t.Brush.CliffLayering, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Horizontal sedimentary rock banding effect");
        }
        if (t.Brush.Mode == TerrainBrushMode::MountainStamp) {
            ImGui::DragFloat("Peak Height", &t.Brush.MountainHeight, 0.5f, 1.0f, t.MaxHeight, "%.1f");
            ImGui::SetItemTooltip("Height of the mountain peak in world units");
            ImGui::SliderFloat("Ridge Scale", &t.Brush.MountainRidgeScale, 0.05f, 0.5f, "%.3f");
            ImGui::SetItemTooltip("Scale of ridge features - smaller creates more ridges");
            ImGui::SliderFloat("Rockiness", &t.Brush.MountainRockiness, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Amount of rocky surface detail");
            ImGui::SliderFloat("Steepness", &t.Brush.MountainSteepness, 0.5f, 3.0f, "%.2f");
            ImGui::SetItemTooltip("How steep the mountain slopes are");
        }
        ImGui::BeginDisabled(isHeightmapStampMode);
        ImGui::SliderFloat("Brush Falloff", &t.Brush.Falloff, 0.1f, 8.0f, "%.2f");
        ImGui::EndDisabled();
        if (isHeightmapStampMode) {
            ImGui::SetItemTooltip("Heightmap Stamp uses the texture footprint directly rather than the circular falloff.");
        }
        ImGui::Checkbox("Align To Surface", &t.Brush.AlignToNormal);

        int maxLayer = t.Layers.empty() ? -1 : static_cast<int>(t.Layers.size()) - 1;
        if (isTextureMode) {
            if (maxLayer >= 0) {
                int activeLayer = std::clamp(t.Brush.ActiveLayer, 0, maxLayer);
                if (ImGui::SliderInt("Active Layer", &activeLayer, 0, maxLayer)) {
                    t.Brush.ActiveLayer = activeLayer;
                }
            } else {
                ImGui::TextDisabled("Add at least one layer to paint textures.");
                t.Brush.ActiveLayer = 0;
            }
        }
        if (!t.GrassLayers.empty() && t.Brush.Mode == TerrainBrushMode::Grass) {
            int maxGrass = static_cast<int>(t.GrassLayers.size()) - 1;
            maxGrass = std::max(maxGrass, 0);
            int activeGrass = std::clamp(t.Brush.ActiveGrassLayer, 0, maxGrass);
            if (ImGui::SliderInt("Active Grass Layer", &activeGrass, 0, maxGrass)) {
                t.Brush.ActiveGrassLayer = activeGrass;
            }
        } else if (t.GrassLayers.empty()) {
            t.Brush.ActiveGrassLayer = 0;
        }
        if (!t.InstancerLayers.empty() && t.Brush.Mode == TerrainBrushMode::Instancer) {
            int maxInstancer = static_cast<int>(t.InstancerLayers.size()) - 1;
            maxInstancer = std::max(maxInstancer, 0);
            int activeInstancer = std::clamp(t.Brush.ActiveInstancerLayer, 0, maxInstancer);
            if (ImGui::SliderInt("Active Instancer Layer", &activeInstancer, 0, maxInstancer)) {
                t.Brush.ActiveInstancerLayer = activeInstancer;
            }
        } else if (t.InstancerLayers.empty()) {
            t.Brush.ActiveInstancerLayer = 0;
        }
        ImGui::EndTabItem();
        }
        
        if (ImGui::BeginTabItem("Layers")) {
        
        // Layer Texture Array Settings
        if (ImGui::TreeNodeEx("Texture Array Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Resolution dropdown
            static const char* resOptions[] = { "256", "512", "1024", "2048" };
            static const uint32_t resValues[] = { 256, 512, 1024, 2048 };
            int resIdx = 2; // Default to 1024
            for (int i = 0; i < IM_ARRAYSIZE(resValues); ++i) {
                if (t.LayerTextureResolution == resValues[i]) { resIdx = i; break; }
            }
            if (ImGui::Combo("Layer Texture Resolution", &resIdx, resOptions, IM_ARRAYSIZE(resOptions))) {
                t.LayerTextureResolution = resValues[resIdx];
                t.LayerTextureArraysDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("All layer textures are resized to this resolution.\nHigher = better quality, more VRAM.");
            }
            
            // Filter dropdown
            static const char* filterOptions[] = { "Nearest", "Bilinear", "Bicubic (Catmull-Rom)" };
            int filterIdx = static_cast<int>(t.LayerResizeFilter);
            if (ImGui::Combo("Resize Filter", &filterIdx, filterOptions, IM_ARRAYSIZE(filterOptions))) {
                t.LayerResizeFilter = static_cast<TerrainTextureFilter>(filterIdx);
                t.LayerTextureArraysDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Filter used when resizing layer textures.\nNearest: fast, pixelated\nBilinear: balanced\nBicubic: best quality, slower");
            }
            
            // Manual rebuild button
            if (ImGui::Button("Rebuild Texture Arrays")) {
                t.LayerTextureArraysDirty = true;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Force rebuild of texture arrays.\nUseful if textures were modified externally.");
            }
            
            ImGui::TreePop();
        }
        
        if (ImGui::TreeNodeEx("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto assignTexture = [&t](std::string& path, bgfx::TextureHandle& handle, const std::string& newPath) {
                path = newPath;
                if (!newPath.empty()) {
                    TextureSpecifier spec;
                    spec.Path = newPath;
                    handle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                } else {
                    handle = BGFX_INVALID_HANDLE;
                }
                // Mark texture arrays dirty when layer texture changes
                t.LayerTextureArraysDirty = true;
            };

            auto drawTextureSlot = [&](const char* label, std::string& path, bgfx::TextureHandle& handle, int layerIndex) {
                ImGui::Text("%s", label);
                if (!path.empty()) {
                    ImGui::TextDisabled("%s", path.c_str());
                }
                ImGui::PushID(label);
                ImTextureID texId = TextureLoader::ToImGuiTextureID(handle);
                ImVec2 preview(56, 56);
                bool requestPicker = false;
                if (texId) {
                    ImGui::Image(texId, preview);
                } else if (ImGui::Button("Assign", preview)) {
                    requestPicker = true;
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                        const char* dropPath = static_cast<const char*>(payload->Data);
                        if (dropPath) {
                            std::string ext = std::filesystem::path(dropPath).extension().string();
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") {
                                assignTexture(path, handle, dropPath);
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                std::string popupId = "TerrainTexturePicker_" + std::string(label) + std::to_string(layerIndex);
                if (requestPicker) {
                    ImGui::OpenPopup(popupId.c_str());
                }
                texturepicker::DrawTexturePickerPopup(popupId.c_str(), [&](const std::string& selectedPath) {
                    assignTexture(path, handle, selectedPath);
                }, path);
                if (ImGui::Button("Clear")) {
                    assignTexture(path, handle, std::string());
                }
                ImGui::PopID();
            };

            for (int i = 0; i < static_cast<int>(t.Layers.size()); ++i) {
                TerrainLayerDesc& layer = t.Layers[i];
                std::string header = layer.Name.empty() ? ("Layer " + std::to_string(i)) : layer.Name;
                if (ImGui::TreeNodeEx((header + "##terrain_layer_" + std::to_string(i)).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                    char nameBuf[64];
                    std::strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf));
                    nameBuf[sizeof(nameBuf) - 1] = '\0';
                    if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                        layer.Name = nameBuf;
                    }
                    ImGui::ColorEdit3("Placeholder Color", &layer.PlaceholderColor.x);
                    ImGui::DragFloat("Tiling", &layer.Tiling, 0.05f, 0.01f, 2048.0f, "%.2f");
                    ImGui::DragFloat("Nav Cost", &layer.NavCost, 0.05f, 0.01f, 100.0f, "%.2f");
                    drawTextureSlot("Albedo", layer.AlbedoPath, layer.AlbedoHandle, i);
                    drawTextureSlot("Normal", layer.NormalPath, layer.NormalHandle, i);
                    if (t.Layers.size() > 1) {
                        if (ImGui::Button("Remove Layer")) {
                            const int removedIndex = i;
                            t.Layers.erase(t.Layers.begin() + i);
                            RemapTerrainSplatAfterLayerRemoval(t, removedIndex);
                            if (t.Brush.ActiveLayer >= static_cast<int>(t.Layers.size())) {
                                t.Brush.ActiveLayer = t.Layers.empty() ? 0 : static_cast<int>(t.Layers.size()) - 1;
                            }
                            t.LayerTextureArraysDirty = true; // Rebuild arrays after removal
                            ImGui::TreePop();
                            --i;
                            continue;
                        }
                    }
                    ImGui::TreePop();
                }
            }
            if (t.Layers.size() < kMaxTerrainLayers && ImGui::Button("Add Layer")) {
                TerrainLayerDesc layer;
                layer.Name = "Layer " + std::to_string(t.Layers.size());
                // Ensure SplatMap2 exists when adding layer 5+
                if (t.Layers.size() >= 4) {
                    t.EnsureSplatMap2();
                }
                t.Layers.push_back(std::move(layer));
                t.LayerTextureArraysDirty = true; // Rebuild arrays for new layer
            }
            ImGui::TreePop();
        }
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Instancers")) {
    if (ImGui::TreeNodeEx("Instancer Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto markInstancerLayerDirty = [&t](TerrainInstancerLayerDesc& layer, bool assetMaskDirty = false) {
            layer.MarkRuntimeDirty();
            if (assetMaskDirty) {
                t.AssetDirty = true;
            }
        };

        auto markInstancerLayerCollisionDirty = [](TerrainInstancerLayerDesc& layer) {
            if (Physics::GetSystem() != nullptr) {
                for (auto& entry : layer.ActiveCollisionBodies) {
                    if (!entry.second.IsInvalid()) {
                        Physics::DestroyBody(entry.second);
                    }
                }
            }
            layer.ActiveCollisionBodies.clear();
        };

        auto ensureAssetEntry = [](const std::string& path, AssetType type) -> AssetEntry* {
            AssetEntry* entry = AssetLibrary::Instance().GetAsset(path);
            if (!entry) {
                ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(path);
                if (guid.high == 0 && guid.low == 0) {
                    guid = ClaymoreGUID::Generate();
                }
                AssetReference ref(guid, 0, static_cast<int>(type));
                AssetLibrary::Instance().RegisterAsset(ref, type, path, std::filesystem::path(path).filename().string());
                entry = AssetLibrary::Instance().GetAsset(path);
            }
            return entry;
        };

        auto assignInstancerMesh = [&](TerrainInstancerLayerDesc& layer, const std::string& path) {
            ScopedTerrainInstancerUiTimer timer("mesh slot assignment");
            if (path.empty()) {
                layer.Instancer.MeshAsset = AssetReference{};
                layer.Instancer.MeshPath.clear();
                layer.ReleaseRuntime();
                markInstancerLayerDirty(layer);
                return;
            }
            if (AssetEntry* entry = ensureAssetEntry(path, AssetType::Mesh)) {
                layer.Instancer.MeshAsset = entry->reference;
                layer.Instancer.MeshPath = entry->path;
                layer.Instancer.ReloadMesh();
                layer.ReleaseCollisionBodies();
                layer.SharedCollisionShape = nullptr;
                if (layer.Instancer.Runtime.Instances.empty() &&
                    !layer.RuntimeDirty &&
                    !layer.RuntimeRebuildInProgress) {
                    layer.MarkRuntimeDirty();
                }
            }
        };

        auto assignInstancerPrefab = [&](TerrainInstancerLayerDesc& layer, const std::string& path) {
            ScopedTerrainInstancerUiTimer timer("prefab slot assignment");
            const bool hasActivePrefabs =
                layer.Instancer.Runtime.ActivePrefabs > 0 ||
                !layer.Instancer.Runtime.ActivePrefabIndices.empty();
            if (path.empty()) {
                layer.Instancer.PrefabAsset = AssetReference{};
                layer.Instancer.PrefabPath.clear();
                if (hasActivePrefabs) {
                    markInstancerLayerDirty(layer);
                }
                return;
            }
            if (AssetEntry* entry = ensureAssetEntry(path, AssetType::Prefab)) {
                layer.Instancer.PrefabAsset = entry->reference;
                layer.Instancer.PrefabPath = entry->path;
                if (hasActivePrefabs) {
                    markInstancerLayerDirty(layer);
                }
            }
        };

        auto drawInstancerAssetDropSlot = [&](const char* id,
                                              const char* title,
                                              const std::string& assignedPath,
                                              const char* emptyHint,
                                              const char* acceptHint,
                                              auto&& acceptDrop,
                                              auto&& clearValue) {
            ImGui::PushID(id);
            ImGui::TextUnformatted(title);

            std::string displayName = emptyHint;
            if (assignedPath.empty()) {
                displayName = emptyHint;
            } else {
                const std::filesystem::path assetPath(assignedPath);
                displayName = assetPath.stem().string();
                if (displayName.empty()) {
                    displayName = assetPath.filename().string();
                }
                if (displayName.empty()) {
                    displayName = assignedPath;
                }
            }

            const float clearWidth = ImGui::GetFrameHeight();
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float buttonWidth = std::max(80.0f, ImGui::GetContentRegionAvail().x - clearWidth - spacing);
            ImGui::Button(displayName.c_str(), ImVec2(buttonWidth, 0.0f));
            if (ImGui::IsItemHovered()) {
                if (assignedPath.empty()) {
                    ImGui::SetTooltip("%s", acceptHint);
                } else {
                    ImGui::SetTooltip("%s", assignedPath.c_str());
                }
            }
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Clear")) {
                    clearValue();
                }
                ImGui::EndPopup();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                    const char* dropPath = static_cast<const char*>(payload->Data);
                    if (dropPath) {
                        acceptDrop(dropPath);
                    }
                }
                if (ImGui::IsDragDropPayloadBeingAccepted()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) {
                clearValue();
            }
            ImGui::PopID();
        };

        for (size_t i = 0; i < t.InstancerLayers.size(); ++i) {
            TerrainInstancerLayerDesc& layer = t.InstancerLayers[i];
            std::string header = layer.Name.empty() ? ("Instancer " + std::to_string(i)) : layer.Name;
            if (ImGui::TreeNodeEx((header + "##terrain_instancer_layer_" + std::to_string(i)).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                char nameBuf[64];
                std::strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf));
                nameBuf[sizeof(nameBuf) - 1] = '\0';
                if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                    layer.Name = nameBuf;
                }
                if (ImGui::Checkbox("Enabled", &layer.Enabled)) {
                    if (layer.Enabled && layer.Instancer.Runtime.Instances.empty()) {
                        markInstancerLayerDirty(layer);
                    } else {
                        markInstancerLayerCollisionDirty(layer);
                    }
                }
                ImGui::SameLine();
                if (ImGui::RadioButton("Paint Target", t.Brush.ActiveInstancerLayer == static_cast<int>(i))) {
                    t.Brush.ActiveInstancerLayer = static_cast<int>(i);
                    t.Brush.Mode = TerrainBrushMode::Instancer;
                }

                const char* maskOptions[] = { "Painted", "Splat R", "Splat G", "Splat B", "Splat A" };
                int mask = static_cast<int>(layer.Mask);
                if (ImGui::Combo("Mask Source", &mask, maskOptions, IM_ARRAYSIZE(maskOptions))) {
                    layer.Mask = static_cast<TerrainInstancerMaskSource>(std::clamp(mask, 0, 4));
                    markInstancerLayerDirty(layer);
                }
                if (layer.Mask != TerrainInstancerMaskSource::Painted) {
                    if (ImGui::DragFloat("Splat Threshold", &layer.SplatThreshold, 0.01f, 0.0f, 1.0f, "%.2f")) {
                        markInstancerLayerDirty(layer);
                    }
                }

                ImGui::SeparatorText("Assets");
                drawInstancerAssetDropSlot(
                    "mesh_slot",
                    "Mesh Model",
                    layer.Instancer.MeshPath,
                    "Drop mesh or model",
                    "Accepts .fbx, .glb, .gltf, .obj, .meshbin, .meta",
                    [&](const char* dropPath) {
                        std::string ext = std::filesystem::path(dropPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".fbx" || ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".meshbin" || ext == ".meta") {
                            assignInstancerMesh(layer, dropPath);
                        }
                    },
                    [&]() { assignInstancerMesh(layer, std::string()); });

                drawInstancerAssetDropSlot(
                    "prefab_slot",
                    "Close Prefab",
                    layer.Instancer.PrefabPath,
                    "Drop prefab",
                    "Accepts .prefab and .prefabb",
                    [&](const char* dropPath) {
                        std::string ext = std::filesystem::path(dropPath).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".prefab" || ext == ".prefabb") {
                            assignInstancerPrefab(layer, dropPath);
                        }
                    },
                    [&]() { assignInstancerPrefab(layer, std::string()); });

                ImGui::SeparatorText("Distribution");
                auto& dist = layer.Instancer.Distribution;
                int seed = static_cast<int>(dist.Seed);
                if (ImGui::InputInt("Seed", &seed)) { dist.Seed = static_cast<uint32_t>(std::max(0, seed)); markInstancerLayerDirty(layer); }
                if (ImGui::DragFloat("Density", &dist.DensityPerSquareMeter, 0.001f, 0.0f, 10.0f, "%.4f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Min Spacing", &dist.MinSpacing, 0.1f, 0.05f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Min Scale", &dist.MinScale, 0.01f, 0.01f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Max Scale", &dist.MaxScale, 0.01f, 0.01f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);
                if (dist.MaxScale < dist.MinScale) dist.MaxScale = dist.MinScale;
                if (ImGui::Checkbox("Nonuniform Scale", &dist.NonUniformScale)) markInstancerLayerDirty(layer);
                if (dist.NonUniformScale) {
                    if (ImGui::DragFloat3("Min Scale XYZ", &dist.MinScaleVec.x, 0.01f, 0.01f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);
                    if (ImGui::DragFloat3("Max Scale XYZ", &dist.MaxScaleVec.x, 0.01f, 0.01f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);
                }
                if (ImGui::DragFloat("Yaw Variance", &dist.YawVarianceDegrees, 1.0f, 0.0f, 360.0f, "%.0f deg")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Pitch Variance", &dist.PitchVarianceDegrees, 1.0f, 0.0f, 180.0f, "%.0f deg")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Roll Variance", &dist.RollVarianceDegrees, 1.0f, 0.0f, 180.0f, "%.0f deg")) markInstancerLayerDirty(layer);
                if (ImGui::Checkbox("Align To Slope", &dist.AlignToSlope)) markInstancerLayerDirty(layer);
                if (dist.AlignToSlope && ImGui::SliderFloat("Slope Alignment", &dist.SlopeAlignmentFactor, 0.0f, 1.0f, "%.2f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Min Slope", &dist.MinSlopeDegrees, 0.1f, 0.0f, 89.0f, "%.1f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Max Slope", &dist.MaxSlopeDegrees, 0.1f, 0.0f, 89.0f, "%.1f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Height Offset", &dist.HeightOffset, 0.01f, -100.0f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);
                if (ImGui::DragFloat("Height Variance", &dist.HeightOffsetVariance, 0.01f, 0.0f, 100.0f, "%.2f")) markInstancerLayerDirty(layer);

                ImGui::SeparatorText("Culling And Prefabs");
                auto& swap = layer.Instancer.Swap;
                ImGui::DragFloat("Swap Distance", &swap.SwapDistance, 1.0f, 0.0f, 5000.0f, "%.0f");
                ImGui::DragFloat("Cull Distance", &swap.CullDistance, 1.0f, 1.0f, 20000.0f, "%.0f");
                ImGui::DragFloat("Swap Hysteresis", &swap.SwapHysteresis, 0.25f, 0.0f, 1000.0f, "%.1f");
                int maxPrefabs = static_cast<int>(swap.MaxActivePrefabs);
                if (ImGui::DragInt("Max Active Prefabs", &maxPrefabs, 1, 0, 4096)) {
                    swap.MaxActivePrefabs = static_cast<uint32_t>(std::max(0, maxPrefabs));
                }

                ImGui::SeparatorText("Collision");
                if (ImGui::Checkbox("Enable Collision", &layer.Collision.Enabled)) {
                    markInstancerLayerCollisionDirty(layer);
                }
                if (layer.Collision.Enabled) {
                    if (ImGui::DragFloat("Collision Distance", &layer.Collision.ActivationDistance, 1.0f, 0.0f, 1000.0f, "%.0f")) markInstancerLayerCollisionDirty(layer);
                    int maxBodies = static_cast<int>(layer.Collision.MaxActiveBodies);
                    if (ImGui::DragInt("Max Collision Bodies", &maxBodies, 1, 1, 4096)) {
                        layer.Collision.MaxActiveBodies = static_cast<uint32_t>(std::max(1, maxBodies));
                        markInstancerLayerCollisionDirty(layer);
                    }
                    if (DrawPhysicsLayerCombo("Physics Layer", layer.Collision.PhysicsLayer, layer.Collision.PhysicsLayerName)) {
                        markInstancerLayerCollisionDirty(layer);
                    }
                }

                ImGui::SeparatorText("Rendering");
                ImGui::Checkbox("Alpha Cutout", &layer.Instancer.UseAlphaCutout);
                if (layer.Instancer.UseAlphaCutout) {
                    ImGui::SliderFloat("Cutout Threshold", &layer.Instancer.AlphaCutoutThreshold, 0.0f, 1.0f, "%.2f");
                }
                ImGui::Checkbox("Debug Markers", &layer.Instancer.ShowDebugMarkers);

                ImGui::TextDisabled("Instances: %u  Visible: %u  Prefabs: %u  Collision: %zu",
                    layer.Instancer.Runtime.TotalInstances,
                    layer.Instancer.Runtime.VisibleInstances,
                    layer.Instancer.Runtime.ActivePrefabs,
                    layer.ActiveCollisionBodies.size());

                if (ImGui::Button("Regenerate")) {
                    markInstancerLayerDirty(layer);
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Painted Mask")) {
                    layer.EnsureMaskSize(t.GridResolution);
                    std::fill(layer.PaintedMask.begin(), layer.PaintedMask.end(), 0);
                    layer.InvalidatePaintedMaskBounds();
                    markInstancerLayerDirty(layer, true);
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove Instancer Layer")) {
                    layer.ReleaseRuntime();
                    t.InstancerLayers.erase(t.InstancerLayers.begin() + static_cast<long long>(i));
                    t.Brush.ActiveInstancerLayer = std::clamp(t.Brush.ActiveInstancerLayer, 0, std::max(0, static_cast<int>(t.InstancerLayers.size()) - 1));
                    ImGui::TreePop();
                    --i;
                    continue;
                }

                ImGui::TreePop();
            }
        }

        if (ImGui::Button("Add Instancer Layer")) {
            TerrainInstancerLayerDesc layer;
            layer.Name = "Instancer " + std::to_string(t.InstancerLayers.size());
            layer.EnsureMaskSize(t.GridResolution);
            t.InstancerLayers.push_back(std::move(layer));
        }

        ImGui::TreePop();
    }
        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Grass")) {
    if (ImGui::TreeNodeEx("Grass Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        int chunkRes = static_cast<int>(t.GrassChunkResolution);
        if (ImGui::DragInt("Chunk Resolution", &chunkRes, 1, 8, 512)) {
            t.GrassChunkResolution = std::clamp(chunkRes, 8, 1024);
            t.GrassStructureDirty = true;
        }

        // Grass sampling multiplier for denser grass on low-res terrains
        static const char* multOptions[] = { "1x", "2x", "4x", "8x", "16x" };
        static const uint32_t multValues[] = { 1, 2, 4, 8, 16 };
        int multIdx = 0;
        for (int i = 0; i < IM_ARRAYSIZE(multValues); ++i) {
            if (t.GrassSamplingMultiplier == multValues[i]) { multIdx = i; break; }
        }
        if (ImGui::Combo("Sampling Multiplier", &multIdx, multOptions, IM_ARRAYSIZE(multOptions))) {
            t.GrassSamplingMultiplier = multValues[multIdx];
            t.GrassStructureDirty = true;
            // Invalidate all grass layer runtimes to force buffer resize
            for (auto& layer : t.GrassLayers) {
                layer.RuntimeDirty = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Multiplies the grass spawning grid resolution.\n"
                              "Use higher values for dense grass on low-resolution terrains.\n"
                              "Current effective grid: %dx%d", 
                              t.GridResolution * t.GrassSamplingMultiplier,
                              t.GridResolution * t.GrassSamplingMultiplier);
        }

        auto assignGrassTexture = [&](TerrainGrassLayerDesc& layer, const std::string& path) {
            layer.BillboardTexturePath = path;
            if (!path.empty()) {
                try {
                    TextureSpecifier spec;
                    spec.Path = path;
                    layer.BillboardTexture = AcquireTextureHandle(spec, TextureColorSpace::Linear);
                } catch (...) {
                    layer.BillboardTexture = BGFX_INVALID_HANDLE;
                }
            } else {
                layer.BillboardTexture = BGFX_INVALID_HANDLE;
            }
            layer.RuntimeDirty = true;
            t.GrassStructureDirty = true;
        };

        auto assignMeshAsset = [&](TerrainGrassLayerDesc& layer, const std::string& path) {
            AssetEntry* entry = AssetLibrary::Instance().GetAsset(path);
            if (!entry) {
                ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(path);
                if (guid.high == 0 && guid.low == 0) {
                    guid = ClaymoreGUID::Generate();
                }
                AssetReference ref(guid, 0, static_cast<int>(AssetType::Mesh));
                AssetLibrary::Instance().RegisterAsset(ref, AssetType::Mesh, path, std::filesystem::path(path).filename().string());
                entry = AssetLibrary::Instance().GetAsset(path);
            }
            if (entry) {
                layer.MeshAsset = entry->reference;
                layer.MeshPath = entry->path;
                layer.RuntimeDirty = true;
                t.GrassStructureDirty = true;
            }
        };

        for (size_t i = 0; i < t.GrassLayers.size(); ++i) {
            TerrainGrassLayerDesc& layer = t.GrassLayers[i];
            std::string header = layer.Name.empty() ? ("Grass " + std::to_string(i)) : layer.Name;
            if (ImGui::TreeNodeEx((header + "##grass_layer_" + std::to_string(i)).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                char nameBuf[64];
                std::strncpy(nameBuf, layer.Name.c_str(), sizeof(nameBuf));
                nameBuf[sizeof(nameBuf) - 1] = '\0';
                if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
                    layer.Name = nameBuf;
                }
                if (ImGui::Checkbox("Enabled", &layer.Enabled)) {
                    layer.RuntimeDirty = true;
                }
                if (ImGui::Checkbox("Use GPU", &layer.UseGPU)) {
                    layer.RuntimeDirty = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Use GPU compute shaders for grass generation (experimental).\nMay have better performance for high grass counts but requires compute shader support.");
                }

                const char* renderModes[] = { "Billboard", "Billboard Fixed", "Mesh" };
                int mode = static_cast<int>(layer.RenderMode);
                if (ImGui::Combo("Render Mode", &mode, renderModes, IM_ARRAYSIZE(renderModes))) {
                    layer.RenderMode = static_cast<GrassRenderMode>(std::clamp(mode, 0, 2));
                    layer.RuntimeDirty = true;
                }

                const char* maskOptions[] = { "None", "Splat R", "Splat G", "Splat B", "Splat A", "Painted" };
                int mask = static_cast<int>(layer.Mask);
                if (ImGui::Combo("Mask Source", &mask, maskOptions, IM_ARRAYSIZE(maskOptions))) {
                    std::cout << "[GrassUI] Mask changed from " << static_cast<int>(layer.Mask) << " to " << mask << std::endl;
                    layer.Mask = static_cast<GrassMaskSource>(std::clamp(mask, 0, 5));
                    std::cout << "[GrassUI] layer.Mask is now " << static_cast<int>(layer.Mask) << std::endl;
                    layer.RuntimeDirty = true;
                    t.GrassMasksDirty = true;
                }

                const bool usesSplatMask = (layer.Mask == GrassMaskSource::SplatR ||
                                            layer.Mask == GrassMaskSource::SplatG ||
                                            layer.Mask == GrassMaskSource::SplatB ||
                                            layer.Mask == GrassMaskSource::SplatA);
                if (usesSplatMask) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Splat Procedural");
                    int splatSeed = static_cast<int>(layer.SplatSeed);
                    if (ImGui::InputInt("Splat Seed", &splatSeed)) {
                        splatSeed = std::max(0, splatSeed);
                        layer.SplatSeed = static_cast<uint32_t>(splatSeed);
                        layer.RuntimeDirty = true;
                    }
                    ImGui::SetItemTooltip("Seed used to stabilize procedural splat distribution.");

                    if (ImGui::DragFloat("Splat Noise Scale", &layer.SplatNoiseScale, 0.01f, 0.0f, 10.0f, "%.3f")) layer.RuntimeDirty = true;
                    ImGui::SetItemTooltip("Higher values add smaller-scale variation.");
                    if (ImGui::DragFloat("Splat Noise Strength", &layer.SplatNoiseStrength, 0.01f, 0.0f, 1.0f, "%.2f")) layer.RuntimeDirty = true;
                    ImGui::SetItemTooltip("0 disables noise; 1 fully uses noise for density.");
                    if (ImGui::DragFloat("Splat Threshold", &layer.SplatThreshold, 0.01f, 0.0f, 1.0f, "%.2f")) layer.RuntimeDirty = true;
                    ImGui::SetItemTooltip("Minimum splatmap channel value (0-1) required for grass to spawn. Higher values provide cleaner separation from other textures when painting.");
                }

                if (ImGui::DragFloat("Density", &layer.DensityPerSquareMeter, 1.0f, 1.0f, 1000.0f, "%.0f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat2("Scale Range", &layer.ScaleRange.x, 0.01f, 0.01f, 10.0f, "%.2f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat("Random Yaw", &layer.RandomYawDegrees, 0.1f, 0.0f, 360.0f, "%.1f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat2("Height Range", &layer.HeightRange.x, 0.1f, -1000.0f, 1000.0f, "%.1f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat("Max Slope", &layer.MaxSlopeDegrees, 0.1f, 0.0f, 89.0f, "%.1f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat("Min Distance", &layer.MinDistance, 0.1f, 0.0f, 500.0f, "%.1f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat("Max Distance", &layer.MaxDistance, 0.1f, 0.0f, 500.0f, "%.1f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat("Wind Strength", &layer.WindStrength, 0.01f, 0.0f, 10.0f, "%.2f")) layer.RuntimeDirty = true;
                if (ImGui::DragFloat("Wind Direction", &layer.WindDirectionDegrees, 1.0f, 0.0f, 360.0f, "%.0f deg")) layer.RuntimeDirty = true;
                ImGui::SetItemTooltip("Wind direction in degrees (0 = +X, 90 = +Z). Used for consistent sway in BillboardFixed mode.");
                if (ImGui::ColorEdit3("Base Color", &layer.BaseColor.x)) layer.RuntimeDirty = true;
                if (ImGui::ColorEdit3("Color Variance", &layer.ColorVariance.x)) layer.RuntimeDirty = true;

                if (layer.RenderMode == GrassRenderMode::Billboard || layer.RenderMode == GrassRenderMode::BillboardFixed) {
                    ImGui::Text("Billboard Texture");
                    if (!layer.BillboardTexturePath.empty()) {
                        ImGui::TextDisabled("%s", layer.BillboardTexturePath.c_str());
                    } else {
                        ImGui::TextDisabled("Drop a texture to assign.");
                    }
                    ImGui::PushID(static_cast<int>(i));
                    ImVec2 preview(56, 56);
                    bool requestPicker = false;
                    ImTextureID texId = TextureLoader::ToImGuiTextureID(layer.BillboardTexture);
                    if (texId) {
                        ImGui::Image(texId, preview);
                    } else {
                        if (ImGui::Button("Assign##grass_tex", preview)) {
                            requestPicker = true;
                        }
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                            const char* dropPath = static_cast<const char*>(payload->Data);
                            if (dropPath) {
                                std::string ext = std::filesystem::path(dropPath).extension().string();
                                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") {
                                    assignGrassTexture(layer, dropPath);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (requestPicker) {
                        ImGui::OpenPopup("GrassTexturePicker");
                    }
                    texturepicker::DrawTexturePickerPopup("GrassTexturePicker", [&](const std::string& selectedPath) {
                        assignGrassTexture(layer, selectedPath);
                    }, layer.BillboardTexturePath);
                    if (ImGui::Button("Clear Texture")) {
                        assignGrassTexture(layer, std::string());
                    }
                    ImGui::PopID();
                } else {
                    ImGui::Text("Mesh Asset");
                    if (!layer.MeshPath.empty()) {
                        ImGui::TextDisabled("%s", layer.MeshPath.c_str());
                    } else if (layer.MeshAsset.IsValid()) {
                        ImGui::TextDisabled("GUID: %s", layer.MeshAsset.guid.ToString().c_str());
                    } else {
                        ImGui::TextDisabled("Drop a .meshbin asset to assign.");
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                            const char* dropPath = static_cast<const char*>(payload->Data);
                            if (dropPath) {
                                std::string ext = std::filesystem::path(dropPath).extension().string();
                                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                                if (ext == ".meshbin") {
                                    assignMeshAsset(layer, dropPath);
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                    if (ImGui::Button("Clear Mesh")) {
                        layer.MeshAsset = AssetReference{};
                        layer.MeshPath.clear();
                        layer.Mesh.reset();
                        layer.RuntimeDirty = true;
                        t.GrassStructureDirty = true;
                    }
                }

                if (ImGui::Button("Clear Painted Mask")) {
                    std::fill(layer.PaintedMask.begin(), layer.PaintedMask.end(), 0);
                    layer.RuntimeDirty = true;
                    t.GrassMasksDirty = true;
                }

                if (ImGui::Button("Remove Grass Layer")) {
                    t.GrassLayers.erase(t.GrassLayers.begin() + static_cast<long long>(i));
                    t.Brush.ActiveGrassLayer = std::clamp(t.Brush.ActiveGrassLayer, 0, std::max(0, static_cast<int>(t.GrassLayers.size()) - 1));
                    t.GrassStructureDirty = true;
                    ImGui::TreePop();
                    --i;
                    continue;
                }

                ImGui::TreePop();
            }
        }

        if (ImGui::Button("Add Grass Layer")) {
            TerrainGrassLayerDesc layer;
            layer.Name = "Grass " + std::to_string(t.GrassLayers.size());
            layer.EnsureMaskSize(t.GridResolution);
            t.GrassLayers.push_back(std::move(layer));
            t.GrassStructureDirty = true;
        }

        ImGui::TreePop();
    }
        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
    });


    registry.Register<CameraComponent>("Camera", [](CameraComponent& c) {
        ImGui::ClayInspectorContentScope scope("CameraGrid");
        bool projDirty = false;

        bool active = c.Active;
        if (ImGui::ClayFieldCheckbox(scope, "Active", &active)) {
            c.Active = active;
        }

        int priority = c.priority;
        if (ImGui::ClayFieldInt(scope, "Priority", &priority, 1)) {
            c.priority = priority;
        }

        scope.BeginRow("Projection");
        bool persp = c.IsPerspective;
        if (ImGui::RadioButton("Perspective", persp)) persp = true;
        ImGui::SameLine();
        if (ImGui::RadioButton("Orthographic", !persp)) persp = false;
        if (persp != c.IsPerspective) {
            c.IsPerspective = persp;
            projDirty = true;
        }
        scope.EndRow();

        float fov = c.FieldOfView;
        if (ImGui::ClayFieldSlider(scope, "Field of View", &fov, 1.0f, 179.0f, "%.1f")) {
            c.FieldOfView = fov;
            projDirty = true;
        }

        float nearClip = c.NearClip;
        if (ImGui::ClayFieldFloat(scope, "Near Clip", &nearClip, 0.001f)) {
            c.NearClip = glm::max(0.001f, nearClip);
            projDirty = true;
        }

        float farClip = c.FarClip;
        if (ImGui::ClayFieldFloat(scope, "Far Clip", &farClip, 1.0f)) {
            c.FarClip = glm::max(c.NearClip + 0.01f, farClip);
            projDirty = true;
        }

        scope.BeginRow("Layer Mask");
        ImGui::InputScalar("##LayerMaskValue", ImGuiDataType_U32, &c.LayerMask, nullptr, nullptr, "%08X");
        scope.EndRow();

        if (projDirty) {
            float aspectRatio = (float)Renderer::Get().GetWidth() / (float)Renderer::Get().GetHeight();
            c.UpdateProjection(aspectRatio);
        }
    });

    // AnimationPlayer (Animator) drawer
    registry.Register<cm::animation::AnimationPlayerComponent>("Animator", [](cm::animation::AnimationPlayerComponent& ap) {
        // Ensure at least one state exists
        if (ap.ActiveStates.empty()) ap.ActiveStates.push_back({});

        ImGui::DragFloat("Playback Speed", &ap.PlaybackSpeed, 0.01f, 0.0f, 5.0f);

        // Loop flag applies to the first active state
        bool loop = ap.ActiveStates.front().Loop;
        if (ImGui::Checkbox("Loop", &loop)) {
            ap.ActiveStates.front().Loop = loop;
        }

        // Root Motion routing is automatic (self/direct parent physics, else transform).
        ImGui::TextDisabled("Root Motion");
        ImGui::TextDisabled("Auto Driver: Self/Parent CharacterController or Kinematic RigidBody, else Transform");
        
        // Note about per-animation settings
        ImGui::TextDisabled("(Root motion mode is set per-animation in import settings)");

        // Quick Play Animation section - for simple single-clip use without a controller file
        // This shows when no controller file is assigned
        if (ap.ControllerPath.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Quick Play Animation");
            // Single clip path text + choose from registry
            if (!ap.SingleClipPath.empty()) {
                ImGui::Text("Clip: %s", ap.SingleClipPath.c_str());
            } else {
                ImGui::TextDisabled("Clip: (None)");
            }
            ImGui::Checkbox("Play on Start", &ap.PlayOnStart);
            ImGui::Checkbox("Playing", &ap.IsPlaying);

            const auto& animOptions = ui::GetAnimationAssetOptions();
            auto resolveCurrentIndex = [&](const std::string& path) -> int {
                for (int i = 0; i < static_cast<int>(animOptions.size()); ++i) {
                    if (animOptions[i].path == path) return i;
                }
                return -1;
            };
            int currentIndex = resolveCurrentIndex(ap.SingleClipPath);
            const char* currentLabel = (currentIndex >= 0)
                                           ? animOptions[currentIndex].name.c_str()
                                           : "<Select Clip>";
            if (ImGui::BeginCombo("##AnimDropdown", currentLabel)) {
                for (int i = 0; i < static_cast<int>(animOptions.size()); ++i) {
                    bool isSelected = (i == currentIndex);
                    if (ImGui::Selectable(animOptions[i].name.c_str(), isSelected)) {
                        auto assetPtr = cm::animation::LoadAnimationAssetCached(animOptions[i].path, true);
                        if (assetPtr && AnimationAssetHasSkeletalTracks(*assetPtr)) {
                            ap.SingleClipPath = animOptions[i].path;
                            ap._InitApplied = false; // allow PlayOnStart to apply on next run
                            ap._AutoControllerGenerated = false; // force re-generation of controller
                            ap.CachedAssets[0] = assetPtr;
                            if (ap.ActiveStates.empty()) ap.ActiveStates.push_back({});
                            ap.ActiveStates.front().Asset = assetPtr.get();
                            ap.ActiveStates.front().LegacyClip = nullptr;
                            ap.Controller.reset();
                            ap.CurrentStateId = -1;
                            ap.Debug_CurrentAnimationName = animOptions[i].name;
                        } else {
                            ap.Debug_CurrentAnimationName = std::string("(Non-skeletal) ") + animOptions[i].name;
                        }
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }

        // Debug info
        if (!ap.Debug_CurrentAnimationName.empty()) {
            ImGui::Text("Now Playing: %s", ap.Debug_CurrentAnimationName.c_str());
        }
        // Show controller info when a controller file is assigned
        if (!ap.ControllerPath.empty()) {
            if (!ap.Debug_CurrentControllerStateName.empty()) {
                ImGui::Text("Controller State: %s", ap.Debug_CurrentControllerStateName.c_str());
            }
            ImGui::Text("Playing: %s", ap.IsPlaying ? "yes" : "no");
            // Live controller parameter view
            if (ap.Controller) {
                ImGui::Separator();
                ImGui::TextDisabled("Parameters");
                auto& bb = ap.AnimatorInstance.Blackboard();
                for (const auto& p : ap.Controller->Parameters) {
                    switch (p.Type) {
                        case cm::animation::AnimatorParamType::Bool: {
                            bool v = false; auto it = bb.Bools.find(p.Name); if (it != bb.Bools.end()) v = it->second;
                            ImGui::Text("%s = %s", p.Name.c_str(), v ? "true" : "false");
                        } break;
                        case cm::animation::AnimatorParamType::Int: {
                            int v = 0; auto it = bb.Ints.find(p.Name); if (it != bb.Ints.end()) v = it->second;
                            ImGui::Text("%s = %d", p.Name.c_str(), v);
                        } break;
                        case cm::animation::AnimatorParamType::Float: {
                            float v = 0.0f; auto it = bb.Floats.find(p.Name); if (it != bb.Floats.end()) v = it->second;
                            ImGui::Text("%s = %.3f", p.Name.c_str(), v);
                        } break;
                        case cm::animation::AnimatorParamType::Trigger: {
                            bool v = false; auto it = bb.Triggers.find(p.Name); if (it != bb.Triggers.end()) v = it->second;
                            ImGui::Text("%s (trigger) = %s", p.Name.c_str(), v ? "set" : "unset");
                        } break;
                    }
                }

                // Live transition diagnostics from current state
                ImGui::Separator();
                const int curId = ap.CurrentStateId;
                const cm::animation::AnimatorState* curSt = ap.Controller->FindState(curId);
                if (curSt) {
                    ImGui::TextDisabled("Transitions from '%s' (id=%d)", curSt->Name.c_str(), curId);
                    auto evalCond = [&](const cm::animation::AnimatorCondition& c)->bool{
                        using cm::animation::ConditionMode;
                        switch (c.Mode) {
                            case ConditionMode::If: { auto it=bb.Bools.find(c.Parameter); return it!=bb.Bools.end() && it->second; }
                            case ConditionMode::IfNot: { auto it=bb.Bools.find(c.Parameter); return it!=bb.Bools.end() && !it->second; }
                            case ConditionMode::Greater: {
                                auto itf=bb.Floats.find(c.Parameter); if(itf!=bb.Floats.end()) return itf->second>c.Threshold;
                                auto iti=bb.Ints.find(c.Parameter);   if(iti!=bb.Ints.end())   return iti->second>c.IntThreshold; return false; }
                            case ConditionMode::Less: {
                                auto itf=bb.Floats.find(c.Parameter); if(itf!=bb.Floats.end()) return itf->second<c.Threshold;
                                auto iti=bb.Ints.find(c.Parameter);   if(iti!=bb.Ints.end())   return iti->second<c.IntThreshold; return false; }
                            case ConditionMode::Equals: {
                                auto itf=bb.Floats.find(c.Parameter); if(itf!=bb.Floats.end()) return itf->second==c.Threshold;
                                auto iti=bb.Ints.find(c.Parameter);   if(iti!=bb.Ints.end())   return iti->second==c.IntThreshold; return false; }
                            case ConditionMode::NotEquals: {
                                auto itf=bb.Floats.find(c.Parameter); if(itf!=bb.Floats.end()) return itf->second!=c.Threshold;
                                auto iti=bb.Ints.find(c.Parameter);   if(iti!=bb.Ints.end())   return iti->second!=c.IntThreshold; return false; }
                            case ConditionMode::Trigger: { auto it=bb.Triggers.find(c.Parameter); return it!=bb.Triggers.end() && it->second; }
                        }
                        return false;
                    };
                    for (const auto& tr : ap.Controller->Transitions) {
                        if (tr.FromState != curId) continue;
                        bool ok = true; for (const auto& c : tr.Conditions) { if (!evalCond(c)) { ok=false; break; } }
                        const auto* toSt = ap.Controller->FindState(tr.ToState);
                        ImGui::Text("-> %s (id=%d): %s", toSt?toSt->Name.c_str():"?", tr.ToState, ok?"match":"no match");
                    }
                }
            }
        }
    });

    registry.Register<RigidBodyComponent>("RigidBody", [](RigidBodyComponent& rb) {
        // Physics layer
        DrawPhysicsLayerCombo("Physics Layer##RigidBodyPhysicsLayer", rb.PhysicsLayer, rb.PhysicsLayerName);
        if (DrawPhysicsLayerMaskCombo("Collision Mask##RigidBodyCollisionMask", rb.CollisionMask) &&
            !rb.BodyID.IsInvalid() && Physics::GetSystem()) {
            Physics::GetBodyInterface().ActivateBody(rb.BodyID);
        }
        const bool scenePlaying = Scene::Get().m_IsPlaying;
        
        ImGui::DragFloat("Mass", &rb.Mass, 0.1f, 0.01f, 1000.0f);
        ImGui::DragFloat("Friction", &rb.Friction, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Restitution", &rb.Restitution, 0.01f, 0.0f, 1.0f);
        ImGui::Checkbox("Use Gravity", &rb.UseGravity);
        ImGui::Checkbox("Is Kinematic", &rb.IsKinematic);
        
        if (rb.IsKinematic) {
            ImGui::Separator();
            ImGui::Text("Kinematic Properties:");
#if !defined(CLAYMORE_RUNTIME)
            if (scenePlaying) {
                glm::vec3 displayLinearVelocity = rb._EditorDisplayLinearVelocity;
                ImGui::BeginDisabled();
                ImGui::DragFloat3("Linear Velocity", &displayLinearVelocity.x, 0.1f);
                ImGui::EndDisabled();
            } else
#endif
            {
                ImGui::DragFloat3("Linear Velocity", &rb.LinearVelocity.x, 0.1f);
            }
            ImGui::DragFloat3("Angular Velocity", &rb.AngularVelocity.x, 0.1f);
        }
    });

    registry.Register<StaticBodyComponent>("StaticBody", [](StaticBodyComponent& sb) {
        // Physics layer
        DrawPhysicsLayerCombo("Physics Layer##StaticBodyPhysicsLayer", sb.PhysicsLayer, sb.PhysicsLayerName);
        
        ImGui::DragFloat("Friction", &sb.Friction, 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Restitution", &sb.Restitution, 0.01f, 0.0f, 1.0f);
    });

    // Character Controller (Jolt CharacterVirtual)
    registry.Register<CharacterControllerComponent>("CharacterController", [](CharacterControllerComponent& cc) {
        // Physics layer
        DrawPhysicsLayerCombo("Physics Layer##CharacterControllerPhysicsLayer", cc.PhysicsLayer, cc.PhysicsLayerName);
        const bool scenePlaying = Scene::Get().m_IsPlaying;
        
        ImGui::DragFloat("Radius", &cc.Radius, 0.01f, 0.05f, 10.0f);
        ImGui::DragFloat("Height", &cc.Height, 0.01f, 0.3f, 10.0f);
        ImGui::DragFloat3("Up", &cc.Up.x, 0.01f);
        ImGui::DragFloat3("Offset", &cc.Offset.x, 0.01f);
        ImGui::DragFloat("Max Slope (deg)", &cc.MaxSlopeDegrees, 0.1f, 0.0f, 89.0f);
        ImGui::Checkbox("Stick To Floor", &cc.StickToFloor);
        ImGui::Checkbox("Walk Stairs", &cc.EnableWalkStairs);
        ImGui::DragFloat("Jump Speed", &cc.JumpSpeed, 0.01f, 0.0f, 50.0f);
        ImGui::TextDisabled("Runtime");
        ImGui::Text("Grounded: %s", cc.IsGrounded ? "true" : "false");
#if !defined(CLAYMORE_RUNTIME)
        if (scenePlaying) {
            glm::vec3 displayDesiredVelocity = cc._EditorDisplayDesiredVelocity;
            ImGui::BeginDisabled();
            ImGui::DragFloat3("Desired Velocity", &displayDesiredVelocity.x, 0.1f);
            ImGui::EndDisabled();
        } else
#endif
        {
            ImGui::DragFloat3("Desired Velocity", &cc.DesiredVelocity.x, 0.1f);
        }
    });

    // Text Renderer
    registry.Register<TextRendererComponent>("TextRenderer", [](TextRendererComponent& t) {
        ImGui::PushID(&t);
        if (ShouldDrawUITextRenderSpaceControls(t))
            DrawUIWorldSpaceControls();

        // Helper to process escape sequences (literal \n \t \r to actual newline/tab/cr)
        auto processEscapes = [](const std::string& input) -> std::string {
            std::string result;
            result.reserve(input.size());
            for (size_t i = 0; i < input.size(); ++i) {
                if (input[i] == '\\' && i + 1 < input.size()) {
                    char next = input[i + 1];
                    if (next == 'n') { result += '\n'; ++i; continue; }
                    if (next == 't') { result += '\t'; ++i; continue; }
                    if (next == 'r') { result += '\r'; ++i; continue; }
                    if (next == '\\') { result += '\\'; ++i; continue; }
                }
                result += input[i];
            }
            return result;
        };
        
        // Helper to display text with escapes shown visually in editor
        auto escapeForDisplay = [](const std::string& input) -> std::string {
            std::string result;
            result.reserve(input.size() * 2);
            for (char c : input) {
                if (c == '\n') { result += "\\n"; }
                else if (c == '\t') { result += "\\t"; }
                else if (c == '\r') { result += "\\r"; }
                else { result += c; }
            }
            return result;
        };
        
        // Text field - show actual newlines in the multiline input
        static char buffer[1024];
        strncpy(buffer, t.Text.c_str(), sizeof(buffer)-1);
        buffer[sizeof(buffer)-1] = '\0';
        if (ImGui::InputTextMultiline("Text", buffer, sizeof(buffer), ImVec2(-1, 80))) {
            // Process escape sequences when text changes
            t.Text = processEscapes(buffer);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Escape sequences: \\n (newline), \\t (tab), \\\\ (backslash)");
        }

        // Size and color
        ImGui::DragFloat("Pixel Size", &t.PixelSize, 1.0f, 6.0f, 256.0f);
        const char* textAlignments[] = { "Left", "Center", "Right" };
        int textAlignment = (int)t.TextAlignment;
        if (ImGui::Combo("Alignment", &textAlignment, textAlignments, IM_ARRAYSIZE(textAlignments))) {
            t.TextAlignment = (TextRendererComponent::Alignment)textAlignment;
        }

        // Convert ABGR to ImGui RGBA for UI editing
        ImVec4 col = ImGui::ColorConvertU32ToFloat4(t.ColorAbgr);
        if (ImGui::ColorEdit4("Color", &col.x)) {
            t.ColorAbgr = ImGui::ColorConvertFloat4ToU32(col);
        }

        ImGui::Checkbox("Outline", &t.OutlineEnabled);
        if (t.OutlineEnabled) {
            ImVec4 outlineCol = ImGui::ColorConvertU32ToFloat4(t.OutlineColorAbgr);
            if (ImGui::ColorEdit4("Outline Color", &outlineCol.x)) {
                t.OutlineColorAbgr = ImGui::ColorConvertFloat4ToU32(outlineCol);
            }
            ImGui::DragFloat("Outline Thickness", &t.OutlineThickness, 0.1f, 0.0f, 16.0f);
        }

        ImGui::Checkbox("Drop Shadow", &t.ShadowEnabled);
        if (t.ShadowEnabled) {
            ImVec4 shadowCol = ImGui::ColorConvertU32ToFloat4(t.ShadowColorAbgr);
            if (ImGui::ColorEdit4("Shadow Color", &shadowCol.x)) {
                t.ShadowColorAbgr = ImGui::ColorConvertFloat4ToU32(shadowCol);
            }
            ImGui::DragFloat2("Shadow Offset", &t.ShadowOffset.x, 0.25f, -64.0f, 64.0f);
        }

        ImGui::Checkbox("Visible", &t.Visible);
        ImGui::DragInt("Z Order", &t.ZOrder, 1, -1000, 1000);
        ImGui::SliderFloat("Opacity", &t.Opacity, 0.0f, 1.0f);
        ImGui::Separator();
        ImGui::TextDisabled("Wrapping");
        ImGui::Checkbox("Word Wrap", &t.WordWrap);
        ImGui::DragFloat2("Rect Size", &t.RectSize.x, 1.0f, 0.0f, 4096.0f);

        // Font selection from Asset Library (registered TTFs)
        {
            auto assets = AssetLibrary::Instance().GetAllAssets();
            std::vector<std::string> fontPaths; fontPaths.reserve(assets.size());
            for (auto& tup : assets) {
                const std::string& path = std::get<0>(tup);
                AssetType type = std::get<2>(tup);
                if (type == AssetType::Font) { fontPaths.push_back(path); continue; }
                // Back-compat: also pick up .ttf/.otf if registered before Font type existed
                std::string lower = path; std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.size() >= 4 && (lower.rfind(".ttf") == lower.size()-4 || lower.rfind(".otf") == lower.size()-4)) fontPaths.push_back(path);
            }
            int cur = 0;
            if (!t.FontPath.empty()) {
                for (int i=0;i<(int)fontPaths.size();++i) if (fontPaths[i] == t.FontPath) { cur = i; break; }
            }
            if (!fontPaths.empty()) {
                // Robust combo using BeginCombo/Selectable to avoid edge-cases with transient arrays
                std::string current = (!t.FontPath.empty() ? t.FontPath : fontPaths[0]);
                std::string currentLabel = std::filesystem::path(current).filename().string();
                if (ImGui::BeginCombo("Font", currentLabel.c_str())) {
                    for (int i = 0; i < (int)fontPaths.size(); ++i) {
                        const std::string& path = fontPaths[i];
                        bool selected = (path == t.FontPath) || (t.FontPath.empty() && i == 0);
                        std::string label = std::filesystem::path(path).filename().string();
                        if (ImGui::Selectable(label.c_str(), selected)) {
                            t.FontPath = path;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextDisabled("No fonts registered (.ttf)");
            }
        }

        const bool hasCanvasBackedUIEntity = TextUsesCanvasBackedUI(t);
        if (hasCanvasBackedUIEntity) {
            bool standaloneWorldText = t.WorldSpace;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Standalone World Text", &standaloneWorldText);
            ImGui::EndDisabled();
            ImGui::TextDisabled("Canvas-backed UI text uses the entity render-space controls above");
        } else {
            ImGui::Checkbox("Standalone World Text", &t.WorldSpace);
        }

        if (t.WorldSpace && !hasCanvasBackedUIEntity) {
            ImGui::Checkbox("Billboard", &t.Billboard);
        }

        if (!t.WorldSpace) {
            ImGui::Separator();
            ImGui::TextDisabled("UI Anchoring");

            // Binary mode: either parent anchor OR screen anchor, never both
            bool parentAnchor = t.AnchorToParentUI;
            if (ImGui::Checkbox("Anchor To Parent UI", &parentAnchor)) {
                t.AnchorToParentUI = parentAnchor;
                if (parentAnchor) {
                    t.AnchorEnabled = false;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, text is positioned relative to the parent UI rect (Panel/UIRect) "
                                  "using the parent's computed bounds and this entity's UIRect (if present).");
            }

            bool screenAnchor = t.AnchorEnabled;
            if (ImGui::Checkbox("Use Screen Anchor", &screenAnchor)) {
                t.AnchorEnabled = screenAnchor;
                if (screenAnchor) {
                    t.AnchorToParentUI = false;
                }
            }

            if (t.AnchorEnabled) {
                const char* anchors[] = { "TopLeft","Top","TopRight","Left","Center","Right","BottomLeft","Bottom","BottomRight" };
                int a = (int)t.Anchor;
                if (ImGui::Combo("Anchor", &a, anchors, IM_ARRAYSIZE(anchors))) {
                    t.Anchor = (UIAnchorPreset)a;
                }
            }

            if (t.AnchorEnabled || t.AnchorToParentUI) {
                ImGui::DragFloat2("Offset", &t.AnchorOffset.x, 1.0f);
            } else {
                ImGui::Text("Screen Position = Transform.Position.xy");
            }
        }
        ImGui::PopID();
    });

    // Canvas drawer
    // Helper for drawing AssetReference texture slots (like mesh albedo picker)
    auto drawUITextureSlot = [](const char* label, AssetReference& texRef) {
        ImGui::PushID(label);
        ImGui::Text("%s", label);
        
        // Get current texture handle and path for preview
        bgfx::TextureHandle currentTex = BGFX_INVALID_HANDLE;
        std::string currentPath;
        if (texRef.IsValid()) {
            if (auto* entry = AssetLibrary::Instance().GetAsset(texRef)) {
                currentPath = AssetLibrary::Instance().GetPathForGUID(texRef.guid);
                if (!entry->texture || !bgfx::isValid(*entry->texture)) {
                    auto tex = AssetLibrary::Instance().LoadTexture(texRef);
                    (void)tex;
                }
                if (entry->texture && bgfx::isValid(*entry->texture)) {
                    currentTex = *entry->texture;
                }
            }
        }
        
        // Show current path if valid
        if (!currentPath.empty()) {
            std::string filename = std::filesystem::path(currentPath).filename().string();
            ImGui::TextDisabled("%s", filename.c_str());
        } else {
            ImGui::TextDisabled("Drag texture or click to browse");
        }
        
        // Thumbnail preview with click-to-browse
        ImTextureID texId = bgfx::isValid(currentTex) ? TextureLoader::ToImGuiTextureID(currentTex) : 0;
        ImVec2 size(64, 64);
        bool requestPicker = false;
        
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        if (texId) {
            if (ImGui::ImageButton("##texpreview", texId, size)) {
                requestPicker = true;
            }
        } else {
            if (ImGui::Button("##texslot", size)) {
                requestPicker = true;
            }
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        
        // Drag-drop target
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                const char* path = static_cast<const char*>(payload->Data);
                if (path) {
                    std::string ext = std::filesystem::path(path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp" || ext == ".hdr") {
                        AssetEntry* entry = AssetLibrary::Instance().GetAsset(std::string(path));
                        if (!entry) {
                            ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(path);
                            if (guid.high == 0 && guid.low == 0) {
                                guid = ClaymoreGUID::Generate();
                            }
                            AssetReference aref(guid, 0, (int)AssetType::Texture);
                            AssetLibrary::Instance().RegisterAsset(aref, AssetType::Texture, path, std::filesystem::path(path).filename().string());
                            entry = AssetLibrary::Instance().GetAsset(path);
                        }
                        if (entry) {
                            texRef = entry->reference;
                            auto tex = AssetLibrary::Instance().LoadTexture(texRef);
                            (void)tex;
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        
        // Texture picker popup
        if (requestPicker) {
            ImGui::OpenPopup("UITexturePicker");
        }
        texturepicker::DrawTexturePickerPopup("UITexturePicker",
            [&texRef](const std::string& path) {
                AssetEntry* entry = AssetLibrary::Instance().GetAsset(path);
                if (!entry) {
                    ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(path);
                    if (guid.high == 0 && guid.low == 0) {
                        guid = ClaymoreGUID::Generate();
                    }
                    AssetReference aref(guid, 0, (int)AssetType::Texture);
                    AssetLibrary::Instance().RegisterAsset(aref, AssetType::Texture, path, std::filesystem::path(path).filename().string());
                    entry = AssetLibrary::Instance().GetAsset(path);
                }
                if (entry) {
                    texRef = entry->reference;
                    auto tex = AssetLibrary::Instance().LoadTexture(texRef);
                    (void)tex;
                }
            }, currentPath);
        
        // Clear button
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            texRef = AssetReference();
        }
        
        ImGui::PopID();
    };

    registry.Register<CanvasComponent>("Canvas", [](CanvasComponent& c){
        ImGui::PushID(&c);
        DrawUIWorldSpaceControls();
        ImGui::DragInt("Width", &c.Width, 1, 0, 16384);
        ImGui::DragInt("Height", &c.Height, 1, 0, 16384);
        ImGui::DragFloat("DPI Scale", &c.DPIScale, 0.01f, 0.25f, 4.0f);
        ImGui::DragInt("Sort Order", &c.SortOrder, 1, -1000, 1000);
        ImGui::SliderFloat("Opacity", &c.Opacity, 0.0f, 1.0f);
        ImGui::Checkbox("Block Scene Input", &c.BlockSceneInput);
        
        // Reference Resolution for resolution-independent UI
        ImGui::Separator();
        ImGui::Text("Reference Resolution");
        ImGui::DragInt("Ref Width", &c.ReferenceWidth, 1, 0, 7680);
        ImGui::DragInt("Ref Height", &c.ReferenceHeight, 1, 0, 4320);
        int scaleMode = (int)c.ReferenceScaleMode;
        const char* scaleModes[] = { "Constant Pixel Size", "Scale With Width", "Scale With Height", "Scale With Smallest", "Scale With Largest", "Expand" };
        ImGui::Combo("Scale Mode", &scaleMode, scaleModes, IM_ARRAYSIZE(scaleModes));
        c.ReferenceScaleMode = (CanvasComponent::ScaleMode)scaleMode;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Constant Pixel Size: No scaling (legacy behavior)\n"
                "Scale With Width: Scale based on width ratio\n"
                "Scale With Height: Scale based on height ratio\n"
                "Scale With Smallest: Use smaller scale (UI never overflows)\n"
                "Scale With Largest: Use larger scale (UI always fills)\n"
                "Expand: Independent scaling per axis"
            );
        }
        ImGui::PopID();
    });

    // Panel drawer
    registry.Register<PanelComponent>("Panel", [drawUITextureSlot](PanelComponent& p){
        // Scope IDs by component address to avoid collisions with Transform controls (e.g., Scale)
        ImGui::PushID(&p);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Visible", &p.Visible);
        ImGui::Checkbox("Allow Drag", &p.AllowDrag);
        ImGui::Checkbox("Allow Drop", &p.AllowDrop);
        ImGui::Checkbox("Drive Children Opacity", &p.DriveChildrenOpacity);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled, descendant UI elements multiply their own opacity by this panel's opacity at render time.");
        }
        ImGui::DragFloat2("Size", &p.Size.x, 1.0f, 0.0f, 10000.0f);
        ImGui::DragFloat2("Scale", &p.Scale.x, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Rotation", &p.Rotation, 0.1f, -360.0f, 360.0f);
        ImGui::DragInt("Z Order", &p.ZOrder, 1, -1000, 1000);
        ImGui::Separator();
        drawUITextureSlot("Texture", p.Texture);
        ImGui::Separator();
        ImGui::TextDisabled("Anchoring");
        ImGui::Checkbox("Use Anchor", &p.AnchorEnabled);
        if (p.AnchorEnabled) {
            ImGui::Checkbox("Anchor To Parent UI", &p.AnchorToParentUI);
            const char* anchors[] = { "TopLeft","Top","TopRight","Left","Center","Right","BottomLeft","Bottom","BottomRight" };
            int a = (int)p.Anchor;
            if (ImGui::Combo("Anchor", &a, anchors, IM_ARRAYSIZE(anchors))) p.Anchor = (UIAnchorPreset)a;
            ImGui::DragFloat2("Offset", &p.AnchorOffset.x, 1.0f);
        } else {
            ImGui::DragFloat2("Position", &p.Position.x, 1.0f);
            ImGui::DragFloat2("Pivot", &p.Pivot.x, 0.01f, 0.0f, 1.0f);
        }
        ImGui::ColorEdit4("Tint", &p.TintColor.x);
        ImGui::SliderFloat("Opacity", &p.Opacity, 0.0f, 1.0f);
        ImGui::DragFloat4("UV Rect", &p.UVRect.x, 0.001f, 0.0f, 1.0f);
        // Fill mode & theming
        const char* modes[] = { "Stretch", "Tile", "NineSlice" };
        int m = (int)p.Mode;
        if (ImGui::Combo("Fill Mode", &m, modes, IM_ARRAYSIZE(modes))) p.Mode = (PanelComponent::FillMode)m;
        if (p.Mode == PanelComponent::FillMode::Tile) {
            ImGui::DragFloat2("Tile Repeat", &p.TileRepeat.x, 0.01f, 0.01f, 1000.0f);
        }
        if (p.Mode == PanelComponent::FillMode::NineSlice) {
            // Get texture dimensions for pixel-based editing
            int texW = 0, texH = 0;
            std::string texPath;
            if (p.Texture.IsValid()) {
                // Try to get path from asset entry first
                if (auto* entry = AssetLibrary::Instance().GetAsset(p.Texture)) {
                    texPath = entry->path;
                }
                // Fallback to GUID lookup
                if (texPath.empty()) {
                    texPath = AssetLibrary::Instance().GetPathForGUID(p.Texture.guid);
                }
                // Make path absolute if relative
                if (!texPath.empty() && !std::filesystem::path(texPath).is_absolute()) {
                    texPath = (Project::GetProjectDirectory() / texPath).string();
                }
                if (!texPath.empty() && std::filesystem::exists(texPath)) {
                    int c = 0;
                    stbi_info(texPath.c_str(), &texW, &texH, &c);
                }
            }
            
            if (texW > 0 && texH > 0) {
                ImGui::TextDisabled("Texture: %dx%d", texW, texH);
                
                // Calculate current pixel values from SliceUV
                glm::vec4 texBorderPx = {
                    p.SliceUV.x * texW,
                    p.SliceUV.y * texH,
                    p.SliceUV.z * texW,
                    p.SliceUV.w * texH
                };
                
                // Texture border input (where to slice the texture)
                ImGui::Text("Texture Borders (pixels)");
                bool changed = false;
                ImGui::PushItemWidth(60);
                ImGui::Text("L:"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##tbl", &texBorderPx.x, 0.5f, 0.0f, texW * 0.5f, "%.0f");
                ImGui::SameLine(); ImGui::Text("T:"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##tbt", &texBorderPx.y, 0.5f, 0.0f, texH * 0.5f, "%.0f");
                ImGui::SameLine(); ImGui::Text("R:"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##tbr", &texBorderPx.z, 0.5f, 0.0f, texW * 0.5f, "%.0f");
                ImGui::SameLine(); ImGui::Text("B:"); ImGui::SameLine();
                changed |= ImGui::DragFloat("##tbb", &texBorderPx.w, 0.5f, 0.0f, texH * 0.5f, "%.0f");
                ImGui::PopItemWidth();
                
                // Auto-calculate SliceUV from pixel borders
                if (changed) {
                    p.SliceUV.x = texBorderPx.x / texW;
                    p.SliceUV.y = texBorderPx.y / texH;
                    p.SliceUV.z = texBorderPx.z / texW;
                    p.SliceUV.w = texBorderPx.w / texH;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Where to slice the texture (in texture pixels).\nThese define the 9-slice border regions.");
                }
                
                // Render size option - detect current mode from SliceBorder values
                bool preserveSize = (p.SliceBorder.x > 0 || p.SliceBorder.y > 0 || 
                                     p.SliceBorder.z > 0 || p.SliceBorder.w > 0);
                ImGui::Separator();
                if (ImGui::Checkbox("Preserve Border Size", &preserveSize)) {
                    if (preserveSize) {
                        // Set SliceBorder to match texture borders
                        p.SliceBorder = texBorderPx;
                    } else {
                        // Clear to use percentage-based scaling
                        p.SliceBorder = glm::vec4(0);
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When checked, borders render at their original texture pixel size.\nWhen unchecked, borders scale proportionally with the panel.");
                }
                
                // Keep SliceBorder in sync when editing texture borders with preserve on
                if (preserveSize && changed) {
                    p.SliceBorder = texBorderPx;
                }
                
                if (preserveSize) {
                    ImGui::TextDisabled("Borders: %.0f, %.0f, %.0f, %.0f px (fixed)", 
                        p.SliceBorder.x, p.SliceBorder.y, p.SliceBorder.z, p.SliceBorder.w);
                } else {
                    float renderL = p.Size.x * p.SliceUV.x;
                    float renderT = p.Size.y * p.SliceUV.y;
                    float renderR = p.Size.x * p.SliceUV.z;
                    float renderB = p.Size.y * p.SliceUV.w;
                    ImGui::TextDisabled("Borders: %.0f, %.0f, %.0f, %.0f px (scaled)", 
                        renderL, renderT, renderR, renderB);
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Assign a texture to configure NineSlice");
                // Fallback to raw UV editing if no texture
                ImGui::DragFloat4("Slice UV (L T R B)", &p.SliceUV.x, 0.001f, 0.0f, 0.5f);
            }
        }
        ImGui::PopID();
    });

    // Button drawer
    registry.Register<ButtonComponent>("Button", [](ButtonComponent& b){
        ImGui::PushID(&b);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Interactable", &b.Interactable);
        ImGui::Checkbox("Toggle", &b.Toggle);
        ImGui::Checkbox("Toggled", &b.Toggled);
        ImGui::ColorEdit4("Normal Tint", &b.NormalTint.x);
        ImGui::ColorEdit4("Hover Tint", &b.HoverTint.x);
        ImGui::ColorEdit4("Pressed Tint", &b.PressedTint.x);
        ImGui::ColorEdit4("Disabled Tint", &b.DisabledTint.x);
        // Live state read-only
        ImGui::Separator();
        ImGui::TextDisabled("Runtime State (read-only)");
        ImGui::Text("Hovered: %s", b.Hovered ? "true" : "false");
        ImGui::Text("Pressed: %s", b.Pressed ? "true" : "false");
        ImGui::Text("Clicked: %s", b.Clicked ? "true" : "false");
        ImGui::PopID();
    });
    
    // Slider drawer
    registry.Register<SliderComponent>("Slider", [drawUITextureSlot](SliderComponent& s){
        ImGui::PushID(&s);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Interactable", &s.Interactable);
        ImGui::Separator();
        
        ImGui::Text("Value Range");
        ImGui::DragFloat("Min Value", &s.MinValue, 0.1f);
        ImGui::DragFloat("Max Value", &s.MaxValue, 0.1f);
        ImGui::DragFloat("Value", &s.Value, 0.01f, s.MinValue, s.MaxValue);
        ImGui::DragFloat("Step", &s.Step, 0.01f, 0.0f, FLT_MAX, "%.3f", ImGuiSliderFlags_None);
        ImGui::Checkbox("Whole Numbers", &s.WholeNumbers);
        
        const char* directions[] = { "Horizontal", "Vertical" };
        int dir = (int)s.SliderDirection;
        if (ImGui::Combo("Direction", &dir, directions, IM_ARRAYSIZE(directions))) 
            s.SliderDirection = (SliderComponent::Direction)dir;
        
        ImGui::Separator();
        ImGui::Text("Handle");
        ImGui::DragFloat2("Handle Size", &s.HandleSize.x, 1.0f);
        drawUITextureSlot("Handle Texture", s.HandleTexture);
        ImGui::ColorEdit4("Handle Normal Tint", &s.HandleNormalTint.x);
        ImGui::ColorEdit4("Handle Hover Tint", &s.HandleHoverTint.x);
        ImGui::ColorEdit4("Handle Pressed Tint", &s.HandlePressedTint.x);
        ImGui::ColorEdit4("Handle Disabled Tint", &s.HandleDisabledTint.x);
        
        ImGui::Separator();
        ImGui::Text("Fill Bar");
        ImGui::Checkbox("Show Fill", &s.ShowFill);
        if (s.ShowFill) {
            drawUITextureSlot("Fill Texture", s.FillTexture);
            ImGui::ColorEdit4("Fill Color", &s.FillColor.x);
        }
        
        ImGui::Separator();
        ImGui::TextDisabled("Runtime State (read-only)");
        ImGui::Text("Hovered: %s", s.Hovered ? "true" : "false");
        ImGui::Text("Dragging: %s", s.Dragging ? "true" : "false");
        ImGui::PopID();
    });
    
    // Progress Bar drawer
    registry.Register<ProgressBarComponent>("Progress Bar", [drawUITextureSlot](ProgressBarComponent& p){
        ImGui::PushID(&p);
        DrawUIWorldSpaceControls();
        ImGui::Text("Value");
        ImGui::DragFloat("Min Value", &p.MinValue, 0.1f);
        ImGui::DragFloat("Max Value", &p.MaxValue, 0.1f);
        ImGui::SliderFloat("Value", &p.Value, p.MinValue, p.MaxValue);
        
        const char* directions[] = { "Left to Right", "Right to Left", "Bottom to Top", "Top to Bottom" };
        int dir = (int)p.Direction;
        if (ImGui::Combo("Fill Direction", &dir, directions, IM_ARRAYSIZE(directions))) 
            p.Direction = (ProgressBarComponent::FillDirection)dir;
        
        ImGui::Separator();
        ImGui::Text("Fill Visuals");
        drawUITextureSlot("Fill Texture", p.FillTexture);
        ImGui::ColorEdit4("Fill Color", &p.FillColor.x);
        ImGui::Checkbox("Use Gradient", &p.UseGradient);
        if (p.UseGradient) {
            ImGui::ColorEdit4("Low Color", &p.GradientLowColor.x);
            ImGui::ColorEdit4("High Color", &p.GradientHighColor.x);
        }
        
        ImGui::Separator();
        ImGui::Text("Fill Padding");
        ImGui::Checkbox("Match Panel NineSlice", &p.UsePanelBorderAsPadding);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Automatically inset the fill bar to match the Panel's NineSlice borders.\nThe fill will render inside the panel's content area.\n\nRequires the Panel to use NineSlice fill mode.");
        }
        if (!p.UsePanelBorderAsPadding) {
            ImGui::DragFloat4("Padding (L T R B)", &p.Padding.x, 1.0f, 0.0f, 100.0f);
        } else {
            ImGui::BeginDisabled();
            ImGui::DragFloat4("Padding (L T R B)", &p.Padding.x, 1.0f, 0.0f, 100.0f);
            ImGui::EndDisabled();
            ImGui::TextDisabled("(Padding derived from Panel's NineSlice borders)");
        }
        
        ImGui::Separator();
        ImGui::Text("Animation");
        ImGui::Checkbox("Animate", &p.Animate);
        if (p.Animate) {
            ImGui::DragFloat("Animation Speed", &p.AnimationSpeed, 0.1f, 0.1f, 100.0f);
        }
        ImGui::PopID();
    });
    
    // Toggle drawer
    registry.Register<ToggleComponent>("Toggle", [drawUITextureSlot](ToggleComponent& t){
        ImGui::PushID(&t);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Interactable", &t.Interactable);
        ImGui::Checkbox("Is On", &t.IsOn);
        ImGui::DragInt("Group ID", &t.GroupID, 1.0f, 0, 100);
        
        ImGui::Separator();
        ImGui::Text("Checkmark");
        drawUITextureSlot("Checkmark Texture", t.CheckmarkTexture);
        ImGui::DragFloat2("Checkmark Size", &t.CheckmarkSize.x, 1.0f);
        ImGui::DragFloat2("Checkmark Offset", &t.CheckmarkOffset.x, 1.0f);
        ImGui::ColorEdit4("Checkmark Tint", &t.CheckmarkTint.x);
        
        ImGui::Separator();
        ImGui::Text("Background Tints");
        ImGui::ColorEdit4("Off Tint", &t.OffTint.x);
        ImGui::ColorEdit4("On Tint", &t.OnTint.x);
        ImGui::ColorEdit4("Hover Tint", &t.HoverTint.x);
        ImGui::ColorEdit4("Disabled Tint", &t.DisabledTint.x);
        
        ImGui::Separator();
        ImGui::TextDisabled("Runtime State (read-only)");
        ImGui::Text("Hovered: %s", t.Hovered ? "true" : "false");
        ImGui::Text("Pressed: %s", t.Pressed ? "true" : "false");
        ImGui::PopID();
    });
    
    // Scroll View drawer
    registry.Register<ScrollViewComponent>("Scroll View", [drawUITextureSlot](ScrollViewComponent& s){
        ImGui::PushID(&s);
        DrawUIWorldSpaceControls();
        ImGui::Text("Content");
        ImGui::DragFloat2("Content Size", &s.ContentSize.x, 1.0f);
        ImGui::DragFloat2("Content Offset", &s.ContentOffset.x, 1.0f);
        
        ImGui::Separator();
        ImGui::Text("Scroll Settings");
        ImGui::Checkbox("Horizontal Scroll", &s.HorizontalScroll);
        ImGui::Checkbox("Vertical Scroll", &s.VerticalScroll);
        ImGui::DragFloat("Scroll Sensitivity", &s.ScrollSensitivity, 1.0f, 1.0f, 200.0f);
        
        ImGui::Separator();
        ImGui::Text("Scrollbars");
        ImGui::Checkbox("Show Scrollbars", &s.ShowScrollbars);
        if (s.ShowScrollbars) {
            ImGui::DragFloat("Scrollbar Width", &s.ScrollbarWidth, 1.0f, 4.0f, 50.0f);
            drawUITextureSlot("Track Texture", s.ScrollbarTrackTexture);
            drawUITextureSlot("Thumb Texture", s.ScrollbarThumbTexture);
            ImGui::ColorEdit4("Track Color", &s.ScrollbarTrackColor.x);
            ImGui::ColorEdit4("Thumb Color", &s.ScrollbarThumbColor.x);
            ImGui::ColorEdit4("Thumb Hover Color", &s.ScrollbarThumbHoverColor.x);
        }
        
        ImGui::Separator();
        ImGui::Text("Physics");
        ImGui::Checkbox("Use Inertia", &s.UseInertia);
        if (s.UseInertia) {
            ImGui::DragFloat("Inertia Deceleration", &s.InertiaDeceleration, 10.0f, 0.0f, 2000.0f);
        }
        ImGui::Checkbox("Elastic", &s.Elastic);
        if (s.Elastic) {
            ImGui::DragFloat("Elastic Amount", &s.ElasticAmount, 1.0f, 0.0f, 200.0f);
        }
        ImGui::PopID();
    });
    
    // Layout Group drawer
    registry.Register<LayoutGroupComponent>("Layout Group", [](LayoutGroupComponent& l){
        ImGui::PushID(&l);
        DrawUIWorldSpaceControls();
        const char* directions[] = { "Horizontal", "Vertical" };
        int dir = (int)l.Direction;
        if (ImGui::Combo("Direction", &dir, directions, IM_ARRAYSIZE(directions))) 
            l.Direction = (LayoutGroupComponent::LayoutDirection)dir;
        
        ImGui::DragFloat4("Padding (L T R B)", &l.Padding.x, 1.0f, 0.0f, 100.0f);
        ImGui::DragFloat("Spacing", &l.Spacing, 1.0f, 0.0f, 100.0f);
        
        const char* alignments[] = { "Start", "Center", "End" };
        int childAlign = (int)l.ChildAlignment;
        if (ImGui::Combo("Child Alignment", &childAlign, alignments, IM_ARRAYSIZE(alignments))) 
            l.ChildAlignment = (LayoutGroupComponent::Alignment)childAlign;
        int crossAlign = (int)l.CrossAlignment;
        if (ImGui::Combo("Cross Alignment", &crossAlign, alignments, IM_ARRAYSIZE(alignments))) 
            l.CrossAlignment = (LayoutGroupComponent::Alignment)crossAlign;
        
        ImGui::Separator();
        ImGui::Text("Size Control");
        ImGui::Checkbox("Control Child Width", &l.ControlChildWidth);
        ImGui::Checkbox("Control Child Height", &l.ControlChildHeight);
        ImGui::Checkbox("Force Expand Width", &l.ChildForceExpandWidth);
        ImGui::Checkbox("Force Expand Height", &l.ChildForceExpandHeight);
        ImGui::Checkbox("Reverse Order", &l.ReverseOrder);
        
        ImGui::Separator();
        ImGui::Text("Grid Layout (set Columns > 0)");
        ImGui::DragInt("Columns", &l.Columns, 1.0f, 0, 20);
        ImGui::DragInt("Rows", &l.Rows, 1.0f, 0, 20);
        if (l.Columns > 0) {
            ImGui::DragFloat2("Cell Size", &l.CellSize.x, 1.0f);
        }
        ImGui::PopID();
    });
    
    // Input Field drawer
    registry.Register<InputFieldComponent>("Input Field", [](InputFieldComponent& i){
        ImGui::PushID(&i);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Interactable", &i.Interactable);
        ImGui::Checkbox("Read Only", &i.ReadOnly);
        ImGui::Checkbox("Multiline", &i.Multiline);
        
        char textBuf[1024];
        strncpy(textBuf, i.Text.c_str(), sizeof(textBuf) - 1);
        textBuf[sizeof(textBuf) - 1] = '\0';
        if (ImGui::InputText("Text", textBuf, sizeof(textBuf))) {
            i.Text = textBuf;
        }
        
        char placeholderBuf[256];
        strncpy(placeholderBuf, i.PlaceholderText.c_str(), sizeof(placeholderBuf) - 1);
        placeholderBuf[sizeof(placeholderBuf) - 1] = '\0';
        if (ImGui::InputText("Placeholder", placeholderBuf, sizeof(placeholderBuf))) {
            i.PlaceholderText = placeholderBuf;
        }
        
        ImGui::DragInt("Max Length", &i.MaxLength, 1.0f, 0, 10000);
        
        const char* contentTypes[] = { "Standard", "Integer", "Decimal", "Alphanumeric", "Password" };
        int ct = (int)i.Type;
        if (ImGui::Combo("Content Type", &ct, contentTypes, IM_ARRAYSIZE(contentTypes))) 
            i.Type = (InputFieldComponent::ContentType)ct;
        
        ImGui::Separator();
        ImGui::Text("Colors");
        ImGui::ColorEdit4("Text Color", &i.TextColor.x);
        ImGui::ColorEdit4("Placeholder Color", &i.PlaceholderColor.x);
        ImGui::ColorEdit4("Selection Color", &i.SelectionColor.x);
        ImGui::ColorEdit4("Cursor Color", &i.CursorColor.x);
        ImGui::DragFloat("Cursor Width", &i.CursorWidth, 0.5f, 1.0f, 10.0f);
        ImGui::PopID();
    });
    
    // Dropdown drawer
    registry.Register<DropdownComponent>("Dropdown", [drawUITextureSlot](DropdownComponent& d){
        ImGui::PushID(&d);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Interactable", &d.Interactable);
        
        char captionBuf[256];
        strncpy(captionBuf, d.Caption.c_str(), sizeof(captionBuf) - 1);
        captionBuf[sizeof(captionBuf) - 1] = '\0';
        if (ImGui::InputText("Caption", captionBuf, sizeof(captionBuf))) {
            d.Caption = captionBuf;
        }
        
        ImGui::Separator();
        ImGui::Text("Options (one per line):");
        // Simple options editor
        static char optionsBuf[2048] = "";
        // Build current options into buffer
        std::string allOptions;
        for (size_t i = 0; i < d.Options.size(); ++i) {
            allOptions += d.Options[i];
            if (i + 1 < d.Options.size()) allOptions += "\n";
        }
        strncpy(optionsBuf, allOptions.c_str(), sizeof(optionsBuf) - 1);
        optionsBuf[sizeof(optionsBuf) - 1] = '\0';
        if (ImGui::InputTextMultiline("##Options", optionsBuf, sizeof(optionsBuf), ImVec2(-1, 100))) {
            // Parse back into vector
            d.Options.clear();
            std::stringstream ss(optionsBuf);
            std::string line;
            while (std::getline(ss, line)) {
                if (!line.empty()) d.Options.push_back(line);
            }
        }
        
        if (!d.Options.empty()) {
            int sel = d.SelectedIndex;
            if (sel < 0 || sel >= (int)d.Options.size()) sel = 0;
            if (ImGui::Combo("Selected", &sel, [](void* data, int idx, const char** out) {
                auto* opts = (std::vector<std::string>*)data;
                if (idx >= 0 && idx < (int)opts->size()) { *out = (*opts)[idx].c_str(); return true; }
                return false;
            }, &d.Options, (int)d.Options.size())) {
                d.SelectedIndex = sel;
            }
        }
        
        ImGui::Separator();
        ImGui::Text("Dropdown List");
        ImGui::DragFloat("Option Height", &d.OptionHeight, 1.0f, 20.0f, 100.0f);
        ImGui::DragInt("Max Visible Options", &d.MaxVisibleOptions, 1.0f, 1, 20);
        ImGui::ColorEdit4("Option Normal Color", &d.OptionNormalColor.x);
        ImGui::ColorEdit4("Option Hover Color", &d.OptionHoverColor.x);
        ImGui::ColorEdit4("Option Selected Color", &d.OptionSelectedColor.x);
        
        ImGui::Separator();
        ImGui::Text("Arrow");
        ImGui::Checkbox("Show Arrow", &d.ShowArrow);
        if (d.ShowArrow) {
            drawUITextureSlot("Arrow Texture", d.ArrowTexture);
            ImGui::DragFloat2("Arrow Size", &d.ArrowSize.x, 1.0f);
            ImGui::ColorEdit4("Arrow Tint", &d.ArrowTint.x);
        }
        ImGui::PopID();
    });
    
    // UI Rect Component drawer - parent-relative positioning
    registry.Register<UIRectComponent>("UI Rect", [](UIRectComponent& r){
        ImGui::PushID(&r);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Anchor To Parent", &r.AnchorToParent);
        if (r.AnchorToParent) {
            ImGui::TextDisabled("Position is relative to parent UI element");
            ImGui::SliderFloat("Horizontal Anchor", &r.HorizontalAnchor, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Vertical Anchor", &r.VerticalAnchor, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat2("Pivot", &r.Pivot.x, 0.0f, 1.0f, "%.2f");
            ImGui::DragFloat2("Offset", &r.Offset.x, 1.0f);
            ImGui::DragFloat2("Size (for Text)", &r.Size.x, 1.0f, 0.0f, 10000.0f);
            ImGui::TextDisabled("(0,0)=top-left anchor, (0.5,0.5)=center, (1,1)=bottom-right");
        }
        ImGui::Separator();
        ImGui::TextDisabled("Computed Rect (read-only)");
        ImGui::Text("X: %.1f  Y: %.1f", r._ComputedRect.x, r._ComputedRect.y);
        ImGui::Text("W: %.1f  H: %.1f", r._ComputedRect.z, r._ComputedRect.w);
        ImGui::PopID();
    });
    
    // Fit To Content Component drawer - auto-size panel to children
    registry.Register<FitToContentComponent>("Fit To Content", [](FitToContentComponent& f){
        ImGui::PushID(&f);
        DrawUIWorldSpaceControls();
        ImGui::Checkbox("Enabled", &f.Enabled);
        if (f.Enabled) {
            ImGui::TextDisabled("Panel will auto-size to fit children");
            ImGui::Checkbox("Fit Width", &f.FitWidth);
            ImGui::Checkbox("Fit Height", &f.FitHeight);
            ImGui::DragFloat4("Padding (L T R B)", &f.Padding.x, 1.0f, 0.0f, 200.0f);
            ImGui::DragFloat2("Min Size", &f.MinSize.x, 1.0f, 0.0f, 10000.0f);
            ImGui::DragFloat2("Max Size", &f.MaxSize.x, 1.0f, 0.0f, 10000.0f);
            ImGui::TextDisabled("(0 = no constraint)");
            ImGui::Checkbox("Direct Children Only", &f.DirectChildrenOnly);
            ImGui::TextDisabled("If unchecked, includes all descendants");
        }
        ImGui::PopID();
    });

    // UI Scene Capture Component drawer - render scene to panel
    registry.Register<UISceneCaptureComponent>("UI Scene Capture", [](UISceneCaptureComponent& c){
        ImGui::PushID(&c);
        DrawUIWorldSpaceControls();
        Scene* scene = GetCurrentComponentDrawScene();
        ImGui::Checkbox("Enabled", &c.Enabled);
        ImGui::Checkbox("Auto Frame", &c.AutoFrame);
        ImGui::Checkbox("Include Children", &c.IncludeChildren);
        ImGui::DragFloat("Bounds Padding", &c.BoundsPadding, 0.05f, 0.1f, 4.0f);
        ImGui::DragFloat("Field Of View", &c.FieldOfView, 0.1f, 10.0f, 179.0f);
        ImGui::DragFloat("Near Clip", &c.NearClip, 0.01f, 0.001f, 1000.0f);
        ImGui::DragFloat("Far Clip", &c.FarClip, 1.0f, 1.0f, 100000.0f);
        ImGui::DragFloat3("View Direction", &c.ViewDirection.x, 0.01f);
        ImGui::DragFloat3("Up Direction", &c.UpDirection.x, 0.01f);
        ImGui::DragFloat3("Focus Offset", &c.FocusOffset.x, 0.1f);
        ImGui::Checkbox("Lock View To Target", &c.LockViewToTarget);
        if (c.LockViewToTarget) {
            ImGui::TextDisabled("View/Up Direction are treated as local target-space axes");
        }

        int targetId = c.TargetEntity;
        if (ImGui::InputInt("Target Entity", &targetId)) {
            c.TargetEntity = targetId;
            if (scene && scene->GetEntityData(targetId)) {
                auto* td = scene->GetEntityData(targetId);
                c.TargetGuidHigh = td->EntityGuid.high;
                c.TargetGuidLow = td->EntityGuid.low;
            } else {
                c.TargetGuidHigh = 0;
                c.TargetGuidLow = 0;
            }
        }
        if (scene && scene->GetEntityData(c.TargetEntity)) {
            auto* td = scene->GetEntityData(c.TargetEntity);
            ImGui::TextDisabled("Target: %s", td->Name.c_str());
        } else {
            ImGui::TextDisabled("Target: <none>");
        }

        ImGui::DragInt("Render Width", &c.RenderWidth, 1.0f, 0, 8192);
        ImGui::DragInt("Render Height", &c.RenderHeight, 1.0f, 0, 8192);
        ImGui::TextDisabled("Set to 0 to use panel size");

        ImVec4 clr(
            ((c.ClearColor >> 24) & 0xFF) / 255.0f,
            ((c.ClearColor >> 16) & 0xFF) / 255.0f,
            ((c.ClearColor >> 8) & 0xFF) / 255.0f,
            (c.ClearColor & 0xFF) / 255.0f
        );
        if (ImGui::ColorEdit4("Clear Color", &clr.x)) {
            auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
            uint8_t r = (uint8_t)(clamp01(clr.x) * 255.0f);
            uint8_t g = (uint8_t)(clamp01(clr.y) * 255.0f);
            uint8_t b = (uint8_t)(clamp01(clr.z) * 255.0f);
            uint8_t a = (uint8_t)(clamp01(clr.w) * 255.0f);
            c.ClearColor = (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | uint32_t(a);
        }
        ImGui::Checkbox("Show Grid", &c.ShowGrid);
        ImGui::PopID();
    });

    // Audio Source Component drawer
    registry.Register<AudioSourceComponent>("Audio Source", [](AudioSourceComponent& a) {
        auto syncAudioReferenceFromPath = [&](const std::string& path) {
            ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(path);
            if (guid.high != 0 || guid.low != 0) {
                a.AudioClip = AssetReference(guid, 0, static_cast<int>(AssetType::Audio));
            } else {
                a.AudioClip = AssetReference();
            }
        };

        // Audio Clip dropdown (select from project audio files)
        {
            const auto& options = ui::GetAudioAssetOptions();
            int currentIndex = -1;
            for (int i = 0; i < static_cast<int>(options.size()); ++i) {
                if (options[i].path == a.AudioPath ||
                    (a.AudioClip.IsValid() && options[i].guid == a.AudioClip.guid)) {
                    currentIndex = i;
                    break;
                }
            }
            
            const char* currentLabel = currentIndex >= 0 ? options[currentIndex].name.c_str() : "<Select Clip>";
            if (ImGui::BeginCombo("Audio Clip", currentLabel)) {
                // None option
                if (ImGui::Selectable("<None>", currentIndex < 0)) {
                    a.AudioPath.clear();
                    a.AudioClip = AssetReference();
                }
                ImGui::Separator();
                
                for (int i = 0; i < static_cast<int>(options.size()); ++i) {
                    bool isSel = (i == currentIndex);
                    if (ImGui::Selectable(options[i].name.c_str(), isSel)) {
                        a.AudioPath = options[i].path;
                        syncAudioReferenceFromPath(options[i].path);
                    }
                    if (isSel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
        }
        
        // Direct path input (fallback/manual)
        char pathBuf[512];
        strncpy(pathBuf, a.AudioPath.c_str(), sizeof(pathBuf) - 1);
        pathBuf[sizeof(pathBuf) - 1] = '\0';
        if (ImGui::InputText("Audio Path", pathBuf, sizeof(pathBuf))) {
            a.AudioPath = pathBuf;
            syncAudioReferenceFromPath(a.AudioPath);
        }
        
        // Accept drag-drop of audio files
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_FILE")) {
                const char* path = (const char*)payload->Data;
                if (path) {
                    std::string ext = std::filesystem::path(path).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") {
                        a.AudioPath = path;
                        syncAudioReferenceFromPath(a.AudioPath);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        
        ImGui::Separator();
        ImGui::Text("Playback Settings");
        
        ImGui::SliderFloat("Volume", &a.Volume, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Pitch", &a.Pitch, 0.1f, 3.0f, "%.2f");
        ImGui::Checkbox("Loop", &a.Loop);
        ImGui::Checkbox("Play On Awake", &a.PlayOnAwake);
        ImGui::Checkbox("Mute", &a.Mute);
        
        ImGui::Separator();
        ImGui::Text("3D Spatial Settings");
        
        ImGui::Checkbox("Spatial (3D)", &a.Spatial);
        if (a.Spatial) {
            ImGui::DragFloat("Min Distance", &a.MinDistance, 0.1f, 0.0f, 100.0f);
            ImGui::DragFloat("Max Distance", &a.MaxDistance, 1.0f, 0.0f, 500.0f);
            ImGui::SliderFloat("Doppler Factor", &a.DopplerFactor, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Rolloff", &a.Rolloff, 0.0f, 5.0f, "%.2f");
        }
        
        ImGui::Separator();
        ImGui::Text("Runtime State");
        ImGui::TextDisabled("Playing: %s", a.IsPlaying ? "Yes" : "No");
        ImGui::TextDisabled("Paused: %s", a.IsPaused ? "Yes" : "No");
        ImGui::TextDisabled("Handle: %u", a.SoundHandle);
    });

    // Audio Listener Component drawer
    registry.Register<AudioListenerComponent>("Audio Listener", [](AudioListenerComponent& l) {
        ImGui::Checkbox("Active", &l.Active);
        ImGui::DragInt("Priority", &l.Priority, 1.0f, 0, 100);
        ImGui::SliderFloat("Volume Multiplier", &l.VolumeMultiplier, 0.0f, 2.0f, "%.2f");
        
        ImGui::Separator();
        ImGui::Text("Runtime State");
        ImGui::TextDisabled("Position: (%.2f, %.2f, %.2f)", l.LastPosition.x, l.LastPosition.y, l.LastPosition.z);
        ImGui::TextDisabled("Velocity: (%.2f, %.2f, %.2f)", l.Velocity.x, l.Velocity.y, l.Velocity.z);
    });
}
