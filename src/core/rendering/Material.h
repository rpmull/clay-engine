#pragma once
#include <bgfx/bgfx.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <glm/glm.hpp>
#include "PropertyID.h"
#include "BgfxLifecycle.h"

class Material
{
public: 
    Material(const std::string& name,
             bgfx::ProgramHandle program, 
             uint64_t stateFlags = BGFX_STATE_DEFAULT)
        : m_Name(name), m_Program(program), m_StateFlags(stateFlags) {}

    // Prevent copying - materials own bgfx handles that can't be safely copied
    Material(const Material&) = delete;
    Material& operator=(const Material&) = delete;
    
    // Allow move semantics
    Material(Material&& other) noexcept
        : m_Name(std::move(other.m_Name))
        , m_Program(other.m_Program)
        , m_StateFlags(other.m_StateFlags)
        , m_UniformIndexByID(std::move(other.m_UniformIndexByID))
        , m_UniformsFlat(std::move(other.m_UniformsFlat))
        , m_Uniforms(std::move(other.m_Uniforms))
        , m_SamplerHandlesByID(std::move(other.m_SamplerHandlesByID))
    {
        // Invalidate moved-from object's handles so destructor doesn't destroy them
        other.m_UniformsFlat.clear();
        other.m_Uniforms.clear();
        other.m_SamplerHandlesByID.clear();
    }
    
    Material& operator=(Material&& other) noexcept {
        if (this != &other) {
            // Destroy our existing handles
            for (auto& uniform : m_UniformsFlat) {
                cm::rendering::SafeDestroyHandle(uniform.handle);
            }
            // Move from other
            m_Name = std::move(other.m_Name);
            m_Program = other.m_Program;
            m_StateFlags = other.m_StateFlags;
            m_UniformIndexByID = std::move(other.m_UniformIndexByID);
            m_UniformsFlat = std::move(other.m_UniformsFlat);
            m_Uniforms = std::move(other.m_Uniforms);
            for (auto& sampler : m_SamplerHandlesByID) {
                cm::rendering::SafeDestroyHandle(sampler);
            }
            m_SamplerHandlesByID = std::move(other.m_SamplerHandlesByID);
            // Invalidate moved-from
            other.m_UniformsFlat.clear();
            other.m_Uniforms.clear();
            other.m_SamplerHandlesByID.clear();
        }
        return *this;
    }

    // Polymorphic base must have a virtual destructor to ensure
    // correct cleanup when deleted through a base pointer.
    virtual ~Material() {
        // Destroy uniform handles to prevent resource leaks
        for (auto& uniform : m_UniformsFlat) {
            cm::rendering::SafeDestroyHandle(uniform.handle);
        }
        for (auto& sampler : m_SamplerHandlesByID) {
            cm::rendering::SafeDestroyHandle(sampler);
        }
    }

    // Builds the display name for a clone. Idempotent: repeated cloning keeps a
    // single "_Clone" suffix instead of accumulating "_Clone_Clone_..." names
    // (which previously broke name-based material detection and bloated scenes).
    static std::string MakeCloneName(const std::string& name) {
        static const std::string kSuffix = "_Clone";
        if (name.size() >= kSuffix.size() &&
            name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
            return name;
        }
        return name + kSuffix;
    }

    // Create a deep copy of this material with fresh bgfx handles
    virtual std::shared_ptr<Material> Clone() const {
        auto clone = std::make_shared<Material>(MakeCloneName(m_Name), m_Program, m_StateFlags);
        // Copy uniform values (clone will create fresh handles when SetUniform is called)
        CopyUniformValuesTo(*clone);
        return clone;
    }

    // === Fast PropertyID-based API (preferred) ===
    void SetUniform(PropertyID id, const glm::vec4& value);
    bool TryGetUniform(PropertyID id, glm::vec4& outValue) const;
    bgfx::UniformHandle GetUniformHandle(PropertyID id) const;
    
    // === Legacy string-based API (backward compatible) ===
    void SetUniform(const std::string& name, const glm::vec4& value);
    bool TryGetUniform(const std::string& name, glm::vec4& outValue) const;
    
    virtual void BindUniforms() const;

    struct PackedPropertyOverrides {
        struct PackedVec4Override {
            bgfx::UniformHandle Handle = BGFX_INVALID_HANDLE;
            glm::vec4 Value = glm::vec4(0.0f);
        };

        struct PackedTextureOverride {
            bgfx::UniformHandle Sampler = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle Texture = BGFX_INVALID_HANDLE;
            uint8_t Stage = 0;
        };

        std::vector<PackedVec4Override> Vec4Overrides;
        std::vector<PackedTextureOverride> TextureOverrides;
        bgfx::UniformHandle TextureUsageHandle = BGFX_INVALID_HANDLE;
        glm::vec4 TextureUsageValue = glm::vec4(0.0f);
        bool TextureUsageDirty = false;

        void Clear() {
            Vec4Overrides.clear();
            TextureOverrides.clear();
            TextureUsageHandle = BGFX_INVALID_HANDLE;
            TextureUsageValue = glm::vec4(0.0f);
            TextureUsageDirty = false;
        }

        [[nodiscard]] bool Empty() const {
            return Vec4Overrides.empty() &&
                   TextureOverrides.empty() &&
                   !TextureUsageDirty;
        }
    };

    // Apply per-instance overrides before draw (fast path using PropertyIDs)
    void ApplyPropertyBlock(const struct MaterialPropertyBlock& block) const;
    
    // Apply property block using fast ID-based lookups
    void ApplyPropertyBlockFast(const struct MaterialPropertyBlock& block) const;
    void BuildPackedPropertyOverrides(const struct MaterialPropertyBlock& block,
                                      PackedPropertyOverrides& out) const;
    void ApplyPackedPropertyOverrides(const PackedPropertyOverrides& packed) const;
    bgfx::UniformHandle GetOrCreateSamplerHandle(PropertyID id) const;

    bgfx::ProgramHandle GetProgram() const { return m_Program; }
    uint64_t GetStateFlags() const { return m_StateFlags; }
    std::string GetName() const { return m_Name; }
    bool IsBindCacheSafe() const { return m_BindCacheSafe; }
    void SetBindCacheSafe(bool enabled) { m_BindCacheSafe = enabled; }

    uint64_t m_StateFlags;

protected:
    // Allow derived classes to access uniform data
    struct UniformData {
        bgfx::UniformHandle handle;
        glm::vec4 value;
        PropertyID propertyId;
    };
    
    // Fast lookup by PropertyID value (sparse array, resized as needed)
    mutable std::vector<int> m_UniformIndexByID;  // PropertyID.Value() -> index in m_UniformsFlat, -1 if not present
    std::vector<UniformData> m_UniformsFlat;      // Flat array for cache-friendly iteration
    
    // Legacy string-based map (for backward compatibility)
    std::unordered_map<std::string, UniformData> m_Uniforms;
    
    // Copies every generic vec4 uniform value to another material. Derived
    // Clone() implementations must call this so uniforms that live outside the
    // derived class's typed fields (e.g. u_psxParams/u_toonParams/u_psxEmission
    // on PSX materials, u_ColorTint/u_TintParams on all PBR-family materials)
    // survive cloning. Losing them silently converted PSX clones into plain PBR
    // materials, which broke per-entity overrides and serialization.
    void CopyUniformValuesTo(Material& destination) const {
        for (const auto& uniform : m_UniformsFlat) {
            destination.SetUniform(uniform.propertyId, uniform.value);
        }
    }

    // Helper to ensure ID lookup array is large enough
    void EnsureUniformIndexCapacity(uint32_t id) const {
        if (id >= m_UniformIndexByID.size()) {
            m_UniformIndexByID.resize(id + 16, -1);  // Grow with padding
        }
    }

    void EnsureSamplerIndexCapacity(uint32_t id) const {
        if (id >= m_SamplerHandlesByID.size()) {
            m_SamplerHandlesByID.resize(id + 16, BGFX_INVALID_HANDLE);
        }
    }

private:  
    std::string m_Name;
    bgfx::ProgramHandle m_Program;
    bool m_BindCacheSafe = true;
    mutable std::vector<bgfx::UniformHandle> m_SamplerHandlesByID;
};
