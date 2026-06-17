#pragma once

#include "core/rendering/MaterialSource.h"
#include "core/rendering/Material.h"
#include "core/rendering/TextureLoader.h"
#include "MaterialPropertyBlock.h"
#include <cstdint>
#include <memory>
#include <unordered_map>

class Scene;

struct MaterialEquivalenceKeyInfo {
    uint64_t Key = 0;
    bool EquivalentSafe = false;
};

struct TextureLoadSettings {
    uint32_t MaxDimension = 0;
};

class ScopedTextureLoadSettings {
public:
    explicit ScopedTextureLoadSettings(TextureLoadSettings settings);
    ~ScopedTextureLoadSettings();

    ScopedTextureLoadSettings(const ScopedTextureLoadSettings&) = delete;
    ScopedTextureLoadSettings& operator=(const ScopedTextureLoadSettings&) = delete;

private:
    bool m_Active = false;
    bool m_PreviousActive = false;
    TextureLoadSettings m_PreviousSettings{};
};

std::shared_ptr<Material> AcquireMaterialFromSource(const MaterialSource& source, Scene& scene);
bgfx::TextureHandle AcquireTextureHandle(const TextureSpecifier& spec, TextureColorSpace colorSpace);
bgfx::TextureHandle AcquireTextureHandle(const TextureSpecifier& spec); // Legacy overload, defaults to Linear
void RefreshSceneTextureOverrides(Scene& scene);
void RebuildPropertyBlockTexturesFromPaths(
    MaterialPropertyBlock& block,
    const std::unordered_map<std::string, std::string>& texturePaths,
    TextureColorSpace colorSpace = TextureColorSpace::Linear);
void PopulatePropertyBlockFromSource(const MaterialSource& source,
    MaterialPropertyBlock& block,
    std::unordered_map<std::string, std::string>& texturePaths);
MaterialSource CaptureMaterialSource(const std::shared_ptr<Material>& material);
bool MaterialSourceHasData(const MaterialSource& source);
MaterialEquivalenceKeyInfo GetMaterialEquivalenceKey(const Material* material);
std::shared_ptr<Material> AcquireEquivalentMaterial(const std::shared_ptr<Material>& material);
bool MaterialNeedsSkinnedVariant(const std::shared_ptr<Material>& material);
std::shared_ptr<Material> AcquireSkinnedMaterialVariant(Scene& scene, const std::shared_ptr<Material>& material);

/// @brief Invalidate cached texture handles for a specific path.
/// Call this when a texture file is modified to force reload on next access.
/// @param path The texture file path (will be normalized internally)
void InvalidateTextureCache(const std::string& path);

/// @brief Invalidate all cached texture handles.
/// Use sparingly - prefer InvalidateTextureCache(path) for targeted invalidation.
void InvalidateAllTextureCaches();

/// @brief Monotonic generation that changes whenever texture caches are invalidated.
/// Materials can use this to refresh path-backed handles only when needed.
uint64_t GetTextureCacheGeneration();

TextureLoadSettings GetTextureLoadSettings();
bool SetPersistentTextureLoadSettings(TextureLoadSettings settings);
void AdvanceTextureCacheFrame();
void FlushRetiredTextureHandles();

