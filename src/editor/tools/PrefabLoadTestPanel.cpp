#include "editor/tools/PrefabLoadTestPanel.h"

#include "editor/Project.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/ui/Logger.h"
#include "editor/ui/UILayer.h"
#include "editor/ui/utility/ModelSnapshotUtils.h"
#include "core/audio/Audio.h"
#include "core/assets/AssetReference.h"
#include "core/ecs/Components.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/ParticleEmitterSystem.h"
#include "core/ecs/RenderOverridesComponent.h"
#include "core/ecs/Scene.h"
#include "core/ecs/SkinningSystem.h"
#include "core/navigation/NavAgent.h"
#include "core/navigation/Navigation.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "core/physics/Physics.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/StandardMeshManager.h"
#include "core/utils/Profiler.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/norm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr uint16_t kLoadTestViewIdBase = 216;
constexpr float kSimulatedDeltaTime = 1.0f / 60.0f;
// Keep the synthetic benchmark scene centered near the world origin.
// Some nav bake paths still derive large positive-only bounds from zero,
// which can accidentally trigger chunked large-world behavior for a tiny floor.
constexpr glm::vec3 kBenchmarkOrigin(0.0f, 0.0f, 0.0f);

struct FrustumPlane {
    glm::vec4 Equation{0.0f};
};

struct ViewFrustum {
    FrustumPlane Planes[6];
};

struct FrameCapture {
    double FrameMs = 0.0;
    double SceneUpdateMs = 0.0;
    double AudioMs = 0.0;
    double RenderMs = 0.0;
    uint64_t WorkingSetBytes = 0;
    uint64_t PrivateBytes = 0;
    Renderer::RuntimeStatsFrame RuntimeStats{};
    std::vector<Profiler::Entry> Entries;
    std::unordered_map<std::string, uint64_t> Counters;
    std::string ErrorMessage;
};

struct SectionAccumulator {
    std::vector<double> TimesMs;
    std::vector<double> CallCounts;
};

struct CounterAccumulator {
    std::vector<double> Values;
};

struct AggregateBuilder {
    std::vector<double> FrameMs;
    std::vector<double> SceneUpdateMs;
    std::vector<double> AudioMs;
    std::vector<double> RenderMs;
    std::vector<double> WorkingSetMb;
    std::vector<double> PrivateMb;
    std::vector<double> RenderedMeshObjects;
    std::vector<double> CulledMeshObjects;
    std::vector<double> RenderedSkinnedMeshObjects;
    std::vector<double> CulledSkinnedMeshObjects;
    std::map<std::string, SectionAccumulator> Sections;
    std::map<std::string, CounterAccumulator> Counters;

    void Add(const FrameCapture& sample)
    {
        FrameMs.push_back(sample.FrameMs);
        SceneUpdateMs.push_back(sample.SceneUpdateMs);
        AudioMs.push_back(sample.AudioMs);
        RenderMs.push_back(sample.RenderMs);
        WorkingSetMb.push_back(static_cast<double>(sample.WorkingSetBytes) / (1024.0 * 1024.0));
        PrivateMb.push_back(static_cast<double>(sample.PrivateBytes) / (1024.0 * 1024.0));
        RenderedMeshObjects.push_back(static_cast<double>(sample.RuntimeStats.RenderedMeshObjects));
        CulledMeshObjects.push_back(static_cast<double>(sample.RuntimeStats.CulledMeshObjects));
        RenderedSkinnedMeshObjects.push_back(static_cast<double>(sample.RuntimeStats.RenderedSkinnedMeshObjects));
        CulledSkinnedMeshObjects.push_back(static_cast<double>(sample.RuntimeStats.CulledSkinnedMeshObjects));

        auto addSection = [&](const std::string& name, double totalMs, double callCount) {
            SectionAccumulator& entry = Sections[name];
            entry.TimesMs.push_back(totalMs);
            entry.CallCounts.push_back(callCount);
        };

        for (const Profiler::Entry& entry : sample.Entries) {
            addSection(entry.name, entry.totalMs, static_cast<double>(entry.callCount));
        }

        addSection("LoadTest/Frame", sample.FrameMs, 1.0);
        addSection("LoadTest/SceneUpdate", sample.SceneUpdateMs, 1.0);
        addSection("LoadTest/Audio", sample.AudioMs, 1.0);
        addSection("LoadTest/Render", sample.RenderMs, sample.RenderMs > 0.0 ? 1.0 : 0.0);

        for (const auto& counter : sample.Counters) {
            Counters[counter.first].Values.push_back(static_cast<double>(counter.second));
        }
    }
};

struct BenchmarkSceneState {
    Scene SceneInstance;
    EntityID CameraEntity = INVALID_ENTITY_ID;
    EntityID FloorEntity = INVALID_ENTITY_ID;
    EntityID NavMeshEntity = INVALID_ENTITY_ID;
    EntityID PlayerProxyEntity = INVALID_ENTITY_ID;
    std::vector<EntityID> PrefabRoots;
    std::vector<EntityID> NavAgents;
    std::vector<glm::vec3> CrowdPositions;
    glm::vec3 SingleBoundsMin{0.0f};
    glm::vec3 SingleBoundsMax{0.0f};
    float RootGroundOffsetY = 0.0f;
    bool HasParticleEmitters = false;
    bool HasVisualContent = false;
    bool NeedsNavigationFloor = false;
};

struct NavBindingReport {
    size_t TotalAgents = 0;
    size_t BoundAgents = 0;
    size_t OnMeshAgents = 0;
};

struct CrowdDestinationReport {
    size_t TotalAgents = 0;
    size_t ProjectedDestinations = 0;
    size_t IssuedDestinations = 0;
};

const char* ScenarioModeLabel(PrefabLoadTestPanel::ScenarioMode scenario)
{
    switch (scenario) {
    case PrefabLoadTestPanel::ScenarioMode::GeneralPrefab:
        return "General Prefab";
    case PrefabLoadTestPanel::ScenarioMode::OnscreenCrowd:
        return "Onscreen Crowd";
    case PrefabLoadTestPanel::ScenarioMode::OnscreenCrowdFullLod:
        return "Onscreen Crowd (Full LOD)";
    default:
        return "Unknown";
    }
}

std::string ScenarioModeToken(PrefabLoadTestPanel::ScenarioMode scenario)
{
    switch (scenario) {
    case PrefabLoadTestPanel::ScenarioMode::GeneralPrefab:
        return "general_prefab";
    case PrefabLoadTestPanel::ScenarioMode::OnscreenCrowd:
        return "onscreen_crowd";
    case PrefabLoadTestPanel::ScenarioMode::OnscreenCrowdFullLod:
        return "onscreen_crowd_full_lod";
    default:
        return "unknown";
    }
}

bool ScenarioRequiresVisibility(PrefabLoadTestPanel::ScenarioMode scenario)
{
    return scenario != PrefabLoadTestPanel::ScenarioMode::GeneralPrefab;
}

bool ScenarioRequiresFullLod(PrefabLoadTestPanel::ScenarioMode scenario)
{
    return scenario == PrefabLoadTestPanel::ScenarioMode::OnscreenCrowdFullLod;
}

float ResolveCrowdSpacingMultiplier(PrefabLoadTestPanel::ScenarioMode scenario)
{
    switch (scenario) {
    case PrefabLoadTestPanel::ScenarioMode::OnscreenCrowd:
        return 1.15f;
    case PrefabLoadTestPanel::ScenarioMode::OnscreenCrowdFullLod:
        return 1.05f;
    case PrefabLoadTestPanel::ScenarioMode::GeneralPrefab:
    default:
        return 1.75f;
    }
}

std::string NormalizePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string LeafName(const std::string& path)
{
    try {
        return fs::path(path).filename().string();
    } catch (...) {
        return path;
    }
}

std::string StemName(const std::string& path)
{
    try {
        return fs::path(path).stem().string();
    } catch (...) {
        return path;
    }
}

std::string SanitizeFileToken(const std::string& value)
{
    std::string out = value.empty() ? "prefab_load_test" : value;
    for (char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if ((uc >= 'a' && uc <= 'z') ||
            (uc >= 'A' && uc <= 'Z') ||
            (uc >= '0' && uc <= '9') ||
            c == '_' || c == '-' || c == '.') {
            continue;
        }
        c = '_';
    }
    return out;
}

std::string FormatNowLocal()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

std::string FormatTimestampToken()
{
    const std::time_t now = std::time(nullptr);
    std::tm localTime{};
#if defined(_WIN32)
    localtime_s(&localTime, &now);
#else
    localtime_r(&now, &localTime);
#endif
    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return stream.str();
}

double PercentDelta(double previous, double current)
{
    const double scale = std::max(std::abs(previous), 1e-6);
    return std::abs(current - previous) / scale * 100.0;
}

double CounterValueOrZero(const FrameCapture& sample, const char* name)
{
    auto it = sample.Counters.find(name);
    return it != sample.Counters.end() ? static_cast<double>(it->second) : 0.0;
}

ViewFrustum BuildViewFrustum(const glm::mat4& viewMatrix, const glm::mat4& projectionMatrix)
{
    const glm::mat4 clip = projectionMatrix * viewMatrix;
    ViewFrustum frustum{};
    glm::vec4 planes[6] = {
        clip[3] + clip[0],
        clip[3] - clip[0],
        clip[3] + clip[1],
        clip[3] - clip[1],
        clip[3] + clip[2],
        clip[3] - clip[2]
    };

    for (int index = 0; index < 6; ++index) {
        const glm::vec3 normal(planes[index]);
        const float length = glm::length(normal);
        if (length > 1e-5f) {
            planes[index] /= length;
        }
        frustum.Planes[index].Equation = planes[index];
    }

    return frustum;
}

bool AabbIntersectsFrustum(const ViewFrustum& frustum, const glm::vec3& boundsMin, const glm::vec3& boundsMax)
{
    for (const FrustumPlane& plane : frustum.Planes) {
        const glm::vec3 positiveVertex(
            plane.Equation.x >= 0.0f ? boundsMax.x : boundsMin.x,
            plane.Equation.y >= 0.0f ? boundsMax.y : boundsMin.y,
            plane.Equation.z >= 0.0f ? boundsMax.z : boundsMin.z);
        if (glm::dot(glm::vec3(plane.Equation), positiveVertex) + plane.Equation.w < 0.0f) {
            return false;
        }
    }
    return true;
}

double PercentileFromSorted(const std::vector<double>& values, double q)
{
    if (values.empty()) {
        return 0.0;
    }
    if (q <= 0.0) {
        return values.front();
    }
    if (q >= 1.0) {
        return values.back();
    }

    const double pos = q * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = static_cast<size_t>(std::ceil(pos));
    if (lo == hi) {
        return values[lo];
    }

    const double t = pos - static_cast<double>(lo);
    return values[lo] * (1.0 - t) + values[hi] * t;
}

PrefabLoadTestPanel::NumericStats ComputeStats(std::vector<double> values)
{
    PrefabLoadTestPanel::NumericStats stats{};
    if (values.empty()) {
        return stats;
    }

    stats.Samples = values.size();
    stats.Average = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    stats.Min = *std::min_element(values.begin(), values.end());
    stats.Max = *std::max_element(values.begin(), values.end());
    std::sort(values.begin(), values.end());
    stats.P50 = PercentileFromSorted(values, 0.50);
    stats.P95 = PercentileFromSorted(values, 0.95);
    stats.P99 = PercentileFromSorted(values, 0.99);
    return stats;
}

std::vector<int> BuildStageCounts(int maxInstances, int instanceStep)
{
    const int clampedMax = std::max(1, maxInstances);
    const int clampedStep = std::max(1, instanceStep);

    std::vector<int> counts;
    counts.push_back(1);
    for (int count = std::max(2, clampedStep); count < clampedMax; count += clampedStep) {
        if (counts.back() != count) {
            counts.push_back(count);
        }
    }
    if (counts.back() != clampedMax) {
        counts.push_back(clampedMax);
    }
    return counts;
}

template <typename Fn>
void TraverseHierarchy(Scene& scene, EntityID rootId, Fn&& fn)
{
    std::vector<EntityID> stack;
    stack.push_back(rootId);
    while (!stack.empty()) {
        const EntityID current = stack.back();
        stack.pop_back();

        EntityData* data = scene.GetEntityData(current);
        if (!data) {
            continue;
        }

        fn(current, *data);
        for (auto it = data->Children.rbegin(); it != data->Children.rend(); ++it) {
            stack.push_back(*it);
        }
    }
}

PrefabLoadTestPanel::PrefabComposition AnalyzePrefabComposition(Scene& scene, EntityID rootId)
{
    PrefabLoadTestPanel::PrefabComposition composition{};
    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        ++composition.EntityCount;
        if (data.Mesh) {
            ++composition.MeshCount;
        }
        if (data.Skinning) {
            ++composition.SkinnedMeshCount;
        }
        if (data.Camera) {
            ++composition.CameraCount;
        }
        if (data.Light) {
            ++composition.LightCount;
        }
        if (data.Emitter && data.Emitter->Enabled) {
            ++composition.ParticleEmitterCount;
        }
        if (data.Navigation) {
            ++composition.NavigationMeshCount;
        }
        if (data.NavAgent) {
            ++composition.NavAgentCount;
        }
        if (data.NavLink) {
            ++composition.NavLinkCount;
        }
        if (data.CharacterController) {
            ++composition.CharacterControllerCount;
        }
        if (data.AudioSource) {
            ++composition.AudioSourceCount;
        }
        composition.ScriptCount += data.Scripts.size();
        if (data.RigidBody) {
            ++composition.RigidBodyCount;
        }
        if (data.StaticBody) {
            ++composition.StaticBodyCount;
        }
    });

    composition.HasVisualContent =
        composition.MeshCount > 0 ||
        composition.SkinnedMeshCount > 0 ||
        composition.ParticleEmitterCount > 0;
    composition.NeedsNavigationFloor =
        composition.NavigationMeshCount > 0 ||
        composition.NavAgentCount > 0 ||
        composition.NavLinkCount > 0 ||
        composition.CharacterControllerCount > 0;
    return composition;
}

bool HierarchyHasParticleEmitters(Scene& scene, EntityID rootId)
{
    bool hasEmitters = false;
    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        if (!hasEmitters && data.Emitter && data.Emitter->Enabled) {
            hasEmitters = true;
        }
    });
    return hasEmitters;
}

float ComputeParticleWarmupTime(Scene& scene, EntityID rootId)
{
    float maxWarmup = 0.0f;
    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        if (!data.Emitter || !data.Emitter->Enabled) {
            return;
        }

        const ParticleEmitterComponent& emitter = *data.Emitter;
        const float lifetime = (emitter.Lifetime.Min + emitter.Lifetime.Max) * 0.5f;
        const float targetTime = emitter.Looping
            ? std::min(emitter.Duration, lifetime)
            : std::min(emitter.Duration * 0.75f, lifetime);
        maxWarmup = std::max(maxWarmup, std::max(targetTime, 0.5f));
    });
    return maxWarmup;
}

void TriggerParticleEmitters(Scene& scene, EntityID rootId)
{
    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        if (data.Emitter && data.Emitter->Enabled) {
            data.Emitter->Play();
        }
    });
}

void ComputeParticleEmitterBounds(const ParticleEmitterComponent& emitter,
                                  const glm::mat4& worldMatrix,
                                  glm::vec3& outMin,
                                  glm::vec3& outMax)
{
    const glm::vec3 worldPos = glm::vec3(worldMatrix[3]);
    float shapeRadius = 0.0f;
    glm::vec3 shapeExtent(0.0f);

    switch (emitter.Shape) {
    case ParticleEmissionShape::Sphere:
    case ParticleEmissionShape::Hemisphere:
    case ParticleEmissionShape::Circle:
    case ParticleEmissionShape::Disc:
    case ParticleEmissionShape::Cone:
        shapeRadius = emitter.ShapeRadius;
        break;
    case ParticleEmissionShape::Box:
    case ParticleEmissionShape::Rectangle:
        shapeExtent = emitter.ShapeScale * 0.5f;
        break;
    case ParticleEmissionShape::Edge:
        shapeExtent.x = emitter.ShapeLength * 0.5f;
        break;
    default:
        break;
    }

    const float maxLifetime = emitter.Lifetime.Max;
    const float maxSpeed = emitter.StartSpeed.Max;
    const float travelDistance = maxSpeed * maxLifetime;
    const float gravityDrop = 0.5f * std::abs(emitter.GravityModifier) * 9.81f * maxLifetime * maxLifetime;

    glm::vec3 velocityExtent(0.0f);
    if (emitter.VelocityOverLifetimeEnabled) {
        velocityExtent = glm::abs(emitter.LinearVelocity) * maxLifetime;
    }

    glm::vec3 totalExtent = shapeExtent + velocityExtent + glm::vec3(shapeRadius + travelDistance);
    totalExtent.y += gravityDrop;
    totalExtent *= 1.15f;

    outMin = worldPos - totalExtent;
    outMax = worldPos + totalExtent;
}

bool ComputeHierarchyWorldBounds(Scene& scene, EntityID rootId, glm::vec3& outMin, glm::vec3& outMax)
{
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool foundAny = false;

    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        if (!data.Visible || !data.Active) {
            return;
        }

        if (data.Mesh && data.Mesh->mesh) {
            const glm::vec3 localMin = data.Mesh->mesh->BoundsMin;
            const glm::vec3 localMax = data.Mesh->mesh->BoundsMax;
            if (localMax.x > localMin.x && localMax.y > localMin.y && localMax.z > localMin.z) {
                const glm::vec3 corners[8] = {
                    {localMin.x, localMin.y, localMin.z},
                    {localMax.x, localMin.y, localMin.z},
                    {localMin.x, localMax.y, localMin.z},
                    {localMax.x, localMax.y, localMin.z},
                    {localMin.x, localMin.y, localMax.z},
                    {localMax.x, localMin.y, localMax.z},
                    {localMin.x, localMax.y, localMax.z},
                    {localMax.x, localMax.y, localMax.z}
                };

                for (const glm::vec3& corner : corners) {
                    const glm::vec3 worldCorner = glm::vec3(data.Transform.WorldMatrix * glm::vec4(corner, 1.0f));
                    outMin = glm::min(outMin, worldCorner);
                    outMax = glm::max(outMax, worldCorner);
                    foundAny = true;
                }
            }
        }

        if (data.Emitter && data.Emitter->Enabled) {
            glm::vec3 emitterMin(0.0f);
            glm::vec3 emitterMax(0.0f);
            ComputeParticleEmitterBounds(*data.Emitter, data.Transform.WorldMatrix, emitterMin, emitterMax);
            outMin = glm::min(outMin, emitterMin);
            outMax = glm::max(outMax, emitterMax);
            foundAny = true;
        }
    });

    if (!foundAny) {
        outMin = glm::vec3(-0.5f);
        outMax = glm::vec3(0.5f);
    }
    return foundAny;
}

glm::vec3 TransformLocalPointToWorld(const TransformComponent& transform, const glm::vec3& localPoint)
{
    return glm::vec3(transform.WorldMatrix * glm::vec4(localPoint, 1.0f));
}

bool TryComputeEntityGroundSupportPoint(const EntityData& data, glm::vec3& outSupportPoint)
{
    if (data.CharacterController) {
        outSupportPoint = TransformLocalPointToWorld(data.Transform, data.CharacterController->Offset);
        return true;
    }

    if (data.Collider) {
        glm::vec3 localSupport = data.Collider->Offset;
        switch (data.Collider->ShapeType) {
        case ColliderShape::Box:
            localSupport.y -= data.Collider->Size.y * 0.5f;
            break;
        case ColliderShape::Capsule:
            localSupport.y -= (data.Collider->Height * 0.5f) + data.Collider->Radius;
            break;
        case ColliderShape::Sphere:
            localSupport.y -= data.Collider->Radius;
            break;
        default:
            return false;
        }

        outSupportPoint = TransformLocalPointToWorld(data.Transform, localSupport);
        return true;
    }

    if (data.NavAgent) {
        outSupportPoint = glm::vec3(data.Transform.WorldMatrix[3]);
        return true;
    }

    return false;
}

bool TryComputeHierarchyGroundSupportY(Scene& scene, EntityID rootId, float& outMinSupportY)
{
    outMinSupportY = std::numeric_limits<float>::max();
    bool foundAny = false;

    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        if (!data.Active) {
            return;
        }

        glm::vec3 supportPoint(0.0f);
        if (!TryComputeEntityGroundSupportPoint(data, supportPoint)) {
            return;
        }

        outMinSupportY = std::min(outMinSupportY, supportPoint.y);
        foundAny = true;
    });

    if (!foundAny) {
        outMinSupportY = 0.0f;
    }
    return foundAny;
}

float ResolveRootGroundOffsetY(Scene& scene, EntityID rootId, float fallbackMinY)
{
    float minSupportY = 0.0f;
    if (TryComputeHierarchyGroundSupportY(scene, rootId, minSupportY)) {
        return -minSupportY;
    }

    return -fallbackMinY;
}

float ComputeNavProbeSnapDistance(const EntityData& data)
{
    float snapDistance = 4.0f;
    if (data.NavAgent) {
        snapDistance = std::max(
            snapDistance,
            data.NavAgent->Params.height + data.NavAgent->Params.maxStep + 1.0f);
    }
    if (data.Collider) {
        snapDistance = std::max(
            snapDistance,
            std::abs(data.Collider->Offset.y) +
                data.Collider->Height +
                data.Collider->Radius +
                1.0f);
    }
    if (data.CharacterController) {
        snapDistance = std::max(
            snapDistance,
            std::abs(data.CharacterController->Offset.y) +
                data.CharacterController->Height +
                data.CharacterController->Radius +
                1.0f);
    }

    glm::vec3 supportPoint(0.0f);
    if (TryComputeEntityGroundSupportPoint(data, supportPoint)) {
        const float rootY = glm::vec3(data.Transform.WorldMatrix[3]).y;
        snapDistance = std::max(snapDistance, std::abs(rootY - supportPoint.y) + 1.0f);
    }

    return snapDistance;
}

void BuildNavProbePositions(const EntityData& data, std::vector<glm::vec3>& outProbePositions)
{
    outProbePositions.clear();
    outProbePositions.reserve(8);

    const glm::vec3 rootPosition = glm::vec3(data.Transform.WorldMatrix[3]);
    outProbePositions.push_back(rootPosition);

    glm::vec3 supportPoint(0.0f);
    if (TryComputeEntityGroundSupportPoint(data, supportPoint)) {
        outProbePositions.push_back(supportPoint);
        outProbePositions.push_back(supportPoint + glm::vec3(0.0f, 0.1f, 0.0f));
    }

    if (data.Collider) {
        const glm::vec3 colliderCenter = TransformLocalPointToWorld(data.Transform, data.Collider->Offset);
        outProbePositions.push_back(colliderCenter);
        if (data.Collider->Height > 0.0f) {
            outProbePositions.push_back(
                colliderCenter + glm::vec3(0.0f, data.Collider->Height * 0.5f, 0.0f));
            outProbePositions.push_back(
                colliderCenter - glm::vec3(0.0f, data.Collider->Height * 0.5f, 0.0f));
        }
    }

    if (data.NavAgent) {
        outProbePositions.push_back(
            rootPosition + glm::vec3(0.0f, data.NavAgent->Params.height * 0.5f, 0.0f));
        outProbePositions.push_back(
            rootPosition + glm::vec3(0.0f, data.NavAgent->Params.height, 0.0f));
    }
}

bool TryProjectAgentOntoNavMesh(Scene& scene,
                                EntityID navMeshEntity,
                                nav::NavMeshComponent* navMesh,
                                EntityData& data,
                                glm::vec3& outNearest)
{
    if (!data.NavAgent) {
        return false;
    }

    const float snapDistance = ComputeNavProbeSnapDistance(data);
    std::vector<glm::vec3> probePositions;
    BuildNavProbePositions(data, probePositions);

    constexpr int kBindAttempts = 20;
    for (int attempt = 0; attempt < kBindAttempts; ++attempt) {
        if (navMesh) {
            navMesh->EnsureRuntimeLoaded(scene);
        }

        for (const glm::vec3& probe : probePositions) {
            if (nav::Navigation::Get().NearestPoint(scene, navMeshEntity, probe, snapDistance, outNearest)) {
                return true;
            }
        }

        if (!navMesh || !navMesh->ChunkedNavEnabled) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return false;
}

bool ComputeCombinedBounds(Scene& scene,
                           const std::vector<EntityID>& roots,
                           EntityID floorEntity,
                           glm::vec3& outMin,
                           glm::vec3& outMax)
{
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool foundAny = false;

    for (EntityID root : roots) {
        glm::vec3 rootMin(0.0f);
        glm::vec3 rootMax(0.0f);
        if (ComputeHierarchyWorldBounds(scene, root, rootMin, rootMax)) {
            outMin = glm::min(outMin, rootMin);
            outMax = glm::max(outMax, rootMax);
            foundAny = true;
        }
    }

    if (floorEntity != INVALID_ENTITY_ID) {
        glm::vec3 floorMin(0.0f);
        glm::vec3 floorMax(0.0f);
        if (ComputeHierarchyWorldBounds(scene, floorEntity, floorMin, floorMax)) {
            outMin = glm::min(outMin, floorMin);
            outMax = glm::max(outMax, floorMax);
            foundAny = true;
        }
    }

    if (!foundAny) {
        outMin = glm::vec3(-1.0f);
        outMax = glm::vec3(1.0f);
    }
    return foundAny;
}

std::vector<glm::vec3> BuildCrowdPositions(PrefabLoadTestPanel::ScenarioMode scenario,
                                           int maxInstances,
                                           const glm::vec3& singleBoundsMin,
                                           const glm::vec3& singleBoundsMax,
                                           float rootGroundOffsetY)
{
    const int count = std::max(1, maxInstances);
    const float width = std::max(1.0f, singleBoundsMax.x - singleBoundsMin.x);
    const float depth = std::max(1.0f, singleBoundsMax.z - singleBoundsMin.z);
    const float spacingMultiplier = ResolveCrowdSpacingMultiplier(scenario);
    const float minSpacing = ScenarioRequiresVisibility(scenario) ? 1.25f : 2.0f;
    const float spacingX = std::max(minSpacing, width * spacingMultiplier);
    const float spacingZ = std::max(minSpacing, depth * spacingMultiplier);

    const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count)))));
    const int rows = std::max(1, static_cast<int>(std::ceil(static_cast<float>(count) / static_cast<float>(columns))));

    std::vector<glm::vec3> positions;
    positions.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        const int row = index / columns;
        const int column = index % columns;
        const float x = (static_cast<float>(column) - (static_cast<float>(columns) - 1.0f) * 0.5f) * spacingX;
        const float z = (static_cast<float>(row) - (static_cast<float>(rows) - 1.0f) * 0.5f) * spacingZ;
        positions.emplace_back(kBenchmarkOrigin.x + x, rootGroundOffsetY, kBenchmarkOrigin.z + z);
    }
    return positions;
}

EntityID CreatePlayerProxy(Scene& scene, const glm::vec3& position)
{
    Entity playerProxy = scene.CreateEntity("Player");
    EntityData* playerData = scene.GetEntityData(playerProxy.GetID());
    if (!playerData) {
        return INVALID_ENTITY_ID;
    }

    playerData->Transform.Position = position;
    playerData->Transform.TransformDirty = true;
    scene.MarkTransformDirty(playerProxy.GetID());
    return playerProxy.GetID();
}

void UpdatePlayerProxyPosition(Scene& scene, EntityID playerProxyEntity, const glm::vec3& position)
{
    if (playerProxyEntity == INVALID_ENTITY_ID) {
        return;
    }

    EntityData* playerData = scene.GetEntityData(playerProxyEntity);
    if (!playerData) {
        return;
    }

    playerData->Transform.Position = position;
    playerData->Transform.TransformDirty = true;
    scene.MarkTransformDirty(playerProxyEntity);
}

void ApplyFullLodOverrides(Scene& scene, EntityID rootId)
{
    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        data.ScriptLodEnabled = false;
        data.ScriptLodForceDisabled = true;
        if (data.AnimationPlayer) {
            data.AnimationPlayer->LODEnabled = false;
            data.AnimationPlayer->CrowdThrottleEnabled = false;
            data.AnimationPlayer->OffscreenDormancyEnabled = false;
            data.AnimationPlayer->LODMediumInterval = 0.0f;
            data.AnimationPlayer->LODFarInterval = 0.0f;
            data.AnimationPlayer->LODVeryFarInterval = 0.0f;
            data.AnimationPlayer->OffscreenNearInterval = 0.0f;
            data.AnimationPlayer->OffscreenMediumInterval = 0.0f;
            data.AnimationPlayer->OffscreenFarInterval = 0.0f;
            data.AnimationPlayer->OffscreenVeryFarInterval = 0.0f;
            data.AnimationPlayer->DormantOffscreenIdle = false;
        }
        if (data.Skeleton) {
            data.Skeleton->LodAccumulatedTime = 0.0f;
            data.Skeleton->LodLastDistance = 0.0f;
            data.Skeleton->LodMeshVisibleLastFrame = true;
        }
    });
}

void SyncSceneTransforms(Scene& scene)
{
    scene.ProcessPendingCreations();
    scene.ProcessPendingRemovals();
    scene.UpdateTransforms();
    scene.ProcessBoneAttachments();
    SkinningSystem::Update(scene);
    scene.ResolveScriptEntityReferencesFromMetadata();
}

EntityID CreateBenchmarkCamera(Scene& scene, int width, int height)
{
    Entity cameraEntity = scene.CreateEntity("Load Test Camera");
    EntityData* cameraData = scene.GetEntityData(cameraEntity.GetID());
    if (!cameraData) {
        return INVALID_ENTITY_ID;
    }

    cameraData->Camera = std::make_unique<CameraComponent>();
    cameraData->Camera->Active = true;
    cameraData->Camera->priority = -1000;
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    cameraData->Camera->UpdateProjection(aspect);
    return cameraEntity.GetID();
}

void DisablePrefabOwnedCameras(Scene& scene, EntityID rootId)
{
    TraverseHierarchy(scene, rootId, [&](EntityID, EntityData& data) {
        if (data.Camera) {
            data.Camera->Active = false;
        }
    });
}

void UpdateBenchmarkCamera(Scene& scene,
                           EntityID cameraEntity,
                           const glm::vec3& boundsMin,
                           const glm::vec3& boundsMax,
                           int width,
                           int height)
{
    EntityData* cameraData = scene.GetEntityData(cameraEntity);
    if (!cameraData || !cameraData->Camera) {
        return;
    }

    const glm::vec3 center = 0.5f * (boundsMin + boundsMax);
    glm::vec3 extents = 0.5f * (boundsMax - boundsMin);
    extents = glm::max(extents, glm::vec3(0.5f));

    const snapshot_utils::SnapshotViewFit viewFit = snapshot_utils::ComputeSnapshotViewFit(extents);
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    const float fovDegrees = 60.0f;
    const float halfFov = glm::radians(fovDegrees * 0.5f);
    const float distHeight = viewFit.halfHeight / std::tan(halfFov);
    const float distWidth = viewFit.halfWidth / (std::tan(halfFov) * aspect);
    const float distance = std::max(distHeight, distWidth) * 1.2f;
    const float radius = std::max(extents.x, std::max(extents.y, extents.z));
    const float farClip = std::max(250.0f, distance + radius * 6.0f);

    cameraData->Camera->Camera.SetViewportSize(static_cast<float>(width), static_cast<float>(height));
    cameraData->Camera->Camera.SetPerspective(fovDegrees, aspect, 0.1f, farClip);
    cameraData->Camera->Camera.SetPosition(center + viewFit.viewDir * distance);
    cameraData->Camera->Camera.LookAt(center, viewFit.upDir);
}

EntityID CreateNavigationFloor(Scene& scene,
                               const std::vector<glm::vec3>& crowdPositions,
                               const glm::vec3& singleBoundsMin,
                               const glm::vec3& singleBoundsMax)
{
    glm::vec3 crowdMin(std::numeric_limits<float>::max());
    glm::vec3 crowdMax(std::numeric_limits<float>::lowest());
    for (const glm::vec3& position : crowdPositions) {
        crowdMin = glm::min(crowdMin, position);
        crowdMax = glm::max(crowdMax, position);
    }

    const float prefabHalfWidth = std::max(0.5f, (singleBoundsMax.x - singleBoundsMin.x) * 0.5f);
    const float prefabHalfDepth = std::max(0.5f, (singleBoundsMax.z - singleBoundsMin.z) * 0.5f);
    const float halfX = std::max(8.0f, (crowdMax.x - crowdMin.x) * 0.5f + prefabHalfWidth + 6.0f);
    const float halfZ = std::max(8.0f, (crowdMax.z - crowdMin.z) * 0.5f + prefabHalfDepth + 6.0f);
    const glm::vec3 center = 0.5f * (crowdMin + crowdMax);

    Entity floorEntity = scene.CreateEntity("LoadTestFloor");
    EntityData* floorData = scene.GetEntityData(floorEntity.GetID());
    if (!floorData) {
        return INVALID_ENTITY_ID;
    }

    floorData->Transform.Position = glm::vec3(center.x, -0.5f, center.z);
    floorData->Transform.Scale = glm::vec3(halfX * 2.0f, 1.0f, halfZ * 2.0f);
    floorData->Transform.TransformDirty = true;
    floorData->Mesh = std::make_unique<MeshComponent>(
        StandardMeshManager::Instance().GetCubeMesh(),
        "LoadTestFloor",
        MaterialManager::Instance().CreateSceneDefaultMaterial(&scene));
    floorData->Mesh->meshReference = AssetReference::CreatePrimitive("Cube");
    if (!floorData->RenderOverrides) {
        floorData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
    }
    floorData->Collider = std::make_unique<ColliderComponent>();
    floorData->Collider->ShapeType = ColliderShape::Box;
    floorData->StaticBody = std::make_unique<StaticBodyComponent>();
    scene.MarkTransformDirty(floorEntity.GetID());
    return floorEntity.GetID();
}

EntityID CreateNavigationMesh(Scene& scene)
{
    Entity navEntity = scene.CreateEntity("LoadTestNavMesh");
    EntityData* navData = scene.GetEntityData(navEntity.GetID());
    if (!navData) {
        return INVALID_ENTITY_ID;
    }

    navData->Navigation = std::make_unique<nav::NavMeshComponent>();
    navData->Navigation->GeometryIncludeRegexEnabled = true;
    navData->Navigation->GeometryIncludeRegexPattern = "^LoadTestFloor$";
    navData->Navigation->ChunkedNavEnabled = false;
    return navEntity.GetID();
}

bool WaitForNavBake(Scene& scene, EntityID navMeshEntity, std::string& outError)
{
    EntityData* navData = scene.GetEntityData(navMeshEntity);
    if (!navData || !navData->Navigation) {
        outError = "Benchmark navigation mesh entity is missing its navmesh component.";
        return false;
    }

    nav::NavMeshComponent& navigation = *navData->Navigation;
    navigation.RequestBake(scene);

    const auto start = std::chrono::steady_clock::now();
    while (navigation.IsBaking()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(15)) {
            outError = "Timed out waiting for the benchmark navmesh bake to complete.";
            return false;
        }
    }

    if (!navigation.Runtime && !navigation.EnsureRuntimeLoaded(scene)) {
        outError = "Benchmark navmesh bake completed without a usable runtime navmesh.";
        return false;
    }

    return true;
}

void CollectNavAgents(Scene& scene, EntityID rootId, std::vector<EntityID>& outAgents)
{
    TraverseHierarchy(scene, rootId, [&](EntityID id, EntityData& data) {
        if (data.NavAgent) {
            outAgents.push_back(id);
        }
    });
}

NavBindingReport BindAgentsToNavMesh(Scene& scene, EntityID navMeshEntity, const std::vector<EntityID>& agents)
{
    NavBindingReport report{};
    EntityData* navMeshData = scene.GetEntityData(navMeshEntity);
    nav::NavMeshComponent* navMesh = (navMeshData && navMeshData->Navigation)
        ? navMeshData->Navigation.get()
        : nullptr;

    for (EntityID agentId : agents) {
        EntityData* data = scene.GetEntityData(agentId);
        if (!data || !data->NavAgent) {
            continue;
        }

        ++report.TotalAgents;
        data->NavAgent->NavMeshEntity = navMeshEntity;
        ++report.BoundAgents;

        glm::vec3 nearest(0.0f);
        if (TryProjectAgentOntoNavMesh(scene, navMeshEntity, navMesh, *data, nearest)) {
            data->NavAgent->Warp(nearest, &data->Transform, &Physics::Get(), data->RigidBody.get(), data->Collider.get());
            ++report.OnMeshAgents;
        }
    }

    scene.UpdateTransforms();
    return report;
}

CrowdDestinationReport AssignCrowdDestinations(Scene& scene,
                                               EntityID navMeshEntity,
                                               const std::vector<EntityID>& agents,
                                               const glm::vec3& crowdCenter,
                                               float travelRadius)
{
    CrowdDestinationReport report{};
    if (agents.empty()) {
        return report;
    }

    const float safeRadius = std::max(2.0f, travelRadius);
    const float maxProjectDistance = safeRadius * 2.0f + 4.0f;

    for (size_t index = 0; index < agents.size(); ++index) {
        EntityData* data = scene.GetEntityData(agents[index]);
        if (!data || !data->NavAgent) {
            continue;
        }

        ++report.TotalAgents;
        const glm::vec3 currentPos = glm::vec3(data->Transform.WorldMatrix[3]);
        glm::vec3 destination = crowdCenter - (currentPos - crowdCenter);
        if (glm::length2(destination - currentPos) < 4.0f) {
            const float angle = glm::two_pi<float>() * (static_cast<float>(index) / std::max(1.0f, static_cast<float>(agents.size())));
            destination = crowdCenter + glm::vec3(std::cos(angle), 0.0f, std::sin(angle)) * safeRadius;
        }

        glm::vec3 projected(0.0f);
        if (nav::Navigation::Get().NearestPoint(scene, navMeshEntity, destination, maxProjectDistance, projected)) {
            destination = projected;
            ++report.ProjectedDestinations;
        }

        data->NavAgent->NavMeshEntity = navMeshEntity;
        data->NavAgent->SetDestination(destination);
        ++report.IssuedDestinations;
    }

    return report;
}

void CollectHarnessCounters(Scene& scene,
                            EntityID cameraEntity,
                            EntityID navMeshEntity,
                            EntityID playerProxyEntity,
                            const std::vector<EntityID>& activeRoots,
                            const std::vector<EntityID>& navAgents)
{
    Profiler& profiler = Profiler::Get();

    uint64_t visibleRoots = 0;
    if (cameraEntity != INVALID_ENTITY_ID) {
        EntityData* cameraData = scene.GetEntityData(cameraEntity);
        if (cameraData && cameraData->Camera) {
            const ViewFrustum frustum = BuildViewFrustum(
                cameraData->Camera->Camera.GetViewMatrix(),
                cameraData->Camera->Camera.GetProjectionMatrix());
            for (EntityID rootId : activeRoots) {
                glm::vec3 rootMin(0.0f);
                glm::vec3 rootMax(0.0f);
                if (!ComputeHierarchyWorldBounds(scene, rootId, rootMin, rootMax)) {
                    continue;
                }
                if (AabbIntersectsFrustum(frustum, rootMin, rootMax)) {
                    ++visibleRoots;
                }
            }
        }
    }

    uint64_t navAgentsBound = 0;
    uint64_t navAgentsOnMesh = 0;
    uint64_t navAgentsWithPath = 0;
    uint64_t navAgentsWithDestination = 0;
    uint64_t navAgentsMoving = 0;
    for (EntityID agentId : navAgents) {
        EntityData* data = scene.GetEntityData(agentId);
        if (!data || !data->NavAgent) {
            continue;
        }

        if (data->NavAgent->NavMeshEntity == navMeshEntity) {
            ++navAgentsBound;
        }
        if (data->NavAgent->HasDestination) {
            ++navAgentsWithDestination;
        }
        if (data->NavAgent->HasPath()) {
            ++navAgentsWithPath;
        }
        if (glm::length2(data->NavAgent->CurrentVelocity) > 1e-4f) {
            ++navAgentsMoving;
        }

        glm::vec3 nearest(0.0f);
        if (navMeshEntity != INVALID_ENTITY_ID &&
            TryProjectAgentOntoNavMesh(scene, navMeshEntity, nullptr, *data, nearest)) {
            ++navAgentsOnMesh;
        }
    }

    uint64_t rootsWithinPlayerNear = 0;
    uint64_t rootsWithinPlayerMedium = 0;
    uint64_t rootsWithinPlayerFar = 0;
    if (playerProxyEntity != INVALID_ENTITY_ID) {
        EntityData* playerData = scene.GetEntityData(playerProxyEntity);
        if (playerData) {
            const glm::vec3 playerPosition = glm::vec3(playerData->Transform.WorldMatrix[3]);
            for (EntityID rootId : activeRoots) {
                EntityData* rootData = scene.GetEntityData(rootId);
                if (!rootData) {
                    continue;
                }
                const glm::vec3 rootPosition = glm::vec3(rootData->Transform.WorldMatrix[3]);
                const float distanceSq = glm::distance2(playerPosition, rootPosition);
                if (distanceSq <= (6.0f * 6.0f)) {
                    ++rootsWithinPlayerNear;
                }
                if (distanceSq <= (12.0f * 12.0f)) {
                    ++rootsWithinPlayerMedium;
                }
                if (distanceSq <= (20.0f * 20.0f)) {
                    ++rootsWithinPlayerFar;
                }
            }
        }
    }

    profiler.SetCounter("LoadTest/ActiveRoots", static_cast<uint64_t>(activeRoots.size()));
    profiler.SetCounter("LoadTest/VisibleRoots", visibleRoots);
    profiler.SetCounter("LoadTest/NavAgentsTotal", static_cast<uint64_t>(navAgents.size()));
    profiler.SetCounter("LoadTest/NavAgentsBound", navAgentsBound);
    profiler.SetCounter("LoadTest/NavAgentsOnMesh", navAgentsOnMesh);
    profiler.SetCounter("LoadTest/NavAgentsWithPath", navAgentsWithPath);
    profiler.SetCounter("LoadTest/NavAgentsWithDestination", navAgentsWithDestination);
    profiler.SetCounter("LoadTest/NavAgentsMoving", navAgentsMoving);
    profiler.SetCounter("LoadTest/RootsWithinPlayerNear", rootsWithinPlayerNear);
    profiler.SetCounter("LoadTest/RootsWithinPlayerMedium", rootsWithinPlayerMedium);
    profiler.SetCounter("LoadTest/RootsWithinPlayerFar", rootsWithinPlayerFar);
}

bool IsStageSteadyState(const std::vector<FrameCapture>& history,
                        const PrefabLoadTestPanel::BenchmarkConfig& config,
                        size_t activeRootCount,
                        size_t navAgentCount,
                        std::string& outReason)
{
    if (!config.RequireSteadyState) {
        outReason = "steady-state checks disabled";
        return true;
    }

    const size_t minFrames = static_cast<size_t>(std::max(0, config.MinStabilizationFrames));
    const size_t windowFrames = static_cast<size_t>(std::max(5, config.StabilizationWindowFrames));
    if (history.size() < minFrames) {
        outReason = "waiting for minimum stabilization frames";
        return false;
    }
    if (history.size() < windowFrames * 2) {
        outReason = "collecting enough stabilization history";
        return false;
    }

    auto buildWindow = [&](size_t startIndex) {
        std::vector<double> values;
        values.reserve(windowFrames);
        for (size_t index = startIndex; index < startIndex + windowFrames; ++index) {
            values.push_back(history[index].FrameMs);
        }
        return values;
    };

    const size_t currentStart = history.size() - windowFrames;
    const size_t previousStart = history.size() - (windowFrames * 2);
    const PrefabLoadTestPanel::NumericStats currentFrameStats =
        ComputeStats(buildWindow(currentStart));
    const PrefabLoadTestPanel::NumericStats previousFrameStats =
        ComputeStats(buildWindow(previousStart));

    const double averageDeltaPct =
        PercentDelta(previousFrameStats.Average, currentFrameStats.Average);
    const double p95DeltaPct =
        PercentDelta(previousFrameStats.P95, currentFrameStats.P95);

    double minVisibleRoots = std::numeric_limits<double>::max();
    double maxNavAgentsUnbound = 0.0;
    double minNavAgentsOnMesh = std::numeric_limits<double>::max();
    double maxNavNeedsPath = 0.0;
    double maxNavPendingQueue = 0.0;
    double minAnimationEntitiesEvaluated = std::numeric_limits<double>::max();
    double maxAnimationCoverage = 0.0;
    double maxAnimationLodSkipped = 0.0;
    double maxScriptsLodSkipped = 0.0;

    for (size_t index = currentStart; index < history.size(); ++index) {
        const FrameCapture& sample = history[index];
        const double activeRoots = CounterValueOrZero(sample, "LoadTest/ActiveRoots");
        const double visibleRoots = CounterValueOrZero(sample, "LoadTest/VisibleRoots");
        const double navAgentsTotal = CounterValueOrZero(sample, "LoadTest/NavAgentsTotal");
        const double navAgentsBound = CounterValueOrZero(sample, "LoadTest/NavAgentsBound");
        const double navAgentsOnMesh = CounterValueOrZero(sample, "LoadTest/NavAgentsOnMesh");
        const double animationEntitiesEvaluated =
            CounterValueOrZero(sample, "Animation/EntitiesEvaluated");
        const double animationLodSkipped =
            CounterValueOrZero(sample, "Animation/LodSkipped");
        minVisibleRoots = std::min(minVisibleRoots, visibleRoots);
        maxNavAgentsUnbound = std::max(maxNavAgentsUnbound, navAgentsTotal - navAgentsBound);
        minNavAgentsOnMesh = std::min(minNavAgentsOnMesh, navAgentsOnMesh);
        maxNavNeedsPath = std::max(maxNavNeedsPath, CounterValueOrZero(sample, "Nav/NeedsPath"));
        maxNavPendingQueue = std::max(maxNavPendingQueue, CounterValueOrZero(sample, "Nav/PendingQueue"));
        minAnimationEntitiesEvaluated =
            std::min(minAnimationEntitiesEvaluated, animationEntitiesEvaluated);
        maxAnimationCoverage =
            std::max(maxAnimationCoverage, animationEntitiesEvaluated + animationLodSkipped);
        maxAnimationLodSkipped = std::max(maxAnimationLodSkipped, animationLodSkipped);
        maxScriptsLodSkipped =
            std::max(maxScriptsLodSkipped, CounterValueOrZero(sample, "Scripts/LodSkipped"));

        if (activeRoots > 0.0 && activeRoots < static_cast<double>(activeRootCount)) {
            outReason = "active root count changed during stabilization";
            return false;
        }
    }

    if (averageDeltaPct > config.StabilizationTolerancePct ||
        p95DeltaPct > config.StabilizationTolerancePct) {
        std::ostringstream reason;
        reason << "frame timings still drifting (avg delta "
               << std::fixed << std::setprecision(2) << averageDeltaPct
               << "%, p95 delta " << p95DeltaPct << "%)";
        outReason = reason.str();
        return false;
    }

    if (navAgentCount > 0) {
        if (maxNavAgentsUnbound > 0.0 || minNavAgentsOnMesh + 0.5 < static_cast<double>(navAgentCount)) {
            outReason = "navigation agents are not fully bound to the test navmesh";
            return false;
        }
        if (maxNavNeedsPath > 0.0 || maxNavPendingQueue > 0.0) {
            outReason = "navigation is still converging";
            return false;
        }
    }

    if (ScenarioRequiresVisibility(config.Scenario) &&
        minVisibleRoots + 0.5 < static_cast<double>(activeRootCount)) {
        outReason = "not all active prefab roots are visible in the benchmark camera";
        return false;
    }

    if (ScenarioRequiresFullLod(config.Scenario)) {
        if (maxAnimationLodSkipped > 0.0) {
            outReason = "animation LOD is still skipping work in full-LOD mode";
            return false;
        }
        if (maxAnimationCoverage > 0.5 &&
            minAnimationEntitiesEvaluated + 0.5 < maxAnimationCoverage) {
            outReason = "not all animated entities are evaluated every frame in full-LOD mode";
            return false;
        }
        if (maxScriptsLodSkipped > 0.0) {
            outReason = "script LOD is still skipping updates in full-LOD mode";
            return false;
        }
    }

    outReason = "steady state reached";
    return true;
}

FrameCapture CaptureBenchmarkFrame(Scene& scene,
                                   const BenchmarkSceneState& state,
                                   const PrefabLoadTestPanel::BenchmarkConfig& config,
                                   const std::vector<EntityID>& activeRoots,
                                   int instanceCount,
                                   int stageIndex)
{
    FrameCapture sample{};

    Profiler& profiler = Profiler::Get();
    Renderer& renderer = Renderer::Get();
    const bool renderVisuals = state.HasVisualContent;
    const EntityID cameraEntity = state.CameraEntity;
    const int width = config.RenderWidth;
    const int height = config.RenderHeight;
    const bool hasParticleEmitters = state.HasParticleEmitters;

    profiler.BeginFrame();
    const auto frameStart = std::chrono::high_resolution_clock::now();
    Scene::CurrentScene = &scene;

    const auto updateStart = std::chrono::high_resolution_clock::now();
    scene.Update(kSimulatedDeltaTime);
    const auto updateEnd = std::chrono::high_resolution_clock::now();
    sample.SceneUpdateMs = std::chrono::duration<double, std::milli>(updateEnd - updateStart).count();

    const auto audioStart = std::chrono::high_resolution_clock::now();
    Audio::Update(kSimulatedDeltaTime);
    const auto audioEnd = std::chrono::high_resolution_clock::now();
    sample.AudioMs = std::chrono::duration<double, std::milli>(audioEnd - audioStart).count();

    if (renderVisuals) {
        EntityData* cameraData = scene.GetEntityData(cameraEntity);
        if (!cameraData || !cameraData->Camera) {
            sample.ErrorMessage = "Benchmark camera was not available while rendering.";
        } else {
            const auto renderStart = std::chrono::high_resolution_clock::now();
            renderer.BeginFrame(0.0f, 0.0f, 0.0f);
            const bgfx::TextureHandle texture = renderer.RenderSceneToTexture(
                &scene,
                static_cast<uint32_t>(std::max(1, width)),
                static_cast<uint32_t>(std::max(1, height)),
                &cameraData->Camera->Camera,
                kLoadTestViewIdBase,
                false,
                0x000000ff,
                false,
                nullptr,
                true);
            if (hasParticleEmitters) {
                const glm::mat4 viewMatrix = cameraData->Camera->Camera.GetViewMatrix();
                const glm::vec3 cameraPosition = cameraData->Camera->Camera.GetPosition();
                const bx::Vec3 eye{cameraPosition.x, cameraPosition.y, cameraPosition.z};
                ecs::ParticleEmitterSystem::Get().RenderAllUnfiltered(
                    static_cast<uint8_t>(kLoadTestViewIdBase),
                    glm::value_ptr(viewMatrix),
                    eye);
            }
            renderer.EndFrame();
            const auto renderEnd = std::chrono::high_resolution_clock::now();
            sample.RenderMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
            if (!bgfx::isValid(texture)) {
                sample.ErrorMessage = "Offscreen rendering failed during the prefab load test.";
            }
        }
    }

    profiler.SetCounter("LoadTest/Instances", static_cast<uint64_t>(std::max(0, instanceCount)));
    profiler.SetCounter("LoadTest/StageIndex", static_cast<uint64_t>(std::max(0, stageIndex)));
    profiler.SetCounter("LoadTest/ScenarioMode", static_cast<uint64_t>(config.Scenario));
    profiler.SetCounter("Audio/ActiveSounds", static_cast<uint64_t>(Audio::GetActiveSoundCount()));
    profiler.SetCounter("Audio/PendingLoads", static_cast<uint64_t>(Audio::GetPendingLoadCount()));
    CollectHarnessCounters(
        scene,
        state.CameraEntity,
        state.NavMeshEntity,
        state.PlayerProxyEntity,
        activeRoots,
        state.NavAgents);

    const auto frameEnd = std::chrono::high_resolution_clock::now();
    sample.FrameMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();

    profiler.EndFrame();

    sample.Entries = profiler.GetSortedLastFrameEntriesByTimeDesc();
    sample.Counters = profiler.GetLastFrameCounters();
    const Profiler::MemoryStats memory = profiler.GetProcessMemory();
    sample.WorkingSetBytes = memory.workingSetBytes;
    sample.PrivateBytes = memory.privateBytes;
    sample.RuntimeStats = renderer.GetLastRuntimeStatsFrame();
    return sample;
}

void PadWithZeros(std::vector<double>& values, size_t count)
{
    if (values.size() < count) {
        values.resize(count, 0.0);
    }
}

json StatsToJson(const PrefabLoadTestPanel::NumericStats& stats)
{
    return json{
        {"average", stats.Average},
        {"min", stats.Min},
        {"max", stats.Max},
        {"p50", stats.P50},
        {"p95", stats.P95},
        {"p99", stats.P99},
        {"samples", stats.Samples}
    };
}

std::string BoolWord(bool value)
{
    return value ? "Yes" : "No";
}

const PrefabLoadTestPanel::NumericStats* FindCounterValues(const PrefabLoadTestPanel::StageResult& stage,
                                                           const char* name)
{
    for (const auto& counter : stage.Counters) {
        if (counter.first == name) {
            return &counter.second.Values;
        }
    }
    return nullptr;
}

double AverageOrZero(const PrefabLoadTestPanel::NumericStats* stats)
{
    return stats ? stats->Average : 0.0;
}

double P95OrZero(const PrefabLoadTestPanel::NumericStats* stats)
{
    return stats ? stats->P95 : 0.0;
}

void AppendTopSections(std::ostringstream& stream,
                       const std::vector<std::pair<std::string, PrefabLoadTestPanel::SectionStats>>& sections,
                       size_t limit)
{
    size_t written = 0;
    for (const auto& section : sections) {
        if (written >= limit) {
            break;
        }
        stream << "    " << section.first
               << " avg=" << std::fixed << std::setprecision(3) << section.second.TotalMs.Average
               << "ms p95=" << section.second.TotalMs.P95
               << "ms calls(avg)=" << section.second.CallCount.Average << "\n";
        ++written;
    }
}

} // namespace

PrefabLoadTestPanel::PrefabLoadTestPanel(UILayer* uiLayer)
    : m_UILayer(uiLayer)
{
}

void PrefabLoadTestPanel::RefreshPrefabOptions()
{
    m_Prefabs.clear();

    const auto assets = AssetLibrary::Instance().GetAllAssets();
    m_Prefabs.reserve(assets.size());
    for (const auto& asset : assets) {
        if (std::get<2>(asset) != AssetType::Prefab) {
            continue;
        }

        PrefabOption option{};
        option.Path = NormalizePath(std::get<0>(asset));
        option.DisplayName = LeafName(option.Path) + "  [" + option.Path + "]";
        m_Prefabs.push_back(std::move(option));
    }

    std::sort(m_Prefabs.begin(), m_Prefabs.end(), [](const PrefabOption& a, const PrefabOption& b) {
        return a.DisplayName < b.DisplayName;
    });

    if (m_Prefabs.empty()) {
        m_SelectedPrefabIndex = -1;
        return;
    }

    if (m_SelectedPrefabIndex < 0 || m_SelectedPrefabIndex >= static_cast<int>(m_Prefabs.size())) {
        m_SelectedPrefabIndex = 0;
    }
}

void PrefabLoadTestPanel::QueueRunFromUI()
{
    if (m_SelectedPrefabIndex < 0 || m_SelectedPrefabIndex >= static_cast<int>(m_Prefabs.size())) {
        m_StatusMessage = "Select a registered prefab before starting a load test.";
        return;
    }

    m_PendingRequest.PrefabPath = m_Prefabs[m_SelectedPrefabIndex].Path;
    m_PendingRequest.Config = m_Config;
    m_HasPendingRequest = true;
    m_StatusMessage = "Prefab load test queued.";
}

void PrefabLoadTestPanel::ProcessPendingRun()
{
    if (!m_HasPendingRequest || m_Running) {
        return;
    }

    if (m_UILayer && (m_UILayer->IsPlayMode() || m_UILayer->IsExternalRuntimePreviewActive())) {
        m_StatusMessage = "Stop play mode and close any external runtime preview before running a prefab load test.";
        m_HasPendingRequest = false;
        return;
    }

    m_Running = true;
    m_Open = true;

    Logger::Log("[PrefabLoadTest] Running benchmark for " + m_PendingRequest.PrefabPath);
    RunResult result = RunBenchmark(m_PendingRequest);
    if (result.Success && result.Config.AutoExport) {
        if (ExportResult(result)) {
            m_StatusMessage = "Prefab load test finished and exported.";
        } else {
            m_StatusMessage = "Prefab load test finished, but export failed.";
        }
    } else if (result.Success) {
        m_StatusMessage = "Prefab load test finished.";
    } else {
        m_StatusMessage = result.ErrorMessage.empty() ? "Prefab load test failed." : result.ErrorMessage;
        Logger::LogError("[PrefabLoadTest] " + m_StatusMessage);
    }

    m_LastResult = std::move(result);
    m_HasLastResult = true;
    m_Running = false;
    m_HasPendingRequest = false;
}

bool PrefabLoadTestPanel::ExportResult(RunResult& result) const
{
    const fs::path projectRoot = Project::GetProjectDirectory();
    if (projectRoot.empty()) {
        return false;
    }

    const fs::path logsDir = projectRoot / "logs" / "prefab_load_tests";
    std::error_code createError;
    fs::create_directories(logsDir, createError);
    if (createError) {
        return false;
    }

    const std::string fileStem = SanitizeFileToken(StemName(result.PrefabPath)) + "_" + FormatTimestampToken();
    const fs::path jsonPath = logsDir / (fileStem + ".json");
    const fs::path logPath = logsDir / (fileStem + ".log");

    json output;
    output["success"] = result.Success;
    output["prefabPath"] = result.PrefabPath;
    output["prefabName"] = result.PrefabName;
    output["startedAtLocal"] = result.StartedAtLocal;
    output["config"] = {
        {"scenario", ScenarioModeToken(result.Config.Scenario)},
        {"scenarioLabel", ScenarioModeLabel(result.Config.Scenario)},
        {"maxInstances", result.Config.MaxInstances},
        {"instanceStep", result.Config.InstanceStep},
        {"warmupFrames", result.Config.WarmupFrames},
        {"sampleFrames", result.Config.SampleFrames},
        {"requireSteadyState", result.Config.RequireSteadyState},
        {"minStabilizationFrames", result.Config.MinStabilizationFrames},
        {"maxStabilizationFrames", result.Config.MaxStabilizationFrames},
        {"stabilizationWindowFrames", result.Config.StabilizationWindowFrames},
        {"stabilizationTolerancePct", result.Config.StabilizationTolerancePct},
        {"renderWidth", result.Config.RenderWidth},
        {"renderHeight", result.Config.RenderHeight},
        {"unplayableFps", result.Config.UnplayableFps},
        {"stopAtUnplayable", result.Config.StopAtUnplayable},
        {"autoExport", result.Config.AutoExport}
    };
    output["composition"] = {
        {"entityCount", result.Composition.EntityCount},
        {"meshCount", result.Composition.MeshCount},
        {"skinnedMeshCount", result.Composition.SkinnedMeshCount},
        {"cameraCount", result.Composition.CameraCount},
        {"lightCount", result.Composition.LightCount},
        {"particleEmitterCount", result.Composition.ParticleEmitterCount},
        {"navigationMeshCount", result.Composition.NavigationMeshCount},
        {"navAgentCount", result.Composition.NavAgentCount},
        {"navLinkCount", result.Composition.NavLinkCount},
        {"characterControllerCount", result.Composition.CharacterControllerCount},
        {"audioSourceCount", result.Composition.AudioSourceCount},
        {"scriptCount", result.Composition.ScriptCount},
        {"rigidBodyCount", result.Composition.RigidBodyCount},
        {"staticBodyCount", result.Composition.StaticBodyCount},
        {"hasVisualContent", result.Composition.HasVisualContent},
        {"needsNavigationFloor", result.Composition.NeedsNavigationFloor}
    };
    output["firstUnplayableInstanceCount"] = result.FirstUnplayableInstanceCount;
    output["unplayableFrameBudgetMs"] = result.UnplayableFrameBudgetMs;
    output["unplayableReason"] = result.UnplayableReason;
    output["errorMessage"] = result.ErrorMessage;
    output["stages"] = json::array();

    for (const StageResult& stage : result.Stages) {
        json stageJson;
        stageJson["instanceCount"] = stage.InstanceCount;
        stageJson["warmupFramesUsed"] = stage.WarmupFramesUsed;
        stageJson["stabilizationFramesUsed"] = stage.StabilizationFramesUsed;
        stageJson["unplayable"] = stage.Unplayable;
        stageJson["reachedSteadyState"] = stage.ReachedSteadyState;
        stageJson["stabilizationReason"] = stage.StabilizationReason;
        stageJson["frameMs"] = StatsToJson(stage.FrameMs);
        stageJson["sceneUpdateMs"] = StatsToJson(stage.SceneUpdateMs);
        stageJson["audioMs"] = StatsToJson(stage.AudioMs);
        stageJson["renderMs"] = StatsToJson(stage.RenderMs);
        stageJson["workingSetMb"] = StatsToJson(stage.WorkingSetMb);
        stageJson["privateMb"] = StatsToJson(stage.PrivateMb);
        stageJson["renderedMeshObjects"] = StatsToJson(stage.RenderedMeshObjects);
        stageJson["culledMeshObjects"] = StatsToJson(stage.CulledMeshObjects);
        stageJson["renderedSkinnedMeshObjects"] = StatsToJson(stage.RenderedSkinnedMeshObjects);
        stageJson["culledSkinnedMeshObjects"] = StatsToJson(stage.CulledSkinnedMeshObjects);

        json sections = json::array();
        for (const auto& section : stage.Sections) {
            sections.push_back({
                {"name", section.first},
                {"totalMs", StatsToJson(section.second.TotalMs)},
                {"callCount", StatsToJson(section.second.CallCount)}
            });
        }
        stageJson["sections"] = std::move(sections);

        json counters = json::array();
        for (const auto& counter : stage.Counters) {
            counters.push_back({
                {"name", counter.first},
                {"values", StatsToJson(counter.second.Values)}
            });
        }
        stageJson["counters"] = std::move(counters);

        output["stages"].push_back(std::move(stageJson));
    }

    {
        std::ofstream stream(jsonPath);
        if (!stream.is_open()) {
            return false;
        }
        stream << std::setw(2) << output << std::endl;
    }

    std::ostringstream summary;
    summary << "Prefab Load Test\n";
    summary << "Prefab: " << result.PrefabName << "\n";
    summary << "Path: " << result.PrefabPath << "\n";
    summary << "Started: " << result.StartedAtLocal << "\n";
    summary << "Scenario: " << ScenarioModeLabel(result.Config.Scenario) << "\n";
    summary << "Unplayable budget: " << std::fixed << std::setprecision(3) << result.UnplayableFrameBudgetMs << " ms\n";
    summary << "Steady-state required: " << BoolWord(result.Config.RequireSteadyState) << "\n";
    if (result.Config.RequireSteadyState) {
        summary << "Stabilization settings: min=" << result.Config.MinStabilizationFrames
                << " max=" << result.Config.MaxStabilizationFrames
                << " window=" << result.Config.StabilizationWindowFrames
                << " tolerance=" << result.Config.StabilizationTolerancePct << "%\n";
    }
    summary << "First unplayable instance count: ";
    if (result.FirstUnplayableInstanceCount > 0) {
        summary << result.FirstUnplayableInstanceCount;
    } else {
        summary << "not reached";
    }
    summary << "\n";
    if (!result.UnplayableReason.empty()) {
        summary << "Reason: " << result.UnplayableReason << "\n";
    }
    summary << "\nComposition\n";
    summary << "  Entities: " << result.Composition.EntityCount << "\n";
    summary << "  Meshes: " << result.Composition.MeshCount << "\n";
    summary << "  Skinned Meshes: " << result.Composition.SkinnedMeshCount << "\n";
    summary << "  Particle Emitters: " << result.Composition.ParticleEmitterCount << "\n";
    summary << "  Nav Agents: " << result.Composition.NavAgentCount << "\n";
    summary << "  Audio Sources: " << result.Composition.AudioSourceCount << "\n";
    summary << "  Scripts: " << result.Composition.ScriptCount << "\n";
    summary << "\nStages\n";

    for (const StageResult& stage : result.Stages) {
        const PrefabLoadTestPanel::NumericStats* visibleRoots =
            FindCounterValues(stage, "LoadTest/VisibleRoots");
        const PrefabLoadTestPanel::NumericStats* navAgentsBound =
            FindCounterValues(stage, "LoadTest/NavAgentsBound");
        const PrefabLoadTestPanel::NumericStats* navAgentsOnMesh =
            FindCounterValues(stage, "LoadTest/NavAgentsOnMesh");
        const PrefabLoadTestPanel::NumericStats* navAgentsWithPath =
            FindCounterValues(stage, "LoadTest/NavAgentsWithPath");
        const PrefabLoadTestPanel::NumericStats* animationEntitiesEvaluated =
            FindCounterValues(stage, "Animation/EntitiesEvaluated");
        const PrefabLoadTestPanel::NumericStats* animationLodSkipped =
            FindCounterValues(stage, "Animation/LodSkipped");
        const PrefabLoadTestPanel::NumericStats* scriptsLodSkipped =
            FindCounterValues(stage, "Scripts/LodSkipped");

        summary << "  Instance Count: " << stage.InstanceCount
                << " | avg=" << std::fixed << std::setprecision(3) << stage.FrameMs.Average
                << "ms | p95=" << stage.FrameMs.P95
                << "ms | max=" << stage.FrameMs.Max
                << "ms | unplayable=" << BoolWord(stage.Unplayable) << "\n";
        summary << "    warmup=" << stage.WarmupFramesUsed
                << " frames, stabilization=" << stage.StabilizationFramesUsed
                << " frames, steadyState=" << BoolWord(stage.ReachedSteadyState);
        if (!stage.StabilizationReason.empty()) {
            summary << " (" << stage.StabilizationReason << ")";
        }
        summary << "\n";
        summary << "    sceneUpdate(avg/p95)=" << stage.SceneUpdateMs.Average << "/" << stage.SceneUpdateMs.P95
                << " ms, render(avg/p95)=" << stage.RenderMs.Average << "/" << stage.RenderMs.P95
                << " ms, audio(avg/p95)=" << stage.AudioMs.Average << "/" << stage.AudioMs.P95 << " ms\n";
        summary << "    renderedMeshes(avg)=" << stage.RenderedMeshObjects.Average
                << ", culledMeshes(avg)=" << stage.CulledMeshObjects.Average
                << ", privateMB(avg/p95)=" << stage.PrivateMb.Average << "/" << stage.PrivateMb.P95 << "\n";
        summary << "    visibleRoots(avg/p95)=" << AverageOrZero(visibleRoots) << "/" << P95OrZero(visibleRoots)
                << ", navBound(avg)=" << AverageOrZero(navAgentsBound)
                << ", navOnMesh(avg)=" << AverageOrZero(navAgentsOnMesh)
                << ", navWithPath(avg)=" << AverageOrZero(navAgentsWithPath) << "\n";
        summary << "    animationEvaluated(avg)=" << AverageOrZero(animationEntitiesEvaluated)
                << ", animationLodSkipped(avg)=" << AverageOrZero(animationLodSkipped)
                << ", scriptsLodSkipped(avg)=" << AverageOrZero(scriptsLodSkipped) << "\n";
        summary << "    Top Sections\n";
        AppendTopSections(summary, stage.Sections, 8);
    }

    {
        std::ofstream stream(logPath);
        if (!stream.is_open()) {
            return false;
        }
        stream << summary.str();
    }

    result.JsonExportPath = jsonPath.string();
    result.LogExportPath = logPath.string();
    Logger::Log("[PrefabLoadTest] Exported results to " + result.LogExportPath);
    return true;
}

void PrefabLoadTestPanel::ExportLatestResult()
{
    if (!m_HasLastResult) {
        m_StatusMessage = "Run a prefab load test before exporting.";
        return;
    }
    if (!m_LastResult.Success) {
        m_StatusMessage = "The last prefab load test failed, so there is nothing reliable to export.";
        return;
    }

    if (ExportResult(m_LastResult)) {
        m_StatusMessage = "Exported the latest prefab load test results.";
    } else {
        m_StatusMessage = "Failed to export the latest prefab load test results.";
    }
}

PrefabLoadTestPanel::RunResult PrefabLoadTestPanel::RunBenchmark(const BenchmarkRequest& request) const
{
    RunResult result{};
    result.PrefabPath = request.PrefabPath;
    result.PrefabName = LeafName(request.PrefabPath);
    result.Config = request.Config;
    result.StartedAtLocal = FormatNowLocal();
    result.UnplayableFrameBudgetMs = 1000.0 / std::max(1.0, static_cast<double>(request.Config.UnplayableFps));
    const bool scenarioRequiresVisibility = ScenarioRequiresVisibility(request.Config.Scenario);
    const bool scenarioRequiresFullLod = ScenarioRequiresFullLod(request.Config.Scenario);

    if (Project::GetProjectDirectory().empty()) {
        result.ErrorMessage = "No user project is loaded, so prefab load test logs cannot be written.";
        return result;
    }

    Profiler& profiler = Profiler::Get();
    Renderer& renderer = Renderer::Get();

    const bool previousProfilerEnabled = profiler.IsEnabled();
    const bool previousTelemetryEnabled = profiler.IsTelemetryEnabled();
    const bool previousRuntimeStatsEnabled = renderer.GetRuntimeStatsCaptureEnabled();
    Scene* previousCurrentScene = Scene::CurrentScene;

    BenchmarkSceneState state{};
    state.SceneInstance.m_IsPlaying = true;
    Scene::CurrentScene = &state.SceneInstance;

    state.SceneInstance.GetEnvironment().ProceduralSky = false;
    state.SceneInstance.GetEnvironment().UseSkybox = false;
    state.SceneInstance.GetEnvironment().EnableFog = false;
    state.SceneInstance.GetEnvironment().OutlineEnabled = false;
    state.SceneInstance.GetEnvironment().ShadowsEnabled = false;
    state.SceneInstance.GetEnvironment().AmbientColor = glm::vec3(0.8f);
    state.SceneInstance.GetEnvironment().AmbientIntensity = 1.25f;

    profiler.SetEnabled(true);
    profiler.SetTelemetryEnabled(true);
    renderer.SetRuntimeStatsCaptureEnabled(false);

    auto restoreState = [&]() {
        renderer.ReleaseOffscreenTarget(kLoadTestViewIdBase);
        renderer.SetRuntimeStatsCaptureEnabled(previousRuntimeStatsEnabled);
        profiler.SetEnabled(previousProfilerEnabled);
        profiler.SetTelemetryEnabled(previousTelemetryEnabled);
        Audio::StopAll();
        ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(&state.SceneInstance);
        runtime::RuntimePrefabInstantiator::CancelAsyncForScene(state.SceneInstance, true);
        state.SceneInstance.OnStop();
        Scene::CurrentScene = previousCurrentScene;
    };

    auto fail = [&](const std::string& message) {
        result.ErrorMessage = message;
        restoreState();
        return result;
    };

    auto navBindingFailure = [&](const NavBindingReport& report, int instanceCount) {
        std::ostringstream message;
        message << "Stage " << instanceCount
                << " could not bind every nav agent to the benchmark navmesh (assigned "
                << report.BoundAgents << "/" << report.TotalAgents
                << ", on-mesh " << report.OnMeshAgents << "/" << report.TotalAgents << ").";
        return fail(message.str());
    };

    const EntityID firstRoot = InstantiatePrefabFromPathBlocking(request.PrefabPath, state.SceneInstance);
    if (firstRoot == INVALID_ENTITY_ID) {
        return fail("Failed to instantiate the selected prefab for load testing.");
    }

    SyncSceneTransforms(state.SceneInstance);
    result.Composition = AnalyzePrefabComposition(state.SceneInstance, firstRoot);
    state.HasParticleEmitters = HierarchyHasParticleEmitters(state.SceneInstance, firstRoot);
    state.HasVisualContent = result.Composition.HasVisualContent;
    state.NeedsNavigationFloor = result.Composition.NeedsNavigationFloor;
    if (scenarioRequiresVisibility && !state.HasVisualContent) {
        return fail("The selected scenario requires visible mesh or skinned-mesh content, but the prefab does not expose any renderable bounds.");
    }

    DisablePrefabOwnedCameras(state.SceneInstance, firstRoot);

    ComputeHierarchyWorldBounds(state.SceneInstance, firstRoot, state.SingleBoundsMin, state.SingleBoundsMax);
    state.RootGroundOffsetY =
        ResolveRootGroundOffsetY(state.SceneInstance, firstRoot, state.SingleBoundsMin.y);
    state.CrowdPositions = BuildCrowdPositions(
        request.Config.Scenario,
        request.Config.MaxInstances,
        state.SingleBoundsMin,
        state.SingleBoundsMax,
        state.RootGroundOffsetY);

    EntityData* firstRootData = state.SceneInstance.GetEntityData(firstRoot);
    if (!firstRootData) {
        return fail("The benchmark scene lost the initial prefab root unexpectedly.");
    }
    firstRootData->Transform.Position = state.CrowdPositions.front();
    firstRootData->Transform.TransformDirty = true;
    state.SceneInstance.MarkTransformDirty(firstRoot);
    SyncSceneTransforms(state.SceneInstance);

    state.PrefabRoots.push_back(firstRoot);
    CollectNavAgents(state.SceneInstance, firstRoot, state.NavAgents);

    if (state.HasVisualContent) {
        state.CameraEntity = CreateBenchmarkCamera(
            state.SceneInstance,
            request.Config.RenderWidth,
            request.Config.RenderHeight);
        if (state.CameraEntity == INVALID_ENTITY_ID) {
            return fail("Failed to create the benchmark camera.");
        }

        glm::vec3 initialMin(0.0f);
        glm::vec3 initialMax(0.0f);
        ComputeCombinedBounds(state.SceneInstance, state.PrefabRoots, INVALID_ENTITY_ID, initialMin, initialMax);
        UpdateBenchmarkCamera(
            state.SceneInstance,
            state.CameraEntity,
            initialMin,
            initialMax,
            request.Config.RenderWidth,
            request.Config.RenderHeight);
    }

    if (state.NeedsNavigationFloor) {
        state.FloorEntity = CreateNavigationFloor(
            state.SceneInstance,
            state.CrowdPositions,
            state.SingleBoundsMin,
            state.SingleBoundsMax);
        state.NavMeshEntity = CreateNavigationMesh(state.SceneInstance);
        if (state.FloorEntity == INVALID_ENTITY_ID || state.NavMeshEntity == INVALID_ENTITY_ID) {
            return fail("Failed to create the benchmark navigation floor or navmesh.");
        }

        SyncSceneTransforms(state.SceneInstance);

        std::string navError;
        if (!WaitForNavBake(state.SceneInstance, state.NavMeshEntity, navError)) {
            return fail(navError);
        }

        const NavBindingReport bindReport =
            BindAgentsToNavMesh(state.SceneInstance, state.NavMeshEntity, state.NavAgents);
        if (bindReport.TotalAgents > 0 &&
            (bindReport.BoundAgents != bindReport.TotalAgents ||
             bindReport.OnMeshAgents != bindReport.TotalAgents)) {
            return navBindingFailure(bindReport, 1);
        }
    }

    if (state.HasParticleEmitters) {
        TriggerParticleEmitters(state.SceneInstance, firstRoot);
    }

    renderer.SetRuntimeStatsCaptureEnabled(state.HasVisualContent);

    const std::vector<int> stageCounts = BuildStageCounts(request.Config.MaxInstances, request.Config.InstanceStep);
    for (size_t stageIndex = 0; stageIndex < stageCounts.size(); ++stageIndex) {
        const int targetCount = stageCounts[stageIndex];

        while (static_cast<int>(state.PrefabRoots.size()) < targetCount) {
            const EntityID root = InstantiatePrefabFromPathBlocking(request.PrefabPath, state.SceneInstance);
            if (root == INVALID_ENTITY_ID) {
                return fail("A later prefab instance failed to instantiate during the load test.");
            }

            EntityData* rootData = state.SceneInstance.GetEntityData(root);
            if (!rootData) {
                return fail("A later prefab instance was missing immediately after instantiation.");
            }

            DisablePrefabOwnedCameras(state.SceneInstance, root);
            rootData->Transform.Position = state.CrowdPositions[state.PrefabRoots.size()];
            rootData->Transform.TransformDirty = true;
            state.SceneInstance.MarkTransformDirty(root);
            state.PrefabRoots.push_back(root);
            CollectNavAgents(state.SceneInstance, root, state.NavAgents);

            if (state.HasParticleEmitters) {
                TriggerParticleEmitters(state.SceneInstance, root);
            }
        }

        std::vector<EntityID> activeRoots(state.PrefabRoots.begin(), state.PrefabRoots.begin() + targetCount);
        if (scenarioRequiresFullLod) {
            for (EntityID rootId : activeRoots) {
                ApplyFullLodOverrides(state.SceneInstance, rootId);
            }
        }

        SyncSceneTransforms(state.SceneInstance);

        glm::vec3 crowdMin(0.0f);
        glm::vec3 crowdMax(0.0f);
        ComputeCombinedBounds(state.SceneInstance, activeRoots, INVALID_ENTITY_ID, crowdMin, crowdMax);
        const glm::vec3 crowdCenter = 0.5f * (crowdMin + crowdMax);
        const float travelRadius = std::max(crowdMax.x - crowdMin.x, crowdMax.z - crowdMin.z) * 0.3f;

        if (scenarioRequiresVisibility) {
            if (state.PlayerProxyEntity == INVALID_ENTITY_ID) {
                state.PlayerProxyEntity = CreatePlayerProxy(state.SceneInstance, crowdCenter);
                if (state.PlayerProxyEntity == INVALID_ENTITY_ID) {
                    return fail("Failed to create the benchmark player proxy required for on-screen NPC testing.");
                }
            }
            UpdatePlayerProxyPosition(state.SceneInstance, state.PlayerProxyEntity, crowdCenter);
        }

        if (state.NeedsNavigationFloor && state.NavMeshEntity != INVALID_ENTITY_ID) {
            const NavBindingReport bindReport =
                BindAgentsToNavMesh(state.SceneInstance, state.NavMeshEntity, state.NavAgents);
            if (bindReport.TotalAgents > 0 &&
                (bindReport.BoundAgents != bindReport.TotalAgents ||
                 bindReport.OnMeshAgents != bindReport.TotalAgents)) {
                return navBindingFailure(bindReport, targetCount);
            }

            const CrowdDestinationReport destinationReport = AssignCrowdDestinations(
                state.SceneInstance,
                state.NavMeshEntity,
                state.NavAgents,
                crowdCenter,
                travelRadius);
            if (destinationReport.TotalAgents > 0 &&
                destinationReport.IssuedDestinations != destinationReport.TotalAgents) {
                std::ostringstream message;
                message << "Stage " << targetCount
                        << " could not assign destinations to every nav agent (issued "
                        << destinationReport.IssuedDestinations << "/" << destinationReport.TotalAgents << ").";
                return fail(message.str());
            }
        }

        if (state.HasVisualContent && state.CameraEntity != INVALID_ENTITY_ID) {
            UpdateBenchmarkCamera(
                state.SceneInstance,
                state.CameraEntity,
                crowdMin,
                crowdMax,
                request.Config.RenderWidth,
                request.Config.RenderHeight);
        }

        if (state.PlayerProxyEntity != INVALID_ENTITY_ID) {
            SyncSceneTransforms(state.SceneInstance);
        }

        const int warmupFrames = state.HasParticleEmitters
            ? std::max(
                request.Config.WarmupFrames,
                static_cast<int>(std::ceil(ComputeParticleWarmupTime(state.SceneInstance, firstRoot) / kSimulatedDeltaTime)))
            : request.Config.WarmupFrames;
        StageResult stage{};
        stage.InstanceCount = targetCount;
        stage.WarmupFramesUsed = warmupFrames;

        for (int frame = 0; frame < warmupFrames; ++frame) {
            const FrameCapture warmup = CaptureBenchmarkFrame(
                state.SceneInstance,
                state,
                request.Config,
                activeRoots,
                targetCount,
                static_cast<int>(stageIndex));
            if (!warmup.ErrorMessage.empty()) {
                return fail(warmup.ErrorMessage);
            }
        }

        stage.ReachedSteadyState = !request.Config.RequireSteadyState;
        stage.StabilizationReason = request.Config.RequireSteadyState
            ? ""
            : "steady-state checks disabled";
        if (request.Config.RequireSteadyState) {
            std::vector<FrameCapture> stabilizationHistory;
            const int stabilizationWindowFrames = std::max(5, request.Config.StabilizationWindowFrames);
            const int minimumStabilizationFrames =
                std::max(request.Config.MinStabilizationFrames, stabilizationWindowFrames * 2);
            const int maxStabilizationFrames =
                std::max(std::max(request.Config.MaxStabilizationFrames, minimumStabilizationFrames), 0);

            for (int frame = 0; frame < maxStabilizationFrames; ++frame) {
                const FrameCapture stabilizationFrame = CaptureBenchmarkFrame(
                    state.SceneInstance,
                    state,
                    request.Config,
                    activeRoots,
                    targetCount,
                    static_cast<int>(stageIndex));
                if (!stabilizationFrame.ErrorMessage.empty()) {
                    return fail(stabilizationFrame.ErrorMessage);
                }

                stabilizationHistory.push_back(stabilizationFrame);
                ++stage.StabilizationFramesUsed;
                if (IsStageSteadyState(
                        stabilizationHistory,
                        request.Config,
                        activeRoots.size(),
                        state.NavAgents.size(),
                        stage.StabilizationReason)) {
                    stage.ReachedSteadyState = true;
                    break;
                }
            }

            if (!stage.ReachedSteadyState) {
                std::ostringstream message;
                message << "Stage " << targetCount
                        << " did not reach steady state after "
                        << stage.StabilizationFramesUsed << " stabilization frames";
                if (!stage.StabilizationReason.empty()) {
                    message << " (" << stage.StabilizationReason << ")";
                }
                message << ".";
                return fail(message.str());
            }
        }

        AggregateBuilder builder{};
        for (int sampleIndex = 0; sampleIndex < request.Config.SampleFrames; ++sampleIndex) {
            const FrameCapture sample = CaptureBenchmarkFrame(
                state.SceneInstance,
                state,
                request.Config,
                activeRoots,
                targetCount,
                static_cast<int>(stageIndex));
            if (!sample.ErrorMessage.empty()) {
                return fail(sample.ErrorMessage);
            }
            builder.Add(sample);
        }

        stage.FrameMs = ComputeStats(builder.FrameMs);
        stage.SceneUpdateMs = ComputeStats(builder.SceneUpdateMs);
        stage.AudioMs = ComputeStats(builder.AudioMs);
        stage.RenderMs = ComputeStats(builder.RenderMs);
        stage.WorkingSetMb = ComputeStats(builder.WorkingSetMb);
        stage.PrivateMb = ComputeStats(builder.PrivateMb);
        stage.RenderedMeshObjects = ComputeStats(builder.RenderedMeshObjects);
        stage.CulledMeshObjects = ComputeStats(builder.CulledMeshObjects);
        stage.RenderedSkinnedMeshObjects = ComputeStats(builder.RenderedSkinnedMeshObjects);
        stage.CulledSkinnedMeshObjects = ComputeStats(builder.CulledSkinnedMeshObjects);

        for (auto& section : builder.Sections) {
            PadWithZeros(section.second.TimesMs, stage.FrameMs.Samples);
            PadWithZeros(section.second.CallCounts, stage.FrameMs.Samples);

            SectionStats stats{};
            stats.TotalMs = ComputeStats(section.second.TimesMs);
            stats.CallCount = ComputeStats(section.second.CallCounts);
            stage.Sections.emplace_back(section.first, std::move(stats));
        }
        std::sort(stage.Sections.begin(), stage.Sections.end(), [](const auto& a, const auto& b) {
            return a.second.TotalMs.P95 > b.second.TotalMs.P95;
        });

        for (auto& counter : builder.Counters) {
            PadWithZeros(counter.second.Values, stage.FrameMs.Samples);

            CounterStats stats{};
            stats.Values = ComputeStats(counter.second.Values);
            stage.Counters.emplace_back(counter.first, std::move(stats));
        }
        std::sort(stage.Counters.begin(), stage.Counters.end(), [](const auto& a, const auto& b) {
            return a.second.Values.Average > b.second.Values.Average;
        });

        stage.Unplayable = stage.FrameMs.P95 >= result.UnplayableFrameBudgetMs;
        result.Stages.push_back(std::move(stage));

        if (result.Stages.back().Unplayable && result.FirstUnplayableInstanceCount == 0) {
            result.FirstUnplayableInstanceCount = targetCount;
            std::ostringstream reason;
            reason << "Stage " << targetCount
                   << " crossed the unplayable threshold because p95 frame time hit "
                   << std::fixed << std::setprecision(3) << result.Stages.back().FrameMs.P95
                   << " ms against a budget of " << result.UnplayableFrameBudgetMs << " ms.";
            result.UnplayableReason = reason.str();
            if (request.Config.StopAtUnplayable) {
                break;
            }
        }
    }

    result.Success = !result.Stages.empty();
    restoreState();
    return result;
}

void PrefabLoadTestPanel::RenderResults(const RunResult& result)
{
    ImGui::SeparatorText("Latest Run");
    ImGui::TextWrapped("%s", result.Success ? "Completed successfully." : "Run failed.");
    ImGui::TextWrapped("Prefab: %s", result.PrefabPath.c_str());
    ImGui::TextWrapped("Started: %s", result.StartedAtLocal.c_str());
    ImGui::TextWrapped("Scenario: %s", ScenarioModeLabel(result.Config.Scenario));
    ImGui::Text("Unplayable Budget: %.3f ms (%.1f FPS)", result.UnplayableFrameBudgetMs, result.Config.UnplayableFps);
    if (result.Config.RequireSteadyState) {
        ImGui::Text("Steady State: min %d, max %d, window %d, tolerance %.1f%%",
                    result.Config.MinStabilizationFrames,
                    result.Config.MaxStabilizationFrames,
                    result.Config.StabilizationWindowFrames,
                    result.Config.StabilizationTolerancePct);
    } else {
        ImGui::TextUnformatted("Steady State: disabled");
    }
    if (result.FirstUnplayableInstanceCount > 0) {
        ImGui::Text("First Unplayable Stage: %d instances", result.FirstUnplayableInstanceCount);
    } else {
        ImGui::TextUnformatted("First Unplayable Stage: not reached");
    }
    if (!result.UnplayableReason.empty()) {
        ImGui::TextWrapped("%s", result.UnplayableReason.c_str());
    }
    if (!result.ErrorMessage.empty()) {
        ImGui::TextWrapped("Error: %s", result.ErrorMessage.c_str());
    }
    if (!result.LogExportPath.empty()) {
        ImGui::TextWrapped("Log Export: %s", result.LogExportPath.c_str());
    }
    if (!result.JsonExportPath.empty()) {
        ImGui::TextWrapped("JSON Export: %s", result.JsonExportPath.c_str());
    }

    ImGui::SeparatorText("Prefab Composition");
    if (ImGui::BeginTable("PrefabLoadTestComposition", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        auto row = [](const char* label, size_t value) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%zu", value);
        };
        row("Entities", result.Composition.EntityCount);
        row("Meshes", result.Composition.MeshCount);
        row("Skinned Meshes", result.Composition.SkinnedMeshCount);
        row("Particle Emitters", result.Composition.ParticleEmitterCount);
        row("Nav Agents", result.Composition.NavAgentCount);
        row("Navigation Meshes", result.Composition.NavigationMeshCount);
        row("Nav Links", result.Composition.NavLinkCount);
        row("Character Controllers", result.Composition.CharacterControllerCount);
        row("Audio Sources", result.Composition.AudioSourceCount);
        row("Scripts", result.Composition.ScriptCount);
        row("RigidBodies", result.Composition.RigidBodyCount);
        row("StaticBodies", result.Composition.StaticBodyCount);
        ImGui::EndTable();
    }

    if (result.Stages.empty()) {
        return;
    }

    ImGui::SeparatorText("Stage Summary");
    if (ImGui::BeginTable("PrefabLoadTestStages", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Instances");
        ImGui::TableSetupColumn("Avg Frame");
        ImGui::TableSetupColumn("P95 Frame");
        ImGui::TableSetupColumn("Avg Update");
        ImGui::TableSetupColumn("Avg Render");
        ImGui::TableSetupColumn("Visible Roots");
        ImGui::TableSetupColumn("Nav On Mesh");
        ImGui::TableSetupColumn("Steady");
        ImGui::TableSetupColumn("Unplayable");
        ImGui::TableHeadersRow();

        for (const StageResult& stage : result.Stages) {
            const PrefabLoadTestPanel::NumericStats* visibleRoots =
                FindCounterValues(stage, "LoadTest/VisibleRoots");
            const PrefabLoadTestPanel::NumericStats* navAgentsOnMesh =
                FindCounterValues(stage, "LoadTest/NavAgentsOnMesh");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", stage.InstanceCount);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%.3f ms", stage.FrameMs.Average);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.3f ms", stage.FrameMs.P95);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.3f ms", stage.SceneUpdateMs.Average);
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.3f ms", stage.RenderMs.Average);
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.1f", AverageOrZero(visibleRoots));
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.1f", AverageOrZero(navAgentsOnMesh));
            ImGui::TableSetColumnIndex(7);
            ImGui::TextUnformatted(stage.ReachedSteadyState ? "Yes" : "No");
            ImGui::TableSetColumnIndex(8);
            ImGui::TextUnformatted(stage.Unplayable ? "Yes" : "No");
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText("Stage Details");
    for (const StageResult& stage : result.Stages) {
        std::ostringstream label;
        label << "Instances " << stage.InstanceCount;
        if (stage.Unplayable) {
            label << " (Unplayable)";
        }

        if (!ImGui::TreeNode(label.str().c_str())) {
            continue;
        }

        const PrefabLoadTestPanel::NumericStats* visibleRoots =
            FindCounterValues(stage, "LoadTest/VisibleRoots");
        const PrefabLoadTestPanel::NumericStats* navAgentsBound =
            FindCounterValues(stage, "LoadTest/NavAgentsBound");
        const PrefabLoadTestPanel::NumericStats* navAgentsOnMesh =
            FindCounterValues(stage, "LoadTest/NavAgentsOnMesh");
        const PrefabLoadTestPanel::NumericStats* navAgentsWithPath =
            FindCounterValues(stage, "LoadTest/NavAgentsWithPath");
        const PrefabLoadTestPanel::NumericStats* animationEntitiesEvaluated =
            FindCounterValues(stage, "Animation/EntitiesEvaluated");
        const PrefabLoadTestPanel::NumericStats* animationLodSkipped =
            FindCounterValues(stage, "Animation/LodSkipped");
        const PrefabLoadTestPanel::NumericStats* scriptsLodSkipped =
            FindCounterValues(stage, "Scripts/LodSkipped");

        ImGui::Text("Frame avg/p95/max: %.3f / %.3f / %.3f ms", stage.FrameMs.Average, stage.FrameMs.P95, stage.FrameMs.Max);
        ImGui::Text("Update avg/p95/max: %.3f / %.3f / %.3f ms", stage.SceneUpdateMs.Average, stage.SceneUpdateMs.P95, stage.SceneUpdateMs.Max);
        ImGui::Text("Render avg/p95/max: %.3f / %.3f / %.3f ms", stage.RenderMs.Average, stage.RenderMs.P95, stage.RenderMs.Max);
        ImGui::Text("Audio avg/p95/max: %.3f / %.3f / %.3f ms", stage.AudioMs.Average, stage.AudioMs.P95, stage.AudioMs.Max);
        ImGui::Text("Private MB avg/p95: %.2f / %.2f", stage.PrivateMb.Average, stage.PrivateMb.P95);
        ImGui::Text("Warmup/Stabilization Frames: %d / %d", stage.WarmupFramesUsed, stage.StabilizationFramesUsed);
        if (!stage.StabilizationReason.empty()) {
            ImGui::TextWrapped("Steady State: %s (%s)",
                               stage.ReachedSteadyState ? "Yes" : "No",
                               stage.StabilizationReason.c_str());
        } else {
            ImGui::TextWrapped("Steady State: %s", stage.ReachedSteadyState ? "Yes" : "No");
        }
        ImGui::Text("Rendered meshes avg: %.1f | Culled meshes avg: %.1f", stage.RenderedMeshObjects.Average, stage.CulledMeshObjects.Average);
        ImGui::Text("Visible roots avg/p95: %.1f / %.1f", AverageOrZero(visibleRoots), P95OrZero(visibleRoots));
        ImGui::Text("Nav bound/on-mesh/with-path avg: %.1f / %.1f / %.1f",
                    AverageOrZero(navAgentsBound),
                    AverageOrZero(navAgentsOnMesh),
                    AverageOrZero(navAgentsWithPath));
        ImGui::Text("Animation evaluated avg: %.1f | Animation LOD skipped avg: %.1f | Script LOD skipped avg: %.1f",
                    AverageOrZero(animationEntitiesEvaluated),
                    AverageOrZero(animationLodSkipped),
                    AverageOrZero(scriptsLodSkipped));

        if (ImGui::TreeNode("Profiler Sections")) {
            if (ImGui::BeginTable(("PrefabLoadTestSections" + std::to_string(stage.InstanceCount)).c_str(),
                                  5,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Section");
                ImGui::TableSetupColumn("Avg");
                ImGui::TableSetupColumn("P95");
                ImGui::TableSetupColumn("Max");
                ImGui::TableSetupColumn("Calls Avg");
                ImGui::TableHeadersRow();

                for (const auto& section : stage.Sections) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(section.first.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", section.second.TotalMs.Average);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f", section.second.TotalMs.P95);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.3f", section.second.TotalMs.Max);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%.2f", section.second.CallCount.Average);
                }

                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Counters")) {
            if (ImGui::BeginTable(("PrefabLoadTestCounters" + std::to_string(stage.InstanceCount)).c_str(),
                                  4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Counter");
                ImGui::TableSetupColumn("Avg");
                ImGui::TableSetupColumn("P95");
                ImGui::TableSetupColumn("Max");
                ImGui::TableHeadersRow();

                for (const auto& counter : stage.Counters) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(counter.first.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.3f", counter.second.Values.Average);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.3f", counter.second.Values.P95);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%.3f", counter.second.Values.Max);
                }

                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        ImGui::TreePop();
    }
}

void PrefabLoadTestPanel::OnImGuiRender()
{
    if (!m_Open) {
        return;
    }

    if (!ImGui::Begin("Prefab Load Test", &m_Open)) {
        ImGui::End();
        return;
    }

    if (Project::GetProjectDirectory().empty()) {
        ImGui::TextUnformatted("No user project is loaded.");
        ImGui::End();
        return;
    }

    if (m_Prefabs.empty()) {
        RefreshPrefabOptions();
    }

    ImGui::TextWrapped("Benchmark one registered prefab at a time in an isolated scene. Results are exported under the active user project root at logs/prefab_load_tests/.");
    ImGui::Spacing();

    if (ImGui::Button("Refresh Prefabs")) {
        RefreshPrefabOptions();
    }
    ImGui::SameLine();
    const bool canExportLatest = m_HasLastResult && m_LastResult.Success;
    if (!canExportLatest) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Export Latest")) {
        ExportLatestResult();
    }
    if (!canExportLatest) {
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Prefab");
    if (m_Prefabs.empty()) {
        ImGui::TextUnformatted("No registered prefab assets were found in the current user project.");
    } else {
        const char* preview = (m_SelectedPrefabIndex >= 0 && m_SelectedPrefabIndex < static_cast<int>(m_Prefabs.size()))
            ? m_Prefabs[m_SelectedPrefabIndex].DisplayName.c_str()
            : "Select a prefab";
        if (ImGui::BeginCombo("Registered Prefab", preview)) {
            for (int index = 0; index < static_cast<int>(m_Prefabs.size()); ++index) {
                const bool selected = (index == m_SelectedPrefabIndex);
                if (ImGui::Selectable(m_Prefabs[index].DisplayName.c_str(), selected)) {
                    m_SelectedPrefabIndex = index;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SeparatorText("Settings");
    const PrefabLoadTestPanel::ScenarioMode scenarioModes[] = {
        PrefabLoadTestPanel::ScenarioMode::GeneralPrefab,
        PrefabLoadTestPanel::ScenarioMode::OnscreenCrowd,
        PrefabLoadTestPanel::ScenarioMode::OnscreenCrowdFullLod
    };
    if (ImGui::BeginCombo("Scenario", ScenarioModeLabel(m_Config.Scenario))) {
        for (PrefabLoadTestPanel::ScenarioMode scenario : scenarioModes) {
            const bool selected = (scenario == m_Config.Scenario);
            if (ImGui::Selectable(ScenarioModeLabel(scenario), selected)) {
                m_Config.Scenario = scenario;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    if (m_Config.Scenario == PrefabLoadTestPanel::ScenarioMode::GeneralPrefab) {
        ImGui::TextWrapped("General Prefab keeps the harness generic and measures isolated prefab cost without forcing on-screen correctness.");
    } else if (m_Config.Scenario == PrefabLoadTestPanel::ScenarioMode::OnscreenCrowd) {
        ImGui::TextWrapped("Onscreen Crowd keeps all active roots visible and creates a Player proxy so NPC distance-based behavior can settle realistically.");
    } else {
        ImGui::TextWrapped("Onscreen Crowd (Full LOD) keeps all active roots visible, creates a Player proxy, and disables script and animation LOD throttling where the engine exposes those controls.");
    }
    ImGui::InputInt("Max Instances", &m_Config.MaxInstances);
    ImGui::InputInt("Instance Step", &m_Config.InstanceStep);
    ImGui::InputInt("Warmup Frames", &m_Config.WarmupFrames);
    ImGui::InputInt("Sample Frames", &m_Config.SampleFrames);
    ImGui::Checkbox("Require Steady State", &m_Config.RequireSteadyState);
    if (m_Config.RequireSteadyState) {
        ImGui::InputInt("Min Stabilization Frames", &m_Config.MinStabilizationFrames);
        ImGui::InputInt("Max Stabilization Frames", &m_Config.MaxStabilizationFrames);
        ImGui::InputInt("Stabilization Window", &m_Config.StabilizationWindowFrames);
        ImGui::InputFloat("Stabilization Tolerance %", &m_Config.StabilizationTolerancePct, 0.5f, 1.0f, "%.1f");
    }
    ImGui::InputInt("Render Width", &m_Config.RenderWidth);
    ImGui::InputInt("Render Height", &m_Config.RenderHeight);
    ImGui::InputFloat("Unplayable FPS", &m_Config.UnplayableFps, 1.0f, 5.0f, "%.1f");
    ImGui::Checkbox("Stop At First Unplayable Stage", &m_Config.StopAtUnplayable);
    ImGui::Checkbox("Auto Export After Run", &m_Config.AutoExport);

    m_Config.MaxInstances = std::max(1, m_Config.MaxInstances);
    m_Config.InstanceStep = std::max(1, m_Config.InstanceStep);
    m_Config.WarmupFrames = std::max(0, m_Config.WarmupFrames);
    m_Config.SampleFrames = std::max(1, m_Config.SampleFrames);
    m_Config.StabilizationWindowFrames = std::max(5, m_Config.StabilizationWindowFrames);
    m_Config.MinStabilizationFrames =
        std::max(0, std::max(m_Config.MinStabilizationFrames, m_Config.StabilizationWindowFrames * 2));
    m_Config.MaxStabilizationFrames =
        std::max(m_Config.MinStabilizationFrames, m_Config.MaxStabilizationFrames);
    m_Config.RenderWidth = std::max(64, m_Config.RenderWidth);
    m_Config.RenderHeight = std::max(64, m_Config.RenderHeight);
    m_Config.UnplayableFps = std::max(1.0f, m_Config.UnplayableFps);
    m_Config.StabilizationTolerancePct = std::max(0.1f, m_Config.StabilizationTolerancePct);

    if (m_Running) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Run Load Test")) {
        QueueRunFromUI();
    }
    if (m_Running) {
        ImGui::EndDisabled();
    }

    if (!m_StatusMessage.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_StatusMessage.c_str());
    }

    if (m_HasLastResult) {
        RenderResults(m_LastResult);
    }

    ImGui::End();
}
