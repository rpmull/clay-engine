#include "editor/tools/WorldGenerationPanel.h"

#include "imgui.h"
#include "core/ecs/Scene.h"
#include "core/ecs/EntityData.h"
#include "core/rendering/Terrain.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
#include <string_view>

namespace
{
    constexpr float kFloatEpsilon = 1e-6f;
    constexpr float kMetersPerKilometer = 1000.0f;
    constexpr float kSquareMetersPerSquareKilometer = 1'000'000.0f;
    constexpr float kEdgeFadeStart = 0.85f;
    constexpr float kEdgeFadeEnd = 1.25f;

    // Zone display names for UI
    const char* kZoneNames[] = {
        "Mountains",
        "Highlands", 
        "Plains",
        "Valley",
        "Cliffs",
        "Beach",
        "Mesa",
        "Canyon",
        "Wetlands",
        "Plateau"
    };
    static_assert(sizeof(kZoneNames) / sizeof(kZoneNames[0]) == static_cast<int>(TerrainZone::Count), "Zone names must match enum");

    const char* kTemplateNames[] = {
        "Custom",
        "Continental",
        "Island Kingdom",
        "Mountain Fortress",
        "Grand Canyon",
        "Coastal Range",
        "Rift Valley",
        "Archipelago",
        "Highland Plains"
    };
    static_assert(sizeof(kTemplateNames) / sizeof(kTemplateNames[0]) == static_cast<int>(WorldTemplate::Count), "Template names must match enum");

    const char* kModeNames[] = {
        "Simple",
        "Region",
        "Freeform",
        "Advanced"
    };
    
    const char* kStampNames[] = {
        "None",
        "Mountain",
        "Hill",
        "Valley",
        "River",
        "Mesa",
        "Plateau",
        "Canyon",
        "Lake",
        "Forest"
    };
    static_assert(sizeof(kStampNames) / sizeof(kStampNames[0]) == static_cast<int>(FeatureStamp::Count), "Stamp names must match enum");
    
    // Colors for drawing stamps on canvas
    ImU32 GetStampColor(FeatureStamp stamp)
    {
        switch (stamp)
        {
        case FeatureStamp::Mountain: return IM_COL32(139, 90, 43, 200);   // Brown
        case FeatureStamp::Hill: return IM_COL32(107, 142, 35, 200);      // Olive
        case FeatureStamp::Valley: return IM_COL32(34, 139, 34, 200);     // Forest green
        case FeatureStamp::River: return IM_COL32(65, 105, 225, 200);     // Royal blue
        case FeatureStamp::Mesa: return IM_COL32(210, 180, 140, 200);     // Tan
        case FeatureStamp::Plateau: return IM_COL32(188, 143, 143, 200);  // Rosy brown
        case FeatureStamp::Canyon: return IM_COL32(160, 82, 45, 200);     // Sienna
        case FeatureStamp::Lake: return IM_COL32(70, 130, 180, 200);      // Steel blue
        case FeatureStamp::Forest: return IM_COL32(0, 100, 0, 200);       // Dark green
        default: return IM_COL32(128, 128, 128, 200);
        }
    }

    // Properties for each terrain zone type
    struct ZoneProperties
    {
        float BaseElevation;        // 0-1 normalized base height
        float ElevationVariation;   // How much the elevation varies
        float Roughness;            // Surface roughness/bumpiness
        float RidgeStrength;        // How much ridged noise to add
        float TerraceStrength;      // Terracing effect
        bool AllowRivers;           // Whether rivers can form here
    };

    ZoneProperties GetZoneProperties(TerrainZone zone)
    {
        switch (zone)
        {
        case TerrainZone::Mountains:
            return { 0.75f, 0.25f, 0.8f, 0.9f, 0.1f, true };
        case TerrainZone::Highlands:
            return { 0.55f, 0.15f, 0.5f, 0.3f, 0.2f, true };
        case TerrainZone::Plains:
            return { 0.35f, 0.08f, 0.15f, 0.0f, 0.0f, true };
        case TerrainZone::Valley:
            return { 0.25f, 0.1f, 0.2f, 0.0f, 0.0f, true };
        case TerrainZone::Cliffs:
            return { 0.6f, 0.3f, 0.9f, 0.5f, 0.4f, false };
        case TerrainZone::Beach:
            return { 0.18f, 0.05f, 0.1f, 0.0f, 0.0f, false };
        case TerrainZone::Mesa:
            return { 0.5f, 0.2f, 0.3f, 0.2f, 0.85f, false };
        case TerrainZone::Canyon:
            return { 0.15f, 0.35f, 0.6f, 0.4f, 0.3f, true };
        case TerrainZone::Wetlands:
            return { 0.2f, 0.03f, 0.05f, 0.0f, 0.0f, true };
        case TerrainZone::Plateau:
            return { 0.5f, 0.1f, 0.25f, 0.1f, 0.7f, true };
        default:
            return { 0.35f, 0.1f, 0.2f, 0.0f, 0.0f, true };
        }
    }

    float TerrainAreaKm2(const glm::vec2& worldSizeMeters)
    {
        if (worldSizeMeters.x <= 0.0f || worldSizeMeters.y <= 0.0f)
            return 0.0f;
        return (worldSizeMeters.x * worldSizeMeters.y) / kSquareMetersPerSquareKilometer;
    }

    float SizePresetAreaKm2(WorldGenerationPanel::TerrainSizePreset preset)
    {
        using Preset = WorldGenerationPanel::TerrainSizePreset;
        switch (preset)
        {
        case Preset::Small: return 2.0f;
        case Preset::Medium: return 6.0f;
        case Preset::Large: return 12.0f;
        case Preset::Huge: return 20.0f;
        default: return 0.0f;
        }
    }

    glm::vec2 WorldSizeFromPreset(WorldGenerationPanel::TerrainSizePreset preset)
    {
        float areaKm2 = SizePresetAreaKm2(preset);
        if (areaKm2 <= kFloatEpsilon)
            return glm::vec2(3460.0f, 3460.0f);
        float sideMeters = std::sqrt(areaKm2) * kMetersPerKilometer;
        return glm::vec2(sideMeters, sideMeters);
    }

    float Fade(float t)
    {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    uint32_t HashUInt(uint32_t x, uint32_t y, uint32_t seed)
    {
        uint32_t h = x * 374761393u + y * 668265263u + seed * 362437u;
        h = (h ^ (h >> 13u)) * 1274126177u;
        h ^= (h >> 16u);
        return h;
    }

    glm::vec2 RandomGradient(int ix, int iy, uint32_t seed)
    {
        uint32_t h = HashUInt(static_cast<uint32_t>(ix), static_cast<uint32_t>(iy), seed);
        float angle = (h & 1023u) / 1023.0f * glm::two_pi<float>();
        return glm::vec2(std::cos(angle), std::sin(angle));
    }

    float GradientNoise(const glm::vec2& p, uint32_t seed)
    {
        int x0 = static_cast<int>(std::floor(p.x));
        int y0 = static_cast<int>(std::floor(p.y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float tx = p.x - static_cast<float>(x0);
        float ty = p.y - static_cast<float>(y0);
        glm::vec2 d00(tx, ty);
        glm::vec2 d10(tx - 1.0f, ty);
        glm::vec2 d01(tx, ty - 1.0f);
        glm::vec2 d11(tx - 1.0f, ty - 1.0f);
        float v00 = glm::dot(RandomGradient(x0, y0, seed), d00);
        float v10 = glm::dot(RandomGradient(x1, y0, seed), d10);
        float v01 = glm::dot(RandomGradient(x0, y1, seed), d01);
        float v11 = glm::dot(RandomGradient(x1, y1, seed), d11);
        float u = Fade(tx);
        float v = Fade(ty);
        float nx0 = glm::mix(v00, v10, u);
        float nx1 = glm::mix(v01, v11, u);
        return glm::mix(nx0, nx1, v);
    }

    float FBM(const glm::vec2& p, int octaves, float lacunarity, float gain, uint32_t seed)
    {
        float amplitude = 1.0f;
        float frequency = 1.0f;
        float sum = 0.0f;
        float weight = 0.0f;
        for (int i = 0; i < octaves; ++i)
        {
            float n = GradientNoise(p * frequency, seed + static_cast<uint32_t>(i) * 7919u);
            sum += n * amplitude;
            weight += amplitude;
            frequency *= lacunarity;
            amplitude *= gain;
        }
        if (weight <= kFloatEpsilon)
            return 0.5f;
        float normalized = sum / weight;
        return 0.5f + 0.5f * glm::clamp(normalized, -1.0f, 1.0f);
    }

    float RidgedFBM(const glm::vec2& p, int octaves, float lacunarity, float gain, uint32_t seed)
    {
        float amplitude = 0.5f;
        float frequency = 1.0f;
        float sum = 0.0f;
        float weight = 0.0f;
        for (int i = 0; i < octaves; ++i)
        {
            float n = GradientNoise(p * frequency, seed + static_cast<uint32_t>(i) * 1699u);
            n = 1.0f - std::abs(n);
            n *= n;
            sum += n * amplitude;
            weight += amplitude;
            frequency *= lacunarity;
            amplitude *= gain;
        }
        if (weight <= kFloatEpsilon)
            return 0.0f;
        return glm::clamp(sum / weight, 0.0f, 1.0f);
    }

    glm::vec2 DomainWarp(const glm::vec2& p, uint32_t seed, float strength, float frequency)
    {
        glm::vec2 q(
            GradientNoise(p * frequency + glm::vec2(5.2f, 1.3f), seed),
            GradientNoise(p * frequency + glm::vec2(8.3f, 2.8f), seed + 19u));
        return (q * 2.0f - glm::vec2(1.0f)) * strength;
    }

    float Terrace(float value, float step)
    {
        if (step <= kFloatEpsilon)
            return value;
        float steps = glm::round(value / step);
        return glm::clamp(steps * step, 0.0f, 1.0f);
    }

    struct HeightField
    {
        explicit HeightField(int resolution = 0)
        {
            Resize(resolution);
        }

        void Resize(int resolution)
        {
            Resolution = std::max(2, resolution);
            Values.assign(static_cast<size_t>(Resolution) * static_cast<size_t>(Resolution), 0.0f);
        }

        size_t Index(int x, int y) const
        {
            x = std::clamp(x, 0, Resolution - 1);
            y = std::clamp(y, 0, Resolution - 1);
            return static_cast<size_t>(y) * Resolution + static_cast<size_t>(x);
        }

        float Get(int x, int y) const
        {
            return Values[Index(x, y)];
        }

        void Set(int x, int y, float value)
        {
            Values[Index(x, y)] = glm::clamp(value, 0.0f, 1.0f);
        }

        float Sample(float fx, float fy) const
        {
            fx = glm::clamp(fx, 0.0f, static_cast<float>(Resolution - 1));
            fy = glm::clamp(fy, 0.0f, static_cast<float>(Resolution - 1));
            int x0 = static_cast<int>(std::floor(fx));
            int y0 = static_cast<int>(std::floor(fy));
            int x1 = std::min(x0 + 1, Resolution - 1);
            int y1 = std::min(y0 + 1, Resolution - 1);
            float tx = fx - static_cast<float>(x0);
            float ty = fy - static_cast<float>(y0);
            float v00 = Values[Index(x0, y0)];
            float v10 = Values[Index(x1, y0)];
            float v01 = Values[Index(x0, y1)];
            float v11 = Values[Index(x1, y1)];
            float a = glm::mix(v00, v10, tx);
            float b = glm::mix(v01, v11, tx);
            return glm::mix(a, b, ty);
        }

        glm::vec2 Gradient(float fx, float fy) const
        {
            float left = Sample(fx - 1.0f, fy);
            float right = Sample(fx + 1.0f, fy);
            float down = Sample(fx, fy - 1.0f);
            float up = Sample(fx, fy + 1.0f);
            return glm::vec2((right - left) * 0.5f, (up - down) * 0.5f);
        }

        float Deposit(float fx, float fy, float amount)
        {
            if (amount <= 0.0f)
                return 0.0f;
            fx = glm::clamp(fx, 0.0f, static_cast<float>(Resolution - 1));
            fy = glm::clamp(fy, 0.0f, static_cast<float>(Resolution - 1));
            int x0 = static_cast<int>(std::floor(fx));
            int y0 = static_cast<int>(std::floor(fy));
            int x1 = std::min(x0 + 1, Resolution - 1);
            int y1 = std::min(y0 + 1, Resolution - 1);
            float tx = fx - static_cast<float>(x0);
            float ty = fy - static_cast<float>(y0);
            float w00 = (1.0f - tx) * (1.0f - ty);
            float w10 = tx * (1.0f - ty);
            float w01 = (1.0f - tx) * ty;
            float w11 = tx * ty;
            Values[Index(x0, y0)] = glm::clamp(Values[Index(x0, y0)] + amount * w00, 0.0f, 1.0f);
            Values[Index(x1, y0)] = glm::clamp(Values[Index(x1, y0)] + amount * w10, 0.0f, 1.0f);
            Values[Index(x0, y1)] = glm::clamp(Values[Index(x0, y1)] + amount * w01, 0.0f, 1.0f);
            Values[Index(x1, y1)] = glm::clamp(Values[Index(x1, y1)] + amount * w11, 0.0f, 1.0f);
            return amount;
        }

        float Erode(float fx, float fy, float amount)
        {
            if (amount <= 0.0f)
                return 0.0f;
            fx = glm::clamp(fx, 0.0f, static_cast<float>(Resolution - 1));
            fy = glm::clamp(fy, 0.0f, static_cast<float>(Resolution - 1));
            int x0 = static_cast<int>(std::floor(fx));
            int y0 = static_cast<int>(std::floor(fy));
            int x1 = std::min(x0 + 1, Resolution - 1);
            int y1 = std::min(y0 + 1, Resolution - 1);
            float tx = fx - static_cast<float>(x0);
            float ty = fy - static_cast<float>(y0);
            float w00 = (1.0f - tx) * (1.0f - ty);
            float w10 = tx * (1.0f - ty);
            float w01 = (1.0f - tx) * ty;
            float w11 = tx * ty;
            float weightSum = w00 + w10 + w01 + w11;
            if (weightSum <= kFloatEpsilon)
                return 0.0f;
            float removed = 0.0f;
            auto consume = [&](int x, int y, float weight)
            {
                if (weight <= 0.0f)
                    return;
                float target = amount * (weight / weightSum);
                size_t idx = Index(x, y);
                float take = std::min(Values[idx], target);
                Values[idx] -= take;
                removed += take;
            };
            consume(x0, y0, w00);
            consume(x1, y0, w10);
            consume(x0, y1, w01);
            consume(x1, y1, w11);
            return removed;
        }

        void ClampAll()
        {
            for (float& v : Values)
            {
                v = glm::clamp(v, 0.0f, 1.0f);
            }
        }

        float Min() const
        {
            return *std::min_element(Values.begin(), Values.end());
        }

        float Max() const
        {
            return *std::max_element(Values.begin(), Values.end());
        }

        int Resolution = 0;
        std::vector<float> Values;
    };

    struct MacroPlate
    {
        glm::vec2 Center = glm::vec2(0.5f);
        glm::vec2 Axis = glm::vec2(0.3f, 0.4f);
        glm::mat2 Rotation = glm::mat2(1.0f);
        float InfluencePower = 1.5f;
        float PlateauRadius = 0.45f;
    };

    std::vector<MacroPlate> BuildMacroPlates(const WorldGenerationPanel::Settings& settings)
    {
        int count = std::clamp(settings.MacroPlateCount, 1, 8);
        std::vector<MacroPlate> plates;
        plates.reserve(count);
        std::mt19937 rng(settings.BaseSeed * 48271u + 0x27d4eb2du);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
        float clusterStrength = glm::clamp(settings.MacroClusterStrength, 0.0f, 1.0f);
        for (int i = 0; i < count; ++i)
        {
            MacroPlate plate;
            glm::vec2 randomCenter(rand01(rng), rand01(rng));
            glm::vec2 biasTarget = glm::vec2(0.5f);
            glm::vec2 jitter(rand01(rng) - 0.5f, rand01(rng) - 0.5f);
            biasTarget += jitter * (0.4f * clusterStrength);
            plate.Center = glm::mix(randomCenter, biasTarget, clusterStrength);
            plate.Center = glm::clamp(plate.Center, glm::vec2(0.05f), glm::vec2(0.95f));
            float axisBase = glm::mix(0.18f, 0.42f, rand01(rng));
            float aspect = glm::mix(0.6f, 1.45f, rand01(rng));
            if (rand01(rng) > 0.5f)
                aspect = 1.0f / glm::max(aspect, 0.001f);
            plate.Axis = glm::vec2(axisBase, axisBase * aspect);
            float angle = rand01(rng) * glm::two_pi<float>();
            float c = std::cos(angle);
            float s = std::sin(angle);
            plate.Rotation = glm::mat2(c, -s, s, c);
            plate.InfluencePower = glm::mix(1.2f, 2.6f, rand01(rng));
            plate.PlateauRadius = glm::mix(0.3f, 0.65f, rand01(rng));
            plates.push_back(plate);
        }
        return plates;
    }

    float SampleMaskAt(const std::vector<float>& mask, float fx, float fy, int res)
    {
        if (mask.empty())
            return 0.0f;
        fx = glm::clamp(fx, 0.0f, static_cast<float>(res - 1));
        fy = glm::clamp(fy, 0.0f, static_cast<float>(res - 1));
        int x0 = static_cast<int>(std::floor(fx));
        int y0 = static_cast<int>(std::floor(fy));
        int x1 = std::min(x0 + 1, res - 1);
        int y1 = std::min(y0 + 1, res - 1);
        float tx = fx - static_cast<float>(x0);
        float ty = fy - static_cast<float>(y0);
        size_t idx00 = static_cast<size_t>(y0) * res + static_cast<size_t>(x0);
        size_t idx10 = static_cast<size_t>(y0) * res + static_cast<size_t>(x1);
        size_t idx01 = static_cast<size_t>(y1) * res + static_cast<size_t>(x0);
        size_t idx11 = static_cast<size_t>(y1) * res + static_cast<size_t>(x1);
        float a = glm::mix(mask[idx00], mask[idx10], tx);
        float b = glm::mix(mask[idx01], mask[idx11], tx);
        return glm::mix(a, b, ty);
    }

    void RunHydraulicErosion(HeightField& field,
                             const std::vector<float>& plateauMask,
                             const WorldGenerationPanel::Settings& settings)
    {
        const int res = field.Resolution;
        std::mt19937 rng(settings.ErosionSeed);
        std::uniform_real_distribution<float> randPos(0.0f, static_cast<float>(res - 1));
        for (int drop = 0; drop < settings.ErosionDroplets; ++drop)
        {
            float posX = randPos(rng);
            float posY = randPos(rng);
            float dirX = 0.0f;
            float dirY = 0.0f;
            float speed = settings.ErosionStartSpeed;
            float water = settings.ErosionStartWater;
            float sediment = 0.0f;
            for (int step = 0; step < settings.ErosionMaxSteps; ++step)
            {
                float height = field.Sample(posX, posY);
                glm::vec2 gradient = field.Gradient(posX, posY);
                dirX = dirX * settings.ErosionInertia - gradient.x * (1.0f - settings.ErosionInertia);
                dirY = dirY * settings.ErosionInertia - gradient.y * (1.0f - settings.ErosionInertia);
                float dirLength = std::sqrt(dirX * dirX + dirY * dirY);
                if (dirLength > kFloatEpsilon)
                {
                    dirX /= dirLength;
                    dirY /= dirLength;
                }
                posX += dirX;
                posY += dirY;
                if (dirLength <= kFloatEpsilon || posX < 0.0f || posX >= res - 1 || posY < 0.0f || posY >= res - 1)
                    break;

                float newHeight = field.Sample(posX, posY);
                float deltaH = newHeight - height;
                float heightNorm = field.Sample(posX, posY);
                float highlandFactor = glm::mix(1.0f, settings.HighlandErosionMultiplier, glm::clamp(heightNorm, 0.0f, 1.0f));
                float plateauFactor = glm::mix(1.0f, settings.PlateauErosionDamping, glm::clamp(SampleMaskAt(plateauMask, posX, posY, res), 0.0f, 1.0f));
                float slope = std::max(-deltaH, settings.ErosionMinSlope);
                float capacity = slope * speed * water * settings.ErosionCapacity * highlandFactor * plateauFactor;

                if (sediment > capacity || deltaH > 0.0f)
                {
                    float amount = (sediment - capacity) * settings.ErosionDepositSpeed;
                    amount = glm::max(amount, deltaH > 0.0f ? std::min(deltaH, sediment) : 0.0f);
                    amount = glm::min(amount, sediment);
                    if (amount > 0.0f)
                    {
                        field.Deposit(posX, posY, amount);
                        sediment -= amount;
                    }
                }
                else
                {
                    float amount = (capacity - sediment) * settings.ErosionErodeSpeed;
                    if (amount > 0.0f)
                    {
                        float eroded = field.Erode(posX, posY, amount);
                        sediment += eroded;
                    }
                }

                speed = std::sqrt(std::max(0.0f, speed * speed + deltaH * settings.ErosionGravity));
                water *= (1.0f - settings.ErosionEvaporateSpeed);
                if (water < 0.05f)
                    break;
            }
        }
    }

    void ComputeFlowAndRivers(const HeightField& field,
                              const std::vector<float>& landMask,
                              const WorldGenerationPanel::Settings& settings,
                              const glm::vec2& worldSize,
                              std::vector<float>& flow,
                              std::vector<float>& riverMask,
                              std::vector<float>& riverDistance)
    {
        const int res = field.Resolution;
        const size_t count = field.Values.size();
        flow.assign(count, 1.0f);
        riverMask.assign(count, 0.0f);
        riverDistance.assign(count, std::numeric_limits<float>::max());

        std::vector<int> receiver(count, -1);
        std::vector<size_t> order(count);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
        {
            return field.Values[a] > field.Values[b];
        });

        const int offsets[8][2] = {
            { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 },
            { 1, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }
        };

        for (size_t idx : order)
        {
            int x = static_cast<int>(idx % res);
            int y = static_cast<int>(idx / res);
            float currentHeight = field.Values[idx];
            float bestDrop = 0.0f;
            int bestIndex = -1;
            for (const auto& off : offsets)
            {
                int nx = x + off[0];
                int ny = y + off[1];
                if (nx < 0 || ny < 0 || nx >= res || ny >= res)
                    continue;
                size_t nIdx = static_cast<size_t>(ny) * res + static_cast<size_t>(nx);
                float drop = currentHeight - field.Values[nIdx];
                if (drop > bestDrop)
                {
                    bestDrop = drop;
                    bestIndex = static_cast<int>(nIdx);
                }
            }
            receiver[idx] = bestIndex;
        }

        for (size_t idx : order)
        {
            int downstream = receiver[idx];
            if (downstream >= 0)
            {
                flow[static_cast<size_t>(downstream)] += flow[idx];
            }
        }

        std::vector<float> sortedFlow = flow;
        std::sort(sortedFlow.begin(), sortedFlow.end());
        float density = glm::clamp(settings.RiverDensity, 0.01f, 0.95f);
        size_t thresholdIndex = static_cast<size_t>(glm::clamp(1.0f - density, 0.0f, 0.999f) * (sortedFlow.size() - 1));
        float threshold = glm::max(settings.RiverFlowFloor, sortedFlow[thresholdIndex]);
        float normFactor = threshold * 0.65f + 1e-3f;

        for (size_t i = 0; i < count; ++i)
        {
            if (landMask[i] <= 0.05f)
                continue;
            float value = glm::clamp((flow[i] - threshold) / normFactor, 0.0f, 1.0f);
            riverMask[i] = value;
        }

        std::queue<size_t> q;
        for (size_t i = 0; i < count; ++i)
        {
            if (riverMask[i] > 0.25f)
            {
                riverDistance[i] = 0.0f;
                q.push(i);
            }
        }

        while (!q.empty())
        {
            size_t idx = q.front();
            q.pop();
            int x = static_cast<int>(idx % res);
            int y = static_cast<int>(idx / res);
            for (int i = 0; i < 4; ++i)
            {
                int nx = x + offsets[i][0];
                int ny = y + offsets[i][1];
                if (nx < 0 || ny < 0 || nx >= res || ny >= res)
                    continue;
                size_t nIdx = static_cast<size_t>(ny) * res + static_cast<size_t>(nx);
                if (riverDistance[nIdx] > riverDistance[idx] + 1.0f)
                {
                    riverDistance[nIdx] = riverDistance[idx] + 1.0f;
                    q.push(nIdx);
                }
            }
        }

        float avgCell = (worldSize.x + worldSize.y) * 0.5f / glm::max(1, res - 1);
        for (float& dist : riverDistance)
        {
            if (dist >= std::numeric_limits<float>::max() * 0.5f)
                dist = settings.RiverDistanceFalloff;
            else
                dist = glm::min(dist * avgCell, settings.RiverDistanceFalloff);
        }
    }

    void SmoothRiverBanks(HeightField& field,
                          const std::vector<float>& riverDistance,
                          const WorldGenerationPanel::Settings& settings,
                          const glm::vec2& worldSize)
    {
        const int res = field.Resolution;
        if (res <= 2)
            return;
        float avgCell = (worldSize.x + worldSize.y) * 0.5f / glm::max(1, res - 1);
        float bankRadius = settings.RiverBankWidth;
        if (bankRadius <= kFloatEpsilon)
            return;

        HeightField copy = field;
        int blurRadius = 2;
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
                float distance = riverDistance[idx];
                float proximity = 1.0f - glm::clamp(distance / bankRadius, 0.0f, 1.0f);
                if (proximity <= 0.0f)
                    continue;
                float accum = 0.0f;
                int samples = 0;
                for (int dz = -blurRadius; dz <= blurRadius; ++dz)
                {
                    for (int dx = -blurRadius; dx <= blurRadius; ++dx)
                    {
                        int nx = x + dx;
                        int ny = y + dz;
                        if (nx < 0 || ny < 0 || nx >= res || ny >= res)
                            continue;
                        accum += copy.Get(nx, ny);
                        ++samples;
                    }
                }
                if (samples == 0)
                    continue;

                float avg = accum / static_cast<float>(samples);
                float factor = proximity * settings.RiverBankSmoothing;
                field.Set(x, y, glm::mix(copy.Get(x, y), avg, factor));
            }
        }
    }

    void ComputeSlopeDegrees(const HeightField& field,
                             const glm::vec2& worldSize,
                             float maxHeight,
                             std::vector<float>& outSlope)
    {
        const int res = field.Resolution;
        outSlope.resize(field.Values.size());
        glm::vec2 cellSize(
            worldSize.x / glm::max(1, res - 1),
            worldSize.y / glm::max(1, res - 1));
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                float hL = field.Get(std::max(0, x - 1), y);
                float hR = field.Get(std::min(res - 1, x + 1), y);
                float hD = field.Get(x, std::max(0, y - 1));
                float hU = field.Get(x, std::min(res - 1, y + 1));
                float dzdx = (hR - hL) * maxHeight / (2.0f * glm::max(cellSize.x, kFloatEpsilon));
                float dzdy = (hU - hD) * maxHeight / (2.0f * glm::max(cellSize.y, kFloatEpsilon));
                float slopeRad = std::atan(std::sqrt(dzdx * dzdx + dzdy * dzdy));
                outSlope[static_cast<size_t>(y) * res + static_cast<size_t>(x)] = glm::degrees(slopeRad);
            }
        }
    }

    void AssignBiomes(const HeightField& field,
                      const std::vector<float>& slopeDeg,
                      const std::vector<float>& riverDistance,
                      const std::vector<float>& riverMask,
                      const std::vector<float>& landMask,
                      const WorldGenerationPanel::Settings& settings,
                      TerrainComponent& terrain)
    {
        auto ensureLayer = [&](int idx, const char* name, const glm::vec3& color)
        {
            if (static_cast<int>(terrain.Layers.size()) <= idx)
                terrain.Layers.resize(idx + 1);
            terrain.Layers[idx].Name = name;
            terrain.Layers[idx].PlaceholderColor = color;
        };

        ensureLayer(0, "Valley Grass", glm::vec3(0.31f, 0.57f, 0.33f));
        ensureLayer(1, "Rocky Cliff", glm::vec3(0.55f, 0.53f, 0.50f));
        ensureLayer(2, "Sandy Flats", glm::vec3(0.76f, 0.67f, 0.45f));
        ensureLayer(3, "High Snow", glm::vec3(0.92f, 0.94f, 0.96f));

        const int res = field.Resolution;
        terrain.SplatMap.resize(field.Values.size());

        for (size_t i = 0; i < field.Values.size(); ++i)
        {
            float heightNorm = field.Values[i];
            float slope = slopeDeg.empty() ? 0.0f : slopeDeg[i];
            float riverDist = riverDistance.empty() ? settings.RiverDistanceFalloff : riverDistance[i];
            float riverStrength = riverMask.empty() ? 0.0f : riverMask[i];
            float land = landMask.empty() ? 1.0f : landMask[i];
            float riverProximity = 1.0f - glm::clamp(riverDist / settings.RiverDistanceFalloff, 0.0f, 1.0f);

            float sand = glm::clamp((1.0f - slope / glm::max(settings.SandSlopeMax, 0.1f)) *
                                    glm::smoothstep(0.0f, settings.SandHeightMax, 1.0f - heightNorm) *
                                    (0.5f + riverProximity * 0.5f), 0.0f, 1.0f);

            float cliff = glm::smoothstep(settings.CliffSlopeStart, settings.CliffSlopeMax, slope);
            cliff *= glm::mix(0.4f, 1.0f, heightNorm);

            float snow = glm::smoothstep(settings.SnowHeightStart, settings.SnowHeightFull, heightNorm);

            float grass = glm::clamp((1.0f - cliff) * (1.0f - snow) * (1.0f - sand), 0.0f, 1.0f);
            grass *= (0.6f + settings.ValleyGrassBoost * riverProximity);
            grass *= land;

            float rock = glm::clamp(cliff * (1.0f - snow), 0.0f, 1.0f);
            float valleySand = sand * (1.0f - snow);
            float snowLayer = glm::clamp(snow, 0.0f, 1.0f);

            float sum = grass + rock + valleySand + snowLayer;
            if (sum <= kFloatEpsilon)
            {
                grass = land;
                sum = land;
            }

            grass /= sum;
            rock /= sum;
            valleySand /= sum;
            snowLayer /= sum;

            glm::u8vec4 packed(
                static_cast<uint8_t>(glm::round(glm::clamp(grass, 0.0f, 1.0f) * 255.0f)),
                static_cast<uint8_t>(glm::round(glm::clamp(rock, 0.0f, 1.0f) * 255.0f)),
                static_cast<uint8_t>(glm::round(glm::clamp(valleySand, 0.0f, 1.0f) * 255.0f)),
                0);
            int remaining = 255 - packed.r - packed.g - packed.b;
            packed.a = static_cast<uint8_t>(glm::clamp(remaining, 0, 255));
            terrain.SplatMap[i] = packed;
        }
    }

    void BuildChunkSummaries(const HeightField& field,
                             const std::vector<float>& slopeDeg,
                             const std::vector<float>& landMask,
                             const std::vector<float>& riverMask,
                             const WorldGenerationPanel::Settings& settings,
                             float maxHeight,
                             std::vector<TerrainChunkSummary>& outSummaries)
    {
        const int res = field.Resolution;
        const int chunkRes = std::max(8, std::min(settings.ChunkResolution, res));
        const int stride = chunkRes;
        const int chunkCountX = (res + stride - 1) / stride;
        const int chunkCountY = (res + stride - 1) / stride;
        outSummaries.clear();
        outSummaries.reserve(chunkCountX * chunkCountY);

        for (int cy = 0; cy < chunkCountY; ++cy)
        {
            for (int cx = 0; cx < chunkCountX; ++cx)
            {
                int startX = cx * stride;
                int startY = cy * stride;
                int endX = std::min(res - 1, startX + stride - 1);
                int endY = std::min(res - 1, startY + stride - 1);
                float minHeightNorm = 1.0f;
                float maxHeightNorm = 0.0f;
                float slopeAccum = 0.0f;
                float landCount = 0.0f;
                float riverCount = 0.0f;
                int sampleCount = 0;
                for (int y = startY; y <= endY; ++y)
                {
                    for (int x = startX; x <= endX; ++x)
                    {
                        size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
                        float h = field.Values[idx];
                        minHeightNorm = std::min(minHeightNorm, h);
                        maxHeightNorm = std::max(maxHeightNorm, h);
                        slopeAccum += slopeDeg.empty() ? 0.0f : slopeDeg[idx];
                        landCount += landMask.empty() ? 1.0f : (landMask[idx] > 0.5f ? 1.0f : 0.0f);
                        riverCount += riverMask.empty() ? 0.0f : (riverMask[idx] > 0.2f ? 1.0f : 0.0f);
                        ++sampleCount;
                    }
                }

                TerrainChunkSummary summary;
                summary.chunkX = cx;
                summary.chunkY = cy;
                summary.start = glm::ivec2(startX, startY);
                summary.size = glm::ivec2(endX - startX + 1, endY - startY + 1);
                summary.minHeight = minHeightNorm * maxHeight;
                summary.maxHeight = maxHeightNorm * maxHeight;
                summary.avgSlope = sampleCount > 0 ? slopeAccum / static_cast<float>(sampleCount) : 0.0f;
                summary.landCoverage = sampleCount > 0 ? landCount / static_cast<float>(sampleCount) : 0.0f;
                summary.riverCoverage = sampleCount > 0 ? riverCount / static_cast<float>(sampleCount) : 0.0f;
                outSummaries.push_back(summary);
            }
        }
    }

    // Compute directional weights for region-based generation
    struct RegionWeights
    {
        float north = 0.0f;
        float south = 0.0f;
        float east = 0.0f;
        float west = 0.0f;
        float center = 0.0f;
    };

    RegionWeights ComputeRegionWeights(const glm::vec2& uv, float centerRadius, float blendSharpness)
    {
        RegionWeights w;
        glm::vec2 centered = uv - glm::vec2(0.5f);
        
        // Distance from center (0 at center, 0.5 at edges, ~0.7 at corners)
        float distFromCenter = glm::length(centered);
        
        // Center weight - strong in the middle, fades outward
        float centerFalloff = glm::max(0.08f, centerRadius);
        w.center = 1.0f - glm::smoothstep(centerFalloff * 0.6f, centerFalloff * 1.2f, distFromCenter);
        w.center = std::pow(glm::max(0.0f, w.center), glm::mix(0.6f, 1.5f, blendSharpness));
        
        // Compute directional weights based on angle, not just axis projection
        // This creates proper "pie slice" regions instead of axis-aligned bands
        float outerInfluence = 1.0f - w.center;
        
        if (outerInfluence > kFloatEpsilon && distFromCenter > kFloatEpsilon)
        {
            // Get angle from center (0 = east, PI/2 = north, PI = west, -PI/2 = south)
            float angle = std::atan2(centered.y, centered.x);
            
            // Convert to 0-1 range for each cardinal direction
            // Each direction has a 90-degree cone of influence
            float pi = glm::pi<float>();
            float halfPi = pi * 0.5f;
            
            // Blend width depends on sharpness (smaller = sharper transitions)
            float blendWidth = glm::mix(0.8f, 0.3f, blendSharpness);
            
            // North: angle near +PI/2
            float northAngle = std::abs(angle - halfPi);
            w.north = 1.0f - glm::smoothstep(0.0f, halfPi * blendWidth, northAngle);
            
            // South: angle near -PI/2  
            float southAngle = std::abs(angle + halfPi);
            w.south = 1.0f - glm::smoothstep(0.0f, halfPi * blendWidth, southAngle);
            
            // East: angle near 0
            float eastAngle = std::abs(angle);
            w.east = 1.0f - glm::smoothstep(0.0f, halfPi * blendWidth, eastAngle);
            
            // West: angle near +/-PI
            float westAngle = pi - std::abs(angle);
            w.west = 1.0f - glm::smoothstep(0.0f, halfPi * blendWidth, westAngle);
            
            // Scale by distance from center (stronger at edges)
            float edgeStrength = glm::smoothstep(centerFalloff * 0.5f, 0.45f, distFromCenter);
            edgeStrength = glm::mix(edgeStrength, 1.0f, 0.3f); // Always some influence
            
            w.north *= edgeStrength * outerInfluence;
            w.south *= edgeStrength * outerInfluence;
            w.east *= edgeStrength * outerInfluence;
            w.west *= edgeStrength * outerInfluence;
        }
        
        // Normalize so weights sum to 1
        float sum = w.north + w.south + w.east + w.west + w.center;
        if (sum > kFloatEpsilon)
        {
            float invSum = 1.0f / sum;
            w.north *= invSum;
            w.south *= invSum;
            w.east *= invSum;
            w.west *= invSum;
            w.center *= invSum;
        }
        else
        {
            // Fallback - at dead center
            w.center = 1.0f;
        }
        
        return w;
    }

    // Generate continental shape mask with irregular coastline
    float ComputeLandMask(const glm::vec2& uv, float borderWidth, float coastComplexity, uint32_t seed)
    {
        glm::vec2 centered = (uv - glm::vec2(0.5f)) * 2.0f;
        float baseRadius = 1.0f - borderWidth * 2.0f;
        
        // Base elliptical shape with slight aspect ratio variation
        float aspectNoise = GradientNoise(uv * 0.5f, seed + 777u) * 0.15f;
        glm::vec2 scaled = centered / glm::vec2(baseRadius + aspectNoise, baseRadius - aspectNoise);
        float dist = glm::length(scaled);
        
        // Add coastline complexity via domain warping
        if (coastComplexity > kFloatEpsilon)
        {
            glm::vec2 warp = DomainWarp(uv, seed, coastComplexity * 0.3f, 2.0f);
            float coastNoise = FBM((uv + warp) * 3.0f, 4, 2.0f, 0.5f, seed + 111u);
            dist += (coastNoise - 0.5f) * coastComplexity * 0.4f;
        }
        
        // Smooth transition from land to sea
        return 1.0f - glm::smoothstep(0.85f, 1.05f, dist);
    }

    // Blend zone properties based on region weights
    ZoneProperties BlendZoneProperties(const RegionWeights& w,
                                       TerrainZone north, TerrainZone south,
                                       TerrainZone east, TerrainZone west,
                                       TerrainZone center)
    {
        ZoneProperties pN = GetZoneProperties(north);
        ZoneProperties pS = GetZoneProperties(south);
        ZoneProperties pE = GetZoneProperties(east);
        ZoneProperties pW = GetZoneProperties(west);
        ZoneProperties pC = GetZoneProperties(center);
        
        ZoneProperties result;
        result.BaseElevation = pN.BaseElevation * w.north + pS.BaseElevation * w.south +
                              pE.BaseElevation * w.east + pW.BaseElevation * w.west +
                              pC.BaseElevation * w.center;
        result.ElevationVariation = pN.ElevationVariation * w.north + pS.ElevationVariation * w.south +
                                   pE.ElevationVariation * w.east + pW.ElevationVariation * w.west +
                                   pC.ElevationVariation * w.center;
        result.Roughness = pN.Roughness * w.north + pS.Roughness * w.south +
                          pE.Roughness * w.east + pW.Roughness * w.west +
                          pC.Roughness * w.center;
        result.RidgeStrength = pN.RidgeStrength * w.north + pS.RidgeStrength * w.south +
                              pE.RidgeStrength * w.east + pW.RidgeStrength * w.west +
                              pC.RidgeStrength * w.center;
        result.TerraceStrength = pN.TerraceStrength * w.north + pS.TerraceStrength * w.south +
                                pE.TerraceStrength * w.east + pW.TerraceStrength * w.west +
                                pC.TerraceStrength * w.center;
        
        // Rivers allowed if any weighted zone allows them (with sufficient weight)
        float riverWeight = (pN.AllowRivers ? w.north : 0.0f) + (pS.AllowRivers ? w.south : 0.0f) +
                           (pE.AllowRivers ? w.east : 0.0f) + (pW.AllowRivers ? w.west : 0.0f) +
                           (pC.AllowRivers ? w.center : 0.0f);
        result.AllowRivers = riverWeight > 0.3f;
        
        return result;
    }
} // namespace

WorldGenerationPanel::WorldGenerationPanel(Scene* scene, EntityID* selectedEntity)
    : m_SelectedEntity(selectedEntity)
{
    SetContext(scene);
    m_Settings.WorldSize = WorldSizeFromPreset(m_Settings.SizePreset);
}

void WorldGenerationPanel::Open()
{
    SyncSelectionTarget();
    m_Open = true;
}

void WorldGenerationPanel::SyncSelectionTarget()
{
    if (m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID && *m_SelectedEntity != 0)
    {
        m_TargetTerrain = *m_SelectedEntity;
    }
}

bool WorldGenerationPanel::HasValidTarget() const
{
    if (!m_Context)
        return false;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return false;
    EntityData* data = m_Context->GetEntityData(m_TargetTerrain);
    return data && data->Terrain;
}

EntityData* WorldGenerationPanel::GetTargetEntityData() const
{
    if (!m_Context)
        return nullptr;
    if (m_TargetTerrain == INVALID_ENTITY_ID || m_TargetTerrain == 0)
        return nullptr;
    return m_Context->GetEntityData(m_TargetTerrain);
}

void WorldGenerationPanel::OnImGuiRender()
{
    if (!m_Open)
        return;

    ImGui::SetNextWindowSize(ImVec2(480.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("World Generation", &m_Open))
    {
        ImGui::End();
        return;
    }

    DrawTargetSelector();
    ImGui::Spacing();
    DrawModeSelector();
    ImGui::Spacing();
    
    // Draw the appropriate UI based on mode
    switch (m_Mode)
    {
    case GeneratorMode::Simple:
        DrawSimpleModeUI();
        break;
    case GeneratorMode::Region:
        DrawRegionModeUI();
        break;
    case GeneratorMode::Freeform:
        DrawFreeformModeUI();
        break;
    case GeneratorMode::Advanced:
        DrawAdvancedModeUI();
        break;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool canGenerate = HasValidTarget();
    ImGui::BeginDisabled(!canGenerate);
    if (ImGui::Button("Generate Terrain", ImVec2(-FLT_MIN, 32.0f)))
    {
        RunGeneration();
    }
    ImGui::EndDisabled();

    if (!canGenerate)
    {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "Select an entity with a Terrain component.");
    }

    if (!m_StatusLine.empty())
    {
        ImGui::TextWrapped("%s", m_StatusLine.c_str());
    }
    if (m_LastGenerationMs > 0.0)
    {
        ImGui::Text("Last generation: %.1f ms", m_LastGenerationMs);
    }

    if (m_Mode == GeneratorMode::Advanced)
    {
        DrawChunkTable();
    }

    ImGui::End();
}

void WorldGenerationPanel::DrawTargetSelector()
{
    if (!m_Context)
    {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Scene context unavailable.");
        return;
    }

    std::vector<std::pair<EntityID, std::string>> terrains;
    for (const Entity& entity : m_Context->GetEntities())
    {
        EntityData* data = m_Context->GetEntityData(entity.GetID());
        if (data && data->Terrain)
        {
            terrains.emplace_back(entity.GetID(), data->Name);
        }
    }

    if (terrains.empty())
    {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "No terrain entities in this scene.");
        m_TargetTerrain = INVALID_ENTITY_ID;
        return;
    }

    const char* currentLabel = "<none>";
    bool foundMatch = false;
    for (auto& entry : terrains)
    {
        if (entry.first == m_TargetTerrain)
        {
            currentLabel = entry.second.c_str();
            foundMatch = true;
            break;
        }
    }
    if (!foundMatch && !terrains.empty())
    {
        m_TargetTerrain = terrains.front().first;
        currentLabel = terrains.front().second.c_str();
    }

    if (ImGui::BeginCombo("Terrain Entity", currentLabel))
    {
        for (auto& entry : terrains)
        {
            bool selected = entry.first == m_TargetTerrain;
            if (ImGui::Selectable(entry.second.c_str(), selected))
            {
                m_TargetTerrain = entry.first;
            }
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    if (ImGui::Button("Use Selected") && m_SelectedEntity && *m_SelectedEntity != INVALID_ENTITY_ID)
    {
        m_TargetTerrain = *m_SelectedEntity;
    }
    ImGui::SameLine();
    if (ImGui::Button("Randomize Seeds"))
    {
        RandomizeSeeds();
    }
}

void WorldGenerationPanel::DrawModeSelector()
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    
    float buttonWidth = (ImGui::GetContentRegionAvail().x - 12.0f) / 4.0f;
    
    for (int i = 0; i < 4; ++i)
    {
        if (i > 0) ImGui::SameLine();
        
        bool isActive = (static_cast<int>(m_Mode) == i);
        if (isActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        
        if (ImGui::Button(kModeNames[i], ImVec2(buttonWidth, 0.0f)))
        {
            m_Mode = static_cast<GeneratorMode>(i);
        }
        
        if (isActive)
        {
            ImGui::PopStyleColor();
        }
    }
    
    ImGui::PopStyleVar();
    
    // Mode description
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
    switch (m_Mode)
    {
    case GeneratorMode::Simple:
        ImGui::TextWrapped("Pick a world template and generate. Quick and easy.");
        break;
    case GeneratorMode::Region:
        ImGui::TextWrapped("Define terrain types for each region of your world.");
        break;
    case GeneratorMode::Freeform:
        ImGui::TextWrapped("Draw your coastline by hand and stamp terrain features.");
        break;
    case GeneratorMode::Advanced:
        ImGui::TextWrapped("Full control over all generation parameters.");
        break;
    }
    ImGui::PopStyleColor();
}

void WorldGenerationPanel::DrawSimpleModeUI()
{
    ImGui::SeparatorText("World Template");
    
    int templateIdx = static_cast<int>(m_RegionSettings.Template);
    if (ImGui::Combo("Template", &templateIdx, kTemplateNames, static_cast<int>(WorldTemplate::Count)))
    {
        m_RegionSettings.Template = static_cast<WorldTemplate>(templateIdx);
        if (m_RegionSettings.Template != WorldTemplate::Custom)
        {
            ApplyTemplate(m_RegionSettings.Template);
        }
    }
    
    // Show template description
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 0.7f, 1.0f));
    switch (m_RegionSettings.Template)
    {
    case WorldTemplate::Continental:
        ImGui::TextWrapped("A natural continent with mountains to the north, a fertile central valley, beaches to the west, and dramatic cliffs to the south.");
        break;
    case WorldTemplate::IslandKingdom:
        ImGui::TextWrapped("A large island with highland interior surrounded by sandy beaches on all sides.");
        break;
    case WorldTemplate::MountainFortress:
        ImGui::TextWrapped("An impenetrable realm - towering mountains on all borders protecting a peaceful valley within.");
        break;
    case WorldTemplate::GrandCanyon:
        ImGui::TextWrapped("A massive central canyon/pit with highlands surrounding it. Dramatic elevation changes.");
        break;
    case WorldTemplate::CoastalRange:
        ImGui::TextWrapped("A mountain range along the western edge descending into rolling plains and beaches to the east.");
        break;
    case WorldTemplate::RiftValley:
        ImGui::TextWrapped("Mountains to the north and south with a deep valley corridor running east to west between them.");
        break;
    case WorldTemplate::Archipelago:
        ImGui::TextWrapped("Scattered highland regions with wetlands between - like multiple islands close together.");
        break;
    case WorldTemplate::HighlandPlains:
        ImGui::TextWrapped("Gentle rolling terrain throughout - peaceful and easy to traverse.");
        break;
    case WorldTemplate::Custom:
        ImGui::TextWrapped("Switch to Region mode to customize your world layout.");
        break;
    default:
        break;
    }
    ImGui::PopStyleColor();
    
    ImGui::Spacing();
    ImGui::SeparatorText("Quick Settings");
    
    // Size preset
    static const char* kSizeLabels[] = { "Small (2 km²)", "Medium (6 km²)", "Large (12 km²)", "Huge (20 km²)", "Custom" };
    int presetIndex = static_cast<int>(m_Settings.SizePreset);
    if (ImGui::Combo("World Size", &presetIndex, kSizeLabels, IM_ARRAYSIZE(kSizeLabels)))
    {
        m_Settings.SizePreset = static_cast<TerrainSizePreset>(presetIndex);
        if (m_Settings.SizePreset != TerrainSizePreset::Custom)
        {
            m_Settings.WorldSize = WorldSizeFromPreset(m_Settings.SizePreset);
        }
    }
    
    ImGui::SliderFloat("Feature Intensity", &m_RegionSettings.FeatureIntensity, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How dramatic the terrain features are (mountains taller, canyons deeper)");
    
    ImGui::SliderFloat("Terrain Smoothness", &m_RegionSettings.BaselineSmoothness, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Higher = smoother terrain, Lower = more rugged");
}

void WorldGenerationPanel::DrawRegionModeUI()
{
    ImGui::SeparatorText("Region Layout");
    
    // Visual region selector
    float availWidth = ImGui::GetContentRegionAvail().x;
    float comboWidth = availWidth * 0.35f;
    float centerPad = (availWidth - comboWidth) * 0.5f;
    
    // North
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerPad);
    ImGui::SetNextItemWidth(comboWidth);
    int northIdx = static_cast<int>(m_RegionSettings.North);
    if (ImGui::Combo("##North", &northIdx, kZoneNames, static_cast<int>(TerrainZone::Count)))
    {
        m_RegionSettings.North = static_cast<TerrainZone>(northIdx);
        m_RegionSettings.Template = WorldTemplate::Custom;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("North");
    
    // West - Center - East row
    ImGui::SetNextItemWidth(comboWidth);
    int westIdx = static_cast<int>(m_RegionSettings.West);
    if (ImGui::Combo("##West", &westIdx, kZoneNames, static_cast<int>(TerrainZone::Count)))
    {
        m_RegionSettings.West = static_cast<TerrainZone>(westIdx);
        m_RegionSettings.Template = WorldTemplate::Custom;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("W");
    
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    int centerIdx = static_cast<int>(m_RegionSettings.Center);
    if (ImGui::Combo("##Center", &centerIdx, kZoneNames, static_cast<int>(TerrainZone::Count)))
    {
        m_RegionSettings.Center = static_cast<TerrainZone>(centerIdx);
        m_RegionSettings.Template = WorldTemplate::Custom;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Center");
    
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboWidth);
    int eastIdx = static_cast<int>(m_RegionSettings.East);
    if (ImGui::Combo("##East", &eastIdx, kZoneNames, static_cast<int>(TerrainZone::Count)))
    {
        m_RegionSettings.East = static_cast<TerrainZone>(eastIdx);
        m_RegionSettings.Template = WorldTemplate::Custom;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("E");
    
    // South
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centerPad);
    ImGui::SetNextItemWidth(comboWidth);
    int southIdx = static_cast<int>(m_RegionSettings.South);
    if (ImGui::Combo("##South", &southIdx, kZoneNames, static_cast<int>(TerrainZone::Count)))
    {
        m_RegionSettings.South = static_cast<TerrainZone>(southIdx);
        m_RegionSettings.Template = WorldTemplate::Custom;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("South");
    
    ImGui::Spacing();
    ImGui::SeparatorText("Region Blending");
    
    ImGui::SliderFloat("Center Size", &m_RegionSettings.CenterRadius, 0.05f, 0.5f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much of the map the center region covers");
    
    ImGui::SliderFloat("Blend Sharpness", &m_RegionSettings.RegionBlendSharpness, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("0 = soft gradual transitions, 1 = sharper region boundaries");
    
    ImGui::Spacing();
    ImGui::SeparatorText("Terrain Settings");
    
    // Size preset
    static const char* kSizeLabels[] = { "Small (2 km²)", "Medium (6 km²)", "Large (12 km²)", "Huge (20 km²)", "Custom" };
    int presetIndex = static_cast<int>(m_Settings.SizePreset);
    if (ImGui::Combo("World Size", &presetIndex, kSizeLabels, IM_ARRAYSIZE(kSizeLabels)))
    {
        m_Settings.SizePreset = static_cast<TerrainSizePreset>(presetIndex);
        if (m_Settings.SizePreset != TerrainSizePreset::Custom)
        {
            m_Settings.WorldSize = WorldSizeFromPreset(m_Settings.SizePreset);
        }
    }
    
    ImGui::SliderFloat("Max Height (m)", &m_Settings.MaxHeight, 50.0f, 500.0f, "%.0f");
    ImGui::SliderFloat("Feature Intensity", &m_RegionSettings.FeatureIntensity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Local Variation", &m_RegionSettings.LocalVariation, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How much variation within each region");
    
    ImGui::SliderFloat("Terrain Smoothness", &m_RegionSettings.BaselineSmoothness, 0.0f, 1.0f, "%.2f");
    
    ImGui::Spacing();
    ImGui::SeparatorText("Continental Shape");
    
    ImGui::Checkbox("Ocean Border", &m_RegionSettings.OceanBorder);
    if (m_RegionSettings.OceanBorder)
    {
        ImGui::SliderFloat("Border Width", &m_RegionSettings.OceanBorderWidth, 0.02f, 0.3f, "%.2f");
        ImGui::SliderFloat("Coast Complexity", &m_RegionSettings.CoastComplexity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("0 = smooth oval coast, 1 = irregular fractal coastline");
    }
    
    ImGui::Spacing();
    ImGui::SeparatorText("Rivers");
    
    ImGui::Checkbox("Generate Rivers", &m_RegionSettings.GenerateRivers);
    if (m_RegionSettings.GenerateRivers)
    {
        ImGui::SliderFloat("River Density", &m_RegionSettings.RiverDensity, 0.05f, 0.6f, "%.2f");
    }
}

void WorldGenerationPanel::DrawAdvancedModeUI()
{
    ImGui::SeparatorText("Pipeline Settings");
    DrawBaseSettings();
    DrawFeatureSettings();
    DrawRiverSettings();
    DrawBiomeSettings();
    DrawChunkSettings();
}

void WorldGenerationPanel::ApplyTemplate(WorldTemplate tmpl)
{
    switch (tmpl)
    {
    case WorldTemplate::Continental:
        m_RegionSettings.North = TerrainZone::Mountains;
        m_RegionSettings.South = TerrainZone::Cliffs;
        m_RegionSettings.East = TerrainZone::Highlands;
        m_RegionSettings.West = TerrainZone::Beach;
        m_RegionSettings.Center = TerrainZone::Valley;
        m_RegionSettings.CenterRadius = 0.28f;
        m_RegionSettings.RegionBlendSharpness = 0.35f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.1f;
        m_RegionSettings.CoastComplexity = 0.4f;
        break;
        
    case WorldTemplate::IslandKingdom:
        m_RegionSettings.North = TerrainZone::Beach;
        m_RegionSettings.South = TerrainZone::Beach;
        m_RegionSettings.East = TerrainZone::Beach;
        m_RegionSettings.West = TerrainZone::Beach;
        m_RegionSettings.Center = TerrainZone::Highlands;
        m_RegionSettings.CenterRadius = 0.4f;
        m_RegionSettings.RegionBlendSharpness = 0.25f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.15f;
        m_RegionSettings.CoastComplexity = 0.5f;
        break;
        
    case WorldTemplate::MountainFortress:
        m_RegionSettings.North = TerrainZone::Mountains;
        m_RegionSettings.South = TerrainZone::Mountains;
        m_RegionSettings.East = TerrainZone::Mountains;
        m_RegionSettings.West = TerrainZone::Mountains;
        m_RegionSettings.Center = TerrainZone::Valley;
        m_RegionSettings.CenterRadius = 0.35f;
        m_RegionSettings.RegionBlendSharpness = 0.5f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.08f;
        m_RegionSettings.CoastComplexity = 0.3f;
        break;
        
    case WorldTemplate::GrandCanyon:
        m_RegionSettings.North = TerrainZone::Highlands;
        m_RegionSettings.South = TerrainZone::Highlands;
        m_RegionSettings.East = TerrainZone::Plateau;
        m_RegionSettings.West = TerrainZone::Plateau;
        m_RegionSettings.Center = TerrainZone::Canyon;
        m_RegionSettings.CenterRadius = 0.4f;
        m_RegionSettings.RegionBlendSharpness = 0.45f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.1f;
        m_RegionSettings.CoastComplexity = 0.35f;
        m_RegionSettings.FeatureIntensity = 0.7f;
        break;
        
    case WorldTemplate::CoastalRange:
        m_RegionSettings.North = TerrainZone::Highlands;
        m_RegionSettings.South = TerrainZone::Plains;
        m_RegionSettings.East = TerrainZone::Beach;
        m_RegionSettings.West = TerrainZone::Mountains;
        m_RegionSettings.Center = TerrainZone::Plains;
        m_RegionSettings.CenterRadius = 0.25f;
        m_RegionSettings.RegionBlendSharpness = 0.3f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.12f;
        m_RegionSettings.CoastComplexity = 0.45f;
        break;
        
    case WorldTemplate::RiftValley:
        m_RegionSettings.North = TerrainZone::Mountains;
        m_RegionSettings.South = TerrainZone::Mountains;
        m_RegionSettings.East = TerrainZone::Plains;
        m_RegionSettings.West = TerrainZone::Plains;
        m_RegionSettings.Center = TerrainZone::Valley;
        m_RegionSettings.CenterRadius = 0.2f;
        m_RegionSettings.RegionBlendSharpness = 0.4f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.1f;
        m_RegionSettings.CoastComplexity = 0.3f;
        break;
        
    case WorldTemplate::Archipelago:
        m_RegionSettings.North = TerrainZone::Wetlands;
        m_RegionSettings.South = TerrainZone::Wetlands;
        m_RegionSettings.East = TerrainZone::Highlands;
        m_RegionSettings.West = TerrainZone::Highlands;
        m_RegionSettings.Center = TerrainZone::Wetlands;
        m_RegionSettings.CenterRadius = 0.2f;
        m_RegionSettings.RegionBlendSharpness = 0.2f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.05f;
        m_RegionSettings.CoastComplexity = 0.7f;
        break;
        
    case WorldTemplate::HighlandPlains:
        m_RegionSettings.North = TerrainZone::Highlands;
        m_RegionSettings.South = TerrainZone::Plains;
        m_RegionSettings.East = TerrainZone::Plains;
        m_RegionSettings.West = TerrainZone::Highlands;
        m_RegionSettings.Center = TerrainZone::Plains;
        m_RegionSettings.CenterRadius = 0.35f;
        m_RegionSettings.RegionBlendSharpness = 0.2f;
        m_RegionSettings.OceanBorder = true;
        m_RegionSettings.OceanBorderWidth = 0.1f;
        m_RegionSettings.CoastComplexity = 0.35f;
        m_RegionSettings.FeatureIntensity = 0.3f;
        break;
        
    default:
        break;
    }
}

void WorldGenerationPanel::DrawFreeformModeUI()
{
    // Size preset at top
    static const char* kSizeLabels[] = { "Small (2 km²)", "Medium (6 km²)", "Large (12 km²)", "Huge (20 km²)", "Custom" };
    int presetIndex = static_cast<int>(m_Settings.SizePreset);
    if (ImGui::Combo("World Size", &presetIndex, kSizeLabels, IM_ARRAYSIZE(kSizeLabels)))
    {
        m_Settings.SizePreset = static_cast<TerrainSizePreset>(presetIndex);
        if (m_Settings.SizePreset != TerrainSizePreset::Custom)
        {
            m_Settings.WorldSize = WorldSizeFromPreset(m_Settings.SizePreset);
        }
    }
    
    ImGui::SliderFloat("Max Height (m)", &m_Settings.MaxHeight, 50.0f, 500.0f, "%.0f");
    
    ImGui::Spacing();
    ImGui::SeparatorText("Coastline Drawing");
    
    // Drawing controls
    bool hasPolygon = m_FreeformSettings.Coastline.IsValid();
    
    if (!m_IsDrawingPolygon)
    {
        if (ImGui::Button(hasPolygon ? "Redraw Coastline" : "Start Drawing", ImVec2(150, 0)))
        {
            m_FreeformSettings.Coastline.Clear();
            m_IsDrawingPolygon = true;
            m_IsStamping = false;
        }
        if (hasPolygon)
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear All"))
            {
                m_FreeformSettings.Coastline.Clear();
                m_FreeformSettings.Stamps.clear();
            }
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Click to add points. Click near start to close.");
        if (ImGui::Button("Cancel Drawing"))
        {
            m_FreeformSettings.Coastline.Clear();
            m_IsDrawingPolygon = false;
        }
        ImGui::SameLine();
        if (m_FreeformSettings.Coastline.Points.size() >= 3)
        {
            if (ImGui::Button("Close Polygon"))
            {
                m_FreeformSettings.Coastline.IsClosed = true;
                m_IsDrawingPolygon = false;
            }
        }
    }
    
    // The drawing canvas
    DrawPolygonCanvas();
    
    // Feature stamp controls (only if we have a valid polygon)
    if (hasPolygon)
    {
        DrawFeatureStampControls();
    }
    
    ImGui::Spacing();
    ImGui::SeparatorText("Terrain Settings");
    
    ImGui::SliderFloat("Base Elevation", &m_FreeformSettings.BaseElevation, 0.2f, 0.5f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Base height of land inside the coastline");
    
    ImGui::SliderFloat("Shore Width", &m_FreeformSettings.ShoreWidth, 0.02f, 0.15f, "%.3f");
    ImGui::SliderFloat("Shore Noise", &m_FreeformSettings.ShoreNoiseStrength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Cliff Chance", &m_FreeformSettings.CliffChance, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Probability of cliffs vs gradual beaches at shore");
    
    ImGui::SliderFloat("Terrain Roughness", &m_FreeformSettings.TerrainRoughness, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Feature Intensity", &m_FreeformSettings.FeatureIntensity, 0.0f, 1.0f, "%.2f");
    
    ImGui::Spacing();
    ImGui::SeparatorText("Auto-Generation");
    
    ImGui::Checkbox("Auto Hills", &m_FreeformSettings.AutoGenerateHills);
    if (m_FreeformSettings.AutoGenerateHills)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("##HillCount", &m_FreeformSettings.AutoHillCount, 0, 20, "%d hills");
    }
    
    ImGui::Checkbox("Auto Rivers", &m_FreeformSettings.AutoGenerateRivers);
    if (m_FreeformSettings.AutoGenerateRivers)
    {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::SliderInt("##RiverCount", &m_FreeformSettings.AutoRiverCount, 0, 8, "%d rivers");
    }
}

void WorldGenerationPanel::DrawPolygonCanvas()
{
    ImGui::Spacing();
    
    // Canvas size
    float canvasSize = glm::min(ImGui::GetContentRegionAvail().x, 300.0f);
    ImVec2 canvasSizeVec(canvasSize, canvasSize);
    
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Draw canvas background
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize, canvasPos.y + canvasSize),
                           IM_COL32(30, 50, 70, 255));
    drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize, canvasPos.y + canvasSize),
                     IM_COL32(80, 100, 120, 255));
    
    // Draw grid
    int gridLines = 8;
    float gridStep = canvasSize / static_cast<float>(gridLines);
    for (int i = 1; i < gridLines; ++i)
    {
        float pos = i * gridStep;
        drawList->AddLine(ImVec2(canvasPos.x + pos, canvasPos.y),
                         ImVec2(canvasPos.x + pos, canvasPos.y + canvasSize),
                         IM_COL32(60, 80, 100, 100));
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + pos),
                         ImVec2(canvasPos.x + canvasSize, canvasPos.y + pos),
                         IM_COL32(60, 80, 100, 100));
    }
    
    // Helper to convert UV (0-1) to screen position
    auto uvToScreen = [&](const glm::vec2& uv) -> ImVec2 {
        return ImVec2(canvasPos.x + uv.x * canvasSize, canvasPos.y + (1.0f - uv.y) * canvasSize);
    };
    
    // Helper to convert screen position to UV
    auto screenToUV = [&](const ImVec2& screen) -> glm::vec2 {
        return glm::vec2((screen.x - canvasPos.x) / canvasSize,
                        1.0f - (screen.y - canvasPos.y) / canvasSize);
    };
    
    const CoastlinePolygon& poly = m_FreeformSettings.Coastline;
    
    // Draw the polygon fill if closed
    if (poly.IsValid() && poly.Points.size() >= 3)
    {
        std::vector<ImVec2> screenPoints;
        screenPoints.reserve(poly.Points.size());
        for (const glm::vec2& p : poly.Points)
        {
            screenPoints.push_back(uvToScreen(p));
        }
        drawList->AddConvexPolyFilled(screenPoints.data(), static_cast<int>(screenPoints.size()),
                                      IM_COL32(60, 120, 60, 100));
    }
    
    // Draw polygon edges
    if (poly.Points.size() >= 2)
    {
        for (size_t i = 0; i < poly.Points.size(); ++i)
        {
            size_t next = (i + 1) % poly.Points.size();
            if (!poly.IsClosed && next == 0) continue;
            
            ImVec2 p1 = uvToScreen(poly.Points[i]);
            ImVec2 p2 = uvToScreen(poly.Points[next]);
            drawList->AddLine(p1, p2, IM_COL32(100, 200, 100, 255), 2.0f);
        }
    }
    
    // Draw polygon vertices
    m_HoveredVertex = -1;
    ImVec2 mousePos = ImGui::GetMousePos();
    for (size_t i = 0; i < poly.Points.size(); ++i)
    {
        ImVec2 screenP = uvToScreen(poly.Points[i]);
        float dist = std::sqrt((mousePos.x - screenP.x) * (mousePos.x - screenP.x) +
                              (mousePos.y - screenP.y) * (mousePos.y - screenP.y));
        bool hovered = dist < 8.0f;
        if (hovered) m_HoveredVertex = static_cast<int>(i);
        
        ImU32 color = (i == 0) ? IM_COL32(255, 200, 100, 255) :  // First point is gold
                      hovered ? IM_COL32(255, 255, 255, 255) :
                               IM_COL32(150, 255, 150, 255);
        drawList->AddCircleFilled(screenP, hovered ? 6.0f : 4.0f, color);
    }
    
    // Draw feature stamps
    for (size_t i = 0; i < m_FreeformSettings.Stamps.size(); ++i)
    {
        const TerrainFeatureStamp& stamp = m_FreeformSettings.Stamps[i];
        ImVec2 screenP = uvToScreen(stamp.Position);
        float screenRadius = stamp.Radius * canvasSize;
        
        ImU32 color = GetStampColor(stamp.Type);
        drawList->AddCircleFilled(screenP, screenRadius, color);
        drawList->AddCircle(screenP, screenRadius, IM_COL32(255, 255, 255, 150), 0, 1.5f);
        
        // Draw stamp type indicator
        const char* label = kStampNames[static_cast<int>(stamp.Type)];
        ImVec2 textSize = ImGui::CalcTextSize(label);
        drawList->AddText(ImVec2(screenP.x - textSize.x * 0.5f, screenP.y - textSize.y * 0.5f),
                         IM_COL32(255, 255, 255, 200), label);
    }
    
    // Draw current brush preview when stamping
    if (m_IsStamping && !m_IsDrawingPolygon && poly.IsValid())
    {
        bool inCanvas = mousePos.x >= canvasPos.x && mousePos.x <= canvasPos.x + canvasSize &&
                       mousePos.y >= canvasPos.y && mousePos.y <= canvasPos.y + canvasSize;
        if (inCanvas)
        {
            float brushRadius = m_FreeformSettings.BrushSize * canvasSize;
            ImU32 brushColor = GetStampColor(m_FreeformSettings.CurrentBrush);
            brushColor = (brushColor & 0x00FFFFFF) | 0x80000000; // Semi-transparent
            drawList->AddCircleFilled(mousePos, brushRadius, brushColor);
            drawList->AddCircle(mousePos, brushRadius, IM_COL32(255, 255, 255, 200), 0, 1.0f);
        }
    }
    
    // Create invisible button to capture input
    ImGui::InvisibleButton("PolygonCanvas", canvasSizeVec);
    bool canvasHovered = ImGui::IsItemHovered();
    bool canvasClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool rightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
    
    // Handle input
    if (canvasHovered)
    {
        glm::vec2 uvPos = screenToUV(mousePos);
        uvPos = glm::clamp(uvPos, glm::vec2(0.01f), glm::vec2(0.99f));
        
        if (m_IsDrawingPolygon)
        {
            if (canvasClicked)
            {
                // Check if clicking near first point to close
                if (poly.Points.size() >= 3)
                {
                    glm::vec2 first = poly.Points[0];
                    float dist = glm::length(uvPos - first);
                    if (dist < 0.03f)
                    {
                        m_FreeformSettings.Coastline.IsClosed = true;
                        m_IsDrawingPolygon = false;
                    }
                    else
                    {
                        m_FreeformSettings.Coastline.Points.push_back(uvPos);
                    }
                }
                else
                {
                    m_FreeformSettings.Coastline.Points.push_back(uvPos);
                }
            }
        }
        else if (m_IsStamping && poly.IsValid())
        {
            if (canvasClicked)
            {
                // Check if point is inside polygon
                if (IsPointInPolygon(uvPos, poly))
                {
                    AddStampAtPosition(uvPos);
                }
            }
        }
        
        // Right click to delete stamp
        if (rightClicked && !m_IsDrawingPolygon)
        {
            // Find and delete nearest stamp
            float bestDist = 0.05f;
            int bestIdx = -1;
            for (size_t i = 0; i < m_FreeformSettings.Stamps.size(); ++i)
            {
                float dist = glm::length(uvPos - m_FreeformSettings.Stamps[i].Position);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestIdx = static_cast<int>(i);
                }
            }
            if (bestIdx >= 0)
            {
                m_FreeformSettings.Stamps.erase(m_FreeformSettings.Stamps.begin() + bestIdx);
            }
        }
    }
    
    // Vertex dragging
    if (m_SelectedVertex >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        glm::vec2 uvPos = screenToUV(mousePos);
        uvPos = glm::clamp(uvPos, glm::vec2(0.01f), glm::vec2(0.99f));
        m_FreeformSettings.Coastline.Points[m_SelectedVertex] = uvPos;
    }
    
    if (m_HoveredVertex >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !m_IsDrawingPolygon)
    {
        m_SelectedVertex = m_HoveredVertex;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        m_SelectedVertex = -1;
    }
}

void WorldGenerationPanel::DrawFeatureStampControls()
{
    ImGui::Spacing();
    ImGui::SeparatorText("Feature Stamps");
    
    // Toggle stamping mode - capture state before button changes it
    bool wasStamping = m_IsStamping;
    if (wasStamping)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
    }
    if (ImGui::Button(m_IsStamping ? "Stop Stamping" : "Start Stamping", ImVec2(120, 0)))
    {
        m_IsStamping = !m_IsStamping;
        m_IsDrawingPolygon = false;
    }
    if (wasStamping)
    {
        ImGui::PopStyleColor();
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Clear Stamps"))
    {
        m_FreeformSettings.Stamps.clear();
    }
    
    // Brush type selector
    ImGui::Text("Brush Type:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    int brushIdx = static_cast<int>(m_FreeformSettings.CurrentBrush);
    if (ImGui::Combo("##BrushType", &brushIdx, kStampNames, static_cast<int>(FeatureStamp::Count)))
    {
        m_FreeformSettings.CurrentBrush = static_cast<FeatureStamp>(brushIdx);
    }
    
    ImGui::SliderFloat("Brush Size", &m_FreeformSettings.BrushSize, 0.03f, 0.2f, "%.3f");
    ImGui::SliderFloat("Brush Intensity", &m_FreeformSettings.BrushIntensity, 0.2f, 1.0f, "%.2f");
    
    // Quick brush buttons
    ImGui::Text("Quick:");
    ImGui::SameLine();
    const FeatureStamp quickStamps[] = { FeatureStamp::Mountain, FeatureStamp::Hill, 
                                         FeatureStamp::Valley, FeatureStamp::River,
                                         FeatureStamp::Lake };
    for (int i = 0; i < 5; ++i)
    {
        if (i > 0) ImGui::SameLine();
        FeatureStamp stamp = quickStamps[i];
        bool selected = (m_FreeformSettings.CurrentBrush == stamp);
        if (selected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(kStampNames[static_cast<int>(stamp)]))
        {
            m_FreeformSettings.CurrentBrush = stamp;
            m_IsStamping = true;
        }
        if (selected)
        {
            ImGui::PopStyleColor();
        }
    }
    
    // Stamp count
    ImGui::TextDisabled("%d stamps placed", static_cast<int>(m_FreeformSettings.Stamps.size()));
}

void WorldGenerationPanel::HandlePolygonDrawing(const glm::vec2& canvasMin, const glm::vec2& canvasSize)
{
    // This is handled inline in DrawPolygonCanvas for now
}

void WorldGenerationPanel::AddStampAtPosition(const glm::vec2& uvPos)
{
    TerrainFeatureStamp stamp;
    stamp.Type = m_FreeformSettings.CurrentBrush;
    stamp.Position = uvPos;
    stamp.Radius = m_FreeformSettings.BrushSize;
    stamp.Intensity = m_FreeformSettings.BrushIntensity;
    m_FreeformSettings.Stamps.push_back(stamp);
}

bool WorldGenerationPanel::IsPointInPolygon(const glm::vec2& point, const CoastlinePolygon& poly) const
{
    if (!poly.IsValid()) return false;
    
    // Ray casting algorithm
    bool inside = false;
    size_t n = poly.Points.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++)
    {
        const glm::vec2& pi = poly.Points[i];
        const glm::vec2& pj = poly.Points[j];
        
        if (((pi.y > point.y) != (pj.y > point.y)) &&
            (point.x < (pj.x - pi.x) * (point.y - pi.y) / (pj.y - pi.y) + pi.x))
        {
            inside = !inside;
        }
    }
    return inside;
}

float WorldGenerationPanel::DistanceToPolygonEdge(const glm::vec2& point, const CoastlinePolygon& poly) const
{
    if (poly.Points.size() < 2) return 1.0f;
    
    float minDist = std::numeric_limits<float>::max();
    size_t n = poly.Points.size();
    
    for (size_t i = 0; i < n; ++i)
    {
        size_t j = (i + 1) % n;
        if (!poly.IsClosed && j == 0) continue;
        
        const glm::vec2& a = poly.Points[i];
        const glm::vec2& b = poly.Points[j];
        
        // Distance to line segment
        glm::vec2 ab = b - a;
        glm::vec2 ap = point - a;
        float t = glm::clamp(glm::dot(ap, ab) / glm::dot(ab, ab), 0.0f, 1.0f);
        glm::vec2 closest = a + t * ab;
        float dist = glm::length(point - closest);
        minDist = glm::min(minDist, dist);
    }
    
    return minDist;
}

void WorldGenerationPanel::DrawBaseSettings()
{
    ImGui::SeparatorText("1. Base Structure");
    ImGui::SliderInt("Grid Resolution", &m_Settings.GridResolution, 128, 2048);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Higher resolutions take longer to generate.");
    }
    static const char* kSizeLabels[] = {
        "Small (2 km^2)",
        "Medium (6 km^2)",
        "Large (12 km^2)",
        "Huge (20 km^2)",
        "Custom"
    };
    int presetIndex = static_cast<int>(m_Settings.SizePreset);
    if (ImGui::Combo("Terrain Size", &presetIndex, kSizeLabels, IM_ARRAYSIZE(kSizeLabels)))
    {
        m_Settings.SizePreset = static_cast<TerrainSizePreset>(presetIndex);
        if (m_Settings.SizePreset != TerrainSizePreset::Custom)
        {
            m_Settings.WorldSize = WorldSizeFromPreset(m_Settings.SizePreset);
        }
    }
    if (ImGui::SliderFloat2("World Size (m)", glm::value_ptr(m_Settings.WorldSize), 64.0f, 6000.0f, "%.0f"))
    {
        m_Settings.SizePreset = TerrainSizePreset::Custom;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("~%.1f km^2", TerrainAreaKm2(m_Settings.WorldSize));
    ImGui::SliderFloat("Max Height (m)", &m_Settings.MaxHeight, 10.0f, 600.0f, "%.0f");
    ImGui::SliderFloat("Sea Level", &m_Settings.SeaLevel, 0.2f, 0.8f);
    ImGui::SliderFloat("Ocean Floor Height", &m_Settings.OceanFloor, 0.0f, 0.2f);
    ImGui::SliderFloat("Base Elevation Min", &m_Settings.BaseElevationMin, 0.0f, 0.6f);
    ImGui::SliderFloat("Base Elevation Max", &m_Settings.BaseElevationMax, 0.2f, 1.0f);
    ImGui::SliderFloat("Continent Frequency", &m_Settings.BaseContinentFrequency, 0.1f, 2.0f);
    ImGui::SliderFloat("Continent Warp", &m_Settings.BaseWarpStrength, 0.0f, 1.0f);
    ImGui::SliderInt("Macro Plate Count", &m_Settings.MacroPlateCount, 1, 8);
    ImGui::SliderFloat("Macro Mask Blend", &m_Settings.MacroMaskBlend, 0.0f, 1.0f);
    ImGui::SliderFloat("Macro Cluster Strength", &m_Settings.MacroClusterStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Edge Fade Strength", &m_Settings.EdgeFadeStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Sea Shelf Depth", &m_Settings.SeaShelfDepth, 0.01f, 0.25f);
    ImGui::SliderFloat("Interior Highlands Boost", &m_Settings.InteriorHighlandsBoost, 0.0f, 0.4f);
}

void WorldGenerationPanel::DrawFeatureSettings()
{
    ImGui::SeparatorText("2. Major Features");
    ImGui::SliderFloat("Mountain Frequency", &m_Settings.MountainFrequency, 0.5f, 6.0f);
    ImGui::SliderFloat("Mountain Intensity", &m_Settings.MountainIntensity, 0.0f, 1.5f);
    ImGui::SliderFloat("Mountain Sharpness", &m_Settings.MountainSharpness, 0.5f, 3.0f);
    ImGui::SliderFloat("Mountain Warp", &m_Settings.MountainWarpStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Plateau Frequency", &m_Settings.PlateauFrequency, 0.5f, 4.0f);
    ImGui::SliderFloat("Plateau Threshold", &m_Settings.PlateauThreshold, 0.3f, 0.85f);
    ImGui::SliderFloat("Terrace Step", &m_Settings.TerraceStep, 0.02f, 0.2f);
    ImGui::SliderFloat("Terrace Strength", &m_Settings.PlateauTerraceStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Post-Terrace Blend", &m_Settings.PlateauPostTerraceStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Plateau Erosion Damp", &m_Settings.PlateauErosionDamping, 0.1f, 1.0f);
    ImGui::SliderFloat("Canyon Frequency", &m_Settings.CanyonFrequency, 0.5f, 6.0f, "%.2f");
    ImGui::SliderFloat("Canyon Depth", &m_Settings.CanyonDepth, 0.0f, 0.2f, "%.3f");
    ImGui::SliderFloat("Canyon Warp", &m_Settings.CanyonWarpStrength, 0.0f, 1.0f);
    ImGui::SliderFloat("Canyon Sharpness", &m_Settings.CanyonSharpness, 0.5f, 3.0f, "%.2f");
    ImGui::SliderInt("Fault Iterations", &m_Settings.FaultIterations, 0, 128);
    ImGui::SliderFloat("Fault Displacement", &m_Settings.FaultDisplacement, 0.0f, 0.1f);
    ImGui::SliderFloat("Fault Falloff", &m_Settings.FaultFalloff, 0.1f, 1.0f);
}

void WorldGenerationPanel::DrawRiverSettings()
{
    ImGui::SeparatorText("3. Rivers & Valleys");
    ImGui::SliderInt("Erosion Droplets", &m_Settings.ErosionDroplets, 2000, 60000);
    ImGui::SliderInt("Droplet Steps", &m_Settings.ErosionMaxSteps, 10, 120);
    ImGui::SliderFloat("Erosion Capacity", &m_Settings.ErosionCapacity, 0.5f, 8.0f);
    ImGui::SliderFloat("Erosion Min Slope", &m_Settings.ErosionMinSlope, 0.0001f, 0.01f, "%.4f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Deposit Speed", &m_Settings.ErosionDepositSpeed, 0.1f, 0.6f);
    ImGui::SliderFloat("Erode Speed", &m_Settings.ErosionErodeSpeed, 0.1f, 0.6f);
    ImGui::SliderFloat("Evaporation", &m_Settings.ErosionEvaporateSpeed, 0.0f, 0.2f);
    ImGui::SliderFloat("Gravity", &m_Settings.ErosionGravity, 1.0f, 10.0f);
    ImGui::SliderFloat("Highland Erosion Mult", &m_Settings.HighlandErosionMultiplier, 0.1f, 1.0f);
    ImGui::SliderFloat("River Density", &m_Settings.RiverDensity, 0.05f, 0.5f);
    ImGui::SliderFloat("River Depth", &m_Settings.RiverDepth, 0.0f, 0.2f);
    ImGui::SliderFloat("River Bank Width (m)", &m_Settings.RiverBankWidth, 5.0f, 120.0f);
    ImGui::SliderFloat("River Bank Smoothing", &m_Settings.RiverBankSmoothing, 0.0f, 1.0f);
    ImGui::SliderFloat("River Influence Radius", &m_Settings.RiverDistanceFalloff, 20.0f, 400.0f);
}

void WorldGenerationPanel::DrawBiomeSettings()
{
    ImGui::SeparatorText("4. Biome & Material Assignment");
    ImGui::SliderFloat("Cliff Slope Start", &m_Settings.CliffSlopeStart, 5.0f, 45.0f);
    ImGui::SliderFloat("Cliff Slope Max", &m_Settings.CliffSlopeMax, 20.0f, 80.0f);
    ImGui::SliderFloat("Sand Height Max", &m_Settings.SandHeightMax, 0.05f, 0.5f);
    ImGui::SliderFloat("Sand Slope Max", &m_Settings.SandSlopeMax, 5.0f, 25.0f);
    ImGui::SliderFloat("Valley Grass Boost", &m_Settings.ValleyGrassBoost, 0.0f, 1.0f);
    ImGui::SliderFloat("Snow Start Height", &m_Settings.SnowHeightStart, 0.5f, 0.9f);
    ImGui::SliderFloat("Snow Full Height", &m_Settings.SnowHeightFull, 0.6f, 1.0f);
}

void WorldGenerationPanel::DrawChunkSettings()
{
    ImGui::SeparatorText("5. Gameplay Knobs & Chunks");
    ImGui::SliderInt("Chunk Resolution", &m_Settings.ChunkResolution, 32, 512);
    ImGui::Text("Seeds");
    ImGui::InputScalar("Base Seed", ImGuiDataType_U32, &m_Settings.BaseSeed);
    ImGui::InputScalar("Mountain Seed", ImGuiDataType_U32, &m_Settings.MountainSeed);
    ImGui::InputScalar("Plateau Seed", ImGuiDataType_U32, &m_Settings.PlateauSeed);
    ImGui::InputScalar("Fault Seed", ImGuiDataType_U32, &m_Settings.FaultSeed);
    ImGui::InputScalar("Erosion Seed", ImGuiDataType_U32, &m_Settings.ErosionSeed);
    ImGui::InputScalar("River Seed", ImGuiDataType_U32, &m_Settings.RiverSeed);
}

void WorldGenerationPanel::DrawChunkTable()
{
    ImGui::SeparatorText("Chunk Breakdown");
    if (m_LastChunks.empty())
    {
        ImGui::TextDisabled("Generate a world to see per-chunk stats.");
        return;
    }

    if (ImGui::BeginTable("WorldGenChunks", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Chunk");
        ImGui::TableSetupColumn("Min Height");
        ImGui::TableSetupColumn("Max Height");
        ImGui::TableSetupColumn("Avg Slope");
        ImGui::TableSetupColumn("Land %");
        ImGui::TableSetupColumn("River %");
        ImGui::TableHeadersRow();
        for (const TerrainChunkSummary& summary : m_LastChunks)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d,%d", summary.chunkX, summary.chunkY);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.1f", summary.minHeight);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", summary.maxHeight);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.1f", summary.avgSlope);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.0f%%", summary.landCoverage * 100.0f);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.0f%%", summary.riverCoverage * 100.0f);
        }
        ImGui::EndTable();
    }
}

void WorldGenerationPanel::RandomizeSeeds()
{
    std::random_device rd;
    std::mt19937 rng(rd());
    std::uniform_int_distribution<uint32_t> dist(1u, std::numeric_limits<uint32_t>::max());
    m_Settings.BaseSeed = dist(rng);
    m_Settings.MountainSeed = dist(rng);
    m_Settings.PlateauSeed = dist(rng);
    m_Settings.FaultSeed = dist(rng);
    m_Settings.ErosionSeed = dist(rng);
    m_Settings.RiverSeed = dist(rng);
}

void WorldGenerationPanel::RunGeneration()
{
    if (!m_Context)
    {
        m_StatusLine = "No active scene available.";
        return;
    }
    EntityData* data = GetTargetEntityData();
    if (!data || !data->Terrain)
    {
        m_StatusLine = "Target entity is missing a Terrain component.";
        return;
    }

    // Dispatch based on mode
    if (m_Mode == GeneratorMode::Advanced)
    {
        RunLegacyGeneration();
    }
    else if (m_Mode == GeneratorMode::Freeform)
    {
        RunFreeformGeneration();
    }
    else
    {
        RunRegionBasedGeneration();
    }
}

void WorldGenerationPanel::RunRegionBasedGeneration()
{
    EntityData* data = GetTargetEntityData();
    TerrainComponent& terrain = *data->Terrain;
    
    Settings settings = m_Settings;
    if (settings.SizePreset != TerrainSizePreset::Custom)
    {
        settings.WorldSize = WorldSizeFromPreset(settings.SizePreset);
    }
    
    const RegionSettings& region = m_RegionSettings;
    
    settings.GridResolution = std::clamp(settings.GridResolution, 64, 2048);
    settings.ChunkResolution = std::clamp(settings.ChunkResolution, 16, settings.GridResolution);
    settings.WorldSize.x = glm::max(settings.WorldSize.x, 64.0f);
    settings.WorldSize.y = glm::max(settings.WorldSize.y, 64.0f);
    settings.MaxHeight = glm::max(settings.MaxHeight, 1.0f);
    
    terrain.Resize(settings.GridResolution);
    terrain.WorldSize = settings.WorldSize;
    terrain.MaxHeight = settings.MaxHeight;
    terrain.ChunkResolution = settings.ChunkResolution;
    terrain.EnsureMapSize();
    
    const int res = settings.GridResolution;
    const size_t count = static_cast<size_t>(res) * static_cast<size_t>(res);
    HeightField field(res);
    std::vector<float> landMask(count, 1.0f);
    std::vector<float> riverMask;
    std::vector<float> riverDistance;
    std::vector<float> flow;
    std::vector<float> slopeDeg;
    
    // Pre-generate terrain feature layers (actual geometry, not just properties)
    std::vector<float> mountainLayer(count, 0.0f);
    std::vector<float> valleyLayer(count, 0.0f);
    std::vector<float> plateauLayer(count, 0.0f);
    std::vector<float> canyonLayer(count, 0.0f);
    std::vector<float> cliffLayer(count, 0.0f);
    std::vector<float> mesaLayer(count, 0.0f);
    
    auto start = std::chrono::steady_clock::now();
    
    float invRes = 1.0f / glm::max(res - 1, 1);
    float featureScale = glm::mix(0.5f, 1.5f, region.FeatureIntensity);
    float localVar = region.LocalVariation;
    
    // ============================================
    // PASS 1: Generate distinct terrain feature layers
    // Each layer is actual terrain geometry that will be composited
    // ============================================
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            
            // MOUNTAINS: Tall ridged peaks
            {
                glm::vec2 warp = DomainWarp(uv, settings.MountainSeed, 0.25f, 1.2f);
                float ridge = RidgedFBM((uv + warp) * 1.8f, 5, 2.1f, 0.52f, settings.MountainSeed);
                ridge = std::pow(ridge, 1.4f); // Sharpen peaks
                // Add some base noise for foothills
                float foothills = FBM(uv * 3.0f, 3, 2.0f, 0.5f, settings.MountainSeed + 100u);
                mountainLayer[idx] = glm::mix(foothills * 0.3f, ridge, ridge) * featureScale;
            }
            
            // VALLEY: Low, gently rolling depression
            {
                float base = FBM(uv * 2.0f, 3, 2.0f, 0.45f, settings.BaseSeed + 200u);
                // Valleys are inverted and compressed - low areas
                float valleyShape = 1.0f - std::pow(base, 0.7f);
                valleyLayer[idx] = valleyShape * 0.25f + 0.1f; // Low elevation
            }
            
            // PLATEAU: Flat elevated areas with stepped edges  
            {
                float plateauBase = FBM(uv * 1.5f, 2, 2.0f, 0.4f, settings.PlateauSeed);
                // Create flat-topped shape
                float flatTop = glm::smoothstep(0.3f, 0.5f, plateauBase);
                flatTop = Terrace(flatTop * 0.6f + 0.35f, 0.08f);
                plateauLayer[idx] = flatTop;
            }
            
            // CANYON: Deep carved channels
            {
                glm::vec2 warp = DomainWarp(uv, settings.RiverSeed, 0.3f, 1.5f);
                float carve = RidgedFBM((uv + warp) * 2.5f, 3, 2.2f, 0.5f, settings.RiverSeed + 500u);
                // Invert ridges to create canyons
                float canyonDepth = std::pow(carve, 2.0f);
                // Create canyon walls - high at edges, low in center
                float walls = 0.5f - canyonDepth * 0.45f * featureScale;
                canyonLayer[idx] = glm::clamp(walls, 0.05f, 0.6f);
            }
            
            // CLIFFS: Sharp vertical drops
            {
                glm::vec2 warp = DomainWarp(uv, settings.FaultSeed, 0.2f, 2.0f);
                float fault = FBM((uv + warp) * 3.0f, 4, 2.0f, 0.55f, settings.FaultSeed);
                // Create sharp step transitions
                float stepped = glm::smoothstep(0.35f, 0.65f, fault);
                stepped = stepped * stepped; // Sharpen
                float cliffHeight = stepped * 0.5f + 0.3f;
                // Add jagged detail
                float jagged = RidgedFBM(uv * 6.0f, 2, 2.0f, 0.5f, settings.FaultSeed + 77u);
                cliffLayer[idx] = cliffHeight + jagged * 0.1f * featureScale;
            }
            
            // MESA: Flat tops with steep sides
            {
                float mesaBase = FBM(uv * 1.2f, 2, 2.0f, 0.4f, settings.PlateauSeed + 888u);
                // Create distinct flat-topped formations
                float mesaShape = glm::smoothstep(0.4f, 0.55f, mesaBase);
                mesaShape = std::pow(mesaShape, 0.5f); // Flatten tops more
                mesaLayer[idx] = mesaShape * 0.45f + 0.3f;
            }
        }
    }
    
    // ============================================
    // PASS 2: Composite layers based on region weights
    // Actually blend the generated terrain, not properties
    // ============================================
    
    // Map zone types to their feature layers
    auto getZoneHeight = [&](TerrainZone zone, size_t idx, const glm::vec2& uv) -> float
    {
        float localNoise = FBM(uv * 4.0f, 2, 2.0f, 0.5f, settings.BaseSeed) * localVar * 0.15f;
        
        switch (zone)
        {
        case TerrainZone::Mountains:
            return 0.45f + mountainLayer[idx] * 0.55f + localNoise;
        case TerrainZone::Highlands:
            return 0.4f + mountainLayer[idx] * 0.25f + plateauLayer[idx] * 0.2f + localNoise;
        case TerrainZone::Plains:
            return 0.28f + valleyLayer[idx] * 0.15f + localNoise * 0.5f;
        case TerrainZone::Valley:
            return 0.15f + valleyLayer[idx] * 0.2f + localNoise * 0.3f;
        case TerrainZone::Cliffs:
            return cliffLayer[idx] + localNoise * 0.5f;
        case TerrainZone::Beach:
            return 0.18f + localNoise * 0.2f;
        case TerrainZone::Mesa:
            return mesaLayer[idx] + localNoise * 0.3f;
        case TerrainZone::Canyon:
            return canyonLayer[idx] + localNoise * 0.2f;
        case TerrainZone::Wetlands:
            return 0.2f + valleyLayer[idx] * 0.1f + localNoise * 0.15f;
        case TerrainZone::Plateau:
            return plateauLayer[idx] + localNoise * 0.25f;
        default:
            return 0.35f + localNoise;
        }
    };
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            
            // Get region weights
            RegionWeights w = ComputeRegionWeights(uv, region.CenterRadius, region.RegionBlendSharpness);
            
            // Sample actual terrain height for each zone
            float hNorth = getZoneHeight(region.North, idx, uv);
            float hSouth = getZoneHeight(region.South, idx, uv);
            float hEast = getZoneHeight(region.East, idx, uv);
            float hWest = getZoneHeight(region.West, idx, uv);
            float hCenter = getZoneHeight(region.Center, idx, uv);
            
            // Weighted blend of actual terrain heights
            float height = hNorth * w.north + hSouth * w.south + 
                          hEast * w.east + hWest * w.west + hCenter * w.center;
            
            field.Values[idx] = glm::clamp(height, 0.0f, 1.0f);
        }
    }
    
    // ============================================
    // PASS 3: Apply terracing for plateau/mesa zones
    // ============================================
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            
            RegionWeights w = ComputeRegionWeights(uv, region.CenterRadius, region.RegionBlendSharpness);
            
            // Compute terrace strength from zones that have terracing
            auto getTerraceWeight = [](TerrainZone z) -> float {
                switch (z) {
                    case TerrainZone::Plateau: return 0.8f;
                    case TerrainZone::Mesa: return 0.9f;
                    case TerrainZone::Canyon: return 0.3f;
                    case TerrainZone::Cliffs: return 0.4f;
                    default: return 0.0f;
                }
            };
            
            float terraceStr = getTerraceWeight(region.North) * w.north +
                              getTerraceWeight(region.South) * w.south +
                              getTerraceWeight(region.East) * w.east +
                              getTerraceWeight(region.West) * w.west +
                              getTerraceWeight(region.Center) * w.center;
            
            if (terraceStr > 0.1f)
            {
                float terraced = Terrace(field.Values[idx], 0.06f);
                field.Values[idx] = glm::mix(field.Values[idx], terraced, terraceStr * 0.6f);
            }
        }
    }
    
    // ============================================
    // PASS 4: Apply ocean border
    // ============================================
    
    if (region.OceanBorder)
    {
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
                glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
                
                float land = ComputeLandMask(uv, region.OceanBorderWidth, region.CoastComplexity, settings.BaseSeed);
                landMask[idx] = land;
                
                // Create coastal shelf - gradual descent near shores
                float coastalHeight = field.Values[idx];
                if (land < 1.0f)
                {
                    float shelfDepth = glm::smoothstep(0.0f, 0.3f, land);
                    coastalHeight = glm::mix(settings.OceanFloor, coastalHeight, shelfDepth);
                }
                field.Values[idx] = coastalHeight;
            }
        }
    }
    
    // ============================================
    // PASS 5: Smoothing based on terrain smoothness setting
    // ============================================
    
    if (region.BaselineSmoothness > 0.2f)
    {
        HeightField smoothed = field;
        int blurRadius = static_cast<int>(region.BaselineSmoothness * 3.0f);
        blurRadius = std::clamp(blurRadius, 1, 4);
        
        for (int y = blurRadius; y < res - blurRadius; ++y)
        {
            for (int x = blurRadius; x < res - blurRadius; ++x)
            {
                float sum = 0.0f;
                int samples = 0;
                for (int dy = -blurRadius; dy <= blurRadius; ++dy)
                {
                    for (int dx = -blurRadius; dx <= blurRadius; ++dx)
                    {
                        sum += field.Get(x + dx, y + dy);
                        ++samples;
                    }
                }
                smoothed.Set(x, y, sum / static_cast<float>(samples));
            }
        }
        
        // Blend smoothed with original based on setting
        float smoothBlend = (region.BaselineSmoothness - 0.2f) / 0.8f * 0.5f;
        for (size_t i = 0; i < count; ++i)
        {
            field.Values[i] = glm::mix(field.Values[i], smoothed.Values[i], smoothBlend);
        }
    }
    
    // ============================================
    // PASS 6: Light erosion for natural look
    // ============================================
    
    int erosionDroplets = static_cast<int>(6000 * (1.0f - region.BaselineSmoothness * 0.4f));
    Settings erosionSettings = settings;
    erosionSettings.ErosionDroplets = erosionDroplets;
    erosionSettings.ErosionMaxSteps = 35;
    erosionSettings.ErosionCapacity = 2.5f;
    erosionSettings.ErosionErodeSpeed = 0.2f;
    erosionSettings.ErosionDepositSpeed = 0.25f;
    std::vector<float> emptyPlateau(count, 0.0f);
    RunHydraulicErosion(field, emptyPlateau, erosionSettings);
    
    // ============================================
    // PASS 7: Rivers
    // ============================================
    
    if (region.GenerateRivers)
    {
        settings.RiverDensity = region.RiverDensity;
        ComputeFlowAndRivers(field, landMask, settings, settings.WorldSize, flow, riverMask, riverDistance);
        
        for (size_t i = 0; i < count; ++i)
        {
            if (riverMask[i] > 0.0f)
            {
                field.Values[i] = glm::clamp(field.Values[i] - riverMask[i] * settings.RiverDepth, 0.0f, 1.0f);
            }
        }
        
        SmoothRiverBanks(field, riverDistance, settings, settings.WorldSize);
    }
    else
    {
        riverMask.assign(count, 0.0f);
        riverDistance.assign(count, settings.RiverDistanceFalloff);
    }
    
    // Compute slopes and assign biomes
    ComputeSlopeDegrees(field, settings.WorldSize, settings.MaxHeight, slopeDeg);
    AssignBiomes(field, slopeDeg, riverDistance, riverMask, landMask, settings, terrain);
    
    // Write final heightmap
    for (size_t i = 0; i < count; ++i)
    {
        terrain.HeightMap[i] = static_cast<uint16_t>(glm::clamp(field.Values[i], 0.0f, 1.0f) * 65535.0f);
    }
    
    BuildChunkSummaries(field, slopeDeg, landMask, riverMask, settings, settings.MaxHeight, m_LastChunks);
    
    terrain.MarkDataDirty();
    Terrain::EnsureChunkLayout(terrain);
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    Terrain::MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastGenerationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream oss;
    oss << "Generated " << res << "x" << res << " region-based terrain for '" << data->Name
        << "' (" << static_cast<int>(m_LastChunks.size()) << " chunks).";
    m_StatusLine = oss.str();
}

void WorldGenerationPanel::RunFreeformGeneration()
{
    EntityData* data = GetTargetEntityData();
    TerrainComponent& terrain = *data->Terrain;
    
    const FreeformSettings& freeform = m_FreeformSettings;
    
    if (!freeform.Coastline.IsValid())
    {
        m_StatusLine = "Please draw a closed coastline polygon first.";
        return;
    }
    
    Settings settings = m_Settings;
    if (settings.SizePreset != TerrainSizePreset::Custom)
    {
        settings.WorldSize = WorldSizeFromPreset(settings.SizePreset);
    }
    
    settings.GridResolution = std::clamp(settings.GridResolution, 64, 2048);
    settings.ChunkResolution = std::clamp(settings.ChunkResolution, 16, settings.GridResolution);
    settings.WorldSize.x = glm::max(settings.WorldSize.x, 64.0f);
    settings.WorldSize.y = glm::max(settings.WorldSize.y, 64.0f);
    settings.MaxHeight = glm::max(settings.MaxHeight, 1.0f);
    
    terrain.Resize(settings.GridResolution);
    terrain.WorldSize = settings.WorldSize;
    terrain.MaxHeight = settings.MaxHeight;
    terrain.ChunkResolution = settings.ChunkResolution;
    terrain.EnsureMapSize();
    
    const int res = settings.GridResolution;
    const size_t count = static_cast<size_t>(res) * static_cast<size_t>(res);
    HeightField field(res);
    std::vector<float> landMask(count, 0.0f);
    std::vector<float> edgeDistance(count, 1.0f);
    std::vector<float> riverMask;
    std::vector<float> riverDistance;
    std::vector<float> flow;
    std::vector<float> slopeDeg;
    
    auto start = std::chrono::steady_clock::now();
    
    float invRes = 1.0f / glm::max(res - 1, 1);
    
    // ============================================
    // PASS 1: Generate land mask from polygon
    // ============================================
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            
            bool inside = IsPointInPolygon(uv, freeform.Coastline);
            float dist = DistanceToPolygonEdge(uv, freeform.Coastline);
            
            landMask[idx] = inside ? 1.0f : 0.0f;
            edgeDistance[idx] = inside ? dist : -dist;
        }
    }
    
    // ============================================
    // PASS 2: Generate base terrain with shore gradient
    // ============================================
    
    float shoreWidth = glm::max(0.01f, freeform.ShoreWidth);
    
    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            
            float dist = edgeDistance[idx];
            float land = landMask[idx];
            
            if (land < 0.5f)
            {
                // Ocean - flat floor
                field.Values[idx] = settings.OceanFloor;
                continue;
            }
            
            // Shore noise for irregular coastline detail
            float shoreNoise = 0.0f;
            if (freeform.ShoreNoiseStrength > kFloatEpsilon)
            {
                glm::vec2 warp = DomainWarp(uv, settings.BaseSeed, 0.1f, 3.0f);
                shoreNoise = FBM((uv + warp) * 8.0f, 3, 2.0f, 0.5f, settings.BaseSeed + 555u);
                shoreNoise = (shoreNoise - 0.5f) * freeform.ShoreNoiseStrength * shoreWidth * 0.5f;
            }
            
            // Distance from shore with noise
            float effectiveDist = dist + shoreNoise;
            
            // Shore gradient (beach to inland)
            float shoreT = glm::clamp(effectiveDist / shoreWidth, 0.0f, 1.0f);
            
            // Determine if this shore section is cliff or beach
            float cliffNoise = FBM(uv * 4.0f, 2, 2.0f, 0.5f, settings.BaseSeed + 777u);
            bool isCliff = cliffNoise < freeform.CliffChance;
            
            float baseHeight;
            if (isCliff && shoreT < 0.3f)
            {
                // Cliff - sharp rise from ocean
                float cliffT = shoreT / 0.3f;
                baseHeight = glm::mix(settings.OceanFloor + 0.05f, freeform.BaseElevation, 
                                     std::pow(cliffT, 0.3f));
            }
            else
            {
                // Beach/shore - gradual gradient
                baseHeight = glm::mix(settings.OceanFloor + 0.1f, freeform.BaseElevation,
                                     glm::smoothstep(0.0f, 1.0f, shoreT));
            }
            
            // Interior terrain roughness
            if (shoreT > 0.5f)
            {
                float interiorNoise = FBM(uv * 3.0f, 4, 2.0f, 0.5f, settings.BaseSeed);
                interiorNoise = (interiorNoise - 0.5f) * 2.0f;
                baseHeight += interiorNoise * freeform.TerrainRoughness * 0.15f * (shoreT - 0.5f) * 2.0f;
            }
            
            field.Values[idx] = glm::clamp(baseHeight, 0.0f, 1.0f);
        }
    }
    
    // ============================================
    // PASS 3: Apply feature stamps
    // ============================================
    
    float featureIntensity = freeform.FeatureIntensity;
    
    for (const TerrainFeatureStamp& stamp : freeform.Stamps)
    {
        float stampRadius = stamp.Radius;
        float stampIntensity = stamp.Intensity * featureIntensity;
        
        // Determine affected area
        int minX = static_cast<int>((stamp.Position.x - stampRadius) * res);
        int maxX = static_cast<int>((stamp.Position.x + stampRadius) * res) + 1;
        int minY = static_cast<int>((stamp.Position.y - stampRadius) * res);
        int maxY = static_cast<int>((stamp.Position.y + stampRadius) * res) + 1;
        minX = std::clamp(minX, 0, res - 1);
        maxX = std::clamp(maxX, 0, res - 1);
        minY = std::clamp(minY, 0, res - 1);
        maxY = std::clamp(maxY, 0, res - 1);
        
        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
                if (landMask[idx] < 0.5f) continue; // Skip ocean
                
                glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
                float dist = glm::length(uv - stamp.Position);
                
                // Smooth falloff from stamp center
                float falloff = 1.0f - glm::smoothstep(0.0f, stampRadius, dist);
                if (falloff <= kFloatEpsilon) continue;
                
                float effect = falloff * stampIntensity;
                float currentHeight = field.Values[idx];
                
                // Generate feature-specific noise at this position
                float featureNoise = 0.0f;
                
                switch (stamp.Type)
                {
                case FeatureStamp::Mountain:
                {
                    // Ridged mountain peak
                    glm::vec2 warp = DomainWarp(uv, settings.MountainSeed, 0.15f, 2.0f);
                    float ridge = RidgedFBM((uv + warp) * 4.0f, 4, 2.1f, 0.5f, settings.MountainSeed);
                    ridge = std::pow(ridge, 1.3f);
                    featureNoise = 0.35f + ridge * 0.5f;
                    field.Values[idx] = glm::clamp(currentHeight + featureNoise * effect, 0.0f, 1.0f);
                    break;
                }
                case FeatureStamp::Hill:
                {
                    // Smooth rounded hill
                    float hill = FBM(uv * 5.0f, 3, 2.0f, 0.45f, settings.BaseSeed + 111u);
                    featureNoise = 0.15f + hill * 0.15f;
                    // Bell curve shape
                    featureNoise *= (1.0f - dist / stampRadius);
                    field.Values[idx] = glm::clamp(currentHeight + featureNoise * effect, 0.0f, 1.0f);
                    break;
                }
                case FeatureStamp::Valley:
                {
                    // Depression
                    float valleyDepth = 0.15f * effect;
                    field.Values[idx] = glm::clamp(currentHeight - valleyDepth, 0.0f, 1.0f);
                    break;
                }
                case FeatureStamp::River:
                {
                    // Carve a channel - will be enhanced by flow computation
                    float riverDepth = 0.08f * effect;
                    field.Values[idx] = glm::clamp(currentHeight - riverDepth, 0.0f, 1.0f);
                    break;
                }
                case FeatureStamp::Mesa:
                {
                    // Flat-topped elevation
                    float mesaHeight = freeform.BaseElevation + 0.2f;
                    float terraced = Terrace(mesaHeight, 0.05f);
                    field.Values[idx] = glm::mix(currentHeight, terraced, effect * 0.8f);
                    break;
                }
                case FeatureStamp::Plateau:
                {
                    // Elevated flat area
                    float plateauHeight = freeform.BaseElevation + 0.15f;
                    plateauHeight = Terrace(plateauHeight, 0.06f);
                    field.Values[idx] = glm::mix(currentHeight, plateauHeight, effect * 0.7f);
                    break;
                }
                case FeatureStamp::Canyon:
                {
                    // Deep carved channel
                    glm::vec2 warp = DomainWarp(uv, settings.RiverSeed, 0.2f, 3.0f);
                    float carve = RidgedFBM((uv + warp) * 6.0f, 3, 2.0f, 0.5f, settings.RiverSeed);
                    float canyonDepth = (0.25f + carve * 0.15f) * effect;
                    field.Values[idx] = glm::clamp(currentHeight - canyonDepth, 0.0f, 1.0f);
                    break;
                }
                case FeatureStamp::Lake:
                {
                    // Flat water level depression
                    float lakeLevel = settings.OceanFloor + 0.12f;
                    field.Values[idx] = glm::mix(currentHeight, lakeLevel, effect);
                    break;
                }
                case FeatureStamp::Forest:
                    // Forest doesn't affect height, just biome (handled later)
                    break;
                default:
                    break;
                }
            }
        }
    }
    
    // ============================================
    // PASS 4: Auto-generate hills if enabled
    // ============================================
    
    if (freeform.AutoGenerateHills && freeform.AutoHillCount > 0)
    {
        std::mt19937 rng(settings.BaseSeed + 12345u);
        std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
        
        for (int h = 0; h < freeform.AutoHillCount; ++h)
        {
            // Find a random point inside the polygon
            glm::vec2 pos;
            int attempts = 0;
            do {
                pos = glm::vec2(dist01(rng), dist01(rng));
                ++attempts;
            } while (!IsPointInPolygon(pos, freeform.Coastline) && attempts < 100);
            
            if (attempts >= 100) continue;
            
            // Check distance from shore
            float edgeDist = DistanceToPolygonEdge(pos, freeform.Coastline);
            if (edgeDist < 0.08f) continue; // Too close to shore
            
            float hillRadius = 0.04f + dist01(rng) * 0.06f;
            float hillHeight = 0.1f + dist01(rng) * 0.15f;
            
            // Apply hill
            int minX = static_cast<int>((pos.x - hillRadius) * res);
            int maxX = static_cast<int>((pos.x + hillRadius) * res) + 1;
            int minY = static_cast<int>((pos.y - hillRadius) * res);
            int maxY = static_cast<int>((pos.y + hillRadius) * res) + 1;
            minX = std::clamp(minX, 0, res - 1);
            maxX = std::clamp(maxX, 0, res - 1);
            minY = std::clamp(minY, 0, res - 1);
            maxY = std::clamp(maxY, 0, res - 1);
            
            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
                    if (landMask[idx] < 0.5f) continue;
                    
                    glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
                    float d = glm::length(uv - pos);
                    float falloff = 1.0f - glm::smoothstep(0.0f, hillRadius, d);
                    falloff = falloff * falloff; // Smoother falloff
                    
                    field.Values[idx] = glm::clamp(field.Values[idx] + hillHeight * falloff, 0.0f, 1.0f);
                }
            }
        }
    }
    
    // ============================================
    // PASS 5: Light erosion for naturalness
    // ============================================
    
    int erosionDroplets = 5000;
    Settings erosionSettings = settings;
    erosionSettings.ErosionDroplets = erosionDroplets;
    erosionSettings.ErosionMaxSteps = 30;
    erosionSettings.ErosionCapacity = 2.0f;
    erosionSettings.ErosionErodeSpeed = 0.2f;
    erosionSettings.ErosionDepositSpeed = 0.25f;
    std::vector<float> emptyPlateau(count, 0.0f);
    RunHydraulicErosion(field, emptyPlateau, erosionSettings);
    
    // ============================================
    // PASS 6: Rivers
    // ============================================
    
    if (freeform.AutoGenerateRivers && freeform.AutoRiverCount > 0)
    {
        settings.RiverDensity = 0.15f + freeform.AutoRiverCount * 0.05f;
        ComputeFlowAndRivers(field, landMask, settings, settings.WorldSize, flow, riverMask, riverDistance);
        
        for (size_t i = 0; i < count; ++i)
        {
            if (riverMask[i] > 0.0f && landMask[i] > 0.5f)
            {
                field.Values[i] = glm::clamp(field.Values[i] - riverMask[i] * settings.RiverDepth, 0.0f, 1.0f);
            }
        }
        
        SmoothRiverBanks(field, riverDistance, settings, settings.WorldSize);
    }
    else
    {
        riverMask.assign(count, 0.0f);
        riverDistance.assign(count, settings.RiverDistanceFalloff);
    }
    
    // Clamp and finalize
    field.ClampAll();
    
    // Compute slopes and assign biomes
    ComputeSlopeDegrees(field, settings.WorldSize, settings.MaxHeight, slopeDeg);
    AssignBiomes(field, slopeDeg, riverDistance, riverMask, landMask, settings, terrain);
    
    // Write final heightmap
    for (size_t i = 0; i < count; ++i)
    {
        terrain.HeightMap[i] = static_cast<uint16_t>(glm::clamp(field.Values[i], 0.0f, 1.0f) * 65535.0f);
    }
    
    BuildChunkSummaries(field, slopeDeg, landMask, riverMask, settings, settings.MaxHeight, m_LastChunks);
    
    terrain.MarkDataDirty();
    Terrain::EnsureChunkLayout(terrain);
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    Terrain::MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();
    
    auto end = std::chrono::steady_clock::now();
    m_LastGenerationMs = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream oss;
    oss << "Generated freeform terrain for '" << data->Name
        << "' with " << freeform.Stamps.size() << " stamps.";
    m_StatusLine = oss.str();
}

void WorldGenerationPanel::RunLegacyGeneration()
{
    EntityData* data = GetTargetEntityData();
    TerrainComponent& terrain = *data->Terrain;

    Settings settings = m_Settings;
    if (settings.SizePreset != TerrainSizePreset::Custom)
    {
        settings.WorldSize = WorldSizeFromPreset(settings.SizePreset);
    }
    settings.GridResolution = std::clamp(settings.GridResolution, 64, 2048);
    settings.ChunkResolution = std::clamp(settings.ChunkResolution, 16, settings.GridResolution);
    settings.WorldSize.x = glm::max(settings.WorldSize.x, 64.0f);
    settings.WorldSize.y = glm::max(settings.WorldSize.y, 64.0f);
    settings.MaxHeight = glm::max(settings.MaxHeight, 1.0f);
    settings.BaseElevationMax = glm::max(settings.BaseElevationMax, settings.BaseElevationMin + 0.05f);
    settings.MacroPlateCount = std::clamp(settings.MacroPlateCount, 1, 8);
    settings.MacroMaskBlend = glm::clamp(settings.MacroMaskBlend, 0.0f, 1.0f);
    settings.MacroClusterStrength = glm::clamp(settings.MacroClusterStrength, 0.0f, 1.0f);
    settings.EdgeFadeStrength = glm::clamp(settings.EdgeFadeStrength, 0.0f, 1.0f);
    settings.SeaShelfDepth = glm::clamp(settings.SeaShelfDepth, 0.005f, 0.5f);
    settings.InteriorHighlandsBoost = glm::clamp(settings.InteriorHighlandsBoost, 0.0f, 0.5f);
    settings.CanyonFrequency = glm::max(settings.CanyonFrequency, 0.1f);
    settings.CanyonDepth = glm::clamp(settings.CanyonDepth, 0.0f, 0.4f);
    settings.CanyonWarpStrength = glm::clamp(settings.CanyonWarpStrength, 0.0f, 1.0f);
    settings.CanyonSharpness = glm::max(settings.CanyonSharpness, 0.25f);

    terrain.Resize(settings.GridResolution);
    terrain.WorldSize = settings.WorldSize;
    terrain.MaxHeight = settings.MaxHeight;
    terrain.ChunkResolution = settings.ChunkResolution;
    terrain.EnsureMapSize();

    const int res = settings.GridResolution;
    const size_t count = static_cast<size_t>(res) * static_cast<size_t>(res);
    HeightField field(res);
    std::vector<float> landMask(count, 0.0f);
    std::vector<float> plateauMask(count, 0.0f);
    std::vector<float> plateauInterior(count, 0.0f);
    std::vector<float> riverMask;
    std::vector<float> riverDistance;
    std::vector<float> flow;
    std::vector<float> slopeDeg;
    std::vector<glm::vec2> radialCoords(count);

    auto start = std::chrono::steady_clock::now();

    const auto macroPlates = BuildMacroPlates(settings);
    float invRes = 1.0f / glm::max(res - 1, 1);
    float shelfWidth = glm::max(settings.SeaShelfDepth, 0.005f);
    auto plateauEdgeFrom = [](float interior)
    {
        return glm::clamp(4.0f * interior * (1.0f - interior), 0.0f, 1.0f);
    };

    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            float macroMask = 0.0f;
            float macroPlateau = 0.0f;
            for (const MacroPlate& plate : macroPlates)
            {
                glm::vec2 offset = uv - plate.Center;
                offset = plate.Rotation * offset;
                offset.x /= glm::max(plate.Axis.x, 0.01f);
                offset.y /= glm::max(plate.Axis.y, 0.01f);
                float dist = glm::length(offset);
                float influence = glm::clamp(1.0f - dist, 0.0f, 1.0f);
                influence = std::pow(influence, plate.InfluencePower);
                macroMask = glm::max(macroMask, influence);
                float plateauCore = 1.0f - glm::smoothstep(plate.PlateauRadius, 1.0f, dist);
                macroPlateau = glm::max(macroPlateau, glm::clamp(plateauCore, 0.0f, 1.0f));
            }

            glm::vec2 warp = DomainWarp(uv, settings.BaseSeed, settings.BaseWarpStrength, settings.BaseContinentFrequency * 0.5f);
            float baseNoise = FBM((uv + warp) * settings.BaseContinentFrequency,
                                  settings.BaseOctaves,
                                  settings.BaseLacunarity,
                                  settings.BaseGain,
                                  settings.BaseSeed);
            float mask = glm::mix(baseNoise, macroMask, settings.MacroMaskBlend);
            float radial = glm::length((uv - glm::vec2(0.5f)) * 2.0f);
            float rim = glm::smoothstep(kEdgeFadeStart, kEdgeFadeEnd, radial);
            float fade = glm::mix(1.0f, 1.0f - rim, settings.EdgeFadeStrength);
            mask = glm::clamp(mask * fade, 0.0f, 1.0f);
            macroPlateau *= fade;

            float land = glm::smoothstep(settings.SeaLevel - 0.03f, settings.SeaLevel + 0.03f, mask);
            float coast = glm::smoothstep(settings.SeaLevel - shelfWidth, settings.SeaLevel + shelfWidth, mask);
            float interior = glm::smoothstep(settings.SeaLevel + shelfWidth * 0.5f, 1.0f, mask);

            landMask[idx] = land;
            plateauInterior[idx] = macroPlateau * land;
            plateauMask[idx] = plateauInterior[idx];

            float coastalBlend = glm::mix(settings.OceanFloor, settings.BaseElevationMin, coast);
            float inland = glm::mix(settings.BaseElevationMin, settings.BaseElevationMax, interior);
            float elevation = glm::mix(coastalBlend, inland, land);
            elevation += interior * settings.InteriorHighlandsBoost;
            field.Values[idx] = glm::clamp(elevation, 0.0f, 1.0f);

            radialCoords[idx] = glm::vec2(
                (static_cast<float>(x) / glm::max(res - 1, 1) * 2.0f) - 1.0f,
                (static_cast<float>(y) / glm::max(res - 1, 1) * 2.0f) - 1.0f);
        }
    }

    for (int y = 0; y < res; ++y)
    {
        for (int x = 0; x < res; ++x)
        {
            size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
            glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
            glm::vec2 warp = DomainWarp(uv, settings.MountainSeed, settings.MountainWarpStrength, settings.MountainFrequency * 0.5f);
            float ridged = RidgedFBM((uv + warp) * settings.MountainFrequency,
                                     settings.MountainOctaves,
                                     settings.MountainLacunarity,
                                     settings.MountainGain,
                                     settings.MountainSeed);
            ridged = std::pow(ridged, settings.MountainSharpness);
            float edgeMask = plateauEdgeFrom(plateauInterior[idx]);
            float chainMask = glm::mix(0.35f, 1.0f, edgeMask);
            float contribution = ridged * settings.MountainIntensity * landMask[idx] * chainMask;
            field.Values[idx] = glm::clamp(field.Values[idx] + contribution, 0.0f, 1.0f);

            float plateauNoise = FBM(uv * settings.PlateauFrequency,
                                     3,
                                     2.0f,
                                     0.5f,
                                     settings.PlateauSeed);
            float noisePlateau = glm::smoothstep(settings.PlateauThreshold, 1.0f, plateauNoise) * landMask[idx];
            float plateau = glm::clamp(glm::max(noisePlateau, plateauInterior[idx]), 0.0f, 1.0f);
            plateauMask[idx] = plateau;
            if (plateau > 0.0f)
            {
                float terraced = Terrace(field.Values[idx], settings.TerraceStep);
                field.Values[idx] = glm::mix(field.Values[idx], terraced, plateau * settings.PlateauTerraceStrength);
            }
        }
    }

    if (settings.CanyonDepth > kFloatEpsilon)
    {
        uint32_t canyonSeed = settings.RiverSeed ^ 0xa511e9b3u;
        float canyonFrequency = glm::max(0.1f, settings.CanyonFrequency);
        for (int y = 0; y < res; ++y)
        {
            for (int x = 0; x < res; ++x)
            {
                size_t idx = static_cast<size_t>(y) * res + static_cast<size_t>(x);
                glm::vec2 uv = glm::vec2(x * invRes, y * invRes);
                float suitability = landMask[idx] * (1.0f - plateauMask[idx]);
                float edgeBoost = plateauEdgeFrom(plateauInterior[idx]);
                suitability *= glm::mix(0.35f, 1.0f, edgeBoost);
                if (suitability <= kFloatEpsilon)
                    continue;
                glm::vec2 warp = DomainWarp(uv, canyonSeed, settings.CanyonWarpStrength, canyonFrequency * 0.5f);
                float canyon = RidgedFBM((uv + warp) * canyonFrequency,
                                         3,
                                         2.15f,
                                         0.5f,
                                         canyonSeed);
                canyon = std::pow(glm::clamp(canyon, 0.0f, 1.0f), settings.CanyonSharpness);
                float lowland = 1.0f - glm::smoothstep(0.55f, 0.9f, field.Values[idx]);
                float depth = settings.CanyonDepth * suitability * lowland;
                if (depth <= kFloatEpsilon)
                    continue;
                field.Values[idx] = glm::clamp(field.Values[idx] - canyon * depth, 0.0f, 1.0f);
            }
        }
    }

    if (settings.FaultIterations > 0 && settings.FaultDisplacement > 0.0f)
    {
        std::mt19937 rng(settings.FaultSeed);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        for (int iter = 0; iter < settings.FaultIterations; ++iter)
        {
            glm::vec2 normal(dist(rng), dist(rng));
            if (glm::length2(normal) < 1e-4f)
                continue;
            normal = glm::normalize(normal);
            glm::vec2 point(dist(rng), dist(rng));
            float displacement = settings.FaultDisplacement * (1.0f - static_cast<float>(iter) / settings.FaultIterations);
            for (size_t idx = 0; idx < count; ++idx)
            {
                if (landMask[idx] <= 0.05f)
                    continue;
                float d = glm::dot(radialCoords[idx] - point, normal);
                float delta = displacement * std::tanh(d * settings.FaultFalloff);
                field.Values[idx] = glm::clamp(field.Values[idx] + delta, 0.0f, 1.0f);
            }
        }
    }

    RunHydraulicErosion(field, plateauMask, settings);

    field.ClampAll();
    float minH = field.Min();
    float maxH = field.Max();
    if (maxH - minH > kFloatEpsilon)
    {
        for (float& v : field.Values)
        {
            v = glm::clamp((v - minH) / (maxH - minH), 0.0f, 1.0f);
        }
    }

    for (size_t i = 0; i < count; ++i)
    {
        field.Values[i] = glm::mix(settings.OceanFloor, field.Values[i], landMask[i]);
    }

    ComputeFlowAndRivers(field, landMask, settings, settings.WorldSize, flow, riverMask, riverDistance);

    for (size_t i = 0; i < count; ++i)
    {
        if (riverMask[i] > 0.0f)
        {
            field.Values[i] = glm::clamp(field.Values[i] - riverMask[i] * settings.RiverDepth, 0.0f, 1.0f);
        }
    }

    SmoothRiverBanks(field, riverDistance, settings, settings.WorldSize);

    for (size_t i = 0; i < count; ++i)
    {
        if (plateauMask[i] > 0.0f)
        {
            float terraced = Terrace(field.Values[i], settings.TerraceStep * 0.5f);
            field.Values[i] = glm::mix(field.Values[i], terraced, plateauMask[i] * settings.PlateauPostTerraceStrength);
        }
    }

    ComputeSlopeDegrees(field, settings.WorldSize, settings.MaxHeight, slopeDeg);

    AssignBiomes(field, slopeDeg, riverDistance, riverMask, landMask, settings, terrain);

    for (size_t i = 0; i < count; ++i)
    {
        terrain.HeightMap[i] = static_cast<uint16_t>(glm::clamp(field.Values[i], 0.0f, 1.0f) * 65535.0f);
    }

    BuildChunkSummaries(field, slopeDeg, landMask, riverMask, settings, settings.MaxHeight, m_LastChunks);

    terrain.MarkDataDirty();
    Terrain::EnsureChunkLayout(terrain);
    Terrain::MarkHeightRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    Terrain::MarkSplatRegionDirty(terrain, glm::ivec2(0), glm::ivec2(res - 1));
    terrain.PhysicsDirty = true;
    m_Context->MarkDirty();

    auto end = std::chrono::steady_clock::now();
    m_LastGenerationMs = std::chrono::duration<double, std::milli>(end - start).count();

    std::ostringstream oss;
    oss << "Generated " << res << "x" << res << " terrain for '" << data->Name
        << "' (" << static_cast<int>(m_LastChunks.size()) << " chunks).";
    m_StatusLine = oss.str();
}


