#include "EligibilityFilter.h"
#include "core/ecs/Components.h"
#include "core/rendering/Terrain.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <imgui.h>

namespace cm {
namespace resourcelayer {

//------------------------------------------------------------------------------
// Helper Functions
//------------------------------------------------------------------------------

float DistanceToLineSegment(
    const glm::vec3& point,
    const glm::vec3& lineA,
    const glm::vec3& lineB)
{
    glm::vec3 ab = lineB - lineA;
    glm::vec3 ap = point - lineA;
    
    float abLen2 = glm::dot(ab, ab);
    if (abLen2 < 0.0001f) {
        return glm::length(ap);
    }
    
    float t = glm::clamp(glm::dot(ap, ab) / abLen2, 0.0f, 1.0f);
    glm::vec3 closest = lineA + t * ab;
    return glm::length(point - closest);
}

// Permutation table for simplex noise
static const int perm[512] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
    8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
    35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
    134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
    55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
    18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
    250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
    189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
    172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
    228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
    107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
    138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180,
    // Repeat
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
    8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
    35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
    134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
    55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
    18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
    250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
    189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
    172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
    228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
    107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
    138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180
};

static float Grad2D(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

float SimplexNoise2D(float x, float y, int seed) {
    // Add seed offset
    x += seed * 0.31f;
    y += seed * 0.17f;
    
    const float F2 = 0.366025403f; // (sqrt(3) - 1) / 2
    const float G2 = 0.211324865f; // (3 - sqrt(3)) / 6
    
    float s = (x + y) * F2;
    int i = (int)std::floor(x + s);
    int j = (int)std::floor(y + s);
    
    float t = (i + j) * G2;
    float X0 = i - t;
    float Y0 = j - t;
    float x0 = x - X0;
    float y0 = y - Y0;
    
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    else { i1 = 0; j1 = 1; }
    
    float x1 = x0 - i1 + G2;
    float y1 = y0 - j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;
    
    int ii = i & 255;
    int jj = j & 255;
    
    float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;
    
    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 >= 0.0f) {
        t0 *= t0;
        n0 = t0 * t0 * Grad2D(perm[ii + perm[jj]], x0, y0);
    }
    
    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 >= 0.0f) {
        t1 *= t1;
        n1 = t1 * t1 * Grad2D(perm[ii + i1 + perm[jj + j1]], x1, y1);
    }
    
    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 >= 0.0f) {
        t2 *= t2;
        n2 = t2 * t2 * Grad2D(perm[ii + 1 + perm[jj + 1]], x2, y2);
    }
    
    return 70.0f * (n0 + n1 + n2);
}

float FractalNoise(float x, float z, int octaves, float persistence, float lacunarity, int seed) {
    float total = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float maxValue = 0.0f;
    
    for (int i = 0; i < octaves; ++i) {
        total += SimplexNoise2D(x * frequency, z * frequency, seed + i * 1000) * amplitude;
        maxValue += amplitude;
        amplitude *= persistence;
        frequency *= lacunarity;
    }
    
    return total / maxValue;
}

//------------------------------------------------------------------------------
// RegionPolygon Implementation
//------------------------------------------------------------------------------

bool RegionPolygon::ContainsPoint(const glm::vec2& xz) const {
    // Quick AABB rejection
    if (xz.x < BoundsMin.x || xz.x > BoundsMax.x ||
        xz.y < BoundsMin.y || xz.y > BoundsMax.y) {
        return false;
    }
    
    // Ray casting algorithm
    bool inside = false;
    size_t n = Points.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const glm::vec2& pi = Points[i];
        const glm::vec2& pj = Points[j];
        
        if (((pi.y > xz.y) != (pj.y > xz.y)) &&
            (xz.x < (pj.x - pi.x) * (xz.y - pi.y) / (pj.y - pi.y) + pi.x)) {
            inside = !inside;
        }
    }
    return inside;
}

float RegionPolygon::DistanceToBoundary(const glm::vec2& xz) const {
    float minDist = std::numeric_limits<float>::max();
    size_t n = Points.size();
    
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        glm::vec3 p3(xz.x, 0, xz.y);
        glm::vec3 a(Points[i].x, 0, Points[i].y);
        glm::vec3 b(Points[j].x, 0, Points[j].y);
        float d = DistanceToLineSegment(p3, a, b);
        minDist = std::min(minDist, d);
    }
    
    return minDist;
}

void RegionPolygon::UpdateBounds() {
    if (Points.empty()) {
        BoundsMin = BoundsMax = glm::vec2(0);
        return;
    }
    
    BoundsMin = BoundsMax = Points[0];
    for (const auto& p : Points) {
        BoundsMin = glm::min(BoundsMin, p);
        BoundsMax = glm::max(BoundsMax, p);
    }
}

float RegionPolygon::CalculateArea() const {
    if (Points.size() < 3) return 0.0f;
    
    float area = 0.0f;
    size_t n = Points.size();
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        area += Points[i].x * Points[j].y;
        area -= Points[j].x * Points[i].y;
    }
    return std::abs(area) * 0.5f;
}

//------------------------------------------------------------------------------
// EligibilityContext Implementation
//------------------------------------------------------------------------------

void EligibilityContext::SampleTerrain(const glm::vec3& pos) const {
    if (TerrainSampled && glm::distance2(pos, LastSampledPosition) < 0.01f) {
        return; // Already sampled at this position
    }
    
    LastSampledPosition = pos;
    TerrainSampled = true;
    
    if (!Terrain) {
        Height = pos.y;
        SlopeDegrees = 0.0f;
        Normal = glm::vec3(0, 1, 0);
        return;
    }
    
    // Transform world position to terrain local space
    glm::mat4 invTransform = glm::inverse(TerrainTransform);
    glm::vec4 localPos = invTransform * glm::vec4(pos.x, 0.0f, pos.z, 1.0f);
    if (localPos.x < 0.0f || localPos.x > Terrain->WorldSize.x ||
        localPos.z < 0.0f || localPos.z > Terrain->WorldSize.y ||
        Terrain::IsHoleAtLocal(*Terrain, localPos.x, localPos.z)) {
        Height = pos.y;
        SlopeDegrees = 90.0f;
        Normal = glm::vec3(0.0f, 1.0f, 0.0f);
        return;
    }
    
    // Sample height
    Height = Terrain::SampleHeightWorld(*Terrain, localPos.x, localPos.z);
    glm::vec4 worldPos = TerrainTransform * glm::vec4(localPos.x, Height, localPos.z, 1.0f);
    Height = worldPos.y;
    
    // Sample normal
    Normal = Terrain::SampleNormal(*Terrain, localPos.x, localPos.z);
    
    // Calculate slope from normal
    SlopeDegrees = glm::degrees(std::acos(glm::clamp(Normal.y, 0.0f, 1.0f)));
}

void EligibilityContext::SampleClimate(const glm::vec3& pos) const {
    if (ClimateSampled && glm::distance2(pos, LastSampledPosition) < 0.01f) {
        return;
    }
    
    ClimateSampled = true;
    
    if (!Climate) {
        ClimateVals = ClimateValues{};
        return;
    }
    
    ClimateVals = Climate->SampleAt(pos);
}

void EligibilityContext::SampleRoadDistance(const glm::vec3& pos) const {
    if (RoadDistanceSampled && glm::distance2(pos, LastSampledPosition) < 0.01f) {
        return;
    }
    
    RoadDistanceSampled = true;
    
    if (!Roads || Roads->empty()) {
        NearestRoadDistance = std::numeric_limits<float>::max();
        return;
    }
    
    float minDist = std::numeric_limits<float>::max();
    for (const auto& road : *Roads) {
        for (size_t i = 0; i + 1 < road.size(); ++i) {
            float d = DistanceToLineSegment(pos, road[i], road[i + 1]);
            minDist = std::min(minDist, d);
        }
    }
    
    NearestRoadDistance = minDist;
}

void EligibilityContext::InvalidateCache() const {
    TerrainSampled = false;
    ClimateSampled = false;
    RoadDistanceSampled = false;
}

//------------------------------------------------------------------------------
// SlopeFilter Implementation
//------------------------------------------------------------------------------

float SlopeFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    ctx.SampleTerrain(worldPos);
    float slope = ctx.SlopeDegrees;
    
    // Hard rejection outside extended range
    if (slope < MinSlopeDegrees - FalloffDegrees || slope > MaxSlopeDegrees + FalloffDegrees) {
        return 0.0f;
    }
    
    // Full eligibility within core range
    if (slope >= MinSlopeDegrees && slope <= MaxSlopeDegrees) {
        return 1.0f;
    }
    
    // Soft falloff at edges
    if (slope < MinSlopeDegrees) {
        return (slope - (MinSlopeDegrees - FalloffDegrees)) / FalloffDegrees;
    }
    return 1.0f - (slope - MaxSlopeDegrees) / FalloffDegrees;
}

void SlopeFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "Slope";
    j["minSlope"] = MinSlopeDegrees;
    j["maxSlope"] = MaxSlopeDegrees;
    j["falloff"] = FalloffDegrees;
}

void SlopeFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("minSlope")) MinSlopeDegrees = j["minSlope"];
    if (j.contains("maxSlope")) MaxSlopeDegrees = j["maxSlope"];
    if (j.contains("falloff")) FalloffDegrees = j["falloff"];
}

void SlopeFilter::DrawInspector() {
    ImGui::DragFloatRange2("Slope Range", &MinSlopeDegrees, &MaxSlopeDegrees, 0.5f, 0.0f, 90.0f, "Min: %.1f°", "Max: %.1f°");
    ImGui::DragFloat("Falloff", &FalloffDegrees, 0.1f, 0.0f, 30.0f, "%.1f°");
}

//------------------------------------------------------------------------------
// AltitudeFilter Implementation
//------------------------------------------------------------------------------

float AltitudeFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    ctx.SampleTerrain(worldPos);
    float alt = ctx.Height;
    
    if (alt < MinAltitude - FalloffMeters || alt > MaxAltitude + FalloffMeters) {
        return 0.0f;
    }
    
    if (alt >= MinAltitude && alt <= MaxAltitude) {
        return 1.0f;
    }
    
    if (alt < MinAltitude) {
        return (alt - (MinAltitude - FalloffMeters)) / FalloffMeters;
    }
    return 1.0f - (alt - MaxAltitude) / FalloffMeters;
}

void AltitudeFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "Altitude";
    j["minAlt"] = MinAltitude;
    j["maxAlt"] = MaxAltitude;
    j["falloff"] = FalloffMeters;
}

void AltitudeFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("minAlt")) MinAltitude = j["minAlt"];
    if (j.contains("maxAlt")) MaxAltitude = j["maxAlt"];
    if (j.contains("falloff")) FalloffMeters = j["falloff"];
}

void AltitudeFilter::DrawInspector() {
    ImGui::DragFloatRange2("Altitude Range", &MinAltitude, &MaxAltitude, 1.0f, -1000.0f, 10000.0f, "Min: %.1fm", "Max: %.1fm");
    ImGui::DragFloat("Falloff", &FalloffMeters, 0.5f, 0.0f, 100.0f, "%.1fm");
}

//------------------------------------------------------------------------------
// LightExposureFilter Implementation
//------------------------------------------------------------------------------

float LightExposureFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    ctx.SampleTerrain(worldPos);
    
    glm::vec3 sun = UseContextSunDirection ? ctx.SunDirection : CustomSunDirection;
    float ndotl = glm::dot(ctx.Normal, glm::normalize(sun));
    float exposure = glm::clamp((ndotl + 1.0f) * 0.5f, 0.0f, 1.0f);
    
    if (exposure < MinExposure - FalloffRange || exposure > MaxExposure + FalloffRange) {
        return 0.0f;
    }
    
    if (exposure >= MinExposure && exposure <= MaxExposure) {
        return 1.0f;
    }
    
    if (exposure < MinExposure) {
        return (exposure - (MinExposure - FalloffRange)) / FalloffRange;
    }
    return 1.0f - (exposure - MaxExposure) / FalloffRange;
}

void LightExposureFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "LightExposure";
    j["minExposure"] = MinExposure;
    j["maxExposure"] = MaxExposure;
    j["falloff"] = FalloffRange;
    j["useContextSun"] = UseContextSunDirection;
    j["customSunX"] = CustomSunDirection.x;
    j["customSunY"] = CustomSunDirection.y;
    j["customSunZ"] = CustomSunDirection.z;
}

void LightExposureFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("minExposure")) MinExposure = j["minExposure"];
    if (j.contains("maxExposure")) MaxExposure = j["maxExposure"];
    if (j.contains("falloff")) FalloffRange = j["falloff"];
    if (j.contains("useContextSun")) UseContextSunDirection = j["useContextSun"];
    if (j.contains("customSunX")) CustomSunDirection.x = j["customSunX"];
    if (j.contains("customSunY")) CustomSunDirection.y = j["customSunY"];
    if (j.contains("customSunZ")) CustomSunDirection.z = j["customSunZ"];
}

void LightExposureFilter::DrawInspector() {
    ImGui::DragFloatRange2("Exposure Range", &MinExposure, &MaxExposure, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Falloff", &FalloffRange, 0.01f, 0.0f, 0.5f);
    ImGui::Checkbox("Use Scene Sun", &UseContextSunDirection);
    if (!UseContextSunDirection) {
        ImGui::DragFloat3("Sun Direction", &CustomSunDirection.x, 0.01f, -1.0f, 1.0f);
    }
}

//------------------------------------------------------------------------------
// RoadDistanceFilter Implementation
//------------------------------------------------------------------------------

float RoadDistanceFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    ctx.SampleRoadDistance(worldPos);
    float dist = ctx.NearestRoadDistance;
    
    // No roads = everywhere eligible
    if (dist >= std::numeric_limits<float>::max() * 0.9f) {
        return 1.0f;
    }
    
    // Check minimum distance (avoid spawning on roads)
    if (dist < MinDistance) {
        return 0.0f;
    }
    
    // Check maximum distance (if not unlimited)
    if (MaxDistance > 0.0f && dist > MaxDistance + FalloffDistance) {
        return 0.0f;
    }
    
    float eligibility = 1.0f;
    
    if (InvertForNearRoad) {
        // Higher eligibility closer to road
        float range = MaxDistance > 0.0f ? (MaxDistance - MinDistance) : 100.0f;
        eligibility = 1.0f - glm::clamp((dist - MinDistance) / range, 0.0f, 1.0f);
    } else if (MaxDistance > 0.0f && dist > MaxDistance) {
        // Falloff past max distance
        eligibility = 1.0f - (dist - MaxDistance) / FalloffDistance;
    }
    
    return glm::clamp(eligibility, 0.0f, 1.0f);
}

void RoadDistanceFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "RoadDistance";
    j["minDist"] = MinDistance;
    j["maxDist"] = MaxDistance;
    j["falloff"] = FalloffDistance;
    j["invertNear"] = InvertForNearRoad;
}

void RoadDistanceFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("minDist")) MinDistance = j["minDist"];
    if (j.contains("maxDist")) MaxDistance = j["maxDist"];
    if (j.contains("falloff")) FalloffDistance = j["falloff"];
    if (j.contains("invertNear")) InvertForNearRoad = j["invertNear"];
}

void RoadDistanceFilter::DrawInspector() {
    ImGui::DragFloat("Min Distance", &MinDistance, 0.5f, 0.0f, 100.0f, "%.1fm");
    ImGui::DragFloat("Max Distance", &MaxDistance, 1.0f, 0.0f, 1000.0f, "%.1fm (0 = unlimited)");
    ImGui::DragFloat("Falloff", &FalloffDistance, 0.5f, 0.0f, 50.0f, "%.1fm");
    ImGui::Checkbox("Prefer Near Road", &InvertForNearRoad);
}

//------------------------------------------------------------------------------
// RegionMaskFilter Implementation
//------------------------------------------------------------------------------

float RegionMaskFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    if (!ctx.Regions) {
        return InvertMask ? 0.0f : 1.0f;
    }
    
    // Find matching region
    for (const auto& region : *ctx.Regions) {
        if (region.Guid != RegionGuid) continue;
        
        glm::vec2 xz(worldPos.x, worldPos.z);
        bool inside = region.ContainsPoint(xz);
        float eligibility = inside ? 1.0f : 0.0f;
        
        // Soft edge falloff
        if (EdgeFalloff > 0.0f) {
            float dist = region.DistanceToBoundary(xz);
            if (dist < EdgeFalloff) {
                if (inside) {
                    eligibility = glm::clamp(dist / EdgeFalloff, 0.0f, 1.0f);
                } else {
                    eligibility = glm::clamp(1.0f - dist / EdgeFalloff, 0.0f, 1.0f);
                }
            }
        }
        
        return InvertMask ? (1.0f - eligibility) : eligibility;
    }
    
    return InvertMask ? 1.0f : 0.0f;
}

void RegionMaskFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "RegionMask";
    j["regionGuid"] = RegionGuid.ToString();
    j["regionName"] = RegionName;
    j["invert"] = InvertMask;
    j["edgeFalloff"] = EdgeFalloff;
}

void RegionMaskFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("regionGuid")) RegionGuid = ClaymoreGUID::FromString(j["regionGuid"]);
    if (j.contains("regionName")) RegionName = j["regionName"];
    if (j.contains("invert")) InvertMask = j["invert"];
    if (j.contains("edgeFalloff")) EdgeFalloff = j["edgeFalloff"];
}

void RegionMaskFilter::DrawInspector() {
    char buf[256];
    strncpy(buf, RegionName.c_str(), sizeof(buf) - 1);
    if (ImGui::InputText("Region Name", buf, sizeof(buf))) {
        RegionName = buf;
    }
    ImGui::Checkbox("Invert (spawn outside)", &InvertMask);
    ImGui::DragFloat("Edge Falloff", &EdgeFalloff, 0.1f, 0.0f, 20.0f, "%.1fm");
}

//------------------------------------------------------------------------------
// ClimateFilter Implementation
//------------------------------------------------------------------------------

float ClimateFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    ctx.SampleClimate(worldPos);
    
    auto evalRange = [](float value, float min, float max, float falloff) -> float {
        if (value < min - falloff || value > max + falloff) return 0.0f;
        if (value >= min && value <= max) return 1.0f;
        if (value < min) return (value - (min - falloff)) / falloff;
        return 1.0f - (value - max) / falloff;
    };
    
    float tempE = evalRange(ctx.ClimateVals.Temperature, MinTemperature, MaxTemperature, TemperatureFalloff);
    float moistE = evalRange(ctx.ClimateVals.Moisture, MinMoisture, MaxMoisture, MoistureFalloff);
    float windE = (ctx.ClimateVals.WindExposure >= MinWindExposure && ctx.ClimateVals.WindExposure <= MaxWindExposure) ? 1.0f : 0.0f;
    
    return tempE * moistE * windE;
}

void ClimateFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "Climate";
    j["minTemp"] = MinTemperature;
    j["maxTemp"] = MaxTemperature;
    j["tempFalloff"] = TemperatureFalloff;
    j["minMoist"] = MinMoisture;
    j["maxMoist"] = MaxMoisture;
    j["moistFalloff"] = MoistureFalloff;
    j["minWind"] = MinWindExposure;
    j["maxWind"] = MaxWindExposure;
}

void ClimateFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("minTemp")) MinTemperature = j["minTemp"];
    if (j.contains("maxTemp")) MaxTemperature = j["maxTemp"];
    if (j.contains("tempFalloff")) TemperatureFalloff = j["tempFalloff"];
    if (j.contains("minMoist")) MinMoisture = j["minMoist"];
    if (j.contains("maxMoist")) MaxMoisture = j["maxMoist"];
    if (j.contains("moistFalloff")) MoistureFalloff = j["moistFalloff"];
    if (j.contains("minWind")) MinWindExposure = j["minWind"];
    if (j.contains("maxWind")) MaxWindExposure = j["maxWind"];
}

void ClimateFilter::DrawInspector() {
    ImGui::Text("Temperature");
    ImGui::DragFloatRange2("##TempRange", &MinTemperature, &MaxTemperature, 0.5f, -40.0f, 50.0f, "%.1f°C", "%.1f°C");
    ImGui::DragFloat("Temp Falloff", &TemperatureFalloff, 0.1f, 0.0f, 20.0f, "%.1f°C");
    
    ImGui::Separator();
    ImGui::Text("Moisture");
    ImGui::DragFloatRange2("##MoistRange", &MinMoisture, &MaxMoisture, 0.01f, 0.0f, 1.0f, "%.0f%%", "%.0f%%");
    ImGui::DragFloat("Moist Falloff", &MoistureFalloff, 0.01f, 0.0f, 0.5f);
    
    ImGui::Separator();
    ImGui::Text("Wind Exposure");
    ImGui::DragFloatRange2("##WindRange", &MinWindExposure, &MaxWindExposure, 0.01f, 0.0f, 1.0f);
}

//------------------------------------------------------------------------------
// NoiseFilter Implementation
//------------------------------------------------------------------------------

float NoiseFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    (void)ctx; // Not using context for noise
    
    float nx = worldPos.x / Scale;
    float nz = UseWorldPosition ? worldPos.z / Scale : worldPos.y / Scale;
    
    float noise = FractalNoise(nx, nz, Octaves, Persistence, Lacunarity, Seed);
    noise = noise * 0.5f + 0.5f; // Map to 0-1
    
    if (noise < Threshold - Falloff) return 0.0f;
    if (noise >= Threshold) return 1.0f;
    
    return (noise - (Threshold - Falloff)) / Falloff;
}

void NoiseFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "Noise";
    j["scale"] = Scale;
    j["threshold"] = Threshold;
    j["falloff"] = Falloff;
    j["octaves"] = Octaves;
    j["persistence"] = Persistence;
    j["lacunarity"] = Lacunarity;
    j["seed"] = Seed;
    j["useWorldPos"] = UseWorldPosition;
}

void NoiseFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("scale")) Scale = j["scale"];
    if (j.contains("threshold")) Threshold = j["threshold"];
    if (j.contains("falloff")) Falloff = j["falloff"];
    if (j.contains("octaves")) Octaves = j["octaves"];
    if (j.contains("persistence")) Persistence = j["persistence"];
    if (j.contains("lacunarity")) Lacunarity = j["lacunarity"];
    if (j.contains("seed")) Seed = j["seed"];
    if (j.contains("useWorldPos")) UseWorldPosition = j["useWorldPos"];
}

void NoiseFilter::DrawInspector() {
    ImGui::DragFloat("Scale", &Scale, 1.0f, 1.0f, 500.0f, "%.1f");
    ImGui::DragFloat("Threshold", &Threshold, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Falloff", &Falloff, 0.01f, 0.0f, 0.5f);
    ImGui::DragInt("Octaves", &Octaves, 0.1f, 1, 8);
    ImGui::DragFloat("Persistence", &Persistence, 0.01f, 0.1f, 1.0f);
    ImGui::DragFloat("Lacunarity", &Lacunarity, 0.1f, 1.0f, 4.0f);
    ImGui::DragInt("Seed", &Seed);
    ImGui::Checkbox("Use XZ (vs XY)", &UseWorldPosition);
}

//------------------------------------------------------------------------------
// SplatMaskFilter Implementation
//------------------------------------------------------------------------------

float SplatMaskFilter::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    ctx.SampleTerrain(worldPos);
    
    // Sample splat data from terrain
    if (!ctx.Terrain || ctx.Terrain->SplatMap.empty()) {
        return 1.0f; // No splat data = everywhere eligible
    }
    
    // Convert world position to terrain texel coordinates
    glm::mat4 invTransform = glm::inverse(ctx.TerrainTransform);
    glm::vec4 localPos = invTransform * glm::vec4(worldPos.x, 0.0f, worldPos.z, 1.0f);
    
    if (ctx.Terrain->GridResolution == 0) return 1.0f;
    
    float u = localPos.x / ctx.Terrain->WorldSize.x;
    float v = localPos.z / ctx.Terrain->WorldSize.y;  // WorldSize is vec2 (x, y for width/depth)
    u = glm::clamp(u, 0.0f, 1.0f);
    v = glm::clamp(v, 0.0f, 1.0f);
    
    int tx = static_cast<int>(u * (ctx.Terrain->GridResolution - 1));
    int ty = static_cast<int>(v * (ctx.Terrain->GridResolution - 1));
    int idx = ty * ctx.Terrain->GridResolution + tx;
    
    if (idx < 0 || idx >= static_cast<int>(ctx.Terrain->SplatMap.size())) {
        return 1.0f;
    }
    
    // Extract channel value from u8vec4
    const glm::u8vec4& splat = ctx.Terrain->SplatMap[idx];
    float channelValue = 0.0f;
    
    switch (MaskChannel) {
        case Channel::R: channelValue = splat.r / 255.0f; break;
        case Channel::G: channelValue = splat.g / 255.0f; break;
        case Channel::B: channelValue = splat.b / 255.0f; break;
        case Channel::A: channelValue = splat.a / 255.0f; break;
    }
    
    if (Invert) {
        channelValue = 1.0f - channelValue;
    }
    
    if (channelValue < MinValue - Falloff || channelValue > MaxValue + Falloff) {
        return 0.0f;
    }
    
    if (channelValue >= MinValue && channelValue <= MaxValue) {
        return 1.0f;
    }
    
    if (channelValue < MinValue) {
        return (channelValue - (MinValue - Falloff)) / Falloff;
    }
    return 1.0f - (channelValue - MaxValue) / Falloff;
}

void SplatMaskFilter::Serialize(nlohmann::json& j) const {
    j["type"] = "SplatMask";
    j["channel"] = static_cast<int>(MaskChannel);
    j["minValue"] = MinValue;
    j["maxValue"] = MaxValue;
    j["falloff"] = Falloff;
    j["invert"] = Invert;
}

void SplatMaskFilter::Deserialize(const nlohmann::json& j) {
    if (j.contains("channel")) MaskChannel = static_cast<Channel>(j["channel"].get<int>());
    if (j.contains("minValue")) MinValue = j["minValue"];
    if (j.contains("maxValue")) MaxValue = j["maxValue"];
    if (j.contains("falloff")) Falloff = j["falloff"];
    if (j.contains("invert")) Invert = j["invert"];
}

void SplatMaskFilter::DrawInspector() {
    const char* channels[] = { "Red", "Green", "Blue", "Alpha" };
    int ch = static_cast<int>(MaskChannel);
    if (ImGui::Combo("Channel", &ch, channels, 4)) {
        MaskChannel = static_cast<Channel>(ch);
    }
    ImGui::DragFloatRange2("Value Range", &MinValue, &MaxValue, 0.01f, 0.0f, 1.0f);
    ImGui::DragFloat("Falloff", &Falloff, 0.01f, 0.0f, 0.5f);
    ImGui::Checkbox("Invert", &Invert);
}

//------------------------------------------------------------------------------
// CompositeEligibilityMap Implementation
//------------------------------------------------------------------------------

float CompositeEligibilityMap::Evaluate(const glm::vec3& worldPos, const EligibilityContext& ctx) const {
    if (Filters.empty()) return 1.0f;
    
    ctx.InvalidateCache();
    
    float result = 0.0f;
    float weightSum = 0.0f;
    bool first = true;
    
    for (const auto& entry : Filters) {
        if (!entry.Enabled || !entry.Filter) continue;
        
        float value = entry.Filter->Evaluate(worldPos, ctx);
        float weighted = value * entry.Weight;
        weightSum += entry.Weight;
        
        switch (Mode) {
            case CombineMode::Multiply:
                if (first) {
                    result = value;
                    first = false;
                } else {
                    result *= value;
                }
                break;
                
            case CombineMode::Min:
                if (first) {
                    result = value;
                    first = false;
                } else {
                    result = std::min(result, value);
                }
                break;
                
            case CombineMode::Max:
                if (first) {
                    result = value;
                    first = false;
                } else {
                    result = std::max(result, value);
                }
                break;
                
            case CombineMode::Average:
                result += weighted;
                break;
        }
    }
    
    if (Mode == CombineMode::Average && weightSum > 0.0f) {
        result /= weightSum;
    }
    
    return glm::clamp(result, 0.0f, 1.0f);
}

void CompositeEligibilityMap::AddFilter(std::unique_ptr<IEligibilityFilter> filter, float weight) {
    FilterEntry entry;
    entry.Filter = std::move(filter);
    entry.Weight = weight;
    entry.Enabled = true;
    Filters.push_back(std::move(entry));
}

void CompositeEligibilityMap::RemoveFilter(size_t index) {
    if (index < Filters.size()) {
        Filters.erase(Filters.begin() + index);
    }
}

void CompositeEligibilityMap::MoveFilter(size_t fromIndex, size_t toIndex) {
    if (fromIndex >= Filters.size() || toIndex >= Filters.size()) return;
    if (fromIndex == toIndex) return;
    
    FilterEntry entry = std::move(Filters[fromIndex]);
    Filters.erase(Filters.begin() + fromIndex);
    Filters.insert(Filters.begin() + toIndex, std::move(entry));
}

void CompositeEligibilityMap::ClearFilters() {
    Filters.clear();
}

void CompositeEligibilityMap::Serialize(nlohmann::json& j) const {
    j["mode"] = static_cast<int>(Mode);
    j["filters"] = nlohmann::json::array();
    
    for (const auto& entry : Filters) {
        if (!entry.Filter) continue;
        
        nlohmann::json fj;
        entry.Filter->Serialize(fj);
        fj["weight"] = entry.Weight;
        fj["enabled"] = entry.Enabled;
        j["filters"].push_back(fj);
    }
}

void CompositeEligibilityMap::Deserialize(const nlohmann::json& j) {
    Filters.clear();
    
    if (j.contains("mode")) {
        Mode = static_cast<CombineMode>(j["mode"].get<int>());
    }
    
    if (j.contains("filters") && j["filters"].is_array()) {
        for (const auto& fj : j["filters"]) {
            if (!fj.contains("type")) continue;
            
            std::string type = fj["type"];
            auto filter = IEligibilityFilter::Create(type);
            if (filter) {
                filter->Deserialize(fj);
                
                FilterEntry entry;
                entry.Filter = std::move(filter);
                entry.Weight = fj.value("weight", 1.0f);
                entry.Enabled = fj.value("enabled", true);
                Filters.push_back(std::move(entry));
            }
        }
    }
}

bool CompositeEligibilityMap::CanBake() const {
    for (const auto& entry : Filters) {
        if (entry.Enabled && entry.Filter && !entry.Filter->SupportsBaking()) {
            return false;
        }
    }
    return true;
}

//------------------------------------------------------------------------------
// Filter Factory
//------------------------------------------------------------------------------

std::unique_ptr<IEligibilityFilter> IEligibilityFilter::Create(const std::string& typeName) {
    if (typeName == "Slope") return std::make_unique<SlopeFilter>();
    if (typeName == "Altitude") return std::make_unique<AltitudeFilter>();
    if (typeName == "Light Exposure" || typeName == "LightExposure") return std::make_unique<LightExposureFilter>();
    if (typeName == "Road Distance" || typeName == "RoadDistance") return std::make_unique<RoadDistanceFilter>();
    if (typeName == "Region Mask" || typeName == "RegionMask") return std::make_unique<RegionMaskFilter>();
    if (typeName == "Climate") return std::make_unique<ClimateFilter>();
    if (typeName == "Noise") return std::make_unique<NoiseFilter>();
    if (typeName == "Splat Mask" || typeName == "SplatMask") return std::make_unique<SplatMaskFilter>();
    return nullptr;
}

std::vector<std::string> IEligibilityFilter::GetRegisteredTypes() {
    return {
        "Slope",
        "Altitude",
        "Light Exposure",
        "Road Distance",
        "Region Mask",
        "Climate",
        "Noise",
        "Splat Mask"
    };
}

} // namespace resourcelayer
} // namespace cm
