#pragma once
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <atomic>
#include <chrono>
#include "Entity.h"
#include "EntityData.h"
#include "Components.h"
#include "ModuleComponent.h"
#include "core/rendering/Camera.h"
#include "core/rendering/Environment.h"
#include "core/navigation/Navigation.h"
#include "core/utils/TypeId.h"

namespace cm::world {
class RuntimeWorld;
struct RuntimeRenderWorld;
enum class RuntimeDirtyBits : uint32_t;
enum class RuntimeSyncReason : uint8_t;
}

// Forward declare ModelLoader (editor-only, used conditionally)
class ModelLoader;

class Scene {
public:
   enum class ShaderPreset { PBR = 0, PSX = 1 };
   enum class BoneAttachmentProcessMode {
      All,
      ExcludeActiveRagdolls,
      OnlyActiveRagdolls
   };
   
   Scene();
   ~Scene();
   
   // Non-copyable (EntityData contains unique_ptrs)
   Scene(const Scene&) = delete;
   Scene& operator=(const Scene&) = delete;
   
   // Movable (defined in .cpp to handle incomplete AsyncLoadJob type)
   Scene(Scene&&) noexcept;
   Scene& operator=(Scene&&) noexcept;
   
   // Returns true if the scene is currently being destroyed (in destructor)
   // Scripts should NOT destroy entities when this is true
   bool IsBeingDestroyed() const { return m_IsBeingDestroyed; }

   struct EditorViewportState {
       glm::vec3 Target{ 0.0f };
       float Yaw = 0.0f;
       float Pitch = 0.0f;
       float Distance = 10.0f;
       float FieldOfView = 60.0f;
       float NearClip = 0.1f;
       float FarClip = 1000.0f;
   };
   
    static Scene* CurrentScene;
	static Scene& Get() {
		if (!CurrentScene)
			CurrentScene = new Scene();
		return *CurrentScene;
	}

   Entity CreateEntity(const std::string& name = "Entity");
   // Create an entity preserving the exact provided name (no suffixing). For deserialization.
   Entity CreateEntityExact(const std::string& name);
   // Fast path for runtime/deserialization: skips dirty + hierarchy cache invalidation.
   Entity CreateEntityExactFast(const std::string& name);
   void RemoveEntity(EntityID id);

   EntityData* GetEntityData(EntityID id);
   Entity FindEntityByID(EntityID id);
   
   /// Find entity by its GUID (returns INVALID_ENTITY_ID if not found)
   EntityID FindEntityByGUID(const ClaymoreGUID& guid) const;
   
   /// Find entity by hierarchical path (e.g., "Parent/Child/GrandChild")
   EntityID FindEntityByPath(const std::string& path) const;

   const std::vector<Entity>& GetEntities() const { return m_EntityList; }

   Entity CreateLight(const std::string& name, LightType type, const glm::vec3& color, float intensity);

   EntityID InstantiateAsset(const std::string& path, const glm::vec3& position);
   EntityID InstantiateModel(const std::string& path, const glm::vec3& rootPosition, EntityID existingRoot = INVALID_ENTITY_ID);
   // Fast path for models imported via cached binaries (.meta/.meshbin/.skelbin)
   // Optional synchronous flag for deterministic construction (used by deserialization)
   EntityID InstantiateModelFast(const std::string& metaPath, const glm::vec3& position, bool synchronous = false, EntityID existingRoot = INVALID_ENTITY_ID);

   void DestroyEntity(Entity e) {RemoveEntity(e.GetID());}

   void SetParent(EntityID child, EntityID parent, bool preserveWorldTransform = false);
   void SetChild(EntityID parent, EntityID child, bool preserveWorldTransform = false) {SetParent(child, parent, preserveWorldTransform);} 
   // Fast parent assignment for newly created entities (no cycles, no old parent).
   void SetParentFast(EntityID child, EntityID parent);

   // Reorder an entity relative to a sibling (same parent). Returns true if moved.
   bool ReorderEntity(EntityID entity, EntityID targetSibling, bool placeAfter);

   // Duplicate an entity and its entire subtree. Returns the new root entity id or -1 on failure.
   EntityID DuplicateEntity(EntityID srcRoot);
   

   // Set entity visibility, and if entity has Panel+LayoutGroup, propagate to all children
   void SetEntityVisible(EntityID id, bool visible);
   void SetEntityVisibleDirect(EntityID id, bool visible);
   void SetEntityPresentationHidden(EntityID id, bool hidden);
   void SetEntityActive(EntityID id, bool active);
   void SetEntityName(EntityID id, const std::string& name);
   void SetEntityTag(EntityID id, const std::string& tag);
   void SetEntityLayer(EntityID id, int layer);
   void SetMeshBoundsPadding(EntityID id, float boundsPadding);
   void SetMeshSkipFrustumCulling(EntityID id, bool skipFrustumCulling);

   // Transform Updates
   void UpdateTransforms();
   void FinalizeRuntimeTransforms();
   bool ProcessBoneAttachments(BoneAttachmentProcessMode mode = BoneAttachmentProcessMode::All);  // Update entities with BoneAttachmentComponent
   void MarkBoneAttachmentCacheDirty();
   void NotifyAnimationPosePaletteChanged();
   void TopologicalSortEntities(std::vector<EntityID>& outSorted);
   void SetPosition(EntityID id, const glm::vec3& pos);
   void MarkTransformDirty(EntityID id);
   void NotifyTransformChanged(EntityID id);
   void NotifyComponentChanged(EntityID id, cm::world::RuntimeDirtyBits bits);
   void NotifyWorldTransformOverride(EntityID id);
   void ResolveScriptEntityReferencesFromMetadata();
   cm::world::RuntimeWorld& EnsureRuntimeWorld();
   const cm::world::RuntimeWorld* GetRuntimeWorld() const { return m_RuntimeWorldBridge.get(); }
   void SyncRuntimeWorld(bool forceFullRebuild = false);
   [[nodiscard]] bool HasPendingRuntimeWorldStructuralSyncWork() const;
   [[nodiscard]] bool HasPendingRuntimeWorldSyncWork() const;
   [[nodiscard]] bool IsRuntimeWorldFrameSyncLocked() const { return m_RuntimeWorldFrameSyncLocked; }
   const cm::world::RuntimeRenderWorld& BuildRuntimeRenderWorld(uint32_t activeLayerMask = 0xFFFFFFFFu,
                                                                bool enforceLayerMask = false);

   std::shared_ptr<Scene> RuntimeClone();
   
   // Recreate all script instances with fresh types from the current assembly.
   // Used after hot-reload to ensure script instances use types from the new assembly.
   // Returns the number of script instances recreated.
   int RecreateScriptInstances();

   std::shared_ptr<Scene> m_EditScene;
   std::shared_ptr<Scene> m_RuntimeScene;
   bool m_IsPlaying = false;
    bool m_IsPaused = false;

   std::unordered_map<EntityID, JPH::BodyID> m_BodyMap;

   void CreatePhysicsBody(EntityID id, const TransformComponent&, const ColliderComponent&);
   void DestroyPhysicsBody(EntityID id);
   bool SyncPhysicsBodyFromSceneTransform(EntityID id, bool resetRigidBodyVelocity);
   // Clear runtime-only physics handles (bodies/controllers/areas) from this scene.
   // Used before cloning into play mode to prevent duplicate colliders in the global physics world.
   void ReleasePhysicsRuntimeState();

   void Update(float dt);
   
   // Propagate all UnifiedMorph weights to child mesh BlendShapeComponents.
   // Uses parallel-for when there are multiple meshes. Call after bulk weight changes.
   void PropagateUnifiedMorphWeights(EntityID unifiedMorphEntity);

   void OnStop();
   // Clear runtime animator clip caches for this scene and its runtime clone.
   // Use when animation assets are reimported/hot-reloaded so entities pick up new data.
   void InvalidateAllAnimatorAssetCaches();

   bool HasComponent(const char* componentName);

   // Dirty-state API for editor serialization tracking
   void MarkDirty() {
      m_IsDirty = true;
      ++m_DirtyRevision;
      m_PhysicsParticipantCacheDirty = true;
      m_BoneAttachmentEntitiesDirty = true;
      if (!m_IsPlaying) {
         m_RuntimeWorldBridgeFullSyncPending = true;
         ++m_RuntimeWorldBridgeRevision;
      }
   }
   void MarkEntityStructureDirty(EntityID id);
   void ClearDirty() { m_IsDirty = false; }
   bool IsDirty() const { return m_IsDirty; }
   uint64_t GetDirtyRevision() const { return m_DirtyRevision; }

   Camera* GetActiveCamera();
   EntityID GetActiveCameraEntityID();
    Environment& GetEnvironment() { return m_Environment; }
    const Environment& GetEnvironment() const { return m_Environment; }

    // Scene-wide default shader preset
    void SetDefaultShaderPreset(ShaderPreset preset) { m_DefaultShaderPreset = preset; }
    ShaderPreset GetDefaultShaderPreset() const { return m_DefaultShaderPreset; }

    // Editor viewport state API
   void SetEditorViewportState(const EditorViewportState& state);
   void ClearEditorViewportState();
   bool HasEditorViewportState() const { return m_HasEditorViewportState; }
   const EditorViewportState& GetEditorViewportState() const { return m_EditorViewportState; }

    // Deferred deletion API to avoid mid-render invalidation
    void QueueRemoveEntity(EntityID id);
    void ProcessPendingRemovals();
    bool HasPendingEntityRemovals() const { return !m_PendingRemovals.empty(); }
    
    // Deferred creation API to avoid iterator invalidation during iteration
    EntityID QueueCreateEntity(const std::string& name, EntityID parentID = INVALID_ENTITY_ID);
    void ProcessPendingCreations();
    bool HasPendingEntityCreations() const { return !m_PendingCreations.empty(); }
    
    // Model node deletion tracking: removes entity and tracks it in model root's DeletedModelNodes
    // Returns true if the entity was a model child and was tracked for stable deletion
    bool QueueRemoveModelChild(EntityID id);

    /// Reset all children and deep children of a model root to their model-default transforms
    /// (from the latest import). Keeps the model root's transform unchanged.
    /// Returns true on success. Editor-only; no-op in runtime builds.
    bool ResetModelChildrenToDefault(EntityID modelRootId);
    
    /// For a model root, vertically aligns each direct child subtree onto terrain.
    /// Uses each child subtree's world AABB min Y and samples terrain at AABB center X/Z.
    /// Returns true if at least one child was adjusted. Editor-only; no-op in runtime builds.
    bool AlignModelRootChildrenToTerrain(EntityID modelRootId);

    // Reset monotonically increasing ID counter (editor-only usage before full deserialization)
    void ResetEntityIdCounter(EntityID next = 1) {
       m_NextID = next;
       EnsureRuntimeWorldTransformTrackingCapacity(static_cast<size_t>(m_NextID));
    }

    // Dynamic module component API
    bool AddDynamicComponent(EntityID id, const cm::TypeId& typeId);
    bool RemoveDynamicComponent(EntityID id, const cm::TypeId& typeId);
    cm::ModuleComponent* GetDynamicComponent(EntityID id, const cm::TypeId& typeId);

   // Collect entities that should persist across scene loads (play mode only).
   // Returns full subtree of any root that contains a DontDestroyOnLoad script.
   std::unordered_set<EntityID> CollectPersistentEntities();

   // Scene loading helpers (used by managed SceneManager)
   bool LoadSceneImmediate(const std::string& path, bool async = false);
   void RequestSceneLoad(const std::string& path);
   void RequestSceneLoad(const std::string& path, bool async);
   bool HasPendingSceneLoad() const { return m_PendingSceneLoad; }

   // Scene loading state (progress + status)
   float GetLoadProgress() const { return m_LoadProgress; }
   bool IsLoading() const { return m_Loading; }
   bool IsLoaded() const { return m_Loaded; }

   // Path of the currently loaded scene (e.g. "scenes/TestScene.scene" or "assets/scenes/Main.sceneb")
   const std::string& GetScenePath() const { return m_ScenePath; }
   void SetScenePath(const std::string& path) { m_ScenePath = path; }

    // Invalidate cached hierarchy levels (call after parent/child changes)
    void InvalidateHierarchyCache() { m_HierarchyDirty = true; }

private:
   friend class cm::world::RuntimeWorld;

   std::unordered_map<EntityID, EntityData> m_Entities;
   std::vector<Entity> m_EntityList;
   EntityID m_NextID = 1;
    Environment m_Environment{};
    std::vector<EntityID> m_PendingRemovals;
    
    // Pending creations: (assigned ID, name, parent ID)
    struct PendingCreation {
        EntityID id;
        std::string name;
        EntityID parentID;
    };
    std::vector<PendingCreation> m_PendingCreations;
   bool m_IsDirty = false;
   uint64_t m_DirtyRevision = 1;
   bool m_IsBeingDestroyed = false;  // Set true in destructor to prevent entity ops during cleanup
   ShaderPreset m_DefaultShaderPreset = ShaderPreset::PBR;
   EditorViewportState m_EditorViewportState{};
   bool m_HasEditorViewportState = false;
   bool m_PendingSceneLoad = false;
   bool m_PendingSceneLoadAsync = false;
   std::string m_PendingSceneLoadPath;

   // Scene load tracking (used by runtime + editor)
   bool m_Loading = false;
   bool m_Loaded = true;
   bool m_LoadingAsync = false;
   float m_LoadProgress = 1.0f;
   size_t m_LoadScriptsTotal = 0;
   size_t m_LoadScriptsProcessed = 0;
   bool m_LoadScriptsReady = false;
   bool m_DeferScriptOnCreate = false;
   bool m_RestoreDeferredScriptInit = false;
   bool m_PrevDeferredScriptInit = false;
   std::string m_LoadingScenePath;
   std::string m_ScenePath;  // Path of successfully loaded scene (persists after load)
   std::chrono::steady_clock::time_point m_LoadStartTime{};
   std::chrono::steady_clock::time_point m_LoadHoldStartTime{};
   float m_LoadHoldDuration = 0.0f;
   float m_LoadHoldStartProgress = 0.0f;
   bool m_LoadingHold = false;
   struct AsyncLoadJob;
   std::shared_ptr<AsyncLoadJob> m_AsyncLoadJob;

   void PrepareSceneForLoad();
   void BeginLoadTracking(const std::string& path, bool async);
   void FinalizeLoadTracking(bool success);
   void StartAsyncLoadJob(const std::string& path);
   void TickAsyncLoadJob();
   void PostLoadFixups();
   
   // =========================================================================
   // PERF: Cached hierarchy levels for transform propagation
   // Avoids O(n) BFS rebuild every frame - only rebuild on structural changes
   // =========================================================================
   mutable std::vector<std::vector<EntityID>> m_CachedHierarchyLevels;
   mutable std::vector<uint8_t> m_CachedRecomputedFlags;
   mutable bool m_HierarchyDirty = true;
   void RebuildHierarchyCacheIfNeeded() const;
   void RebuildPhysicsParticipantCacheIfNeeded();
   [[nodiscard]] bool HasPendingTransformUpdates() const;
   uint64_t SyncPhysicsBodiesFromSceneTransforms(bool includeDynamicRigidBodies);
   void SyncCameraComponentsFromTransforms();
   void EnsureRuntimeWorldTransformTrackingCapacity(size_t requiredEntityCapacity);
   void QueueRuntimeWorldTransformDirty(EntityID id);
   void ResetRuntimeWorldTransformTracking();
   void ResetRuntimeWorldDirtyTracking();
   void MarkRuntimeWorldDirty(EntityID id, cm::world::RuntimeDirtyBits bits);
   void MarkRuntimeWorldDirtySubtree(EntityID id, cm::world::RuntimeDirtyBits bits);
   void MarkRuntimeWorldRemoved(EntityID id);
   const std::vector<EntityID>& GetBoneAttachmentEntities();
   std::vector<EntityID> m_PhysicsParticipants;
   std::vector<EntityID> m_BoneAttachmentEntities;
   uint64_t m_PhysicsParticipantCacheRevision = 0;
   size_t m_PhysicsParticipantEntityCount = 0;
   uint32_t m_PhysicsParticipantRescanFrames = 0;
   bool m_PhysicsParticipantCacheDirty = true;
   bool m_BoneAttachmentEntitiesDirty = true;
   bool m_AnimationPosePaletteDirtyForAttachments = false;
   uint64_t m_RuntimeWorldBridgeRevision = 1;
   bool m_RuntimeWorldBridgeFullSyncPending = true;
   std::vector<uint32_t> m_RuntimeWorldBridgeDirtyMasks;
   std::vector<EntityID> m_RuntimeWorldBridgeDirtyEntities;
   std::vector<EntityID> m_RuntimeWorldBridgeRemovedEntities;
   std::unique_ptr<std::atomic<uint64_t>[]> m_RuntimeWorldPendingTransformBitset;
   size_t m_RuntimeWorldPendingTransformBitWordCount = 0;
   bool m_RuntimeWorldFrameSyncLocked = false;
   std::unique_ptr<cm::world::RuntimeWorld> m_RuntimeWorldBridge;
   };
