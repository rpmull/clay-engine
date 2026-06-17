#include "ComponentInterop.h"
#include "core/ecs/Scene.h"
#include "core/ecs/Components.h"
#include "core/ecs/ComponentUtils.h"
#include "core/ecs/NpcScalability.h"
#include "core/ecs/SoftbodySystem.h"
#include "core/ecs/UIComponents.h"
#include "core/ecs/AnimationComponents.h"
#include "core/physics/Physics.h"
#include "core/physics/PhysicsLayerManager.h"
#include "core/physics/area/AreaSystem.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimatorRuntime.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/animation/AnimatorControllerOverrideIO.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/animation/AvatarMask.h"
#include "core/managed/ScriptSystem.h"
#include "core/assets/IAssetResolver.h"
#include "core/rendering/PBRMaterial.h"
#include "core/rendering/RuntimeShaderGraphMaterial.h"
#include "core/rendering/MaterialPropertyBlock.h"
#include "core/rendering/MaterialCache.h"
#include "core/serialization/MaterialCache.h"
#include "core/rendering/Terrain.h"
#include "core/spline/SplineUtils.h"
#include "core/navigation/NavAgent.h"
#include "core/navigation/NavMesh.h"
#include <algorithm>
#ifdef CLAYMORE_EDITOR
#include "core/rendering/MaterialAssetCache.h"
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "editor/ui/Logger.h"
#include "editor/pipeline/BinaryAssetCache.h"
#endif
#include <glm/glm.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <Jolt/Physics/Body/BodyLock.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <cmath>
#include <array>
#include <filesystem>
#include <sstream>

// ============================================================================
// Re-entrant safe string buffer for interop returns
// Uses rotating thread-local buffers to handle nested calls safely
// ============================================================================
namespace {
    static constexpr int kNumStringBuffers = 8;
    
    std::string& GetRotatingStringBuffer() {
        thread_local std::string s_Buffers[kNumStringBuffers];
        thread_local int s_CurrentBuffer = 0;
        s_CurrentBuffer = (s_CurrentBuffer + 1) % kNumStringBuffers;
        return s_Buffers[s_CurrentBuffer];
    }

    bool ScriptClassMatches(const std::string& storedClassName, const char* requestedClassName)
    {
        if (!requestedClassName || !*requestedClassName) {
            return false;
        }

        if (storedClassName == requestedClassName) {
            return true;
        }

        const std::string requested(requestedClassName);
        const size_t storedSeparator = storedClassName.find_last_of(".+");
        const size_t requestedSeparator = requested.find_last_of(".+");
        if (storedSeparator != std::string::npos && requestedSeparator != std::string::npos) {
            return false;
        }

        const std::string storedSimple =
            storedClassName.substr(storedSeparator == std::string::npos ? 0 : storedSeparator + 1);
        const std::string requestedSimple =
            requested.substr(requestedSeparator == std::string::npos ? 0 : requestedSeparator + 1);
        return storedSimple == requestedSimple;
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

    void SeedCanvasComponentDefaults(EntityData* data)
    {
        if (!data || !data->Canvas)
            return;

        glm::ivec2 suggested = ComputeSuggestedWorldSpaceUICanvasSize(data);
        if (data->Canvas->Width <= 0)
            data->Canvas->Width = suggested.x;
        if (data->Canvas->Height <= 0)
            data->Canvas->Height = suggested.y;
    }

    void NormalizeWorldSpaceUIRootLayout(EntityData* data)
    {
        if (!data)
            return;

        if (data->Panel) {
            if (data->UIRect && data->UIRect->AnchorToParent) {
                data->UIRect->Offset = glm::vec2(0.0f);
            } else {
                data->Panel->AnchorEnabled = false;
                data->Panel->AnchorToParentUI = false;
                data->Panel->AnchorOffset = glm::vec2(0.0f);
                data->Panel->Position = glm::vec2(0.0f);
            }
        }

        if (data->Text && !data->Text->WorldSpace) {
            if (data->UIRect && data->UIRect->AnchorToParent) {
                data->UIRect->Offset = glm::vec2(0.0f);
            } else {
                data->Text->AnchorEnabled = false;
                data->Text->AnchorToParentUI = false;
                data->Text->AnchorOffset = glm::vec2(0.0f);
                data->Transform.Position.x = 0.0f;
                data->Transform.Position.y = 0.0f;
            }
        }

        if (!data->Panel && !data->Text && data->UIRect) {
            if (data->UIRect->AnchorToParent) {
                data->UIRect->Offset = glm::vec2(0.0f);
            } else {
                data->Transform.Position.x = 0.0f;
                data->Transform.Position.y = 0.0f;
            }
        }
    }

    bool EntityUsesCanvasBackedUI(const EntityData* data)
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

    bool HasCanvasAncestor(Scene& scene, EntityID entityId)
    {
        auto* data = scene.GetEntityData(entityId);
        EntityID current = data ? data->Parent : INVALID_ENTITY_ID;
        while (current != INVALID_ENTITY_ID) {
            auto* currentData = scene.GetEntityData(current);
            if (!currentData)
                break;
            if (currentData->Canvas)
                return true;
            current = currentData->Parent;
        }
        return false;
    }

    enum class ManagedMaterialKind : int {
        None = 0,
        PBR = 1,
        PSX = 2,
        ShaderGraph = 3,
        Custom = 4,
    };

    enum class ManagedPbrScalar : int {
        Metallic = 0,
        Roughness = 1,
        NormalScale = 2,
        AmbientOcclusion = 3,
        EmissionStrength = 4,
        DisplacementScale = 5,
    };

    size_t GetEffectiveMaterialSlotCount(const MeshComponent& mesh)
    {
        size_t slotCount = mesh.materials.size();
        slotCount = std::max(slotCount, mesh.MaterialSlotNames.size());
        slotCount = std::max(slotCount, mesh.MaterialAssetPaths.size());
        slotCount = std::max(slotCount, mesh.SlotPropertyBlocks.size());
        slotCount = std::max(slotCount, mesh.OwnedMaterialSlots.size());
        if (mesh.mesh) {
            slotCount = std::max(slotCount, mesh.mesh->Submeshes.size());
        }
        if (slotCount == 0 && mesh.material) {
            slotCount = 1;
        }
        return slotCount;
    }

    bool MeshNeedsSkinnedMaterial(const EntityData& data, const MeshComponent& mesh)
    {
        return data.Skinning != nullptr || (mesh.mesh && mesh.mesh->HasSkinning());
    }

    void EnsureMaterialSlotStorage(MeshComponent& mesh, size_t slot)
    {
        const size_t required = slot + 1;
        if (mesh.materials.size() < required) {
            mesh.materials.resize(required);
        }
        if (mesh.MaterialAssetPaths.size() < required) {
            mesh.MaterialAssetPaths.resize(required);
        }
        if (mesh.SlotPropertyBlocks.size() < required) {
            mesh.SlotPropertyBlocks.resize(required);
        }
        if (mesh.SlotPropertyBlockTexturePaths.size() < required) {
            mesh.SlotPropertyBlockTexturePaths.resize(required);
        }
        if (mesh.MaterialSources.size() < required) {
            mesh.MaterialSources.resize(required);
        }
        if (mesh.OwnedMaterialSlots.size() < required) {
            mesh.OwnedMaterialSlots.resize(required, false);
        }
    }

    void SyncPrimaryMaterialReference(MeshComponent& mesh)
    {
        if (!mesh.materials.empty()) {
            mesh.material = mesh.materials[0];
        } else {
            mesh.material.reset();
        }
    }

    void ClearSerializedMaterialSlotCache(MeshComponent& mesh, size_t slot)
    {
        mesh.InlineMaterials.erase(
            std::remove_if(
                mesh.InlineMaterials.begin(),
                mesh.InlineMaterials.end(),
                [slot](const binary::InlineMaterialData& data) {
                    return data.slotIndex == slot;
                }),
            mesh.InlineMaterials.end());
        mesh.ShaderGraphMaterials.erase(
            std::remove_if(
                mesh.ShaderGraphMaterials.begin(),
                mesh.ShaderGraphMaterials.end(),
                [slot](const binary::ShaderGraphMaterialData& data) {
                    return data.slotIndex == slot;
                }),
            mesh.ShaderGraphMaterials.end());
    }

    void SyncUniqueMaterialFlag(MeshComponent& mesh)
    {
        mesh.UniqueMaterial = std::any_of(
            mesh.OwnedMaterialSlots.begin(),
            mesh.OwnedMaterialSlots.end(),
            [](bool owned) { return owned; });
    }

    std::shared_ptr<Material> CreateDefaultMaterialForEntity(EntityData& data)
    {
        MaterialSource source;
        source.Skinned = MeshNeedsSkinnedMaterial(data, *data.Mesh);
        return AcquireMaterialFromSource(source, Scene::Get());
    }

    std::string NormalizeManagedAssetPath(const char* assetPath)
    {
        if (!assetPath) {
            return {};
        }

        std::string normalized(assetPath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return normalized;
    }

    std::shared_ptr<Material> LoadManagedMaterialAsset(const std::string& assetPath, bool needsSkinned)
    {
        if (assetPath.empty()) {
            return nullptr;
        }

        std::string resolvedPath = assetPath;
        if (IAssetResolver* resolver = Assets::GetResolver()) {
            std::string candidate = resolver->ResolvePath(assetPath);
            if (!candidate.empty()) {
                resolvedPath = candidate;
            }
        }

#if defined(CLAYMORE_EDITOR)
        return MaterialAssetCache::Acquire(resolvedPath);
#else
        std::filesystem::path runtimePath(resolvedPath);
        if (runtimePath.extension() == ".mat") {
            runtimePath.replace_extension(".matbin");
        }
        return RuntimeMaterialCache::GetOrLoad(runtimePath.string(), needsSkinned);
#endif
    }

    std::shared_ptr<Material> TryGetManagedMaterialSlot(EntityData& data, size_t slot)
    {
        if (!data.Mesh) {
            return nullptr;
        }

        MeshComponent& mesh = *data.Mesh;
        if (slot >= GetEffectiveMaterialSlotCount(mesh)) {
            return nullptr;
        }

        if (slot < mesh.materials.size() && mesh.materials[slot]) {
            return mesh.materials[slot];
        }

        if (slot == 0 && mesh.material) {
            return mesh.material;
        }

        if (slot < mesh.MaterialAssetPaths.size() &&
            !mesh.MaterialAssetPaths[slot].empty()) {
            auto material = LoadManagedMaterialAsset(
                mesh.MaterialAssetPaths[slot],
                MeshNeedsSkinnedMaterial(data, mesh));
            if (material) {
                if (mesh.materials.size() <= slot) {
                    mesh.materials.resize(slot + 1);
                }
                mesh.materials[slot] = material;
                SyncPrimaryMaterialReference(mesh);
            }
            return material;
        }

        return nullptr;
    }

    std::shared_ptr<Material> ResolveManagedMaterialSlot(EntityData& data, size_t slot)
    {
        if (!data.Mesh) {
            return nullptr;
        }

        MeshComponent& mesh = *data.Mesh;
        EnsureMaterialSlotStorage(mesh, slot);

        if (auto existing = TryGetManagedMaterialSlot(data, slot)) {
            if (!mesh.materials[slot]) {
                mesh.materials[slot] = existing;
            }
            SyncPrimaryMaterialReference(mesh);
            return existing;
        }

        if (!mesh.materials[slot]) {
            mesh.materials[slot] = CreateDefaultMaterialForEntity(data);
            mesh.MaterialAssetPaths[slot].clear();
            mesh.OwnedMaterialSlots[slot] = true;
            SyncUniqueMaterialFlag(mesh);
        }

        SyncPrimaryMaterialReference(mesh);
        return mesh.materials[slot];
    }

    std::shared_ptr<Material> EnsureEditableManagedMaterialSlot(EntityData& data, size_t slot)
    {
        auto material = ResolveManagedMaterialSlot(data, slot);
        if (!material || !data.Mesh) {
            return nullptr;
        }

        MeshComponent& mesh = *data.Mesh;
        EnsureMaterialSlotStorage(mesh, slot);
        if (!mesh.OwnedMaterialSlots[slot]) {
            auto clone = material->Clone();
            if (!clone) {
                return nullptr;
            }
            mesh.materials[slot] = clone;
            mesh.OwnedMaterialSlots[slot] = true;
            SyncUniqueMaterialFlag(mesh);
            material = clone;
        }

        SyncPrimaryMaterialReference(mesh);
        return material;
    }

    void CanonicalizeManagedMaterialSlot(EntityData& data, size_t slot)
    {
        if (!data.Mesh) {
            return;
        }

        MeshComponent& mesh = *data.Mesh;
        EnsureMaterialSlotStorage(mesh, slot);

        std::shared_ptr<Material> material = TryGetManagedMaterialSlot(data, slot);
        if (!material) {
            SyncPrimaryMaterialReference(mesh);
            SyncUniqueMaterialFlag(mesh);
            return;
        }

        ClearSerializedMaterialSlotCache(mesh, slot);
        if (slot < mesh.MaterialSources.size()) {
            mesh.MaterialSources[slot] = CaptureMaterialSource(material);
        }

        material = AcquireEquivalentMaterial(material);
        mesh.materials[slot] = material;
        mesh.OwnedMaterialSlots[slot] =
            !GetMaterialEquivalenceKey(material.get()).EquivalentSafe;

        SyncUniqueMaterialFlag(mesh);
        SyncPrimaryMaterialReference(mesh);
    }

    ManagedMaterialKind GetManagedMaterialKind(const std::shared_ptr<Material>& material);

    bool IsCustomManagedMaterial(const std::shared_ptr<Material>& material)
    {
        return GetManagedMaterialKind(material) == ManagedMaterialKind::Custom;
    }

    bool ShouldUseManagedSlotPropertyBlockOverride(const std::shared_ptr<Material>& material)
    {
        return !material || IsCustomManagedMaterial(material);
    }

    bool TryGetManagedSlotVectorOverride(const MeshComponent& mesh,
                                         size_t slot,
                                         const std::string& propertyName,
                                         glm::vec4& outValue)
    {
        if (slot >= mesh.SlotPropertyBlocks.size()) {
            return false;
        }

        return mesh.SlotPropertyBlocks[slot].TryGetVector(PropertyID::Get(propertyName), outValue);
    }

    bool TryGetManagedSlotTextureOverridePath(const MeshComponent& mesh,
                                              size_t slot,
                                              const std::string& propertyName,
                                              std::string& outPath)
    {
        if (slot >= mesh.SlotPropertyBlockTexturePaths.size()) {
            return false;
        }

        const auto& texturePaths = mesh.SlotPropertyBlockTexturePaths[slot];
        auto it = texturePaths.find(propertyName);
        if (it == texturePaths.end()) {
            return false;
        }

        outPath = it->second;
        return true;
    }

    void SetManagedSlotVectorOverride(MeshComponent& mesh,
                                      size_t slot,
                                      const std::string& propertyName,
                                      const glm::vec4& value)
    {
        EnsureMaterialSlotStorage(mesh, slot);
        mesh.SlotPropertyBlocks[slot].SetVector(propertyName, value);
    }

    void SetManagedSlotTextureOverride(MeshComponent& mesh,
                                       size_t slot,
                                       const std::string& propertyName,
                                       const std::string& assetPath)
    {
        EnsureMaterialSlotStorage(mesh, slot);

        if (!assetPath.empty()) {
            TextureSpecifier spec;
            spec.Path = assetPath;
            bgfx::TextureHandle tex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
            if (bgfx::isValid(tex)) {
                mesh.SlotPropertyBlocks[slot].SetTexture(propertyName, tex);
                mesh.SlotPropertyBlockTexturePaths[slot][propertyName] = assetPath;
                return;
            }
        }

        mesh.SlotPropertyBlocks[slot].RemoveTexture(propertyName);
        mesh.SlotPropertyBlockTexturePaths[slot].erase(propertyName);
    }

    ManagedMaterialKind GetManagedMaterialKind(const std::shared_ptr<Material>& material)
    {
        if (!material) {
            return ManagedMaterialKind::None;
        }

#if defined(CLAYMORE_EDITOR)
        if (std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(material)) {
            return ManagedMaterialKind::ShaderGraph;
        }
#endif
        if (std::dynamic_pointer_cast<cm::RuntimeShaderGraphMaterial>(material)) {
            return ManagedMaterialKind::ShaderGraph;
        }

        if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material)) {
            glm::vec4 value(0.0f);
            if (pbr->TryGetUniform("u_psxParams", value) ||
                pbr->TryGetUniform("u_psxWorld", value) ||
                pbr->TryGetUniform("u_toonParams", value) ||
                pbr->TryGetUniform("u_psxEmission", value)) {
                return ManagedMaterialKind::PSX;
            }
            return ManagedMaterialKind::PBR;
        }

        return ManagedMaterialKind::Custom;
    }

    bool TryGetShaderGraphVector4(const std::shared_ptr<Material>& material,
                                  const std::string& propertyName,
                                  glm::vec4& outValue)
    {
#if defined(CLAYMORE_EDITOR)
        if (auto shaderGraph = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(material)) {
            for (const auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type != shadergraph::ShaderValueType::Texture2D) {
                    outValue = parameter.value;
                    return true;
                }
            }
        }
#endif
        if (auto shaderGraph = std::dynamic_pointer_cast<cm::RuntimeShaderGraphMaterial>(material)) {
            for (const auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type != cm::RuntimeShaderValueType::Texture2D) {
                    outValue = parameter.value;
                    return true;
                }
            }
        }
        return false;
    }

    bool TrySetShaderGraphVector4(const std::shared_ptr<Material>& material,
                                  const std::string& propertyName,
                                  const glm::vec4& value)
    {
#if defined(CLAYMORE_EDITOR)
        if (auto shaderGraph = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(material)) {
            for (auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type != shadergraph::ShaderValueType::Texture2D) {
                    parameter.value = value;
                    return true;
                }
            }
        }
#endif
        if (auto shaderGraph = std::dynamic_pointer_cast<cm::RuntimeShaderGraphMaterial>(material)) {
            for (auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type != cm::RuntimeShaderValueType::Texture2D) {
                    parameter.value = value;
                    return true;
                }
            }
        }
        return false;
    }

    bool TryGetShaderGraphTexturePath(const std::shared_ptr<Material>& material,
                                      const std::string& propertyName,
                                      std::string& outPath)
    {
#if defined(CLAYMORE_EDITOR)
        if (auto shaderGraph = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(material)) {
            for (const auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type == shadergraph::ShaderValueType::Texture2D) {
                    outPath = parameter.texturePath;
                    return true;
                }
            }
        }
#endif
        if (auto shaderGraph = std::dynamic_pointer_cast<cm::RuntimeShaderGraphMaterial>(material)) {
            for (const auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type == cm::RuntimeShaderValueType::Texture2D) {
                    outPath = parameter.texturePath;
                    return true;
                }
            }
        }
        return false;
    }

    bool TrySetShaderGraphTexturePath(const std::shared_ptr<Material>& material,
                                      const std::string& propertyName,
                                      const std::string& assetPath)
    {
#if defined(CLAYMORE_EDITOR)
        if (auto shaderGraph = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(material)) {
            for (auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type == shadergraph::ShaderValueType::Texture2D) {
                    shaderGraph->SetTextureFromPath(propertyName, assetPath);
                    return true;
                }
            }
        }
#endif
        if (auto shaderGraph = std::dynamic_pointer_cast<cm::RuntimeShaderGraphMaterial>(material)) {
            for (auto& parameter : shaderGraph->GetParameters()) {
                if (parameter.name == propertyName &&
                    parameter.type == cm::RuntimeShaderValueType::Texture2D) {
                    shaderGraph->SetTextureFromPath(propertyName, assetPath);
                    return true;
                }
            }
        }
        return false;
    }

    bool TryGetMaterialTexturePath(const std::shared_ptr<Material>& material,
                                   const std::string& propertyName,
                                   std::string& outPath)
    {
        if (!material) {
            return false;
        }

        if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material)) {
            if (propertyName == "s_albedo") {
                outPath = pbr->GetAlbedoPath();
                return true;
            }
            if (propertyName == "s_metallicRoughness") {
                outPath = pbr->GetMetallicRoughnessPath();
                return true;
            }
            if (propertyName == "s_normalMap") {
                outPath = pbr->GetNormalPath();
                return true;
            }
            if (propertyName == "s_ao") {
                outPath = pbr->GetAOPath();
                return true;
            }
            if (propertyName == "s_emission") {
                outPath = pbr->GetEmissionPath();
                return true;
            }
            if (propertyName == "s_displacement") {
                outPath = pbr->GetDisplacementPath();
                return true;
            }
            if (propertyName == "s_tintMask") {
                outPath = pbr->GetTintMaskPath();
                return true;
            }
        }

        return TryGetShaderGraphTexturePath(material, propertyName, outPath);
    }

    bool TrySetMaterialTexturePath(const std::shared_ptr<Material>& material,
                                   const std::string& propertyName,
                                   const std::string& assetPath)
    {
        if (!material) {
            return false;
        }

        if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material)) {
            if (propertyName == "s_albedo") {
                pbr->SetAlbedoTextureFromPath(assetPath);
                return true;
            }
            if (propertyName == "s_metallicRoughness") {
                pbr->SetMetallicRoughnessTextureFromPath(assetPath);
                return true;
            }
            if (propertyName == "s_normalMap") {
                pbr->SetNormalTextureFromPath(assetPath);
                return true;
            }
            if (propertyName == "s_ao") {
                pbr->SetAmbientOcclusionTextureFromPath(assetPath);
                return true;
            }
            if (propertyName == "s_emission") {
                pbr->SetEmissionTextureFromPath(assetPath);
                return true;
            }
            if (propertyName == "s_displacement") {
                pbr->SetDisplacementTextureFromPath(assetPath);
                return true;
            }
            if (propertyName == "s_tintMask") {
                pbr->SetTintMaskTextureFromPath(assetPath);
                return true;
            }
        }

        return TrySetShaderGraphTexturePath(material, propertyName, assetPath);
    }

    bool TryGetMaterialVector4(const std::shared_ptr<Material>& material,
                               const std::string& propertyName,
                               glm::vec4& outValue)
    {
        if (!material) {
            return false;
        }

        if (TryGetShaderGraphVector4(material, propertyName, outValue)) {
            return true;
        }

        return material->TryGetUniform(propertyName, outValue);
    }

    void SetMaterialVector4Value(const std::shared_ptr<Material>& material,
                                 const std::string& propertyName,
                                 const glm::vec4& value)
    {
        if (!material) {
            return;
        }

        if (TrySetShaderGraphVector4(material, propertyName, value)) {
            return;
        }

        if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material)) {
            if (propertyName == "u_UVTransform") {
                pbr->SetUVTransform(glm::vec2(value.x, value.y), glm::vec2(value.z, value.w));
                return;
            }
            if (propertyName == "u_EmissionColor") {
                pbr->SetEmissionColor(glm::vec3(value.x, value.y, value.z));
                return;
            }
            if (propertyName == "u_PBRScalar0") {
                pbr->SetMetallic(value.x);
                pbr->SetRoughness(value.y);
                pbr->SetAmbientOcclusion(value.z);
                pbr->SetNormalScale(value.w);
                return;
            }
            if (propertyName == "u_PBRScalar1") {
                pbr->SetEmissionStrength(value.x);
                return;
            }
            if (propertyName == "u_DisplacementParams") {
                pbr->SetDisplacementScale(value.x);
                return;
            }
        }

        material->SetUniform(propertyName, value);
    }

}

// Function pointer definitions for managed interop
using HasComponent_fn = bool(*)(int, const char*);
using AddComponent_fn = void(*)(int, const char*);
using RemoveComponent_fn = void(*)(int, const char*);
using AddScript_fn = void(*)(int, const char*);

using GetLightType_fn = int(*)(int);
using SetLightType_fn = void(*)(int, int);
using GetLightColor_fn = void(*)(int, float*, float*, float*);
using SetLightColor_fn = void(*)(int, float, float, float);
using GetLightIntensity_fn = float(*)(int);
using SetLightIntensity_fn = void(*)(int, float);

using GetRigidBodyMass_fn = float(*)(int);
using SetRigidBodyMass_fn = void(*)(int, float);
using GetRigidBodyIsKinematic_fn = bool(*)(int);
using SetRigidBodyIsKinematic_fn = void(*)(int, bool);
using GetRigidBodyUseGravity_fn = bool(*)(int);
using SetRigidBodyUseGravity_fn = void(*)(int, bool);
using GetRigidBodyCollisionMask_fn = uint32_t(*)(int);
using SetRigidBodyCollisionMask_fn = void(*)(int, uint32_t);
using SetRigidBodyPhysicsLayer_fn = bool(*)(int, const char*);
using GetRigidBodyLinearVelocity_fn = void(*)(int, float*, float*, float*);
using SetRigidBodyLinearVelocity_fn = void(*)(int, float, float, float);
using GetRigidBodyAngularVelocity_fn = void(*)(int, float*, float*, float*);
using SetRigidBodyAngularVelocity_fn = void(*)(int, float, float, float);
using ApplyRigidBodyForce_fn = void(*)(int, float, float, float);
using ApplyRigidBodyTorque_fn = void(*)(int, float, float, float);
using ApplyRigidBodyImpulse_fn = void(*)(int, float, float, float);
using ApplyRigidBodyAngularImpulse_fn = void(*)(int, float, float, float);
using RigidBody_GetDebugSummary_fn = const char*(*)(int);
using Collider_GetOffset_fn = void(*)(int, float*, float*, float*);

// TerrainComponent
using Terrain_GetHeightAtWorld_fn = bool(*)(int, float, float, float*);
using Terrain_GetNormalAtWorld_fn = bool(*)(int, float, float, float*, float*, float*);
using Terrain_GetNearestPoint_fn = bool(*)(int, float, float, float*, float*, float*);
using Terrain_Raycast_fn = bool(*)(int, float, float, float, float, float, float, float*, float*, float*, float*, float*, float*);
using Terrain_GetDominantLayerAtWorld_fn = bool(*)(int, float, float, int*, float*);
using Terrain_SetHeightAtWorld_fn = bool(*)(int, float, float, float);
using Terrain_ApplyHeightDelta_fn = bool(*)(int, float, float, float, float, float);
using Terrain_GetInstancerLayerCount_fn = int(*)(int);
using Terrain_GetInstancerLayerName_fn = const char*(*)(int, int);
using Terrain_SetInstancerLayerEnabled_fn = bool(*)(int, int, bool);
using Terrain_SetInstancerLayerDensity_fn = bool(*)(int, int, float);
using Terrain_RegenerateInstancers_fn = bool(*)(int);

// PortalComponent
using Portal_GetEnabled_fn = bool(*)(int);
using Portal_SetEnabled_fn = void(*)(int, bool);
using Portal_GetTargetScenePath_fn = const char*(*)(int);
using Portal_SetTargetScenePath_fn = void(*)(int, const char*);
using Portal_GetTargetPortalGuid_fn = const char*(*)(int);
using Portal_SetTargetPortalGuid_fn = void(*)(int, const char*);
using Portal_GetTargetPortalPath_fn = const char*(*)(int);
using Portal_SetTargetPortalPath_fn = void(*)(int, const char*);
using Portal_GetVec3_fn = void(*)(int, float*, float*, float*);
using Portal_SetVec3_fn = void(*)(int, float, float, float);
using Portal_GetBool_fn = bool(*)(int);
using Portal_SetBool_fn = void(*)(int, bool);
using Portal_GetFloat_fn = float(*)(int);
using Portal_SetFloat_fn = void(*)(int, float);

using CC_SetDesiredVelocity_fn = void(*)(int, float, float, float);
using CC_GetDesiredVelocity_fn = void(*)(int, float*, float*, float*);
using CC_SetVerticalVelocity_fn = void(*)(int, float);
using CC_GetVerticalVelocity_fn = float(*)(int);
using CC_Jump_fn = void(*)(int, float);
using CC_IsGrounded_fn = bool(*)(int);
using CC_SetPosition_fn = void(*)(int, float, float, float);
using CC_GetCollisionMask_fn = uint32_t(*)(int);
using CC_SetCollisionMask_fn = void(*)(int, uint32_t);

using GetCameraLayerMask_fn = unsigned int(*)(int);
using SetCameraLayerMask_fn = void(*)(int, unsigned int);
using Camera_SetLayerMaskByName_fn = void(*)(int, const char*, bool);
// Camera settings
using GetCameraActive_fn = bool(*)(int);
using SetCameraActive_fn = void(*)(int, bool);
using GetCameraPriority_fn = int(*)(int);
using SetCameraPriority_fn = void(*)(int, int);
using GetCameraFieldOfView_fn = float(*)(int);
using SetCameraFieldOfView_fn = void(*)(int, float);
using GetCameraNearClip_fn = float(*)(int);
using SetCameraNearClip_fn = void(*)(int, float);
using GetCameraFarClip_fn = float(*)(int);
using SetCameraFarClip_fn = void(*)(int, float);
using GetCameraIsPerspective_fn = bool(*)(int);
using SetCameraIsPerspective_fn = void(*)(int, bool);

using SetBlendShapeWeight_fn = void(*)(int, const char*, float);
using GetBlendShapeWeight_fn = float(*)(int, const char*);
using GetBlendShapeCount_fn = int(*)(int);
using GetBlendShapeName_fn = const char*(*)(int, int);

using UnifiedMorph_GetCount_fn = int(*)(int);
using UnifiedMorph_GetName_fn = const char*(*)(int, int);
using UnifiedMorph_GetWeight_fn = float(*)(int, int);
using UnifiedMorph_SetWeight_fn = void(*)(int, int, float);
using UnifiedMorph_PropagateAll_fn = void(*)(int);

// TintMaskController function types
using TintController_HasComponent_fn = bool(*)(int);
using TintController_GetNamePattern_fn = const char*(*)(int);
using TintController_SetNamePattern_fn = void(*)(int, const char*);
using TintController_GetBaseTint_fn = void(*)(int, float*, float*, float*, float*);
using TintController_SetBaseTint_fn = void(*)(int, float, float, float, float);
using TintController_GetTintColor_fn = void(*)(int, int, float*, float*, float*, float*);
using TintController_SetTintColor_fn = void(*)(int, int, float, float, float, float);
using TintController_GetUseTintMask_fn = bool(*)(int);
using TintController_SetUseTintMask_fn = void(*)(int, bool);
using TintController_GetUsePbrOverrides_fn = bool(*)(int);
using TintController_SetUsePbrOverrides_fn = void(*)(int, bool);
using TintController_GetPbrMetallic_fn = float(*)(int);
using TintController_SetPbrMetallic_fn = void(*)(int, float);
using TintController_GetPbrRoughness_fn = float(*)(int);
using TintController_SetPbrRoughness_fn = void(*)(int, float);
using TintController_GetPbrEmissionColor_fn = void(*)(int, float*, float*, float*);
using TintController_SetPbrEmissionColor_fn = void(*)(int, float, float, float);
using TintController_GetPbrEmissionStrength_fn = float(*)(int);
using TintController_SetPbrEmissionStrength_fn = void(*)(int, float);
using TintController_GetGlobalBlendMode_fn = int(*)(int);
using TintController_SetGlobalBlendMode_fn = void(*)(int, int);
using TintController_GetAutoIncludeParentedSkinnedMeshes_fn = bool(*)(int);
using TintController_SetAutoIncludeParentedSkinnedMeshes_fn = void(*)(int, bool);
using TintController_Refresh_fn = void(*)(int);
using TintController_ClearTargets_fn = void(*)(int);
using TintController_RemoveTargetsForEntity_fn = void(*)(int, int);
using TintController_AddTarget_fn = void(*)(int, int, int, int, bool, float, float, float, float);
using TintController_GetTrackedTargetCount_fn = int(*)(int);
using TintController_GetTrackedTargetEntity_fn = int(*)(int, int);

// BoneAttachment function types
using BoneAttachment_HasComponent_fn = bool(*)(int);
using BoneAttachment_GetEnabled_fn = bool(*)(int);
using BoneAttachment_SetEnabled_fn = void(*)(int, bool);
using BoneAttachment_GetBoneName_fn = const char*(*)(int);
using BoneAttachment_SetBoneName_fn = void(*)(int, const char*);
using BoneAttachment_GetLocalPosition_fn = void(*)(int, float*, float*, float*);
using BoneAttachment_SetLocalPosition_fn = void(*)(int, float, float, float);
using BoneAttachment_GetLocalRotation_fn = void(*)(int, float*, float*, float*);
using BoneAttachment_SetLocalRotation_fn = void(*)(int, float, float, float);
using BoneAttachment_GetLocalScale_fn = void(*)(int, float*, float*, float*);
using BoneAttachment_SetLocalScale_fn = void(*)(int, float, float, float);
using BoneAttachment_GetInheritRotation_fn = bool(*)(int);
using BoneAttachment_SetInheritRotation_fn = void(*)(int, bool);
using BoneAttachment_GetInheritScale_fn = bool(*)(int);
using BoneAttachment_SetInheritScale_fn = void(*)(int, bool);
using BoneAttachment_IsResolved_fn = bool(*)(int);
using BoneAttachment_InvalidateResolution_fn = void(*)(int);
using BoneAttachment_GetSkeletonEntity_fn = int(*)(int);
using BoneAttachment_SetSkeletonEntity_fn = void(*)(int, int);

using Animator_SetBool_fn = void(*)(int, const char*, bool);
using Animator_SetInt_fn = void(*)(int, const char*, int);
using Animator_SetFloat_fn = void(*)(int, const char*, float);
using Animator_SetTrigger_fn = void(*)(int, const char*);
using Animator_ResetTrigger_fn = void(*)(int, const char*);
using Animator_GetBool_fn = bool(*)(int, const char*);
using Animator_GetInt_fn = int(*)(int, const char*);
using Animator_GetFloat_fn = float(*)(int, const char*);
using Animator_GetTrigger_fn = bool(*)(int, const char*);
using Animator_GetEnabled_fn = bool(*)(int);
using Animator_SetEnabled_fn = void(*)(int, bool);
using Animator_SetController_fn = void(*)(int, const char*, float);
using Animator_SetOverride_fn = void(*)(int, const char*);

using UI_ButtonIsHovered_fn = bool(*)(int);
using UI_ButtonIsPressed_fn = bool(*)(int);
using UI_ButtonWasClicked_fn = bool(*)(int);

// UI Panel interaction function types
using UI_Panel_IsHovered_fn = bool(*)(int);
using UI_Panel_IsPressed_fn = bool(*)(int);
using UI_Panel_IsDragging_fn = bool(*)(int);
using UI_Panel_DragStarted_fn = bool(*)(int);
using UI_Panel_DragEnded_fn = bool(*)(int);
using UI_Panel_WasDropped_fn = bool(*)(int);
using UI_Panel_GetDropSource_fn = int(*)(int);
using UI_Panel_GetDropTarget_fn = int(*)(int);
using UI_Panel_GetAllowDrag_fn = bool(*)(int);
using UI_Panel_SetAllowDrag_fn = void(*)(int, bool);
using UI_Panel_GetAllowDrop_fn = bool(*)(int);
using UI_Panel_SetAllowDrop_fn = void(*)(int, bool);

// UI Slider function types
using UI_Slider_GetValue_fn = float(*)(int);
using UI_Slider_SetValue_fn = void(*)(int, float);
using UI_Slider_GetMinValue_fn = float(*)(int);
using UI_Slider_SetMinValue_fn = void(*)(int, float);
using UI_Slider_GetMaxValue_fn = float(*)(int);
using UI_Slider_SetMaxValue_fn = void(*)(int, float);
using UI_Slider_IsHovered_fn = bool(*)(int);
using UI_Slider_IsDragging_fn = bool(*)(int);
using UI_Slider_ValueChanged_fn = bool(*)(int);

// UI ProgressBar function types
using UI_ProgressBar_GetValue_fn = float(*)(int);
using UI_ProgressBar_SetValue_fn = void(*)(int, float);
using UI_ProgressBar_GetMinValue_fn = float(*)(int);
using UI_ProgressBar_SetMinValue_fn = void(*)(int, float);
using UI_ProgressBar_GetMaxValue_fn = float(*)(int);
using UI_ProgressBar_SetMaxValue_fn = void(*)(int, float);
using UI_ProgressBar_GetOpacity_fn = float(*)(int);
using UI_ProgressBar_SetOpacity_fn = void(*)(int, float);
using UI_ProgressBar_GetVisible_fn = bool(*)(int);
using UI_ProgressBar_SetVisible_fn = void(*)(int, bool);

// UI Toggle function types
using UI_Toggle_GetIsOn_fn = bool(*)(int);
using UI_Toggle_SetIsOn_fn = void(*)(int, bool);
using UI_Toggle_IsHovered_fn = bool(*)(int);
using UI_Toggle_IsPressed_fn = bool(*)(int);
using UI_Toggle_ValueChanged_fn = bool(*)(int);

// UI ScrollView function types
using UI_ScrollView_GetContentOffset_fn = void(*)(int, float*, float*);
using UI_ScrollView_SetContentOffset_fn = void(*)(int, float, float);
using UI_ScrollView_GetContentSize_fn = void(*)(int, float*, float*);
using UI_ScrollView_SetContentSize_fn = void(*)(int, float, float);
using UI_ScrollView_GetOpacity_fn = float(*)(int);
using UI_ScrollView_SetOpacity_fn = void(*)(int, float);
using UI_ScrollView_GetVisible_fn = bool(*)(int);
using UI_ScrollView_SetVisible_fn = void(*)(int, bool);

// UI InputField function types
using UI_InputField_GetText_fn = void(*)(int, const char**);
using UI_InputField_SetText_fn = void(*)(int, const char*);
using UI_InputField_GetPlaceholder_fn = void(*)(int, const char**);
using UI_InputField_SetPlaceholder_fn = void(*)(int, const char*);
using UI_InputField_IsFocused_fn = bool(*)(int);
using UI_InputField_TextChanged_fn = bool(*)(int);

// UI Dropdown function types
using UI_Dropdown_GetSelectedIndex_fn = int(*)(int);
using UI_Dropdown_SetSelectedIndex_fn = void(*)(int, int);
using UI_Dropdown_GetOptionCount_fn = int(*)(int);
using UI_Dropdown_GetOption_fn = void(*)(int, int, const char**);
using UI_Dropdown_SetOption_fn = void(*)(int, int, const char*);
using UI_Dropdown_AddOption_fn = void(*)(int, const char*);
using UI_Dropdown_ClearOptions_fn = void(*)(int);
using UI_Dropdown_IsOpen_fn = bool(*)(int);
using UI_Dropdown_SelectionChanged_fn = bool(*)(int);

// Managed Logging
using ManagedLog_fn = void(*)(int, const char*);

// Exported function pointers
ManagedLog_fn ManagedLogPtr = &ManagedLog;

HasComponent_fn HasComponentPtr = &HasComponent;
AddComponent_fn AddComponentPtr = &AddComponent;
RemoveComponent_fn RemoveComponentPtr = &RemoveComponent;
AddScript_fn AddScriptPtr = &AddScript;

GetLightType_fn GetLightTypePtr = &GetLightType;
SetLightType_fn SetLightTypePtr = &SetLightType;
GetLightColor_fn GetLightColorPtr = &GetLightColor;
SetLightColor_fn SetLightColorPtr = &SetLightColor;
GetLightIntensity_fn GetLightIntensityPtr = &GetLightIntensity;
SetLightIntensity_fn SetLightIntensityPtr = &SetLightIntensity;

GetRigidBodyMass_fn GetRigidBodyMassPtr = &GetRigidBodyMass;
SetRigidBodyMass_fn SetRigidBodyMassPtr = &SetRigidBodyMass;
GetRigidBodyIsKinematic_fn GetRigidBodyIsKinematicPtr = &GetRigidBodyIsKinematic;
SetRigidBodyIsKinematic_fn SetRigidBodyIsKinematicPtr = &SetRigidBodyIsKinematic;
GetRigidBodyUseGravity_fn GetRigidBodyUseGravityPtr = &GetRigidBodyUseGravity;
SetRigidBodyUseGravity_fn SetRigidBodyUseGravityPtr = &SetRigidBodyUseGravity;
GetRigidBodyCollisionMask_fn GetRigidBodyCollisionMaskPtr = &GetRigidBodyCollisionMask;
SetRigidBodyCollisionMask_fn SetRigidBodyCollisionMaskPtr = &SetRigidBodyCollisionMask;
SetRigidBodyPhysicsLayer_fn SetRigidBodyPhysicsLayerPtr = &SetRigidBodyPhysicsLayer;
GetRigidBodyLinearVelocity_fn GetRigidBodyLinearVelocityPtr = &GetRigidBodyLinearVelocity;
SetRigidBodyLinearVelocity_fn SetRigidBodyLinearVelocityPtr = &SetRigidBodyLinearVelocity;
GetRigidBodyAngularVelocity_fn GetRigidBodyAngularVelocityPtr = &GetRigidBodyAngularVelocity;
SetRigidBodyAngularVelocity_fn SetRigidBodyAngularVelocityPtr = &SetRigidBodyAngularVelocity;
ApplyRigidBodyForce_fn ApplyRigidBodyForcePtr = &ApplyRigidBodyForce;
ApplyRigidBodyTorque_fn ApplyRigidBodyTorquePtr = &ApplyRigidBodyTorque;
ApplyRigidBodyImpulse_fn ApplyRigidBodyImpulsePtr = &ApplyRigidBodyImpulse;
ApplyRigidBodyAngularImpulse_fn ApplyRigidBodyAngularImpulsePtr = &ApplyRigidBodyAngularImpulse;
RigidBody_GetDebugSummary_fn RigidBody_GetDebugSummaryPtr = &RigidBody_GetDebugSummary;
Collider_GetOffset_fn Collider_GetOffsetPtr = &Collider_GetOffset;

Terrain_GetHeightAtWorld_fn Terrain_GetHeightAtWorldPtr = &Terrain_GetHeightAtWorld;
Terrain_GetNormalAtWorld_fn Terrain_GetNormalAtWorldPtr = &Terrain_GetNormalAtWorld;
Terrain_GetNearestPoint_fn Terrain_GetNearestPointPtr = &Terrain_GetNearestPoint;
Terrain_Raycast_fn Terrain_RaycastPtr = &Terrain_Raycast;
Terrain_GetDominantLayerAtWorld_fn Terrain_GetDominantLayerAtWorldPtr = &Terrain_GetDominantLayerAtWorld;
Terrain_SetHeightAtWorld_fn Terrain_SetHeightAtWorldPtr = &Terrain_SetHeightAtWorld;
Terrain_ApplyHeightDelta_fn Terrain_ApplyHeightDeltaPtr = &Terrain_ApplyHeightDelta;
Terrain_GetInstancerLayerCount_fn Terrain_GetInstancerLayerCountPtr = &Terrain_GetInstancerLayerCount;
Terrain_GetInstancerLayerName_fn Terrain_GetInstancerLayerNamePtr = &Terrain_GetInstancerLayerName;
Terrain_SetInstancerLayerEnabled_fn Terrain_SetInstancerLayerEnabledPtr = &Terrain_SetInstancerLayerEnabled;
Terrain_SetInstancerLayerDensity_fn Terrain_SetInstancerLayerDensityPtr = &Terrain_SetInstancerLayerDensity;
Terrain_RegenerateInstancers_fn Terrain_RegenerateInstancersPtr = &Terrain_RegenerateInstancers;

using Spline_GetControlPointCount_fn = int(*)(int);
using Spline_GetControlPoint_fn = bool(*)(int, int, float*, float*, float*);
using Spline_GetSampledPointCount_fn = int(*)(int);
using Spline_GetSampledPoint_fn = bool(*)(int, int, float*, float*, float*);
using Spline_GetNearestPoint_fn = bool(*)(int, float, float, float, float*, float*, float*, float*);
using Spline_GetPointAtNormalized_fn = bool(*)(int, float, float*, float*, float*);

Spline_GetControlPointCount_fn Spline_GetControlPointCountPtr = &Spline_GetControlPointCount;
Spline_GetControlPoint_fn Spline_GetControlPointPtr = &Spline_GetControlPoint;
Spline_GetSampledPointCount_fn Spline_GetSampledPointCountPtr = &Spline_GetSampledPointCount;
Spline_GetSampledPoint_fn Spline_GetSampledPointPtr = &Spline_GetSampledPoint;
Spline_GetNearestPoint_fn Spline_GetNearestPointPtr = &Spline_GetNearestPoint;
Spline_GetPointAtNormalized_fn Spline_GetPointAtNormalizedPtr = &Spline_GetPointAtNormalized;

Portal_GetEnabled_fn Portal_GetEnabledPtr = &Portal_GetEnabled;
Portal_SetEnabled_fn Portal_SetEnabledPtr = &Portal_SetEnabled;
Portal_GetTargetScenePath_fn Portal_GetTargetScenePathPtr = &Portal_GetTargetScenePath;
Portal_SetTargetScenePath_fn Portal_SetTargetScenePathPtr = &Portal_SetTargetScenePath;
Portal_GetTargetPortalGuid_fn Portal_GetTargetPortalGuidPtr = &Portal_GetTargetPortalGuid;
Portal_SetTargetPortalGuid_fn Portal_SetTargetPortalGuidPtr = &Portal_SetTargetPortalGuid;
Portal_GetTargetPortalPath_fn Portal_GetTargetPortalPathPtr = &Portal_GetTargetPortalPath;
Portal_SetTargetPortalPath_fn Portal_SetTargetPortalPathPtr = &Portal_SetTargetPortalPath;
Portal_GetVec3_fn Portal_GetEntryOffsetPtr = &Portal_GetEntryOffset;
Portal_SetVec3_fn Portal_SetEntryOffsetPtr = &Portal_SetEntryOffset;
Portal_GetVec3_fn Portal_GetExitOffsetPtr = &Portal_GetExitOffset;
Portal_SetVec3_fn Portal_SetExitOffsetPtr = &Portal_SetExitOffset;
Portal_GetBool_fn Portal_GetAutoDetectPtr = &Portal_GetAutoDetect;
Portal_SetBool_fn Portal_SetAutoDetectPtr = &Portal_SetAutoDetect;
Portal_GetFloat_fn Portal_GetTriggerRadiusPtr = &Portal_GetTriggerRadius;
Portal_SetFloat_fn Portal_SetTriggerRadiusPtr = &Portal_SetTriggerRadius;
Portal_GetBool_fn Portal_GetFireExitEventsPtr = &Portal_GetFireExitEvents;
Portal_SetBool_fn Portal_SetFireExitEventsPtr = &Portal_SetFireExitEvents;

CC_SetDesiredVelocity_fn CC_SetDesiredVelocityPtr = &CC_SetDesiredVelocity;
CC_GetDesiredVelocity_fn CC_GetDesiredVelocityPtr = &CC_GetDesiredVelocity;
CC_SetVerticalVelocity_fn CC_SetVerticalVelocityPtr = &CC_SetVerticalVelocity;
CC_GetVerticalVelocity_fn CC_GetVerticalVelocityPtr = &CC_GetVerticalVelocity;
CC_Jump_fn CC_JumpPtr = &CC_Jump;
CC_IsGrounded_fn CC_IsGroundedPtr = &CC_IsGrounded;
CC_SetPosition_fn CC_SetPositionPtr = &CC_SetPosition;
CC_GetCollisionMask_fn CC_GetCollisionMaskPtr = &CC_GetCollisionMask;
CC_SetCollisionMask_fn CC_SetCollisionMaskPtr = &CC_SetCollisionMask;

GetCameraLayerMask_fn GetCameraLayerMaskPtr = &GetCameraLayerMask;
SetCameraLayerMask_fn SetCameraLayerMaskPtr = &SetCameraLayerMask;
Camera_SetLayerMaskByName_fn Camera_SetLayerMaskByNamePtr = &Camera_SetLayerMaskByName;
// Camera settings pointers
GetCameraActive_fn GetCameraActivePtr = &GetCameraActive;
SetCameraActive_fn SetCameraActivePtr = &SetCameraActive;
GetCameraPriority_fn GetCameraPriorityPtr = &GetCameraPriority;
SetCameraPriority_fn SetCameraPriorityPtr = &SetCameraPriority;
GetCameraFieldOfView_fn GetCameraFieldOfViewPtr = &GetCameraFieldOfView;
SetCameraFieldOfView_fn SetCameraFieldOfViewPtr = &SetCameraFieldOfView;
GetCameraNearClip_fn GetCameraNearClipPtr = &GetCameraNearClip;
SetCameraNearClip_fn SetCameraNearClipPtr = &SetCameraNearClip;
GetCameraFarClip_fn GetCameraFarClipPtr = &GetCameraFarClip;
SetCameraFarClip_fn SetCameraFarClipPtr = &SetCameraFarClip;
GetCameraIsPerspective_fn GetCameraIsPerspectivePtr = &GetCameraIsPerspective;
SetCameraIsPerspective_fn SetCameraIsPerspectivePtr = &SetCameraIsPerspective;

SetBlendShapeWeight_fn SetBlendShapeWeightPtr = &SetBlendShapeWeight;
GetBlendShapeWeight_fn GetBlendShapeWeightPtr = &GetBlendShapeWeight;
GetBlendShapeCount_fn GetBlendShapeCountPtr = &GetBlendShapeCount;
GetBlendShapeName_fn GetBlendShapeNamePtr = &GetBlendShapeName;

UnifiedMorph_GetCount_fn UnifiedMorph_GetCountPtr = &UnifiedMorph_GetCount;
UnifiedMorph_GetName_fn UnifiedMorph_GetNamePtr = &UnifiedMorph_GetName;
UnifiedMorph_GetWeight_fn UnifiedMorph_GetWeightPtr = &UnifiedMorph_GetWeight;
UnifiedMorph_SetWeight_fn UnifiedMorph_SetWeightPtr = &UnifiedMorph_SetWeight;
UnifiedMorph_PropagateAll_fn UnifiedMorph_PropagateAllPtr = &UnifiedMorph_PropagateAll;

// TintMaskController
TintController_HasComponent_fn TintController_HasComponentPtr = &TintController_HasComponent;
TintController_GetNamePattern_fn TintController_GetNamePatternPtr = &TintController_GetNamePattern;
TintController_SetNamePattern_fn TintController_SetNamePatternPtr = &TintController_SetNamePattern;
TintController_GetBaseTint_fn TintController_GetBaseTintPtr = &TintController_GetBaseTint;
TintController_SetBaseTint_fn TintController_SetBaseTintPtr = &TintController_SetBaseTint;
TintController_GetTintColor_fn TintController_GetTintColorPtr = &TintController_GetTintColor;
TintController_SetTintColor_fn TintController_SetTintColorPtr = &TintController_SetTintColor;
TintController_GetUseTintMask_fn TintController_GetUseTintMaskPtr = &TintController_GetUseTintMask;
TintController_SetUseTintMask_fn TintController_SetUseTintMaskPtr = &TintController_SetUseTintMask;
TintController_GetUsePbrOverrides_fn TintController_GetUsePbrOverridesPtr = &TintController_GetUsePbrOverrides;
TintController_SetUsePbrOverrides_fn TintController_SetUsePbrOverridesPtr = &TintController_SetUsePbrOverrides;
TintController_GetPbrMetallic_fn TintController_GetPbrMetallicPtr = &TintController_GetPbrMetallic;
TintController_SetPbrMetallic_fn TintController_SetPbrMetallicPtr = &TintController_SetPbrMetallic;
TintController_GetPbrRoughness_fn TintController_GetPbrRoughnessPtr = &TintController_GetPbrRoughness;
TintController_SetPbrRoughness_fn TintController_SetPbrRoughnessPtr = &TintController_SetPbrRoughness;
TintController_GetPbrEmissionColor_fn TintController_GetPbrEmissionColorPtr = &TintController_GetPbrEmissionColor;
TintController_SetPbrEmissionColor_fn TintController_SetPbrEmissionColorPtr = &TintController_SetPbrEmissionColor;
TintController_GetPbrEmissionStrength_fn TintController_GetPbrEmissionStrengthPtr = &TintController_GetPbrEmissionStrength;
TintController_SetPbrEmissionStrength_fn TintController_SetPbrEmissionStrengthPtr = &TintController_SetPbrEmissionStrength;
TintController_GetGlobalBlendMode_fn TintController_GetGlobalBlendModePtr = &TintController_GetGlobalBlendMode;
TintController_SetGlobalBlendMode_fn TintController_SetGlobalBlendModePtr = &TintController_SetGlobalBlendMode;
TintController_GetAutoIncludeParentedSkinnedMeshes_fn TintController_GetAutoIncludeParentedSkinnedMeshesPtr = &TintController_GetAutoIncludeParentedSkinnedMeshes;
TintController_SetAutoIncludeParentedSkinnedMeshes_fn TintController_SetAutoIncludeParentedSkinnedMeshesPtr = &TintController_SetAutoIncludeParentedSkinnedMeshes;
TintController_Refresh_fn TintController_RefreshPtr = &TintController_Refresh;
TintController_ClearTargets_fn TintController_ClearTargetsPtr = &TintController_ClearTargets;
TintController_RemoveTargetsForEntity_fn TintController_RemoveTargetsForEntityPtr = &TintController_RemoveTargetsForEntity;
TintController_AddTarget_fn TintController_AddTargetPtr = &TintController_AddTarget;
TintController_GetTrackedTargetCount_fn TintController_GetTrackedTargetCountPtr = &TintController_GetTrackedTargetCount;
TintController_GetTrackedTargetEntity_fn TintController_GetTrackedTargetEntityPtr = &TintController_GetTrackedTargetEntity;

// BoneAttachment
BoneAttachment_HasComponent_fn BoneAttachment_HasComponentPtr = &BoneAttachment_HasComponent;
BoneAttachment_GetEnabled_fn BoneAttachment_GetEnabledPtr = &BoneAttachment_GetEnabled;
BoneAttachment_SetEnabled_fn BoneAttachment_SetEnabledPtr = &BoneAttachment_SetEnabled;
BoneAttachment_GetBoneName_fn BoneAttachment_GetBoneNamePtr = &BoneAttachment_GetBoneName;
BoneAttachment_SetBoneName_fn BoneAttachment_SetBoneNamePtr = &BoneAttachment_SetBoneName;
BoneAttachment_GetLocalPosition_fn BoneAttachment_GetLocalPositionPtr = &BoneAttachment_GetLocalPosition;
BoneAttachment_SetLocalPosition_fn BoneAttachment_SetLocalPositionPtr = &BoneAttachment_SetLocalPosition;
BoneAttachment_GetLocalRotation_fn BoneAttachment_GetLocalRotationPtr = &BoneAttachment_GetLocalRotation;
BoneAttachment_SetLocalRotation_fn BoneAttachment_SetLocalRotationPtr = &BoneAttachment_SetLocalRotation;
BoneAttachment_GetLocalScale_fn BoneAttachment_GetLocalScalePtr = &BoneAttachment_GetLocalScale;
BoneAttachment_SetLocalScale_fn BoneAttachment_SetLocalScalePtr = &BoneAttachment_SetLocalScale;
BoneAttachment_GetInheritRotation_fn BoneAttachment_GetInheritRotationPtr = &BoneAttachment_GetInheritRotation;
BoneAttachment_SetInheritRotation_fn BoneAttachment_SetInheritRotationPtr = &BoneAttachment_SetInheritRotation;
BoneAttachment_GetInheritScale_fn BoneAttachment_GetInheritScalePtr = &BoneAttachment_GetInheritScale;
BoneAttachment_SetInheritScale_fn BoneAttachment_SetInheritScalePtr = &BoneAttachment_SetInheritScale;
BoneAttachment_IsResolved_fn BoneAttachment_IsResolvedPtr = &BoneAttachment_IsResolved;
BoneAttachment_InvalidateResolution_fn BoneAttachment_InvalidateResolutionPtr = &BoneAttachment_InvalidateResolution;
BoneAttachment_GetSkeletonEntity_fn BoneAttachment_GetSkeletonEntityPtr = &BoneAttachment_GetSkeletonEntity;
BoneAttachment_SetSkeletonEntity_fn BoneAttachment_SetSkeletonEntityPtr = &BoneAttachment_SetSkeletonEntity;

Animator_SetBool_fn Animator_SetBoolPtr = &Animator_SetBool;
Animator_SetInt_fn Animator_SetIntPtr = &Animator_SetInt;
Animator_SetFloat_fn Animator_SetFloatPtr = &Animator_SetFloat;
Animator_SetTrigger_fn Animator_SetTriggerPtr = &Animator_SetTrigger;
Animator_ResetTrigger_fn Animator_ResetTriggerPtr = &Animator_ResetTrigger;
Animator_GetBool_fn Animator_GetBoolPtr = &Animator_GetBool;
Animator_GetInt_fn Animator_GetIntPtr = &Animator_GetInt;
Animator_GetFloat_fn Animator_GetFloatPtr = &Animator_GetFloat;
Animator_GetTrigger_fn Animator_GetTriggerPtr = &Animator_GetTrigger;
Animator_GetEnabled_fn Animator_GetEnabledPtr = &Animator_GetEnabled;
Animator_SetEnabled_fn Animator_SetEnabledPtr = &Animator_SetEnabled;
Animator_SetController_fn Animator_SetControllerPtr = &Animator_SetController;
Animator_SetOverride_fn Animator_SetOverridePtr = &Animator_SetOverride;

UI_ButtonIsHovered_fn UI_ButtonIsHoveredPtr = &UI_ButtonIsHovered;
UI_ButtonIsPressed_fn UI_ButtonIsPressedPtr = &UI_ButtonIsPressed;
UI_ButtonWasClicked_fn UI_ButtonWasClickedPtr = &UI_ButtonWasClicked;

// UI Slider function pointers
UI_Slider_GetValue_fn UI_Slider_GetValuePtr = &UI_Slider_GetValue;
UI_Slider_SetValue_fn UI_Slider_SetValuePtr = &UI_Slider_SetValue;
UI_Slider_GetMinValue_fn UI_Slider_GetMinValuePtr = &UI_Slider_GetMinValue;
UI_Slider_SetMinValue_fn UI_Slider_SetMinValuePtr = &UI_Slider_SetMinValue;
UI_Slider_GetMaxValue_fn UI_Slider_GetMaxValuePtr = &UI_Slider_GetMaxValue;
UI_Slider_SetMaxValue_fn UI_Slider_SetMaxValuePtr = &UI_Slider_SetMaxValue;
UI_Slider_IsHovered_fn UI_Slider_IsHoveredPtr = &UI_Slider_IsHovered;
UI_Slider_IsDragging_fn UI_Slider_IsDraggingPtr = &UI_Slider_IsDragging;
UI_Slider_ValueChanged_fn UI_Slider_ValueChangedPtr = &UI_Slider_ValueChanged;

// UI ProgressBar function pointers
UI_ProgressBar_GetValue_fn UI_ProgressBar_GetValuePtr = &UI_ProgressBar_GetValue;
UI_ProgressBar_SetValue_fn UI_ProgressBar_SetValuePtr = &UI_ProgressBar_SetValue;
UI_ProgressBar_GetMinValue_fn UI_ProgressBar_GetMinValuePtr = &UI_ProgressBar_GetMinValue;
UI_ProgressBar_SetMinValue_fn UI_ProgressBar_SetMinValuePtr = &UI_ProgressBar_SetMinValue;
UI_ProgressBar_GetMaxValue_fn UI_ProgressBar_GetMaxValuePtr = &UI_ProgressBar_GetMaxValue;
UI_ProgressBar_SetMaxValue_fn UI_ProgressBar_SetMaxValuePtr = &UI_ProgressBar_SetMaxValue;
UI_ProgressBar_GetOpacity_fn UI_ProgressBar_GetOpacityPtr = &UI_ProgressBar_GetOpacity;
UI_ProgressBar_SetOpacity_fn UI_ProgressBar_SetOpacityPtr = &UI_ProgressBar_SetOpacity;
UI_ProgressBar_GetVisible_fn UI_ProgressBar_GetVisiblePtr = &UI_ProgressBar_GetVisible;
UI_ProgressBar_SetVisible_fn UI_ProgressBar_SetVisiblePtr = &UI_ProgressBar_SetVisible;

// UI Toggle function pointers
UI_Toggle_GetIsOn_fn UI_Toggle_GetIsOnPtr = &UI_Toggle_GetIsOn;
UI_Toggle_SetIsOn_fn UI_Toggle_SetIsOnPtr = &UI_Toggle_SetIsOn;
UI_Toggle_IsHovered_fn UI_Toggle_IsHoveredPtr = &UI_Toggle_IsHovered;
UI_Toggle_IsPressed_fn UI_Toggle_IsPressedPtr = &UI_Toggle_IsPressed;
UI_Toggle_ValueChanged_fn UI_Toggle_ValueChangedPtr = &UI_Toggle_ValueChanged;

// UI ScrollView function pointers
UI_ScrollView_GetContentOffset_fn UI_ScrollView_GetContentOffsetPtr = &UI_ScrollView_GetContentOffset;
UI_ScrollView_SetContentOffset_fn UI_ScrollView_SetContentOffsetPtr = &UI_ScrollView_SetContentOffset;
UI_ScrollView_GetContentSize_fn UI_ScrollView_GetContentSizePtr = &UI_ScrollView_GetContentSize;
UI_ScrollView_SetContentSize_fn UI_ScrollView_SetContentSizePtr = &UI_ScrollView_SetContentSize;
UI_ScrollView_GetOpacity_fn UI_ScrollView_GetOpacityPtr = &UI_ScrollView_GetOpacity;
UI_ScrollView_SetOpacity_fn UI_ScrollView_SetOpacityPtr = &UI_ScrollView_SetOpacity;
UI_ScrollView_GetVisible_fn UI_ScrollView_GetVisiblePtr = &UI_ScrollView_GetVisible;
UI_ScrollView_SetVisible_fn UI_ScrollView_SetVisiblePtr = &UI_ScrollView_SetVisible;

// UI InputField function pointers
UI_InputField_GetText_fn UI_InputField_GetTextPtr = &UI_InputField_GetText;
UI_InputField_SetText_fn UI_InputField_SetTextPtr = &UI_InputField_SetText;
UI_InputField_GetPlaceholder_fn UI_InputField_GetPlaceholderPtr = &UI_InputField_GetPlaceholder;
UI_InputField_SetPlaceholder_fn UI_InputField_SetPlaceholderPtr = &UI_InputField_SetPlaceholder;
UI_InputField_IsFocused_fn UI_InputField_IsFocusedPtr = &UI_InputField_IsFocused;
UI_InputField_TextChanged_fn UI_InputField_TextChangedPtr = &UI_InputField_TextChanged;

// UI Dropdown function pointers
UI_Dropdown_GetSelectedIndex_fn UI_Dropdown_GetSelectedIndexPtr = &UI_Dropdown_GetSelectedIndex;
UI_Dropdown_SetSelectedIndex_fn UI_Dropdown_SetSelectedIndexPtr = &UI_Dropdown_SetSelectedIndex;
UI_Dropdown_GetOptionCount_fn UI_Dropdown_GetOptionCountPtr = &UI_Dropdown_GetOptionCount;
UI_Dropdown_GetOption_fn UI_Dropdown_GetOptionPtr = &UI_Dropdown_GetOption;
UI_Dropdown_SetOption_fn UI_Dropdown_SetOptionPtr = &UI_Dropdown_SetOption;
UI_Dropdown_AddOption_fn UI_Dropdown_AddOptionPtr = &UI_Dropdown_AddOption;
UI_Dropdown_ClearOptions_fn UI_Dropdown_ClearOptionsPtr = &UI_Dropdown_ClearOptions;
UI_Dropdown_IsOpen_fn UI_Dropdown_IsOpenPtr = &UI_Dropdown_IsOpen;
UI_Dropdown_SelectionChanged_fn UI_Dropdown_SelectionChangedPtr = &UI_Dropdown_SelectionChanged;

// UI Text functions
static void UI_Text_GetText(int entityId, const char** outText) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text || !outText) {
        if (outText) *outText = "";
        return;
    }
    static thread_local std::string s_textBuffer;
    s_textBuffer = data->Text->Text;
    *outText = s_textBuffer.c_str();
}

static void UI_Text_SetText(int entityId, const char* text) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text || !text) return;
    data->Text->Text = text;
}

static float UI_Text_GetOpacity(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return 1.0f;
    return data->Text->Opacity;
}

static void UI_Text_SetOpacity(int entityId, float opacity) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->Opacity = opacity;
}

static bool UI_Text_GetVisible(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return true;
    return data->Text->Visible;
}

static void UI_Text_SetVisible(int entityId, bool visible) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->Visible = visible;
}

static void UI_Text_GetColor(int entityId, float* r, float* g, float* b, float* a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) {
        if (r) *r = 1.0f; if (g) *g = 1.0f; if (b) *b = 1.0f; if (a) *a = 1.0f;
        return;
    }
    // ColorAbgr is 0xAABBGGRR
    uint32_t c = data->Text->ColorAbgr;
    if (r) *r = (c & 0xFF) / 255.0f;
    if (g) *g = ((c >> 8) & 0xFF) / 255.0f;
    if (b) *b = ((c >> 16) & 0xFF) / 255.0f;
    if (a) *a = ((c >> 24) & 0xFF) / 255.0f;
}

static void UI_Text_SetColor(int entityId, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    uint8_t ri = static_cast<uint8_t>(glm::clamp(r, 0.0f, 1.0f) * 255.0f);
    uint8_t gi = static_cast<uint8_t>(glm::clamp(g, 0.0f, 1.0f) * 255.0f);
    uint8_t bi = static_cast<uint8_t>(glm::clamp(b, 0.0f, 1.0f) * 255.0f);
    uint8_t ai = static_cast<uint8_t>(glm::clamp(a, 0.0f, 1.0f) * 255.0f);
    data->Text->ColorAbgr = (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

static float UI_Text_GetPixelSize(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return 32.0f;
    return data->Text->PixelSize;
}

static void UI_Text_SetPixelSize(int entityId, float size) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->PixelSize = size;
}

static int UI_Text_GetZOrder(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return 0;
    return data->Text->ZOrder;
}

static void UI_Text_SetZOrder(int entityId, int zOrder) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->ZOrder = zOrder;
}

static void UI_Text_GetFontPath(int entityId, const char** outPath) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text || !outPath) {
        if (outPath) *outPath = "";
        return;
    }
    static thread_local std::string s_fontPathBuffer;
    s_fontPathBuffer = data->Text->FontPath;
    *outPath = s_fontPathBuffer.c_str();
}

static void UI_Text_SetFontPath(int entityId, const char* path) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->FontPath = path ? path : "";
}

static bool UI_Text_GetAnchorEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return false;
    return data->Text->AnchorEnabled;
}

static void UI_Text_SetAnchorEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->AnchorEnabled = enabled;
}

static int UI_Text_GetAnchor(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return 0;
    return static_cast<int>(data->Text->Anchor);
}

static void UI_Text_SetAnchor(int entityId, int anchor) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->Anchor = static_cast<UIAnchorPreset>(anchor);
}

static void UI_Text_GetAnchorOffset(int entityId, float* x, float* y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) {
        if (x) *x = 0.0f; if (y) *y = 0.0f;
        return;
    }
    if (x) *x = data->Text->AnchorOffset.x;
    if (y) *y = data->Text->AnchorOffset.y;
}

static void UI_Text_SetAnchorOffset(int entityId, float x, float y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->AnchorOffset = glm::vec2(x, y);
}

static bool UI_Text_GetWordWrap(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return false;
    return data->Text->WordWrap;
}

static void UI_Text_SetWordWrap(int entityId, bool wrap) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->WordWrap = wrap;
}

static void UI_Text_GetRectSize(int entityId, float* w, float* h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) {
        if (w) *w = 0.0f; if (h) *h = 0.0f;
        return;
    }
    if (w) *w = data->Text->RectSize.x;
    if (h) *h = data->Text->RectSize.y;
}

static void UI_Text_SetRectSize(int entityId, float w, float h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->RectSize = glm::vec2(w, h);
}

static bool UI_Text_GetWorldSpace(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return true;
    return data->Text->WorldSpace;
}

static void UI_Text_SetWorldSpace(int entityId, bool worldSpace) {
    auto& scene = Scene::Get();
    auto* data = scene.GetEntityData(entityId);
    if (!data || !data->Text) return;
    if (worldSpace && (EntityUsesCanvasBackedUI(data) || HasCanvasAncestor(scene, entityId)))
        return;
    data->Text->WorldSpace = worldSpace;
}

static bool UI_Text_GetBillboard(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return true;
    return data->Text->Billboard;
}

static void UI_Text_SetBillboard(int entityId, bool billboard) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->Billboard = billboard;
}

static bool UI_Text_GetOutlineEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return false;
    return data->Text->OutlineEnabled;
}

static void UI_Text_SetOutlineEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->OutlineEnabled = enabled;
}

static void UI_Text_GetOutlineColor(int entityId, float* r, float* g, float* b, float* a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) {
        if (r) *r = 0.0f; if (g) *g = 0.0f; if (b) *b = 0.0f; if (a) *a = 1.0f;
        return;
    }
    uint32_t c = data->Text->OutlineColorAbgr;
    if (r) *r = (c & 0xFF) / 255.0f;
    if (g) *g = ((c >> 8) & 0xFF) / 255.0f;
    if (b) *b = ((c >> 16) & 0xFF) / 255.0f;
    if (a) *a = ((c >> 24) & 0xFF) / 255.0f;
}

static void UI_Text_SetOutlineColor(int entityId, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    uint8_t ri = static_cast<uint8_t>(glm::clamp(r, 0.0f, 1.0f) * 255.0f);
    uint8_t gi = static_cast<uint8_t>(glm::clamp(g, 0.0f, 1.0f) * 255.0f);
    uint8_t bi = static_cast<uint8_t>(glm::clamp(b, 0.0f, 1.0f) * 255.0f);
    uint8_t ai = static_cast<uint8_t>(glm::clamp(a, 0.0f, 1.0f) * 255.0f);
    data->Text->OutlineColorAbgr = (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

static float UI_Text_GetOutlineThickness(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return 1.0f;
    return data->Text->OutlineThickness;
}

static void UI_Text_SetOutlineThickness(int entityId, float thickness) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->OutlineThickness = std::max(0.0f, thickness);
}

static bool UI_Text_GetShadowEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return false;
    return data->Text->ShadowEnabled;
}

static void UI_Text_SetShadowEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->ShadowEnabled = enabled;
}

static void UI_Text_GetShadowColor(int entityId, float* r, float* g, float* b, float* a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) {
        if (r) *r = 0.0f; if (g) *g = 0.0f; if (b) *b = 0.0f; if (a) *a = 0.5f;
        return;
    }
    uint32_t c = data->Text->ShadowColorAbgr;
    if (r) *r = (c & 0xFF) / 255.0f;
    if (g) *g = ((c >> 8) & 0xFF) / 255.0f;
    if (b) *b = ((c >> 16) & 0xFF) / 255.0f;
    if (a) *a = ((c >> 24) & 0xFF) / 255.0f;
}

static void UI_Text_SetShadowColor(int entityId, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    uint8_t ri = static_cast<uint8_t>(glm::clamp(r, 0.0f, 1.0f) * 255.0f);
    uint8_t gi = static_cast<uint8_t>(glm::clamp(g, 0.0f, 1.0f) * 255.0f);
    uint8_t bi = static_cast<uint8_t>(glm::clamp(b, 0.0f, 1.0f) * 255.0f);
    uint8_t ai = static_cast<uint8_t>(glm::clamp(a, 0.0f, 1.0f) * 255.0f);
    data->Text->ShadowColorAbgr = (ai << 24) | (bi << 16) | (gi << 8) | ri;
}

static void UI_Text_GetShadowOffset(int entityId, float* x, float* y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) {
        if (x) *x = 2.0f;
        if (y) *y = 2.0f;
        return;
    }
    if (x) *x = data->Text->ShadowOffset.x;
    if (y) *y = data->Text->ShadowOffset.y;
}

static void UI_Text_SetShadowOffset(int entityId, float x, float y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    data->Text->ShadowOffset = glm::vec2(x, y);
}

static int UI_Text_GetAlignment(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return 0;
    return static_cast<int>(data->Text->TextAlignment);
}

static void UI_Text_SetAlignment(int entityId, int alignment) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Text) return;
    alignment = std::max(0, std::min(2, alignment));
    data->Text->TextAlignment = static_cast<TextRendererComponent::Alignment>(alignment);
}

// UI Panel functions
static float UI_Panel_GetOpacity(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return 1.0f;
    return data->Panel->Opacity;
}

static void UI_Panel_SetOpacity(int entityId, float opacity) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->Opacity = opacity;
}

static bool UI_Panel_GetVisible(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->Visible;
}

static void UI_Panel_SetVisible(int entityId, bool visible) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->Visible = visible;
    // NOTE: Component-level visibility (Panel.visible) does NOT propagate to children.
    // Use Entity.SetVisible() for hierarchical visibility propagation.
    // This allows independent fade animations on child components.
}

static void UI_Panel_GetSize(int entityId, float* w, float* h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) { *w = 100; *h = 100; return; }
    *w = data->Panel->Size.x;
    *h = data->Panel->Size.y;
}

static void UI_Panel_SetSize(int entityId, float w, float h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->Size = glm::vec2(w, h);
}

static void UI_Panel_GetTintColor(int entityId, float* r, float* g, float* b, float* a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) { *r = *g = *b = *a = 1.0f; return; }
    *r = data->Panel->TintColor.r;
    *g = data->Panel->TintColor.g;
    *b = data->Panel->TintColor.b;
    *a = data->Panel->TintColor.a;
}

static void UI_Panel_SetTintColor(int entityId, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->TintColor = glm::vec4(r, g, b, a);
}

static bool UI_Panel_GetAnchorEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->AnchorEnabled;
}

static void UI_Panel_SetAnchorEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->AnchorEnabled = enabled;
}

static bool UI_Panel_GetAnchorToParent(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->AnchorToParentUI;
}

static void UI_Panel_SetAnchorToParent(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->AnchorToParentUI = enabled;
}

static int UI_Panel_GetAnchor(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return 0;
    return static_cast<int>(data->Panel->Anchor);
}

static void UI_Panel_SetAnchor(int entityId, int anchor) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->Anchor = static_cast<UIAnchorPreset>(anchor);
}

static void UI_Panel_GetAnchorOffset(int entityId, float* x, float* y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) { *x = *y = 0; return; }
    *x = data->Panel->AnchorOffset.x;
    *y = data->Panel->AnchorOffset.y;
}

static void UI_Panel_SetAnchorOffset(int entityId, float x, float y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->AnchorOffset = glm::vec2(x, y);
}

static int UI_Panel_GetZOrder(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return 0;
    return data->Panel->ZOrder;
}

static void UI_Panel_SetZOrder(int entityId, int z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->ZOrder = z;
}

static bool UI_Panel_IsHovered(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->Hovered;
}

static bool UI_Panel_IsPressed(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->Pressed;
}

static bool UI_Panel_IsDragging(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->Dragging;
}

static bool UI_Panel_DragStarted(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->DragStarted;
}

static bool UI_Panel_DragEnded(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->DragEnded;
}

static bool UI_Panel_WasDropped(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->Dropped;
}

static int UI_Panel_GetDropSource(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return -1;
    return data->Panel->DropSourceEntity;
}

static int UI_Panel_GetDropTarget(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return -1;
    return data->Panel->DropTargetEntity;
}

static bool UI_Panel_GetAllowDrag(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->AllowDrag;
}

static void UI_Panel_SetAllowDrag(int entityId, bool allow) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->AllowDrag = allow;
}

static bool UI_Panel_GetAllowDrop(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->AllowDrop;
}

static void UI_Panel_SetAllowDrop(int entityId, bool allow) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->AllowDrop = allow;
}

static void UI_Panel_SetTexture(int entityId, uint64_t guidHigh, uint64_t guidLow, int fileId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->Texture.guid.high = guidHigh;
    data->Panel->Texture.guid.low = guidLow;
    data->Panel->Texture.fileID = fileId;
    data->Panel->Texture.type = 2; // Texture asset type
    data->Panel->CachedTextureHandle = BGFX_INVALID_HANDLE; // Invalidate cache to reload
}

static void UI_Panel_GetTexture(int entityId, uint64_t* guidHigh, uint64_t* guidLow, int* fileId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel || !guidHigh || !guidLow || !fileId) {
        if (guidHigh) *guidHigh = 0;
        if (guidLow) *guidLow = 0;
        if (fileId) *fileId = 0;
        return;
    }
    *guidHigh = data->Panel->Texture.guid.high;
    *guidLow = data->Panel->Texture.guid.low;
    *fileId = data->Panel->Texture.fileID;
}

static bool UI_Panel_GetDriveChildrenOpacity(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return false;
    return data->Panel->DriveChildrenOpacity;
}

static void UI_Panel_SetDriveChildrenOpacity(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Panel) return;
    data->Panel->DriveChildrenOpacity = enabled;
}

// UI Rect (parent-relative anchoring) functions
static bool UI_Rect_GetAnchorToParent(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return false;
    return data->UIRect->AnchorToParent;
}

static void UI_Rect_SetAnchorToParent(int entityId, bool anchorToParent) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return;
    data->UIRect->AnchorToParent = anchorToParent;
    data->UIRect->_RectDirty = true;
}

static float UI_Rect_GetHorizontalAnchor(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return 0.5f;
    return data->UIRect->HorizontalAnchor;
}

static void UI_Rect_SetHorizontalAnchor(int entityId, float anchor) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return;
    data->UIRect->HorizontalAnchor = anchor;
    data->UIRect->_RectDirty = true;
}

static float UI_Rect_GetVerticalAnchor(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return 0.5f;
    return data->UIRect->VerticalAnchor;
}

static void UI_Rect_SetVerticalAnchor(int entityId, float anchor) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return;
    data->UIRect->VerticalAnchor = anchor;
    data->UIRect->_RectDirty = true;
}

static void UI_Rect_GetPivot(int entityId, float* x, float* y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) { if (x) *x = 0.5f; if (y) *y = 0.5f; return; }
    if (x) *x = data->UIRect->Pivot.x;
    if (y) *y = data->UIRect->Pivot.y;
}

static void UI_Rect_SetPivot(int entityId, float x, float y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return;
    data->UIRect->Pivot = glm::vec2(x, y);
    data->UIRect->_RectDirty = true;
}

static void UI_Rect_GetOffset(int entityId, float* x, float* y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) { if (x) *x = 0.0f; if (y) *y = 0.0f; return; }
    if (x) *x = data->UIRect->Offset.x;
    if (y) *y = data->UIRect->Offset.y;
}

static void UI_Rect_SetOffset(int entityId, float x, float y) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return;
    data->UIRect->Offset = glm::vec2(x, y);
    data->UIRect->_RectDirty = true;
}

static void UI_Rect_GetSize(int entityId, float* w, float* h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) { if (w) *w = 0.0f; if (h) *h = 0.0f; return; }
    if (w) *w = data->UIRect->Size.x;
    if (h) *h = data->UIRect->Size.y;
}

static void UI_Rect_SetSize(int entityId, float w, float h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UIRect) return;
    data->UIRect->Size = glm::vec2(w, h);
    data->UIRect->_RectDirty = true;
}

// UI Canvas functions  
static float UI_Canvas_GetOpacity(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Canvas) return 1.0f;
    return data->Canvas->Opacity;
}

static void UI_Canvas_SetOpacity(int entityId, float opacity) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Canvas) return;
    data->Canvas->Opacity = opacity;
}

static int UI_Canvas_GetRenderSpace(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Canvas) return (int)CanvasComponent::RenderSpace::ScreenSpace;
    return (int)data->Canvas->Space;
}

static void UI_Canvas_SetRenderSpace(int entityId, int renderSpace) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Canvas) return;
    const bool wasWorldSpace = data->Canvas->Space == CanvasComponent::RenderSpace::WorldSpace;
    if (renderSpace <= (int)CanvasComponent::RenderSpace::ScreenSpace) {
        data->Canvas->Space = CanvasComponent::RenderSpace::ScreenSpace;
    } else {
        data->Canvas->Space = CanvasComponent::RenderSpace::WorldSpace;
        if (!wasWorldSpace)
            NormalizeWorldSpaceUIRootLayout(data);
        SeedCanvasComponentDefaults(data);
    }
    if (data->Text) {
        data->Text->WorldSpace = false;
    }
}

static bool UI_Canvas_GetBillboard(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Canvas) return true;
    return data->Canvas->Billboard;
}

static void UI_Canvas_SetBillboard(int entityId, bool billboard) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Canvas) return;
    data->Canvas->Billboard = billboard;
}

// UI LayoutGroup functions
static int UI_LayoutGroup_GetDirection(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return 1; // Vertical default
    return (int)data->LayoutGroup->Direction;
}

static void UI_LayoutGroup_SetDirection(int entityId, int direction) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->Direction = (LayoutGroupComponent::LayoutDirection)direction;
}

static void UI_LayoutGroup_GetPadding(int entityId, float* left, float* top, float* right, float* bottom) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) {
        if (left) *left = 0.0f; if (top) *top = 0.0f;
        if (right) *right = 0.0f; if (bottom) *bottom = 0.0f;
        return;
    }
    if (left) *left = data->LayoutGroup->Padding.x;
    if (top) *top = data->LayoutGroup->Padding.y;
    if (right) *right = data->LayoutGroup->Padding.z;
    if (bottom) *bottom = data->LayoutGroup->Padding.w;
}

static void UI_LayoutGroup_SetPadding(int entityId, float left, float top, float right, float bottom) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->Padding = glm::vec4(left, top, right, bottom);
}

static float UI_LayoutGroup_GetSpacing(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return 5.0f;
    return data->LayoutGroup->Spacing;
}

static void UI_LayoutGroup_SetSpacing(int entityId, float spacing) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->Spacing = spacing;
}

static int UI_LayoutGroup_GetChildAlignment(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return 0; // Start
    return (int)data->LayoutGroup->ChildAlignment;
}

static void UI_LayoutGroup_SetChildAlignment(int entityId, int alignment) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->ChildAlignment = (LayoutGroupComponent::Alignment)alignment;
}

static int UI_LayoutGroup_GetCrossAlignment(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return 0; // Start
    return (int)data->LayoutGroup->CrossAlignment;
}

static void UI_LayoutGroup_SetCrossAlignment(int entityId, int alignment) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->CrossAlignment = (LayoutGroupComponent::Alignment)alignment;
}

static bool UI_LayoutGroup_GetControlChildWidth(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return false;
    return data->LayoutGroup->ControlChildWidth;
}

static void UI_LayoutGroup_SetControlChildWidth(int entityId, bool control) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->ControlChildWidth = control;
}

static bool UI_LayoutGroup_GetControlChildHeight(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return false;
    return data->LayoutGroup->ControlChildHeight;
}

static void UI_LayoutGroup_SetControlChildHeight(int entityId, bool control) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->ControlChildHeight = control;
}

static bool UI_LayoutGroup_GetReverseOrder(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return false;
    return data->LayoutGroup->ReverseOrder;
}

static void UI_LayoutGroup_SetReverseOrder(int entityId, bool reverse) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->LayoutGroup) return;
    data->LayoutGroup->ReverseOrder = reverse;
}

// UI FitToContent functions (14 entries)
static bool UI_FitToContent_GetEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return false;
    return data->FitToContent->Enabled;
}

static void UI_FitToContent_SetEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->Enabled = enabled;
    data->FitToContent->_BoundsDirty = true;
}

static bool UI_FitToContent_GetFitWidth(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return true;
    return data->FitToContent->FitWidth;
}

static void UI_FitToContent_SetFitWidth(int entityId, bool fitWidth) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->FitWidth = fitWidth;
    data->FitToContent->_BoundsDirty = true;
}

static bool UI_FitToContent_GetFitHeight(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return true;
    return data->FitToContent->FitHeight;
}

static void UI_FitToContent_SetFitHeight(int entityId, bool fitHeight) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->FitHeight = fitHeight;
    data->FitToContent->_BoundsDirty = true;
}

static void UI_FitToContent_GetPadding(int entityId, float* left, float* top, float* right, float* bottom) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) {
        if (left) *left = 10.0f; if (top) *top = 10.0f;
        if (right) *right = 10.0f; if (bottom) *bottom = 10.0f;
        return;
    }
    if (left) *left = data->FitToContent->Padding.x;
    if (top) *top = data->FitToContent->Padding.y;
    if (right) *right = data->FitToContent->Padding.z;
    if (bottom) *bottom = data->FitToContent->Padding.w;
}

static void UI_FitToContent_SetPadding(int entityId, float left, float top, float right, float bottom) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->Padding = glm::vec4(left, top, right, bottom);
    data->FitToContent->_BoundsDirty = true;
}

static void UI_FitToContent_GetMinSize(int entityId, float* w, float* h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) { if (w) *w = 0.0f; if (h) *h = 0.0f; return; }
    if (w) *w = data->FitToContent->MinSize.x;
    if (h) *h = data->FitToContent->MinSize.y;
}

static void UI_FitToContent_SetMinSize(int entityId, float w, float h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->MinSize = glm::vec2(w, h);
    data->FitToContent->_BoundsDirty = true;
}

static void UI_FitToContent_GetMaxSize(int entityId, float* w, float* h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) { if (w) *w = 0.0f; if (h) *h = 0.0f; return; }
    if (w) *w = data->FitToContent->MaxSize.x;
    if (h) *h = data->FitToContent->MaxSize.y;
}

static void UI_FitToContent_SetMaxSize(int entityId, float w, float h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->MaxSize = glm::vec2(w, h);
    data->FitToContent->_BoundsDirty = true;
}

static bool UI_FitToContent_GetDirectChildrenOnly(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return true;
    return data->FitToContent->DirectChildrenOnly;
}

static void UI_FitToContent_SetDirectChildrenOnly(int entityId, bool directOnly) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->FitToContent) return;
    data->FitToContent->DirectChildrenOnly = directOnly;
    data->FitToContent->_BoundsDirty = true;
}

// UI Scene Capture functions (28 entries)
static bool UI_SceneCapture_GetEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return false;
    return data->UISceneCapture->Enabled;
}

static void UI_SceneCapture_SetEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->Enabled = enabled;
}

static bool UI_SceneCapture_GetAutoFrame(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return true;
    return data->UISceneCapture->AutoFrame;
}

static void UI_SceneCapture_SetAutoFrame(int entityId, bool autoFrame) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->AutoFrame = autoFrame;
}

static bool UI_SceneCapture_GetIncludeChildren(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return true;
    return data->UISceneCapture->IncludeChildren;
}

static void UI_SceneCapture_SetIncludeChildren(int entityId, bool includeChildren) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->IncludeChildren = includeChildren;
}

static float UI_SceneCapture_GetBoundsPadding(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return 1.15f;
    return data->UISceneCapture->BoundsPadding;
}

static void UI_SceneCapture_SetBoundsPadding(int entityId, float padding) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->BoundsPadding = padding;
}

static float UI_SceneCapture_GetFieldOfView(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return 60.0f;
    return data->UISceneCapture->FieldOfView;
}

static void UI_SceneCapture_SetFieldOfView(int entityId, float fov) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->FieldOfView = fov;
}

static float UI_SceneCapture_GetNearClip(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return 0.1f;
    return data->UISceneCapture->NearClip;
}

static void UI_SceneCapture_SetNearClip(int entityId, float nearClip) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->NearClip = nearClip;
}

static float UI_SceneCapture_GetFarClip(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return 500.0f;
    return data->UISceneCapture->FarClip;
}

static void UI_SceneCapture_SetFarClip(int entityId, float farClip) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->FarClip = farClip;
}

static void UI_SceneCapture_GetViewDirection(int entityId, float* x, float* y, float* z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) { if (x) *x = 0; if (y) *y = 0; if (z) *z = 1; return; }
    if (x) *x = data->UISceneCapture->ViewDirection.x;
    if (y) *y = data->UISceneCapture->ViewDirection.y;
    if (z) *z = data->UISceneCapture->ViewDirection.z;
}

static void UI_SceneCapture_SetViewDirection(int entityId, float x, float y, float z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->ViewDirection = glm::vec3(x, y, z);
}

static void UI_SceneCapture_GetUpDirection(int entityId, float* x, float* y, float* z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) { if (x) *x = 0; if (y) *y = 1; if (z) *z = 0; return; }
    if (x) *x = data->UISceneCapture->UpDirection.x;
    if (y) *y = data->UISceneCapture->UpDirection.y;
    if (z) *z = data->UISceneCapture->UpDirection.z;
}

static void UI_SceneCapture_SetUpDirection(int entityId, float x, float y, float z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->UpDirection = glm::vec3(x, y, z);
}

static void UI_SceneCapture_GetFocusOffset(int entityId, float* x, float* y, float* z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) { if (x) *x = 0; if (y) *y = 0; if (z) *z = 0; return; }
    if (x) *x = data->UISceneCapture->FocusOffset.x;
    if (y) *y = data->UISceneCapture->FocusOffset.y;
    if (z) *z = data->UISceneCapture->FocusOffset.z;
}

static void UI_SceneCapture_SetFocusOffset(int entityId, float x, float y, float z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->FocusOffset = glm::vec3(x, y, z);
}

static int UI_SceneCapture_GetTargetEntity(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return -1;
    return data->UISceneCapture->TargetEntity;
}

static void UI_SceneCapture_SetTargetEntity(int entityId, int targetEntity) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->TargetEntity = targetEntity;
    if (auto* targetData = Scene::Get().GetEntityData(targetEntity)) {
        data->UISceneCapture->TargetGuidHigh = targetData->EntityGuid.high;
        data->UISceneCapture->TargetGuidLow = targetData->EntityGuid.low;
    } else {
        data->UISceneCapture->TargetGuidHigh = 0;
        data->UISceneCapture->TargetGuidLow = 0;
    }
}

static void UI_SceneCapture_GetRenderSize(int entityId, int* w, int* h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) { if (w) *w = 0; if (h) *h = 0; return; }
    if (w) *w = data->UISceneCapture->RenderWidth;
    if (h) *h = data->UISceneCapture->RenderHeight;
}

static void UI_SceneCapture_SetRenderSize(int entityId, int w, int h) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->RenderWidth = w;
    data->UISceneCapture->RenderHeight = h;
}

static void UI_SceneCapture_GetClearColor(int entityId, float* r, float* g, float* b, float* a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) {
        if (r) *r = 0.0f; if (g) *g = 0.0f; if (b) *b = 0.0f; if (a) *a = 0.0f;
        return;
    }
    uint32_t color = data->UISceneCapture->ClearColor;
    if (r) *r = ((color >> 24) & 0xFF) / 255.0f;
    if (g) *g = ((color >> 16) & 0xFF) / 255.0f;
    if (b) *b = ((color >> 8) & 0xFF) / 255.0f;
    if (a) *a = (color & 0xFF) / 255.0f;
}

static void UI_SceneCapture_SetClearColor(int entityId, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
    uint8_t rr = (uint8_t)(clamp01(r) * 255.0f);
    uint8_t gg = (uint8_t)(clamp01(g) * 255.0f);
    uint8_t bb = (uint8_t)(clamp01(b) * 255.0f);
    uint8_t aa = (uint8_t)(clamp01(a) * 255.0f);
    data->UISceneCapture->ClearColor = (uint32_t(rr) << 24) | (uint32_t(gg) << 16) | (uint32_t(bb) << 8) | uint32_t(aa);
}

static bool UI_SceneCapture_GetShowGrid(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return false;
    return data->UISceneCapture->ShowGrid;
}

static void UI_SceneCapture_SetShowGrid(int entityId, bool showGrid) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->ShowGrid = showGrid;
}

static bool UI_SceneCapture_GetLockViewToTarget(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return false;
    return data->UISceneCapture->LockViewToTarget;
}

static void UI_SceneCapture_SetLockViewToTarget(int entityId, bool lockView) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->UISceneCapture) return;
    data->UISceneCapture->LockViewToTarget = lockView;
}

// UI Text/Panel/Canvas function pointers
void (*UI_Text_GetTextPtr)(int, const char**) = &UI_Text_GetText;
void (*UI_Text_SetTextPtr)(int, const char*) = &UI_Text_SetText;
float (*UI_Text_GetOpacityPtr)(int) = &UI_Text_GetOpacity;
void (*UI_Text_SetOpacityPtr)(int, float) = &UI_Text_SetOpacity;
bool (*UI_Text_GetVisiblePtr)(int) = &UI_Text_GetVisible;
void (*UI_Text_SetVisiblePtr)(int, bool) = &UI_Text_SetVisible;
void (*UI_Text_GetColorPtr)(int, float*, float*, float*, float*) = &UI_Text_GetColor;
void (*UI_Text_SetColorPtr)(int, float, float, float, float) = &UI_Text_SetColor;
float (*UI_Text_GetPixelSizePtr)(int) = &UI_Text_GetPixelSize;
void (*UI_Text_SetPixelSizePtr)(int, float) = &UI_Text_SetPixelSize;
int (*UI_Text_GetZOrderPtr)(int) = &UI_Text_GetZOrder;
void (*UI_Text_SetZOrderPtr)(int, int) = &UI_Text_SetZOrder;
void (*UI_Text_GetFontPathPtr)(int, const char**) = &UI_Text_GetFontPath;
void (*UI_Text_SetFontPathPtr)(int, const char*) = &UI_Text_SetFontPath;
bool (*UI_Text_GetAnchorEnabledPtr)(int) = &UI_Text_GetAnchorEnabled;
void (*UI_Text_SetAnchorEnabledPtr)(int, bool) = &UI_Text_SetAnchorEnabled;
int (*UI_Text_GetAnchorPtr)(int) = &UI_Text_GetAnchor;
void (*UI_Text_SetAnchorPtr)(int, int) = &UI_Text_SetAnchor;
void (*UI_Text_GetAnchorOffsetPtr)(int, float*, float*) = &UI_Text_GetAnchorOffset;
void (*UI_Text_SetAnchorOffsetPtr)(int, float, float) = &UI_Text_SetAnchorOffset;
bool (*UI_Text_GetWordWrapPtr)(int) = &UI_Text_GetWordWrap;
void (*UI_Text_SetWordWrapPtr)(int, bool) = &UI_Text_SetWordWrap;
void (*UI_Text_GetRectSizePtr)(int, float*, float*) = &UI_Text_GetRectSize;
void (*UI_Text_SetRectSizePtr)(int, float, float) = &UI_Text_SetRectSize;
bool (*UI_Text_GetWorldSpacePtr)(int) = &UI_Text_GetWorldSpace;
void (*UI_Text_SetWorldSpacePtr)(int, bool) = &UI_Text_SetWorldSpace;
bool (*UI_Text_GetBillboardPtr)(int) = &UI_Text_GetBillboard;
void (*UI_Text_SetBillboardPtr)(int, bool) = &UI_Text_SetBillboard;
bool (*UI_Text_GetOutlineEnabledPtr)(int) = &UI_Text_GetOutlineEnabled;
void (*UI_Text_SetOutlineEnabledPtr)(int, bool) = &UI_Text_SetOutlineEnabled;
void (*UI_Text_GetOutlineColorPtr)(int, float*, float*, float*, float*) = &UI_Text_GetOutlineColor;
void (*UI_Text_SetOutlineColorPtr)(int, float, float, float, float) = &UI_Text_SetOutlineColor;
float (*UI_Text_GetOutlineThicknessPtr)(int) = &UI_Text_GetOutlineThickness;
void (*UI_Text_SetOutlineThicknessPtr)(int, float) = &UI_Text_SetOutlineThickness;
bool (*UI_Text_GetShadowEnabledPtr)(int) = &UI_Text_GetShadowEnabled;
void (*UI_Text_SetShadowEnabledPtr)(int, bool) = &UI_Text_SetShadowEnabled;
void (*UI_Text_GetShadowColorPtr)(int, float*, float*, float*, float*) = &UI_Text_GetShadowColor;
void (*UI_Text_SetShadowColorPtr)(int, float, float, float, float) = &UI_Text_SetShadowColor;
void (*UI_Text_GetShadowOffsetPtr)(int, float*, float*) = &UI_Text_GetShadowOffset;
void (*UI_Text_SetShadowOffsetPtr)(int, float, float) = &UI_Text_SetShadowOffset;
int (*UI_Text_GetAlignmentPtr)(int) = &UI_Text_GetAlignment;
void (*UI_Text_SetAlignmentPtr)(int, int) = &UI_Text_SetAlignment;
float (*UI_Panel_GetOpacityPtr)(int) = &UI_Panel_GetOpacity;
void (*UI_Panel_SetOpacityPtr)(int, float) = &UI_Panel_SetOpacity;
bool (*UI_Panel_GetVisiblePtr)(int) = &UI_Panel_GetVisible;
void (*UI_Panel_SetVisiblePtr)(int, bool) = &UI_Panel_SetVisible;
void (*UI_Panel_GetSizePtr)(int, float*, float*) = &UI_Panel_GetSize;
void (*UI_Panel_SetSizePtr)(int, float, float) = &UI_Panel_SetSize;
void (*UI_Panel_GetTintColorPtr)(int, float*, float*, float*, float*) = &UI_Panel_GetTintColor;
void (*UI_Panel_SetTintColorPtr)(int, float, float, float, float) = &UI_Panel_SetTintColor;
bool (*UI_Panel_GetAnchorEnabledPtr)(int) = &UI_Panel_GetAnchorEnabled;
void (*UI_Panel_SetAnchorEnabledPtr)(int, bool) = &UI_Panel_SetAnchorEnabled;
int (*UI_Panel_GetAnchorPtr)(int) = &UI_Panel_GetAnchor;
void (*UI_Panel_SetAnchorPtr)(int, int) = &UI_Panel_SetAnchor;
void (*UI_Panel_GetAnchorOffsetPtr)(int, float*, float*) = &UI_Panel_GetAnchorOffset;
void (*UI_Panel_SetAnchorOffsetPtr)(int, float, float) = &UI_Panel_SetAnchorOffset;
bool (*UI_Panel_GetAnchorToParentPtr)(int) = &UI_Panel_GetAnchorToParent;
void (*UI_Panel_SetAnchorToParentPtr)(int, bool) = &UI_Panel_SetAnchorToParent;
int (*UI_Panel_GetZOrderPtr)(int) = &UI_Panel_GetZOrder;
void (*UI_Panel_SetZOrderPtr)(int, int) = &UI_Panel_SetZOrder;
UI_Panel_IsHovered_fn UI_Panel_IsHoveredPtr = &UI_Panel_IsHovered;
UI_Panel_IsPressed_fn UI_Panel_IsPressedPtr = &UI_Panel_IsPressed;
UI_Panel_IsDragging_fn UI_Panel_IsDraggingPtr = &UI_Panel_IsDragging;
UI_Panel_DragStarted_fn UI_Panel_DragStartedPtr = &UI_Panel_DragStarted;
UI_Panel_DragEnded_fn UI_Panel_DragEndedPtr = &UI_Panel_DragEnded;
UI_Panel_WasDropped_fn UI_Panel_WasDroppedPtr = &UI_Panel_WasDropped;
UI_Panel_GetDropSource_fn UI_Panel_GetDropSourcePtr = &UI_Panel_GetDropSource;
UI_Panel_GetDropTarget_fn UI_Panel_GetDropTargetPtr = &UI_Panel_GetDropTarget;
UI_Panel_GetAllowDrag_fn UI_Panel_GetAllowDragPtr = &UI_Panel_GetAllowDrag;
UI_Panel_SetAllowDrag_fn UI_Panel_SetAllowDragPtr = &UI_Panel_SetAllowDrag;
UI_Panel_GetAllowDrop_fn UI_Panel_GetAllowDropPtr = &UI_Panel_GetAllowDrop;
UI_Panel_SetAllowDrop_fn UI_Panel_SetAllowDropPtr = &UI_Panel_SetAllowDrop;
void (*UI_Panel_SetTexturePtr)(int, uint64_t, uint64_t, int) = &UI_Panel_SetTexture;
void (*UI_Panel_GetTexturePtr)(int, uint64_t*, uint64_t*, int*) = &UI_Panel_GetTexture;
bool (*UI_Panel_GetDriveChildrenOpacityPtr)(int) = &UI_Panel_GetDriveChildrenOpacity;
void (*UI_Panel_SetDriveChildrenOpacityPtr)(int, bool) = &UI_Panel_SetDriveChildrenOpacity;
bool (*UI_Rect_GetAnchorToParentPtr)(int) = &UI_Rect_GetAnchorToParent;
void (*UI_Rect_SetAnchorToParentPtr)(int, bool) = &UI_Rect_SetAnchorToParent;
float (*UI_Rect_GetHorizontalAnchorPtr)(int) = &UI_Rect_GetHorizontalAnchor;
void (*UI_Rect_SetHorizontalAnchorPtr)(int, float) = &UI_Rect_SetHorizontalAnchor;
float (*UI_Rect_GetVerticalAnchorPtr)(int) = &UI_Rect_GetVerticalAnchor;
void (*UI_Rect_SetVerticalAnchorPtr)(int, float) = &UI_Rect_SetVerticalAnchor;
void (*UI_Rect_GetPivotPtr)(int, float*, float*) = &UI_Rect_GetPivot;
void (*UI_Rect_SetPivotPtr)(int, float, float) = &UI_Rect_SetPivot;
void (*UI_Rect_GetOffsetPtr)(int, float*, float*) = &UI_Rect_GetOffset;
void (*UI_Rect_SetOffsetPtr)(int, float, float) = &UI_Rect_SetOffset;
void (*UI_Rect_GetSizePtr)(int, float*, float*) = &UI_Rect_GetSize;
void (*UI_Rect_SetSizePtr)(int, float, float) = &UI_Rect_SetSize;
float (*UI_Canvas_GetOpacityPtr)(int) = &UI_Canvas_GetOpacity;
void (*UI_Canvas_SetOpacityPtr)(int, float) = &UI_Canvas_SetOpacity;
int (*UI_Canvas_GetRenderSpacePtr)(int) = &UI_Canvas_GetRenderSpace;
void (*UI_Canvas_SetRenderSpacePtr)(int, int) = &UI_Canvas_SetRenderSpace;
bool (*UI_Canvas_GetBillboardPtr)(int) = &UI_Canvas_GetBillboard;
void (*UI_Canvas_SetBillboardPtr)(int, bool) = &UI_Canvas_SetBillboard;
int (*UI_LayoutGroup_GetDirectionPtr)(int) = &UI_LayoutGroup_GetDirection;
void (*UI_LayoutGroup_SetDirectionPtr)(int, int) = &UI_LayoutGroup_SetDirection;
void (*UI_LayoutGroup_GetPaddingPtr)(int, float*, float*, float*, float*) = &UI_LayoutGroup_GetPadding;
void (*UI_LayoutGroup_SetPaddingPtr)(int, float, float, float, float) = &UI_LayoutGroup_SetPadding;
float (*UI_LayoutGroup_GetSpacingPtr)(int) = &UI_LayoutGroup_GetSpacing;
void (*UI_LayoutGroup_SetSpacingPtr)(int, float) = &UI_LayoutGroup_SetSpacing;
int (*UI_LayoutGroup_GetChildAlignmentPtr)(int) = &UI_LayoutGroup_GetChildAlignment;
void (*UI_LayoutGroup_SetChildAlignmentPtr)(int, int) = &UI_LayoutGroup_SetChildAlignment;
int (*UI_LayoutGroup_GetCrossAlignmentPtr)(int) = &UI_LayoutGroup_GetCrossAlignment;
void (*UI_LayoutGroup_SetCrossAlignmentPtr)(int, int) = &UI_LayoutGroup_SetCrossAlignment;
bool (*UI_LayoutGroup_GetControlChildWidthPtr)(int) = &UI_LayoutGroup_GetControlChildWidth;
void (*UI_LayoutGroup_SetControlChildWidthPtr)(int, bool) = &UI_LayoutGroup_SetControlChildWidth;
bool (*UI_LayoutGroup_GetControlChildHeightPtr)(int) = &UI_LayoutGroup_GetControlChildHeight;
void (*UI_LayoutGroup_SetControlChildHeightPtr)(int, bool) = &UI_LayoutGroup_SetControlChildHeight;
bool (*UI_LayoutGroup_GetReverseOrderPtr)(int) = &UI_LayoutGroup_GetReverseOrder;
void (*UI_LayoutGroup_SetReverseOrderPtr)(int, bool) = &UI_LayoutGroup_SetReverseOrder;
bool (*UI_FitToContent_GetEnabledPtr)(int) = &UI_FitToContent_GetEnabled;
void (*UI_FitToContent_SetEnabledPtr)(int, bool) = &UI_FitToContent_SetEnabled;
bool (*UI_FitToContent_GetFitWidthPtr)(int) = &UI_FitToContent_GetFitWidth;
void (*UI_FitToContent_SetFitWidthPtr)(int, bool) = &UI_FitToContent_SetFitWidth;
bool (*UI_FitToContent_GetFitHeightPtr)(int) = &UI_FitToContent_GetFitHeight;
void (*UI_FitToContent_SetFitHeightPtr)(int, bool) = &UI_FitToContent_SetFitHeight;
void (*UI_FitToContent_GetPaddingPtr)(int, float*, float*, float*, float*) = &UI_FitToContent_GetPadding;
void (*UI_FitToContent_SetPaddingPtr)(int, float, float, float, float) = &UI_FitToContent_SetPadding;
void (*UI_FitToContent_GetMinSizePtr)(int, float*, float*) = &UI_FitToContent_GetMinSize;
void (*UI_FitToContent_SetMinSizePtr)(int, float, float) = &UI_FitToContent_SetMinSize;
void (*UI_FitToContent_GetMaxSizePtr)(int, float*, float*) = &UI_FitToContent_GetMaxSize;
void (*UI_FitToContent_SetMaxSizePtr)(int, float, float) = &UI_FitToContent_SetMaxSize;
bool (*UI_FitToContent_GetDirectChildrenOnlyPtr)(int) = &UI_FitToContent_GetDirectChildrenOnly;
void (*UI_FitToContent_SetDirectChildrenOnlyPtr)(int, bool) = &UI_FitToContent_SetDirectChildrenOnly;

bool (*UI_SceneCapture_GetEnabledPtr)(int) = &UI_SceneCapture_GetEnabled;
void (*UI_SceneCapture_SetEnabledPtr)(int, bool) = &UI_SceneCapture_SetEnabled;
bool (*UI_SceneCapture_GetAutoFramePtr)(int) = &UI_SceneCapture_GetAutoFrame;
void (*UI_SceneCapture_SetAutoFramePtr)(int, bool) = &UI_SceneCapture_SetAutoFrame;
bool (*UI_SceneCapture_GetIncludeChildrenPtr)(int) = &UI_SceneCapture_GetIncludeChildren;
void (*UI_SceneCapture_SetIncludeChildrenPtr)(int, bool) = &UI_SceneCapture_SetIncludeChildren;
float (*UI_SceneCapture_GetBoundsPaddingPtr)(int) = &UI_SceneCapture_GetBoundsPadding;
void (*UI_SceneCapture_SetBoundsPaddingPtr)(int, float) = &UI_SceneCapture_SetBoundsPadding;
float (*UI_SceneCapture_GetFieldOfViewPtr)(int) = &UI_SceneCapture_GetFieldOfView;
void (*UI_SceneCapture_SetFieldOfViewPtr)(int, float) = &UI_SceneCapture_SetFieldOfView;
float (*UI_SceneCapture_GetNearClipPtr)(int) = &UI_SceneCapture_GetNearClip;
void (*UI_SceneCapture_SetNearClipPtr)(int, float) = &UI_SceneCapture_SetNearClip;
float (*UI_SceneCapture_GetFarClipPtr)(int) = &UI_SceneCapture_GetFarClip;
void (*UI_SceneCapture_SetFarClipPtr)(int, float) = &UI_SceneCapture_SetFarClip;
void (*UI_SceneCapture_GetViewDirectionPtr)(int, float*, float*, float*) = &UI_SceneCapture_GetViewDirection;
void (*UI_SceneCapture_SetViewDirectionPtr)(int, float, float, float) = &UI_SceneCapture_SetViewDirection;
void (*UI_SceneCapture_GetUpDirectionPtr)(int, float*, float*, float*) = &UI_SceneCapture_GetUpDirection;
void (*UI_SceneCapture_SetUpDirectionPtr)(int, float, float, float) = &UI_SceneCapture_SetUpDirection;
void (*UI_SceneCapture_GetFocusOffsetPtr)(int, float*, float*, float*) = &UI_SceneCapture_GetFocusOffset;
void (*UI_SceneCapture_SetFocusOffsetPtr)(int, float, float, float) = &UI_SceneCapture_SetFocusOffset;
int (*UI_SceneCapture_GetTargetEntityPtr)(int) = &UI_SceneCapture_GetTargetEntity;
void (*UI_SceneCapture_SetTargetEntityPtr)(int, int) = &UI_SceneCapture_SetTargetEntity;
void (*UI_SceneCapture_GetRenderSizePtr)(int, int*, int*) = &UI_SceneCapture_GetRenderSize;
void (*UI_SceneCapture_SetRenderSizePtr)(int, int, int) = &UI_SceneCapture_SetRenderSize;
void (*UI_SceneCapture_GetClearColorPtr)(int, float*, float*, float*, float*) = &UI_SceneCapture_GetClearColor;
void (*UI_SceneCapture_SetClearColorPtr)(int, float, float, float, float) = &UI_SceneCapture_SetClearColor;
bool (*UI_SceneCapture_GetShowGridPtr)(int) = &UI_SceneCapture_GetShowGrid;
void (*UI_SceneCapture_SetShowGridPtr)(int, bool) = &UI_SceneCapture_SetShowGrid;
bool (*UI_SceneCapture_GetLockViewToTargetPtr)(int) = &UI_SceneCapture_GetLockViewToTarget;
void (*UI_SceneCapture_SetLockViewToTargetPtr)(int, bool) = &UI_SceneCapture_SetLockViewToTarget;

// ============================================
// Material Property Block Functions
// ============================================

// Per-mesh property block (applies to all slots)
static void Material_SetVector4(int entityId, const char* propertyName, float x, float y, float z, float w) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    data->Mesh->PropertyBlock.SetVector(propertyName, glm::vec4(x, y, z, w));
}

static void Material_GetVector4(int entityId, const char* propertyName, float* x, float* y, float* z, float* w) {
    if (!x || !y || !z || !w) return;
    *x = *y = *z = *w = 0.0f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    
    glm::vec4 value;
    auto it = data->Mesh->PropertyBlock.Vec4Uniforms.find(propertyName);
    if (it != data->Mesh->PropertyBlock.Vec4Uniforms.end()) {
        value = it->second;
        *x = value.x; *y = value.y; *z = value.z; *w = value.w;
    }
}

static bool Material_HasProperty(int entityId, const char* propertyName) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return false;
    return data->Mesh->PropertyBlock.Vec4Uniforms.find(propertyName) != data->Mesh->PropertyBlock.Vec4Uniforms.end();
}

static void Material_RemoveProperty(int entityId, const char* propertyName) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    data->Mesh->PropertyBlock.RemoveVector(propertyName);
}

static void Material_ClearAll(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh) return;
    data->Mesh->PropertyBlock.Clear();
    data->Mesh->PropertyBlockTexturePaths.clear();
}

static void Material_SetTexturePath(int entityId, const char* propertyName, const char* assetPath) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    
    if (assetPath && strlen(assetPath) > 0) {
        TextureSpecifier spec;
        spec.Path = assetPath;
        bgfx::TextureHandle tex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        if (bgfx::isValid(tex)) {
            data->Mesh->PropertyBlock.SetTexture(propertyName, tex);
            data->Mesh->PropertyBlockTexturePaths[propertyName] = assetPath;
        } else {
            // Failed loads should clear any previous override to avoid leaking unrelated textures.
            data->Mesh->PropertyBlock.RemoveTexture(propertyName);
            data->Mesh->PropertyBlockTexturePaths.erase(propertyName);
        }
    } else {
        // Clear texture
        data->Mesh->PropertyBlock.RemoveTexture(propertyName);
        data->Mesh->PropertyBlockTexturePaths.erase(propertyName);
    }
}

// Per-slot property block
static void Material_SetVector4Slot(int entityId, int slot, const char* propertyName, float x, float y, float z, float w) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    if (slot < 0) return;
    
    // Ensure slot exists
    while ((size_t)slot >= data->Mesh->SlotPropertyBlocks.size()) {
        data->Mesh->SlotPropertyBlocks.push_back({});
        data->Mesh->SlotPropertyBlockTexturePaths.push_back({});
    }
    
    data->Mesh->SlotPropertyBlocks[slot].SetVector(propertyName, glm::vec4(x, y, z, w));
}

static void Material_GetVector4Slot(int entityId, int slot, const char* propertyName, float* x, float* y, float* z, float* w) {
    if (!x || !y || !z || !w) return;
    *x = *y = *z = *w = 0.0f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    if (slot < 0 || (size_t)slot >= data->Mesh->SlotPropertyBlocks.size()) return;
    
    auto it = data->Mesh->SlotPropertyBlocks[slot].Vec4Uniforms.find(propertyName);
    if (it != data->Mesh->SlotPropertyBlocks[slot].Vec4Uniforms.end()) {
        *x = it->second.x; *y = it->second.y; *z = it->second.z; *w = it->second.w;
    }
}

static bool Material_HasPropertySlot(int entityId, int slot, const char* propertyName) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return false;
    if (slot < 0 || (size_t)slot >= data->Mesh->SlotPropertyBlocks.size()) return false;
    return data->Mesh->SlotPropertyBlocks[slot].Vec4Uniforms.find(propertyName) != 
           data->Mesh->SlotPropertyBlocks[slot].Vec4Uniforms.end();
}

static void Material_RemovePropertySlot(int entityId, int slot, const char* propertyName) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    if (slot < 0 || (size_t)slot >= data->Mesh->SlotPropertyBlocks.size()) return;
    data->Mesh->SlotPropertyBlocks[slot].RemoveVector(propertyName);
}

static void Material_ClearSlot(int entityId, int slot) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh) return;
    if (slot < 0 || (size_t)slot >= data->Mesh->SlotPropertyBlocks.size()) return;
    data->Mesh->SlotPropertyBlocks[slot].Clear();
    if ((size_t)slot < data->Mesh->SlotPropertyBlockTexturePaths.size()) {
        data->Mesh->SlotPropertyBlockTexturePaths[slot].clear();
    }
}

static int Material_GetSlotCount(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh) return 0;
    return static_cast<int>(GetEffectiveMaterialSlotCount(*data->Mesh));
}

static void Material_SetTexturePathSlot(int entityId, int slot, const char* propertyName, const char* assetPath) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || !propertyName) return;
    if (slot < 0) return;
    
    // Ensure slot exists
    while ((size_t)slot >= data->Mesh->SlotPropertyBlocks.size()) {
        data->Mesh->SlotPropertyBlocks.push_back({});
        data->Mesh->SlotPropertyBlockTexturePaths.push_back({});
    }
    
    if (assetPath && strlen(assetPath) > 0) {
        TextureSpecifier spec;
        spec.Path = assetPath;
        bgfx::TextureHandle tex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
        if (bgfx::isValid(tex)) {
            data->Mesh->SlotPropertyBlocks[slot].SetTexture(propertyName, tex);
            data->Mesh->SlotPropertyBlockTexturePaths[slot][propertyName] = assetPath;
        } else {
            data->Mesh->SlotPropertyBlocks[slot].RemoveTexture(propertyName);
            if ((size_t)slot < data->Mesh->SlotPropertyBlockTexturePaths.size()) {
                data->Mesh->SlotPropertyBlockTexturePaths[slot].erase(propertyName);
            }
        }
    } else {
        data->Mesh->SlotPropertyBlocks[slot].RemoveTexture(propertyName);
        if ((size_t)slot < data->Mesh->SlotPropertyBlockTexturePaths.size()) {
            data->Mesh->SlotPropertyBlockTexturePaths[slot].erase(propertyName);
        }
    }
}

static int Material_GetMaterialTypeSlot(int entityId, int slot) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return static_cast<int>(ManagedMaterialKind::None);
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= GetEffectiveMaterialSlotCount(*data->Mesh)) {
        return static_cast<int>(ManagedMaterialKind::None);
    }

    auto material = TryGetManagedMaterialSlot(*data, slotIndex);
    return static_cast<int>(GetManagedMaterialKind(material));
}

static const char* Material_GetMaterialNameSlot(int entityId, int slot) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return "";
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= GetEffectiveMaterialSlotCount(*data->Mesh)) {
        return "";
    }

    auto material = TryGetManagedMaterialSlot(*data, slotIndex);
    if (!material) {
        return "";
    }

    std::string& buffer = GetRotatingStringBuffer();
    buffer = material->GetName();
    return buffer.c_str();
}

static const char* Material_GetMaterialAssetPathSlot(int entityId, int slot) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return "";
    const size_t slotIndex = static_cast<size_t>(slot);
    if (slotIndex >= data->Mesh->MaterialAssetPaths.size()) {
        return "";
    }

    std::string& buffer = GetRotatingStringBuffer();
    buffer = data->Mesh->MaterialAssetPaths[slotIndex];
    return buffer.c_str();
}

static bool Material_SetMaterialAssetPathSlot(int entityId, int slot, const char* assetPath) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return false;

    MeshComponent& mesh = *data->Mesh;
    const size_t slotIndex = static_cast<size_t>(slot);
    EnsureMaterialSlotStorage(mesh, slotIndex);

    const std::string normalizedPath = NormalizeManagedAssetPath(assetPath);
    if (normalizedPath.empty()) {
        mesh.MaterialAssetPaths[slotIndex].clear();
        mesh.OwnedMaterialSlots[slotIndex] = false;
        ClearSerializedMaterialSlotCache(mesh, slotIndex);
        SyncUniqueMaterialFlag(mesh);
        SyncPrimaryMaterialReference(mesh);
        return true;
    }

    auto material = LoadManagedMaterialAsset(normalizedPath, MeshNeedsSkinnedMaterial(*data, mesh));
    if (!material) {
        return false;
    }

    mesh.materials[slotIndex] = material;
    mesh.MaterialAssetPaths[slotIndex] = normalizedPath;
    mesh.OwnedMaterialSlots[slotIndex] = false;
    ClearSerializedMaterialSlotCache(mesh, slotIndex);
    SyncUniqueMaterialFlag(mesh);
    SyncPrimaryMaterialReference(mesh);
    return true;
}

static void Material_SetMaterialVector4Slot(int entityId, int slot, const char* propertyName, float x, float y, float z, float w) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0 || !propertyName) return;

    const size_t slotIndex = static_cast<size_t>(slot);
    const glm::vec4 value(x, y, z, w);

    // No-op short circuit: if the slot's current material already holds this
    // exact value, skip the editable-clone + canonicalize round trip. Scripts
    // that re-apply the same value every frame previously cloned a material
    // (fresh bgfx uniform handles) and re-hashed it on every call.
    {
        auto existing = TryGetManagedMaterialSlot(*data, slotIndex);
        if (existing && !ShouldUseManagedSlotPropertyBlockOverride(existing)) {
            glm::vec4 current(0.0f);
            if (TryGetMaterialVector4(existing, propertyName, current) && current == value) {
                return;
            }
        }
    }

    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    if (!material) {
        return;
    }

    SetMaterialVector4Value(material, propertyName, value);
    if (IsCustomManagedMaterial(material)) {
        SetManagedSlotVectorOverride(*data->Mesh, slotIndex, propertyName, value);
    }
    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

static void Material_GetMaterialVector4Slot(int entityId, int slot, const char* propertyName, float* x, float* y, float* z, float* w) {
    if (!x || !y || !z || !w) return;
    *x = *y = *z = *w = 0.0f;

    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0 || !propertyName) return;

    const size_t slotIndex = static_cast<size_t>(slot);
    auto material = TryGetManagedMaterialSlot(*data, slotIndex);

    glm::vec4 value(0.0f);
    if (ShouldUseManagedSlotPropertyBlockOverride(material) &&
        TryGetManagedSlotVectorOverride(*data->Mesh, slotIndex, propertyName, value)) {
        *x = value.x;
        *y = value.y;
        *z = value.z;
        *w = value.w;
        return;
    }

    if (material && TryGetMaterialVector4(material, propertyName, value)) {
        *x = value.x;
        *y = value.y;
        *z = value.z;
        *w = value.w;
    }
}

static bool Material_HasMaterialPropertySlot(int entityId, int slot, const char* propertyName) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0 || !propertyName) return false;

    const size_t slotIndex = static_cast<size_t>(slot);
    auto material = TryGetManagedMaterialSlot(*data, slotIndex);

    if (ShouldUseManagedSlotPropertyBlockOverride(material)) {
        glm::vec4 slotValue(0.0f);
        if (TryGetManagedSlotVectorOverride(*data->Mesh, slotIndex, propertyName, slotValue)) {
            return true;
        }

        std::string slotTexturePath;
        if (TryGetManagedSlotTextureOverridePath(*data->Mesh, slotIndex, propertyName, slotTexturePath)) {
            return true;
        }
    }

    glm::vec4 value(0.0f);
    if (material && TryGetMaterialVector4(material, propertyName, value)) {
        return true;
    }

    std::string texturePath;
    return material && TryGetMaterialTexturePath(material, propertyName, texturePath);
}

static void Material_SetMaterialTexturePathSlot(int entityId, int slot, const char* propertyName, const char* assetPath) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0 || !propertyName) return;

    const size_t slotIndex = static_cast<size_t>(slot);
    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    if (!material) {
        return;
    }

    const std::string normalizedPath = NormalizeManagedAssetPath(assetPath);
    const bool appliedToMaterial = TrySetMaterialTexturePath(material, propertyName, normalizedPath);
    if (IsCustomManagedMaterial(material) || !appliedToMaterial) {
        SetManagedSlotTextureOverride(*data->Mesh, slotIndex, propertyName, normalizedPath);
    }
    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

static const char* Material_GetMaterialTexturePathSlot(int entityId, int slot, const char* propertyName) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0 || !propertyName) return "";

    const size_t slotIndex = static_cast<size_t>(slot);
    auto material = TryGetManagedMaterialSlot(*data, slotIndex);

    std::string texturePath;
    if (ShouldUseManagedSlotPropertyBlockOverride(material) &&
        TryGetManagedSlotTextureOverridePath(*data->Mesh, slotIndex, propertyName, texturePath)) {
        std::string& buffer = GetRotatingStringBuffer();
        buffer = texturePath;
        return buffer.c_str();
    }

    if (!material || !TryGetMaterialTexturePath(material, propertyName, texturePath)) {
        return "";
    }

    std::string& buffer = GetRotatingStringBuffer();
    buffer = texturePath;
    return buffer.c_str();
}

static float Material_GetPbrScalarSlot(int entityId, int slot, int scalarProperty) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return 0.0f;

    auto material = TryGetManagedMaterialSlot(*data, static_cast<size_t>(slot));
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return 0.0f;
    }

    switch (static_cast<ManagedPbrScalar>(scalarProperty)) {
    case ManagedPbrScalar::Metallic:
        return pbr->GetMetallic();
    case ManagedPbrScalar::Roughness:
        return pbr->GetRoughness();
    case ManagedPbrScalar::NormalScale:
        return pbr->GetNormalScale();
    case ManagedPbrScalar::AmbientOcclusion:
        return pbr->GetAmbientOcclusion();
    case ManagedPbrScalar::EmissionStrength:
        return pbr->GetEmissionStrength();
    case ManagedPbrScalar::DisplacementScale:
        return pbr->GetDisplacementScale();
    default:
        return 0.0f;
    }
}

static void Material_SetPbrScalarSlot(int entityId, int slot, int scalarProperty, float value) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    const size_t slotIndex = static_cast<size_t>(slot);

    // No-op short circuit: avoid clone + canonicalize when nothing changes.
    if (Material_GetPbrScalarSlot(entityId, slot, scalarProperty) == value &&
        std::dynamic_pointer_cast<PBRMaterial>(TryGetManagedMaterialSlot(*data, slotIndex))) {
        return;
    }

    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    switch (static_cast<ManagedPbrScalar>(scalarProperty)) {
    case ManagedPbrScalar::Metallic:
        pbr->SetMetallic(value);
        break;
    case ManagedPbrScalar::Roughness:
        pbr->SetRoughness(value);
        break;
    case ManagedPbrScalar::NormalScale:
        pbr->SetNormalScale(value);
        break;
    case ManagedPbrScalar::AmbientOcclusion:
        pbr->SetAmbientOcclusion(value);
        break;
    case ManagedPbrScalar::EmissionStrength:
        pbr->SetEmissionStrength(value);
        break;
    case ManagedPbrScalar::DisplacementScale:
        pbr->SetDisplacementScale(value);
        break;
    default:
        break;
    }

    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

static void Material_GetPbrEmissionColorSlot(int entityId, int slot, float* x, float* y, float* z) {
    if (!x || !y || !z) return;
    *x = *y = *z = 1.0f;

    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    auto material = TryGetManagedMaterialSlot(*data, static_cast<size_t>(slot));
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    const glm::vec3 color = pbr->GetEmissionColor();
    *x = color.x;
    *y = color.y;
    *z = color.z;
}

static void Material_SetPbrEmissionColorSlot(int entityId, int slot, float x, float y, float z) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    const size_t slotIndex = static_cast<size_t>(slot);

    // No-op short circuit: avoid clone + canonicalize when nothing changes.
    if (auto existingPbr = std::dynamic_pointer_cast<PBRMaterial>(
            TryGetManagedMaterialSlot(*data, slotIndex))) {
        if (existingPbr->GetEmissionColor() == glm::vec3(x, y, z)) {
            return;
        }
    }

    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    pbr->SetEmissionColor(glm::vec3(x, y, z));
    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

static void Material_GetPbrUVTransformSlot(int entityId, int slot, float* scaleX, float* scaleY, float* offsetX, float* offsetY) {
    if (!scaleX || !scaleY || !offsetX || !offsetY) return;
    *scaleX = *scaleY = 1.0f;
    *offsetX = *offsetY = 0.0f;

    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    auto material = TryGetManagedMaterialSlot(*data, static_cast<size_t>(slot));
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    const glm::vec2 scale = pbr->GetUVScale();
    const glm::vec2 offset = pbr->GetUVOffset();
    *scaleX = scale.x;
    *scaleY = scale.y;
    *offsetX = offset.x;
    *offsetY = offset.y;
}

static void Material_SetPbrUVTransformSlot(int entityId, int slot, float scaleX, float scaleY, float offsetX, float offsetY) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    const size_t slotIndex = static_cast<size_t>(slot);

    // No-op short circuit: avoid clone + canonicalize when nothing changes.
    if (auto existingPbr = std::dynamic_pointer_cast<PBRMaterial>(
            TryGetManagedMaterialSlot(*data, slotIndex))) {
        if (existingPbr->GetUVScale() == glm::vec2(scaleX, scaleY) &&
            existingPbr->GetUVOffset() == glm::vec2(offsetX, offsetY)) {
            return;
        }
    }

    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    pbr->SetUVTransform(glm::vec2(scaleX, scaleY), glm::vec2(offsetX, offsetY));
    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

static bool Material_GetPbrReceiveShadowsOverrideSlot(int entityId, int slot) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return false;

    auto material = TryGetManagedMaterialSlot(*data, static_cast<size_t>(slot));
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    return pbr ? pbr->GetReceiveShadowsOverride() : false;
}

static void Material_SetPbrReceiveShadowsOverrideSlot(int entityId, int slot, bool value) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    const size_t slotIndex = static_cast<size_t>(slot);

    // No-op short circuit: avoid clone + canonicalize when nothing changes.
    if (auto existingPbr = std::dynamic_pointer_cast<PBRMaterial>(
            TryGetManagedMaterialSlot(*data, slotIndex))) {
        if (existingPbr->GetReceiveShadowsOverride() == value) {
            return;
        }
    }

    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    pbr->SetReceiveShadowsOverride(value);
    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

static bool Material_GetPbrReceiveShadowsSlot(int entityId, int slot) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return false;

    auto material = TryGetManagedMaterialSlot(*data, static_cast<size_t>(slot));
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    return pbr ? pbr->GetReceiveShadows() : false;
}

static void Material_SetPbrReceiveShadowsSlot(int entityId, int slot, bool value) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Mesh || slot < 0) return;

    const size_t slotIndex = static_cast<size_t>(slot);

    // No-op short circuit: avoid clone + canonicalize when nothing changes.
    if (auto existingPbr = std::dynamic_pointer_cast<PBRMaterial>(
            TryGetManagedMaterialSlot(*data, slotIndex))) {
        if (existingPbr->GetReceiveShadows() == value) {
            return;
        }
    }

    auto material = EnsureEditableManagedMaterialSlot(*data, slotIndex);
    auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material);
    if (!pbr) {
        return;
    }

    pbr->SetReceiveShadows(value);
    CanonicalizeManagedMaterialSlot(*data, slotIndex);
}

// Function pointers for material interop
void (*Material_SetVector4Ptr)(int, const char*, float, float, float, float) = &Material_SetVector4;
void (*Material_GetVector4Ptr)(int, const char*, float*, float*, float*, float*) = &Material_GetVector4;
bool (*Material_HasPropertyPtr)(int, const char*) = &Material_HasProperty;
void (*Material_RemovePropertyPtr)(int, const char*) = &Material_RemoveProperty;
void (*Material_ClearAllPtr)(int) = &Material_ClearAll;
void (*Material_SetTexturePathPtr)(int, const char*, const char*) = &Material_SetTexturePath;
void (*Material_SetVector4SlotPtr)(int, int, const char*, float, float, float, float) = &Material_SetVector4Slot;
void (*Material_GetVector4SlotPtr)(int, int, const char*, float*, float*, float*, float*) = &Material_GetVector4Slot;
bool (*Material_HasPropertySlotPtr)(int, int, const char*) = &Material_HasPropertySlot;
void (*Material_RemovePropertySlotPtr)(int, int, const char*) = &Material_RemovePropertySlot;
void (*Material_ClearSlotPtr)(int, int) = &Material_ClearSlot;
int (*Material_GetSlotCountPtr)(int) = &Material_GetSlotCount;
void (*Material_SetTexturePathSlotPtr)(int, int, const char*, const char*) = &Material_SetTexturePathSlot;
int (*Material_GetMaterialTypeSlotPtr)(int, int) = &Material_GetMaterialTypeSlot;
const char* (*Material_GetMaterialNameSlotPtr)(int, int) = &Material_GetMaterialNameSlot;
const char* (*Material_GetMaterialAssetPathSlotPtr)(int, int) = &Material_GetMaterialAssetPathSlot;
bool (*Material_SetMaterialAssetPathSlotPtr)(int, int, const char*) = &Material_SetMaterialAssetPathSlot;
void (*Material_SetMaterialVector4SlotPtr)(int, int, const char*, float, float, float, float) = &Material_SetMaterialVector4Slot;
void (*Material_GetMaterialVector4SlotPtr)(int, int, const char*, float*, float*, float*, float*) = &Material_GetMaterialVector4Slot;
bool (*Material_HasMaterialPropertySlotPtr)(int, int, const char*) = &Material_HasMaterialPropertySlot;
void (*Material_SetMaterialTexturePathSlotPtr)(int, int, const char*, const char*) = &Material_SetMaterialTexturePathSlot;
const char* (*Material_GetMaterialTexturePathSlotPtr)(int, int, const char*) = &Material_GetMaterialTexturePathSlot;
float (*Material_GetPbrScalarSlotPtr)(int, int, int) = &Material_GetPbrScalarSlot;
void (*Material_SetPbrScalarSlotPtr)(int, int, int, float) = &Material_SetPbrScalarSlot;
void (*Material_GetPbrEmissionColorSlotPtr)(int, int, float*, float*, float*) = &Material_GetPbrEmissionColorSlot;
void (*Material_SetPbrEmissionColorSlotPtr)(int, int, float, float, float) = &Material_SetPbrEmissionColorSlot;
void (*Material_GetPbrUVTransformSlotPtr)(int, int, float*, float*, float*, float*) = &Material_GetPbrUVTransformSlot;
void (*Material_SetPbrUVTransformSlotPtr)(int, int, float, float, float, float) = &Material_SetPbrUVTransformSlot;
bool (*Material_GetPbrReceiveShadowsOverrideSlotPtr)(int, int) = &Material_GetPbrReceiveShadowsOverrideSlot;
void (*Material_SetPbrReceiveShadowsOverrideSlotPtr)(int, int, bool) = &Material_SetPbrReceiveShadowsOverrideSlot;
bool (*Material_GetPbrReceiveShadowsSlotPtr)(int, int) = &Material_GetPbrReceiveShadowsSlot;
void (*Material_SetPbrReceiveShadowsSlotPtr)(int, int, bool) = &Material_SetPbrReceiveShadowsSlot;

// ============================================
// Particle Emitter Functions
// ============================================

// Core
static bool Particle_GetEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return false;
    return data->Emitter->Enabled;
}

static void Particle_SetEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Enabled = enabled;
}

static void Particle_Play(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Play();
}

static void Particle_Stop(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Stop();
}

static void Particle_Restart(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Restart();
}

static bool Particle_IsPlaying(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return false;
    return data->Emitter->IsPlaying;
}

// Space & Shape
static int Particle_GetSimulationSpace(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 1; // World
    return static_cast<int>(data->Emitter->SimulationSpace);
}

static void Particle_SetSimulationSpace(int entityId, int space) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->SimulationSpace = static_cast<ParticleSimulationSpace>(space);
}

static int Particle_GetShape(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 3; // Cone
    return static_cast<int>(data->Emitter->Shape);
}

static void Particle_SetShape(int entityId, int shape) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Shape = static_cast<ParticleEmissionShape>(shape);
}

static float Particle_GetShapeRadius(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 1.0f;
    return data->Emitter->ShapeRadius;
}

static void Particle_SetShapeRadius(int entityId, float radius) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->ShapeRadius = radius;
}

static float Particle_GetShapeAngle(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 25.0f;
    return data->Emitter->ShapeAngle;
}

static void Particle_SetShapeAngle(int entityId, float angle) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->ShapeAngle = angle;
}

// Start Values
static void Particle_GetStartSpeed(int entityId, float* min, float* max) {
    if (!min || !max) return;
    *min = 2.0f; *max = 5.0f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    *min = data->Emitter->StartSpeed.Min;
    *max = data->Emitter->StartSpeed.Max;
}

static void Particle_SetStartSpeed(int entityId, float min, float max) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->StartSpeed.Min = min;
    data->Emitter->StartSpeed.Max = max;
}

static void Particle_GetStartSize(int entityId, float* min, float* max) {
    if (!min || !max) return;
    *min = 0.1f; *max = 0.3f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    *min = data->Emitter->StartSize.Min;
    *max = data->Emitter->StartSize.Max;
}

static void Particle_SetStartSize(int entityId, float min, float max) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->StartSize.Min = min;
    data->Emitter->StartSize.Max = max;
}

static void Particle_GetStartColor(int entityId, float* r, float* g, float* b, float* a) {
    if (!r || !g || !b || !a) return;
    *r = *g = *b = *a = 1.0f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    *r = data->Emitter->StartColor.r;
    *g = data->Emitter->StartColor.g;
    *b = data->Emitter->StartColor.b;
    *a = data->Emitter->StartColor.a;
}

static void Particle_SetStartColor(int entityId, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->StartColor = glm::vec4(r, g, b, a);
}

// Emission
static float Particle_GetEmissionRate(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 100.0f;
    return data->Emitter->EmissionRate;
}

static void Particle_SetEmissionRate(int entityId, float rate) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->EmissionRate = rate;
}

static bool Particle_GetLooping(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return true;
    return data->Emitter->Looping;
}

static void Particle_SetLooping(int entityId, bool looping) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Looping = looping;
}

static float Particle_GetDuration(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 5.0f;
    return data->Emitter->Duration;
}

static void Particle_SetDuration(int entityId, float duration) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Duration = duration;
}

static void Particle_GetLifetime(int entityId, float* min, float* max) {
    if (!min || !max) return;
    *min = 3.0f; *max = 5.0f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    *min = data->Emitter->Lifetime.Min;
    *max = data->Emitter->Lifetime.Max;
}

static void Particle_SetLifetime(int entityId, float min, float max) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->Lifetime.Min = min;
    data->Emitter->Lifetime.Max = max;
}

// Physics
static float Particle_GetGravityModifier(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 0.0f;
    return data->Emitter->GravityModifier;
}

static void Particle_SetGravityModifier(int entityId, float gravity) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->GravityModifier = gravity;
}

static int Particle_GetMaxParticles(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 1024;
    return static_cast<int>(data->Emitter->MaxParticles);
}

static void Particle_SetMaxParticles(int entityId, int maxParticles) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->MaxParticles = static_cast<uint32_t>(maxParticles);
}

// Module Enables
static bool Particle_GetSizeOverLifetimeEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return true;
    return data->Emitter->SizeOverLifetimeEnabled;
}

static void Particle_SetSizeOverLifetimeEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->SizeOverLifetimeEnabled = enabled;
}

static bool Particle_GetColorOverLifetimeEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return true;
    return data->Emitter->ColorOverLifetimeEnabled;
}

static void Particle_SetColorOverLifetimeEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->ColorOverLifetimeEnabled = enabled;
}

static bool Particle_GetVelocityOverLifetimeEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return false;
    return data->Emitter->VelocityOverLifetimeEnabled;
}

static void Particle_SetVelocityOverLifetimeEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->VelocityOverLifetimeEnabled = enabled;
}

static bool Particle_GetRotationOverLifetimeEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return false;
    return data->Emitter->RotationOverLifetimeEnabled;
}

static void Particle_SetRotationOverLifetimeEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->RotationOverLifetimeEnabled = enabled;
}

static bool Particle_GetAlignWithTrajectory(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return false;
    return data->Emitter->AlignWithTrajectory;
}

static void Particle_SetAlignWithTrajectory(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->AlignWithTrajectory = enabled;
}

static bool Particle_GetBurstEnabled(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return false;
    return data->Emitter->BurstEnabled;
}

static void Particle_SetBurstEnabled(int entityId, bool enabled) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->BurstEnabled = enabled;
}

// Size Over Lifetime
static void Particle_GetSizeOverLifetime(int entityId, float* start, float* end, int* curveType) {
    if (!start || !end || !curveType) return;
    *start = 1.0f; *end = 0.0f; *curveType = 1; // Linear
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    *start = data->Emitter->SizeOverLifetime.StartValue;
    *end = data->Emitter->SizeOverLifetime.EndValue;
    *curveType = static_cast<int>(data->Emitter->SizeOverLifetime.CurveType);
}

static void Particle_SetSizeOverLifetime(int entityId, float start, float end, int curveType) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->SizeOverLifetime.StartValue = start;
    data->Emitter->SizeOverLifetime.EndValue = end;
    data->Emitter->SizeOverLifetime.CurveType = static_cast<ParticleCurveType>(curveType);
}

// Color Gradient
static int Particle_GetColorGradientKeyCount(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 0;
    return static_cast<int>(data->Emitter->ColorGradient.size());
}

static void Particle_GetColorGradientKey(int entityId, int index, float* time, float* r, float* g, float* b, float* a) {
    if (!time || !r || !g || !b || !a) return;
    *time = 0.0f; *r = *g = *b = *a = 1.0f;
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    if (index < 0 || index >= static_cast<int>(data->Emitter->ColorGradient.size())) return;
    const auto& key = data->Emitter->ColorGradient[index];
    *time = key.Time;
    *r = key.Color.r;
    *g = key.Color.g;
    *b = key.Color.b;
    *a = key.Color.a;
}

static void Particle_SetColorGradientKey(int entityId, int index, float time, float r, float g, float b, float a) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    
    // Expand gradient if needed
    while (index >= static_cast<int>(data->Emitter->ColorGradient.size())) {
        data->Emitter->ColorGradient.push_back({ 1.0f, glm::vec4(1.0f) });
    }
    
    data->Emitter->ColorGradient[index].Time = time;
    data->Emitter->ColorGradient[index].Color = glm::vec4(r, g, b, a);
}

static void Particle_ClearColorGradient(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->ColorGradient.clear();
    // Reset to default gradient
    data->Emitter->ColorGradient.push_back({ 0.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) });
    data->Emitter->ColorGradient.push_back({ 0.1f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) });
    data->Emitter->ColorGradient.push_back({ 0.9f, glm::vec4(1.0f, 1.0f, 1.0f, 1.0f) });
    data->Emitter->ColorGradient.push_back({ 1.0f, glm::vec4(1.0f, 1.0f, 1.0f, 0.0f) });
}

// Burst
static int Particle_GetBurstCount(int entityId) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return 10;
    return data->Emitter->BurstCount;
}

static void Particle_SetBurstCount(int entityId, int count) {
    auto* data = Scene::Get().GetEntityData(entityId);
    if (!data || !data->Emitter) return;
    data->Emitter->BurstCount = count;
}

// Function pointers for particle interop (52 functions total)
bool (*Particle_GetEnabledPtr)(int) = &Particle_GetEnabled;
void (*Particle_SetEnabledPtr)(int, bool) = &Particle_SetEnabled;
void (*Particle_PlayPtr)(int) = &Particle_Play;
void (*Particle_StopPtr)(int) = &Particle_Stop;
void (*Particle_RestartPtr)(int) = &Particle_Restart;
bool (*Particle_IsPlayingPtr)(int) = &Particle_IsPlaying;
int (*Particle_GetSimulationSpacePtr)(int) = &Particle_GetSimulationSpace;
void (*Particle_SetSimulationSpacePtr)(int, int) = &Particle_SetSimulationSpace;
int (*Particle_GetShapePtr)(int) = &Particle_GetShape;
void (*Particle_SetShapePtr)(int, int) = &Particle_SetShape;
float (*Particle_GetShapeRadiusPtr)(int) = &Particle_GetShapeRadius;
void (*Particle_SetShapeRadiusPtr)(int, float) = &Particle_SetShapeRadius;
float (*Particle_GetShapeAnglePtr)(int) = &Particle_GetShapeAngle;
void (*Particle_SetShapeAnglePtr)(int, float) = &Particle_SetShapeAngle;
void (*Particle_GetStartSpeedPtr)(int, float*, float*) = &Particle_GetStartSpeed;
void (*Particle_SetStartSpeedPtr)(int, float, float) = &Particle_SetStartSpeed;
void (*Particle_GetStartSizePtr)(int, float*, float*) = &Particle_GetStartSize;
void (*Particle_SetStartSizePtr)(int, float, float) = &Particle_SetStartSize;
void (*Particle_GetStartColorPtr)(int, float*, float*, float*, float*) = &Particle_GetStartColor;
void (*Particle_SetStartColorPtr)(int, float, float, float, float) = &Particle_SetStartColor;
float (*Particle_GetEmissionRatePtr)(int) = &Particle_GetEmissionRate;
void (*Particle_SetEmissionRatePtr)(int, float) = &Particle_SetEmissionRate;
bool (*Particle_GetLoopingPtr)(int) = &Particle_GetLooping;
void (*Particle_SetLoopingPtr)(int, bool) = &Particle_SetLooping;
float (*Particle_GetDurationPtr)(int) = &Particle_GetDuration;
void (*Particle_SetDurationPtr)(int, float) = &Particle_SetDuration;
void (*Particle_GetLifetimePtr)(int, float*, float*) = &Particle_GetLifetime;
void (*Particle_SetLifetimePtr)(int, float, float) = &Particle_SetLifetime;
float (*Particle_GetGravityModifierPtr)(int) = &Particle_GetGravityModifier;
void (*Particle_SetGravityModifierPtr)(int, float) = &Particle_SetGravityModifier;
int (*Particle_GetMaxParticlesPtr)(int) = &Particle_GetMaxParticles;
void (*Particle_SetMaxParticlesPtr)(int, int) = &Particle_SetMaxParticles;
bool (*Particle_GetSizeOverLifetimeEnabledPtr)(int) = &Particle_GetSizeOverLifetimeEnabled;
void (*Particle_SetSizeOverLifetimeEnabledPtr)(int, bool) = &Particle_SetSizeOverLifetimeEnabled;
bool (*Particle_GetColorOverLifetimeEnabledPtr)(int) = &Particle_GetColorOverLifetimeEnabled;
void (*Particle_SetColorOverLifetimeEnabledPtr)(int, bool) = &Particle_SetColorOverLifetimeEnabled;
bool (*Particle_GetVelocityOverLifetimeEnabledPtr)(int) = &Particle_GetVelocityOverLifetimeEnabled;
void (*Particle_SetVelocityOverLifetimeEnabledPtr)(int, bool) = &Particle_SetVelocityOverLifetimeEnabled;
bool (*Particle_GetRotationOverLifetimeEnabledPtr)(int) = &Particle_GetRotationOverLifetimeEnabled;
void (*Particle_SetRotationOverLifetimeEnabledPtr)(int, bool) = &Particle_SetRotationOverLifetimeEnabled;
bool (*Particle_GetAlignWithTrajectoryPtr)(int) = &Particle_GetAlignWithTrajectory;
void (*Particle_SetAlignWithTrajectoryPtr)(int, bool) = &Particle_SetAlignWithTrajectory;
bool (*Particle_GetBurstEnabledPtr)(int) = &Particle_GetBurstEnabled;
void (*Particle_SetBurstEnabledPtr)(int, bool) = &Particle_SetBurstEnabled;
void (*Particle_GetSizeOverLifetimePtr)(int, float*, float*, int*) = &Particle_GetSizeOverLifetime;
void (*Particle_SetSizeOverLifetimePtr)(int, float, float, int) = &Particle_SetSizeOverLifetime;
int (*Particle_GetColorGradientKeyCountPtr)(int) = &Particle_GetColorGradientKeyCount;
void (*Particle_GetColorGradientKeyPtr)(int, int, float*, float*, float*, float*, float*) = &Particle_GetColorGradientKey;
void (*Particle_SetColorGradientKeyPtr)(int, int, float, float, float, float, float) = &Particle_SetColorGradientKey;
void (*Particle_ClearColorGradientPtr)(int) = &Particle_ClearColorGradient;
int (*Particle_GetBurstCountPtr)(int) = &Particle_GetBurstCount;
void (*Particle_SetBurstCountPtr)(int, int) = &Particle_SetBurstCount;

namespace {
    bool GetTerrainData(int entityID, TerrainComponent*& terrain, TransformComponent*& transform)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Terrain) return false;
        terrain = data->Terrain.get();
        transform = &data->Transform;
        return true;
    }

    bool WorldToLocalXZ(const TransformComponent& transform, float worldX, float worldZ, glm::vec2& outLocalXZ)
    {
        const float originY = transform.WorldMatrix[3].y;
        glm::mat4 inv = glm::inverse(transform.WorldMatrix);
        glm::vec3 local = glm::vec3(inv * glm::vec4(worldX, originY, worldZ, 1.0f));
        outLocalXZ = glm::vec2(local.x, local.z);
        return true;
    }

    bool IsLocalWithinTerrain(const TerrainComponent& terrain, const glm::vec2& localXZ)
    {
        return localXZ.x >= 0.0f && localXZ.y >= 0.0f &&
               localXZ.x <= terrain.WorldSize.x && localXZ.y <= terrain.WorldSize.y;
    }

    glm::vec3 LocalToWorldPoint(const TransformComponent& transform, const glm::vec3& localPos)
    {
        return glm::vec3(transform.WorldMatrix * glm::vec4(localPos, 1.0f));
    }

    glm::vec3 LocalNormalToWorld(const TransformComponent& transform, const glm::vec3& localNormal)
    {
        glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(transform.WorldMatrix)));
        return glm::normalize(normalMat * localNormal);
    }

    enum class RigidBodyCommandKind {
        Force,
        Torque,
        Impulse,
        AngularImpulse
    };

    bool TryApplyRigidBodyCommandNow(RigidBodyComponent& rigidBody,
                                     RigidBodyCommandKind kind,
                                     const glm::vec3& value)
    {
        if (rigidBody.IsKinematic || rigidBody.BodyID.IsInvalid()) {
            return false;
        }

        JPH::PhysicsSystem* system = Physics::GetSystem();
        if (!system) {
            return false;
        }

        {
            JPH::BodyLockRead lock(system->GetBodyLockInterface(), rigidBody.BodyID);
            if (!lock.Succeeded() || lock.GetBody().GetMotionType() != JPH::EMotionType::Dynamic) {
                return false;
            }
        }

        JPH::BodyInterface& bodyInterface = Physics::GetBodyInterface();
        bodyInterface.ActivateBody(rigidBody.BodyID);

        switch (kind) {
        case RigidBodyCommandKind::Force:
            bodyInterface.AddForce(rigidBody.BodyID, JPH::Vec3(value.x, value.y, value.z));
            break;
        case RigidBodyCommandKind::Torque:
            bodyInterface.AddTorque(rigidBody.BodyID, JPH::Vec3(value.x, value.y, value.z));
            break;
        case RigidBodyCommandKind::Impulse: {
            bodyInterface.AddImpulse(rigidBody.BodyID, JPH::Vec3(value.x, value.y, value.z));
            const JPH::Vec3 velocity = bodyInterface.GetLinearVelocity(rigidBody.BodyID);
            rigidBody.LinearVelocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
            break;
        }
        case RigidBodyCommandKind::AngularImpulse: {
            bodyInterface.AddAngularImpulse(rigidBody.BodyID, JPH::Vec3(value.x, value.y, value.z));
            const JPH::Vec3 velocity = bodyInterface.GetAngularVelocity(rigidBody.BodyID);
            rigidBody.AngularVelocity = glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
            break;
        }
        }

        ++rigidBody._DebugImmediatePhysicsCommandCount;
        ++rigidBody._DebugAppliedPhysicsCommandCount;
        return true;
    }
}

extern "C"
{
    // --- Managed Logging ---
    __declspec(dllexport) void ManagedLog(int level, const char* message)
    {
        if (!message) return;
#ifdef CLAYMORE_EDITOR
        switch (level) {
            case 0: Logger::Log(message); break;
            case 1: Logger::LogWarning(message); break;
            case 2: Logger::LogError(message); break;
            default: Logger::Log(message); break;
        }
#else
        // Runtime: use std::cout/cerr
        switch (level) {
            case 0: std::cout << "[LOG] " << message << std::endl; break;
            case 1: std::cout << "[WARNING] " << message << std::endl; break;
            case 2: std::cerr << "[ERROR] " << message << std::endl; break;
            default: std::cout << "[LOG] " << message << std::endl; break;
        }
#endif
    }

    // --- Component Lifetime ---
    __declspec(dllexport) bool HasComponent(int entityID, const char* componentName)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) return false;
        
        if (strcmp(componentName, "LightComponent") == 0) return data->Light != nullptr;
        if (strcmp(componentName, "RigidBodyComponent") == 0) return data->RigidBody != nullptr;
        if (strcmp(componentName, "StaticBodyComponent") == 0 || strcmp(componentName, "StaticBody") == 0) return data->StaticBody != nullptr;
        if (strcmp(componentName, "SoftbodyComponent") == 0) return data->Softbody != nullptr;
        if (strcmp(componentName, "ColliderComponent") == 0) return data->Collider != nullptr;
        if (strcmp(componentName, "CameraComponent") == 0) return data->Camera != nullptr;
        if (strcmp(componentName, "AudioSourceComponent") == 0 || strcmp(componentName, "AudioSource") == 0) return data->AudioSource != nullptr;
        if (strcmp(componentName, "AudioListenerComponent") == 0 || strcmp(componentName, "AudioListener") == 0) return data->AudioListener != nullptr;
        if (strcmp(componentName, "MeshComponent") == 0) return data->Mesh != nullptr;
        if (strcmp(componentName, "MaterialComponent") == 0 || strcmp(componentName, "Material") == 0) return data->Mesh != nullptr;
        if (strcmp(componentName, "SkeletonComponent") == 0) return data->Skeleton != nullptr;
        if (strcmp(componentName, "CharacterControllerComponent") == 0) return data->CharacterController != nullptr;
        if (strcmp(componentName, "AnimationPlayerComponent") == 0) return data->AnimationPlayer != nullptr;
        if (strcmp(componentName, "AreaComponent") == 0 || strcmp(componentName, "Area") == 0) return data->Area != nullptr;
        if (strcmp(componentName, "TextRendererComponent") == 0 || strcmp(componentName, "Text") == 0) return data->Text != nullptr;
        if (strcmp(componentName, "ButtonComponent") == 0 || strcmp(componentName, "Button") == 0) return data->Button != nullptr;
        if (strcmp(componentName, "PanelComponent") == 0 || strcmp(componentName, "Panel") == 0) return data->Panel != nullptr;
        if (strcmp(componentName, "CanvasComponent") == 0 || strcmp(componentName, "Canvas") == 0) return data->Canvas != nullptr;
        if (strcmp(componentName, "SliderComponent") == 0 || strcmp(componentName, "Slider") == 0) return data->Slider != nullptr;
        if (strcmp(componentName, "ProgressBarComponent") == 0 || strcmp(componentName, "ProgressBar") == 0) return data->ProgressBar != nullptr;
        if (strcmp(componentName, "ToggleComponent") == 0 || strcmp(componentName, "Toggle") == 0) return data->Toggle != nullptr;
        if (strcmp(componentName, "ScrollViewComponent") == 0 || strcmp(componentName, "ScrollView") == 0) return data->ScrollView != nullptr;
        if (strcmp(componentName, "InputFieldComponent") == 0 || strcmp(componentName, "InputField") == 0) return data->InputField != nullptr;
        if (strcmp(componentName, "DropdownComponent") == 0 || strcmp(componentName, "Dropdown") == 0) return data->Dropdown != nullptr;
        if (strcmp(componentName, "LayoutGroupComponent") == 0 || strcmp(componentName, "LayoutGroup") == 0) return data->LayoutGroup != nullptr;
        if (strcmp(componentName, "LookAtConstraintComponent") == 0) return !data->LookAtConstraints.empty();
        if (strcmp(componentName, "BoneAttachmentComponent") == 0 || strcmp(componentName, "BoneAttachment") == 0) return data->BoneAttachment != nullptr;
        if (strcmp(componentName, "NavAgentComponent") == 0 || strcmp(componentName, "NavAgent") == 0) return data->NavAgent != nullptr;
        if (strcmp(componentName, "NavMeshComponent") == 0 || strcmp(componentName, "NavMesh") == 0) return data->Navigation != nullptr;
        if (strcmp(componentName, "NavLinkComponent") == 0 || strcmp(componentName, "NavLink") == 0) return data->NavLink != nullptr;
        if (strcmp(componentName, "PortalComponent") == 0 || strcmp(componentName, "Portal") == 0) return data->Portal != nullptr;
        if (strcmp(componentName, "UnifiedMorphComponent") == 0 || strcmp(componentName, "UnifiedMorph") == 0) return data->UnifiedMorph != nullptr;
        if (strcmp(componentName, "BlendShapeComponent") == 0 || strcmp(componentName, "BlendShape") == 0) return data->BlendShapes != nullptr;
        if (strcmp(componentName, "TintMaskController") == 0 || strcmp(componentName, "TintController") == 0) return data->TintController != nullptr;
        if (strcmp(componentName, "UIRectComponent") == 0 || strcmp(componentName, "UIRect") == 0) return data->UIRect != nullptr;
        if (strcmp(componentName, "FitToContentComponent") == 0 || strcmp(componentName, "FitToContent") == 0) return data->FitToContent != nullptr;
        if (strcmp(componentName, "UISceneCaptureComponent") == 0 || strcmp(componentName, "UISceneCapture") == 0) return data->UISceneCapture != nullptr;
        if (strcmp(componentName, "ParticleEmitterComponent") == 0 || strcmp(componentName, "ParticleEmitter") == 0) return data->Emitter != nullptr;
        if (strcmp(componentName, "SplineComponent") == 0 || strcmp(componentName, "Spline") == 0) return data->Spline != nullptr;
        
        return false;
    }

    __declspec(dllexport) void AddComponent(int entityID, const char* componentName)
    {
        auto& scene = Scene::Get();
        auto* data = scene.GetEntityData(entityID);
        if (!data) return;
        const bool hadComponent = HasComponent(entityID, componentName);
        
        if (strcmp(componentName, "LightComponent") == 0 && !data->Light)
            data->Light = std::make_unique<LightComponent>();
        else if (strcmp(componentName, "RigidBodyComponent") == 0 && !data->RigidBody) {
            data->RigidBody = std::make_unique<RigidBodyComponent>();
            // Mirror the editor path: a runtime-added rigidbody should get a
            // sensible default collider so physics can actually create a body.
            const bool createdColliderForRigidBody = !data->Collider;
            EnsureCollider(data->RigidBody.get(), data);
            if (createdColliderForRigidBody && data->Collider) {
                data->Collider->_RuntimeOwnedByRigidBody = true;
            }
            // If entity has a collider, build shape and create the physics body now
            if (data->Collider) {
                // Force recalculate WorldMatrix to ensure we use current position
                // (position may have been set this frame but WorldMatrix not yet updated)
                glm::mat4 worldMat = data->Transform.CalculateLocalMatrix();
                EntityID parentId = data->Parent;
                while (parentId != INVALID_ENTITY_ID) {
                    auto* parentData = Scene::Get().GetEntityData(parentId);
                    if (!parentData) break;
                    worldMat = parentData->Transform.WorldMatrix * worldMat;
                    parentId = parentData->Parent;
                }
                data->Transform.WorldMatrix = worldMat;
                data->Transform.TransformDirty = false;
                Scene::Get().NotifyWorldTransformOverride(entityID);
                
                // Get world scale for shape building
                glm::vec3 wscale(1.0f);
                glm::vec3 wpos, wskew; glm::vec4 wpersp; glm::quat wrot;
                glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
                
                // Build shape if needed
                if (!data->Collider->Shape) {
                    data->Collider->BuildShape(data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr, glm::abs(wscale));
                }
                
                if (data->Collider->Shape) {
                    Scene::Get().CreatePhysicsBody(entityID, data->Transform, *data->Collider);
                }
            }
        }
        else if ((strcmp(componentName, "StaticBodyComponent") == 0 || strcmp(componentName, "StaticBody") == 0) &&
                 !data->StaticBody && !data->RigidBody && !data->Softbody) {
            data->StaticBody = std::make_unique<StaticBodyComponent>();
            EnsureCollider(data->StaticBody.get(), data);

            if (data->Collider) {
                glm::mat4 worldMat = data->Transform.CalculateLocalMatrix();
                EntityID parentId = data->Parent;
                while (parentId != INVALID_ENTITY_ID) {
                    auto* parentData = Scene::Get().GetEntityData(parentId);
                    if (!parentData) break;
                    worldMat = parentData->Transform.WorldMatrix * worldMat;
                    parentId = parentData->Parent;
                }
                data->Transform.WorldMatrix = worldMat;
                data->Transform.TransformDirty = false;
                Scene::Get().NotifyWorldTransformOverride(entityID);

                glm::vec3 wscale(1.0f);
                glm::vec3 wpos, wskew; glm::vec4 wpersp; glm::quat wrot;
                glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);

                if (!data->Collider->Shape) {
                    data->Collider->BuildShape(data->Mesh && data->Mesh->mesh ? data->Mesh->mesh.get() : nullptr, glm::abs(wscale));
                }

                if (data->Collider->Shape) {
                    Scene::Get().CreatePhysicsBody(entityID, data->Transform, *data->Collider);
                }
            }
        }
        else if (strcmp(componentName, "ColliderComponent") == 0 && !data->Collider) {
            data->Collider = std::make_unique<ColliderComponent>();
            // Default to sphere shape
            data->Collider->ShapeType = ColliderShape::Sphere;
            data->Collider->Radius = 0.5f;
            
            // Force recalculate WorldMatrix to ensure we use current position
            glm::mat4 worldMat = data->Transform.CalculateLocalMatrix();
            EntityID parentId = data->Parent;
            while (parentId != INVALID_ENTITY_ID) {
                auto* parentData = Scene::Get().GetEntityData(parentId);
                if (!parentData) break;
                worldMat = parentData->Transform.WorldMatrix * worldMat;
                parentId = parentData->Parent;
            }
            data->Transform.WorldMatrix = worldMat;
            data->Transform.TransformDirty = false;
            Scene::Get().NotifyWorldTransformOverride(entityID);
            
            // Build the shape immediately
            glm::vec3 wscale(1.0f);
            glm::vec3 wpos, wskew; glm::vec4 wpersp; glm::quat wrot;
            glm::decompose(data->Transform.WorldMatrix, wscale, wrot, wpos, wskew, wpersp);
            data->Collider->BuildShape(nullptr, glm::abs(wscale));
            
            // If entity already has a RigidBody, create the physics body now
            if (data->RigidBody && data->Collider->Shape && data->RigidBody->BodyID.IsInvalid()) {
                Scene::Get().CreatePhysicsBody(entityID, data->Transform, *data->Collider);
            }
        }
        else if (strcmp(componentName, "CameraComponent") == 0 && !data->Camera)
            data->Camera = std::make_unique<CameraComponent>();
        else if ((strcmp(componentName, "AudioSourceComponent") == 0 || strcmp(componentName, "AudioSource") == 0) && !data->AudioSource)
            data->AudioSource = std::make_unique<AudioSourceComponent>();
        else if ((strcmp(componentName, "AudioListenerComponent") == 0 || strcmp(componentName, "AudioListener") == 0) && !data->AudioListener)
            data->AudioListener = std::make_unique<AudioListenerComponent>();
        else if (strcmp(componentName, "CharacterControllerComponent") == 0 && !data->CharacterController)
            data->CharacterController = std::make_unique<CharacterControllerComponent>();
        else if (strcmp(componentName, "SoftbodyComponent") == 0 && !data->Softbody)
            data->Softbody = std::make_unique<SoftbodyComponent>();
        else if ((strcmp(componentName, "AreaComponent") == 0 || strcmp(componentName, "Area") == 0) && !data->Area)
            data->Area = std::make_unique<cm::physics::AreaComponent>();
        else if (strcmp(componentName, "MeshComponent") == 0 && !data->Mesh)
            data->Mesh = std::make_unique<MeshComponent>();
        else if ((strcmp(componentName, "BoneAttachmentComponent") == 0 || strcmp(componentName, "BoneAttachment") == 0) && !data->BoneAttachment)
        {
            data->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
            scene.MarkBoneAttachmentCacheDirty();
        }
        else if ((strcmp(componentName, "NavAgentComponent") == 0 || strcmp(componentName, "NavAgent") == 0) && !data->NavAgent)
            data->NavAgent = std::make_unique<nav::NavAgentComponent>();
        else if ((strcmp(componentName, "NavMeshComponent") == 0 || strcmp(componentName, "NavMesh") == 0) && !data->Navigation)
            data->Navigation = std::make_unique<nav::NavMeshComponent>();
        else if ((strcmp(componentName, "NavLinkComponent") == 0 || strcmp(componentName, "NavLink") == 0) && !data->NavLink)
            data->NavLink = std::make_unique<nav::NavLinkComponent>();
        else if ((strcmp(componentName, "PortalComponent") == 0 || strcmp(componentName, "Portal") == 0) && !data->Portal)
            data->Portal = std::make_unique<PortalComponent>();
        else if ((strcmp(componentName, "TextRendererComponent") == 0 || strcmp(componentName, "Text") == 0) && !data->Text)
        {
            data->Text = std::make_unique<TextRendererComponent>();
            if (EntityUsesCanvasBackedUI(data) || HasCanvasAncestor(scene, entityID)) {
                data->Text->WorldSpace = false;
            }
        }
        else if ((strcmp(componentName, "ButtonComponent") == 0 || strcmp(componentName, "Button") == 0) && !data->Button)
            data->Button = std::make_unique<ButtonComponent>();
        else if ((strcmp(componentName, "PanelComponent") == 0 || strcmp(componentName, "Panel") == 0) && !data->Panel)
            data->Panel = std::make_unique<PanelComponent>();
        else if ((strcmp(componentName, "CanvasComponent") == 0 || strcmp(componentName, "Canvas") == 0) && !data->Canvas)
        {
            data->Canvas = std::make_unique<CanvasComponent>();
            if (data->Text) {
                data->Text->WorldSpace = false;
            }
        }
        else if ((strcmp(componentName, "SliderComponent") == 0 || strcmp(componentName, "Slider") == 0) && !data->Slider)
            data->Slider = std::make_unique<SliderComponent>();
        else if ((strcmp(componentName, "ProgressBarComponent") == 0 || strcmp(componentName, "ProgressBar") == 0) && !data->ProgressBar)
            data->ProgressBar = std::make_unique<ProgressBarComponent>();
        else if ((strcmp(componentName, "ToggleComponent") == 0 || strcmp(componentName, "Toggle") == 0) && !data->Toggle)
            data->Toggle = std::make_unique<ToggleComponent>();
        else if ((strcmp(componentName, "UIRectComponent") == 0 || strcmp(componentName, "UIRect") == 0) && !data->UIRect)
            data->UIRect = std::make_unique<UIRectComponent>();
        else if ((strcmp(componentName, "ScrollViewComponent") == 0 || strcmp(componentName, "ScrollView") == 0) && !data->ScrollView)
            data->ScrollView = std::make_unique<ScrollViewComponent>();
        else if ((strcmp(componentName, "InputFieldComponent") == 0 || strcmp(componentName, "InputField") == 0) && !data->InputField)
            data->InputField = std::make_unique<InputFieldComponent>();
        else if ((strcmp(componentName, "DropdownComponent") == 0 || strcmp(componentName, "Dropdown") == 0) && !data->Dropdown)
            data->Dropdown = std::make_unique<DropdownComponent>();
        else if ((strcmp(componentName, "LayoutGroupComponent") == 0 || strcmp(componentName, "LayoutGroup") == 0) && !data->LayoutGroup)
            data->LayoutGroup = std::make_unique<LayoutGroupComponent>();
        else if ((strcmp(componentName, "FitToContentComponent") == 0 || strcmp(componentName, "FitToContent") == 0) && !data->FitToContent)
            data->FitToContent = std::make_unique<FitToContentComponent>();
        else if ((strcmp(componentName, "UISceneCaptureComponent") == 0 || strcmp(componentName, "UISceneCapture") == 0) && !data->UISceneCapture)
            data->UISceneCapture = std::make_unique<UISceneCaptureComponent>();
        else if ((strcmp(componentName, "SplineComponent") == 0 || strcmp(componentName, "Spline") == 0) && !data->Spline)
            data->Spline = std::make_unique<SplineComponent>();

        const bool hasComponentNow = HasComponent(entityID, componentName);
        if (!hadComponent && hasComponentNow) {
            scene.MarkEntityStructureDirty(entityID);
        }
    }

    __declspec(dllexport) void AddScript(int entityID, const char* className)
    {
        auto& scene = Scene::Get();
        auto* data = scene.GetEntityData(entityID);
        if (!data || !className || !*className) {
            return;
        }

        for (const ScriptInstance& script : data->Scripts) {
            if (ScriptClassMatches(script.ClassName, className)) {
                return;
            }
        }

        std::shared_ptr<ScriptComponent> created = ScriptSystem::Instance().Create(className);
        if (!created) {
            std::cerr << "[ComponentInterop] Failed to create script '" << className
                      << "' for entity " << entityID << "\n";
            return;
        }

        ScriptInstance instance;
        instance.ClassName = className;
        instance.Instance = created;
        data->Scripts.push_back(std::move(instance));
        scene.MarkEntityStructureDirty(entityID);

        if (scene.m_IsPlaying) {
            Entity entity(entityID, &scene);
            created->OnBind(entity);
            created->OnCreate(entity);
        }
    }

    __declspec(dllexport) void RemoveComponent(int entityID, const char* componentName)
    {
        auto& scene = Scene::Get();
        auto* data = scene.GetEntityData(entityID);
        if (!data) return;
        const bool hadComponent = HasComponent(entityID, componentName);
        
        if (strcmp(componentName, "LightComponent") == 0) data->Light.reset();
        else if (strcmp(componentName, "RigidBodyComponent") == 0) {
            const bool removeRuntimeOwnedCollider =
                data->Collider &&
                data->Collider->_RuntimeOwnedByRigidBody &&
                !data->StaticBody &&
                !data->CharacterController;

            // CRITICAL: Destroy the physics body before removing the component
            // Otherwise the body stays active in the physics world and causes crashes
            if (data->RigidBody && !data->RigidBody->BodyID.IsInvalid()) {
                try {
                    Physics::Get().DestroyBody(data->RigidBody->BodyID);
                } catch (...) {
                    // Ignore errors during cleanup
                }
                data->RigidBody->BodyID = JPH::BodyID();
            }
            data->RigidBody.reset();

            if (removeRuntimeOwnedCollider) {
                data->Collider.reset();
            }
        }
        else if (strcmp(componentName, "StaticBodyComponent") == 0 || strcmp(componentName, "StaticBody") == 0) {
            if (data->StaticBody && !data->StaticBody->BodyID.IsInvalid()) {
                try {
                    Physics::Get().DestroyBody(data->StaticBody->BodyID);
                } catch (...) {
                    // Ignore errors during cleanup
                }
                data->StaticBody->BodyID = JPH::BodyID();
            }
            data->StaticBody.reset();
        }
        else if (strcmp(componentName, "CameraComponent") == 0) data->Camera.reset();
        else if (strcmp(componentName, "AudioSourceComponent") == 0 || strcmp(componentName, "AudioSource") == 0) data->AudioSource.reset();
        else if (strcmp(componentName, "AudioListenerComponent") == 0 || strcmp(componentName, "AudioListener") == 0) data->AudioListener.reset();
        else if (strcmp(componentName, "CharacterControllerComponent") == 0) data->CharacterController.reset();
        else if (strcmp(componentName, "SoftbodyComponent") == 0) {
            if (data->Softbody) {
                SoftbodySystem::ReleaseRuntime(*data, true);
            }
            data->Softbody.reset();
        }
        else if (strcmp(componentName, "MeshComponent") == 0) data->Mesh.reset();
        else if (strcmp(componentName, "BoneAttachmentComponent") == 0 || strcmp(componentName, "BoneAttachment") == 0) {
            data->BoneAttachment.reset();
            scene.MarkBoneAttachmentCacheDirty();
        }
        else if (strcmp(componentName, "AreaComponent") == 0 || strcmp(componentName, "Area") == 0) {
            if (data->Area) {
                if (auto* areaSystem = Physics::Get().GetAreaSystem()) {
                    try {
                        areaSystem->OnDestroy(Entity(entityID, &Scene::Get()), *data->Area);
                    } catch (...) {
                        // Ignore errors during cleanup
                    }
                }
                data->Area.reset();
            }
        }
        else if (strcmp(componentName, "NavAgentComponent") == 0 || strcmp(componentName, "NavAgent") == 0) data->NavAgent.reset();
        else if (strcmp(componentName, "NavMeshComponent") == 0 || strcmp(componentName, "NavMesh") == 0) data->Navigation.reset();
        else if (strcmp(componentName, "NavLinkComponent") == 0 || strcmp(componentName, "NavLink") == 0) data->NavLink.reset();
        else if (strcmp(componentName, "PortalComponent") == 0 || strcmp(componentName, "Portal") == 0) data->Portal.reset();
        else if (strcmp(componentName, "TextRendererComponent") == 0 || strcmp(componentName, "Text") == 0) data->Text.reset();
        else if (strcmp(componentName, "ButtonComponent") == 0 || strcmp(componentName, "Button") == 0) data->Button.reset();
        else if (strcmp(componentName, "PanelComponent") == 0 || strcmp(componentName, "Panel") == 0) data->Panel.reset();
        else if (strcmp(componentName, "CanvasComponent") == 0 || strcmp(componentName, "Canvas") == 0) data->Canvas.reset();
        else if (strcmp(componentName, "SliderComponent") == 0 || strcmp(componentName, "Slider") == 0) data->Slider.reset();
        else if (strcmp(componentName, "ProgressBarComponent") == 0 || strcmp(componentName, "ProgressBar") == 0) data->ProgressBar.reset();
        else if (strcmp(componentName, "ToggleComponent") == 0 || strcmp(componentName, "Toggle") == 0) data->Toggle.reset();
        else if (strcmp(componentName, "ScrollViewComponent") == 0 || strcmp(componentName, "ScrollView") == 0) data->ScrollView.reset();
        else if (strcmp(componentName, "InputFieldComponent") == 0 || strcmp(componentName, "InputField") == 0) data->InputField.reset();
        else if (strcmp(componentName, "DropdownComponent") == 0 || strcmp(componentName, "Dropdown") == 0) data->Dropdown.reset();
        else if (strcmp(componentName, "LayoutGroupComponent") == 0 || strcmp(componentName, "LayoutGroup") == 0) data->LayoutGroup.reset();
        else if (strcmp(componentName, "UIRectComponent") == 0 || strcmp(componentName, "UIRect") == 0) data->UIRect.reset();
        else if (strcmp(componentName, "FitToContentComponent") == 0 || strcmp(componentName, "FitToContent") == 0) data->FitToContent.reset();
        else if (strcmp(componentName, "UISceneCaptureComponent") == 0 || strcmp(componentName, "UISceneCapture") == 0) data->UISceneCapture.reset();
        else if (strcmp(componentName, "SplineComponent") == 0 || strcmp(componentName, "Spline") == 0) data->Spline.reset();

        const bool hasComponentNow = HasComponent(entityID, componentName);
        if (hadComponent && !hasComponentNow) {
            scene.MarkEntityStructureDirty(entityID);
        }
    }

    // --- LightComponent ---
    __declspec(dllexport) int GetLightType(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Light) return 0;
        return static_cast<int>(data->Light->Type);
    }

    __declspec(dllexport) void SetLightType(int entityID, int type)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Light) return;
        data->Light->Type = static_cast<LightType>(type);
    }

    __declspec(dllexport) void GetLightColor(int entityID, float* r, float* g, float* b)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Light) {
            *r = *g = *b = 1.0f;
            return;
        }
        *r = data->Light->Color.r;
        *g = data->Light->Color.g;
        *b = data->Light->Color.b;
    }

    __declspec(dllexport) void SetLightColor(int entityID, float r, float g, float b)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Light) return;
        data->Light->Color = glm::vec3(r, g, b);
    }

    __declspec(dllexport) float GetLightIntensity(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Light) return 1.0f;
        return data->Light->Intensity;
    }

    __declspec(dllexport) void SetLightIntensity(int entityID, float intensity)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Light) return;
        data->Light->Intensity = intensity;
    }

    // --- RigidBodyComponent ---
    __declspec(dllexport) float GetRigidBodyMass(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return 1.0f;
        return data->RigidBody->Mass;
    }

    __declspec(dllexport) void SetRigidBodyMass(int entityID, float mass)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        const float safeMass = std::max(mass, 1e-6f);
        data->RigidBody->Mass = safeMass;

        if (!data->RigidBody->BodyID.IsInvalid() && !data->RigidBody->IsKinematic) {
            if (JPH::PhysicsSystem* system = Physics::GetSystem()) {
                JPH::BodyLockWrite lock(system->GetBodyLockInterface(), data->RigidBody->BodyID);
                if (lock.Succeeded() && lock.GetBody().GetMotionType() == JPH::EMotionType::Dynamic) {
                    if (JPH::MotionProperties* motionProperties = lock.GetBody().GetMotionPropertiesUnchecked()) {
                        motionProperties->ScaleToMass(safeMass);
                    }
                }
            }
        }
    }

    __declspec(dllexport) bool GetRigidBodyIsKinematic(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return false;
        return data->RigidBody->IsKinematic;
    }

    __declspec(dllexport) void SetRigidBodyIsKinematic(int entityID, bool isKinematic)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        data->RigidBody->IsKinematic = isKinematic;

        if (!data->RigidBody->BodyID.IsInvalid())
        {
            JPH::BodyInterface& bodyInterface = Physics::Get().GetBodyInterface();
            bodyInterface.SetMotionType(
                data->RigidBody->BodyID,
                isKinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic,
                JPH::EActivation::Activate);

            // Kinematic bodies need this flag to generate contact points / callbacks
            // against static and other non-dynamic bodies (e.g. spell projectiles hitting walls).
            if (JPH::PhysicsSystem* system = Physics::GetSystem())
            {
                JPH::BodyLockWrite lock(system->GetBodyLockInterface(), data->RigidBody->BodyID);
                if (lock.Succeeded())
                {
                    lock.GetBody().SetCollideKinematicVsNonDynamic(isKinematic);
                }
            }

            if (isKinematic)
            {
                bodyInterface.SetLinearVelocity(data->RigidBody->BodyID, JPH::Vec3::sZero());
                bodyInterface.SetAngularVelocity(data->RigidBody->BodyID, JPH::Vec3::sZero());
                data->RigidBody->LinearVelocity = glm::vec3(0.0f);
                data->RigidBody->AngularVelocity = glm::vec3(0.0f);
            }

            Scene::Get().SyncPhysicsBodyFromSceneTransform(
                static_cast<EntityID>(entityID),
                isKinematic);
        }
    }

    __declspec(dllexport) bool GetRigidBodyUseGravity(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return true;
        return data->RigidBody->UseGravity;
    }

    __declspec(dllexport) void SetRigidBodyUseGravity(int entityID, bool useGravity)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        data->RigidBody->UseGravity = useGravity;
        const float gravityFactor = useGravity ? 1.0f : 0.0f;
        data->RigidBody->_LastAppliedGravityFactor = gravityFactor;

        // Also update the physics body's gravity factor if it exists
        if (!data->RigidBody->BodyID.IsInvalid()) {
            JPH::BodyInterface& bodyInterface = Physics::GetBodyInterface();
            bodyInterface.SetGravityFactor(data->RigidBody->BodyID, gravityFactor);

            // Jolt intentionally does not wake a body when only its gravity
            // factor changes. Enabling gravity at runtime should make a dynamic
            // sleeping body participate in the next step.
            if (useGravity && !data->RigidBody->IsKinematic) {
                bool isDynamic = true;
                if (JPH::PhysicsSystem* system = Physics::GetSystem()) {
                    JPH::BodyLockRead lock(system->GetBodyLockInterface(), data->RigidBody->BodyID);
                    isDynamic = lock.Succeeded() && lock.GetBody().GetMotionType() == JPH::EMotionType::Dynamic;
                }
                if (isDynamic) {
                    bodyInterface.ActivateBody(data->RigidBody->BodyID);
                }
            }
        }
    }

    __declspec(dllexport) uint32_t GetRigidBodyCollisionMask(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return 0xFFFFFFFFu;
        return data->RigidBody->CollisionMask;
    }

    __declspec(dllexport) void SetRigidBodyCollisionMask(int entityID, uint32_t collisionMask)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        data->RigidBody->CollisionMask = collisionMask;

        if (!data->RigidBody->BodyID.IsInvalid() && Physics::GetSystem()) {
            auto& bodyInterface = Physics::Get().GetBodyInterface();
            bodyInterface.InvalidateContactCache(data->RigidBody->BodyID);
            bodyInterface.ActivateBody(data->RigidBody->BodyID);
        }
    }

    // Sets the physics layer (by name) of an entity's physics body. Updates the
    // collider AND the rigid/static body components together so their inspector
    // layer dropdowns stay matched, and applies the change to the live Jolt body
    // so raycast/spherecast/contact filtering takes effect immediately.
    __declspec(dllexport) bool SetRigidBodyPhysicsLayer(int entityID, const char* layerName)
    {
        if (!layerName) return false;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) return false;

        const int32_t index = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(layerName);
        if (index < 0) {
            std::cerr << "[ComponentInterop] SetRigidBodyPhysicsLayer: unknown physics layer '"
                      << layerName << "'\n";
            return false;
        }
        const uint32_t layerIndex = static_cast<uint32_t>(index);
        const std::string name = layerName;

        bool applied = false;

        if (data->Collider) {
            data->Collider->PhysicsLayer = layerIndex;
            data->Collider->PhysicsLayerName = name;
            applied = true;
        }
        if (data->RigidBody) {
            data->RigidBody->PhysicsLayer = layerIndex;
            data->RigidBody->PhysicsLayerName = name;
            if (!data->RigidBody->BodyID.IsInvalid()) {
                Physics::SetBodyLayer(data->RigidBody->BodyID, layerIndex);
            }
            applied = true;
        }
        if (data->StaticBody) {
            data->StaticBody->PhysicsLayer = layerIndex;
            data->StaticBody->PhysicsLayerName = name;
            if (!data->StaticBody->BodyID.IsInvalid()) {
                Physics::SetBodyLayer(data->StaticBody->BodyID, layerIndex);
            }
            applied = true;
        }
        return applied;
    }

    __declspec(dllexport) void Collider_GetOffset(int entityID, float* x, float* y, float* z)
    {
        if (x) *x = 0.0f;
        if (y) *y = 0.0f;
        if (z) *z = 0.0f;

        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Collider)
        {
            return;
        }

        if (x) *x = data->Collider->Offset.x;
        if (y) *y = data->Collider->Offset.y;
        if (z) *z = data->Collider->Offset.z;
    }

    __declspec(dllexport) void GetRigidBodyLinearVelocity(int entityID, float* x, float* y, float* z)
    {
        *x = *y = *z = 0.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        auto& bodyInterface = Physics::GetBodyInterface();
        if (!data->RigidBody->BodyID.IsInvalid()) {
            JPH::Vec3 vel = bodyInterface.GetLinearVelocity(data->RigidBody->BodyID);
            *x = vel.GetX(); *y = vel.GetY(); *z = vel.GetZ();
        }
    }

    __declspec(dllexport) void SetRigidBodyLinearVelocity(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        // Update component state so kinematic update loop uses the new velocity
        data->RigidBody->LinearVelocity = glm::vec3(x, y, z);
        Physics::SetBodyLinearVelocity(data->RigidBody->BodyID, glm::vec3(x, y, z));
    }

    __declspec(dllexport) void GetRigidBodyAngularVelocity(int entityID, float* x, float* y, float* z)
    {
        *x = *y = *z = 0.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        auto& bodyInterface = Physics::GetBodyInterface();
        if (!data->RigidBody->BodyID.IsInvalid()) {
            JPH::Vec3 vel = bodyInterface.GetAngularVelocity(data->RigidBody->BodyID);
            *x = vel.GetX(); *y = vel.GetY(); *z = vel.GetZ();
        }
    }

    __declspec(dllexport) void SetRigidBodyAngularVelocity(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody) return;
        // Update component state so kinematic update loop uses the new velocity
        data->RigidBody->AngularVelocity = glm::vec3(x, y, z);
        Physics::SetBodyAngularVelocity(data->RigidBody->BodyID, glm::vec3(x, y, z));
    }

    __declspec(dllexport) void ApplyRigidBodyForce(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody || data->RigidBody->IsKinematic) return;

        const glm::vec3 value(x, y, z);
        if (!TryApplyRigidBodyCommandNow(*data->RigidBody, RigidBodyCommandKind::Force, value)) {
            data->RigidBody->PendingForce += value;
            ++data->RigidBody->_DebugQueuedPhysicsCommandCount;
        }
    }

    __declspec(dllexport) void ApplyRigidBodyTorque(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody || data->RigidBody->IsKinematic) return;

        const glm::vec3 value(x, y, z);
        if (!TryApplyRigidBodyCommandNow(*data->RigidBody, RigidBodyCommandKind::Torque, value)) {
            data->RigidBody->PendingTorque += value;
            ++data->RigidBody->_DebugQueuedPhysicsCommandCount;
        }
    }

    __declspec(dllexport) void ApplyRigidBodyImpulse(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody || data->RigidBody->IsKinematic) return;

        const glm::vec3 value(x, y, z);
        if (!TryApplyRigidBodyCommandNow(*data->RigidBody, RigidBodyCommandKind::Impulse, value)) {
            data->RigidBody->PendingImpulse += value;
            ++data->RigidBody->_DebugQueuedPhysicsCommandCount;
        }
    }

    __declspec(dllexport) void ApplyRigidBodyAngularImpulse(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->RigidBody || data->RigidBody->IsKinematic) return;

        const glm::vec3 value(x, y, z);
        if (!TryApplyRigidBodyCommandNow(*data->RigidBody, RigidBodyCommandKind::AngularImpulse, value)) {
            data->RigidBody->PendingAngularImpulse += value;
            ++data->RigidBody->_DebugQueuedPhysicsCommandCount;
        }
    }

    __declspec(dllexport) const char* RigidBody_GetDebugSummary(int entityID)
    {
        std::string& out = GetRotatingStringBuffer();

        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) {
            out = "entity=" + std::to_string(entityID) + " missing";
            return out.c_str();
        }
        if (!data->RigidBody) {
            out = "entity=" + std::to_string(entityID) + " rigidbody=missing";
            return out.c_str();
        }

        auto& rb = *data->RigidBody;
        const bool bodyValid = !rb.BodyID.IsInvalid();
        const bool hasSystem = Physics::GetSystem() != nullptr;

        bool bodyActive = false;
        int motionType = -1;
        float joltGravityFactor = -1.0f;
        float joltInverseMass = -1.0f;
        float joltEffectiveMass = -1.0f;
        glm::vec3 joltInverseInertiaDiagonal(0.0f);
        glm::vec3 joltLinearVelocity(0.0f);
        glm::vec3 joltAngularVelocity(0.0f);

        if (bodyValid && hasSystem) {
            JPH::BodyInterface& bodyInterface = Physics::GetBodyInterface();
            bodyActive = bodyInterface.IsActive(rb.BodyID);
            joltGravityFactor = bodyInterface.GetGravityFactor(rb.BodyID);
            const JPH::Vec3 joltLinear = bodyInterface.GetLinearVelocity(rb.BodyID);
            const JPH::Vec3 joltAngular = bodyInterface.GetAngularVelocity(rb.BodyID);
            joltLinearVelocity = glm::vec3(joltLinear.GetX(), joltLinear.GetY(), joltLinear.GetZ());
            joltAngularVelocity = glm::vec3(joltAngular.GetX(), joltAngular.GetY(), joltAngular.GetZ());

            JPH::BodyLockRead lock(Physics::GetSystem()->GetBodyLockInterface(), rb.BodyID);
            if (lock.Succeeded()) {
                const JPH::Body& body = lock.GetBody();
                switch (body.GetMotionType()) {
                case JPH::EMotionType::Static:
                    motionType = 0;
                    break;
                case JPH::EMotionType::Kinematic:
                    motionType = 1;
                    break;
                case JPH::EMotionType::Dynamic:
                    motionType = 2;
                    break;
                default:
                    motionType = -2;
                    break;
                }

                if (const JPH::MotionProperties* motionProperties = body.GetMotionPropertiesUnchecked()) {
                    joltInverseMass = motionProperties->GetInverseMassUnchecked();
                    if (joltInverseMass > 0.0f) {
                        joltEffectiveMass = 1.0f / joltInverseMass;
                    }
                    const JPH::Vec3 inverseInertiaDiagonal = motionProperties->GetInverseInertiaDiagonal();
                    joltInverseInertiaDiagonal = glm::vec3(
                        inverseInertiaDiagonal.GetX(),
                        inverseInertiaDiagonal.GetY(),
                        inverseInertiaDiagonal.GetZ());
                }
            }
        }

        auto motionTypeName = [](int type) -> const char* {
            switch (type) {
            case 0: return "Static";
            case 1: return "Kinematic";
            case 2: return "Dynamic";
            case -1: return "Unavailable";
            default: return "Unknown";
            }
        };
        auto appendVec3 = [](std::ostringstream& ss, const glm::vec3& v) {
            ss << "(" << v.x << "," << v.y << "," << v.z << ")";
        };

        std::ostringstream ss;
        ss << "entity=" << entityID
           << " hasSystem=" << (hasSystem ? 1 : 0)
           << " hasRigidBody=1"
           << " bodyValid=" << (bodyValid ? 1 : 0)
           << " bodyRaw=" << (bodyValid ? rb.BodyID.GetIndexAndSequenceNumber() : 0u)
           << " bodyIndex=" << (bodyValid ? rb.BodyID.GetIndex() : 0u)
           << " bodySeq=" << (bodyValid ? static_cast<unsigned int>(rb.BodyID.GetSequenceNumber()) : 0u)
           << " active=" << (bodyActive ? 1 : 0)
           << " motion=" << motionTypeName(motionType)
           << " compKinematic=" << (rb.IsKinematic ? 1 : 0)
           << " compUseGravity=" << (rb.UseGravity ? 1 : 0)
           << " collisionMask=0x" << std::hex << rb.CollisionMask << std::dec
           << " gravityLast=" << rb._LastAppliedGravityFactor
           << " gravityJolt=" << joltGravityFactor
           << " mass=" << rb.Mass
           << " joltInvMass=" << joltInverseMass
           << " joltEffectiveMass=" << joltEffectiveMass
           << " hasCollider=" << (data->Collider ? 1 : 0)
           << " colliderRuntimeOwned=" << ((data->Collider && data->Collider->_RuntimeOwnedByRigidBody) ? 1 : 0)
           << " queuedCommands=" << rb._DebugQueuedPhysicsCommandCount
           << " immediateCommands=" << rb._DebugImmediatePhysicsCommandCount
           << " appliedCommands=" << rb._DebugAppliedPhysicsCommandCount;

        ss << " compLin=";
        appendVec3(ss, rb.LinearVelocity);
        ss << " compAng=";
        appendVec3(ss, rb.AngularVelocity);
        ss << " joltLin=";
        appendVec3(ss, joltLinearVelocity);
        ss << " joltAng=";
        appendVec3(ss, joltAngularVelocity);
        ss << " joltInvInertiaDiag=";
        appendVec3(ss, joltInverseInertiaDiagonal);
        ss << " pendingForce=";
        appendVec3(ss, rb.PendingForce);
        ss << " pendingTorque=";
        appendVec3(ss, rb.PendingTorque);
        ss << " pendingImpulse=";
        appendVec3(ss, rb.PendingImpulse);
        ss << " pendingAngularImpulse=";
        appendVec3(ss, rb.PendingAngularImpulse);

        out = ss.str();
        return out.c_str();
    }

    // --- TerrainComponent ---
    __declspec(dllexport) bool Terrain_GetHeightAtWorld(int entityID, float worldX, float worldZ, float* outHeight)
    {
        if (!outHeight) return false;
        *outHeight = 0.0f;

        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;

        glm::vec2 localXZ;
        WorldToLocalXZ(*transform, worldX, worldZ, localXZ);
        if (!IsLocalWithinTerrain(*terrain, localXZ)) return false;
        if (Terrain::IsHoleAtLocal(*terrain, localXZ.x, localXZ.y)) return false;

        const float localHeight = Terrain::SampleHeightWorld(*terrain, localXZ.x, localXZ.y);
        const glm::vec3 worldPoint = LocalToWorldPoint(*transform, glm::vec3(localXZ.x, localHeight, localXZ.y));
        *outHeight = worldPoint.y;
        return true;
    }

    __declspec(dllexport) bool Terrain_GetNormalAtWorld(int entityID, float worldX, float worldZ, float* outX, float* outY, float* outZ)
    {
        if (!outX || !outY || !outZ) return false;
        *outX = 0.0f; *outY = 1.0f; *outZ = 0.0f;

        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;

        glm::vec2 localXZ;
        WorldToLocalXZ(*transform, worldX, worldZ, localXZ);
        if (!IsLocalWithinTerrain(*terrain, localXZ)) return false;
        if (Terrain::IsHoleAtLocal(*terrain, localXZ.x, localXZ.y)) return false;

        const glm::vec3 localNormal = Terrain::SampleNormal(*terrain, localXZ.x, localXZ.y);
        const glm::vec3 worldNormal = LocalNormalToWorld(*transform, localNormal);
        *outX = worldNormal.x; *outY = worldNormal.y; *outZ = worldNormal.z;
        return true;
    }

    __declspec(dllexport) bool Terrain_GetNearestPoint(int entityID, float worldX, float worldZ, float* outX, float* outY, float* outZ)
    {
        if (!outX || !outY || !outZ) return false;
        *outX = *outY = *outZ = 0.0f;

        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;

        glm::vec2 localXZ;
        WorldToLocalXZ(*transform, worldX, worldZ, localXZ);
        if (!IsLocalWithinTerrain(*terrain, localXZ)) return false;
        if (Terrain::IsHoleAtLocal(*terrain, localXZ.x, localXZ.y)) return false;

        const float localHeight = Terrain::SampleHeightWorld(*terrain, localXZ.x, localXZ.y);
        const glm::vec3 worldPoint = LocalToWorldPoint(*transform, glm::vec3(localXZ.x, localHeight, localXZ.y));
        *outX = worldPoint.x; *outY = worldPoint.y; *outZ = worldPoint.z;
        return true;
    }

    __declspec(dllexport) bool Terrain_Raycast(int entityID, float ox, float oy, float oz, float dx, float dy, float dz,
                                               float* outX, float* outY, float* outZ, float* outNx, float* outNy, float* outNz)
    {
        if (!outX || !outY || !outZ || !outNx || !outNy || !outNz) return false;
        *outX = *outY = *outZ = 0.0f;
        *outNx = 0.0f; *outNy = 1.0f; *outNz = 0.0f;

        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;

        glm::vec3 worldPos, worldNormal;
        const bool hit = Terrain::Raycast(*transform, *terrain,
                                          glm::vec3(ox, oy, oz),
                                          glm::vec3(dx, dy, dz),
                                          &worldPos, &worldNormal, nullptr, nullptr);
        if (!hit) return false;
        *outX = worldPos.x; *outY = worldPos.y; *outZ = worldPos.z;
        *outNx = worldNormal.x; *outNy = worldNormal.y; *outNz = worldNormal.z;
        return true;
    }

    __declspec(dllexport) bool Terrain_GetDominantLayerAtWorld(int entityID, float worldX, float worldZ, int* outLayerIndex, float* outWeight)
    {
        if (!outLayerIndex || !outWeight) return false;
        *outLayerIndex = -1;
        *outWeight = 0.0f;

        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;
        if (terrain->SplatMap.empty() || terrain->GridResolution < 2) return false;

        glm::vec2 localXZ;
        WorldToLocalXZ(*transform, worldX, worldZ, localXZ);
        if (!IsLocalWithinTerrain(*terrain, localXZ)) return false;
        if (Terrain::IsHoleAtLocal(*terrain, localXZ.x, localXZ.y)) return false;

        const glm::vec2 cell = Terrain::GetCellSize(*terrain);
        if (cell.x <= 0.0f || cell.y <= 0.0f) return false;

        const float maxCoord = static_cast<float>(terrain->GridResolution - 1);
        const float xf = glm::clamp(localXZ.x / cell.x, 0.0f, maxCoord);
        const float zf = glm::clamp(localXZ.y / cell.y, 0.0f, maxCoord);
        const uint32_t x0 = static_cast<uint32_t>(std::floor(xf));
        const uint32_t z0 = static_cast<uint32_t>(std::floor(zf));
        const uint32_t x1 = std::min(x0 + 1, terrain->GridResolution - 1);
        const uint32_t z1 = std::min(z0 + 1, terrain->GridResolution - 1);
        const float tx = xf - static_cast<float>(x0);
        const float tz = zf - static_cast<float>(z0);

        const size_t mapSize = static_cast<size_t>(terrain->GridResolution) * terrain->GridResolution;
        if (terrain->SplatMap.size() < mapSize) return false;
        const size_t idx00 = static_cast<size_t>(z0) * terrain->GridResolution + x0;
        const size_t idx10 = static_cast<size_t>(z0) * terrain->GridResolution + x1;
        const size_t idx01 = static_cast<size_t>(z1) * terrain->GridResolution + x0;
        const size_t idx11 = static_cast<size_t>(z1) * terrain->GridResolution + x1;

        const glm::vec4 w00 = glm::vec4(terrain->SplatMap[idx00]) / 255.0f;
        const glm::vec4 w10 = glm::vec4(terrain->SplatMap[idx10]) / 255.0f;
        const glm::vec4 w01 = glm::vec4(terrain->SplatMap[idx01]) / 255.0f;
        const glm::vec4 w11 = glm::vec4(terrain->SplatMap[idx11]) / 255.0f;
        const glm::vec4 w0 = glm::mix(w00, w10, tx);
        const glm::vec4 w1 = glm::mix(w01, w11, tx);
        const glm::vec4 w = glm::mix(w0, w1, tz);

        int bestLayer = 0;
        float bestWeight = w.x;
        if (w.y > bestWeight) { bestWeight = w.y; bestLayer = 1; }
        if (w.z > bestWeight) { bestWeight = w.z; bestLayer = 2; }
        if (w.w > bestWeight) { bestWeight = w.w; bestLayer = 3; }

        if (!terrain->SplatMap2.empty() && terrain->SplatMap2.size() >= mapSize)
        {
            const glm::vec4 w2_00 = glm::vec4(terrain->SplatMap2[idx00]) / 255.0f;
            const glm::vec4 w2_10 = glm::vec4(terrain->SplatMap2[idx10]) / 255.0f;
            const glm::vec4 w2_01 = glm::vec4(terrain->SplatMap2[idx01]) / 255.0f;
            const glm::vec4 w2_11 = glm::vec4(terrain->SplatMap2[idx11]) / 255.0f;
            const glm::vec4 w2_0 = glm::mix(w2_00, w2_10, tx);
            const glm::vec4 w2_1 = glm::mix(w2_01, w2_11, tx);
            const glm::vec4 w2 = glm::mix(w2_0, w2_1, tz);

            if (w2.x > bestWeight) { bestWeight = w2.x; bestLayer = 4; }
            if (w2.y > bestWeight) { bestWeight = w2.y; bestLayer = 5; }
            if (w2.z > bestWeight) { bestWeight = w2.z; bestLayer = 6; }
            if (w2.w > bestWeight) { bestWeight = w2.w; bestLayer = 7; }
        }

        *outLayerIndex = bestLayer;
        *outWeight = bestWeight;
        return true;
    }

    __declspec(dllexport) bool Terrain_SetHeightAtWorld(int entityID, float worldX, float worldZ, float worldHeight)
    {
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;

        terrain->EnsureMapSize();
        if (terrain->HeightMap.empty() || terrain->GridResolution < 2) return false;

        glm::vec2 localXZ;
        WorldToLocalXZ(*transform, worldX, worldZ, localXZ);
        if (!IsLocalWithinTerrain(*terrain, localXZ)) return false;

        const glm::vec2 cell = Terrain::GetCellSize(*terrain);
        if (cell.x <= 0.0f || cell.y <= 0.0f || terrain->MaxHeight <= 0.0f) return false;

        const float xf = localXZ.x / cell.x;
        const float zf = localXZ.y / cell.y;
        const uint32_t x = static_cast<uint32_t>(glm::clamp(std::round(xf), 0.0f, (float)(terrain->GridResolution - 1)));
        const uint32_t z = static_cast<uint32_t>(glm::clamp(std::round(zf), 0.0f, (float)(terrain->GridResolution - 1)));
        const size_t idx = static_cast<size_t>(z) * terrain->GridResolution + x;
        if (idx >= terrain->HeightMap.size()) return false;

        float normalized = glm::clamp(worldHeight / terrain->MaxHeight, 0.0f, 1.0f);
        uint16_t newValue = static_cast<uint16_t>(glm::round(normalized * 65535.0f));
        if (terrain->HeightMap[idx] == newValue) return true;

        terrain->HeightMap[idx] = newValue;
        Terrain::EnsureChunkLayout(*terrain);
        Terrain::MarkHeightRegionDirty(*terrain, glm::ivec2((int)x, (int)z), glm::ivec2((int)x, (int)z));
        terrain->PhysicsDirty = true;
        return true;
    }

    __declspec(dllexport) bool Terrain_ApplyHeightDelta(int entityID, float worldX, float worldZ, float radius, float deltaHeight, float falloff)
    {
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;

        terrain->EnsureMapSize();
        if (terrain->HeightMap.empty() || terrain->GridResolution < 2) return false;

        glm::vec2 localXZ;
        WorldToLocalXZ(*transform, worldX, worldZ, localXZ);
        if (!IsLocalWithinTerrain(*terrain, localXZ)) return false;

        const glm::vec2 cell = Terrain::GetCellSize(*terrain);
        if (cell.x <= 0.0f || cell.y <= 0.0f || terrain->MaxHeight <= 0.0f) return false;

        const float radiusClamped = glm::max(radius, 0.0f);
        const int minX = glm::max(0, (int)std::floor((localXZ.x - radiusClamped) / cell.x));
        const int maxX = glm::min((int)terrain->GridResolution - 1, (int)std::ceil((localXZ.x + radiusClamped) / cell.x));
        const int minZ = glm::max(0, (int)std::floor((localXZ.y - radiusClamped) / cell.y));
        const int maxZ = glm::min((int)terrain->GridResolution - 1, (int)std::ceil((localXZ.y + radiusClamped) / cell.y));

        const float heightScale = glm::max(0.0001f, terrain->MaxHeight);
        const float deltaNorm = deltaHeight / heightScale;
        const float falloffPower = glm::max(0.01f, falloff);

        bool changed = false;
        glm::ivec2 minDirty(std::numeric_limits<int>::max());
        glm::ivec2 maxDirty(std::numeric_limits<int>::min());
        const float radiusSq = radiusClamped * radiusClamped;

        for (int z = minZ; z <= maxZ; ++z)
        {
            const float sampleZ = z * cell.y;
            for (int x = minX; x <= maxX; ++x)
            {
                const float sampleX = x * cell.x;
                const float dx = sampleX - localXZ.x;
                const float dz = sampleZ - localXZ.y;
                const float distSq = dx * dx + dz * dz;
                if (radiusClamped > 0.0f && distSq > radiusSq)
                    continue;

                const float dist = (radiusClamped > 0.0f) ? std::sqrt(distSq) : 0.0f;
                float falloffWeight = (radiusClamped > 0.0f) ? (1.0f - (dist / radiusClamped)) : 1.0f;
                falloffWeight = glm::pow(glm::clamp(falloffWeight, 0.0f, 1.0f), falloffPower);

                const size_t idx = static_cast<size_t>(z) * terrain->GridResolution + x;
                if (idx >= terrain->HeightMap.size()) continue;
                float normalized = terrain->HeightMap[idx] / 65535.0f;
                normalized = glm::clamp(normalized + (deltaNorm * falloffWeight), 0.0f, 1.0f);
                const uint16_t newValue = static_cast<uint16_t>(glm::round(normalized * 65535.0f));
                if (terrain->HeightMap[idx] != newValue)
                {
                    terrain->HeightMap[idx] = newValue;
                    changed = true;
                    minDirty.x = glm::min(minDirty.x, x);
                    minDirty.y = glm::min(minDirty.y, z);
                    maxDirty.x = glm::max(maxDirty.x, x);
                    maxDirty.y = glm::max(maxDirty.y, z);
                }
            }
        }

        if (changed)
        {
            Terrain::EnsureChunkLayout(*terrain);
            Terrain::MarkHeightRegionDirty(*terrain, minDirty, maxDirty);
            terrain->PhysicsDirty = true;
        }

        return changed;
    }

    __declspec(dllexport) int Terrain_GetInstancerLayerCount(int entityID)
    {
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return 0;
        return static_cast<int>(terrain->InstancerLayers.size());
    }

    __declspec(dllexport) const char* Terrain_GetInstancerLayerName(int entityID, int layerIndex)
    {
        static thread_local std::string s_name;
        s_name.clear();
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return s_name.c_str();
        if (layerIndex < 0 || layerIndex >= static_cast<int>(terrain->InstancerLayers.size())) return s_name.c_str();
        s_name = terrain->InstancerLayers[static_cast<size_t>(layerIndex)].Name;
        return s_name.c_str();
    }

    __declspec(dllexport) bool Terrain_SetInstancerLayerEnabled(int entityID, int layerIndex, bool enabled)
    {
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;
        if (layerIndex < 0 || layerIndex >= static_cast<int>(terrain->InstancerLayers.size())) return false;
        TerrainInstancerLayerDesc& layer = terrain->InstancerLayers[static_cast<size_t>(layerIndex)];
        if (layer.Enabled != enabled) {
            layer.Enabled = enabled;
            layer.MarkRuntimeDirty();
        }
        return true;
    }

    __declspec(dllexport) bool Terrain_SetInstancerLayerDensity(int entityID, int layerIndex, float density)
    {
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;
        if (layerIndex < 0 || layerIndex >= static_cast<int>(terrain->InstancerLayers.size())) return false;
        TerrainInstancerLayerDesc& layer = terrain->InstancerLayers[static_cast<size_t>(layerIndex)];
        layer.Instancer.Distribution.DensityPerSquareMeter = glm::max(0.0f, density);
        layer.MarkRuntimeDirty();
        return true;
    }

    __declspec(dllexport) bool Terrain_RegenerateInstancers(int entityID)
    {
        TerrainComponent* terrain = nullptr;
        TransformComponent* transform = nullptr;
        if (!GetTerrainData(entityID, terrain, transform)) return false;
        terrain->MarkInstancersDirty();
        return true;
    }

    // --- SplineComponent ---
    __declspec(dllexport) int Spline_GetControlPointCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Spline) return 0;
        return static_cast<int>(data->Spline->ControlPoints.size());
    }

    __declspec(dllexport) bool Spline_GetControlPoint(int entityID, int index, float* outX, float* outY, float* outZ)
    {
        if (!outX || !outY || !outZ) return false;
        *outX = *outY = *outZ = 0.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Spline) return false;
        if (index < 0 || index >= static_cast<int>(data->Spline->ControlPoints.size())) return false;
        const glm::vec3 world = glm::vec3(data->Transform.WorldMatrix * glm::vec4(data->Spline->ControlPoints[static_cast<size_t>(index)].Position, 1.0f));
        *outX = world.x; *outY = world.y; *outZ = world.z;
        return true;
    }

    __declspec(dllexport) int Spline_GetSampledPointCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Spline) return 0;
        auto sampled = cm::spline::SampleSpline(data->Spline->ControlPoints, data->Spline->SplineSubdivision, data->Spline->Closed);
        return static_cast<int>(sampled.size());
    }

    __declspec(dllexport) bool Spline_GetSampledPoint(int entityID, int index, float* outX, float* outY, float* outZ)
    {
        if (!outX || !outY || !outZ) return false;
        *outX = *outY = *outZ = 0.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Spline) return false;
        auto sampled = cm::spline::SampleSpline(data->Spline->ControlPoints, data->Spline->SplineSubdivision, data->Spline->Closed);
        if (index < 0 || index >= static_cast<int>(sampled.size())) return false;
        const glm::vec3 world = glm::vec3(data->Transform.WorldMatrix * glm::vec4(sampled[static_cast<size_t>(index)], 1.0f));
        *outX = world.x; *outY = world.y; *outZ = world.z;
        return true;
    }

    __declspec(dllexport) bool Spline_GetNearestPoint(int entityID, float worldX, float worldY, float worldZ,
                                                      float* outX, float* outY, float* outZ, float* outDistance)
    {
        if (!outX || !outY || !outZ || !outDistance) return false;
        *outX = *outY = *outZ = 0.0f;
        *outDistance = 0.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Spline) return false;
        auto sampled = cm::spline::SampleSpline(data->Spline->ControlPoints, data->Spline->SplineSubdivision, data->Spline->Closed);
        if (sampled.empty()) return false;
        const glm::vec3 query(worldX, worldY, worldZ);
        auto result = cm::spline::GetNearestPoint(sampled, query, data->Transform.WorldMatrix);
        *outX = result.WorldPosition.x;
        *outY = result.WorldPosition.y;
        *outZ = result.WorldPosition.z;
        *outDistance = result.Distance;
        return true;
    }

    __declspec(dllexport) bool Spline_GetPointAtNormalized(int entityID, float t, float* outX, float* outY, float* outZ)
    {
        if (!outX || !outY || !outZ) return false;
        *outX = *outY = *outZ = 0.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Spline) return false;
        glm::vec3 localPos;
        if (!cm::spline::GetPointAtNormalized(data->Spline->ControlPoints, data->Spline->SplineSubdivision,
                data->Spline->Closed, t, localPos))
            return false;
        const glm::vec3 world = glm::vec3(data->Transform.WorldMatrix * glm::vec4(localPos, 1.0f));
        *outX = world.x; *outY = world.y; *outZ = world.z;
        return true;
    }

    // --- PortalComponent ---
    __declspec(dllexport) bool Portal_GetEnabled(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return false;
        return data->Portal->Enabled;
    }

    __declspec(dllexport) void Portal_SetEnabled(int entityID, bool enabled)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->Enabled = enabled;
    }

    __declspec(dllexport) const char* Portal_GetTargetScenePath(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->Portal->TargetScenePath;
        return buf.c_str();
    }

    __declspec(dllexport) void Portal_SetTargetScenePath(int entityID, const char* path)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->TargetScenePath = path ? path : "";
    }

    __declspec(dllexport) const char* Portal_GetTargetPortalGuid(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->Portal->TargetPortalGuid.ToString();
        return buf.c_str();
    }

    __declspec(dllexport) void Portal_SetTargetPortalGuid(int entityID, const char* guid)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        if (!guid || guid[0] == '\0') {
            data->Portal->TargetPortalGuid = ClaymoreGUID();
            return;
        }
        std::string value = guid;
        if (value.size() == 32) {
            data->Portal->TargetPortalGuid = ClaymoreGUID::FromString(value);
        }
    }

    __declspec(dllexport) const char* Portal_GetTargetPortalPath(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->Portal->TargetPortalPath;
        return buf.c_str();
    }

    __declspec(dllexport) void Portal_SetTargetPortalPath(int entityID, const char* path)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->TargetPortalPath = path ? path : "";
    }

    __declspec(dllexport) void Portal_GetEntryOffset(int entityID, float* x, float* y, float* z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) { if (x) *x = 0; if (y) *y = 0; if (z) *z = 0; return; }
        if (x) *x = data->Portal->EntryOffset.x;
        if (y) *y = data->Portal->EntryOffset.y;
        if (z) *z = data->Portal->EntryOffset.z;
    }

    __declspec(dllexport) void Portal_SetEntryOffset(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->EntryOffset = glm::vec3(x, y, z);
    }

    __declspec(dllexport) void Portal_GetExitOffset(int entityID, float* x, float* y, float* z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) { if (x) *x = 0; if (y) *y = 0; if (z) *z = 0; return; }
        if (x) *x = data->Portal->ExitOffset.x;
        if (y) *y = data->Portal->ExitOffset.y;
        if (z) *z = data->Portal->ExitOffset.z;
    }

    __declspec(dllexport) void Portal_SetExitOffset(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->ExitOffset = glm::vec3(x, y, z);
    }

    __declspec(dllexport) bool Portal_GetAutoDetect(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return false;
        return data->Portal->AutoDetect;
    }

    __declspec(dllexport) void Portal_SetAutoDetect(int entityID, bool value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->AutoDetect = value;
    }

    __declspec(dllexport) float Portal_GetTriggerRadius(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return 0.0f;
        return data->Portal->TriggerRadius;
    }

    __declspec(dllexport) void Portal_SetTriggerRadius(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->TriggerRadius = value;
    }

    __declspec(dllexport) bool Portal_GetFireExitEvents(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return false;
        return data->Portal->FireExitEvents;
    }

    __declspec(dllexport) void Portal_SetFireExitEvents(int entityID, bool value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Portal) return;
        data->Portal->FireExitEvents = value;
    }

    // --- Character Controller ---
    __declspec(dllexport) void CC_SetDesiredVelocity(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return;
        data->CharacterController->DesiredVelocity = glm::vec3(x, y, z);
    }
    
    __declspec(dllexport) void CC_GetDesiredVelocity(int entityID, float* x, float* y, float* z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) {
            if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
            return;
        }
        auto& v = data->CharacterController->DesiredVelocity;
        if (x) *x = v.x; if (y) *y = v.y; if (z) *z = v.z;
    }
    
    __declspec(dllexport) void CC_SetVerticalVelocity(int entityID, float v)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return;
        data->CharacterController->VerticalVelocity = v;
    }
    
    __declspec(dllexport) float CC_GetVerticalVelocity(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return 0.0f;
        return data->CharacterController->VerticalVelocity;
    }

    __declspec(dllexport) void CC_Jump(int entityID, float speed)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return;
        data->CharacterController->JumpRequested = true;
        data->CharacterController->JumpSpeed = speed;
    }

    __declspec(dllexport) bool CC_IsGrounded(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return false;
        return data->CharacterController->IsGrounded;
    }

    __declspec(dllexport) void CC_SetPosition(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return;

        auto& cc = *data->CharacterController;

        Scene::Get().SetPosition(entityID, glm::vec3(x, y, z));

        if (cc.Character)
        {
            glm::vec3 up = glm::normalize(cc.Up);
            if (!glm::all(glm::isfinite(up)) || glm::length(up) < 1e-4f)
            {
                up = glm::vec3(0.0f, 1.0f, 0.0f);
            }

            float safeRadius = glm::max(0.05f, cc.Radius);
            float halfHeight = glm::max(0.0f, cc.Height * 0.5f);
            float feetToCenter = halfHeight + safeRadius;

            glm::quat rq = data->Transform.UseQuatRotation ?
                data->Transform.RotationQ : glm::quat(glm::radians(data->Transform.Rotation));

            glm::vec3 offsetWorld = rq * cc.Offset;
            glm::vec3 center = glm::vec3(x, y, z) + offsetWorld + up * feetToCenter;

            cc.Character->SetPosition(JPH::RVec3(center.x, center.y, center.z));
            cc.Character->SetLinearVelocity(JPH::Vec3::sZero());
            cc.VerticalVelocity = 0.0f;
            cc.IsGrounded = false;
        }
    }

    __declspec(dllexport) uint32_t CC_GetCollisionMask(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return 0xFFFFFFFFu;
        return data->CharacterController->CollisionMask;
    }

    __declspec(dllexport) void CC_SetCollisionMask(int entityID, uint32_t collisionMask)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->CharacterController) return;
        // Applied each frame through the CharacterControllerBodyFilter during ExtendedUpdate,
        // so simply updating the field is sufficient.
        data->CharacterController->CollisionMask = collisionMask;
    }

    // --- CameraComponent ---
    __declspec(dllexport) unsigned int GetCameraLayerMask(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return 0xFFFFFFFF;
        return data->Camera->LayerMask;
    }

    __declspec(dllexport) void SetCameraLayerMask(int entityID, unsigned int mask)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->LayerMask = mask;
    }

    __declspec(dllexport) void Camera_SetLayerMaskByName(int entityID, const char* layerName, bool enable)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera || !layerName) return;
        // Layer lookup would need project settings - for now just return
        // TODO: Implement layer name lookup
    }

    // --- CameraComponent Settings ---
    __declspec(dllexport) bool GetCameraActive(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return false;
        return data->Camera->Active;
    }

    __declspec(dllexport) void SetCameraActive(int entityID, bool active)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->Active = active;
    }

    __declspec(dllexport) int GetCameraPriority(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return 0;
        return data->Camera->priority;
    }

    __declspec(dllexport) void SetCameraPriority(int entityID, int priority)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->priority = priority;
    }

    __declspec(dllexport) float GetCameraFieldOfView(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return 60.0f;
        return data->Camera->FieldOfView;
    }

    __declspec(dllexport) void SetCameraFieldOfView(int entityID, float fov)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->FieldOfView = fov;
    }

    __declspec(dllexport) float GetCameraNearClip(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return 0.1f;
        return data->Camera->NearClip;
    }

    __declspec(dllexport) void SetCameraNearClip(int entityID, float nearClip)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->NearClip = nearClip;
    }

    __declspec(dllexport) float GetCameraFarClip(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return 1000.0f;
        return data->Camera->FarClip;
    }

    __declspec(dllexport) void SetCameraFarClip(int entityID, float farClip)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->FarClip = farClip;
    }

    __declspec(dllexport) bool GetCameraIsPerspective(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return true;
        return data->Camera->IsPerspective;
    }

    __declspec(dllexport) void SetCameraIsPerspective(int entityID, bool isPerspective)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Camera) return;
        data->Camera->IsPerspective = isPerspective;
    }

    // --- BlendShapeComponent ---
    __declspec(dllexport) void SetBlendShapeWeight(int entityID, const char* shapeName, float weight)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BlendShapes || !shapeName) return;
        
        for (auto& shape : data->BlendShapes->Shapes) {
            if (shape.Name == shapeName) {
                if (shape.Weight != weight) {
                    shape.Weight = weight;
                    data->BlendShapes->Dirty = true;
                }
                return;
            }
        }
    }

    __declspec(dllexport) float GetBlendShapeWeight(int entityID, const char* shapeName)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BlendShapes || !shapeName) return 0.0f;
        
        for (const auto& shape : data->BlendShapes->Shapes) {
            if (shape.Name == shapeName)
                return shape.Weight;
        }
        return 0.0f;
    }

    __declspec(dllexport) int GetBlendShapeCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BlendShapes) return 0;
        return static_cast<int>(data->BlendShapes->Shapes.size());
    }

    __declspec(dllexport) const char* GetBlendShapeName(int entityID, int index)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BlendShapes) return "";
        if (index < 0 || index >= static_cast<int>(data->BlendShapes->Shapes.size())) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->BlendShapes->Shapes[index].Name;
        return buf.c_str();
    }

    // --- UnifiedMorphComponent ---
    __declspec(dllexport) int UnifiedMorph_GetCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->UnifiedMorph) return 0;
        return static_cast<int>(data->UnifiedMorph->Names.size());
    }

    __declspec(dllexport) const char* UnifiedMorph_GetName(int entityID, int index)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->UnifiedMorph) return "";
        if (index < 0 || index >= static_cast<int>(data->UnifiedMorph->Names.size())) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->UnifiedMorph->Names[index];
        return buf.c_str();
    }

    __declspec(dllexport) float UnifiedMorph_GetWeight(int entityID, int index)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->UnifiedMorph) return 0.0f;
        if (index < 0 || index >= static_cast<int>(data->UnifiedMorph->Weights.size())) return 0.0f;
        return data->UnifiedMorph->Weights[index];
    }

    __declspec(dllexport) void UnifiedMorph_SetWeight(int entityID, int index, float weight)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->UnifiedMorph) return;
        if (index < 0 || index >= static_cast<int>(data->UnifiedMorph->Weights.size())) return;
        
        // Skip if weight unchanged
        if (data->UnifiedMorph->Weights[index] == weight) return;
        data->UnifiedMorph->Weights[index] = weight;
        
        // Propagate to all child mesh BlendShapeComponents
        const std::string& morphName = data->UnifiedMorph->Names[index];
        const uint64_t morphHash = HashBlendShapeName(morphName);
        for (EntityID meshId : data->UnifiedMorph->MemberMeshes) {
            auto* meshData = Scene::Get().GetEntityData(meshId);
            if (!meshData || !meshData->BlendShapes) continue;
            for (auto& shape : meshData->BlendShapes->Shapes) {
                if (shape.NameHash == 0 && !shape.Name.empty()) {
                    shape.UpdateNameHash();
                }
                if (shape.NameHash == morphHash) {
                    shape.Weight = weight;
                    meshData->BlendShapes->Dirty = true;
                    break; // Only one shape per name per mesh
                }
            }
        }
    }
    
    __declspec(dllexport) void UnifiedMorph_PropagateAll(int entityID)
    {
        // Batch propagation using parallel-for (for character creator bulk updates)
        Scene::Get().PropagateUnifiedMorphWeights(entityID);
    }

    // --- TintMaskController ---
    __declspec(dllexport) bool TintController_HasComponent(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        return data && data->TintController != nullptr;
    }
    
    __declspec(dllexport) const char* TintController_GetNamePattern(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->TintController->NamePattern;
        return buf.c_str();
    }
    
    __declspec(dllexport) void TintController_SetNamePattern(int entityID, const char* pattern)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController || !pattern) return;
        data->TintController->NamePattern = pattern;
        data->TintController->NeedsRefresh = true;
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) void TintController_GetBaseTint(int entityID, float* r, float* g, float* b, float* a)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) {
            if (r) *r = 1.0f; if (g) *g = 1.0f; if (b) *b = 1.0f; if (a) *a = 1.0f;
            return;
        }
        const auto& t = data->TintController->BaseTint;
        if (r) *r = t.r; if (g) *g = t.g; if (b) *b = t.b; if (a) *a = t.a;
    }
    
    __declspec(dllexport) void TintController_SetBaseTint(int entityID, float r, float g, float b, float a)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->BaseTint = glm::vec4(r, g, b, a);
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) void TintController_GetTintColor(int entityID, int channel, float* r, float* g, float* b, float* a)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController || channel < 0 || channel > 3) {
            if (r) *r = 1.0f; if (g) *g = 1.0f; if (b) *b = 1.0f; if (a) *a = 1.0f;
            return;
        }
        glm::vec4 color;
        switch (channel) {
            case 0: color = data->TintController->TintColor0; break;
            case 1: color = data->TintController->TintColor1; break;
            case 2: color = data->TintController->TintColor2; break;
            case 3: color = data->TintController->TintColor3; break;
            default: color = glm::vec4(1.0f); break;
        }
        if (r) *r = color.r; if (g) *g = color.g; if (b) *b = color.b; if (a) *a = color.a;
    }
    
    __declspec(dllexport) void TintController_SetTintColor(int entityID, int channel, float r, float g, float b, float a)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController || channel < 0 || channel > 3) return;
        glm::vec4 color(r, g, b, a);
        switch (channel) {
            case 0: data->TintController->TintColor0 = color; break;
            case 1: data->TintController->TintColor1 = color; break;
            case 2: data->TintController->TintColor2 = color; break;
            case 3: data->TintController->TintColor3 = color; break;
        }
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) bool TintController_GetUseTintMask(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return false;
        return data->TintController->UseTintMask;
    }
    
    __declspec(dllexport) void TintController_SetUseTintMask(int entityID, bool use)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->UseTintMask = use;
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) bool TintController_GetUsePbrOverrides(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return false;
        return data->TintController->UsePbrOverrides;
    }
    
    __declspec(dllexport) void TintController_SetUsePbrOverrides(int entityID, bool use)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->UsePbrOverrides = use;
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) float TintController_GetPbrMetallic(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return 0.0f;
        return data->TintController->OverrideMetallic;
    }
    
    __declspec(dllexport) void TintController_SetPbrMetallic(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->OverrideMetallic = value;
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) float TintController_GetPbrRoughness(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return 0.5f;
        return data->TintController->OverrideRoughness;
    }
    
    __declspec(dllexport) void TintController_SetPbrRoughness(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->OverrideRoughness = value;
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) void TintController_GetPbrEmissionColor(int entityID, float* r, float* g, float* b)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) {
            if (r) *r = 1.0f; if (g) *g = 1.0f; if (b) *b = 1.0f;
            return;
        }
        const auto& c = data->TintController->OverrideEmissionColor;
        if (r) *r = c.r; if (g) *g = c.g; if (b) *b = c.b;
    }
    
    __declspec(dllexport) void TintController_SetPbrEmissionColor(int entityID, float r, float g, float b)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->OverrideEmissionColor = glm::vec3(r, g, b);
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) float TintController_GetPbrEmissionStrength(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return 0.0f;
        return data->TintController->OverrideEmissionStrength;
    }
    
    __declspec(dllexport) void TintController_SetPbrEmissionStrength(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->OverrideEmissionStrength = value;
        data->TintController->MarkDirty();
    }

    __declspec(dllexport) int TintController_GetGlobalBlendMode(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return 0;
        return static_cast<int>(data->TintController->GlobalBlendMode);
    }

    __declspec(dllexport) void TintController_SetGlobalBlendMode(int entityID, int blendMode)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->GlobalBlendMode = static_cast<TintBlendMode>(blendMode);
        data->TintController->MarkDirty();
    }

    __declspec(dllexport) bool TintController_GetAutoIncludeParentedSkinnedMeshes(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return true;
        return data->TintController->AutoIncludeParentedSkinnedMeshes;
    }

    __declspec(dllexport) void TintController_SetAutoIncludeParentedSkinnedMeshes(int entityID, bool enabled)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->AutoIncludeParentedSkinnedMeshes = enabled;
        data->TintController->MarkDirty();
    }
    
    __declspec(dllexport) void TintController_Refresh(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->NeedsRefresh = true;
    }

    __declspec(dllexport) void TintController_ClearTargets(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        data->TintController->Targets.clear();
        data->TintController->MarkDirty();
    }

    __declspec(dllexport) void TintController_RemoveTargetsForEntity(int entityID, int targetEntityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;
        auto& targets = data->TintController->Targets;
        targets.erase(
            std::remove_if(
                targets.begin(),
                targets.end(),
                [targetEntityID](const TintTarget& target) {
                    return static_cast<int>(target.TargetEntity) == targetEntityID;
                }),
            targets.end());
        data->TintController->MarkDirty();
    }

    __declspec(dllexport) void TintController_AddTarget(
        int entityID,
        int targetEntityID,
        int materialSlot,
        int blendMode,
        bool useTargetColor,
        float r,
        float g,
        float b,
        float a)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return;

        TintTarget target;
        target.TargetEntity = static_cast<EntityID>(targetEntityID);
        target.MaterialSlot = materialSlot;
        target.BlendMode = static_cast<TintBlendMode>(blendMode);
        target.UseTargetColor = useTargetColor;
        target.Color = glm::vec4(r, g, b, a);
        data->TintController->Targets.push_back(target);
        data->TintController->MarkDirty();
    }

    __declspec(dllexport) int TintController_GetTrackedTargetCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return 0;
        return static_cast<int>(data->TintController->Targets.size());
    }

    __declspec(dllexport) int TintController_GetTrackedTargetEntity(int entityID, int index)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->TintController) return -1;
        const auto& targets = data->TintController->Targets;
        if (index < 0 || index >= static_cast<int>(targets.size())) return -1;
        return static_cast<int>(targets[static_cast<size_t>(index)].TargetEntity);
    }

    // --- BoneAttachment ---
    __declspec(dllexport) bool BoneAttachment_HasComponent(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        return data && data->BoneAttachment != nullptr;
    }
    
    __declspec(dllexport) bool BoneAttachment_GetEnabled(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return false;
        return data->BoneAttachment->Enabled;
    }
    
    __declspec(dllexport) void BoneAttachment_SetEnabled(int entityID, bool enabled)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->Enabled = enabled;
    }
    
    __declspec(dllexport) const char* BoneAttachment_GetBoneName(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->BoneAttachment->TargetBoneName;
        return buf.c_str();
    }
    
    __declspec(dllexport) void BoneAttachment_SetBoneName(int entityID, const char* boneName)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment || !boneName) return;
        data->BoneAttachment->TargetBoneName = boneName;
        data->BoneAttachment->InvalidateResolution();
    }
    
    __declspec(dllexport) void BoneAttachment_GetLocalPosition(int entityID, float* x, float* y, float* z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) {
            if (x) *x = 0.0f; if (y) *y = 0.0f; if (z) *z = 0.0f;
            return;
        }
        const auto& pos = data->BoneAttachment->LocalPosition;
        if (x) *x = pos.x; if (y) *y = pos.y; if (z) *z = pos.z;
    }
    
    __declspec(dllexport) void BoneAttachment_SetLocalPosition(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->LocalPosition = glm::vec3(x, y, z);
    }
    
    __declspec(dllexport) void BoneAttachment_GetLocalRotation(int entityID, float* x, float* y, float* z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) {
            if (x) *x = 0.0f; if (y) *y = 0.0f; if (z) *z = 0.0f;
            return;
        }
        const auto& rot = data->BoneAttachment->LocalRotation;
        if (x) *x = rot.x; if (y) *y = rot.y; if (z) *z = rot.z;
    }
    
    __declspec(dllexport) void BoneAttachment_SetLocalRotation(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->LocalRotation = glm::vec3(x, y, z);
    }
    
    __declspec(dllexport) void BoneAttachment_GetLocalScale(int entityID, float* x, float* y, float* z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) {
            if (x) *x = 1.0f; if (y) *y = 1.0f; if (z) *z = 1.0f;
            return;
        }
        const auto& scale = data->BoneAttachment->LocalScale;
        if (x) *x = scale.x; if (y) *y = scale.y; if (z) *z = scale.z;
    }
    
    __declspec(dllexport) void BoneAttachment_SetLocalScale(int entityID, float x, float y, float z)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->LocalScale = glm::vec3(x, y, z);
    }
    
    __declspec(dllexport) bool BoneAttachment_GetInheritRotation(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return true;
        return data->BoneAttachment->InheritRotation;
    }
    
    __declspec(dllexport) void BoneAttachment_SetInheritRotation(int entityID, bool inherit)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->InheritRotation = inherit;
    }
    
    __declspec(dllexport) bool BoneAttachment_GetInheritScale(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return false;
        return data->BoneAttachment->InheritScale;
    }
    
    __declspec(dllexport) void BoneAttachment_SetInheritScale(int entityID, bool inherit)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->InheritScale = inherit;
    }
    
    __declspec(dllexport) bool BoneAttachment_IsResolved(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return false;
        return data->BoneAttachment->ResolvedBoneEntity != INVALID_ENTITY_ID;
    }
    
    __declspec(dllexport) void BoneAttachment_InvalidateResolution(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->InvalidateResolution();
    }
    
    __declspec(dllexport) int BoneAttachment_GetSkeletonEntity(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return -1;
        return static_cast<int>(data->BoneAttachment->SkeletonEntity);
    }
    
    __declspec(dllexport) void BoneAttachment_SetSkeletonEntity(int entityID, int skeletonEntityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->BoneAttachment) return;
        data->BoneAttachment->SkeletonEntity = static_cast<EntityID>(skeletonEntityID);
        // Invalidate resolution so it re-resolves with the new skeleton
        data->BoneAttachment->InvalidateResolution();
    }

    // --- Animator / AnimationPlayer ---
    __declspec(dllexport) void Animator_SetBool(int entityID, const char* name, bool value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) { return; }
        if (!data->AnimationPlayer) { return; }
        if (!name) { return; }
        data->AnimationPlayer->AnimatorInstance.SetBool(name, value);
    }

    __declspec(dllexport) void Animator_SetInt(int entityID, const char* name, int value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return;
        data->AnimationPlayer->AnimatorInstance.SetInt(name, value);
    }

    __declspec(dllexport) void Animator_SetFloat(int entityID, const char* name, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return;
        data->AnimationPlayer->AnimatorInstance.SetFloat(name, value);
    }

    __declspec(dllexport) void Animator_SetTrigger(int entityID, const char* name)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return;
        data->AnimationPlayer->AnimatorInstance.SetTrigger(name);
    }

    __declspec(dllexport) void Animator_ResetTrigger(int entityID, const char* name)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return;
        data->AnimationPlayer->AnimatorInstance.ResetTrigger(name);
    }

    __declspec(dllexport) bool Animator_GetBool(int entityID, const char* name)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return false;
        auto& bb = data->AnimationPlayer->AnimatorInstance.Blackboard();
        auto it = bb.Bools.find(name);
        return it != bb.Bools.end() ? it->second : false;
    }

    __declspec(dllexport) int Animator_GetInt(int entityID, const char* name)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return 0;
        auto& bb = data->AnimationPlayer->AnimatorInstance.Blackboard();
        auto it = bb.Ints.find(name);
        return it != bb.Ints.end() ? it->second : 0;
    }

    __declspec(dllexport) float Animator_GetFloat(int entityID, const char* name)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return 0.0f;
        auto& bb = data->AnimationPlayer->AnimatorInstance.Blackboard();
        auto it = bb.Floats.find(name);
        return it != bb.Floats.end() ? it->second : 0.0f;
    }

    __declspec(dllexport) bool Animator_GetTrigger(int entityID, const char* name)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer || !name) return false;
        auto& bb = data->AnimationPlayer->AnimatorInstance.Blackboard();
        auto it = bb.Triggers.find(name);
        return it != bb.Triggers.end() ? it->second : false;
    }

    __declspec(dllexport) void Animator_SetController(int entityID, const char* controllerPath, float blendSeconds)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;

        auto& player = *data->AnimationPlayer;
        std::string newPath = controllerPath ? controllerPath : "";

        // Reset runtime caches before switching
        player.InvalidateAssetCache();
        player.ActiveStates.clear();
        player._FiredEventIds.clear();
        player._PrevEventStateId = -1;
        player._PrevEventStateTime = 0.0f;
        player.ResetRootMotionTracking();
        player._AutoControllerGenerated = false;

        player.ControllerPath = newPath;

        if (newPath.empty()) {
            player.Controller.reset();
            player.SyncRuntimeControllerState();
            player._ControllerSwitchBlendActive = false;
            return;
        }

        // Prefer compiled controller binaries for faster switches (editor play mode).
#ifdef CLAYMORE_EDITOR
        if (Assets::ShouldLoadBinary()) {
            BinaryAssetCache::Instance().EnsureBinary(newPath);
        }
#endif
        auto controller = cm::animation::LoadAnimatorControllerFromFile(newPath);
        if (!controller) {
            player.Controller.reset();
            player.SyncRuntimeControllerState();
            player._ControllerSwitchBlendActive = false;
            return;
        }

        player.Controller = controller;
        player.SyncRuntimeControllerState();

        // Preload in parallel when possible to reduce switch hitching.
        player.PreloadAllControllerAssets(true);
        player.BeginControllerSwitchBlend(blendSeconds);
    }

    __declspec(dllexport) void Animator_SetOverride(int entityID, const char* overridePath)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;

        auto& player = *data->AnimationPlayer;
        player.ControllerOverridePath = overridePath ? overridePath : "";
        player.ControllerOverride.reset();

        if (!player.ControllerOverridePath.empty()) {
            player.ControllerOverride = cm::animation::LoadAnimatorControllerOverrideFromFile(player.ControllerOverridePath);
        }

        player.InvalidateResolvedAssetCache();
        player._FiredEventIds.clear();
        player._PrevEventStateId = -1;
        player._PrevEventStateTime = 0.0f;
        player.ResetRootMotionTracking();

        if (player.Controller) {
            player.PreloadAllControllerAssets(true);
        }
    }

    // --- UI Button state ---
    __declspec(dllexport) bool UI_ButtonIsHovered(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Button) return false;
        return data->Button->Hovered;
    }

    __declspec(dllexport) bool UI_ButtonIsPressed(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Button) return false;
        return data->Button->Pressed;
    }

    __declspec(dllexport) bool UI_ButtonWasClicked(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Button) return false;
        return data->Button->Clicked;
    }

    // --- UI Slider ---
    __declspec(dllexport) float UI_Slider_GetValue(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return 0.0f;
        return data->Slider->Value;
    }

    __declspec(dllexport) void UI_Slider_SetValue(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return;
        data->Slider->Value = value;
    }

    __declspec(dllexport) float UI_Slider_GetMinValue(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return 0.0f;
        return data->Slider->MinValue;
    }

    __declspec(dllexport) void UI_Slider_SetMinValue(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return;
        data->Slider->MinValue = value;
    }

    __declspec(dllexport) float UI_Slider_GetMaxValue(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return 1.0f;
        return data->Slider->MaxValue;
    }

    __declspec(dllexport) void UI_Slider_SetMaxValue(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return;
        data->Slider->MaxValue = value;
    }

    __declspec(dllexport) bool UI_Slider_IsHovered(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return false;
        return data->Slider->Hovered;
    }

    __declspec(dllexport) bool UI_Slider_IsDragging(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return false;
        return data->Slider->Dragging;
    }

    __declspec(dllexport) bool UI_Slider_ValueChanged(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Slider) return false;
        return data->Slider->ValueChanged;
    }

    // --- UI ProgressBar ---
    __declspec(dllexport) float UI_ProgressBar_GetValue(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return 0.0f;
        return data->ProgressBar->Value;
    }

    __declspec(dllexport) void UI_ProgressBar_SetValue(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return;
        data->ProgressBar->Value = value;
        data->ProgressBar->_DisplayValue = value;
    }

    __declspec(dllexport) float UI_ProgressBar_GetMinValue(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return 0.0f;
        return data->ProgressBar->MinValue;
    }

    __declspec(dllexport) void UI_ProgressBar_SetMinValue(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return;
        data->ProgressBar->MinValue = value;
    }

    __declspec(dllexport) float UI_ProgressBar_GetMaxValue(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return 1.0f;
        return data->ProgressBar->MaxValue;
    }

    __declspec(dllexport) void UI_ProgressBar_SetMaxValue(int entityID, float value)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return;
        data->ProgressBar->MaxValue = value;
    }

    __declspec(dllexport) float UI_ProgressBar_GetOpacity(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return 1.0f;
        return data->ProgressBar->Opacity;
    }

    __declspec(dllexport) void UI_ProgressBar_SetOpacity(int entityID, float opacity)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return;
        data->ProgressBar->Opacity = opacity;
    }

    __declspec(dllexport) bool UI_ProgressBar_GetVisible(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return true;
        return data->ProgressBar->Visible;
    }

    __declspec(dllexport) void UI_ProgressBar_SetVisible(int entityID, bool visible)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ProgressBar) return;
        data->ProgressBar->Visible = visible;
    }

    // --- UI Toggle ---
    __declspec(dllexport) bool UI_Toggle_GetIsOn(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Toggle) return false;
        return data->Toggle->IsOn;
    }

    __declspec(dllexport) void UI_Toggle_SetIsOn(int entityID, bool isOn)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Toggle) return;
        data->Toggle->IsOn = isOn;
    }

    __declspec(dllexport) bool UI_Toggle_IsHovered(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Toggle) return false;
        return data->Toggle->Hovered;
    }

    __declspec(dllexport) bool UI_Toggle_IsPressed(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Toggle) return false;
        return data->Toggle->Pressed;
    }

    __declspec(dllexport) bool UI_Toggle_ValueChanged(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Toggle) return false;
        return data->Toggle->ValueChanged;
    }

    // --- UI ScrollView ---
    __declspec(dllexport) void UI_ScrollView_GetContentOffset(int entityID, float* x, float* y)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) { if (x) *x = 0; if (y) *y = 0; return; }
        if (x) *x = data->ScrollView->ContentOffset.x;
        if (y) *y = data->ScrollView->ContentOffset.y;
    }

    __declspec(dllexport) void UI_ScrollView_SetContentOffset(int entityID, float x, float y)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) return;
        data->ScrollView->ContentOffset = glm::vec2(x, y);
    }

    __declspec(dllexport) void UI_ScrollView_GetContentSize(int entityID, float* w, float* h)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) { if (w) *w = 0; if (h) *h = 0; return; }
        if (w) *w = data->ScrollView->ContentSize.x;
        if (h) *h = data->ScrollView->ContentSize.y;
    }

    __declspec(dllexport) void UI_ScrollView_SetContentSize(int entityID, float w, float h)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) return;
        data->ScrollView->ContentSize = glm::vec2(w, h);
    }

    __declspec(dllexport) float UI_ScrollView_GetOpacity(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) return 1.0f;
        return data->ScrollView->Opacity;
    }

    __declspec(dllexport) void UI_ScrollView_SetOpacity(int entityID, float opacity)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) return;
        data->ScrollView->Opacity = opacity;
    }

    __declspec(dllexport) bool UI_ScrollView_GetVisible(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) return true;
        return data->ScrollView->Visible;
    }

    __declspec(dllexport) void UI_ScrollView_SetVisible(int entityID, bool visible)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->ScrollView) return;
        data->ScrollView->Visible = visible;
    }

    // --- UI InputField ---
    __declspec(dllexport) void UI_InputField_GetText(int entityID, const char** outText)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->InputField || !outText) { if (outText) *outText = ""; return; }
        auto& buf = GetRotatingStringBuffer();
        buf = data->InputField->Text;
        *outText = buf.c_str();
    }

    __declspec(dllexport) void UI_InputField_SetText(int entityID, const char* text)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->InputField) return;
        data->InputField->Text = text ? text : "";
    }

    __declspec(dllexport) void UI_InputField_GetPlaceholder(int entityID, const char** outText)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->InputField || !outText) { if (outText) *outText = ""; return; }
        auto& buf = GetRotatingStringBuffer();
        buf = data->InputField->PlaceholderText;
        *outText = buf.c_str();
    }

    __declspec(dllexport) void UI_InputField_SetPlaceholder(int entityID, const char* text)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->InputField) return;
        data->InputField->PlaceholderText = text ? text : "";
    }

    __declspec(dllexport) bool UI_InputField_IsFocused(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->InputField) return false;
        return data->InputField->IsFocused;
    }

    __declspec(dllexport) bool UI_InputField_TextChanged(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->InputField) return false;
        return data->InputField->TextChanged;
    }

    // --- UI Dropdown ---
    __declspec(dllexport) int UI_Dropdown_GetSelectedIndex(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return -1;
        return data->Dropdown->SelectedIndex;
    }

    __declspec(dllexport) void UI_Dropdown_SetSelectedIndex(int entityID, int index)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return;
        data->Dropdown->SelectedIndex = index;
    }

    __declspec(dllexport) int UI_Dropdown_GetOptionCount(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return 0;
        return static_cast<int>(data->Dropdown->Options.size());
    }

    __declspec(dllexport) void UI_Dropdown_GetOption(int entityID, int index, const char** outText)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown || !outText) { if (outText) *outText = ""; return; }
        if (index < 0 || index >= static_cast<int>(data->Dropdown->Options.size())) { *outText = ""; return; }
        auto& buf = GetRotatingStringBuffer();
        buf = data->Dropdown->Options[index];
        *outText = buf.c_str();
    }

    __declspec(dllexport) void UI_Dropdown_SetOption(int entityID, int index, const char* text)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return;
        if (index < 0 || index >= static_cast<int>(data->Dropdown->Options.size())) return;
        data->Dropdown->Options[index] = text ? text : "";
    }

    __declspec(dllexport) void UI_Dropdown_AddOption(int entityID, const char* text)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return;
        data->Dropdown->Options.push_back(text ? text : "");
    }

    __declspec(dllexport) void UI_Dropdown_ClearOptions(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return;
        data->Dropdown->Options.clear();
        data->Dropdown->SelectedIndex = -1;
    }

    __declspec(dllexport) bool UI_Dropdown_IsOpen(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return false;
        return data->Dropdown->IsOpen;
    }

    __declspec(dllexport) bool UI_Dropdown_SelectionChanged(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Dropdown) return false;
        return data->Dropdown->ValueChanged;
    }

    // --- Mesh property block (tint) ---
    __declspec(dllexport) void Mesh_SetColorTint(int entityID, float r, float g, float b, float a)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Mesh) return;
        // Set color tint via the first slot's property block
        if (!data->Mesh->SlotPropertyBlocks.empty()) {
            data->Mesh->SlotPropertyBlocks[0].SetVector("u_ColorTint", glm::vec4(r, g, b, a));
        }
    }

    __declspec(dllexport) void Mesh_GetColorTint(int entityID, float* r, float* g, float* b, float* a)
    {
        *r = *g = *b = *a = 1.0f;
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->Mesh) return;
        // Get color tint from the first slot's property block
        if (!data->Mesh->SlotPropertyBlocks.empty()) {
            auto it = data->Mesh->SlotPropertyBlocks[0].Vec4Uniforms.find("u_ColorTint");
            if (it != data->Mesh->SlotPropertyBlocks[0].Vec4Uniforms.end()) {
                *r = it->second.r;
                *g = it->second.g;
                *b = it->second.b;
                *a = it->second.a;
            }
        }
    }

    // --- AnimationPlayer single-clip controls ---
    __declspec(dllexport) void AnimationPlayer_Play(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;
        data->AnimationPlayer->IsPlaying = true;
    }

    __declspec(dllexport) void AnimationPlayer_Stop(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;
        data->AnimationPlayer->IsPlaying = false;
    }

    __declspec(dllexport) bool AnimationPlayer_IsPlaying(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return false;
        return data->AnimationPlayer->IsPlaying;
    }

    __declspec(dllexport) void AnimationPlayer_SetLoop(int entityID, bool loop)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;
        if (!data->AnimationPlayer->ActiveStates.empty()) {
            data->AnimationPlayer->ActiveStates[0].Loop = loop;
        }
    }

    __declspec(dllexport) void AnimationPlayer_SetSpeed(int entityID, float speed)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;
        data->AnimationPlayer->PlaybackSpeed = speed;
    }

    __declspec(dllexport) const char* AnimationPlayer_GetCurrentClipName(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->AnimationPlayer->Debug_CurrentAnimationName;
        return buf.c_str();
    }

    __declspec(dllexport) const char* Animator_GetCurrentStateName(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->AnimationPlayer->Debug_CurrentControllerStateName;
        return buf.c_str();
    }

    __declspec(dllexport) const char* Animator_GetPreviousStateName(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = data->AnimationPlayer->Debug_PreviousControllerStateName;
        return buf.c_str();
    }

    // Predict the next base-layer (layer 0) state given the current state and
    // current parameter/trigger values. Mirrors the exact runtime selection logic
    // used by AnimationSystem so editor play mode and exported runtime agree.
    //
    // Semantics:
    //  - If a crossfade is already in flight, report its committed target state.
    //  - Otherwise evaluate transitions exactly as the runtime would this frame.
    //  - If no transition qualifies (state loops or simply has no pending exit),
    //    return the current state name (i.e. "self"). Never returns null/empty
    //    unless the entity has no animator/controller/current state.
    __declspec(dllexport) const char* Animator_GetNextStateName(int entityID)
    {
        auto& buf = GetRotatingStringBuffer();
        buf.clear();
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return buf.c_str();
        auto& player = *data->AnimationPlayer;
        if (!player.Controller) return buf.c_str();

        auto& anim = player.AnimatorInstance;

        int nextId = -1;
        const auto& pb = anim.Playback();
        if (anim.IsCrossfading() && pb.NextStateId >= 0) {
            // A transition is mid-flight; its committed target is the next state.
            nextId = pb.NextStateId;
        } else {
            // Evaluate transitions from the current state using current params/triggers.
            // This is non-destructive (does not consume triggers).
            nextId = anim.ChooseNextState();
        }

        // No qualifying transition -> the controller stays in (loops back to) the
        // current state. Report self so callers can hook the active state safely.
        if (nextId < 0) nextId = player.CurrentStateId;

        const auto* st = player.Controller->FindStateInLayer(0, nextId);
        if (st) buf = st->Name;
        return buf.c_str();
    }

    __declspec(dllexport) bool Animator_IsPlaying(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return false;
        return data->AnimationPlayer->IsPlaying;
    }

    __declspec(dllexport) bool Animator_GetEnabled(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return false;
        return data->AnimationPlayer->Enabled;
    }

    __declspec(dllexport) void Animator_SetEnabled(int entityID, bool enabled)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data || !data->AnimationPlayer) return;
        data->AnimationPlayer->Enabled = enabled;
    }

    static const cm::npc::ScalabilityState* GetNpcScalabilityState(int entityID)
    {
        auto* data = Scene::Get().GetEntityData(entityID);
        if (!data) {
            return nullptr;
        }
        return &data->NpcScalability;
    }

    __declspec(dllexport) bool NpcScalability_GetParticipates(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->Participates : false;
    }

    __declspec(dllexport) int NpcScalability_GetTier(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? static_cast<int>(state->Tier) : 0;
    }

    __declspec(dllexport) int NpcScalability_GetRepresentation(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? static_cast<int>(state->Representation) : 0;
    }

    __declspec(dllexport) uint32_t NpcScalability_GetReasonFlags(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->ReasonFlags : 0u;
    }

    __declspec(dllexport) float NpcScalability_GetCameraDistance(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->CameraDistance : 0.0f;
    }

    __declspec(dllexport) bool NpcScalability_GetVisible(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->VisualVisible : false;
    }

    __declspec(dllexport) int NpcScalability_GetCrowdRank(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        if (!state || state->CrowdRank == std::numeric_limits<uint32_t>::max()) {
            return -1;
        }
        return static_cast<int>(state->CrowdRank);
    }

    __declspec(dllexport) int NpcScalability_GetCrowdCount(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? static_cast<int>(state->VisibleCrowdCount) : 0;
    }

    __declspec(dllexport) float NpcScalability_GetAnimationUpdateInterval(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->AnimationUpdateInterval : 0.0f;
    }

    __declspec(dllexport) float NpcScalability_GetScriptUpdateInterval(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->ScriptUpdateInterval : 0.0f;
    }

    __declspec(dllexport) float NpcScalability_GetNavigationRepathInterval(int entityID)
    {
        const auto* state = GetNpcScalabilityState(entityID);
        return state ? state->NavigationRepathInterval : 0.0f;
    }

    // =========================================================================
    // Animation Layer Interop (Controller-Based Layers)
    // =========================================================================
    // Layers are now defined in the AnimatorController, not created at runtime.
    // Scripts can only control layer weights to enable/disable layers.
    // Layer properties (mask, animation states, transitions) are authored in the editor.
    // =========================================================================
    
    static cm::animation::AnimationPlayerComponent* GetAnimPlayer(int entityID) {
        auto* data = Scene::Get().GetEntityData(entityID);
        return (data && data->AnimationPlayer) ? data->AnimationPlayer.get() : nullptr;
    }

    __declspec(dllexport) int Animator_GetCurrentClipRootMotionMode(int entityID)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player) {
            return static_cast<int>(cm::animation::RootMotionMode::None);
        }
        return static_cast<int>(player->_CurrentRootMotionMode);
    }
    
    // Get layer index from controller by name
    static int GetLayerIndex(int entityID, const char* layerName) {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller || !layerName) return -1;
        const auto* layer = player->Controller->GetLayerByName(layerName);
        return layer ? layer->Index : -1;
    }
    
    // Get layer state from Animator runtime
    static cm::animation::AnimatorLayerState* GetLayerState(int entityID, const char* layerName) {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !layerName) return nullptr;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return nullptr;
        return player->AnimatorInstance.GetLayerState(idx);
    }
    
    __declspec(dllexport) int AnimLayer_GetOrCreate(int entityID, const char* layerName, int priority)
    {
        // Layers are now defined in controller - this just returns the layer index if it exists
        // priority parameter is ignored (layer priority is defined in controller)
        (void)priority;
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller || !layerName) return -1;
        const auto* layer = player->Controller->GetLayerByName(layerName);
        if (!layer) return -1;
        // Ensure layer state is initialized
        player->AnimatorInstance.GetOrCreateLayerState(layer->Index);
        return layer->Index;
    }
    
    __declspec(dllexport) bool AnimLayer_Remove(int entityID, const char* layerName)
    {
        // Layers cannot be removed at runtime - they are defined in controller
        // This is kept for API compatibility but always returns false
        (void)entityID; (void)layerName;
        return false;
    }
    
    __declspec(dllexport) bool AnimLayer_Has(int entityID, const char* layerName)
    {
        return GetLayerIndex(entityID, layerName) >= 0;
    }
    
    __declspec(dllexport) int AnimLayer_GetCount(int entityID)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller) return 0;
        return player->Controller->GetLayerCount();
    }
    
    __declspec(dllexport) const char* AnimLayer_GetNameByIndex(int entityID, int index)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller) return "";
        const auto* layer = player->Controller->GetLayer(index);
        if (!layer) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = layer->Name;
        return buf.c_str();
    }
    
    __declspec(dllexport) void AnimLayer_SetAnimation(int entityID, const char* layerName, const char* animPath)
    {
        // Animation is controlled by layer's state machine, not directly settable
        // This is kept for API compatibility but does nothing
        (void)entityID; (void)layerName; (void)animPath;
    }
    
    __declspec(dllexport) const char* AnimLayer_GetAnimation(int entityID, const char* layerName)
    {
        // Return current state's animation path if available
        auto* player = GetAnimPlayer(entityID);
        int idx = GetLayerIndex(entityID, layerName);
        if (!player || !player->Controller || idx < 0) return "";
        const auto* layer = player->Controller->GetLayer(idx);
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        if (!layer || !layerState) return "";
        const auto* state = layer->FindState(layerState->CurrentStateId);
        if (!state) return "";
        auto& buf = GetRotatingStringBuffer();
        buf = !state->AnimationAssetPath.empty() ? state->AnimationAssetPath : state->ClipPath;
        return buf.c_str();
    }
    
    __declspec(dllexport) void AnimLayer_SetMask(int entityID, const char* layerName, int maskPreset)
    {
        // Mask is defined in controller, but we allow runtime override on layer state
        auto* layerState = GetLayerState(entityID, layerName);
        if (!layerState) return;
        layerState->Mask.ApplyPreset(static_cast<cm::animation::BodyMaskPreset>(maskPreset));
    }
    
    __declspec(dllexport) int AnimLayer_GetMask(int entityID, const char* layerName)
    {
        auto* layerState = GetLayerState(entityID, layerName);
        return layerState ? static_cast<int>(layerState->Mask.Preset) : 0;
    }
    
    __declspec(dllexport) void AnimLayer_SetBlendMode(int entityID, const char* layerName, int blendMode)
    {
        // Blend mode is defined in controller, but allow runtime override
        auto* layerState = GetLayerState(entityID, layerName);
        if (!layerState) return;
        layerState->BlendMode = static_cast<cm::animation::AnimatorLayerBlendMode>(blendMode);
    }
    
    __declspec(dllexport) int AnimLayer_GetBlendMode(int entityID, const char* layerName)
    {
        auto* layerState = GetLayerState(entityID, layerName);
        return layerState ? static_cast<int>(layerState->BlendMode) : 0;
    }
    
    __declspec(dllexport) void AnimLayer_SetWeight(int entityID, const char* layerName, float weight)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        player->AnimatorInstance.SetLayerWeight(idx, weight);
    }
    
    __declspec(dllexport) float AnimLayer_GetWeight(int entityID, const char* layerName)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player) return 0.0f;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return 0.0f;
        return player->AnimatorInstance.GetLayerWeight(idx);
    }
    
    __declspec(dllexport) void AnimLayer_BlendTo(int entityID, const char* layerName, float targetWeight, float blendSpeed)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        player->AnimatorInstance.BlendLayerWeight(idx, targetWeight, blendSpeed);
    }
    
    __declspec(dllexport) void AnimLayer_Play(int entityID, const char* layerName, bool loop)
    {
        // For controller-based layers, "Play" means set weight to 1 (enable the layer)
        // The loop parameter is defined per-state in the controller
        (void)loop;
        auto* player = GetAnimPlayer(entityID);
        if (!player) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        player->AnimatorInstance.SetLayerWeight(idx, 1.0f);
    }
    
    __declspec(dllexport) void AnimLayer_Stop(int entityID, const char* layerName)
    {
        // "Stop" means blend weight to 0 (disable the layer)
        auto* player = GetAnimPlayer(entityID);
        if (!player) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        if (!layerState) return;
        layerState->TargetWeight = 0.0f;
    }
    
    __declspec(dllexport) bool AnimLayer_IsPlaying(int entityID, const char* layerName)
    {
        // "IsPlaying" means weight > 0 (layer is active)
        auto* layerState = GetLayerState(entityID, layerName);
        return layerState && layerState->IsActive();
    }
    
    __declspec(dllexport) void AnimLayer_SetSpeed(int entityID, const char* layerName, float speed)
    {
        // Speed is per-state in controller, but we could add a runtime speed multiplier
        // For now, this is a no-op since speed comes from state definition
        (void)entityID; (void)layerName; (void)speed;
    }
    
    __declspec(dllexport) float AnimLayer_GetSpeed(int entityID, const char* layerName)
    {
        // Return the speed of the current state
        auto* player = GetAnimPlayer(entityID);
        int idx = GetLayerIndex(entityID, layerName);
        if (!player || !player->Controller || idx < 0) return 1.0f;
        const auto* layer = player->Controller->GetLayer(idx);
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        if (!layer || !layerState) return 1.0f;
        const auto* state = layer->FindState(layerState->CurrentStateId);
        return state ? state->Speed : 1.0f;
    }
    
    __declspec(dllexport) void AnimLayer_SetTime(int entityID, const char* layerName, float time)
    {
        auto* layerState = GetLayerState(entityID, layerName);
        if (!layerState) return;
        layerState->StateTime = time;
    }

    static float NormalizedToStateTime(float normalizedTime, float duration)
    {
        const float nt = glm::clamp(normalizedTime, 0.0f, 1.0f);
        if (duration <= 0.0f) return 0.0f;
        if (nt >= 1.0f) {
            return std::nextafter(duration, 0.0f);
        }
        return nt * duration;
    }

    static const cm::animation::AnimatorTransition* FindBestTransitionToState(
        const cm::animation::AnimatorLayer* layer,
        int currentStateId,
        int targetStateId)
    {
        if (!layer) return nullptr;

        for (const auto& t : layer->Transitions) {
            if (t.FromState == currentStateId && t.ToState == targetStateId) {
                return &t;
            }
        }
        for (const auto& t : layer->Transitions) {
            if (t.FromState == -1 && t.ToState == targetStateId) {
                return &t;
            }
        }
        for (const auto& t : layer->Transitions) {
            if (t.ToState == targetStateId) {
                return &t;
            }
        }
        return nullptr;
    }

    static void ApplyTransitionConditionsToBlackboard(
        const cm::animation::AnimatorController* controller,
        const cm::animation::AnimatorTransition* transition,
        cm::animation::AnimatorBlackboard& blackboard)
    {
        if (!controller || !transition) return;

        auto findParamType = [&](const std::string& name) {
            for (const auto& param : controller->Parameters) {
                if (param.Name == name) return param.Type;
            }
            return cm::animation::AnimatorParamType::Float;
        };

        for (const auto& cond : transition->Conditions) {
            const auto paramType = findParamType(cond.Parameter);
            switch (paramType) {
                case cm::animation::AnimatorParamType::Bool: {
                    bool value = true;
                    switch (cond.Mode) {
                        case cm::animation::ConditionMode::If: value = true; break;
                        case cm::animation::ConditionMode::IfNot: value = false; break;
                        case cm::animation::ConditionMode::Greater: value = cond.Threshold <= 0.0f; break;
                        case cm::animation::ConditionMode::Less: value = cond.Threshold > 0.0f; break;
                        case cm::animation::ConditionMode::Equals: value = cond.Threshold >= 0.5f || cond.IntThreshold != 0; break;
                        case cm::animation::ConditionMode::NotEquals: value = !(cond.Threshold >= 0.5f || cond.IntThreshold != 0); break;
                        case cm::animation::ConditionMode::Trigger: value = true; break;
                    }
                    blackboard.Bools[cond.Parameter] = value;
                    break;
                }
                case cm::animation::AnimatorParamType::Int: {
                    int value = cond.IntThreshold;
                    switch (cond.Mode) {
                        case cm::animation::ConditionMode::Greater: value = cond.IntThreshold + 1; break;
                        case cm::animation::ConditionMode::Less: value = cond.IntThreshold - 1; break;
                        case cm::animation::ConditionMode::Equals: value = cond.IntThreshold; break;
                        case cm::animation::ConditionMode::NotEquals: value = cond.IntThreshold + 1; break;
                        default: break;
                    }
                    blackboard.Ints[cond.Parameter] = value;
                    break;
                }
                case cm::animation::AnimatorParamType::Trigger: {
                    blackboard.Triggers[cond.Parameter] = true;
                    break;
                }
                case cm::animation::AnimatorParamType::Float:
                default: {
                    float value = cond.Threshold;
                    switch (cond.Mode) {
                        case cm::animation::ConditionMode::Greater: value = cond.Threshold + 0.001f; break;
                        case cm::animation::ConditionMode::Less: value = cond.Threshold - 0.001f; break;
                        case cm::animation::ConditionMode::Equals: value = cond.Threshold; break;
                        case cm::animation::ConditionMode::NotEquals: value = cond.Threshold + 0.001f; break;
                        default: break;
                    }
                    blackboard.Floats[cond.Parameter] = value;
                    break;
                }
            }
        }
    }

    static float ResolveLayerStateDuration(cm::animation::AnimationPlayerComponent* player, int layerIdx, const cm::animation::AnimatorState* state)
    {
        if (!player || !state) return 0.0f;
        if (state->Kind == cm::animation::AnimatorStateKind::Blend1D) {
            if (state->Blend1DEntries.empty()) return 0.0f;
            float x = glm::clamp(
                player->AnimatorInstance.GetFloatSlot(state->RuntimeBlend1DParamSlot, 0.0f),
                0.0f,
                1.0f);
            const auto& e = state->Blend1DEntries;
            int i1 = 0, i2 = static_cast<int>(e.size()) - 1;
            for (int i = 0; i < static_cast<int>(e.size()); ++i) {
                if (e[i].Key <= x) i1 = i;
                if (e[i].Key >= x) { i2 = i; break; }
            }
            i1 = glm::clamp(i1, 0, static_cast<int>(e.size()) - 1);
            i2 = glm::clamp(i2, 0, static_cast<int>(e.size()) - 1);
            const auto& ea = e[i1];
            const auto& eb = e[i2];
            const float denom = std::max(1e-6f, (eb.Key - ea.Key));
            const float t = glm::clamp((x - ea.Key) / denom, 0.0f, 1.0f);

            auto getEntryDuration = [&](int idx, const cm::animation::Blend1DEntry& entry) -> float {
                const std::string path = !entry.AssetPath.empty() ? entry.AssetPath : entry.ClipPath;
                if (path.empty()) return 0.0f;
                const int cacheKey = (layerIdx * 10000) + state->Id * 100 + idx;
                auto it = player->CachedAssets.find(cacheKey);
                if (it == player->CachedAssets.end() || !it->second) {
                    auto asset = cm::animation::LoadAnimationAssetCached(path, true);
                    if (asset && asset->Duration() > 0.0f) {
                        player->CachedAssets[cacheKey] = asset;
                        return asset->Duration();
                    }
                    return 0.0f;
                }
                return it->second->Duration();
            };

            const float d0 = getEntryDuration(i1, ea);
            const float d1 = getEntryDuration(i2, eb);
            return glm::mix(d0, d1, t);
        } else if (state->Kind == cm::animation::AnimatorStateKind::Blend2D) {
            if (state->Blend2DEntries.empty()) return 0.0f;

            float x = 0.0f;
            float y = 0.0f;
            x = player->AnimatorInstance.GetFloatSlot(state->RuntimeBlend2DParamXSlot, 0.0f);
            y = player->AnimatorInstance.GetFloatSlot(state->RuntimeBlend2DParamYSlot, 0.0f);

            struct Candidate { int idx = -1; float d2 = 0.0f; };
            constexpr int kMaxBlend2DSamples = 4;
            std::array<Candidate, kMaxBlend2DSamples> nearest{};
            int nearestCount = 0;

            auto insertCandidate = [&](int idx, float d2) {
                int insertPos = nearestCount;
                if (nearestCount < kMaxBlend2DSamples) {
                    nearestCount++;
                } else {
                    if (d2 >= nearest[kMaxBlend2DSamples - 1].d2) {
                        return;
                    }
                    insertPos = kMaxBlend2DSamples - 1;
                }

                nearest[insertPos] = { idx, d2 };
                while (insertPos > 0 && nearest[insertPos].d2 < nearest[insertPos - 1].d2) {
                    std::swap(nearest[insertPos], nearest[insertPos - 1]);
                    --insertPos;
                }
            };

            bool foundExact = false;
            int exactIndex = -1;
            for (int i = 0; i < static_cast<int>(state->Blend2DEntries.size()); ++i) {
                const auto& entry = state->Blend2DEntries[i];
                const std::string path = !entry.AssetPath.empty() ? entry.AssetPath : entry.ClipPath;
                if (path.empty()) continue;
                const float dx = x - entry.X;
                const float dy = y - entry.Y;
                const float d2 = dx * dx + dy * dy;
                if (d2 <= 1e-6f) {
                    foundExact = true;
                    exactIndex = i;
                    break;
                }
                insertCandidate(i, d2);
            }
            if (!foundExact && nearestCount <= 0) return 0.0f;

            std::array<int, kMaxBlend2DSamples> indices{};
            std::array<float, kMaxBlend2DSamples> weights{};
            int sampleCount = 0;
            if (foundExact) {
                indices[0] = exactIndex;
                weights[0] = 1.0f;
                sampleCount = 1;
            } else {
                float totalW = 0.0f;
                sampleCount = nearestCount;
                for (int i = 0; i < sampleCount; ++i) {
                    indices[i] = nearest[i].idx;
                    weights[i] = 1.0f / std::max(1e-4f, nearest[i].d2);
                    totalW += weights[i];
                }
                if (totalW <= 1e-6f) return 0.0f;
                const float invTotalW = 1.0f / totalW;
                for (int i = 0; i < sampleCount; ++i) {
                    weights[i] *= invTotalW;
                }
            }

            float duration = 0.0f;
            float loadedWeight = 0.0f;
            for (int i = 0; i < sampleCount; ++i) {
                const auto& entry = state->Blend2DEntries[indices[i]];
                const std::string path = !entry.AssetPath.empty() ? entry.AssetPath : entry.ClipPath;
                if (path.empty()) continue;
                const int cacheKey = (layerIdx * 10000) + state->Id * 100 + indices[i];
                auto it = player->CachedAssets.find(cacheKey);
                std::shared_ptr<cm::animation::AnimationAsset> asset;
                if (it == player->CachedAssets.end() || !it->second) {
                    asset = cm::animation::LoadAnimationAssetCached(path, true);
                    if (asset && asset->Duration() > 0.0f) {
                        player->CachedAssets[cacheKey] = asset;
                    }
                } else {
                    asset = it->second;
                }
                if (!asset || asset->Duration() <= 0.0f) continue;
                duration += weights[i] * asset->Duration();
                loadedWeight += weights[i];
            }

            if (loadedWeight <= 1e-6f) return 0.0f;
            return duration / loadedWeight;
        }

        const std::string path = !state->AnimationAssetPath.empty() ? state->AnimationAssetPath : state->ClipPath;
        if (path.empty()) return 0.0f;
        const int cacheKey = (layerIdx * 10000) + state->Id;
        auto it = player->CachedAssets.find(cacheKey);
        if (it == player->CachedAssets.end() || !it->second) {
            auto asset = cm::animation::LoadAnimationAssetCached(path, true);
            if (asset && asset->Duration() > 0.0f) {
                player->CachedAssets[cacheKey] = asset;
                return asset->Duration();
            }
            return 0.0f;
        }
        return it->second->Duration();
    }

    __declspec(dllexport) void AnimLayer_SetNormalizedTime(int entityID, const char* layerName, float normalizedTime)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        const auto* layer = player->Controller->GetLayer(idx);
        if (!layerState || !layer) return;
        const auto* state = layer->FindState(layerState->CurrentStateId);
        if (!state) return;

        const float duration = ResolveLayerStateDuration(player, idx, state);
        const float targetTime = NormalizedToStateTime(normalizedTime, duration);
        layerState->SetStateTime(targetTime, duration);

        if (idx == 0) {
            player->AnimatorInstance.SetStateTime(targetTime, duration);
            player->CurrentStateId = layerState->CurrentStateId;
        }
    }

    __declspec(dllexport) void AnimLayer_SetState(int entityID, const char* layerName, int stateId, float normalizedTime)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        const auto* layer = player->Controller->GetLayer(idx);
        if (!layerState || !layer) return;
        const auto* state = layer->FindState(stateId);
        if (!state) return;

        layerState->SetCurrentState(stateId, true);
        layerState->_FiredEventIds.clear();
        layerState->_PrevEventStateId = -1;
        layerState->_PrevEventStateTime = 0.0f;

        const float duration = ResolveLayerStateDuration(player, idx, state);
        const float targetTime = NormalizedToStateTime(normalizedTime, duration);
        layerState->SetStateTime(targetTime, duration);

        if (idx == 0) {
            player->AnimatorInstance.SetCurrentState(stateId, true);
            player->AnimatorInstance.SetStateTime(targetTime, duration);
            player->CurrentStateId = stateId;
        }
    }

    __declspec(dllexport) void AnimLayer_SetStateByName(int entityID, const char* layerName, const char* stateName, float normalizedTime, bool satisfyTransitionConditions)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller || !stateName) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        const auto* layer = player->Controller->GetLayer(idx);
        if (!layerState || !layer) return;

        const cm::animation::AnimatorState* targetState = nullptr;
        for (const auto& state : layer->States) {
            if (state.Name == stateName) {
                targetState = &state;
                break;
            }
        }
        if (!targetState) return;

        const auto* matchedTransition = FindBestTransitionToState(layer, layerState->CurrentStateId, targetState->Id);
        if (satisfyTransitionConditions) {
            ApplyTransitionConditionsToBlackboard(player->Controller.get(), matchedTransition, player->AnimatorInstance.Blackboard());
        }

        layerState->SetCurrentState(targetState->Id, true);
        layerState->_FiredEventIds.clear();
        layerState->_PrevEventStateId = -1;
        layerState->_PrevEventStateTime = 0.0f;

        const float duration = ResolveLayerStateDuration(player, idx, targetState);
        const float targetTime = NormalizedToStateTime(normalizedTime, duration);
        layerState->SetStateTime(targetTime, duration);

        if (satisfyTransitionConditions) {
            player->AnimatorInstance.ConsumeTriggersForTransition(matchedTransition);
        }

        if (idx == 0) {
            player->AnimatorInstance.SetCurrentState(targetState->Id, true);
            player->AnimatorInstance.SetStateTime(targetTime, duration);
            player->CurrentStateId = targetState->Id;
        }
    }

    __declspec(dllexport) void AnimLayer_SetBlend2D(int entityID, const char* layerName, float x, float y, bool clampToUnitRange)
    {
        auto* player = GetAnimPlayer(entityID);
        if (!player || !player->Controller) return;
        int idx = GetLayerIndex(entityID, layerName);
        if (idx < 0) return;

        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        const auto* layer = player->Controller->GetLayer(idx);
        if (!layerState || !layer) return;

        const auto* state = layer->FindState(layerState->CurrentStateId);
        if (!state || state->Kind != cm::animation::AnimatorStateKind::Blend2D) return;

        if (clampToUnitRange) {
            x = glm::clamp(x, -1.0f, 1.0f);
            y = glm::clamp(y, -1.0f, 1.0f);
        }

        auto& blackboard = player->AnimatorInstance.Blackboard();
        if (!state->Blend2DParamX.empty()) {
            blackboard.Floats[state->Blend2DParamX] = x;
        }
        if (!state->Blend2DParamY.empty()) {
            blackboard.Floats[state->Blend2DParamY] = y;
        }
    }
    
    __declspec(dllexport) float AnimLayer_GetTime(int entityID, const char* layerName)
    {
        auto* layerState = GetLayerState(entityID, layerName);
        return layerState ? layerState->StateTime : 0.0f;
    }
    
    __declspec(dllexport) float AnimLayer_GetDuration(int entityID, const char* layerName)
    {
        auto* player = GetAnimPlayer(entityID);
        int idx = GetLayerIndex(entityID, layerName);
        if (!player || !player->Controller || idx < 0) return 0.0f;
        const auto* layer = player->Controller->GetLayer(idx);
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        if (!layer || !layerState) return 0.0f;
        const auto* state = layer->FindState(layerState->CurrentStateId);
        if (!state) return 0.0f;
        return ResolveLayerStateDuration(player, idx, state);
    }
    
    __declspec(dllexport) void AnimLayer_SetLooping(int entityID, const char* layerName, bool loop)
    {
        // Looping is defined per-state in controller, not runtime settable
        (void)entityID; (void)layerName; (void)loop;
    }
    
    __declspec(dllexport) bool AnimLayer_GetLooping(int entityID, const char* layerName)
    {
        // Return loop setting of current state
        auto* player = GetAnimPlayer(entityID);
        int idx = GetLayerIndex(entityID, layerName);
        if (!player || !player->Controller || idx < 0) return true;
        const auto* layer = player->Controller->GetLayer(idx);
        auto* layerState = player->AnimatorInstance.GetLayerState(idx);
        if (!layer || !layerState) return true;
        const auto* state = layer->FindState(layerState->CurrentStateId);
        return state ? state->Loop : true;
    }

    // =========================================================================
    // Material Property Block Interop - Function Pointer Getters
    // The actual implementations are static functions above, exposed via function pointers
    // =========================================================================
    __declspec(dllexport) void* Get_Material_SetVector4_Ptr() { return (void*)Material_SetVector4Ptr; }
    __declspec(dllexport) void* Get_Material_GetVector4_Ptr() { return (void*)Material_GetVector4Ptr; }
    __declspec(dllexport) void* Get_Material_HasProperty_Ptr() { return (void*)Material_HasPropertyPtr; }
    __declspec(dllexport) void* Get_Material_RemoveProperty_Ptr() { return (void*)Material_RemovePropertyPtr; }
    __declspec(dllexport) void* Get_Material_ClearAll_Ptr() { return (void*)Material_ClearAllPtr; }
    __declspec(dllexport) void* Get_Material_SetTexturePath_Ptr() { return (void*)Material_SetTexturePathPtr; }
    __declspec(dllexport) void* Get_Material_SetVector4Slot_Ptr() { return (void*)Material_SetVector4SlotPtr; }
    __declspec(dllexport) void* Get_Material_GetVector4Slot_Ptr() { return (void*)Material_GetVector4SlotPtr; }
    __declspec(dllexport) void* Get_Material_HasPropertySlot_Ptr() { return (void*)Material_HasPropertySlotPtr; }
    __declspec(dllexport) void* Get_Material_RemovePropertySlot_Ptr() { return (void*)Material_RemovePropertySlotPtr; }
    __declspec(dllexport) void* Get_Material_ClearSlot_Ptr() { return (void*)Material_ClearSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetSlotCount_Ptr() { return (void*)Material_GetSlotCountPtr; }
    __declspec(dllexport) void* Get_Material_SetTexturePathSlot_Ptr() { return (void*)Material_SetTexturePathSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetMaterialTypeSlot_Ptr() { return (void*)Material_GetMaterialTypeSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetMaterialNameSlot_Ptr() { return (void*)Material_GetMaterialNameSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetMaterialAssetPathSlot_Ptr() { return (void*)Material_GetMaterialAssetPathSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetMaterialAssetPathSlot_Ptr() { return (void*)Material_SetMaterialAssetPathSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetMaterialVector4Slot_Ptr() { return (void*)Material_SetMaterialVector4SlotPtr; }
    __declspec(dllexport) void* Get_Material_GetMaterialVector4Slot_Ptr() { return (void*)Material_GetMaterialVector4SlotPtr; }
    __declspec(dllexport) void* Get_Material_HasMaterialPropertySlot_Ptr() { return (void*)Material_HasMaterialPropertySlotPtr; }
    __declspec(dllexport) void* Get_Material_SetMaterialTexturePathSlot_Ptr() { return (void*)Material_SetMaterialTexturePathSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetMaterialTexturePathSlot_Ptr() { return (void*)Material_GetMaterialTexturePathSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetPbrScalarSlot_Ptr() { return (void*)Material_GetPbrScalarSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetPbrScalarSlot_Ptr() { return (void*)Material_SetPbrScalarSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetPbrEmissionColorSlot_Ptr() { return (void*)Material_GetPbrEmissionColorSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetPbrEmissionColorSlot_Ptr() { return (void*)Material_SetPbrEmissionColorSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetPbrUVTransformSlot_Ptr() { return (void*)Material_GetPbrUVTransformSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetPbrUVTransformSlot_Ptr() { return (void*)Material_SetPbrUVTransformSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetPbrReceiveShadowsOverrideSlot_Ptr() { return (void*)Material_GetPbrReceiveShadowsOverrideSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetPbrReceiveShadowsOverrideSlot_Ptr() { return (void*)Material_SetPbrReceiveShadowsOverrideSlotPtr; }
    __declspec(dllexport) void* Get_Material_GetPbrReceiveShadowsSlot_Ptr() { return (void*)Material_GetPbrReceiveShadowsSlotPtr; }
    __declspec(dllexport) void* Get_Material_SetPbrReceiveShadowsSlot_Ptr() { return (void*)Material_SetPbrReceiveShadowsSlotPtr; }

    // Particle Emitter Interop - Function Pointer Getters (52 functions)
    __declspec(dllexport) void* Get_Particle_GetEnabled_Ptr() { return (void*)Particle_GetEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_SetEnabled_Ptr() { return (void*)Particle_SetEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_Play_Ptr() { return (void*)Particle_PlayPtr; }
    __declspec(dllexport) void* Get_Particle_Stop_Ptr() { return (void*)Particle_StopPtr; }
    __declspec(dllexport) void* Get_Particle_Restart_Ptr() { return (void*)Particle_RestartPtr; }
    __declspec(dllexport) void* Get_Particle_IsPlaying_Ptr() { return (void*)Particle_IsPlayingPtr; }
    __declspec(dllexport) void* Get_Particle_GetSimulationSpace_Ptr() { return (void*)Particle_GetSimulationSpacePtr; }
    __declspec(dllexport) void* Get_Particle_SetSimulationSpace_Ptr() { return (void*)Particle_SetSimulationSpacePtr; }
    __declspec(dllexport) void* Get_Particle_GetShape_Ptr() { return (void*)Particle_GetShapePtr; }
    __declspec(dllexport) void* Get_Particle_SetShape_Ptr() { return (void*)Particle_SetShapePtr; }
    __declspec(dllexport) void* Get_Particle_GetShapeRadius_Ptr() { return (void*)Particle_GetShapeRadiusPtr; }
    __declspec(dllexport) void* Get_Particle_SetShapeRadius_Ptr() { return (void*)Particle_SetShapeRadiusPtr; }
    __declspec(dllexport) void* Get_Particle_GetShapeAngle_Ptr() { return (void*)Particle_GetShapeAnglePtr; }
    __declspec(dllexport) void* Get_Particle_SetShapeAngle_Ptr() { return (void*)Particle_SetShapeAnglePtr; }
    __declspec(dllexport) void* Get_Particle_GetStartSpeed_Ptr() { return (void*)Particle_GetStartSpeedPtr; }
    __declspec(dllexport) void* Get_Particle_SetStartSpeed_Ptr() { return (void*)Particle_SetStartSpeedPtr; }
    __declspec(dllexport) void* Get_Particle_GetStartSize_Ptr() { return (void*)Particle_GetStartSizePtr; }
    __declspec(dllexport) void* Get_Particle_SetStartSize_Ptr() { return (void*)Particle_SetStartSizePtr; }
    __declspec(dllexport) void* Get_Particle_GetStartColor_Ptr() { return (void*)Particle_GetStartColorPtr; }
    __declspec(dllexport) void* Get_Particle_SetStartColor_Ptr() { return (void*)Particle_SetStartColorPtr; }
    __declspec(dllexport) void* Get_Particle_GetEmissionRate_Ptr() { return (void*)Particle_GetEmissionRatePtr; }
    __declspec(dllexport) void* Get_Particle_SetEmissionRate_Ptr() { return (void*)Particle_SetEmissionRatePtr; }
    __declspec(dllexport) void* Get_Particle_GetLooping_Ptr() { return (void*)Particle_GetLoopingPtr; }
    __declspec(dllexport) void* Get_Particle_SetLooping_Ptr() { return (void*)Particle_SetLoopingPtr; }
    __declspec(dllexport) void* Get_Particle_GetDuration_Ptr() { return (void*)Particle_GetDurationPtr; }
    __declspec(dllexport) void* Get_Particle_SetDuration_Ptr() { return (void*)Particle_SetDurationPtr; }
    __declspec(dllexport) void* Get_Particle_GetLifetime_Ptr() { return (void*)Particle_GetLifetimePtr; }
    __declspec(dllexport) void* Get_Particle_SetLifetime_Ptr() { return (void*)Particle_SetLifetimePtr; }
    __declspec(dllexport) void* Get_Particle_GetGravityModifier_Ptr() { return (void*)Particle_GetGravityModifierPtr; }
    __declspec(dllexport) void* Get_Particle_SetGravityModifier_Ptr() { return (void*)Particle_SetGravityModifierPtr; }
    __declspec(dllexport) void* Get_Particle_GetMaxParticles_Ptr() { return (void*)Particle_GetMaxParticlesPtr; }
    __declspec(dllexport) void* Get_Particle_SetMaxParticles_Ptr() { return (void*)Particle_SetMaxParticlesPtr; }
    __declspec(dllexport) void* Get_Particle_GetSizeOverLifetimeEnabled_Ptr() { return (void*)Particle_GetSizeOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_SetSizeOverLifetimeEnabled_Ptr() { return (void*)Particle_SetSizeOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_GetColorOverLifetimeEnabled_Ptr() { return (void*)Particle_GetColorOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_SetColorOverLifetimeEnabled_Ptr() { return (void*)Particle_SetColorOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_GetVelocityOverLifetimeEnabled_Ptr() { return (void*)Particle_GetVelocityOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_SetVelocityOverLifetimeEnabled_Ptr() { return (void*)Particle_SetVelocityOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_GetRotationOverLifetimeEnabled_Ptr() { return (void*)Particle_GetRotationOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_SetRotationOverLifetimeEnabled_Ptr() { return (void*)Particle_SetRotationOverLifetimeEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_GetAlignWithTrajectory_Ptr() { return (void*)Particle_GetAlignWithTrajectoryPtr; }
    __declspec(dllexport) void* Get_Particle_SetAlignWithTrajectory_Ptr() { return (void*)Particle_SetAlignWithTrajectoryPtr; }
    __declspec(dllexport) void* Get_Particle_GetBurstEnabled_Ptr() { return (void*)Particle_GetBurstEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_SetBurstEnabled_Ptr() { return (void*)Particle_SetBurstEnabledPtr; }
    __declspec(dllexport) void* Get_Particle_GetSizeOverLifetime_Ptr() { return (void*)Particle_GetSizeOverLifetimePtr; }
    __declspec(dllexport) void* Get_Particle_SetSizeOverLifetime_Ptr() { return (void*)Particle_SetSizeOverLifetimePtr; }
    __declspec(dllexport) void* Get_Particle_GetColorGradientKeyCount_Ptr() { return (void*)Particle_GetColorGradientKeyCountPtr; }
    __declspec(dllexport) void* Get_Particle_GetColorGradientKey_Ptr() { return (void*)Particle_GetColorGradientKeyPtr; }
    __declspec(dllexport) void* Get_Particle_SetColorGradientKey_Ptr() { return (void*)Particle_SetColorGradientKeyPtr; }
    __declspec(dllexport) void* Get_Particle_ClearColorGradient_Ptr() { return (void*)Particle_ClearColorGradientPtr; }
    __declspec(dllexport) void* Get_Particle_GetBurstCount_Ptr() { return (void*)Particle_GetBurstCountPtr; }
    __declspec(dllexport) void* Get_Particle_SetBurstCount_Ptr() { return (void*)Particle_SetBurstCountPtr; }
}
