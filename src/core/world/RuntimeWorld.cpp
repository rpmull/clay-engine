#include "RuntimeWorld.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <intrin.h>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

#include "core/ecs/Scene.h"
#include "core/jobs/Jobs.h"
#include "core/jobs/ParallelFor.h"
#include "core/utils/PrefabPerfDiagnostics.h"
#include "core/utils/Profiler.h"

namespace cm::world {

namespace {

constexpr float kTransformEpsilon = 1e-5f;

uint64_t HashCombine64(uint64_t seed, uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

bool NearlyEqual(float a, float b, float epsilon = kTransformEpsilon) {
    return std::abs(a - b) <= epsilon;
}

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b, float epsilon = kTransformEpsilon) {
    return NearlyEqual(a.x, b.x, epsilon) &&
           NearlyEqual(a.y, b.y, epsilon) &&
           NearlyEqual(a.z, b.z, epsilon);
}

bool NearlyEqual(const glm::quat& a, const glm::quat& b, float epsilon = kTransformEpsilon) {
    return NearlyEqual(a.x, b.x, epsilon) &&
           NearlyEqual(a.y, b.y, epsilon) &&
           NearlyEqual(a.z, b.z, epsilon) &&
           NearlyEqual(a.w, b.w, epsilon);
}

bool NearlyEqual(const glm::mat4& a, const glm::mat4& b, float epsilon = kTransformEpsilon) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!NearlyEqual(a[column][row], b[column][row], epsilon)) {
                return false;
            }
        }
    }
    return true;
}

RuntimeLocalTransform ExtractLocalTransform(const TransformComponent& transform) {
    RuntimeLocalTransform local{};
    local.Position = transform.Position;
    local.EulerDegrees = transform.Rotation;
    local.Rotation = transform.RotationQ;
    local.Scale = transform.Scale;
    local.UseQuatRotation = transform.UseQuatRotation;
    return local;
}

glm::mat4 BuildLocalMatrix(const RuntimeLocalTransform& local) {
    const glm::mat4 translation = glm::translate(glm::mat4(1.0f), local.Position);
    const glm::mat4 rotation = local.UseQuatRotation
        ? glm::toMat4(glm::normalize(local.Rotation))
        : glm::yawPitchRoll(glm::radians(local.EulerDegrees.y),
                            glm::radians(local.EulerDegrees.x),
                            glm::radians(local.EulerDegrees.z));
    const glm::mat4 scale = glm::scale(glm::mat4(1.0f), local.Scale);
    return translation * rotation * scale;
}

RuntimeBounds ExtractLocalBounds(const EntityData& data) {
    RuntimeBounds bounds{};
    if (data.Mesh && data.Mesh->mesh) {
        bounds.Valid = true;
        bounds.LocalMin = data.Mesh->mesh->BoundsMin;
        bounds.LocalMax = data.Mesh->mesh->BoundsMax;
    }
    return bounds;
}

glm::vec3 LightDirectionFromWorldMatrix(const glm::mat4& worldMatrix) {
    glm::vec3 forward = glm::vec3(worldMatrix[2]);
    const float lengthSq = glm::dot(forward, forward);
    if (lengthSq <= 1e-8f) {
        return glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return glm::normalize(forward);
}

uint64_t DirtyBitsToStoreMask(RuntimeDirtyBits bits) {
    return static_cast<uint64_t>(static_cast<uint32_t>(bits));
}

void TransformBounds(const RuntimeBounds& localBounds,
                     const glm::mat4& worldMatrix,
                     float boundsPadding,
                     RuntimeBounds& outBounds) {
    if (!localBounds.Valid) {
        outBounds.Valid = false;
        return;
    }

    glm::vec3 localMin = localBounds.LocalMin;
    glm::vec3 localMax = localBounds.LocalMax;
    if (!NearlyEqual(boundsPadding, 1.0f)) {
        const glm::vec3 center = (localMin + localMax) * 0.5f;
        const glm::vec3 halfExtent = (localMax - localMin) * 0.5f * boundsPadding;
        localMin = center - halfExtent;
        localMax = center + halfExtent;
    }

    const glm::vec3 corners[8] = {
        { localMin.x, localMin.y, localMin.z },
        { localMax.x, localMin.y, localMin.z },
        { localMin.x, localMax.y, localMin.z },
        { localMax.x, localMax.y, localMin.z },
        { localMin.x, localMin.y, localMax.z },
        { localMax.x, localMin.y, localMax.z },
        { localMin.x, localMax.y, localMax.z },
        { localMax.x, localMax.y, localMax.z },
    };

    glm::vec3 worldMin(std::numeric_limits<float>::max());
    glm::vec3 worldMax(-std::numeric_limits<float>::max());
    for (const glm::vec3& corner : corners) {
        const glm::vec3 worldCorner = glm::vec3(worldMatrix * glm::vec4(corner, 1.0f));
        worldMin = glm::min(worldMin, worldCorner);
        worldMax = glm::max(worldMax, worldCorner);
    }

    outBounds = localBounds;
    outBounds.Valid = true;
    outBounds.WorldMin = worldMin;
    outBounds.WorldMax = worldMax;
}

EntityID ResolveRuntimeSkinningSkeletonFromHierarchy(Scene& scene, EntityID startEntity) {
    EntityID current = startEntity;
    while (current != INVALID_ENTITY_ID) {
        EntityData* data = scene.GetEntityData(current);
        if (!data) {
            break;
        }

        if (data->Skeleton && !data->Skeleton->BoneEntities.empty()) {
            return current;
        }

        current = data->Parent;
    }

    return INVALID_ENTITY_ID;
}

EntityID ResolveRuntimeSkinningSkeletonRoot(Scene& scene, EntityID meshEntity, SkinningComponent* skinning) {
    if (!skinning) {
        return INVALID_ENTITY_ID;
    }

    if (skinning->ResolvedSkeletonRoot != INVALID_ENTITY_ID &&
        skinning->ResolvedSkeletonRoot != static_cast<EntityID>(-1)) {
        EntityData* resolvedData = scene.GetEntityData(skinning->ResolvedSkeletonRoot);
        if (resolvedData && resolvedData->Skeleton) {
            return skinning->ResolvedSkeletonRoot;
        }
        skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
    }

    if (skinning->UseParentSkeleton) {
        EntityData* meshData = scene.GetEntityData(meshEntity);
        if (meshData && meshData->Parent != INVALID_ENTITY_ID) {
            EntityID resolved = ResolveRuntimeSkinningSkeletonFromHierarchy(scene, meshData->Parent);
            if (resolved != INVALID_ENTITY_ID) {
                skinning->ResolvedSkeletonRoot = resolved;
                return resolved;
            }
        }
    }

    if (skinning->SkeletonRoot != INVALID_ENTITY_ID && skinning->SkeletonRoot != static_cast<EntityID>(-1)) {
        EntityData* skeletonData = scene.GetEntityData(skinning->SkeletonRoot);
        if (skeletonData && skeletonData->Skeleton) {
            skinning->ResolvedSkeletonRoot = skinning->SkeletonRoot;
            return skinning->SkeletonRoot;
        }
        skinning->SkeletonRoot = static_cast<EntityID>(-1);
        skinning->ResolvedSkeletonRoot = INVALID_ENTITY_ID;
        skinning->InvalidateRemap();
    }

    EntityData* meshData = scene.GetEntityData(meshEntity);
    if (meshData && meshData->Parent != INVALID_ENTITY_ID) {
        EntityID resolved = ResolveRuntimeSkinningSkeletonFromHierarchy(scene, meshData->Parent);
        if (resolved != INVALID_ENTITY_ID) {
            skinning->SkeletonRoot = resolved;
            skinning->ResolvedSkeletonRoot = resolved;
            return resolved;
        }
    }

    return INVALID_ENTITY_ID;
}

} // namespace

void RuntimeCommandBuffer::Clear() {
    m_Commands.clear();
}

void RuntimeCommandBuffer::Destroy(RuntimeEntityHandle handle) {
    Command command{};
    command.Type = CommandType::Destroy;
    command.Handle = handle;
    m_Commands.push_back(command);
}

void RuntimeCommandBuffer::SetActive(RuntimeEntityHandle handle, bool active) {
    Command command{};
    command.Type = CommandType::SetActive;
    command.Handle = handle;
    command.BoolValue = active;
    m_Commands.push_back(command);
}

void RuntimeCommandBuffer::SetVisibility(RuntimeEntityHandle handle, bool visible) {
    Command command{};
    command.Type = CommandType::SetVisibility;
    command.Handle = handle;
    command.BoolValue = visible;
    m_Commands.push_back(command);
}

void RuntimeCommandBuffer::SetPresentationHidden(RuntimeEntityHandle handle, bool hidden) {
    Command command{};
    command.Type = CommandType::SetPresentationHidden;
    command.Handle = handle;
    command.BoolValue = hidden;
    m_Commands.push_back(command);
}

void RuntimeCommandBuffer::SetLocalTransform(RuntimeEntityHandle handle, const RuntimeLocalTransform& transform) {
    Command command{};
    command.Type = CommandType::SetLocalTransform;
    command.Handle = handle;
    command.Transform = transform;
    m_Commands.push_back(command);
}

void RuntimeTaskGraph::Clear() {
    m_Tasks.clear();
    m_LastStats.clear();
    m_LastBarrierCount = 0;
}

void RuntimeTaskGraph::Add(TaskDecl decl) {
    m_Tasks.push_back(std::move(decl));
}

void RuntimeTaskGraph::Execute() {
    m_LastStats.clear();
    m_LastBarrierCount = 0;
    if (m_Tasks.empty()) {
        return;
    }

    const size_t taskCount = m_Tasks.size();
    auto runTaskDirect = [this](size_t taskIndex) {
        const TaskDecl& task = m_Tasks[taskIndex];
        RuntimeTaskStats stats{};
        stats.Name = task.Name;
        stats.Category = task.Category;
        stats.DependencyCount = static_cast<uint32_t>(task.After.size());
        stats.Parallelized = false;
        stats.MainThreadOnly = task.MainThreadOnly;

        const auto start = std::chrono::high_resolution_clock::now();
        if (task.Callback) {
            task.Callback();
        }
        const auto end = std::chrono::high_resolution_clock::now();
        stats.DurationMs = std::chrono::duration<double, std::milli>(end - start).count();
        m_LastStats.push_back(std::move(stats));
    };

    if (taskCount == 1) {
        runTaskDirect(0);
        return;
    }

    if (taskCount == 2) {
        const bool task0AfterTask1 =
            std::find(m_Tasks[0].After.begin(), m_Tasks[0].After.end(), m_Tasks[1].Name) != m_Tasks[0].After.end();
        const bool task1AfterTask0 =
            std::find(m_Tasks[1].After.begin(), m_Tasks[1].After.end(), m_Tasks[0].Name) != m_Tasks[1].After.end();
        const uint64_t hazard =
            (m_Tasks[0].WriteMask & (m_Tasks[1].ReadMask | m_Tasks[1].WriteMask)) |
            (m_Tasks[1].WriteMask & m_Tasks[0].ReadMask);

        if (task0AfterTask1 && !task1AfterTask0 && hazard == 0u) {
            runTaskDirect(1);
            ++m_LastBarrierCount;
            runTaskDirect(0);
            return;
        }
        if (!task0AfterTask1) {
            runTaskDirect(0);
            if (task1AfterTask0 || hazard != 0u) {
                ++m_LastBarrierCount;
            }
            runTaskDirect(1);
            return;
        }
    }

    std::vector<std::vector<size_t>> edges(taskCount);
    std::vector<size_t> indegree(taskCount, 0);
    std::unordered_map<std::string, size_t> nameToIndex;
    nameToIndex.reserve(taskCount);

    for (size_t i = 0; i < taskCount; ++i) {
        nameToIndex.emplace(m_Tasks[i].Name, i);
    }

    for (size_t i = 0; i < taskCount; ++i) {
        for (const std::string& depName : m_Tasks[i].After) {
            const auto it = nameToIndex.find(depName);
            if (it == nameToIndex.end()) {
                continue;
            }
            edges[it->second].push_back(i);
            ++indegree[i];
        }
    }

    for (size_t i = 0; i < taskCount; ++i) {
        for (size_t j = i + 1; j < taskCount; ++j) {
            const uint64_t hazard =
                (m_Tasks[i].WriteMask & (m_Tasks[j].ReadMask | m_Tasks[j].WriteMask)) |
                (m_Tasks[j].WriteMask & m_Tasks[i].ReadMask);
            if (hazard == 0) {
                continue;
            }
            edges[i].push_back(j);
            ++indegree[j];
        }
    }

    std::vector<uint8_t> completed(taskCount, 0);
    size_t completedCount = 0;
    bool firstStage = true;

    while (completedCount < taskCount) {
        std::vector<size_t> stage;
        for (size_t i = 0; i < taskCount; ++i) {
            if (!completed[i] && indegree[i] == 0) {
                stage.push_back(i);
            }
        }

        if (stage.empty()) {
            for (size_t i = 0; i < taskCount; ++i) {
                if (!completed[i]) {
                    stage.push_back(i);
                    break;
                }
            }
        }

        if (!firstStage) {
            ++m_LastBarrierCount;
        }
        firstStage = false;

        std::vector<RuntimeTaskStats> stageStats(stage.size());
        const bool canParallelize =
            (stage.size() > 1) &&
            (cm::g_JobSystem != nullptr) &&
            std::all_of(stage.begin(), stage.end(), [&](size_t taskIndex) {
                const TaskDecl& task = m_Tasks[taskIndex];
                return task.AllowParallel && !task.MainThreadOnly;
            });

        auto runTask = [&](size_t stageIndex) {
            const size_t taskIndex = stage[stageIndex];
            const TaskDecl& task = m_Tasks[taskIndex];
            RuntimeTaskStats stats{};
            stats.Name = task.Name;
            stats.Category = task.Category;
            stats.DependencyCount = static_cast<uint32_t>(task.After.size());
            stats.Parallelized = canParallelize;
            stats.MainThreadOnly = task.MainThreadOnly;

            const auto start = std::chrono::high_resolution_clock::now();
            if (task.Callback) {
                task.Callback();
            }
            const auto end = std::chrono::high_resolution_clock::now();
            stats.DurationMs = std::chrono::duration<double, std::milli>(end - start).count();
            stageStats[stageIndex] = std::move(stats);
        };

        if (canParallelize) {
            const size_t stageChunk =
                ComputeOptimalChunkSize(stage.size(), Jobs().GetWorkerCount(), size_t{ 2 });
            parallel_for(Jobs(), size_t{ 0 }, stage.size(), stageChunk,
                [&](size_t begin, size_t count) {
                    for (size_t i = begin; i < begin + count; ++i) {
                        runTask(i);
                    }
                },
                JobSystem::Priority::High);
        } else {
            for (size_t i = 0; i < stage.size(); ++i) {
                runTask(i);
            }
        }

        for (size_t i = 0; i < stage.size(); ++i) {
            const size_t taskIndex = stage[i];
            completed[taskIndex] = 1;
            ++completedCount;
            for (size_t next : edges[taskIndex]) {
                if (indegree[next] > 0) {
                    --indegree[next];
                }
            }
            m_LastStats.push_back(stageStats[i]);
        }
    }
}

RuntimeWorld::RuntimeWorld() {
    UpdateStats();
}

RuntimeWorld::RuntimeWorld(RuntimeWorld&& other) noexcept = default;
RuntimeWorld& RuntimeWorld::operator=(RuntimeWorld&& other) noexcept = default;

void RuntimeWorld::Reset() {
    m_Entities.clear();
    m_LocalTransforms.clear();
    m_LocalMatrices.clear();
    m_WorldMatrices.clear();
    m_Bounds.clear();
    m_Lights.clear();
    m_TransformDirty.clear();
    m_TransformStageQueued.clear();
    m_RecomputedThisPass.clear();
    m_ExternalWorldDirty.clear();
    m_TransformStageCandidates.clear();
    m_ExternalWorldDirtyIndices.clear();
    m_SceneToIndex.clear();
    m_FreeIndices.clear();
    m_SceneOrder.clear();
    m_LevelOrder.clear();
    m_LevelOffsets.clear();
    m_ChildIndices.clear();
    m_ChildOffsets.clear();
    m_ChildCounts.clear();
    m_RenderableIndices.clear();
    m_TerrainIndices.clear();
    m_LightIndices.clear();
    m_CameraIndices.clear();
    m_AudioSourceIndices.clear();
    m_AudioListenerIndices.clear();
    m_EmitterIndices.clear();
    m_ScriptedIndices.clear();
    m_AnimationRootIndices.clear();
    m_SkinnedMeshIndices.clear();
    m_PhysicsIndices.clear();
    m_RenderableSceneEntities.clear();
    m_TerrainSceneEntities.clear();
    m_LightSceneEntities.clear();
    m_CameraSceneEntities.clear();
    m_AudioSourceSceneEntities.clear();
    m_AudioListenerSceneEntities.clear();
    m_EmitterSceneEntities.clear();
    m_ScriptedSceneEntities.clear();
    m_AnimationRootSceneEntities.clear();
    m_SkinnedMeshSceneEntities.clear();
    m_PhysicsSceneEntities.clear();
    m_SkinningGroupCaches.clear();
    m_SkinningGroupCacheLookup.clear();
    m_RenderWorld = {};
    m_TaskGraph.Clear();
    m_GlobalVersion = 1;
    m_LastRenderableListFingerprint = 0;
    m_LastTerrainListFingerprint = 0;
    m_LastLightListFingerprint = 0;
    m_AliveEntityCount = 0;
    m_ActiveEntityCount = 0;
    m_DirtyEntityCount = 0;
    m_HierarchyDirty = true;
    m_PersistentIndicesDirty = true;
    m_SkinningCachesDirty = true;
    m_Stats = {};
    UpdateStats();
}

void RuntimeWorld::InvalidateAll() {
    for (Index index = 0; index < m_Entities.size(); ++index) {
        if (!IsIndexAlive(index)) {
            continue;
        }
        MarkDirty(index, RuntimeDirtyBits::All);
        m_TransformDirty[index] = 1u;
    }
    m_HierarchyDirty = true;
    m_PersistentIndicesDirty = true;
    m_SkinningCachesDirty = true;
}

void RuntimeWorld::InvalidateEntity(EntityID sceneEntity, RuntimeDirtyBits bits) {
    const auto it = m_SceneToIndex.find(sceneEntity);
    if (it == m_SceneToIndex.end()) {
        return;
    }
    MarkDirty(it->second, bits);
}

RuntimeWorld::Index RuntimeWorld::AcquireSlot(EntityID sceneEntity) {
    Index index = InvalidIndex;
    uint32_t generation = 1;

    if (!m_FreeIndices.empty()) {
        index = m_FreeIndices.back();
        m_FreeIndices.pop_back();
        generation = std::max(1u, m_Entities[index].Generation);
    } else {
        index = static_cast<Index>(m_Entities.size());
        EnsureStorageSize(static_cast<size_t>(index) + 1);
    }

    m_Entities[index] = {};
    m_Entities[index].SceneEntity = sceneEntity;
    m_Entities[index].Generation = generation;
    m_Entities[index].Alive = true;
    m_Entities[index].DirtyMask = RuntimeDirtyBits::All;
    m_Entities[index].DirtyVersion = ++m_GlobalVersion;
    m_LocalTransforms[index] = {};
    m_LocalMatrices[index] = glm::mat4(1.0f);
    m_WorldMatrices[index] = glm::mat4(1.0f);
    m_Bounds[index] = {};
    m_Lights[index] = {};
    m_TransformDirty[index] = 1u;
    m_RecomputedThisPass[index] = 0u;
    m_ExternalWorldDirty[index] = 0u;
    m_TransformStageQueued[index] = 0u;
    m_SceneToIndex[sceneEntity] = index;
    ++m_AliveEntityCount;
    ++m_ActiveEntityCount;
    ++m_DirtyEntityCount;
    QueueTransformStageCandidate(index);
    return index;
}

void RuntimeWorld::ReleaseSlot(Index index) {
    if (!IsIndexAlive(index)) {
        return;
    }

    const EntitySlot& slot = m_Entities[index];
    if (slot.Active && m_ActiveEntityCount > 0) {
        --m_ActiveEntityCount;
    }
    if (slot.DirtyMask != RuntimeDirtyBits::None && m_DirtyEntityCount > 0) {
        --m_DirtyEntityCount;
    }
    if (m_AliveEntityCount > 0) {
        --m_AliveEntityCount;
    }

    m_SceneToIndex.erase(m_Entities[index].SceneEntity);
    m_Entities[index].SceneEntity = INVALID_ENTITY_ID;
    m_Entities[index].Alive = false;
    m_Entities[index].Parent = InvalidIndex;
    m_Entities[index].Generation = std::max(1u, m_Entities[index].Generation + 1u);
    m_Entities[index].Name.clear();
    m_Entities[index].Tag.clear();
    m_Entities[index].DirtyMask = RuntimeDirtyBits::All;
    m_LocalTransforms[index] = {};
    m_LocalMatrices[index] = glm::mat4(1.0f);
    m_WorldMatrices[index] = glm::mat4(1.0f);
    m_Bounds[index] = {};
    m_Lights[index] = {};
    m_TransformDirty[index] = 0u;
    m_RecomputedThisPass[index] = 0u;
    m_ExternalWorldDirty[index] = 0u;
    m_TransformStageQueued[index] = 0u;
    m_FreeIndices.push_back(index);
}

void RuntimeWorld::EnsureStorageSize(size_t size) {
    if (m_Entities.size() >= size) {
        return;
    }

    m_Entities.resize(size);
    m_LocalTransforms.resize(size);
    m_LocalMatrices.resize(size, glm::mat4(1.0f));
    m_WorldMatrices.resize(size, glm::mat4(1.0f));
    m_Bounds.resize(size);
    m_Lights.resize(size);
    m_TransformDirty.resize(size, 0u);
    m_TransformStageQueued.resize(size, 0u);
    m_RecomputedThisPass.resize(size, 0u);
    m_ExternalWorldDirty.resize(size, 0u);
}

bool RuntimeWorld::IsIndexAlive(Index index) const {
    return index < m_Entities.size() && m_Entities[index].Alive;
}

bool RuntimeWorld::IsTransformStageDirty(Index index) const {
    return IsIndexAlive(index) &&
        ((m_TransformDirty[index] != 0u) ||
         (m_ExternalWorldDirty[index] != 0u) ||
         HasAny(m_Entities[index].DirtyMask, RuntimeDirtyBits::Hierarchy));
}

void RuntimeWorld::QueueTransformStageCandidate(Index index) {
    if (!IsIndexAlive(index) || index >= m_TransformStageQueued.size() || m_TransformStageQueued[index] != 0u) {
        return;
    }

    m_TransformStageQueued[index] = 1u;
    m_TransformStageCandidates.push_back(index);
}

void RuntimeWorld::CompactTransformStageCandidates() {
    size_t writeIndex = 0;
    for (Index index : m_TransformStageCandidates) {
        if (!IsTransformStageDirty(index)) {
            if (index < m_TransformStageQueued.size()) {
                m_TransformStageQueued[index] = 0u;
            }
            continue;
        }

        m_TransformStageCandidates[writeIndex++] = index;
    }

    m_TransformStageCandidates.resize(writeIndex);
}

void RuntimeWorld::MarkExternalWorldDirty(Index index) {
    if (!IsIndexAlive(index) || m_ExternalWorldDirty[index] != 0u) {
        return;
    }

    m_ExternalWorldDirty[index] = 1u;
    m_ExternalWorldDirtyIndices.push_back(index);
    QueueTransformStageCandidate(index);
}

bool RuntimeWorld::IsHandleAlive(RuntimeEntityHandle handle) const {
    return IsIndexAlive(handle.Index) && m_Entities[handle.Index].Generation == handle.Generation;
}

RuntimeEntityHandle RuntimeWorld::TryGetHandle(EntityID sceneEntity) const {
    const auto it = m_SceneToIndex.find(sceneEntity);
    if (it == m_SceneToIndex.end()) {
        return {};
    }

    RuntimeEntityHandle handle{};
    handle.Index = it->second;
    handle.Generation = m_Entities[it->second].Generation;
    return handle;
}

EntityID RuntimeWorld::ResolveSceneEntity(RuntimeEntityHandle handle) const {
    if (!IsHandleAlive(handle)) {
        return INVALID_ENTITY_ID;
    }
    return m_Entities[handle.Index].SceneEntity;
}

const glm::mat4* RuntimeWorld::TryGetWorldMatrix(RuntimeEntityHandle handle) const {
    if (!IsHandleAlive(handle)) {
        return nullptr;
    }
    return &m_WorldMatrices[handle.Index];
}

const glm::mat4* RuntimeWorld::TryGetWorldMatrix(EntityID sceneEntity) const {
    const auto it = m_SceneToIndex.find(sceneEntity);
    if (it == m_SceneToIndex.end() || !IsIndexAlive(it->second)) {
        return nullptr;
    }
    return &m_WorldMatrices[it->second];
}

const RuntimeBounds* RuntimeWorld::TryGetBounds(RuntimeEntityHandle handle) const {
    if (!IsHandleAlive(handle)) {
        return nullptr;
    }
    return &m_Bounds[handle.Index];
}

const RuntimeSkinningGroupCache* RuntimeWorld::TryGetSkinningGroupCache(EntityID skeletonSceneEntity) const {
    const auto it = m_SkinningGroupCacheLookup.find(skeletonSceneEntity);
    if (it == m_SkinningGroupCacheLookup.end() || it->second >= m_SkinningGroupCaches.size()) {
        return nullptr;
    }
    return &m_SkinningGroupCaches[it->second];
}

bool RuntimeWorld::TryGetSkinningGroupWorldBounds(const RuntimeSkinningGroupCache& cache,
                                                  glm::vec3& outMin,
                                                  glm::vec3& outMax) const {
    glm::vec3 minBounds(std::numeric_limits<float>::max());
    glm::vec3 maxBounds(-std::numeric_limits<float>::max());
    bool hasBounds = false;

    for (RuntimeEntityHandle meshHandle : cache.MeshHandles) {
        const RuntimeBounds* bounds = TryGetBounds(meshHandle);
        if (!bounds || !bounds->Valid) {
            continue;
        }

        minBounds = glm::min(minBounds, bounds->WorldMin);
        maxBounds = glm::max(maxBounds, bounds->WorldMax);
        hasBounds = true;
    }

    if (!hasBounds) {
        return false;
    }

    outMin = minBounds;
    outMax = maxBounds;
    return true;
}

bool RuntimeWorld::TryGetSkinningGroupMaxWorldExtent(const RuntimeSkinningGroupCache& cache,
                                                     glm::vec3& outMaxExtent) const {
    glm::vec3 maxExtent(0.0f);
    bool hasExtent = false;

    for (RuntimeEntityHandle meshHandle : cache.MeshHandles) {
        const RuntimeBounds* bounds = TryGetBounds(meshHandle);
        if (!bounds || !bounds->Valid) {
            continue;
        }

        maxExtent = glm::max(maxExtent, 0.5f * (bounds->WorldMax - bounds->WorldMin));
        hasExtent = true;
    }

    if (!hasExtent) {
        return false;
    }

    outMaxExtent = maxExtent;
    return true;
}

void RuntimeWorld::ApplyWorldTransformOverride(EntityID sceneEntity, const glm::mat4& worldMatrix) {
    const auto it = m_SceneToIndex.find(sceneEntity);
    if (it == m_SceneToIndex.end() || !IsIndexAlive(it->second)) {
        return;
    }

    const Index index = it->second;
    if (!NearlyEqual(m_WorldMatrices[index], worldMatrix)) {
        m_WorldMatrices[index] = worldMatrix;
    }
    MarkDirty(index, RuntimeDirtyBits::TransformWorld | RuntimeDirtyBits::Bounds);
}

void RuntimeWorld::MarkDirty(Index index, RuntimeDirtyBits bits) {
    if (!IsIndexAlive(index)) {
        return;
    }

    EntitySlot& slot = m_Entities[index];
    const RuntimeDirtyBits previousMask = slot.DirtyMask;
    slot.DirtyMask |= bits;
    slot.DirtyVersion = ++m_GlobalVersion;
    if (previousMask == RuntimeDirtyBits::None && slot.DirtyMask != RuntimeDirtyBits::None) {
        ++m_DirtyEntityCount;
    }

    if (HasAny(bits, RuntimeDirtyBits::Metadata)) {
        m_Stats.MetadataVersion = m_GlobalVersion;
    }
    if (HasAny(bits, RuntimeDirtyBits::TransformLocal)) {
        m_Stats.TransformVersion = m_GlobalVersion;
        m_TransformDirty[index] = 1u;
        QueueTransformStageCandidate(index);
        if (slot.HasLight) {
            m_Stats.LightVersion = m_GlobalVersion;
        }
    }
    if (HasAny(bits, RuntimeDirtyBits::TransformWorld)) {
        m_Stats.TransformVersion = m_GlobalVersion;
        m_Stats.BoundsVersion = m_GlobalVersion;
        MarkExternalWorldDirty(index);
        if (slot.HasLight) {
            m_Stats.LightVersion = m_GlobalVersion;
        }
    }
    if (HasAny(bits, RuntimeDirtyBits::Hierarchy | RuntimeDirtyBits::Lifetime)) {
        m_Stats.HierarchyVersion = m_GlobalVersion;
        m_HierarchyDirty = true;
        m_SkinningCachesDirty = true;
        QueueTransformStageCandidate(index);
    }
    if (HasAny(bits, RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Visibility | RuntimeDirtyBits::Lifetime)) {
        m_Stats.RenderBindingVersion = m_GlobalVersion;
    }
    if (HasAny(bits, RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Lifetime)) {
        m_PersistentIndicesDirty = true;
        m_SkinningCachesDirty = true;
    }
    if (HasAny(bits, RuntimeDirtyBits::Light | RuntimeDirtyBits::Lifetime)) {
        m_Stats.LightVersion = m_GlobalVersion;
    }
    if (HasAny(bits, RuntimeDirtyBits::Bounds | RuntimeDirtyBits::TransformLocal | RuntimeDirtyBits::RenderBinding)) {
        m_Stats.BoundsVersion = m_GlobalVersion;
    }
}

void RuntimeWorld::ClearDirty(Index index, RuntimeDirtyBits bits) {
    if (!IsIndexAlive(index)) {
        return;
    }

    EntitySlot& slot = m_Entities[index];
    const RuntimeDirtyBits previousMask = slot.DirtyMask;
    slot.DirtyMask &= ~bits;
    if (previousMask != RuntimeDirtyBits::None && slot.DirtyMask == RuntimeDirtyBits::None && m_DirtyEntityCount > 0) {
        --m_DirtyEntityCount;
    }
    if (!HasAny(slot.DirtyMask, RuntimeDirtyBits::TransformLocal)) {
        m_TransformDirty[index] = 0u;
    }
}

void RuntimeWorld::SyncFromScene(Scene& scene, RuntimeSyncReason reason, bool forceFullRebuild) {
    ScopedTimer syncTimer("RuntimeWorld/SyncFromScene");
    auto& profiler = Profiler::Get();
    profiler.AddCounter("RuntimeWorld/SyncCalls", 1);
    switch (reason) {
    case RuntimeSyncReason::SceneUpdate:
        profiler.AddCounter("RuntimeWorld/SyncReasonSceneUpdate", 1);
        break;
    case RuntimeSyncReason::Render:
        profiler.AddCounter("RuntimeWorld/SyncReasonRender", 1);
        break;
    case RuntimeSyncReason::Bootstrap:
        profiler.AddCounter("RuntimeWorld/SyncReasonBootstrap", 1);
        break;
    case RuntimeSyncReason::FullRebuild:
        profiler.AddCounter("RuntimeWorld/SyncReasonFullRebuild", 1);
        break;
    }
    const bool hardReset = forceFullRebuild || reason == RuntimeSyncReason::FullRebuild;
    const bool fullSyncRequested = hardReset || scene.m_RuntimeWorldBridgeFullSyncPending || m_SceneOrder.empty();

    std::vector<EntityID> queuedTransformEntities;
    if (scene.m_RuntimeWorldPendingTransformBitset) {
        queuedTransformEntities.reserve(64u);
        for (size_t wordIndex = 0; wordIndex < scene.m_RuntimeWorldPendingTransformBitWordCount; ++wordIndex) {
            uint64_t bits =
                scene.m_RuntimeWorldPendingTransformBitset[wordIndex].exchange(0u, std::memory_order_acq_rel);
            while (bits != 0u) {
                unsigned long bitIndex = 0;
                _BitScanForward64(&bitIndex, bits);
                const size_t entityIndex = (wordIndex << 6u) + static_cast<size_t>(bitIndex);
                if (entityIndex < static_cast<size_t>(scene.m_NextID)) {
                    queuedTransformEntities.push_back(static_cast<EntityID>(entityIndex));
                }
                bits &= (bits - 1u);
            }
        }
    }

    const bool collectDetailedPrefabPerf = cm::debug::PrefabPerfDetailedTimingsEnabled();
    std::unordered_set<EntityID> dirtyPrefabEntityIds;
    std::unordered_map<EntityID, uint64_t> dirtyPrefabRoots;
    if (collectDetailedPrefabPerf) {
        dirtyPrefabEntityIds.reserve(
            scene.m_RuntimeWorldBridgeDirtyEntities.size() +
            scene.m_RuntimeWorldBridgeRemovedEntities.size() +
            queuedTransformEntities.size());
    }
    auto noteDirtyPrefabEntity = [&](EntityID sceneEntity) {
        if (!collectDetailedPrefabPerf || sceneEntity == INVALID_ENTITY_ID) {
            return;
        }
        if (!dirtyPrefabEntityIds.insert(sceneEntity).second) {
            return;
        }
        const EntityID prefabRootId = cm::debug::ResolveOwningPrefabRoot(scene, sceneEntity);
        if (prefabRootId != INVALID_ENTITY_ID) {
            ++dirtyPrefabRoots[prefabRootId];
        }
    };
    for (EntityID sceneEntity : scene.m_RuntimeWorldBridgeDirtyEntities) {
        noteDirtyPrefabEntity(sceneEntity);
    }
    for (EntityID sceneEntity : scene.m_RuntimeWorldBridgeRemovedEntities) {
        noteDirtyPrefabEntity(sceneEntity);
    }
    for (EntityID sceneEntity : queuedTransformEntities) {
        noteDirtyPrefabEntity(sceneEntity);
    }
    if (collectDetailedPrefabPerf && fullSyncRequested) {
        for (const Entity& entity : scene.GetEntities()) {
            noteDirtyPrefabEntity(entity.GetID());
        }
    }

    profiler.SetCounter("RuntimeWorld/QueuedTransformEntities", static_cast<uint64_t>(queuedTransformEntities.size()));
    profiler.SetCounter("RuntimeWorld/ExplicitDirtyEntities", static_cast<uint64_t>(scene.m_RuntimeWorldBridgeDirtyEntities.size()));
    profiler.SetCounter("RuntimeWorld/RemovedEntities", static_cast<uint64_t>(scene.m_RuntimeWorldBridgeRemovedEntities.size()));
    profiler.SetCounter("RuntimeWorld/FullSyncRequested", fullSyncRequested ? 1u : 0u);
    profiler.SetCounter("RuntimeWorld/DirtyPrefabEntities", static_cast<uint64_t>(dirtyPrefabEntityIds.size()));
    profiler.SetCounter("RuntimeWorld/DirtyPrefabRoots", static_cast<uint64_t>(dirtyPrefabRoots.size()));

    if (collectDetailedPrefabPerf &&
        cm::debug::PrefabPerfConsoleLoggingEnabled() &&
        reason == RuntimeSyncReason::SceneUpdate) {
        static uint64_t s_RuntimeWorldPrefabLogTick = 0;
        const uint32_t logInterval = cm::debug::PrefabPerfConsoleLogInterval();
        ++s_RuntimeWorldPrefabLogTick;
        if (logInterval > 0 &&
            (s_RuntimeWorldPrefabLogTick % logInterval) == 0u &&
            !dirtyPrefabRoots.empty()) {
            std::vector<std::pair<EntityID, uint64_t>> ordered(dirtyPrefabRoots.begin(), dirtyPrefabRoots.end());
            std::sort(ordered.begin(), ordered.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second > rhs.second;
                });

            const size_t limit = std::min<size_t>(3, ordered.size());
            std::cout << "[PrefabPerf][RuntimeWorld] Top dirty prefab roots before sync:" << std::endl;
            for (size_t i = 0; i < limit; ++i) {
                const auto label = cm::debug::DescribePrefabRoot(scene, ordered[i].first);
                std::cout << "[PrefabPerf][RuntimeWorld]   " << (i + 1) << ". "
                    << cm::debug::MakePrefabDebugLabel(label)
                    << " dirtyEntities=" << ordered[i].second
                    << std::endl;
            }
        }
    }

    if (hardReset) {
        ++m_Stats.FullRebuildCount;
        Reset();
    }

    if (fullSyncRequested) {
        ++m_Stats.IncrementalSyncCount;
        bool structuralChange = hardReset;
        std::unordered_set<EntityID> seenSceneEntities;
        const auto& sceneEntities = scene.GetEntities();
        seenSceneEntities.reserve(sceneEntities.size());

        for (const Entity& entity : sceneEntities) {
            const EntityID sceneEntity = entity.GetID();
            seenSceneEntities.insert(sceneEntity);
            SyncEntityFromScene(scene, sceneEntity, structuralChange);
        }

        for (Index index = 0; index < m_Entities.size(); ++index) {
            if (!IsIndexAlive(index)) {
                continue;
            }
            if (seenSceneEntities.find(m_Entities[index].SceneEntity) == seenSceneEntities.end()) {
                ReleaseSlot(index);
                structuralChange = true;
            }
        }

        RebuildSceneOrder(scene);
        RefreshParentLinks(scene);
        if (structuralChange || m_HierarchyDirty) {
            RebuildHierarchy();
        }
        if (m_PersistentIndicesDirty) {
            RebuildPersistentIndices();
        }
        if (m_SkinningCachesDirty) {
            RebuildSkinningCaches(scene);
            m_SkinningCachesDirty = false;
        }

        scene.ResetRuntimeWorldDirtyTracking();
        scene.m_RuntimeWorldBridgeRemovedEntities.clear();
        scene.m_RuntimeWorldBridgeFullSyncPending = false;
        scene.ResetRuntimeWorldTransformTracking();
        m_Stats.SceneSyncVersion = ++m_GlobalVersion;
        UpdateStats();
        return;
    }

    bool useImplicitTransformScan = false;
    if (scene.m_RuntimeWorldBridgeDirtyEntities.empty() && scene.m_RuntimeWorldBridgeRemovedEntities.empty()) {
        if (queuedTransformEntities.empty() && !useImplicitTransformScan) {
            bool hasImplicitTransformDirty = false;
            for (const auto& [sceneEntity, data] : scene.m_Entities) {
                (void)sceneEntity;
                if (data.Transform.TransformDirty) {
                    hasImplicitTransformDirty = true;
                    break;
                }
            }

            if (!hasImplicitTransformDirty) {
                UpdateStats();
                return;
            }

            useImplicitTransformScan = true;
        }
    }

    {
        ++m_Stats.IncrementalSyncCount;
    }

    bool structuralChange = false;
    std::unordered_set<EntityID> removedEntities;
    removedEntities.reserve(scene.m_RuntimeWorldBridgeRemovedEntities.size());
    for (EntityID sceneEntity : scene.m_RuntimeWorldBridgeRemovedEntities) {
        if (!removedEntities.insert(sceneEntity).second) {
            continue;
        }
        const auto existing = m_SceneToIndex.find(sceneEntity);
        if (existing == m_SceneToIndex.end()) {
            continue;
        }
        ReleaseSlot(existing->second);
        structuralChange = true;
    }

    for (EntityID sceneEntity : scene.m_RuntimeWorldBridgeDirtyEntities) {
        if (sceneEntity == INVALID_ENTITY_ID ||
            static_cast<size_t>(sceneEntity) >= scene.m_RuntimeWorldBridgeDirtyMasks.size()) {
            continue;
        }
        const uint32_t dirtyMask = scene.m_RuntimeWorldBridgeDirtyMasks[sceneEntity];
        if (dirtyMask == 0u) {
            continue;
        }
        if (removedEntities.find(sceneEntity) != removedEntities.end()) {
            continue;
        }
        if (HasAny(static_cast<RuntimeDirtyBits>(dirtyMask), RuntimeDirtyBits::Hierarchy | RuntimeDirtyBits::Lifetime)) {
            structuralChange = true;
        }
        SyncEntityFromScene(scene, sceneEntity, structuralChange);
    }

    if (useImplicitTransformScan) {
        for (const auto& [sceneEntity, data] : scene.m_Entities) {
            if (!data.Transform.TransformDirty || removedEntities.find(sceneEntity) != removedEntities.end()) {
                continue;
            }
            RuntimeDirtyBits explicitDirtyBits = RuntimeDirtyBits::None;
            if (static_cast<size_t>(sceneEntity) < scene.m_RuntimeWorldBridgeDirtyMasks.size()) {
                explicitDirtyBits = static_cast<RuntimeDirtyBits>(scene.m_RuntimeWorldBridgeDirtyMasks[sceneEntity]);
            }
            if (HasAny(explicitDirtyBits, RuntimeDirtyBits::TransformLocal)) {
                continue;
            }
            SyncEntityFromScene(scene, sceneEntity, structuralChange);
        }
        scene.ResetRuntimeWorldTransformTracking();
    } else {
        for (EntityID sceneEntity : queuedTransformEntities) {
            if (sceneEntity == INVALID_ENTITY_ID || removedEntities.find(sceneEntity) != removedEntities.end()) {
                continue;
            }
            RuntimeDirtyBits explicitDirtyBits = RuntimeDirtyBits::None;
            if (static_cast<size_t>(sceneEntity) < scene.m_RuntimeWorldBridgeDirtyMasks.size()) {
                explicitDirtyBits = static_cast<RuntimeDirtyBits>(scene.m_RuntimeWorldBridgeDirtyMasks[sceneEntity]);
            }
            if (HasAny(explicitDirtyBits, RuntimeDirtyBits::TransformLocal)) {
                continue;
            }
            EntityData* data = scene.GetEntityData(sceneEntity);
            if (!data || !data->Transform.TransformDirty) {
                continue;
            }
            SyncEntityFromScene(scene, sceneEntity, structuralChange);
        }
    }

    if (structuralChange) {
        RebuildSceneOrder(scene);
    }
    if (structuralChange || m_HierarchyDirty) {
        RefreshParentLinks(scene);
        RebuildHierarchy();
    }
    if (m_PersistentIndicesDirty) {
        RebuildPersistentIndices();
    }
    if (m_SkinningCachesDirty) {
        RebuildSkinningCaches(scene);
        m_SkinningCachesDirty = false;
    }

    scene.ResetRuntimeWorldDirtyTracking();
    scene.m_RuntimeWorldBridgeRemovedEntities.clear();
    m_Stats.SceneSyncVersion = ++m_GlobalVersion;
    UpdateStats();
}

void RuntimeWorld::SyncQueuedTransformsFromScene(Scene& scene) {
    ScopedTimer transformSyncTimer("RuntimeWorld/SyncQueuedTransforms");

    std::vector<EntityID> queuedTransformEntities;
    if (scene.m_RuntimeWorldPendingTransformBitset) {
        queuedTransformEntities.reserve(64u);
        for (size_t wordIndex = 0; wordIndex < scene.m_RuntimeWorldPendingTransformBitWordCount; ++wordIndex) {
            uint64_t bits =
                scene.m_RuntimeWorldPendingTransformBitset[wordIndex].exchange(0u, std::memory_order_acq_rel);
            while (bits != 0u) {
                unsigned long bitIndex = 0;
                _BitScanForward64(&bitIndex, bits);
                const size_t entityIndex = (wordIndex << 6u) + static_cast<size_t>(bitIndex);
                if (entityIndex < static_cast<size_t>(scene.m_NextID)) {
                    queuedTransformEntities.push_back(static_cast<EntityID>(entityIndex));
                }
                bits &= (bits - 1u);
            }
        }
    }

    auto syncTransformEntity = [&](EntityID sceneEntity) {
        if (sceneEntity == INVALID_ENTITY_ID) {
            return;
        }

        const auto existing = m_SceneToIndex.find(sceneEntity);
        if (existing == m_SceneToIndex.end()) {
            return;
        }

        EntityData* data = scene.GetEntityData(sceneEntity);
        if (!data || !data->Transform.TransformDirty) {
            return;
        }

        const Index index = existing->second;
        RuntimeDirtyBits explicitDirtyBits = RuntimeDirtyBits::None;
        if (static_cast<size_t>(sceneEntity) < scene.m_RuntimeWorldBridgeDirtyMasks.size()) {
            explicitDirtyBits = static_cast<RuntimeDirtyBits>(scene.m_RuntimeWorldBridgeDirtyMasks[sceneEntity]);
        }

        const RuntimeLocalTransform local = ExtractLocalTransform(data->Transform);
        const bool localChanged =
            !NearlyEqual(m_LocalTransforms[index].Position, local.Position) ||
            !NearlyEqual(m_LocalTransforms[index].EulerDegrees, local.EulerDegrees) ||
            !NearlyEqual(m_LocalTransforms[index].Rotation, local.Rotation) ||
            !NearlyEqual(m_LocalTransforms[index].Scale, local.Scale) ||
            (m_LocalTransforms[index].UseQuatRotation != local.UseQuatRotation) ||
            data->Transform.TransformDirty;

        if (localChanged) {
            m_LocalTransforms[index] = local;
            m_LocalMatrices[index] = BuildLocalMatrix(local);
            MarkDirty(index, RuntimeDirtyBits::TransformLocal);
        }

        if (HasAny(explicitDirtyBits, RuntimeDirtyBits::TransformWorld)) {
            if (!NearlyEqual(m_WorldMatrices[index], data->Transform.WorldMatrix)) {
                m_WorldMatrices[index] = data->Transform.WorldMatrix;
            }
            MarkDirty(index, RuntimeDirtyBits::TransformWorld | RuntimeDirtyBits::Bounds);
        }
    };

    if (!queuedTransformEntities.empty()) {
        for (EntityID sceneEntity : queuedTransformEntities) {
            syncTransformEntity(sceneEntity);
        }
        return;
    }

    // PERF: this fallback runs whenever the pending-transform bitset is drained,
    // which happens on every UpdateTransforms pass after the first one in a frame
    // (there are several passes per frame). Previously it called syncTransformEntity
    // for *every* entity -- two hash lookups each (m_SceneToIndex + GetEntityData) --
    // even though almost nothing is dirty on the repeated passes. Read the AoS dirty
    // flag directly from the map iterator (no lookup) and only invoke the full sync
    // for entities that are actually dirty. This keeps the fallback a single cheap
    // bool check per entity instead of O(total entities) hash lookups -- important
    // because deep bone skeletons inflate the entity count (~80 bone entities per
    // character) and that cost is paid several times per frame.
    for (const auto& [sceneEntity, data] : scene.m_Entities) {
        if (!data.Transform.TransformDirty) {
            continue;
        }
        syncTransformEntity(sceneEntity);
    }
}

void RuntimeWorld::SyncEntityFromScene(Scene& scene, EntityID sceneEntity, bool& structuralChange) {
    EntityData* data = scene.GetEntityData(sceneEntity);
    if (!data) {
        const auto existing = m_SceneToIndex.find(sceneEntity);
        if (existing != m_SceneToIndex.end()) {
            ReleaseSlot(existing->second);
            structuralChange = true;
        }
        return;
    }

    Index index = InvalidIndex;
    const auto existing = m_SceneToIndex.find(sceneEntity);
    if (existing != m_SceneToIndex.end()) {
        index = existing->second;
    } else {
        index = AcquireSlot(sceneEntity);
        structuralChange = true;
    }

    EntitySlot& slot = m_Entities[index];
    RuntimeDirtyBits explicitDirtyBits = RuntimeDirtyBits::None;
    if (static_cast<size_t>(sceneEntity) < scene.m_RuntimeWorldBridgeDirtyMasks.size()) {
        explicitDirtyBits = static_cast<RuntimeDirtyBits>(scene.m_RuntimeWorldBridgeDirtyMasks[sceneEntity]);
    }

    const bool worldOverrideOnly =
        existing != m_SceneToIndex.end() &&
        !data->Transform.TransformDirty &&
        HasAny(explicitDirtyBits, RuntimeDirtyBits::TransformWorld) &&
        !HasAny(explicitDirtyBits,
            RuntimeDirtyBits::Metadata |
            RuntimeDirtyBits::TransformLocal |
            RuntimeDirtyBits::Hierarchy |
            RuntimeDirtyBits::Visibility |
            RuntimeDirtyBits::RenderBinding |
            RuntimeDirtyBits::Light |
            RuntimeDirtyBits::Lifetime);
    if (worldOverrideOnly) {
        if (!NearlyEqual(m_WorldMatrices[index], data->Transform.WorldMatrix)) {
            m_WorldMatrices[index] = data->Transform.WorldMatrix;
        }
        MarkDirty(index, RuntimeDirtyBits::TransformWorld | RuntimeDirtyBits::Bounds);
        return;
    }

    bool metadataChanged = false;
    if (slot.Guid.high != data->EntityGuid.high || slot.Guid.low != data->EntityGuid.low) {
        slot.Guid = data->EntityGuid;
        metadataChanged = true;
    }
    if (slot.Name != data->Name) {
        slot.Name = data->Name;
        metadataChanged = true;
    }
    if (slot.Tag != data->Tag) {
        slot.Tag = data->Tag;
        metadataChanged = true;
    }
    if (slot.Layer != static_cast<uint32_t>(data->Layer)) {
        slot.Layer = static_cast<uint32_t>(data->Layer);
        metadataChanged = true;
    }
    if (metadataChanged) {
        MarkDirty(index, RuntimeDirtyBits::Metadata);
    }

    bool visibilityChanged = false;
    if (slot.Active != data->Active) {
        if (slot.Active && m_ActiveEntityCount > 0) {
            --m_ActiveEntityCount;
        } else if (!slot.Active) {
            ++m_ActiveEntityCount;
        }
        slot.Active = data->Active;
        visibilityChanged = true;
    }
    if (slot.Visible != data->Visible) {
        slot.Visible = data->Visible;
        visibilityChanged = true;
    }
    if (slot.PresentationHidden != data->PresentationHidden) {
        slot.PresentationHidden = data->PresentationHidden;
        visibilityChanged = true;
    }
    if (visibilityChanged) {
        MarkDirty(index, RuntimeDirtyBits::Visibility);
    }

    const RuntimeLocalTransform local = ExtractLocalTransform(data->Transform);
    const bool localChanged =
        !NearlyEqual(m_LocalTransforms[index].Position, local.Position) ||
        !NearlyEqual(m_LocalTransforms[index].EulerDegrees, local.EulerDegrees) ||
        !NearlyEqual(m_LocalTransforms[index].Rotation, local.Rotation) ||
        !NearlyEqual(m_LocalTransforms[index].Scale, local.Scale) ||
        (m_LocalTransforms[index].UseQuatRotation != local.UseQuatRotation) ||
        data->Transform.TransformDirty;
    if (localChanged) {
        m_LocalTransforms[index] = local;
        m_LocalMatrices[index] = BuildLocalMatrix(local);
        MarkDirty(index, RuntimeDirtyBits::TransformLocal);
    }
    if (HasAny(explicitDirtyBits, RuntimeDirtyBits::TransformWorld)) {
        if (!NearlyEqual(m_WorldMatrices[index], data->Transform.WorldMatrix)) {
            m_WorldMatrices[index] = data->Transform.WorldMatrix;
        }
        MarkDirty(index, RuntimeDirtyBits::TransformWorld | RuntimeDirtyBits::Bounds);
    }

    const bool hasMesh = (data->Mesh != nullptr) && (data->Mesh->mesh != nullptr);
    const bool hasTerrain = (data->Terrain != nullptr);
    const bool hasLight = (data->Light != nullptr);
    const bool hasCamera = (data->Camera != nullptr);
    const bool hasAudioSource = (data->AudioSource != nullptr);
    const bool hasAudioListener = (data->AudioListener != nullptr);
    const bool hasEmitter = (data->Emitter != nullptr);
    const bool hasScripts = !data->Scripts.empty();
    const bool hasAnimationRoot = (data->AnimationPlayer != nullptr) && (data->Skeleton != nullptr);
    const bool hasSkinning = (data->Skinning != nullptr) && (data->Mesh != nullptr);
    const bool hasPhysicsSync = (data->CharacterController != nullptr) ||
                                (data->RigidBody != nullptr) ||
                                (data->StaticBody != nullptr) ||
                                (data->Collider != nullptr) ||
                                (data->Area != nullptr);
    const bool castsShadows = (!data->RenderOverrides || data->RenderOverrides->CastShadows);
    const bool skipFrustumCulling = (data->Mesh != nullptr) ? data->Mesh->SkipFrustumCulling : false;
    const float boundsPadding = (data->Mesh != nullptr) ? data->Mesh->BoundsPadding : 1.0f;

    bool renderBindingChanged = false;
    if (slot.HasMesh != hasMesh) {
        slot.HasMesh = hasMesh;
        renderBindingChanged = true;
    }
    if (slot.HasTerrain != hasTerrain) {
        slot.HasTerrain = hasTerrain;
        renderBindingChanged = true;
    }
    if (slot.CastsShadows != castsShadows) {
        slot.CastsShadows = castsShadows;
        renderBindingChanged = true;
    }
    if (slot.SkipFrustumCulling != skipFrustumCulling) {
        slot.SkipFrustumCulling = skipFrustumCulling;
        renderBindingChanged = true;
    }
    if (!NearlyEqual(slot.BoundsPadding, boundsPadding)) {
        slot.BoundsPadding = boundsPadding;
        renderBindingChanged = true;
    }
    if (renderBindingChanged) {
        MarkDirty(index, RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Bounds);
    }

    bool systemBindingChanged = false;
    bool skinningBindingChanged = false;
    if (slot.HasCamera != hasCamera) {
        slot.HasCamera = hasCamera;
        systemBindingChanged = true;
    }
    if (slot.HasAudioSource != hasAudioSource) {
        slot.HasAudioSource = hasAudioSource;
        systemBindingChanged = true;
    }
    if (slot.HasAudioListener != hasAudioListener) {
        slot.HasAudioListener = hasAudioListener;
        systemBindingChanged = true;
    }
    if (slot.HasEmitter != hasEmitter) {
        slot.HasEmitter = hasEmitter;
        systemBindingChanged = true;
    }
    if (slot.HasScripts != hasScripts) {
        slot.HasScripts = hasScripts;
        systemBindingChanged = true;
    }
    if (slot.HasAnimationRoot != hasAnimationRoot) {
        slot.HasAnimationRoot = hasAnimationRoot;
        systemBindingChanged = true;
        skinningBindingChanged = true;
    }
    if (slot.HasSkinning != hasSkinning) {
        slot.HasSkinning = hasSkinning;
        systemBindingChanged = true;
        skinningBindingChanged = true;
    }
    if (slot.HasPhysicsSync != hasPhysicsSync) {
        slot.HasPhysicsSync = hasPhysicsSync;
        systemBindingChanged = true;
    }
    if (systemBindingChanged) {
        m_PersistentIndicesDirty = true;
        if (skinningBindingChanged) {
            m_SkinningCachesDirty = true;
        }
        m_Stats.MetadataVersion = ++m_GlobalVersion;
    }

    const RuntimeBounds localBounds = ExtractLocalBounds(*data);
    if (m_Bounds[index].Valid != localBounds.Valid ||
        !NearlyEqual(m_Bounds[index].LocalMin, localBounds.LocalMin) ||
        !NearlyEqual(m_Bounds[index].LocalMax, localBounds.LocalMax)) {
        m_Bounds[index].Valid = localBounds.Valid;
        m_Bounds[index].LocalMin = localBounds.LocalMin;
        m_Bounds[index].LocalMax = localBounds.LocalMax;
        MarkDirty(index, RuntimeDirtyBits::Bounds);
    }

    bool lightChanged = false;
    bool lightBindingChanged = false;
    if (slot.HasLight != hasLight) {
        slot.HasLight = hasLight;
        lightChanged = true;
        lightBindingChanged = true;
    }
    if (hasLight) {
        LightState light{};
        light.Enabled = true;
        light.Type = data->Light->Type;
        light.Color = data->Light->Color;
        light.Intensity = data->Light->Intensity;
        light.Range = data->Light->Range;
        light.SpotInnerAngleDegrees = data->Light->SpotInnerAngleDegrees;
        light.SpotOuterAngleDegrees = data->Light->SpotOuterAngleDegrees;
        if (!m_Lights[index].Enabled ||
            m_Lights[index].Type != light.Type ||
            !NearlyEqual(m_Lights[index].Color, light.Color) ||
            !NearlyEqual(m_Lights[index].Intensity, light.Intensity) ||
            !NearlyEqual(m_Lights[index].Range, light.Range) ||
            !NearlyEqual(m_Lights[index].SpotInnerAngleDegrees, light.SpotInnerAngleDegrees) ||
            !NearlyEqual(m_Lights[index].SpotOuterAngleDegrees, light.SpotOuterAngleDegrees)) {
            m_Lights[index] = light;
            lightChanged = true;
        }
    } else if (m_Lights[index].Enabled) {
        m_Lights[index] = {};
        lightChanged = true;
    }
    if (lightChanged) {
        RuntimeDirtyBits lightDirtyBits = RuntimeDirtyBits::Light;
        if (lightBindingChanged) {
            lightDirtyBits |= RuntimeDirtyBits::RenderBinding;
        }
        MarkDirty(index, lightDirtyBits);
    }
}

void RuntimeWorld::RebuildSceneOrder(const Scene& scene) {
    const auto& sceneEntities = scene.GetEntities();
    m_SceneOrder.clear();
    m_SceneOrder.reserve(sceneEntities.size());

    for (const Entity& entity : sceneEntities) {
        const auto it = m_SceneToIndex.find(entity.GetID());
        if (it == m_SceneToIndex.end() || !IsIndexAlive(it->second)) {
            continue;
        }
        m_SceneOrder.push_back(it->second);
    }
}

void RuntimeWorld::RefreshParentLinks(Scene& scene) {
    for (Index index : m_SceneOrder) {
        if (!IsIndexAlive(index)) {
            continue;
        }

        EntityData* data = scene.GetEntityData(m_Entities[index].SceneEntity);
        if (!data) {
            continue;
        }

        Index desiredParent = InvalidIndex;
        if (data->Parent != INVALID_ENTITY_ID) {
            const auto parentIt = m_SceneToIndex.find(data->Parent);
            if (parentIt != m_SceneToIndex.end() && IsIndexAlive(parentIt->second)) {
                desiredParent = parentIt->second;
            }
        }

        if (m_Entities[index].Parent != desiredParent) {
            m_Entities[index].Parent = desiredParent;
            MarkDirty(index, RuntimeDirtyBits::Hierarchy);
        }
    }
}

void RuntimeWorld::RebuildHierarchy() {
    m_LevelOrder.clear();
    m_LevelOffsets.clear();
    m_ChildIndices.clear();
    m_ChildOffsets.assign(m_Entities.size(), 0u);
    m_ChildCounts.assign(m_Entities.size(), 0u);

    for (Index index : m_SceneOrder) {
        if (!IsIndexAlive(index)) {
            continue;
        }
        const Index parent = m_Entities[index].Parent;
        if (parent != InvalidIndex && IsIndexAlive(parent)) {
            ++m_ChildCounts[parent];
        }
    }

    size_t runningOffset = 0;
    for (Index index = 0; index < m_Entities.size(); ++index) {
        m_ChildOffsets[index] = runningOffset;
        runningOffset += m_ChildCounts[index];
    }
    m_ChildIndices.assign(runningOffset, InvalidIndex);

    std::vector<size_t> cursor = m_ChildOffsets;
    for (Index index : m_SceneOrder) {
        if (!IsIndexAlive(index)) {
            continue;
        }
        const Index parent = m_Entities[index].Parent;
        if (parent != InvalidIndex && IsIndexAlive(parent)) {
            m_ChildIndices[cursor[parent]++] = index;
        }
    }

    m_LevelOffsets.push_back(0u);
    for (Index index : m_SceneOrder) {
        if (!IsIndexAlive(index)) {
            continue;
        }
        if (m_Entities[index].Parent == InvalidIndex) {
            m_Entities[index].Depth = 0u;
            m_LevelOrder.push_back(index);
        }
    }

    size_t levelStart = 0;
    while (levelStart < m_LevelOrder.size()) {
        const size_t levelEnd = m_LevelOrder.size();
        for (size_t i = levelStart; i < levelEnd; ++i) {
            const Index parent = m_LevelOrder[i];
            const uint32_t childDepth = m_Entities[parent].Depth + 1u;
            const size_t childStart = m_ChildOffsets[parent];
            const size_t childCount = m_ChildCounts[parent];
            for (size_t childIndex = 0; childIndex < childCount; ++childIndex) {
                const Index child = m_ChildIndices[childStart + childIndex];
                if (!IsIndexAlive(child)) {
                    continue;
                }
                m_Entities[child].Depth = childDepth;
                m_LevelOrder.push_back(child);
            }
        }
        levelStart = levelEnd;
        if (levelStart < m_LevelOrder.size()) {
            m_LevelOffsets.push_back(levelStart);
        }
    }
    m_LevelOffsets.push_back(m_LevelOrder.size());

    m_HierarchyDirty = false;
    m_PersistentIndicesDirty = true;
    m_SkinningCachesDirty = true;
    m_Stats.HierarchyVersion = ++m_GlobalVersion;
}

void RuntimeWorld::RebuildPersistentIndices() {
    m_RenderableIndices.clear();
    m_TerrainIndices.clear();
    m_LightIndices.clear();
    m_CameraIndices.clear();
    m_AudioSourceIndices.clear();
    m_AudioListenerIndices.clear();
    m_EmitterIndices.clear();
    m_ScriptedIndices.clear();
    m_AnimationRootIndices.clear();
    m_SkinnedMeshIndices.clear();
    m_PhysicsIndices.clear();
    m_RenderableSceneEntities.clear();
    m_TerrainSceneEntities.clear();
    m_LightSceneEntities.clear();
    m_CameraSceneEntities.clear();
    m_AudioSourceSceneEntities.clear();
    m_AudioListenerSceneEntities.clear();
    m_EmitterSceneEntities.clear();
    m_ScriptedSceneEntities.clear();
    m_AnimationRootSceneEntities.clear();
    m_SkinnedMeshSceneEntities.clear();
    m_PhysicsSceneEntities.clear();

    for (Index index : m_SceneOrder) {
        if (!IsIndexAlive(index)) {
            continue;
        }
        const EntitySlot& slot = m_Entities[index];
        if (slot.HasMesh) {
            m_RenderableIndices.push_back(index);
            m_RenderableSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasTerrain) {
            m_TerrainIndices.push_back(index);
            m_TerrainSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasLight) {
            m_LightIndices.push_back(index);
            m_LightSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasCamera) {
            m_CameraIndices.push_back(index);
            m_CameraSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasAudioSource) {
            m_AudioSourceIndices.push_back(index);
            m_AudioSourceSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasAudioListener) {
            m_AudioListenerIndices.push_back(index);
            m_AudioListenerSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasEmitter) {
            m_EmitterIndices.push_back(index);
            m_EmitterSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasScripts) {
            m_ScriptedIndices.push_back(index);
            m_ScriptedSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasAnimationRoot) {
            m_AnimationRootIndices.push_back(index);
            m_AnimationRootSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasSkinning) {
            m_SkinnedMeshIndices.push_back(index);
            m_SkinnedMeshSceneEntities.push_back(slot.SceneEntity);
        }
        if (slot.HasPhysicsSync) {
            m_PhysicsIndices.push_back(index);
            m_PhysicsSceneEntities.push_back(slot.SceneEntity);
        }
    }

    m_PersistentIndicesDirty = false;
    m_Stats.RenderBindingVersion = ++m_GlobalVersion;
}

void RuntimeWorld::RebuildSkinningCaches(Scene& scene) {
    m_SkinningGroupCaches.clear();
    m_SkinningGroupCacheLookup.clear();
    if (m_SkinnedMeshIndices.empty()) {
        return;
    }

    m_SkinningGroupCaches.reserve(m_SkinnedMeshIndices.size());
    m_SkinningGroupCacheLookup.reserve(m_SkinnedMeshIndices.size());

    auto makeHandle = [this](Index index) {
        RuntimeEntityHandle handle{};
        if (IsIndexAlive(index)) {
            handle.Index = index;
            handle.Generation = m_Entities[index].Generation;
        }
        return handle;
    };

    for (Index meshIndex : m_SkinnedMeshIndices) {
        if (!IsIndexAlive(meshIndex)) {
            continue;
        }

        const EntityID meshSceneEntity = m_Entities[meshIndex].SceneEntity;
        EntityData* meshData = scene.GetEntityData(meshSceneEntity);
        if (!meshData || !meshData->Skinning || !meshData->Mesh) {
            continue;
        }

        EntityID skeletonRoot = ResolveRuntimeSkinningSkeletonRoot(scene, meshSceneEntity, meshData->Skinning.get());
        if (skeletonRoot == INVALID_ENTITY_ID) {
            continue;
        }

        EntityData* skeletonData = scene.GetEntityData(skeletonRoot);
        if (!skeletonData || !skeletonData->Skeleton) {
            continue;
        }

        const auto skeletonIt = m_SceneToIndex.find(skeletonRoot);
        if (skeletonIt == m_SceneToIndex.end() || !IsIndexAlive(skeletonIt->second)) {
            continue;
        }

        size_t groupIndex = 0;
        auto groupIt = m_SkinningGroupCacheLookup.find(skeletonRoot);
        if (groupIt == m_SkinningGroupCacheLookup.end()) {
            RuntimeSkinningGroupCache cache{};
            cache.SkeletonSceneEntity = skeletonRoot;
            cache.SkeletonHandle = makeHandle(skeletonIt->second);

            const SkeletonComponent& skeleton = *skeletonData->Skeleton;
            const size_t boneCount = std::min(skeleton.BoneEntities.size(), skeleton.InverseBindPoses.size());
            cache.BoneHandles.reserve(boneCount);
            cache.BoneSceneEntities.reserve(boneCount);
            for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                const EntityID boneSceneEntity = skeleton.BoneEntities[boneIndex];
                cache.BoneSceneEntities.push_back(boneSceneEntity);

                RuntimeEntityHandle boneHandle{};
                const auto boneIt = m_SceneToIndex.find(boneSceneEntity);
                if (boneIt != m_SceneToIndex.end() && IsIndexAlive(boneIt->second)) {
                    boneHandle = makeHandle(boneIt->second);
                }
                cache.BoneHandles.push_back(boneHandle);
            }

            groupIndex = m_SkinningGroupCaches.size();
            m_SkinningGroupCacheLookup.emplace(skeletonRoot, groupIndex);
            m_SkinningGroupCaches.push_back(std::move(cache));
        } else {
            groupIndex = groupIt->second;
        }

        RuntimeSkinningGroupCache& cache = m_SkinningGroupCaches[groupIndex];
        cache.MeshSceneEntities.push_back(meshSceneEntity);
        cache.MeshHandles.push_back(makeHandle(meshIndex));
    }
}

void RuntimeWorld::WriteBackSceneTransforms(Scene& scene) {
    for (Index index : m_SceneOrder) {
        if (!IsIndexAlive(index) || m_RecomputedThisPass[index] == 0u) {
            continue;
        }

        EntityData* data = scene.GetEntityData(m_Entities[index].SceneEntity);
        if (!data) {
            continue;
        }

        data->Transform.LocalMatrix = m_LocalMatrices[index];
        data->Transform.WorldMatrix = m_WorldMatrices[index];
        data->Transform.TransformDirty = false;
    }
}

void RuntimeWorld::UpdateTransforms(Scene& scene) {
    ScopedTimer transformTimer("RuntimeWorld/UpdateTransforms");
    Profiler::Get().AddCounter("RuntimeWorld/TransformPhaseSyncRequests", 1);
    // PERF: only do a full O(N) SyncFromScene when there is real structural/component
    // sync work (entities added/removed, or non-transform component dirty bits). In
    // steady state -- where only transforms move (physics/nav) -- the per-frame sync
    // lock is never set (it is gated on structural work in Scene::Update), so this used
    // to fall through to a full SyncFromScene on EVERY transform pass (~3x/frame),
    // re-reading all N entities (heavily bone-inflated for skeletal characters).
    // Transform-only changes are captured by the pending-transform bitset, so the
    // incremental SyncQueuedTransforms (O(moving entities)) is sufficient and correct.
    // A mid-frame component change sets structural work, so the next pass still performs
    // a full sync the same frame -- no deferral / no 1-frame-lag regression.
    if (scene.HasPendingRuntimeWorldStructuralSyncWork()) {
        Profiler::Get().AddCounter("RuntimeWorld/TransformDeferredStructuralSyncs", 1);
    }
    const bool needsFullStructuralSync =
        m_SceneOrder.empty() ||
        (!scene.IsRuntimeWorldFrameSyncLocked() && scene.HasPendingRuntimeWorldStructuralSyncWork());
    if (needsFullStructuralSync) {
        SyncFromScene(scene, RuntimeSyncReason::SceneUpdate, false);
    } else {
        SyncQueuedTransformsFromScene(scene);
    }
    uint64_t transformAnchorCount = 0;
    uint64_t transformVisitedCount = 0;

    m_TaskGraph.Clear();
    m_TaskGraph.Add({
        "RuntimeWorld/PropagateTransforms",
        RuntimeTaskCategory::Simulation,
        DirtyBitsToStoreMask(RuntimeDirtyBits::TransformLocal | RuntimeDirtyBits::Hierarchy | RuntimeDirtyBits::TransformWorld),
        DirtyBitsToStoreMask(RuntimeDirtyBits::TransformLocal | RuntimeDirtyBits::Bounds | RuntimeDirtyBits::TransformWorld),
        false,
        true,
        {},
        [this, &transformAnchorCount, &transformVisitedCount]() {
            std::fill(m_RecomputedThisPass.begin(), m_RecomputedThisPass.end(), 0u);
            if (m_SceneOrder.empty() || m_TransformStageCandidates.empty()) {
                return;
            }

            static thread_local std::vector<uint8_t> s_QueuedAnchors;
            static thread_local std::vector<Index> s_DirtyAnchors;
            static thread_local std::vector<Index> s_Stack;
            s_QueuedAnchors.assign(m_Entities.size(), 0u);
            s_DirtyAnchors.clear();
            std::vector<uint8_t>& queuedAnchors = s_QueuedAnchors;
            std::vector<Index>& dirtyAnchors = s_DirtyAnchors;
            dirtyAnchors.reserve(m_TransformStageCandidates.size());

            for (Index index : m_TransformStageCandidates) {
                if (!IsTransformStageDirty(index)) {
                    continue;
                }

                Index anchor = index;
                while (true) {
                    const Index parent = m_Entities[anchor].Parent;
                    if (parent == InvalidIndex || !IsTransformStageDirty(parent)) {
                        break;
                    }
                    anchor = parent;
                }

                if (queuedAnchors[anchor] == 0u) {
                    queuedAnchors[anchor] = 1u;
                    dirtyAnchors.push_back(anchor);
                }
            }

            if (dirtyAnchors.empty()) {
                return;
            }

            transformAnchorCount = static_cast<uint64_t>(dirtyAnchors.size());
            std::vector<Index>& stack = s_Stack;
            stack.clear();
            stack.reserve(m_SceneOrder.size());

            for (Index anchor : dirtyAnchors) {
                if (!IsIndexAlive(anchor)) {
                    continue;
                }

                stack.clear();
                stack.push_back(anchor);
                while (!stack.empty()) {
                    const Index index = stack.back();
                    stack.pop_back();
                    if (!IsIndexAlive(index)) {
                        continue;
                    }

                    const Index parent = m_Entities[index].Parent;
                    const bool parentUpdated =
                        (parent != InvalidIndex) &&
                        IsIndexAlive(parent) &&
                        (m_RecomputedThisPass[parent] != 0u);
                    const bool localDirty = (m_TransformDirty[index] != 0u);
                    const bool externalOverride = (m_ExternalWorldDirty[index] != 0u);
                    const bool hierarchyDirty =
                        HasAny(m_Entities[index].DirtyMask, RuntimeDirtyBits::Hierarchy);

                    bool recomputed = false;
                    if (localDirty || hierarchyDirty || parentUpdated) {
                        if (localDirty) {
                            m_LocalMatrices[index] = BuildLocalMatrix(m_LocalTransforms[index]);
                        }
                        const glm::mat4 parentWorld =
                            (parent != InvalidIndex && IsIndexAlive(parent))
                            ? m_WorldMatrices[parent]
                            : glm::mat4(1.0f);
                        m_WorldMatrices[index] = parentWorld * m_LocalMatrices[index];
                        recomputed = true;
                    } else if (externalOverride) {
                        recomputed = true;
                    }

                    if (!recomputed) {
                        continue;
                    }

                    m_RecomputedThisPass[index] = 1u;
                    ++transformVisitedCount;
                    // A light whose world transform changed must be re-extracted.
                    // Children recomputed via ancestor movement are never marked
                    // dirty individually, so flag it here to refresh LightVersion.
                    if (m_Entities[index].HasLight) {
                        m_LightWorldDirtyFromHierarchy = true;
                    }
                    if (externalOverride) {
                        m_ExternalWorldDirty[index] = 0u;
                        ClearDirty(index, RuntimeDirtyBits::TransformWorld);
                    }

                    const size_t childStart = m_ChildOffsets[index];
                    const size_t childCount = m_ChildCounts[index];
                    for (size_t childOffset = 0; childOffset < childCount; ++childOffset) {
                        stack.push_back(m_ChildIndices[childStart + childOffset]);
                    }
                }
            }

            for (Index index : m_TransformStageCandidates) {
                if (!IsIndexAlive(index) || m_RecomputedThisPass[index] == 0u) {
                    continue;
                }
                ClearDirty(index, RuntimeDirtyBits::TransformLocal | RuntimeDirtyBits::Hierarchy);
            }
        }
    });

    m_TaskGraph.Add({
        "RuntimeWorld/WriteBackSceneTransforms",
        RuntimeTaskCategory::Simulation,
        DirtyBitsToStoreMask(RuntimeDirtyBits::TransformLocal | RuntimeDirtyBits::Bounds),
        0u,
        true,
        false,
        { "RuntimeWorld/PropagateTransforms" },
        [this, &scene]() {
            WriteBackSceneTransforms(scene);
        }
    });

    m_TaskGraph.Execute();
    CompactTransformStageCandidates();
    // If a light moved because an ancestor moved, bump LightVersion so the next
    // light extraction rebuilds the list with the updated world position. Done on
    // the main thread after the transform tasks have finished.
    if (m_LightWorldDirtyFromHierarchy) {
        m_Stats.LightVersion = ++m_GlobalVersion;
        m_LightWorldDirtyFromHierarchy = false;
    }
    Profiler::Get().SetCounter("RuntimeWorld/TransformAnchors", transformAnchorCount);
    Profiler::Get().SetCounter("RuntimeWorld/TransformVisited", transformVisitedCount);
    m_Stats.TaskCount = m_TaskGraph.GetLastStats().size();
    m_Stats.BarrierCount = m_TaskGraph.GetBarrierCount();
    UpdateStats();
}

void RuntimeWorld::RecomputeBounds() {
    for (Index index : m_RenderableIndices) {
        if (!IsIndexAlive(index)) {
            continue;
        }

        const EntitySlot& slot = m_Entities[index];
        const bool shouldRefresh =
            (m_RecomputedThisPass[index] != 0u) ||
            (m_ExternalWorldDirty[index] != 0u) ||
            HasAny(slot.DirtyMask, RuntimeDirtyBits::Bounds | RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Visibility);
        if (!shouldRefresh) {
            continue;
        }

        TransformBounds(m_Bounds[index], m_WorldMatrices[index], slot.BoundsPadding, m_Bounds[index]);
        ClearDirty(index, RuntimeDirtyBits::Bounds);
    }
}

void RuntimeWorld::PropagateExternalWorldOverrides(Scene& scene) {
    static thread_local std::vector<uint8_t> s_QueuedAnchors;
    static thread_local std::vector<Index> s_Anchors;
    static thread_local std::vector<Index> s_Stack;
    s_QueuedAnchors.assign(m_Entities.size(), 0u);
    s_Anchors.clear();
    std::vector<uint8_t>& queuedAnchors = s_QueuedAnchors;
    std::vector<Index>& anchors = s_Anchors;
    anchors.reserve(m_ExternalWorldDirtyIndices.size());

    for (Index index : m_ExternalWorldDirtyIndices) {
        if (!IsIndexAlive(index) || m_ExternalWorldDirty[index] == 0u) {
            continue;
        }

        Index anchor = index;
        while (true) {
            const Index parent = m_Entities[anchor].Parent;
            if (parent == InvalidIndex || !IsIndexAlive(parent) || m_ExternalWorldDirty[parent] == 0u) {
                break;
            }
            anchor = parent;
        }

        if (queuedAnchors[anchor] == 0u) {
            queuedAnchors[anchor] = 1u;
            anchors.push_back(anchor);
        }
    }

    if (anchors.empty()) {
        return;
    }

    std::vector<Index>& stack = s_Stack;
    stack.clear();
    stack.reserve(m_SceneOrder.size());

    for (Index anchor : anchors) {
        if (!IsIndexAlive(anchor)) {
            continue;
        }

        m_RecomputedThisPass[anchor] = 1u;

        const size_t childStart = m_ChildOffsets[anchor];
        const size_t childCount = m_ChildCounts[anchor];
        for (size_t childOffset = 0; childOffset < childCount; ++childOffset) {
            stack.push_back(m_ChildIndices[childStart + childOffset]);
        }

        while (!stack.empty()) {
            const Index index = stack.back();
            stack.pop_back();
            if (!IsIndexAlive(index)) {
                continue;
            }

            const Index parent = m_Entities[index].Parent;
            if (parent == InvalidIndex || !IsIndexAlive(parent)) {
                continue;
            }

            if (m_ExternalWorldDirty[index] == 0u) {
                m_WorldMatrices[index] = m_WorldMatrices[parent] * m_LocalMatrices[index];
            }
            m_RecomputedThisPass[index] = 1u;

            const size_t nestedChildStart = m_ChildOffsets[index];
            const size_t nestedChildCount = m_ChildCounts[index];
            for (size_t childOffset = 0; childOffset < nestedChildCount; ++childOffset) {
                stack.push_back(m_ChildIndices[nestedChildStart + childOffset]);
            }
        }
    }

    WriteBackSceneTransforms(scene);
}

void RuntimeWorld::FinalizeTransformStage(Scene& scene) {
    ScopedTimer finalizeTimer("RuntimeWorld/FinalizeTransformStage");

    auto hasPendingSyncWorkExcludingWorldOverrides = [&scene]() {
        if (scene.m_RuntimeWorldBridgeFullSyncPending ||
            !scene.m_RuntimeWorldBridgeRemovedEntities.empty()) {
            return true;
        }

        if (scene.m_RuntimeWorldPendingTransformBitset) {
            for (size_t i = 0; i < scene.m_RuntimeWorldPendingTransformBitWordCount; ++i) {
                if (scene.m_RuntimeWorldPendingTransformBitset[i].load(std::memory_order_acquire) != 0u) {
                    return true;
                }
            }
        }

        const RuntimeDirtyBits worldOverrideOnlyMask =
            RuntimeDirtyBits::TransformWorld | RuntimeDirtyBits::Bounds;

        for (EntityID sceneEntity : scene.m_RuntimeWorldBridgeDirtyEntities) {
            if (sceneEntity == INVALID_ENTITY_ID ||
                static_cast<size_t>(sceneEntity) >= scene.m_RuntimeWorldBridgeDirtyMasks.size()) {
                continue;
            }

            const RuntimeDirtyBits bits =
                static_cast<RuntimeDirtyBits>(scene.m_RuntimeWorldBridgeDirtyMasks[sceneEntity]);
            if (bits == RuntimeDirtyBits::None) {
                continue;
            }

            if ((bits & ~worldOverrideOnlyMask) != RuntimeDirtyBits::None) {
                return true;
            }
        }

        return false;
    };

    if (hasPendingSyncWorkExcludingWorldOverrides() && !scene.IsRuntimeWorldFrameSyncLocked()) {
        Profiler::Get().AddCounter("RuntimeWorld/FinalizeTransformSyncRequests", 1);
        SyncFromScene(scene, RuntimeSyncReason::SceneUpdate, false);
    } else if (hasPendingSyncWorkExcludingWorldOverrides()) {
        Profiler::Get().AddCounter("RuntimeWorld/FinalizeDeferredSyncs", 1);
    }

    bool hasExternalOverrides = false;
    for (Index index : m_ExternalWorldDirtyIndices) {
        if (IsIndexAlive(index) && m_ExternalWorldDirty[index] != 0u) {
            hasExternalOverrides = true;
            break;
        }
    }

    m_TaskGraph.Clear();
    if (hasExternalOverrides) {
        m_TaskGraph.Add({
            "RuntimeWorld/PropagateExternalOverrides",
            RuntimeTaskCategory::Simulation,
            DirtyBitsToStoreMask(RuntimeDirtyBits::TransformWorld | RuntimeDirtyBits::Hierarchy | RuntimeDirtyBits::TransformLocal),
            DirtyBitsToStoreMask(RuntimeDirtyBits::TransformWorld),
            true,
            false,
            {},
            [this, &scene]() {
                PropagateExternalWorldOverrides(scene);
            }
        });
    }

    m_TaskGraph.Add({
        "RuntimeWorld/RecomputeBounds",
        RuntimeTaskCategory::Simulation,
        DirtyBitsToStoreMask(RuntimeDirtyBits::Bounds | RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Visibility | RuntimeDirtyBits::TransformWorld),
        DirtyBitsToStoreMask(RuntimeDirtyBits::Bounds | RuntimeDirtyBits::TransformWorld),
        false,
        true,
        hasExternalOverrides ? std::vector<std::string>{ "RuntimeWorld/PropagateExternalOverrides" } : std::vector<std::string>{},
        [this]() {
            RecomputeBounds();
        }
    });

    m_TaskGraph.Execute();
    for (Index index : m_ExternalWorldDirtyIndices) {
        if (!IsIndexAlive(index) || m_ExternalWorldDirty[index] == 0u) {
            continue;
        }
        ClearDirty(index, RuntimeDirtyBits::TransformWorld);
        m_ExternalWorldDirty[index] = 0u;
    }
    std::fill(m_RecomputedThisPass.begin(), m_RecomputedThisPass.end(), 0u);
    m_ExternalWorldDirtyIndices.clear();
    CompactTransformStageCandidates();
    m_Stats.TaskCount = m_TaskGraph.GetLastStats().size();
    m_Stats.BarrierCount = m_TaskGraph.GetBarrierCount();
    UpdateStats();
}

void RuntimeWorld::BuildRenderWorld(Scene& scene, uint32_t activeLayerMask, bool enforceLayerMask) {
    ScopedTimer renderWorldTimer("RuntimeWorld/BuildRenderWorld");
    Profiler::Get().SetCounter("RuntimeWorld/RenderExtractRenderableTasks", 0);
    Profiler::Get().SetCounter("RuntimeWorld/RenderExtractTerrainTasks", 0);
    Profiler::Get().SetCounter("RuntimeWorld/RenderExtractLightTasks", 0);
    if (m_SceneOrder.empty() ||
        (scene.HasPendingRuntimeWorldStructuralSyncWork() && !scene.IsRuntimeWorldFrameSyncLocked())) {
        Profiler::Get().AddCounter("RuntimeWorld/RenderTriggeredSyncs", 1);
        SyncFromScene(scene, RuntimeSyncReason::Render, false);
    } else if (scene.HasPendingRuntimeWorldStructuralSyncWork()) {
        Profiler::Get().AddCounter("RuntimeWorld/RenderDeferredSyncs", 1);
    }

    uint64_t renderableFingerprint = 1469598103934665603ULL;
    renderableFingerprint = HashCombine64(renderableFingerprint, m_Stats.RenderBindingVersion);
    renderableFingerprint = HashCombine64(renderableFingerprint, enforceLayerMask ? 1u : 0u);
    if (enforceLayerMask) {
        renderableFingerprint = HashCombine64(renderableFingerprint, m_Stats.MetadataVersion);
        renderableFingerprint = HashCombine64(renderableFingerprint, static_cast<uint64_t>(activeLayerMask));
    }

    uint64_t terrainFingerprint = 1469598103934665603ULL;
    terrainFingerprint = HashCombine64(terrainFingerprint, m_Stats.RenderBindingVersion);

    uint64_t lightFingerprint = 1469598103934665603ULL;
    lightFingerprint = HashCombine64(lightFingerprint, m_Stats.LightVersion);
    lightFingerprint = HashCombine64(lightFingerprint, m_Stats.RenderBindingVersion);

    const bool rebuildRenderables = (renderableFingerprint != m_LastRenderableListFingerprint);
    const bool rebuildTerrains = (terrainFingerprint != m_LastTerrainListFingerprint);
    const bool rebuildLights = (lightFingerprint != m_LastLightListFingerprint);
    if (!rebuildRenderables && !rebuildTerrains && !rebuildLights) {
        UpdateStats();
        return;
    }

    std::vector<RuntimeRenderEntityRef> visibleMeshEntities;
    std::vector<RuntimeRenderEntityRef> shadowMeshEntities;
    std::vector<RuntimeRenderEntityRef> visibleTerrainEntities;
    std::vector<RuntimeRenderEntityRef> shadowTerrainEntities;
    std::vector<RuntimeLightEntry> lights;
    std::vector<std::vector<RuntimeRenderEntityRef>> visibleMeshChunks;
    std::vector<std::vector<RuntimeRenderEntityRef>> shadowMeshChunks;
    std::vector<std::vector<RuntimeRenderEntityRef>> visibleTerrainChunks;
    std::vector<std::vector<RuntimeRenderEntityRef>> shadowTerrainChunks;
    std::vector<std::vector<RuntimeLightEntry>> lightChunks;
    const size_t extractionWorkerCount =
        (cm::g_JobSystem != nullptr)
            ? std::max<size_t>(cm::g_JobSystem->GetWorkerCount(), size_t{ 1 })
            : size_t{ 1 };
    auto mergeRenderEntityChunks =
        [](std::vector<RuntimeRenderEntityRef>& destination,
           std::vector<std::vector<RuntimeRenderEntityRef>>& chunks) {
            size_t totalEntries = 0;
            for (const auto& chunk : chunks) {
                totalEntries += chunk.size();
            }

            destination.clear();
            destination.reserve(totalEntries);
            for (auto& chunk : chunks) {
                destination.insert(
                    destination.end(),
                    std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
            }
        };
    auto mergeLightChunks =
        [](std::vector<RuntimeLightEntry>& destination,
           std::vector<std::vector<RuntimeLightEntry>>& chunks) {
            size_t totalEntries = 0;
            for (const auto& chunk : chunks) {
                totalEntries += chunk.size();
            }

            destination.clear();
            destination.reserve(totalEntries);
            for (auto& chunk : chunks) {
                destination.insert(
                    destination.end(),
                    std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
            }
        };
    m_TaskGraph.Clear();
    if (rebuildRenderables) {
        const bool useChunkedRenderableExtraction =
            (cm::g_JobSystem != nullptr) &&
            (extractionWorkerCount > 1) &&
            (m_RenderableIndices.size() > 64);
        if (useChunkedRenderableExtraction) {
            const size_t chunkSize =
                ComputeOptimalChunkSize(m_RenderableIndices.size(), extractionWorkerCount, size_t{ 64 });
            const size_t chunkCount = (m_RenderableIndices.size() + chunkSize - 1) / chunkSize;
            Profiler::Get().SetCounter(
                "RuntimeWorld/RenderExtractRenderableTasks",
                static_cast<uint64_t>(chunkCount));
            visibleMeshChunks.resize(chunkCount);
            shadowMeshChunks.resize(chunkCount);
            std::vector<std::string> renderableChunkTaskNames;
            renderableChunkTaskNames.reserve(chunkCount);

            for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
                const size_t begin = chunkIndex * chunkSize;
                const size_t end = std::min(begin + chunkSize, m_RenderableIndices.size());
                renderableChunkTaskNames.push_back(
                    "RuntimeWorld/ExtractRenderablesChunk" + std::to_string(chunkIndex));
                m_TaskGraph.Add({
                    renderableChunkTaskNames.back(),
                    RuntimeTaskCategory::Extraction,
                    DirtyBitsToStoreMask(RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Metadata),
                    0u,
                    false,
                    true,
                    {},
                    [this,
                     begin,
                     end,
                     chunkIndex,
                     activeLayerMask,
                     enforceLayerMask,
                     &visibleMeshChunks,
                     &shadowMeshChunks]() {
                        auto& visibleChunk = visibleMeshChunks[chunkIndex];
                        auto& shadowChunk = shadowMeshChunks[chunkIndex];
                        visibleChunk.reserve(end - begin);
                        shadowChunk.reserve(end - begin);

                        for (size_t renderableOffset = begin; renderableOffset < end; ++renderableOffset) {
                            const Index index = m_RenderableIndices[renderableOffset];
                            if (!IsIndexAlive(index)) {
                                continue;
                            }

                            const EntitySlot& slot = m_Entities[index];
                            if (!slot.Active || !slot.Visible || slot.PresentationHidden || !slot.HasMesh) {
                                continue;
                            }
                            if (enforceLayerMask && (((activeLayerMask >> (slot.Layer & 31u)) & 1u) == 0u)) {
                                continue;
                            }

                            RuntimeRenderEntityRef renderable{};
                            renderable.Handle = { index, slot.Generation };
                            renderable.SceneEntity = slot.SceneEntity;
                            visibleChunk.push_back(renderable);
                            if (slot.CastsShadows) {
                                shadowChunk.push_back(renderable);
                            }
                        }
                    }
                });
            }

            m_TaskGraph.Add({
                "RuntimeWorld/MergeRenderables",
                RuntimeTaskCategory::Extraction,
                0u,
                0u,
                false,
                false,
                std::move(renderableChunkTaskNames),
                [&visibleMeshEntities,
                 &shadowMeshEntities,
                 &visibleMeshChunks,
                 &shadowMeshChunks,
                 &mergeRenderEntityChunks]() {
                    mergeRenderEntityChunks(visibleMeshEntities, visibleMeshChunks);
                    mergeRenderEntityChunks(shadowMeshEntities, shadowMeshChunks);
                }
            });
        } else {
            Profiler::Get().SetCounter("RuntimeWorld/RenderExtractRenderableTasks", 1);
            m_TaskGraph.Add({
                "RuntimeWorld/ExtractRenderables",
                RuntimeTaskCategory::Extraction,
                DirtyBitsToStoreMask(RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::Metadata),
                0u,
                false,
                true,
                {},
                [this, activeLayerMask, enforceLayerMask, &visibleMeshEntities, &shadowMeshEntities]() {
                    visibleMeshEntities.reserve(m_RenderableIndices.size());
                    shadowMeshEntities.reserve(m_RenderableIndices.size());

                    for (Index index : m_RenderableIndices) {
                        if (!IsIndexAlive(index)) {
                            continue;
                        }

                        const EntitySlot& slot = m_Entities[index];
                        if (!slot.Active || !slot.Visible || slot.PresentationHidden || !slot.HasMesh) {
                            continue;
                        }
                        if (enforceLayerMask && (((activeLayerMask >> (slot.Layer & 31u)) & 1u) == 0u)) {
                            continue;
                        }

                        RuntimeRenderEntityRef renderable{};
                        renderable.Handle = { index, slot.Generation };
                        renderable.SceneEntity = slot.SceneEntity;
                        visibleMeshEntities.push_back(renderable);
                        if (slot.CastsShadows) {
                            shadowMeshEntities.push_back(renderable);
                        }
                    }
                }
            });
        }
    }

    if (rebuildTerrains) {
        const bool useChunkedTerrainExtraction =
            (cm::g_JobSystem != nullptr) &&
            (extractionWorkerCount > 1) &&
            (m_TerrainIndices.size() > 64);
        if (useChunkedTerrainExtraction) {
            const size_t chunkSize =
                ComputeOptimalChunkSize(m_TerrainIndices.size(), extractionWorkerCount, size_t{ 64 });
            const size_t chunkCount = (m_TerrainIndices.size() + chunkSize - 1) / chunkSize;
            Profiler::Get().SetCounter(
                "RuntimeWorld/RenderExtractTerrainTasks",
                static_cast<uint64_t>(chunkCount));
            visibleTerrainChunks.resize(chunkCount);
            shadowTerrainChunks.resize(chunkCount);
            std::vector<std::string> terrainChunkTaskNames;
            terrainChunkTaskNames.reserve(chunkCount);

            for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
                const size_t begin = chunkIndex * chunkSize;
                const size_t end = std::min(begin + chunkSize, m_TerrainIndices.size());
                terrainChunkTaskNames.push_back(
                    "RuntimeWorld/ExtractTerrainsChunk" + std::to_string(chunkIndex));
                m_TaskGraph.Add({
                    terrainChunkTaskNames.back(),
                    RuntimeTaskCategory::Extraction,
                    DirtyBitsToStoreMask(RuntimeDirtyBits::RenderBinding),
                    0u,
                    false,
                    true,
                    {},
                    [this, begin, end, chunkIndex, &visibleTerrainChunks, &shadowTerrainChunks]() {
                        auto& visibleChunk = visibleTerrainChunks[chunkIndex];
                        auto& shadowChunk = shadowTerrainChunks[chunkIndex];
                        visibleChunk.reserve(end - begin);
                        shadowChunk.reserve(end - begin);

                        for (size_t terrainOffset = begin; terrainOffset < end; ++terrainOffset) {
                            const Index index = m_TerrainIndices[terrainOffset];
                            if (!IsIndexAlive(index)) {
                                continue;
                            }

                            const EntitySlot& slot = m_Entities[index];
                            if (!slot.Active || !slot.Visible || slot.PresentationHidden || !slot.HasTerrain) {
                                continue;
                            }

                            RuntimeRenderEntityRef terrain{};
                            terrain.Handle = { index, slot.Generation };
                            terrain.SceneEntity = slot.SceneEntity;
                            visibleChunk.push_back(terrain);
                            if (slot.CastsShadows) {
                                shadowChunk.push_back(terrain);
                            }
                        }
                    }
                });
            }

            m_TaskGraph.Add({
                "RuntimeWorld/MergeTerrains",
                RuntimeTaskCategory::Extraction,
                0u,
                0u,
                false,
                false,
                std::move(terrainChunkTaskNames),
                [&visibleTerrainEntities,
                 &shadowTerrainEntities,
                 &visibleTerrainChunks,
                 &shadowTerrainChunks,
                 &mergeRenderEntityChunks]() {
                    mergeRenderEntityChunks(visibleTerrainEntities, visibleTerrainChunks);
                    mergeRenderEntityChunks(shadowTerrainEntities, shadowTerrainChunks);
                }
            });
        } else {
            Profiler::Get().SetCounter("RuntimeWorld/RenderExtractTerrainTasks", 1);
            m_TaskGraph.Add({
                "RuntimeWorld/ExtractTerrains",
                RuntimeTaskCategory::Extraction,
                DirtyBitsToStoreMask(RuntimeDirtyBits::RenderBinding),
                0u,
                false,
                true,
                {},
                [this, &visibleTerrainEntities, &shadowTerrainEntities]() {
                    visibleTerrainEntities.reserve(m_TerrainIndices.size());
                    shadowTerrainEntities.reserve(m_TerrainIndices.size());

                    for (Index index : m_TerrainIndices) {
                        if (!IsIndexAlive(index)) {
                            continue;
                        }

                        const EntitySlot& slot = m_Entities[index];
                        if (!slot.Active || !slot.Visible || slot.PresentationHidden || !slot.HasTerrain) {
                            continue;
                        }

                        RuntimeRenderEntityRef terrain{};
                        terrain.Handle = { index, slot.Generation };
                        terrain.SceneEntity = slot.SceneEntity;
                        visibleTerrainEntities.push_back(terrain);
                        if (slot.CastsShadows) {
                            shadowTerrainEntities.push_back(terrain);
                        }
                    }
                }
            });
        }
    }

    if (rebuildLights) {
        const bool useChunkedLightExtraction =
            (cm::g_JobSystem != nullptr) &&
            (extractionWorkerCount > 1) &&
            (m_LightIndices.size() > 32);
        if (useChunkedLightExtraction) {
            const size_t chunkSize =
                ComputeOptimalChunkSize(m_LightIndices.size(), extractionWorkerCount, size_t{ 32 });
            const size_t chunkCount = (m_LightIndices.size() + chunkSize - 1) / chunkSize;
            Profiler::Get().SetCounter(
                "RuntimeWorld/RenderExtractLightTasks",
                static_cast<uint64_t>(chunkCount));
            lightChunks.resize(chunkCount);
            std::vector<std::string> lightChunkTaskNames;
            lightChunkTaskNames.reserve(chunkCount);

            for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
                const size_t begin = chunkIndex * chunkSize;
                const size_t end = std::min(begin + chunkSize, m_LightIndices.size());
                lightChunkTaskNames.push_back(
                    "RuntimeWorld/ExtractLightsChunk" + std::to_string(chunkIndex));
                m_TaskGraph.Add({
                    lightChunkTaskNames.back(),
                    RuntimeTaskCategory::Extraction,
                    DirtyBitsToStoreMask(
                        RuntimeDirtyBits::Light |
                        RuntimeDirtyBits::RenderBinding |
                        RuntimeDirtyBits::TransformLocal |
                        RuntimeDirtyBits::TransformWorld),
                    0u,
                    false,
                    true,
                    {},
                    [this, begin, end, chunkIndex, &lightChunks]() {
                        auto& lightChunk = lightChunks[chunkIndex];
                        lightChunk.reserve(end - begin);

                        for (size_t lightOffset = begin; lightOffset < end; ++lightOffset) {
                            const Index index = m_LightIndices[lightOffset];
                            if (!IsIndexAlive(index)) {
                                continue;
                            }

                            const EntitySlot& slot = m_Entities[index];
                            if (!slot.Active || !slot.Visible || slot.PresentationHidden || !slot.HasLight) {
                                continue;
                            }

                            RuntimeLightEntry entry{};
                            entry.Handle = { index, slot.Generation };
                            entry.SceneEntity = slot.SceneEntity;
                            entry.Type = m_Lights[index].Type;
                            entry.Color = m_Lights[index].Color;
                            entry.Intensity = m_Lights[index].Intensity;
                            entry.Range = m_Lights[index].Range;
                            entry.SpotInnerAngleDegrees = m_Lights[index].SpotInnerAngleDegrees;
                            entry.SpotOuterAngleDegrees = m_Lights[index].SpotOuterAngleDegrees;
                            entry.Position = glm::vec3(m_WorldMatrices[index][3]);
                            entry.Direction = LightDirectionFromWorldMatrix(m_WorldMatrices[index]);
                            lightChunk.push_back(entry);
                        }
                    }
                });
            }

            m_TaskGraph.Add({
                "RuntimeWorld/MergeLights",
                RuntimeTaskCategory::Extraction,
                0u,
                0u,
                false,
                false,
                std::move(lightChunkTaskNames),
                [&lights, &lightChunks, &mergeLightChunks]() {
                    mergeLightChunks(lights, lightChunks);
                }
            });
        } else {
            Profiler::Get().SetCounter("RuntimeWorld/RenderExtractLightTasks", 1);
            m_TaskGraph.Add({
                "RuntimeWorld/ExtractLights",
                RuntimeTaskCategory::Extraction,
                DirtyBitsToStoreMask(RuntimeDirtyBits::Light | RuntimeDirtyBits::RenderBinding | RuntimeDirtyBits::TransformLocal | RuntimeDirtyBits::TransformWorld),
                0u,
                false,
                true,
                {},
                [this, &lights]() {
                    lights.reserve(m_LightIndices.size());

                    for (Index index : m_LightIndices) {
                        if (!IsIndexAlive(index)) {
                            continue;
                        }

                        const EntitySlot& slot = m_Entities[index];
                        if (!slot.Active || !slot.Visible || slot.PresentationHidden || !slot.HasLight) {
                            continue;
                        }

                        RuntimeLightEntry entry{};
                        entry.Handle = { index, slot.Generation };
                        entry.SceneEntity = slot.SceneEntity;
                        entry.Type = m_Lights[index].Type;
                        entry.Color = m_Lights[index].Color;
                        entry.Intensity = m_Lights[index].Intensity;
                        entry.Range = m_Lights[index].Range;
                        entry.SpotInnerAngleDegrees = m_Lights[index].SpotInnerAngleDegrees;
                        entry.SpotOuterAngleDegrees = m_Lights[index].SpotOuterAngleDegrees;
                        entry.Position = glm::vec3(m_WorldMatrices[index][3]);
                        entry.Direction = LightDirectionFromWorldMatrix(m_WorldMatrices[index]);
                        lights.push_back(entry);
                    }
                }
            });
        }
    }

    m_TaskGraph.Execute();

    for (Index index : m_ExternalWorldDirtyIndices) {
        if (!IsIndexAlive(index) || m_ExternalWorldDirty[index] == 0u) {
            continue;
        }
        ClearDirty(index, RuntimeDirtyBits::TransformWorld);
        m_ExternalWorldDirty[index] = 0u;
    }
    m_ExternalWorldDirtyIndices.clear();
    CompactTransformStageCandidates();

    bool renderWorldChanged = false;
    if (rebuildRenderables) {
        m_RenderWorld.VisibleMeshEntities = std::move(visibleMeshEntities);
        m_RenderWorld.ShadowMeshEntities = std::move(shadowMeshEntities);
        m_LastRenderableListFingerprint = renderableFingerprint;
        renderWorldChanged = true;
    }
    if (rebuildTerrains) {
        m_RenderWorld.VisibleTerrainEntities = std::move(visibleTerrainEntities);
        m_RenderWorld.ShadowTerrainEntities = std::move(shadowTerrainEntities);
        m_LastTerrainListFingerprint = terrainFingerprint;
        renderWorldChanged = true;
    }
    if (rebuildLights) {
        m_RenderWorld.Lights = std::move(lights);
        m_LastLightListFingerprint = lightFingerprint;
        renderWorldChanged = true;
    }
    if (renderWorldChanged) {
        m_RenderWorld.Version = ++m_GlobalVersion;
    }
    m_Stats.TaskCount = m_TaskGraph.GetLastStats().size();
    m_Stats.BarrierCount = m_TaskGraph.GetBarrierCount();

    Profiler::Get().SetCounter("RuntimeWorld/RenderablesExtracted", static_cast<uint64_t>(m_RenderWorld.VisibleMeshEntities.size()));
    Profiler::Get().SetCounter("RuntimeWorld/TerrainsExtracted", static_cast<uint64_t>(m_RenderWorld.VisibleTerrainEntities.size()));
    Profiler::Get().SetCounter("RuntimeWorld/LightsExtracted", static_cast<uint64_t>(m_RenderWorld.Lights.size()));

    UpdateStats();
}

void RuntimeWorld::ApplyCommandBuffer(Scene* scene, const RuntimeCommandBuffer& commandBuffer) {
    for (const RuntimeCommandBuffer::Command& command : commandBuffer.m_Commands) {
        if (!IsHandleAlive(command.Handle)) {
            continue;
        }

        const Index index = command.Handle.Index;
        EntitySlot& slot = m_Entities[index];
        EntityData* data = scene ? scene->GetEntityData(slot.SceneEntity) : nullptr;

        switch (command.Type) {
        case RuntimeCommandBuffer::CommandType::Destroy:
            if (scene) {
                scene->QueueRemoveEntity(slot.SceneEntity);
            }
            ReleaseSlot(index);
            m_HierarchyDirty = true;
            m_PersistentIndicesDirty = true;
            m_SkinningCachesDirty = true;
            break;

        case RuntimeCommandBuffer::CommandType::SetActive:
            if (slot.Active != command.BoolValue) {
                if (slot.Active && m_ActiveEntityCount > 0) {
                    --m_ActiveEntityCount;
                } else if (!slot.Active) {
                    ++m_ActiveEntityCount;
                }
                slot.Active = command.BoolValue;
                MarkDirty(index, RuntimeDirtyBits::Visibility);
                if (data) {
                    data->Active = command.BoolValue;
                }
            }
            break;

        case RuntimeCommandBuffer::CommandType::SetVisibility:
            if (slot.Visible != command.BoolValue) {
                slot.Visible = command.BoolValue;
                MarkDirty(index, RuntimeDirtyBits::Visibility);
                if (data) {
                    data->Visible = command.BoolValue;
                }
            }
            break;

        case RuntimeCommandBuffer::CommandType::SetPresentationHidden:
            if (slot.PresentationHidden != command.BoolValue) {
                slot.PresentationHidden = command.BoolValue;
                MarkDirty(index, RuntimeDirtyBits::Visibility);
                if (data) {
                    data->PresentationHidden = command.BoolValue;
                }
            }
            break;

        case RuntimeCommandBuffer::CommandType::SetLocalTransform:
            m_LocalTransforms[index] = command.Transform;
            m_LocalMatrices[index] = BuildLocalMatrix(command.Transform);
            MarkDirty(index, RuntimeDirtyBits::TransformLocal);
            if (data) {
                data->Transform.Position = command.Transform.Position;
                data->Transform.Rotation = command.Transform.EulerDegrees;
                data->Transform.RotationQ = command.Transform.Rotation;
                data->Transform.Scale = command.Transform.Scale;
                data->Transform.UseQuatRotation = command.Transform.UseQuatRotation;
                if (scene) {
                    scene->MarkTransformDirty(slot.SceneEntity);
                } else {
                    data->Transform.TransformDirty = true;
                }
            }
            break;
        }
    }

    UpdateStats();
}

void RuntimeWorld::UpdateStats() {
    m_Stats.EntityCount = m_AliveEntityCount;
    m_Stats.ActiveEntityCount = m_ActiveEntityCount;
    m_Stats.RenderableCount = m_RenderableIndices.size();
    m_Stats.TerrainCount = m_TerrainIndices.size();
    m_Stats.LightCount = m_LightIndices.size();
    m_Stats.CameraCount = m_CameraIndices.size();
    m_Stats.AudioSourceCount = m_AudioSourceIndices.size();
    m_Stats.AudioListenerCount = m_AudioListenerIndices.size();
    m_Stats.EmitterCount = m_EmitterIndices.size();
    m_Stats.ScriptedCount = m_ScriptedIndices.size();
    m_Stats.AnimationRootCount = m_AnimationRootIndices.size();
    m_Stats.SkinnedMeshCount = m_SkinnedMeshIndices.size();
    m_Stats.PhysicsSyncCount = m_PhysicsIndices.size();
    m_Stats.HierarchyLevels = (m_LevelOffsets.size() > 1) ? (m_LevelOffsets.size() - 1) : 0;
    m_Stats.DirtyEntityCount = m_DirtyEntityCount;

    Profiler::Get().SetCounter("RuntimeWorld/Entities", static_cast<uint64_t>(m_Stats.EntityCount));
    Profiler::Get().SetCounter("RuntimeWorld/ActiveEntities", static_cast<uint64_t>(m_Stats.ActiveEntityCount));
    Profiler::Get().SetCounter("RuntimeWorld/DirtyEntities", static_cast<uint64_t>(m_Stats.DirtyEntityCount));
    Profiler::Get().SetCounter("RuntimeWorld/Renderables", static_cast<uint64_t>(m_Stats.RenderableCount));
    Profiler::Get().SetCounter("RuntimeWorld/Terrains", static_cast<uint64_t>(m_Stats.TerrainCount));
    Profiler::Get().SetCounter("RuntimeWorld/Lights", static_cast<uint64_t>(m_Stats.LightCount));
    Profiler::Get().SetCounter("RuntimeWorld/Cameras", static_cast<uint64_t>(m_Stats.CameraCount));
    Profiler::Get().SetCounter("RuntimeWorld/AudioSources", static_cast<uint64_t>(m_Stats.AudioSourceCount));
    Profiler::Get().SetCounter("RuntimeWorld/AudioListeners", static_cast<uint64_t>(m_Stats.AudioListenerCount));
    Profiler::Get().SetCounter("RuntimeWorld/Emitters", static_cast<uint64_t>(m_Stats.EmitterCount));
    Profiler::Get().SetCounter("RuntimeWorld/Scripted", static_cast<uint64_t>(m_Stats.ScriptedCount));
    Profiler::Get().SetCounter("RuntimeWorld/AnimationRoots", static_cast<uint64_t>(m_Stats.AnimationRootCount));
    Profiler::Get().SetCounter("RuntimeWorld/SkinnedMeshes", static_cast<uint64_t>(m_Stats.SkinnedMeshCount));
    Profiler::Get().SetCounter("RuntimeWorld/PhysicsSync", static_cast<uint64_t>(m_Stats.PhysicsSyncCount));
    Profiler::Get().SetCounter("RuntimeWorld/HierarchyLevels", static_cast<uint64_t>(m_Stats.HierarchyLevels));
    Profiler::Get().SetCounter("RuntimeWorld/TaskCount", static_cast<uint64_t>(m_Stats.TaskCount));
    Profiler::Get().SetCounter("RuntimeWorld/BarrierCount", static_cast<uint64_t>(m_Stats.BarrierCount));
    Profiler::Get().SetCounter("RuntimeWorld/FullRebuilds", static_cast<uint64_t>(m_Stats.FullRebuildCount));
    Profiler::Get().SetCounter("RuntimeWorld/IncrementalSyncs", static_cast<uint64_t>(m_Stats.IncrementalSyncCount));
}

} // namespace cm::world
