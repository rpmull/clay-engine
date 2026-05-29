#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <bgfx/bgfx.h>
#include "PropertyID.h"

/**
 * MaterialPropertyBlock - Lightweight per-renderer override collection
 * 
 * Supports both PropertyID-based access (fast, preferred) and string-based
 * access (backward compatible, slower).
 * 
 * For best performance, use PropertyID-based methods:
 *   block.SetVector(CommonProperties::ColorTint, tintColor);
 *   block.SetTexture(CommonProperties::Albedo, albedoTex);
 * 
 * String-based methods are provided for backward compatibility and serialization.
 */
struct MaterialPropertyBlock {
    struct Vec4OverrideEntry {
        uint32_t PropertyId = 0;
        glm::vec4 Value = glm::vec4(0.0f);
    };

    struct TextureOverrideEntry {
        uint32_t PropertyId = 0;
        bgfx::TextureHandle Texture = BGFX_INVALID_HANDLE;
    };

    // Fast PropertyID-based storage
    std::unordered_map<uint32_t, glm::vec4> Vec4ByID;
    std::unordered_map<uint32_t, bgfx::TextureHandle> TexturesByID;
    
    // Legacy string-based storage (for serialization compatibility)
    // These are synced with ID-based storage
    std::unordered_map<std::string, glm::vec4> Vec4Uniforms;
    std::unordered_map<std::string, bgfx::TextureHandle> Textures;

    // Flat caches used by the render hot path. These are rebuilt lazily on
    // mutation so per-draw iteration stays cache-friendly and avoids walking
    // unordered_map buckets in the submission path.
    mutable std::vector<Vec4OverrideEntry> Vec4OverridesFlat;
    mutable std::vector<TextureOverrideEntry> TextureOverridesFlat;
    mutable bool FlatOverrideCacheDirty = true;
    uint64_t MutationVersion = 1;

    // === Fast PropertyID-based API (preferred) ===

    void MarkFlatOverrideCacheDirty() const {
        FlatOverrideCacheDirty = true;
    }

    void BumpMutationVersion() {
        ++MutationVersion;
    }

    [[nodiscard]] uint64_t GetMutationVersion() const {
        return MutationVersion;
    }

    void RebuildFlatOverrideCaches() const {
        if (!FlatOverrideCacheDirty &&
            Vec4OverridesFlat.size() == Vec4ByID.size() &&
            TextureOverridesFlat.size() == TexturesByID.size()) {
            return;
        }

        Vec4OverridesFlat.clear();
        TextureOverridesFlat.clear();

        if (!Vec4ByID.empty()) {
            Vec4OverridesFlat.reserve(Vec4ByID.size());
            for (const auto& kv : Vec4ByID) {
                Vec4OverridesFlat.push_back({ kv.first, kv.second });
            }
        }

        if (!TexturesByID.empty()) {
            TextureOverridesFlat.reserve(TexturesByID.size());
            for (const auto& kv : TexturesByID) {
                TextureOverridesFlat.push_back({ kv.first, kv.second });
            }
        }

        FlatOverrideCacheDirty = false;
    }

    const std::vector<Vec4OverrideEntry>& GetVec4OverridesFlat() const {
        RebuildFlatOverrideCaches();
        return Vec4OverridesFlat;
    }

    const std::vector<TextureOverrideEntry>& GetTextureOverridesFlat() const {
        RebuildFlatOverrideCaches();
        return TextureOverridesFlat;
    }
    
    void SetVector(PropertyID id, const glm::vec4& value) {
        Vec4ByID[id.Value()] = value;
        // Also update string map for serialization
        const std::string& name = PropertyID::GetName(id);
        if (!name.empty()) Vec4Uniforms[name] = value;
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    void SetTexture(PropertyID id, bgfx::TextureHandle texture) {
        TexturesByID[id.Value()] = texture;
        // Also update string map for serialization
        const std::string& name = PropertyID::GetName(id);
        if (!name.empty()) Textures[name] = texture;
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    bool TryGetVector(PropertyID id, glm::vec4& outValue) const {
        auto it = Vec4ByID.find(id.Value());
        if (it == Vec4ByID.end()) return false;
        outValue = it->second;
        return true;
    }
    
    bool TryGetTexture(PropertyID id, bgfx::TextureHandle& outTex) const {
        auto it = TexturesByID.find(id.Value());
        if (it == TexturesByID.end()) return false;
        outTex = it->second;
        return true;
    }
    
    bool HasVector(PropertyID id) const {
        return Vec4ByID.find(id.Value()) != Vec4ByID.end();
    }
    
    bool HasTexture(PropertyID id) const {
        return TexturesByID.find(id.Value()) != TexturesByID.end();
    }
    
    void RemoveVector(PropertyID id) {
        Vec4ByID.erase(id.Value());
        const std::string& name = PropertyID::GetName(id);
        if (!name.empty()) Vec4Uniforms.erase(name);
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    void RemoveTexture(PropertyID id) {
        TexturesByID.erase(id.Value());
        const std::string& name = PropertyID::GetName(id);
        if (!name.empty()) Textures.erase(name);
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    // === Legacy string-based API (backward compatible) ===
    // Note: These are slower due to string operations
    
    void SetVector(const std::string& name, const glm::vec4& value) {
        Vec4Uniforms[name] = value;
        PropertyID id = PropertyID::Get(name);
        Vec4ByID[id.Value()] = value;
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    void SetTexture(const std::string& name, bgfx::TextureHandle texture) {
        Textures[name] = texture;
        PropertyID id = PropertyID::Get(name);
        TexturesByID[id.Value()] = texture;
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    // String-based removal (syncs both maps)
    void RemoveVector(const std::string& name) {
        Vec4Uniforms.erase(name);
        PropertyID id = PropertyID::Get(name);
        Vec4ByID.erase(id.Value());
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
    
    void RemoveTexture(const std::string& name) {
        Textures.erase(name);
        PropertyID id = PropertyID::Get(name);
        TexturesByID.erase(id.Value());
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }

    bool Empty() const { 
        return Vec4ByID.empty() && TexturesByID.empty() && 
               Vec4Uniforms.empty() && Textures.empty(); 
    }
    
    bool EmptyFast() const {
        return Vec4ByID.empty() && TexturesByID.empty();
    }

    void Clear() {
        Vec4ByID.clear();
        TexturesByID.clear();
        Vec4Uniforms.clear();
        Textures.clear();
        Vec4OverridesFlat.clear();
        TextureOverridesFlat.clear();
        FlatOverrideCacheDirty = false;
        BumpMutationVersion();
    }
    
    // Sync ID-based maps from string maps (call after deserializing string maps)
    void SyncFromStringMaps() {
        Vec4ByID.clear();
        TexturesByID.clear();
        for (const auto& kv : Vec4Uniforms) {
            PropertyID id = PropertyID::Get(kv.first);
            Vec4ByID[id.Value()] = kv.second;
        }
        for (const auto& kv : Textures) {
            PropertyID id = PropertyID::Get(kv.first);
            TexturesByID[id.Value()] = kv.second;
        }
        MarkFlatOverrideCacheDirty();
        BumpMutationVersion();
    }
};

struct MaterialPropertyBlockStack
{
    const MaterialPropertyBlock* MeshBlock = nullptr;
    const MaterialPropertyBlock* SlotBlock = nullptr;
    const MaterialPropertyBlock* ProxyBlock = nullptr;

    bool Empty() const
    {
        return (!MeshBlock || MeshBlock->Empty()) &&
               (!SlotBlock || SlotBlock->Empty()) &&
               (!ProxyBlock || ProxyBlock->Empty());
    }
};
