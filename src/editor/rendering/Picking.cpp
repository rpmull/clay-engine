#include "Picking.h"
#include "editor/EditorSettings.h"
#include "core/rendering/Renderer.h"
#include "core/ecs/Components.h"
#include "core/ecs/EntityData.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cfloat>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace {
enum class QueuedPickKind {
    None,
    Click,
    Hover,
};

QueuedPickKind s_PendingPickKind = QueuedPickKind::None;
bool s_PendingPickPreferRootSelection = false;

constexpr float kRayAabbEpsilon = 1e-6f;
constexpr float kMinLocalRayLengthSq = 1e-20f;
constexpr size_t kMaxOutsidePickCandidates = 64;
constexpr size_t kMaxInsidePickCandidates = 16;

struct PickCandidate {
    EntityID Id = INVALID_ENTITY_ID;
    float SortT = FLT_MAX;
    float FallbackT = FLT_MAX;
    float Diag = 1.0f;
    glm::mat4 Transform{ 1.0f };
    std::shared_ptr<Mesh> MeshRef;
};

bool AreMeshBoundsValid(const Mesh& mesh) {
    return std::isfinite(mesh.BoundsMin.x) &&
           std::isfinite(mesh.BoundsMin.y) &&
           std::isfinite(mesh.BoundsMin.z) &&
           std::isfinite(mesh.BoundsMax.x) &&
           std::isfinite(mesh.BoundsMax.y) &&
           std::isfinite(mesh.BoundsMax.z) &&
           mesh.BoundsMax.x >= mesh.BoundsMin.x &&
           mesh.BoundsMax.y >= mesh.BoundsMin.y &&
           mesh.BoundsMax.z >= mesh.BoundsMin.z;
}

float ApproximateWorldBoundsDiagonal(const glm::mat4& transform, const Mesh& mesh) {
    const glm::vec3 localSize = glm::max(mesh.BoundsMax - mesh.BoundsMin, glm::vec3(0.0f));
    const glm::vec3 axisScale(
        glm::length(glm::vec3(transform[0])),
        glm::length(glm::vec3(transform[1])),
        glm::length(glm::vec3(transform[2])));
    return glm::length(localSize * axisScale);
}

bool TransformRayToLocalSpace(const Ray& ray, const glm::mat4& transform, glm::vec3& outOrigin, glm::vec3& outDirection) {
    const glm::mat4 inverse = glm::affineInverse(transform);
    outOrigin = glm::vec3(inverse * glm::vec4(ray.Origin, 1.0f));
    outDirection = glm::vec3(inverse * glm::vec4(ray.Direction, 0.0f));
    const float directionLengthSq = glm::dot(outDirection, outDirection);
    if (!std::isfinite(directionLengthSq) || directionLengthSq <= kMinLocalRayLengthSq) {
        return false;
    }
    return true;
}

bool RayIntersectsAABBSlab(const glm::vec3& origin,
                           const glm::vec3& direction,
                           const glm::vec3& min,
                           const glm::vec3& max,
                           float& outEntryT,
                           float& outExitT,
                           bool& outOriginInside) {
    float tMin = -FLT_MAX;
    float tMax = FLT_MAX;
    outOriginInside = true;
    const float directionScale = std::sqrt(glm::dot(direction, direction));
    const float parallelEpsilon = glm::max(kRayAabbEpsilon * directionScale, 1e-12f);

    for (int axis = 0; axis < 3; ++axis) {
        const float originAxis = origin[axis];
        const float dirAxis = direction[axis];
        const float minAxis = min[axis];
        const float maxAxis = max[axis];

        if (std::fabs(dirAxis) <= parallelEpsilon) {
            if (originAxis < minAxis || originAxis > maxAxis) {
                return false;
            }
            continue;
        }

        const float invDir = 1.0f / dirAxis;
        float axisEntry = (minAxis - originAxis) * invDir;
        float axisExit = (maxAxis - originAxis) * invDir;
        if (axisEntry > axisExit) {
            std::swap(axisEntry, axisExit);
        }

        tMin = std::max(tMin, axisEntry);
        tMax = std::min(tMax, axisExit);
        if (tMin > tMax) {
            return false;
        }
    }

    if (tMax < 0.0f) {
        return false;
    }

    outOriginInside =
        origin.x >= min.x && origin.x <= max.x &&
        origin.y >= min.y && origin.y <= max.y &&
        origin.z >= min.z && origin.z <= max.z;
    outEntryT = tMin;
    outExitT = tMax;
    return true;
}

void InsertCandidateLimited(std::vector<PickCandidate>& candidates, PickCandidate candidate, size_t maxCount) {
    auto insertIt = std::upper_bound(
        candidates.begin(),
        candidates.end(),
        candidate.SortT,
        [](float value, const PickCandidate& rhs) { return value < rhs.SortT; });
    candidates.insert(insertIt, std::move(candidate));
    if (candidates.size() > maxCount) {
        candidates.pop_back();
    }
}

EntityID EvaluatePickCandidates(const Ray& ray, const std::vector<PickCandidate>& candidates) {
    EntityID bestId = INVALID_ENTITY_ID;
    float bestHitT = FLT_MAX;
    float bestDiag = FLT_MAX;

    for (const PickCandidate& candidate : candidates) {
        if (bestId != INVALID_ENTITY_ID && candidate.SortT > bestHitT + 1e-4f) {
            break;
        }

        float hitT = candidate.FallbackT;
        bool hit = false;
        bool hasExactMeshData = false;
        if (candidate.MeshRef) {
            const Mesh& mesh = *candidate.MeshRef;
            if (!mesh.Vertices.empty() && mesh.Indices.size() >= 3) {
                hasExactMeshData = true;
                hit = Picking::RayIntersectsMesh(ray, mesh, candidate.Transform, hitT);
            }
        }

        // Match Godot's selection behavior more closely: once a render mesh has
        // enough CPU triangle data for an exact test, discard it if that exact
        // test misses rather than letting an oversized AABB claim the click.
        if (!hit && !hasExactMeshData && candidate.FallbackT > 0.0f) {
            hit = true;
            hitT = candidate.FallbackT;
        }
        if (!hit) {
            continue;
        }

        const float tieThreshold = (bestId == INVALID_ENTITY_ID)
            ? 0.0f
            : glm::max(0.001f, bestHitT * 0.001f);
        if (bestId == INVALID_ENTITY_ID ||
            hitT + tieThreshold < bestHitT ||
            (std::fabs(hitT - bestHitT) <= tieThreshold && candidate.Diag < bestDiag)) {
            bestId = candidate.Id;
            bestHitT = hitT;
            bestDiag = candidate.Diag;
        }
    }

    return bestId;
}
}

// =============================
// Convert screen point to world-space ray
// =============================
Ray Picking::ScreenPointToRay(float nx, float ny, Camera* cam) {
    float x = nx * 2.0f - 1.0f;
    float y = 1.0f - ny * 2.0f;

    glm::vec4 rayClip(x, y, -1.0f, 1.0f);

    glm::mat4 invProj = glm::inverse(cam->GetProjectionMatrix());
    glm::vec4 rayEye = invProj * rayClip;
    rayEye.z = -1.0f;
    rayEye.w = 0.0f;

    glm::mat4 invView = glm::inverse(cam->GetViewMatrix());
    glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayEye));
    glm::vec3 origin = cam->GetPosition();

    return { origin, rayDir };
}

// =============================
// Pick entity at normalized coords
// =============================
int Picking::PickEntity(float nx, float ny, Scene& scene, Camera* cam) {
    Ray ray = ScreenPointToRay(nx, ny, cam);
    return PickEntityRay(ray, scene);
}

int Picking::ResolveSelectionEntity(int pickedEntity, Scene& scene, bool preferRootSelection) {
    if (pickedEntity == INVALID_ENTITY_ID || !preferRootSelection) {
        return pickedEntity;
    }

    EntityID selectionTarget = pickedEntity;
    EntityData* data = scene.GetEntityData(selectionTarget);
    while (data && data->Parent != INVALID_ENTITY_ID) {
        selectionTarget = data->Parent;
        data = scene.GetEntityData(selectionTarget);
    }

    return selectionTarget;
}

// =============================
// Core logic for ray picking
// =============================
int Picking::PickEntityRay(const Ray& ray, Scene& scene) {
    const EditorSettings& settings = EditorSettings::Get();
    std::vector<PickCandidate> outsideCandidates;
    std::vector<PickCandidate> insideCandidates;
    outsideCandidates.reserve(kMaxOutsidePickCandidates);
    insideCandidates.reserve(kMaxInsidePickCandidates);

    EntityID directBestId = INVALID_ENTITY_ID;
    float directBestT = FLT_MAX;
    float directBestDiag = FLT_MAX;

    for (const auto& entity : scene.GetEntities()) {
        auto* data = scene.GetEntityData(entity.GetID());
        if (!data || !data->Mesh) {
            continue;
        }
        if (settings.PickingSkipHidden && !data->Visible) {
            continue;
        }
        if (!data->Active) {
            continue;
        }

        std::shared_ptr<Mesh> meshRef = data->Mesh->mesh;
        if (!meshRef) {
            continue;
        }

        const glm::mat4 transform = data->Transform.WorldMatrix;
        const float diag = ApproximateWorldBoundsDiagonal(transform, *meshRef);

        if (!AreMeshBoundsValid(*meshRef)) {
            if (!meshRef->Vertices.empty() && meshRef->Indices.size() >= 3) {
                float directHitT = FLT_MAX;
                if (RayIntersectsMesh(ray, *meshRef, transform, directHitT) && directHitT > 0.0f) {
                    const float tieThreshold = (directBestId == INVALID_ENTITY_ID)
                        ? 0.0f
                        : glm::max(0.001f, directBestT * 0.001f);
                    if (directBestId == INVALID_ENTITY_ID ||
                        directHitT + tieThreshold < directBestT ||
                        (std::fabs(directHitT - directBestT) <= tieThreshold && diag < directBestDiag)) {
                        directBestId = entity.GetID();
                        directBestT = directHitT;
                        directBestDiag = diag;
                    }
                }
            }
            continue;
        }

        glm::vec3 localOrigin(0.0f);
        glm::vec3 localDirection(0.0f);
        if (!TransformRayToLocalSpace(ray, transform, localOrigin, localDirection)) {
            continue;
        }

        float entryT = FLT_MAX;
        float exitT = FLT_MAX;
        bool originInside = false;
        if (!RayIntersectsAABBSlab(localOrigin, localDirection, meshRef->BoundsMin, meshRef->BoundsMax, entryT, exitT, originInside)) {
            continue;
        }

        PickCandidate candidate;
        candidate.Id = entity.GetID();
        candidate.SortT = originInside ? exitT : entryT;
        candidate.FallbackT = originInside ? exitT : entryT;
        candidate.Diag = diag;
        candidate.Transform = transform;
        candidate.MeshRef = std::move(meshRef);

        if (originInside) {
            InsertCandidateLimited(insideCandidates, std::move(candidate), kMaxInsidePickCandidates);
        } else {
            InsertCandidateLimited(outsideCandidates, std::move(candidate), kMaxOutsidePickCandidates);
        }
    }

    EntityID bestId = EvaluatePickCandidates(ray, outsideCandidates);
    if (bestId != INVALID_ENTITY_ID) {
        return bestId;
    }

    bestId = EvaluatePickCandidates(ray, insideCandidates);
    if (bestId != INVALID_ENTITY_ID) {
        return bestId;
    }

    return directBestId;
}

// =============================
// Queuing system
// =============================
void Picking::QueuePick(float nx, float ny, bool preferRootSelection) {
    s_PickQueue.push_back({ nx, ny, preferRootSelection });
}

void Picking::QueueHoverPick(float nx, float ny) {
    s_HoverQueued = true;
    s_HoverRequest = { nx, ny };
}

void Picking::Process(Scene& scene, Camera* cam) {
    s_LastPick = -1;
    s_LastPickPreferRootSelection = false;
    s_ProcessedThisFrame = false;
    s_AnyHitThisFrame = false;

    // Prefer the renderer's object-id pass so skinned GPU morphs, outlines, and
    // picking all agree on the same per-instance deformed surface.
    EntityID gpuEntity = INVALID_ENTITY_ID;
    bool gpuHadHit = false;
    if (Renderer::Get().ConsumeObjectIdPickResult(gpuEntity, gpuHadHit)) {
        if (s_PendingPickKind == QueuedPickKind::Hover) {
            s_LastHoverPick = gpuHadHit ? gpuEntity : INVALID_ENTITY_ID;
        } else if (s_PendingPickKind == QueuedPickKind::Click) {
            s_ProcessedThisFrame = true;
            if (gpuHadHit) {
                s_LastPick = gpuEntity;
                s_LastPickPreferRootSelection = s_PendingPickPreferRootSelection;
                s_AnyHitThisFrame = true;
            }
        }
        s_PendingPickKind = QueuedPickKind::None;
        s_PendingPickPreferRootSelection = false;
        return;
    }

    if (s_PendingPickKind != QueuedPickKind::None) {
        return;
    }

    if (s_PickQueue.empty() && !s_HoverQueued) {
        return;
    }

    PickRequest request{};
    QueuedPickKind requestKind = QueuedPickKind::None;
    if (!s_PickQueue.empty()) {
        request = s_PickQueue.back();
        s_PickQueue.clear();
        requestKind = QueuedPickKind::Click;
    } else if (s_HoverQueued) {
        request = s_HoverRequest;
        s_HoverQueued = false;
        requestKind = QueuedPickKind::Hover;
    } else {
        return;
    }

    if (Renderer::Get().SupportsObjectIdPicking() &&
        Renderer::Get().RequestObjectIdPick(scene, request.nx, request.ny)) {
        s_PendingPickKind = requestKind;
        s_PendingPickPreferRootSelection = request.PreferRootSelection;
        return;
    }

    const int entity = PickEntity(request.nx, request.ny, scene, cam);
    if (requestKind == QueuedPickKind::Hover) {
        s_LastHoverPick = entity;
        return;
    }

    s_ProcessedThisFrame = true;
    if (entity != -1) {
        s_LastPick = entity;
        s_LastPickPreferRootSelection = request.PreferRootSelection;
        s_AnyHitThisFrame = true;
    }
}

int Picking::GetLastPick() {
    return s_LastPick;
}

int Picking::GetLastHoverPick() {
    return s_LastHoverPick;
}

bool Picking::LastPickPrefersRootSelection() {
    return s_LastPickPreferRootSelection;
}

bool Picking::HadPickThisFrame() { return s_ProcessedThisFrame; }
bool Picking::HadHitThisFrame() { return s_AnyHitThisFrame; }

bool Picking::RayIntersectsAABB(const Ray& ray, const glm::vec3& min, const glm::vec3& max, float& t) {
    float entryT = FLT_MAX;
    float exitT = FLT_MAX;
    bool originInside = false;
    if (!RayIntersectsAABBSlab(ray.Origin, ray.Direction, min, max, entryT, exitT, originInside)) {
        return false;
    }
    t = originInside ? exitT : entryT;
    return t > 0.0f;
}

bool Picking::RayIntersectsOBB(const Ray& ray, const glm::mat4& transform,
    const glm::vec3& min, const glm::vec3& max, float& t) {
    glm::vec3 localOrigin(0.0f);
    glm::vec3 localDirection(0.0f);
    if (!TransformRayToLocalSpace(ray, transform, localOrigin, localDirection)) {
        return false;
    }

    float entryT = FLT_MAX;
    float exitT = FLT_MAX;
    bool originInside = false;
    if (!RayIntersectsAABBSlab(localOrigin, localDirection, min, max, entryT, exitT, originInside)) {
        return false;
    }

    t = originInside ? exitT : entryT;
    return t > 0.0f;
}

bool Picking::RayIntersectsTriangle(const glm::vec3& origin, const glm::vec3& dir,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, float& t) {
    const float epsilon = glm::max(1e-12f, 1e-6f * std::sqrt(glm::dot(dir, dir)));
    glm::vec3 edge1 = v1 - v0;
    glm::vec3 edge2 = v2 - v0;

    glm::vec3 h = glm::cross(dir, edge2);
    float a = glm::dot(edge1, h);

    if (std::fabs(a) < epsilon) return false;
    float f = 1.0f / a;
    glm::vec3 s = origin - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;

    glm::vec3 q = glm::cross(s, edge1);
    float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;

    t = f * glm::dot(edge2, q);
    return t > epsilon;
}

bool Picking::RayIntersectsMesh(const Ray& ray, const Mesh& mesh, const glm::mat4& transform, float& closestT) {
    closestT = FLT_MAX;
    bool hit = false;

    glm::vec3 localOrigin(0.0f);
    glm::vec3 localDir(0.0f);
    if (!TransformRayToLocalSpace(ray, transform, localOrigin, localDir)) {
        return false;
    }

    if (AreMeshBoundsValid(mesh)) {
        float entryT = FLT_MAX;
        float exitT = FLT_MAX;
        bool originInside = false;
        if (!RayIntersectsAABBSlab(localOrigin, localDir, mesh.BoundsMin, mesh.BoundsMax, entryT, exitT, originInside)) {
            return false;
        }
    }

    if (mesh.Vertices.empty() || mesh.Indices.size() < 3) {
        return false;
    }

    for (size_t i = 0; i < mesh.Indices.size(); i += 3) {
        glm::vec3 v0 = mesh.Vertices[mesh.Indices[i]];
        glm::vec3 v1 = mesh.Vertices[mesh.Indices[i + 1]];
        glm::vec3 v2 = mesh.Vertices[mesh.Indices[i + 2]];

        float t;
        if (RayIntersectsTriangle(localOrigin, localDir, v0, v1, v2, t)) {
            if (t < closestT && t > 0.0f) {
                closestT = t;
                hit = true;
            }
        }
    }
    return hit;
}
