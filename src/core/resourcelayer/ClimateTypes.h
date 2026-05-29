#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// ClimateValues - Sampled climate state at a world position
//------------------------------------------------------------------------------
struct ClimateValues {
    float Temperature = 20.0f;   // Degrees Celsius (-40 to +50 typical range)
    float Moisture = 0.5f;       // 0.0 (arid) to 1.0 (saturated)
    float WindExposure = 0.5f;   // 0.0 (sheltered) to 1.0 (exposed)
    
    // Interpolation
    static ClimateValues Lerp(const ClimateValues& a, const ClimateValues& b, float t) {
        ClimateValues result;
        result.Temperature = glm::mix(a.Temperature, b.Temperature, t);
        result.Moisture = glm::mix(a.Moisture, b.Moisture, t);
        result.WindExposure = glm::mix(a.WindExposure, b.WindExposure, t);
        return result;
    }
};

//------------------------------------------------------------------------------
// ClimateGradient - Defines climate variation along an axis
//------------------------------------------------------------------------------
struct ClimateGradient {
    enum class Axis {
        Vertical,       // Altitude (Y world coordinate)
        Longitudinal    // Latitude/position (Z or configurable axis)
    };
    
    struct ControlPoint {
        float Position = 0.0f;      // Normalized 0-1 position along axis
        float Temperature = 20.0f;  // Degrees Celsius at this point
        float Moisture = 0.5f;      // 0.0 - 1.0
        float WindExposure = 0.5f;  // 0.0 - 1.0
    };
    
    Axis axis = Axis::Vertical;
    std::vector<ControlPoint> Points;
    
    // Sample the gradient at a normalized position (0-1)
    ClimateValues Sample(float normalizedPos) const;
    
    // Add a control point, maintaining sorted order
    void AddPoint(const ControlPoint& point);
    
    // Remove control point at index
    void RemovePoint(size_t index);
    
    // Get default gradient presets
    static ClimateGradient DefaultVertical();      // Warm lowlands → cold peaks
    static ClimateGradient DefaultLongitudinal();  // Tropical → temperate → arctic
};

//------------------------------------------------------------------------------
// ClimateConfig - Complete climate configuration for a terrain
//------------------------------------------------------------------------------
struct ClimateConfig {
    //--------------------------------------------------------------------------
    // World-space bounds for normalizing positions to 0-1 range
    //--------------------------------------------------------------------------
    float MinAltitude = 0.0f;       // Sea level (Y coordinate)
    float MaxAltitude = 2000.0f;    // Mountain peaks (Y coordinate)
    float MinLongitude = 0.0f;      // Southern edge of world (Z coordinate)
    float MaxLongitude = 4096.0f;   // Northern edge of world (Z coordinate)
    
    //--------------------------------------------------------------------------
    // Gradient definitions
    //--------------------------------------------------------------------------
    ClimateGradient VerticalGradient;
    ClimateGradient LongitudinalGradient;
    
    //--------------------------------------------------------------------------
    // Blending mode for combining gradients
    //--------------------------------------------------------------------------
    enum class BlendMode {
        Multiply,   // Multiply normalized values (tends toward lower values)
        Average,    // Simple average of both gradients
        Min,        // Take minimum value from either gradient
        Max         // Take maximum value from either gradient
    };
    
    BlendMode TemperatureBlend = BlendMode::Average;
    BlendMode MoistureBlend = BlendMode::Multiply;
    BlendMode WindBlend = BlendMode::Max;
    
    //--------------------------------------------------------------------------
    // Noise modulation (optional micro-variation)
    //--------------------------------------------------------------------------
    bool EnableNoiseModulation = false;
    float NoiseScale = 100.0f;          // World-space frequency
    float TemperatureNoiseAmplitude = 2.0f;  // ±degrees C
    float MoistureNoiseAmplitude = 0.1f;     // ±0-1 range
    int NoiseSeed = 0;
    
    //--------------------------------------------------------------------------
    // Methods
    //--------------------------------------------------------------------------
    
    // Sample combined climate at a world position
    ClimateValues SampleAt(const glm::vec3& worldPos) const;
    
    // Normalize world position to 0-1 range for gradient sampling
    float NormalizeAltitude(float worldY) const;
    float NormalizeLongitude(float worldZ) const;
    
    // Apply preset configurations
    void ApplyPreset(const std::string& presetName);
    
    // Get list of available presets
    static std::vector<std::string> GetPresetNames();
    
    // Create with default values
    static ClimateConfig Default();
    
    // Common presets
    static ClimateConfig Temperate();       // Mild climate, four seasons
    static ClimateConfig Tropical();        // Hot and humid
    static ClimateConfig Arctic();          // Cold and dry
    static ClimateConfig Continental();     // Tropical south to arctic north
    static ClimateConfig Mediterranean();   // Warm, dry summers
};

//------------------------------------------------------------------------------
// Blending helper functions
//------------------------------------------------------------------------------
inline float BlendValue(float a, float b, ClimateConfig::BlendMode mode) {
    switch (mode) {
        case ClimateConfig::BlendMode::Multiply:
            // Normalize to 0-1 range for multiplication, then scale back
            return a * b / 20.0f;  // Assuming typical temperature range
        case ClimateConfig::BlendMode::Average:
            return (a + b) * 0.5f;
        case ClimateConfig::BlendMode::Min:
            return std::min(a, b);
        case ClimateConfig::BlendMode::Max:
            return std::max(a, b);
        default:
            return a;
    }
}

inline float BlendValueNormalized(float a, float b, ClimateConfig::BlendMode mode) {
    switch (mode) {
        case ClimateConfig::BlendMode::Multiply:
            return a * b;
        case ClimateConfig::BlendMode::Average:
            return (a + b) * 0.5f;
        case ClimateConfig::BlendMode::Min:
            return std::min(a, b);
        case ClimateConfig::BlendMode::Max:
            return std::max(a, b);
        default:
            return a;
    }
}

} // namespace resourcelayer
} // namespace cm


