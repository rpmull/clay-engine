#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/assets/AssetReference.h"
#include "core/ecs/Entity.h"
#include "core/ecs/Components.h"

class Scene;

namespace cm::world {

struct RuntimeEntityHandle {
    static constexpr uint32_t InvalidIndex = std::numeric_limits<uint32_t>::max();

    uint32_t Index = InvalidIndex;
    uint32_t Generation = 0;

    [[nodiscard]] bool IsValid() const { return Index != InvalidIndex; }
    [[nodiscard]] explicit operator bool() const { return IsValid(); }
    [[nodiscard]] bool operator==(const RuntimeEntityHandle& other) const {
        return Index == other.Index && Generation == other.Generation;
    }
};

enum class RuntimeDirtyBits : uint32_t {
    None = 0,
    Metadata = 1u << 0,
    TransformLocal = 1u << 1,
    Hierarchy = 1u << 2,
    Visibility = 1u << 3,
    RenderBinding = 1u << 4,
    Light = 1u << 5,
    Bounds = 1u << 6,
    Lifetime = 1u << 7,
    TransformWorld = 1u << 8,
    All = 0xFFFFFFFFu
};

inline RuntimeDirtyBits operator|(RuntimeDirtyBits a, RuntimeDirtyBits b) {
    return static_cast<RuntimeDirtyBits>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline RuntimeDirtyBits operator&(RuntimeDirtyBits a, RuntimeDirtyBits b) {
    return static_cast<RuntimeDirtyBits>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline RuntimeDirtyBits& operator|=(RuntimeDirtyBits& a, RuntimeDirtyBits b) {
    a = a | b;
    return a;
}

inline RuntimeDirtyBits& operator&=(RuntimeDirtyBits& a, RuntimeDirtyBits b) {
    a = a & b;
    return a;
}

inline RuntimeDirtyBits operator~(RuntimeDirtyBits bits) {
    return static_cast<RuntimeDirtyBits>(~static_cast<uint32_t>(bits));
}

[[nodiscard]] inline bool HasAny(RuntimeDirtyBits value, RuntimeDirtyBits mask) {
    return (static_cast<uint32_t>(value & mask) != 0u);
}

enum class RuntimeSyncReason : uint8_t {
    SceneUpdate,
    Render,
    Bootstrap,
    FullRebuild
};

enum class RuntimeTaskCategory : uint8_t {
    Simulation,
    Extraction,
    Streaming,
    BackgroundPreparation,
    MainThreadOnly
};

struct RuntimeTaskStats {
    std::string Name;
    RuntimeTaskCategory Category = RuntimeTaskCategory::Simulation;
    double DurationMs = 0.0;
    uint32_t DependencyCount = 0;
    bool Parallelized = false;
    bool MainThreadOnly = false;
};

struct RuntimeLocalTransform {
    glm::vec3 Position{ 0.0f };
    glm::vec3 EulerDegrees{ 0.0f };
    glm::quat Rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 Scale{ 1.0f };
    bool UseQuatRotation = true;
};

struct RuntimeBounds {
    bool Valid = false;
    glm::vec3 LocalMin{ 0.0f };
    glm::vec3 LocalMax{ 0.0f };
    glm::vec3 WorldMin{ 0.0f };
    glm::vec3 WorldMax{ 0.0f };
};

struct RuntimeLightEntry {
    RuntimeEntityHandle Handle{};
    EntityID SceneEntity = INVALID_ENTITY_ID;
    LightType Type = LightType::Directional;
    glm::vec3 Color{ 1.0f };
    float Intensity = 1.0f;
    glm::vec3 Position{ 0.0f };
    glm::vec3 Direction{ 0.0f, -1.0f, 0.0f };
};

struct RuntimeRenderEntityRef {
    RuntimeEntityHandle Handle{};
    EntityID SceneEntity = INVALID_ENTITY_ID;
};

struct RuntimeRenderWorld {
    uint64_t Version = 0;
    std::vector<RuntimeRenderEntityRef> VisibleMeshEntities;
    std::vector<RuntimeRenderEntityRef> ShadowMeshEntities;
    std::vector<RuntimeRenderEntityRef> VisibleTerrainEntities;
    std::vector<RuntimeRenderEntityRef> ShadowTerrainEntities;
    std::vector<RuntimeLightEntry> Lights;
};

struct RuntimeWorldStats {
    uint64_t SceneSyncVersion = 0;
    uint64_t TransformVersion = 0;
    uint64_t HierarchyVersion = 0;
    uint64_t RenderBindingVersion = 0;
    uint64_t LightVersion = 0;
    uint64_t BoundsVersion = 0;
    uint64_t MetadataVersion = 0;
    size_t EntityCount = 0;
    size_t ActiveEntityCount = 0;
    size_t RenderableCount = 0;
    size_t TerrainCount = 0;
    size_t LightCount = 0;
    size_t CameraCount = 0;
    size_t AudioSourceCount = 0;
    size_t AudioListenerCount = 0;
    size_t EmitterCount = 0;
    size_t ScriptedCount = 0;
    size_t AnimationRootCount = 0;
    size_t SkinnedMeshCount = 0;
    size_t PhysicsSyncCount = 0;
    size_t HierarchyLevels = 0;
    size_t DirtyEntityCount = 0;
    size_t FullRebuildCount = 0;
    size_t IncrementalSyncCount = 0;
    size_t TaskCount = 0;
    size_t BarrierCount = 0;
};

struct RuntimeSkinningGroupCache {
    RuntimeEntityHandle SkeletonHandle{};
    EntityID SkeletonSceneEntity = INVALID_ENTITY_ID;
    std::vector<RuntimeEntityHandle> BoneHandles;
    std::vector<EntityID> BoneSceneEntities;
    std::vector<RuntimeEntityHandle> MeshHandles;
    std::vector<EntityID> MeshSceneEntities;
};

class RuntimeCommandBuffer {
public:
    void Clear();
    void Destroy(RuntimeEntityHandle handle);
    void SetActive(RuntimeEntityHandle handle, bool active);
    void SetVisibility(RuntimeEntityHandle handle, bool visible);
    void SetPresentationHidden(RuntimeEntityHandle handle, bool hidden);
    void SetLocalTransform(RuntimeEntityHandle handle, const RuntimeLocalTransform& transform);
    [[nodiscard]] bool Empty() const { return m_Commands.empty(); }

private:
    friend class RuntimeWorld;

    enum class CommandType : uint8_t {
        Destroy,
        SetActive,
        SetVisibility,
        SetPresentationHidden,
        SetLocalTransform
    };

    struct Command {
        CommandType Type = CommandType::Destroy;
        RuntimeEntityHandle Handle{};
        RuntimeLocalTransform Transform{};
        bool BoolValue = false;
    };

    std::vector<Command> m_Commands;
};

class RuntimeTaskGraph {
public:
    struct TaskDecl {
        std::string Name;
        RuntimeTaskCategory Category = RuntimeTaskCategory::Simulation;
        uint64_t ReadMask = 0;
        uint64_t WriteMask = 0;
        bool MainThreadOnly = false;
        bool AllowParallel = true;
        std::vector<std::string> After;
        std::function<void()> Callback;
    };

    void Clear();
    void Add(TaskDecl decl);
    void Execute();

    [[nodiscard]] size_t GetBarrierCount() const { return m_LastBarrierCount; }
    [[nodiscard]] const std::vector<RuntimeTaskStats>& GetLastStats() const { return m_LastStats; }

private:
    std::vector<TaskDecl> m_Tasks;
    size_t m_LastBarrierCount = 0;
    std::vector<RuntimeTaskStats> m_LastStats;
};

class RuntimeWorld {
public:
    RuntimeWorld();
    RuntimeWorld(RuntimeWorld&& other) noexcept;
    RuntimeWorld& operator=(RuntimeWorld&& other) noexcept;
    RuntimeWorld(const RuntimeWorld&) = delete;
    RuntimeWorld& operator=(const RuntimeWorld&) = delete;
    ~RuntimeWorld() = default;

    void Reset();
    void InvalidateAll();
    void InvalidateEntity(EntityID sceneEntity, RuntimeDirtyBits bits = RuntimeDirtyBits::All);

    void SyncFromScene(Scene& scene,
                       RuntimeSyncReason reason = RuntimeSyncReason::SceneUpdate,
                       bool forceFullRebuild = false);
    void UpdateTransforms(Scene& scene);
    void FinalizeTransformStage(Scene& scene);
    void BuildRenderWorld(Scene& scene, uint32_t activeLayerMask, bool enforceLayerMask);
    void ApplyCommandBuffer(Scene* scene, const RuntimeCommandBuffer& commandBuffer);

    [[nodiscard]] RuntimeCommandBuffer CreateCommandBuffer() const { return RuntimeCommandBuffer{}; }

    [[nodiscard]] bool IsHandleAlive(RuntimeEntityHandle handle) const;
    [[nodiscard]] RuntimeEntityHandle TryGetHandle(EntityID sceneEntity) const;
    [[nodiscard]] EntityID ResolveSceneEntity(RuntimeEntityHandle handle) const;
    [[nodiscard]] const glm::mat4* TryGetWorldMatrix(RuntimeEntityHandle handle) const;
    [[nodiscard]] const glm::mat4* TryGetWorldMatrix(EntityID sceneEntity) const;
    [[nodiscard]] const RuntimeBounds* TryGetBounds(RuntimeEntityHandle handle) const;
    [[nodiscard]] const RuntimeSkinningGroupCache* TryGetSkinningGroupCache(EntityID skeletonSceneEntity) const;
    [[nodiscard]] bool TryGetSkinningGroupWorldBounds(const RuntimeSkinningGroupCache& cache,
                                                      glm::vec3& outMin,
                                                      glm::vec3& outMax) const;
    [[nodiscard]] bool TryGetSkinningGroupMaxWorldExtent(const RuntimeSkinningGroupCache& cache,
                                                         glm::vec3& outMaxExtent) const;
    void ApplyWorldTransformOverride(EntityID sceneEntity, const glm::mat4& worldMatrix);

    [[nodiscard]] const RuntimeRenderWorld& GetRenderWorld() const { return m_RenderWorld; }
    [[nodiscard]] const RuntimeWorldStats& GetStats() const { return m_Stats; }
    [[nodiscard]] const RuntimeTaskGraph& GetTaskGraph() const { return m_TaskGraph; }
    [[nodiscard]] const std::vector<RuntimeSkinningGroupCache>& GetSkinningGroupCaches() const { return m_SkinningGroupCaches; }
    [[nodiscard]] const std::vector<EntityID>& GetRenderableSceneEntities() const { return m_RenderableSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetLightSceneEntities() const { return m_LightSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetTerrainSceneEntities() const { return m_TerrainSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetCameraSceneEntities() const { return m_CameraSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetAudioSourceSceneEntities() const { return m_AudioSourceSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetAudioListenerSceneEntities() const { return m_AudioListenerSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetEmitterSceneEntities() const { return m_EmitterSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetScriptedSceneEntities() const { return m_ScriptedSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetAnimationRootSceneEntities() const { return m_AnimationRootSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetSkinnedMeshSceneEntities() const { return m_SkinnedMeshSceneEntities; }
    [[nodiscard]] const std::vector<EntityID>& GetPhysicsSceneEntities() const { return m_PhysicsSceneEntities; }

private:
    using Index = uint32_t;
    static constexpr Index InvalidIndex = RuntimeEntityHandle::InvalidIndex;

    struct EntitySlot {
        EntityID SceneEntity = INVALID_ENTITY_ID;
        ClaymoreGUID Guid{};
        uint32_t Generation = 1;
        uint32_t Layer = 0;
        uint32_t Depth = 0;
        Index Parent = InvalidIndex;
        RuntimeDirtyBits DirtyMask = RuntimeDirtyBits::All;
        uint64_t DirtyVersion = 0;
        bool Alive = false;
        bool Active = true;
        bool Visible = true;
        bool PresentationHidden = false;
        bool HasMesh = false;
        bool HasTerrain = false;
        bool HasLight = false;
        bool HasCamera = false;
        bool HasAudioSource = false;
        bool HasAudioListener = false;
        bool HasEmitter = false;
        bool HasScripts = false;
        bool HasAnimationRoot = false;
        bool HasSkinning = false;
        bool HasPhysicsSync = false;
        bool CastsShadows = true;
        bool SkipFrustumCulling = false;
        float BoundsPadding = 1.0f;
        std::string Name;
        std::string Tag;
    };

    struct LightState {
        bool Enabled = false;
        LightType Type = LightType::Directional;
        glm::vec3 Color{ 1.0f };
        float Intensity = 1.0f;
    };

    Index AcquireSlot(EntityID sceneEntity);
    void ReleaseSlot(Index index);
    void EnsureStorageSize(size_t size);
    void RebuildSceneOrder(const Scene& scene);
    void RefreshParentLinks(Scene& scene);
    void RebuildHierarchy();
    void RebuildPersistentIndices();
    void RebuildSkinningCaches(Scene& scene);
    void QueueTransformStageCandidate(Index index);
    void CompactTransformStageCandidates();
    void MarkExternalWorldDirty(Index index);
    void RecomputeBounds();
    void PropagateExternalWorldOverrides(Scene& scene);
    void SyncQueuedTransformsFromScene(Scene& scene);
    void SyncEntityFromScene(Scene& scene, EntityID sceneEntity, bool& structuralChange);
    void WriteBackSceneTransforms(Scene& scene);
    void MarkDirty(Index index, RuntimeDirtyBits bits);
    void ClearDirty(Index index, RuntimeDirtyBits bits);
    void UpdateStats();

    [[nodiscard]] bool IsIndexAlive(Index index) const;
    [[nodiscard]] bool IsTransformStageDirty(Index index) const;

    std::vector<EntitySlot> m_Entities;
    std::vector<RuntimeLocalTransform> m_LocalTransforms;
    std::vector<glm::mat4> m_LocalMatrices;
    std::vector<glm::mat4> m_WorldMatrices;
    std::vector<RuntimeBounds> m_Bounds;
    std::vector<LightState> m_Lights;
    std::vector<uint8_t> m_TransformDirty;
    std::vector<uint8_t> m_TransformStageQueued;
    std::vector<uint8_t> m_RecomputedThisPass;
    std::vector<uint8_t> m_ExternalWorldDirty;
    std::vector<Index> m_TransformStageCandidates;
    std::vector<Index> m_ExternalWorldDirtyIndices;

    std::unordered_map<EntityID, Index> m_SceneToIndex;
    std::vector<Index> m_FreeIndices;
    std::vector<Index> m_SceneOrder;
    std::vector<Index> m_LevelOrder;
    std::vector<size_t> m_LevelOffsets;
    std::vector<Index> m_ChildIndices;
    std::vector<size_t> m_ChildOffsets;
    std::vector<size_t> m_ChildCounts;

    std::vector<Index> m_RenderableIndices;
    std::vector<Index> m_TerrainIndices;
    std::vector<Index> m_LightIndices;
    std::vector<Index> m_CameraIndices;
    std::vector<Index> m_AudioSourceIndices;
    std::vector<Index> m_AudioListenerIndices;
    std::vector<Index> m_EmitterIndices;
    std::vector<Index> m_ScriptedIndices;
    std::vector<Index> m_AnimationRootIndices;
    std::vector<Index> m_SkinnedMeshIndices;
    std::vector<Index> m_PhysicsIndices;
    std::vector<EntityID> m_RenderableSceneEntities;
    std::vector<EntityID> m_TerrainSceneEntities;
    std::vector<EntityID> m_LightSceneEntities;
    std::vector<EntityID> m_CameraSceneEntities;
    std::vector<EntityID> m_AudioSourceSceneEntities;
    std::vector<EntityID> m_AudioListenerSceneEntities;
    std::vector<EntityID> m_EmitterSceneEntities;
    std::vector<EntityID> m_ScriptedSceneEntities;
    std::vector<EntityID> m_AnimationRootSceneEntities;
    std::vector<EntityID> m_SkinnedMeshSceneEntities;
    std::vector<EntityID> m_PhysicsSceneEntities;
    std::vector<RuntimeSkinningGroupCache> m_SkinningGroupCaches;
    std::unordered_map<EntityID, size_t> m_SkinningGroupCacheLookup;

    RuntimeRenderWorld m_RenderWorld;
    RuntimeTaskGraph m_TaskGraph;
    RuntimeWorldStats m_Stats;

    uint64_t m_GlobalVersion = 1;
    uint64_t m_LastRenderableListFingerprint = 0;
    uint64_t m_LastTerrainListFingerprint = 0;
    uint64_t m_LastLightListFingerprint = 0;
    size_t m_AliveEntityCount = 0;
    size_t m_ActiveEntityCount = 0;
    size_t m_DirtyEntityCount = 0;
    bool m_HierarchyDirty = true;
    bool m_PersistentIndicesDirty = true;
    bool m_SkinningCachesDirty = true;
};

} // namespace cm::world
