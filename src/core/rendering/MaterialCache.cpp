#include "MaterialCache.h"
#include "MaterialManager.h"
#include "PBRMaterial.h"
#include "SkinnedPBRMaterial.h"
#include "TextureLoader.h"
#include "core/ecs/Scene.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <string_view>

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
static std::mutex s_textureMutex;
static std::unordered_map<MaterialKey, std::weak_ptr<Material>, MaterialKeyHasher> s_materialCache;
static std::atomic<uint64_t> s_textureCacheGeneration{1};
struct TextureCacheEntry
{
    bgfx::TextureHandle Handle = BGFX_INVALID_HANDLE;
    std::string NormalizedResolvedPath;
};
static std::unordered_map<size_t, TextureCacheEntry> s_textureCache;

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

} // namespace

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
                true, colorSpace);
        }
        else
        {
            handle = TextureLoader::Load2DFromRGBA(resolvedSpec.Embedded.Bytes.data(),
                resolvedSpec.Embedded.Width,
                resolvedSpec.Embedded.Height,
                true, 0, colorSpace);
        }
    }
    else if (!resolvedSpec.Path.empty())
    {
        handle = TextureLoader::Load2D(resolvedSpec.Path, true, colorSpace);
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

void InvalidateTextureCache(const std::string& path)
{
    if (path.empty()) return;

    const std::string normalizedPath = NormalizeTexturePath(path);
    const std::string normalizedResolvedPath = NormalizeTexturePath(TextureLoader::ResolveTexturePath(path));
    size_t removed = 0;

    {
        std::lock_guard<std::mutex> lock(s_textureMutex);
        for (auto it = s_textureCache.begin(); it != s_textureCache.end(); )
        {
            const std::string& cachedPath = it->second.NormalizedResolvedPath;
            const bool matchesPath = !cachedPath.empty() &&
                (cachedPath == normalizedPath || cachedPath == normalizedResolvedPath);
            if (matchesPath)
            {
                it = s_textureCache.erase(it);
                ++removed;
                continue;
            }
            ++it;
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
        s_textureCache.clear();
    }
    s_textureCacheGeneration.fetch_add(1, std::memory_order_relaxed);
    std::cout << "[MaterialCache] Invalidated all texture caches" << std::endl;
}

uint64_t GetTextureCacheGeneration()
{
    return s_textureCacheGeneration.load(std::memory_order_relaxed);
}

