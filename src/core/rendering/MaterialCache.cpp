#include "MaterialCache.h"
#include "MaterialManager.h"
#include "MaterialInstance.h"
#include "PBRMaterial.h"
#include "RuntimeShaderGraphMaterial.h"
#include "SkinnedPBRMaterial.h"
#include "BgfxLifecycle.h"
#include "TextureLoader.h"
#include "core/ecs/Scene.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <vector>

namespace
{
struct MaterialKey
{
    bool Skinned = false;
    bool AlphaBlend = false;
    bool AlphaCutout = false;
    bool TwoSided = false;
    bool HasTint = false;
    float AlphaCutoutThreshold = 0.5f;
    glm::vec4 ColorTint = glm::vec4(1.0f);
    size_t AlbedoHash = 0;
    size_t MetallicRoughnessHash = 0;
    size_t NormalHash = 0;
    size_t AOHash = 0;
    size_t EmissionHash = 0;
    size_t DisplacementHash = 0;

    bool operator==(const MaterialKey& other) const
    {
        return Skinned == other.Skinned &&
            AlphaBlend == other.AlphaBlend &&
            AlphaCutout == other.AlphaCutout &&
            TwoSided == other.TwoSided &&
            HasTint == other.HasTint &&
            AlphaCutoutThreshold == other.AlphaCutoutThreshold &&
            ColorTint == other.ColorTint &&
            AlbedoHash == other.AlbedoHash &&
            MetallicRoughnessHash == other.MetallicRoughnessHash &&
            NormalHash == other.NormalHash &&
            AOHash == other.AOHash &&
            EmissionHash == other.EmissionHash &&
            DisplacementHash == other.DisplacementHash;
    }
};

struct MaterialKeyHasher
{
    size_t operator()(const MaterialKey& key) const noexcept
    {
        size_t h = std::hash<bool>{}(key.Skinned);
        h ^= std::hash<bool>{}(key.AlphaBlend) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(key.AlphaCutout) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(key.TwoSided) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(key.HasTint) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(key.AlphaCutoutThreshold) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(key.ColorTint.x) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(key.ColorTint.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(key.ColorTint.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<float>{}(key.ColorTint.w) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= key.AlbedoHash + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= key.MetallicRoughnessHash + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= key.NormalHash + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= key.AOHash + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= key.EmissionHash + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= key.DisplacementHash + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static size_t HashCombine(size_t seed, size_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

static size_t HashTextureSpecifier(const TextureSpecifier& spec, TextureColorSpace colorSpace = TextureColorSpace::Linear)
{
    size_t h = std::hash<std::string>{}(spec.Path);
    // Include color space in hash so same texture can be cached with different color spaces
    h = HashCombine(h, std::hash<int>{}(static_cast<int>(colorSpace)));
    h = HashCombine(h, std::hash<uint32_t>{}(GetTextureLoadSettings().MaxDimension));
    if (spec.Embedded.HasData())
    {
        h = HashCombine(h, std::hash<int>{}(spec.Embedded.Width));
        h = HashCombine(h, std::hash<int>{}(spec.Embedded.Height));
        h = HashCombine(h, std::hash<bool>{}(spec.Embedded.IsCompressed));
        if (!spec.Embedded.Bytes.empty())
        {
            std::string_view view(reinterpret_cast<const char*>(spec.Embedded.Bytes.data()), spec.Embedded.Bytes.size());
            h = HashCombine(h, std::hash<std::string_view>{}(view));
        }
    }
    return h;
}

static bool TextureSpecifierHasPayload(const TextureSpecifier& spec)
{
    return !spec.Path.empty() || spec.Embedded.HasData();
}

static std::mutex s_materialMutex;
static std::mutex s_equivalentMaterialMutex;
static std::mutex s_textureMutex;
static std::unordered_map<MaterialKey, std::weak_ptr<Material>, MaterialKeyHasher> s_materialCache;
static std::unordered_map<uint64_t, std::weak_ptr<Material>> s_equivalentMaterialCache;
static std::atomic<uint64_t> s_textureCacheGeneration{1};
static std::atomic<uint64_t> s_textureFrameEpoch{0};
static std::mutex s_textureLoadSettingsMutex;
static TextureLoadSettings s_persistentTextureLoadSettings{};
thread_local bool s_hasScopedTextureLoadSettings = false;
thread_local TextureLoadSettings s_scopedTextureLoadSettings{};
struct TextureCacheEntry
{
    bgfx::TextureHandle Handle = BGFX_INVALID_HANDLE;
    std::string NormalizedResolvedPath;
};
static std::unordered_map<size_t, TextureCacheEntry> s_textureCache;
struct RetiredTextureHandle
{
    bgfx::TextureHandle Handle = BGFX_INVALID_HANDLE;
    uint64_t RetireEpoch = 0;
};
static std::vector<RetiredTextureHandle> s_retiredTextureHandles;
constexpr uint64_t kTextureRetireLatencyFrames = 3;

static void DestroyTextureHandle(bgfx::TextureHandle& handle)
{
    if (!bgfx::isValid(handle)) {
        return;
    }

    if (cm::rendering::IsBgfxActive()) {
        bgfx::destroy(handle);
    }
    handle = BGFX_INVALID_HANDLE;
}

static void RetireTextureHandle(bgfx::TextureHandle handle)
{
    if (!bgfx::isValid(handle)) {
        return;
    }

    RetiredTextureHandle retired;
    retired.Handle = handle;
    retired.RetireEpoch = s_textureFrameEpoch.load(std::memory_order_relaxed);
    s_retiredTextureHandles.push_back(retired);
}

static void PumpRetiredTextureHandles(bool forceDestroy)
{
    const uint64_t currentEpoch = s_textureFrameEpoch.load(std::memory_order_relaxed);
    size_t writeIndex = 0;
    for (size_t i = 0; i < s_retiredTextureHandles.size(); ++i) {
        RetiredTextureHandle retired = s_retiredTextureHandles[i];
        const bool destroyNow = forceDestroy
            || currentEpoch >= retired.RetireEpoch + kTextureRetireLatencyFrames;
        if (destroyNow) {
            DestroyTextureHandle(retired.Handle);
            continue;
        }

        s_retiredTextureHandles[writeIndex++] = retired;
    }
    s_retiredTextureHandles.resize(writeIndex);
}

static void ClearPropertyBlock(MaterialPropertyBlock& block)
{
    // NOTE: Do NOT destroy texture handles here.
    // Live materials and instances may still reference the same bgfx handle even
    // after the shared lookup cache entry is invalidated or refreshed.
    block.Clear();  // Clear all maps (both string and ID-based)
}

static std::string NormalizeTexturePath(const std::string& path)
{
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    return normalized;
}

static uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint64_t HashCombine64(uint64_t seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

static bool MaterialHasUniform(const Material& material, PropertyID id)
{
    glm::vec4 value(0.0f);
    return material.TryGetUniform(id, value);
}

static void CopyUniformIfPresent(const Material& source, Material& destination, PropertyID id)
{
    glm::vec4 value(0.0f);
    if (source.TryGetUniform(id, value)) {
        destination.SetUniform(id, value);
    }
}

static bool MaterialUsesPsxVariant(const Material& material)
{
    const PropertyID psxParamsId = PropertyID::Get("u_psxParams");
    const PropertyID psxWorldId = PropertyID::Get("u_psxWorld");
    const PropertyID toonParamsId = PropertyID::Get("u_toonParams");
    const PropertyID psxEmissionId = PropertyID::Get("u_psxEmission");

    return MaterialHasUniform(material, psxParamsId) ||
        MaterialHasUniform(material, psxWorldId) ||
        MaterialHasUniform(material, toonParamsId) ||
        MaterialHasUniform(material, psxEmissionId) ||
        material.GetName().find("PSX") != std::string::npos;
}

static void CopyPbrMaterialState(const PBRMaterial& source, PBRMaterial& destination)
{
    destination.SetMetallic(source.GetMetallic());
    destination.SetRoughness(source.GetRoughness());
    destination.SetNormalScale(source.GetNormalScale());
    destination.SetAmbientOcclusion(source.GetAmbientOcclusion());
    destination.SetEmissionStrength(source.GetEmissionStrength());
    destination.SetEmissionColor(source.GetEmissionColor());
    destination.SetDisplacementScale(source.GetDisplacementScale());
    destination.SetUVTransform(source.GetUVScale(), source.GetUVOffset());

    if (!source.GetAlbedoPath().empty()) {
        destination.SetAlbedoTextureFromPath(source.GetAlbedoPath());
    } else if (bgfx::isValid(source.m_AlbedoTex)) {
        destination.SetAlbedoTexture(source.m_AlbedoTex);
    }

    if (!source.GetMetallicRoughnessPath().empty()) {
        destination.SetMetallicRoughnessTextureFromPath(source.GetMetallicRoughnessPath());
    } else if (bgfx::isValid(source.m_MetallicRoughnessTex)) {
        destination.SetMetallicRoughnessTexture(source.m_MetallicRoughnessTex);
    }

    if (!source.GetNormalPath().empty()) {
        destination.SetNormalTextureFromPath(source.GetNormalPath());
    } else if (bgfx::isValid(source.m_NormalTex)) {
        destination.SetNormalTexture(source.m_NormalTex);
    }

    if (!source.GetAOPath().empty()) {
        destination.SetAmbientOcclusionTextureFromPath(source.GetAOPath());
    } else if (bgfx::isValid(source.m_AOTex)) {
        destination.SetAmbientOcclusionTexture(source.m_AOTex);
    }

    if (!source.GetEmissionPath().empty()) {
        destination.SetEmissionTextureFromPath(source.GetEmissionPath());
    } else if (bgfx::isValid(source.m_EmissionTex)) {
        destination.SetEmissionTexture(source.m_EmissionTex);
    }

    if (!source.GetDisplacementPath().empty()) {
        destination.SetDisplacementTextureFromPath(source.GetDisplacementPath());
    } else if (bgfx::isValid(source.m_DisplacementTex)) {
        destination.SetDisplacementTexture(source.m_DisplacementTex);
    }

    if (!source.GetTintMaskPath().empty()) {
        destination.SetTintMaskTextureFromPath(source.GetTintMaskPath());
    } else if (bgfx::isValid(source.m_TintMaskTex)) {
        destination.SetTintMaskTexture(source.m_TintMaskTex);
    }

    destination.SetReceiveShadowsOverride(source.GetReceiveShadowsOverride());
    destination.SetReceiveShadows(source.GetReceiveShadows());
}

} // namespace

TextureLoadSettings GetTextureLoadSettings()
{
    if (s_hasScopedTextureLoadSettings) {
        return s_scopedTextureLoadSettings;
    }

    std::lock_guard<std::mutex> lock(s_textureLoadSettingsMutex);
    return s_persistentTextureLoadSettings;
}

bool SetPersistentTextureLoadSettings(TextureLoadSettings settings)
{
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_textureLoadSettingsMutex);
        changed = s_persistentTextureLoadSettings.MaxDimension != settings.MaxDimension;
        s_persistentTextureLoadSettings = settings;
    }
    if (changed) {
        InvalidateAllTextureCaches();
    }
    return changed;
}

ScopedTextureLoadSettings::ScopedTextureLoadSettings(TextureLoadSettings settings)
{
    m_PreviousActive = s_hasScopedTextureLoadSettings;
    m_PreviousSettings = s_scopedTextureLoadSettings;
    s_scopedTextureLoadSettings = settings;
    s_hasScopedTextureLoadSettings = true;
    m_Active = true;
}

ScopedTextureLoadSettings::~ScopedTextureLoadSettings()
{
    if (!m_Active) {
        return;
    }
    s_hasScopedTextureLoadSettings = m_PreviousActive;
    s_scopedTextureLoadSettings = m_PreviousSettings;
}

bgfx::TextureHandle AcquireTextureHandle(const TextureSpecifier& spec, TextureColorSpace colorSpace)
{
    if (spec.Path.empty() && !spec.Embedded.HasData())
        return BGFX_INVALID_HANDLE;

    TextureSpecifier resolvedSpec = spec;
    if (!resolvedSpec.Path.empty()) {
        resolvedSpec.Path = TextureLoader::ResolveTexturePath(resolvedSpec.Path);
    }

    const size_t key = HashTextureSpecifier(resolvedSpec, colorSpace);
    const std::string normalizedResolvedPath = resolvedSpec.Path.empty()
        ? std::string()
        : NormalizeTexturePath(resolvedSpec.Path);
    {
        std::lock_guard<std::mutex> lock(s_textureMutex);
        auto it = s_textureCache.find(key);
        if (it != s_textureCache.end() && bgfx::isValid(it->second.Handle))
            return it->second.Handle;
    }

    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    if (resolvedSpec.Embedded.HasData())
    {
        if (resolvedSpec.Embedded.IsCompressed)
        {
            handle = TextureLoader::Load2DFromEncodedMemory(resolvedSpec.Embedded.Bytes.data(),
                static_cast<int>(resolvedSpec.Embedded.Bytes.size()),
                true, colorSpace, GetTextureLoadSettings().MaxDimension);
        }
        else
        {
            handle = TextureLoader::Load2DFromRGBA(resolvedSpec.Embedded.Bytes.data(),
                resolvedSpec.Embedded.Width,
                resolvedSpec.Embedded.Height,
                true, 0, colorSpace, GetTextureLoadSettings().MaxDimension);
        }
    }
    else if (!resolvedSpec.Path.empty())
    {
        handle = TextureLoader::Load2D(resolvedSpec.Path, true, colorSpace, GetTextureLoadSettings().MaxDimension);
    }

    if (bgfx::isValid(handle))
    {
        std::lock_guard<std::mutex> lock(s_textureMutex);
        auto it = s_textureCache.find(key);
        if (it != s_textureCache.end() && bgfx::isValid(it->second.Handle))
        {
            if (handle.idx != it->second.Handle.idx)
            {
                bgfx::destroy(handle);
            }
            return it->second.Handle;
        }

        TextureCacheEntry entry;
        entry.Handle = handle;
        entry.NormalizedResolvedPath = normalizedResolvedPath;
        s_textureCache[key] = entry;
    }
    return handle;
}

// Legacy overload for backward compatibility - defaults to Linear
bgfx::TextureHandle AcquireTextureHandle(const TextureSpecifier& spec)
{
    return AcquireTextureHandle(spec, TextureColorSpace::Linear);
}

void RefreshSceneTextureOverrides(Scene& scene)
{
    for (const auto& entity : scene.GetEntities()) {
        EntityData* data = scene.GetEntityData(entity.GetID());
        if (!data) {
            continue;
        }

        if (data->Mesh) {
            RebuildPropertyBlockTexturesFromPaths(
                data->Mesh->PropertyBlock,
                data->Mesh->PropertyBlockTexturePaths);
            const size_t slotCount = std::min(
                data->Mesh->SlotPropertyBlocks.size(),
                data->Mesh->SlotPropertyBlockTexturePaths.size());
            for (size_t slot = 0; slot < slotCount; ++slot) {
                RebuildPropertyBlockTexturesFromPaths(
                    data->Mesh->SlotPropertyBlocks[slot],
                    data->Mesh->SlotPropertyBlockTexturePaths[slot]);
            }
        }

        if (data->MeshProxy) {
            RebuildPropertyBlockTexturesFromPaths(
                data->MeshProxy->PropertyBlock,
                data->MeshProxy->PropertyBlockTexturePaths);
            const size_t slotCount = std::min(
                data->MeshProxy->SlotPropertyBlocks.size(),
                data->MeshProxy->SlotPropertyBlockTexturePaths.size());
            for (size_t slot = 0; slot < slotCount; ++slot) {
                RebuildPropertyBlockTexturesFromPaths(
                    data->MeshProxy->SlotPropertyBlocks[slot],
                    data->MeshProxy->SlotPropertyBlockTexturePaths[slot]);
            }
        }
    }
}

void RebuildPropertyBlockTexturesFromPaths(
    MaterialPropertyBlock& block,
    const std::unordered_map<std::string, std::string>& texturePaths,
    TextureColorSpace colorSpace)
{
    block.Textures.clear();
    block.TexturesByID.clear();
    block.MarkFlatOverrideCacheDirty();
    block.BumpMutationVersion();

    for (const auto& [name, rawPath] : texturePaths)
    {
        if (rawPath.empty()) {
            continue;
        }

        TextureSpecifier spec;
        spec.Path = rawPath;
        bgfx::TextureHandle handle = AcquireTextureHandle(spec, colorSpace);
        if (!bgfx::isValid(handle)) {
            continue;
        }

        block.SetTexture(name, handle);
    }
}

std::shared_ptr<Material> AcquireMaterialFromSource(const MaterialSource& source, Scene& scene)
{
    MaterialKey key;
    key.Skinned = source.Skinned;
    key.AlphaBlend = source.AlphaBlend;
    key.AlphaCutout = source.AlphaCutout;
    key.TwoSided = source.TwoSided;
    key.HasTint = source.HasTint;
    key.AlphaCutoutThreshold = source.AlphaCutoutThreshold;
    key.ColorTint = source.ColorTint;
    key.AlbedoHash = TextureSpecifierHasPayload(source.Albedo) ? HashTextureSpecifier(source.Albedo) : 0;
    key.MetallicRoughnessHash = TextureSpecifierHasPayload(source.MetallicRoughness) ? HashTextureSpecifier(source.MetallicRoughness) : 0;
    key.NormalHash = TextureSpecifierHasPayload(source.Normal) ? HashTextureSpecifier(source.Normal) : 0;
    key.AOHash = TextureSpecifierHasPayload(source.AO) ? HashTextureSpecifier(source.AO) : 0;
    key.EmissionHash = TextureSpecifierHasPayload(source.Emission) ? HashTextureSpecifier(source.Emission) : 0;
    key.DisplacementHash = TextureSpecifierHasPayload(source.Displacement) ? HashTextureSpecifier(source.Displacement) : 0;

    {
        std::lock_guard<std::mutex> lock(s_materialMutex);
        auto it = s_materialCache.find(key);
        if (it != s_materialCache.end())
        {
            if (auto existing = it->second.lock())
            {
                return existing;
            }
        }
    }

    std::shared_ptr<Material> material;
    if (source.Skinned)
        material = MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&scene);
    else
        material = MaterialManager::Instance().CreateSceneDefaultMaterial(&scene);

    if (source.AlphaBlend)
        material->m_StateFlags |= BGFX_STATE_BLEND_ALPHA;
    if (source.TwoSided)
        material->m_StateFlags &= ~(BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW);

    if (auto* pbr = dynamic_cast<PBRMaterial*>(material.get()))
    {
        pbr->SetUniform("u_ColorTint", glm::vec4(1.0f));
        if (source.AlphaCutout)
        {
            material->m_StateFlags &= ~BGFX_STATE_BLEND_ALPHA;
            pbr->SetUniform("u_PBRScalar1", glm::vec4(pbr->GetEmissionStrength(), source.AlphaCutoutThreshold, 0.0f, 0.0f));
        }
    }

    {
        std::lock_guard<std::mutex> lock(s_materialMutex);
        s_materialCache[key] = material;
    }
    return material;
}

void PopulatePropertyBlockFromSource(const MaterialSource& source,
    MaterialPropertyBlock& block,
    std::unordered_map<std::string, std::string>& texturePaths)
{
    ClearPropertyBlock(block);
    texturePaths.clear();

    // Helper to assign a texture with the appropriate color space
    auto assignTexture = [&](const char* sampler, const TextureSpecifier& spec, TextureColorSpace colorSpace)
    {
        if (!TextureSpecifierHasPayload(spec)) return;
        bgfx::TextureHandle handle = AcquireTextureHandle(spec, colorSpace);
        if (!bgfx::isValid(handle)) return;
        block.SetTexture(std::string(sampler), handle);  // Use SetTexture to sync both maps
        if (!spec.Path.empty())
            texturePaths[sampler] = spec.Path;
    };

    // Keep color textures in linear loading path for current project assets.
    assignTexture("s_albedo", source.Albedo, TextureColorSpace::Linear);
    assignTexture("s_emission", source.Emission, TextureColorSpace::Linear);
    assignTexture("s_metallicRoughness", source.MetallicRoughness, TextureColorSpace::Linear);
    assignTexture("s_normalMap", source.Normal, TextureColorSpace::Linear);
    assignTexture("s_ao", source.AO, TextureColorSpace::Linear);
    assignTexture("s_displacement", source.Displacement, TextureColorSpace::Linear);

    if (source.HasTint)
    {
        block.SetVector("u_ColorTint", source.ColorTint);  // Use SetVector to sync both maps
    }
}

MaterialSource CaptureMaterialSource(const std::shared_ptr<Material>& material)
{
    MaterialSource source;
    if (!material) return source;

    source.Skinned = (bool)std::dynamic_pointer_cast<SkinnedPBRMaterial>(material);
    uint64_t flags = material->GetStateFlags();
    source.AlphaBlend = (flags & BGFX_STATE_BLEND_ALPHA) != 0;
    source.TwoSided = (flags & (BGFX_STATE_CULL_CW | BGFX_STATE_CULL_CCW)) == 0;

    if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(material))
    {
        glm::vec4 tint(1.0f);
        if (pbr->TryGetUniform("u_ColorTint", tint) && tint != glm::vec4(1.0f))
        {
            source.HasTint = true;
        }
        source.ColorTint = tint;
        source.Albedo.Path = pbr->GetAlbedoPath();
        source.MetallicRoughness.Path = pbr->GetMetallicRoughnessPath();
        source.Normal.Path = pbr->GetNormalPath();
        source.AO.Path = pbr->GetAOPath();
        source.Emission.Path = pbr->GetEmissionPath();
        source.Displacement.Path = pbr->GetDisplacementPath();
    }
    return source;
}

bool MaterialSourceHasData(const MaterialSource& source)
{
    if (source.Skinned || source.AlphaBlend || source.AlphaCutout || source.TwoSided || source.HasTint)
        return true;
    return TextureSpecifierHasPayload(source.Albedo) ||
           TextureSpecifierHasPayload(source.MetallicRoughness) ||
           TextureSpecifierHasPayload(source.Normal) ||
           TextureSpecifierHasPayload(source.AO) ||
           TextureSpecifierHasPayload(source.Emission) ||
           TextureSpecifierHasPayload(source.Displacement);
}

MaterialEquivalenceKeyInfo GetMaterialEquivalenceKey(const Material* material)
{
    MaterialEquivalenceKeyInfo info{};
    if (!material) {
        return info;
    }

    info.Key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material));
    if (dynamic_cast<const MaterialInstance*>(material) != nullptr) {
        return info;
    }

    const PropertyID colorTintId = PropertyID::Get("u_ColorTint");
    const PropertyID tintParamsId = PropertyID::Get("u_TintParams");
    const PropertyID psxParamsId = PropertyID::Get("u_psxParams");
    const PropertyID psxWorldId = PropertyID::Get("u_psxWorld");
    const PropertyID toonParamsId = PropertyID::Get("u_toonParams");
    const PropertyID posterizeId = PropertyID::Get("u_posterize");
    const PropertyID psxShadowParamsId = PropertyID::Get("u_psxShadowParams");
    const PropertyID psxEmissionId = PropertyID::Get("u_psxEmission");

    if (const auto* pbr = dynamic_cast<const PBRMaterial*>(material)) {
        uint64_t h = 1469598103934665603ULL;
        h = HashCombine64(h, static_cast<uint64_t>(material->GetProgram().idx));
        h = HashCombine64(h, static_cast<uint64_t>(material->GetStateFlags()));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_AlbedoTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_MetallicRoughnessTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_NormalTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_AOTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_EmissionTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_DisplacementTex.idx));
        h = HashCombine64(h, static_cast<uint64_t>(pbr->m_TintMaskTex.idx));
        h = HashCombine64(h, FloatBits(pbr->GetMetallic()));
        h = HashCombine64(h, FloatBits(pbr->GetRoughness()));
        h = HashCombine64(h, FloatBits(pbr->GetNormalScale()));
        h = HashCombine64(h, FloatBits(pbr->GetAmbientOcclusion()));
        h = HashCombine64(h, FloatBits(pbr->GetEmissionStrength()));
        h = HashCombine64(h, FloatBits(pbr->GetDisplacementScale()));
        h = HashCombine64(h, pbr->GetReceiveShadowsOverride() ? 1ULL : 0ULL);
        h = HashCombine64(h, pbr->GetReceiveShadows() ? 1ULL : 0ULL);

        const glm::vec3 emissionColor = pbr->GetEmissionColor();
        h = HashCombine64(h, FloatBits(emissionColor.x));
        h = HashCombine64(h, FloatBits(emissionColor.y));
        h = HashCombine64(h, FloatBits(emissionColor.z));

        const glm::vec2 uvScale = pbr->GetUVScale();
        const glm::vec2 uvOffset = pbr->GetUVOffset();
        h = HashCombine64(h, FloatBits(uvScale.x));
        h = HashCombine64(h, FloatBits(uvScale.y));
        h = HashCombine64(h, FloatBits(uvOffset.x));
        h = HashCombine64(h, FloatBits(uvOffset.y));

        auto hashUniform = [&](PropertyID id) {
            glm::vec4 value(0.0f);
            if (!material->TryGetUniform(id, value)) {
                return;
            }
            h = HashCombine64(h, FloatBits(value.x));
            h = HashCombine64(h, FloatBits(value.y));
            h = HashCombine64(h, FloatBits(value.z));
            h = HashCombine64(h, FloatBits(value.w));
        };

        hashUniform(colorTintId);
        hashUniform(tintParamsId);
        hashUniform(psxParamsId);
        hashUniform(psxWorldId);
        hashUniform(toonParamsId);
        hashUniform(posterizeId);
        hashUniform(psxShadowParamsId);
        hashUniform(psxEmissionId);

        info.Key = h;
        info.EquivalentSafe = true;
        return info;
    }

    if (const auto* shaderGraph = dynamic_cast<const cm::RuntimeShaderGraphMaterial*>(material)) {
        uint64_t h = 1469598103934665603ULL;
        h = HashCombine64(h, static_cast<uint64_t>(material->GetProgram().idx));
        h = HashCombine64(h, static_cast<uint64_t>(material->GetStateFlags()));
        h = HashCombine64(h, std::hash<std::string>{}(shaderGraph->GetShaderGraphPath()));

        const glm::vec2 uvScale = shaderGraph->GetUVScale();
        const glm::vec2 uvOffset = shaderGraph->GetUVOffset();
        h = HashCombine64(h, FloatBits(uvScale.x));
        h = HashCombine64(h, FloatBits(uvScale.y));
        h = HashCombine64(h, FloatBits(uvOffset.x));
        h = HashCombine64(h, FloatBits(uvOffset.y));

        const auto& parameters = shaderGraph->GetParameters();
        h = HashCombine64(h, static_cast<uint64_t>(parameters.size()));
        for (const auto& parameter : parameters) {
            h = HashCombine64(h, std::hash<std::string>{}(parameter.name));
            h = HashCombine64(h, static_cast<uint64_t>(parameter.type));
            h = HashCombine64(h, FloatBits(parameter.value.x));
            h = HashCombine64(h, FloatBits(parameter.value.y));
            h = HashCombine64(h, FloatBits(parameter.value.z));
            h = HashCombine64(h, FloatBits(parameter.value.w));
            h = HashCombine64(h, std::hash<std::string>{}(parameter.texturePath));
            h = HashCombine64(h, static_cast<uint64_t>(parameter.textureHandle.idx));
            h = HashCombine64(h, static_cast<uint64_t>(parameter.textureSlot + 1));
        }

        info.Key = h;
        info.EquivalentSafe = true;
        return info;
    }

    return info;
}

std::shared_ptr<Material> AcquireEquivalentMaterial(const std::shared_ptr<Material>& material)
{
    if (!material) {
        return nullptr;
    }

    const MaterialEquivalenceKeyInfo info = GetMaterialEquivalenceKey(material.get());
    if (!info.EquivalentSafe || info.Key == 0) {
        return material;
    }

    std::lock_guard<std::mutex> lock(s_equivalentMaterialMutex);
    auto it = s_equivalentMaterialCache.find(info.Key);
    if (it != s_equivalentMaterialCache.end()) {
        if (auto existing = it->second.lock()) {
            const MaterialEquivalenceKeyInfo existingInfo =
                GetMaterialEquivalenceKey(existing.get());
            if (existingInfo.EquivalentSafe && existingInfo.Key == info.Key) {
                return existing;
            }
        }
        s_equivalentMaterialCache.erase(it);
    }

    s_equivalentMaterialCache[info.Key] = material;
    return material;
}

bool MaterialNeedsSkinnedVariant(const std::shared_ptr<Material>& material)
{
    if (!material) {
        return true;
    }

    if (auto instance = std::dynamic_pointer_cast<MaterialInstance>(material)) {
        return MaterialNeedsSkinnedVariant(instance->GetParent());
    }

    if (std::dynamic_pointer_cast<SkinnedPBRMaterial>(material)) {
        return false;
    }

    return material->GetName().find("Skinned") == std::string::npos;
}

std::shared_ptr<Material> AcquireSkinnedMaterialVariant(Scene& scene, const std::shared_ptr<Material>& material)
{
    if (auto instance = std::dynamic_pointer_cast<MaterialInstance>(material)) {
        std::shared_ptr<Material> parentVariant =
            AcquireSkinnedMaterialVariant(scene, instance->GetParent());
        if (!parentVariant) {
            return nullptr;
        }

        if (!MaterialNeedsSkinnedVariant(material) &&
            parentVariant == instance->GetParent()) {
            return material;
        }

        auto variantInstance = MaterialInstance::Create(parentVariant);
        if (!variantInstance) {
            return parentVariant;
        }

        variantInstance->m_StateFlags = material->GetStateFlags();
        variantInstance->CopyOverridesFrom(*instance);
        return variantInstance;
    }

    if (!material) {
        return AcquireEquivalentMaterial(
            MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&scene));
    }

    if (!MaterialNeedsSkinnedVariant(material)) {
        return AcquireEquivalentMaterial(material);
    }

    std::shared_ptr<Material> variant =
        MaterialUsesPsxVariant(*material)
            ? MaterialManager::Instance().CreateSkinnedPSXMaterial()
            : MaterialManager::Instance().CreateSkinnedPBRMaterial();
    if (!variant) {
        variant = MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&scene);
    }
    if (!variant) {
        return material;
    }

    variant->m_StateFlags = material->GetStateFlags();

    const PropertyID colorTintId = PropertyID::Get("u_ColorTint");
    const PropertyID tintParamsId = PropertyID::Get("u_TintParams");
    const PropertyID psxParamsId = PropertyID::Get("u_psxParams");
    const PropertyID psxWorldId = PropertyID::Get("u_psxWorld");
    const PropertyID toonParamsId = PropertyID::Get("u_toonParams");
    const PropertyID posterizeId = PropertyID::Get("u_posterize");
    const PropertyID psxShadowParamsId = PropertyID::Get("u_psxShadowParams");
    const PropertyID psxEmissionId = PropertyID::Get("u_psxEmission");
    CopyUniformIfPresent(*material, *variant, colorTintId);
    CopyUniformIfPresent(*material, *variant, tintParamsId);
    CopyUniformIfPresent(*material, *variant, psxParamsId);
    CopyUniformIfPresent(*material, *variant, psxWorldId);
    CopyUniformIfPresent(*material, *variant, toonParamsId);
    CopyUniformIfPresent(*material, *variant, posterizeId);
    CopyUniformIfPresent(*material, *variant, psxShadowParamsId);
    CopyUniformIfPresent(*material, *variant, psxEmissionId);

    if (auto sourcePbr = std::dynamic_pointer_cast<PBRMaterial>(material)) {
        if (auto destinationPbr = std::dynamic_pointer_cast<PBRMaterial>(variant)) {
            CopyPbrMaterialState(*sourcePbr, *destinationPbr);
        }
    }

    return AcquireEquivalentMaterial(variant);
}

void InvalidateTextureCache(const std::string& path)
{
    if (path.empty()) return;

    const std::string normalizedPath = NormalizeTexturePath(path);
    const std::string normalizedResolvedPath = NormalizeTexturePath(TextureLoader::ResolveTexturePath(path));
    size_t removed = 0;
    std::vector<bgfx::TextureHandle> retiredHandles;
    std::unordered_set<uint16_t> retiredHandleIndices;

    {
        std::lock_guard<std::mutex> lock(s_textureMutex);
        for (auto it = s_textureCache.begin(); it != s_textureCache.end(); )
        {
            const std::string& cachedPath = it->second.NormalizedResolvedPath;
            const bool matchesPath = !cachedPath.empty() &&
                (cachedPath == normalizedPath || cachedPath == normalizedResolvedPath);
            if (matchesPath)
            {
                if (bgfx::isValid(it->second.Handle) && retiredHandleIndices.insert(it->second.Handle.idx).second) {
                    retiredHandles.push_back(it->second.Handle);
                }
                it = s_textureCache.erase(it);
                ++removed;
                continue;
            }
            ++it;
        }

        for (bgfx::TextureHandle handle : retiredHandles) {
            RetireTextureHandle(handle);
        }
    }

    if (removed > 0)
    {
        s_textureCacheGeneration.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[MaterialCache] Invalidated " << removed
                  << " cached texture entr"
                  << (removed == 1 ? "y" : "ies")
                  << " for: " << normalizedResolvedPath << std::endl;
    }
}

void InvalidateAllTextureCaches()
{
    {
        std::lock_guard<std::mutex> lock(s_textureMutex);
        std::unordered_set<uint16_t> retiredHandleIndices;
        for (const auto& [key, entry] : s_textureCache) {
            (void)key;
            if (bgfx::isValid(entry.Handle) && retiredHandleIndices.insert(entry.Handle.idx).second) {
                RetireTextureHandle(entry.Handle);
            }
        }
        s_textureCache.clear();
    }
    s_textureCacheGeneration.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[MaterialCache] Invalidated all texture caches" << std::endl;
}

uint64_t GetTextureCacheGeneration()
{
    return s_textureCacheGeneration.load(std::memory_order_relaxed);
}

void AdvanceTextureCacheFrame()
{
    s_textureFrameEpoch.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(s_textureMutex);
    PumpRetiredTextureHandles(false);
}

void FlushRetiredTextureHandles()
{
    std::lock_guard<std::mutex> lock(s_textureMutex);
    PumpRetiredTextureHandles(true);
}

