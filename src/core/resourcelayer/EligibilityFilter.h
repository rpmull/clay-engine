#pragma once

#include "ClimateTypes.h"
#include "core/assets/AssetReference.h"
#include <glm/glm.hpp>
#include <bgfx/bgfx.h>
#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <vector>
#include <string>

// Forward declarations
struct TerrainComponent;

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// RegionPolygon - A closed polygon defining a named region
//------------------------------------------------------------------------------
struct RegionPolygon {
    ClaymoreGUID Guid = ClaymoreGUID::Generate();
    std::string Name = "Region";
    std::vector<glm::vec2> Points;  // XZ coordinates
    glm::vec3 DebugColor = {0.2f, 0.6f, 0.9f};
    
    // Cached for fast queries
    glm::vec2 BoundsMin = {0, 0};
    glm::vec2 BoundsMax = {0, 0};
    
    bool ContainsPoint(const glm::vec2& xz) const;
    float DistanceToBoundary(const glm::vec2& xz) const;
    void UpdateBounds();
    float CalculateArea() const;
};

//------------------------------------------------------------------------------
// EligibilityContext - Shared context passed to filters
//------------------------------------------------------------------------------
struct EligibilityContext {
    // Required references
    const TerrainComponent* Terrain = nullptr;
    glm::mat4 TerrainTransform = glm::mat4(1.0f);
    
    // Climate configuration
    const ClimateConfig* Climate = nullptr;
    
    // Optional: Road/path splines for distance calculations
    const std::vector<std::vector<glm::vec3>>* Roads = nullptr;
    
    // Optional: Named regions for mask filtering
    const std::vector<RegionPolygon>* Regions = nullptr;
    
    // Sun direction for light exposure (normalized)
    glm::vec3 SunDirection = glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f));
    
    //--------------------------------------------------------------------------
    // Cached values (computed lazily, mutable for const methods)
    //--------------------------------------------------------------------------
    mutable glm::vec3 LastSampledPosition = {0, 0, 0};
    mutable float Height = 0.0f;
    mutable float SlopeDegrees = 0.0f;
    mutable glm::vec3 Normal = {0, 1, 0};
    mutable bool TerrainSampled = false;
    
    mutable ClimateValues ClimateVals;
    mutable bool ClimateSampled = false;
    
    mutable float NearestRoadDistance = -1.0f;
    mutable bool RoadDistanceSampled = false;
    
    //--------------------------------------------------------------------------
    // Lazy sampling methods
    //--------------------------------------------------------------------------
    void SampleTerrain(const glm::vec3& pos) const;
    void SampleClimate(const glm::vec3& pos) const;
    void SampleRoadDistance(const glm::vec3& pos) const;
    
    // Invalidate cache (call when sampling a new position)
    void InvalidateCache() const;
};

//------------------------------------------------------------------------------
// IEligibilityFilter - Base interface for all eligibility filters
//------------------------------------------------------------------------------
class IEligibilityFilter {
public:
    virtual ~IEligibilityFilter() = default;
    
    //--------------------------------------------------------------------------
    // Core evaluation - returns 0.0 (rejected) to 1.0 (fully eligible)
    //--------------------------------------------------------------------------
    virtual float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const = 0;
    
    //--------------------------------------------------------------------------
    // GPU acceleration (optional)
    //--------------------------------------------------------------------------
    virtual bool SupportsBaking() const { return false; }
    virtual void BakeToTexture(
        bgfx::TextureHandle target, 
        uint32_t width, uint32_t height,
        const glm::vec2& worldMin,
        const glm::vec2& worldMax,
        const EligibilityContext& ctx) const {}
    
    //--------------------------------------------------------------------------
    // Serialization
    //--------------------------------------------------------------------------
    virtual void Serialize(nlohmann::json& j) const = 0;
    virtual void Deserialize(const nlohmann::json& j) = 0;
    
    //--------------------------------------------------------------------------
    // Editor UI
    //--------------------------------------------------------------------------
    virtual const char* GetTypeName() const = 0;
    virtual const char* GetDescription() const { return ""; }
    virtual void DrawInspector() {}  // ImGui controls
    
    //--------------------------------------------------------------------------
    // Factory for creating filters by type name
    //--------------------------------------------------------------------------
    static std::unique_ptr<IEligibilityFilter> Create(const std::string& typeName);
    static std::vector<std::string> GetRegisteredTypes();
};

//------------------------------------------------------------------------------
// SlopeFilter - Filter based on terrain steepness
//------------------------------------------------------------------------------
class SlopeFilter : public IEligibilityFilter {
public:
    float MinSlopeDegrees = 0.0f;
    float MaxSlopeDegrees = 45.0f;
    float FalloffDegrees = 5.0f;  // Soft transition width
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Slope"; }
    const char* GetDescription() const override { 
        return "Filter by terrain steepness (degrees from horizontal)"; 
    }
    void DrawInspector() override;
    
    bool SupportsBaking() const override { return true; }
};

//------------------------------------------------------------------------------
// AltitudeFilter - Filter based on world height
//------------------------------------------------------------------------------
class AltitudeFilter : public IEligibilityFilter {
public:
    float MinAltitude = -1000.0f;
    float MaxAltitude = 10000.0f;
    float FalloffMeters = 10.0f;
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Altitude"; }
    const char* GetDescription() const override { 
        return "Filter by terrain altitude (Y world coordinate)"; 
    }
    void DrawInspector() override;
    
    bool SupportsBaking() const override { return true; }
};

//------------------------------------------------------------------------------
// LightExposureFilter - Filter based on surface orientation to sun
//------------------------------------------------------------------------------
class LightExposureFilter : public IEligibilityFilter {
public:
    float MinExposure = 0.0f;    // 0 = fully shadowed
    float MaxExposure = 1.0f;    // 1 = direct sunlight
    float FalloffRange = 0.1f;
    bool UseContextSunDirection = true;  // Use sun from context vs custom
    glm::vec3 CustomSunDirection = glm::normalize(glm::vec3(0.5f, 1.0f, 0.3f));
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Light Exposure"; }
    const char* GetDescription() const override { 
        return "Filter by how directly the terrain faces the sun"; 
    }
    void DrawInspector() override;
    
    bool SupportsBaking() const override { return true; }
};

//------------------------------------------------------------------------------
// RoadDistanceFilter - Filter based on distance from roads/paths
//------------------------------------------------------------------------------
class RoadDistanceFilter : public IEligibilityFilter {
public:
    float MinDistance = 0.0f;       // Don't spawn closer than this
    float MaxDistance = 100.0f;     // Don't spawn farther than this (0 = infinite)
    float FalloffDistance = 10.0f;
    bool InvertForNearRoad = false; // Higher eligibility near roads
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Road Distance"; }
    const char* GetDescription() const override { 
        return "Filter by distance from roads and paths"; 
    }
    void DrawInspector() override;
};

//------------------------------------------------------------------------------
// RegionMaskFilter - Filter based on painted/defined regions
//------------------------------------------------------------------------------
class RegionMaskFilter : public IEligibilityFilter {
public:
    ClaymoreGUID RegionGuid;        // Which region to match
    std::string RegionName;         // Display name (for UI)
    bool InvertMask = false;        // Spawn outside region instead of inside
    float EdgeFalloff = 2.0f;       // Soft edge in meters
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Region Mask"; }
    const char* GetDescription() const override { 
        return "Filter by whether position is inside a named region"; 
    }
    void DrawInspector() override;
};

//------------------------------------------------------------------------------
// ClimateFilter - Filter based on temperature/moisture from climate system
//------------------------------------------------------------------------------
class ClimateFilter : public IEligibilityFilter {
public:
    float MinTemperature = -40.0f;
    float MaxTemperature = 50.0f;
    float TemperatureFalloff = 5.0f;
    
    float MinMoisture = 0.0f;
    float MaxMoisture = 1.0f;
    float MoistureFalloff = 0.1f;
    
    float MinWindExposure = 0.0f;
    float MaxWindExposure = 1.0f;
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Climate"; }
    const char* GetDescription() const override { 
        return "Filter by climate values (temperature, moisture, wind)"; 
    }
    void DrawInspector() override;
};

//------------------------------------------------------------------------------
// NoiseFilter - Procedural noise-based variation
//------------------------------------------------------------------------------
class NoiseFilter : public IEligibilityFilter {
public:
    float Scale = 50.0f;            // World-space frequency (larger = bigger patches)
    float Threshold = 0.5f;         // Noise below this = rejected
    float Falloff = 0.1f;           // Soft transition width
    int Octaves = 3;
    float Persistence = 0.5f;
    float Lacunarity = 2.0f;
    int Seed = 0;
    bool UseWorldPosition = true;   // Use XZ vs XYZ for noise input
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Noise"; }
    const char* GetDescription() const override { 
        return "Procedural noise for natural-looking variation"; 
    }
    void DrawInspector() override;
    
    bool SupportsBaking() const override { return true; }
};

//------------------------------------------------------------------------------
// SplatMaskFilter - Filter based on terrain splat/paint channels
//------------------------------------------------------------------------------
class SplatMaskFilter : public IEligibilityFilter {
public:
    enum class Channel { R, G, B, A };
    
    Channel MaskChannel = Channel::R;
    float MinValue = 0.5f;
    float MaxValue = 1.0f;
    float Falloff = 0.1f;
    bool Invert = false;
    
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const override;
    
    void Serialize(nlohmann::json& j) const override;
    void Deserialize(const nlohmann::json& j) override;
    
    const char* GetTypeName() const override { return "Splat Mask"; }
    const char* GetDescription() const override { 
        return "Filter by terrain paint channel values"; 
    }
    void DrawInspector() override;
    
    bool SupportsBaking() const override { return true; }
};

//------------------------------------------------------------------------------
// CompositeEligibilityMap - Combines multiple filters
//------------------------------------------------------------------------------
class CompositeEligibilityMap {
public:
    enum class CombineMode {
        Multiply,   // All filters must pass (AND-like)
        Min,        // Most restrictive wins
        Max,        // Most permissive wins
        Average     // Blend all filters equally
    };
    
    struct FilterEntry {
        std::unique_ptr<IEligibilityFilter> Filter;
        float Weight = 1.0f;
        bool Enabled = true;
    };
    
    std::vector<FilterEntry> Filters;
    CombineMode Mode = CombineMode::Multiply;
    
    //--------------------------------------------------------------------------
    // Evaluation
    //--------------------------------------------------------------------------
    float Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const;
    
    //--------------------------------------------------------------------------
    // Filter management
    //--------------------------------------------------------------------------
    void AddFilter(std::unique_ptr<IEligibilityFilter> filter, float weight = 1.0f);
    void RemoveFilter(size_t index);
    void MoveFilter(size_t fromIndex, size_t toIndex);
    void ClearFilters();
    
    //--------------------------------------------------------------------------
    // Serialization
    //--------------------------------------------------------------------------
    void Serialize(nlohmann::json& j) const;
    void Deserialize(const nlohmann::json& j);
    
    //--------------------------------------------------------------------------
    // Baking (for GPU preview)
    //--------------------------------------------------------------------------
    bool CanBake() const;
    void BakeToTexture(
        bgfx::TextureHandle target,
        uint32_t width, uint32_t height,
        const glm::vec2& worldMin,
        const glm::vec2& worldMax,
        const EligibilityContext& ctx) const;
};

//------------------------------------------------------------------------------
// Helper functions
//------------------------------------------------------------------------------

// Distance from point to line segment
float DistanceToLineSegment(
    const glm::vec3& point, 
    const glm::vec3& lineA, 
    const glm::vec3& lineB);

// Fractal noise (for NoiseFilter)
float FractalNoise(float x, float z, int octaves, float persistence, float lacunarity, int seed);

// Simplex noise 2D
float SimplexNoise2D(float x, float y, int seed);

} // namespace resourcelayer
} // namespace cm


