#include "ClimateTypes.h"
#include <algorithm>
#include <cmath>

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// ClimateGradient Implementation
//------------------------------------------------------------------------------

ClimateValues ClimateGradient::Sample(float normalizedPos) const {
    if (Points.empty()) {
        return ClimateValues{};
    }

    
    normalizedPos = glm::clamp(normalizedPos, 0.0f, 1.0f);
    
    // Find surrounding control points
    size_t lower = 0;
    size_t upper = 0;
    
    for (size_t i = 0; i < Points.size(); ++i) {
        if (Points[i].Position <= normalizedPos) {
            lower = i;
        }
        if (Points[i].Position >= normalizedPos && upper == 0) {
            upper = i;
        }
    }
    
    if (upper == 0) upper = Points.size() - 1;
    if (lower == upper) {
        // Exact match or single point
        return ClimateValues{
            Points[lower].Temperature,
            Points[lower].Moisture,
            Points[lower].WindExposure
        };
    }
    
    // Interpolate between points
    float range = Points[upper].Position - Points[lower].Position;
    float t = (range > 0.0f) ? (normalizedPos - Points[lower].Position) / range : 0.0f;
    
    return ClimateValues{
        glm::mix(Points[lower].Temperature, Points[upper].Temperature, t),
        glm::mix(Points[lower].Moisture, Points[upper].Moisture, t),
        glm::mix(Points[lower].WindExposure, Points[upper].WindExposure, t)
    };
}

void ClimateGradient::AddPoint(const ControlPoint& point) {
    // Insert maintaining sorted order by position
    auto it = std::lower_bound(Points.begin(), Points.end(), point,
        [](const ControlPoint& a, const ControlPoint& b) {
            return a.Position < b.Position;
        });
    Points.insert(it, point);
}

void ClimateGradient::RemovePoint(size_t index) {
    if (index < Points.size()) {
        Points.erase(Points.begin() + index);
    }
}

ClimateGradient ClimateGradient::DefaultVertical() {
    ClimateGradient g;
    g.axis = Axis::Vertical;
    g.Points = {
        { 0.0f,  20.0f, 0.7f, 0.3f },   // Sea level: warm, moist, sheltered
        { 0.25f, 15.0f, 0.8f, 0.4f },   // Lowlands: mild, humid
        { 0.5f,  10.0f, 0.6f, 0.6f },   // Mid-altitude: cool
        { 0.75f,  2.0f, 0.4f, 0.8f },   // High altitude: cold, dry, windy
        { 1.0f, -15.0f, 0.2f, 1.0f },   // Peaks: freezing, dry, very windy
    };
    return g;
}

ClimateGradient ClimateGradient::DefaultLongitudinal() {
    ClimateGradient g;
    g.axis = Axis::Longitudinal;
    g.Points = {
        { 0.0f,  30.0f, 0.9f, 0.4f },   // Tropical (south)
        { 0.25f, 25.0f, 0.7f, 0.5f },   // Subtropical
        { 0.5f,  15.0f, 0.5f, 0.5f },   // Temperate
        { 0.75f,  5.0f, 0.4f, 0.7f },   // Subarctic
        { 1.0f, -20.0f, 0.2f, 0.9f },   // Arctic (north)
    };
    return g;
}

//------------------------------------------------------------------------------
// ClimateConfig Implementation
//------------------------------------------------------------------------------

ClimateValues ClimateConfig::SampleAt(const glm::vec3& worldPos) const {
    // Sample both gradients
    float altNorm = NormalizeAltitude(worldPos.y);
    float lonNorm = NormalizeLongitude(worldPos.z);
    
    ClimateValues vertical = VerticalGradient.Sample(altNorm);
    ClimateValues longitudinal = LongitudinalGradient.Sample(lonNorm);
    
    // Blend based on configured modes
    ClimateValues result;
    
    // Temperature blending
    switch (TemperatureBlend) {
        case BlendMode::Multiply:
            // Normalize temps to 0-1 range (-40 to 50 = 90 range)
            result.Temperature = ((vertical.Temperature + 40.0f) / 90.0f) * 
                                 ((longitudinal.Temperature + 40.0f) / 90.0f) * 90.0f - 40.0f;
            break;
        case BlendMode::Average:
            result.Temperature = (vertical.Temperature + longitudinal.Temperature) * 0.5f;
            break;
        case BlendMode::Min:
            result.Temperature = std::min(vertical.Temperature, longitudinal.Temperature);
            break;
        case BlendMode::Max:
            result.Temperature = std::max(vertical.Temperature, longitudinal.Temperature);
            break;
    }
    
    // Moisture blending (already normalized 0-1)
    result.Moisture = BlendValueNormalized(vertical.Moisture, longitudinal.Moisture, MoistureBlend);
    
    // Wind blending
    result.WindExposure = BlendValueNormalized(vertical.WindExposure, longitudinal.WindExposure, WindBlend);
    
    // Apply noise modulation if enabled
    if (EnableNoiseModulation) {
        // TODO: Add simplex noise modulation
        // float noise = SimplexNoise2D(worldPos.x / NoiseScale, worldPos.z / NoiseScale, NoiseSeed);
        // result.Temperature += noise * TemperatureNoiseAmplitude;
        // result.Moisture = glm::clamp(result.Moisture + noise * MoistureNoiseAmplitude, 0.0f, 1.0f);
    }
    
    return result;
}

float ClimateConfig::NormalizeAltitude(float worldY) const {
    if (MaxAltitude <= MinAltitude) return 0.0f;
    return glm::clamp((worldY - MinAltitude) / (MaxAltitude - MinAltitude), 0.0f, 1.0f);
}

float ClimateConfig::NormalizeLongitude(float worldZ) const {
    if (MaxLongitude <= MinLongitude) return 0.0f;
    return glm::clamp((worldZ - MinLongitude) / (MaxLongitude - MinLongitude), 0.0f, 1.0f);
}

void ClimateConfig::ApplyPreset(const std::string& presetName) {
    if (presetName == "Temperate") *this = Temperate();
    else if (presetName == "Tropical") *this = Tropical();
    else if (presetName == "Arctic") *this = Arctic();
    else if (presetName == "Continental") *this = Continental();
    else if (presetName == "Mediterranean") *this = Mediterranean();
    else *this = Default();
}

std::vector<std::string> ClimateConfig::GetPresetNames() {
    return { "Default", "Temperate", "Tropical", "Arctic", "Continental", "Mediterranean" };
}

ClimateConfig ClimateConfig::Default() {
    ClimateConfig cfg;
    cfg.VerticalGradient = ClimateGradient::DefaultVertical();
    cfg.LongitudinalGradient = ClimateGradient::DefaultLongitudinal();
    return cfg;
}

ClimateConfig ClimateConfig::Temperate() {
    ClimateConfig cfg = Default();
    cfg.VerticalGradient.Points = {
        { 0.0f,  18.0f, 0.65f, 0.3f },
        { 0.3f,  14.0f, 0.70f, 0.4f },
        { 0.6f,   8.0f, 0.55f, 0.6f },
        { 1.0f,  -5.0f, 0.35f, 0.9f },
    };
    cfg.LongitudinalGradient.Points = {
        { 0.0f, 20.0f, 0.6f, 0.4f },
        { 0.5f, 15.0f, 0.5f, 0.5f },
        { 1.0f, 10.0f, 0.4f, 0.6f },
    };
    return cfg;
}

ClimateConfig ClimateConfig::Tropical() {
    ClimateConfig cfg = Default();
    cfg.VerticalGradient.Points = {
        { 0.0f,  32.0f, 0.95f, 0.2f },
        { 0.3f,  28.0f, 0.90f, 0.3f },
        { 0.6f,  22.0f, 0.80f, 0.5f },
        { 1.0f,  12.0f, 0.60f, 0.8f },
    };
    cfg.LongitudinalGradient.Points = {
        { 0.0f, 32.0f, 0.9f, 0.3f },
        { 1.0f, 30.0f, 0.85f, 0.4f },
    };
    return cfg;
}

ClimateConfig ClimateConfig::Arctic() {
    ClimateConfig cfg = Default();
    cfg.VerticalGradient.Points = {
        { 0.0f,  -5.0f, 0.40f, 0.7f },
        { 0.3f, -10.0f, 0.35f, 0.8f },
        { 0.6f, -20.0f, 0.25f, 0.9f },
        { 1.0f, -35.0f, 0.15f, 1.0f },
    };
    cfg.LongitudinalGradient.Points = {
        { 0.0f, -5.0f, 0.4f, 0.7f },
        { 1.0f, -25.0f, 0.2f, 0.95f },
    };
    return cfg;
}

ClimateConfig ClimateConfig::Continental() {
    ClimateConfig cfg = Default();
    // Uses default gradients which span tropical to arctic
    return cfg;
}

ClimateConfig ClimateConfig::Mediterranean() {
    ClimateConfig cfg = Default();
    cfg.VerticalGradient.Points = {
        { 0.0f,  26.0f, 0.30f, 0.4f },  // Hot, dry coast
        { 0.3f,  22.0f, 0.40f, 0.5f },
        { 0.6f,  16.0f, 0.50f, 0.6f },
        { 1.0f,   8.0f, 0.60f, 0.8f },  // Cooler, wetter mountains
    };
    cfg.LongitudinalGradient.Points = {
        { 0.0f, 28.0f, 0.2f, 0.3f },
        { 0.5f, 22.0f, 0.35f, 0.4f },
        { 1.0f, 18.0f, 0.5f, 0.5f },
    };
    return cfg;
}

} // namespace resourcelayer
} // namespace cm


