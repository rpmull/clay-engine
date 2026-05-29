#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_map>
#include <memory>
#include "Entity.h"
#include "core/animation/AvatarDefinition.h"
#include "core/assets/AssetReference.h"

#define GLM_ENABLE_EXPERIMENTAL
#include "glm/gtc/matrix_transform.hpp"
#include <glm/gtx/euler_angles.hpp>

//------------------------------------------------------------------------------
// Blend shape name hashing (stable, deterministic)
//------------------------------------------------------------------------------
static inline uint64_t HashBlendShapeName(const std::string& name) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET;
    for (unsigned char c : name) {
        hash ^= static_cast<uint64_t>(c);
        hash *= FNV_PRIME;
    }
    return hash;
}

static inline uint64_t HashBytes64Stable(const void* data, size_t size) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = FNV_OFFSET;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

//----------------------------------------------------------------------
// Blend Shape Definition
// Supports both dense and sparse storage for optimal performance:
// - Dense: DeltaPos/DeltaNormal arrays sized to full vertex count (legacy)
// - Sparse: SparseIndices + SparseDeltaPos/SparseDeltaNorm for touched vertices only
//----------------------------------------------------------------------
struct BlendShape {
    std::string Name;
    float Weight = 0.0f;
    uint64_t NameHash = 0;
    
    // Dense storage (legacy/fallback) - sized to vertex count
    std::vector<glm::vec3> DeltaPos;
    std::vector<glm::vec3> DeltaNormal;
    
    // Sparse storage (optimized) - only stores non-zero deltas
    // When IsSparse is true, use these instead of dense arrays
    bool IsSparse = false;
    std::vector<uint32_t> SparseIndices;       // Vertex indices with non-zero deltas
    std::vector<glm::vec3> SparseDeltaPos;     // Position deltas (parallel to SparseIndices)
    std::vector<glm::vec3> SparseDeltaNorm;    // Normal deltas (parallel to SparseIndices)
    
    // Convert dense data to sparse representation (call after import)
    void MakeSparse(float threshold = 1e-6f) {
        if (IsSparse) return;
        const size_t vCount = DeltaPos.size();
        SparseIndices.clear();
        SparseDeltaPos.clear();
        SparseDeltaNorm.clear();
        
        for (size_t i = 0; i < vCount; ++i) {
            const glm::vec3& dp = DeltaPos[i];
            const glm::vec3& dn = (i < DeltaNormal.size()) ? DeltaNormal[i] : glm::vec3(0);
            const float lenSq = glm::dot(dp, dp) + glm::dot(dn, dn);
            if (lenSq > threshold * threshold) {
                SparseIndices.push_back(static_cast<uint32_t>(i));
                SparseDeltaPos.push_back(dp);
                SparseDeltaNorm.push_back(dn);
            }
        }
        
        // Clear dense data to free memory
        DeltaPos.clear();
        DeltaPos.shrink_to_fit();
        DeltaNormal.clear();
        DeltaNormal.shrink_to_fit();
        IsSparse = true;
    }
    
    // Get sparse count (number of affected vertices)
    size_t SparseCount() const { return SparseIndices.size(); }

    void UpdateNameHash() {
        NameHash = HashBlendShapeName(Name);
    }
};


//----------------------------------------------------------------------
// Blend Shape Component
// Holds all blend shapes for a mesh plus scratch buffers for CPU blending.
//----------------------------------------------------------------------
struct BlendShapeComponent {
    std::vector<BlendShape> Shapes;
    bool Dirty = false;
    
    // Scratch buffers for CPU blending (reused across frames to avoid allocations)
    // Sized to vertex count on first use, cleared only when mesh changes
    std::vector<glm::vec3> AccDeltaPos;   // Accumulated position deltas
    std::vector<glm::vec3> AccDeltaNorm;  // Accumulated normal deltas
    
    // For sparse mode: indices of vertices that were touched this frame
    std::vector<uint32_t> TouchedIndices;

    // Output buffer for CPU-blended vertices (reused across frames)
    // Stored as raw bytes to support both PBRVertex and SkinnedPBRVertex layouts.
    std::vector<uint8_t> BlendedVertices;
    size_t BlendedStride = 0;

    mutable bool CachedGeometryMetricsValid = false;
    mutable size_t CachedGeometryMetricsVertexCount = 0;
    mutable uint64_t CachedGeometrySignature = 0;
    mutable uint32_t CachedMaxAffectedShapesPerVertex = 0;

    BlendShapeComponent() = default;
    BlendShapeComponent(const BlendShapeComponent& other)
        : Shapes(other.Shapes)
        , Dirty(other.Dirty) {
        // Do not copy scratch/output buffers
        CachedGeometryMetricsValid = false;
        CachedGeometryMetricsVertexCount = 0;
        CachedGeometrySignature = 0;
        CachedMaxAffectedShapesPerVertex = 0;
    }
    BlendShapeComponent& operator=(const BlendShapeComponent& other) {
        if (this == &other) return *this;
        Shapes = other.Shapes;
        Dirty = other.Dirty;
        AccDeltaPos.clear();
        AccDeltaNorm.clear();
        TouchedIndices.clear();
        BlendedVertices.clear();
        BlendedStride = 0;
        CachedGeometryMetricsValid = false;
        CachedGeometryMetricsVertexCount = 0;
        CachedGeometrySignature = 0;
        CachedMaxAffectedShapesPerVertex = 0;
        return *this;
    }
    
    // Resize scratch buffers (call once when vertex count is known)
    void EnsureScratchBuffers(size_t vertexCount) {
        if (AccDeltaPos.size() != vertexCount) {
            AccDeltaPos.assign(vertexCount, glm::vec3(0));
            AccDeltaNorm.assign(vertexCount, glm::vec3(0));
        }
    }

    void EnsureBlendOutput(size_t vertexCount, size_t vertexStride) {
        const size_t required = vertexCount * vertexStride;
        if (BlendedVertices.size() != required) {
            BlendedVertices.resize(required);
        }
        BlendedStride = vertexStride;
    }
    
    // Clear accumulated deltas (call before accumulation loop)
    void ClearAccumulators() {
        std::fill(AccDeltaPos.begin(), AccDeltaPos.end(), glm::vec3(0));
        std::fill(AccDeltaNorm.begin(), AccDeltaNorm.end(), glm::vec3(0));
        TouchedIndices.clear();
    }
    
    // Clear only touched vertices (more efficient for sparse shapes)
    void ClearTouchedOnly() {
        for (uint32_t idx : TouchedIndices) {
            AccDeltaPos[idx] = glm::vec3(0);
            AccDeltaNorm[idx] = glm::vec3(0);
        }
        TouchedIndices.clear();
    }
    
    // Convert all shapes to sparse representation
    void MakeAllSparse(float threshold = 1e-6f) {
        for (auto& shape : Shapes) {
            shape.MakeSparse(threshold);
        }
        InvalidateGeometryMetrics();
    }
    
    // Commit/bake: apply current weights to base mesh and drop morph data.
    // Call this when exiting character creator to free memory.
    // mesh->Vertices and mesh->Normals will be modified in-place.
    // After commit, this component should be removed or cleared.
    void CommitToMesh(std::vector<glm::vec3>& baseVertices, 
                      std::vector<glm::vec3>& baseNormals) {
        const size_t vCount = baseVertices.size();
        
        for (const auto& shape : Shapes) {
            if (shape.Weight == 0.0f) continue;
            
            if (shape.IsSparse) {
                // Sparse path
                for (size_t i = 0; i < shape.SparseIndices.size(); ++i) {
                    const uint32_t idx = shape.SparseIndices[i];
                    if (idx < vCount) {
                        baseVertices[idx] += shape.SparseDeltaPos[i] * shape.Weight;
                        if (idx < baseNormals.size()) {
                            baseNormals[idx] += shape.SparseDeltaNorm[i] * shape.Weight;
                        }
                    }
                }
            } else {
                // Dense path
                for (size_t i = 0; i < vCount && i < shape.DeltaPos.size(); ++i) {
                    baseVertices[i] += shape.DeltaPos[i] * shape.Weight;
                    if (i < baseNormals.size() && i < shape.DeltaNormal.size()) {
                        baseNormals[i] += shape.DeltaNormal[i] * shape.Weight;
                    }
                }
            }
        }
        
        // Clear all morph data after commit
        Shapes.clear();
        Shapes.shrink_to_fit();
        AccDeltaPos.clear();
        AccDeltaPos.shrink_to_fit();
        AccDeltaNorm.clear();
        AccDeltaNorm.shrink_to_fit();
        TouchedIndices.clear();
        TouchedIndices.shrink_to_fit();
        Dirty = false;
        InvalidateGeometryMetrics();
    }

    void InvalidateGeometryMetrics() {
        CachedGeometryMetricsValid = false;
        CachedGeometryMetricsVertexCount = 0;
        CachedGeometrySignature = 0;
        CachedMaxAffectedShapesPerVertex = 0;
    }

    uint32_t CountActiveShapes(float weightThreshold = 1e-4f) const {
        uint32_t activeCount = 0;
        for (const auto& shape : Shapes) {
            if (std::abs(shape.Weight) > weightThreshold) {
                ++activeCount;
            }
        }
        return activeCount;
    }

    uint64_t GetGeometrySignature(size_t vertexCount, float sparseThreshold = 1e-6f) const {
        EnsureGeometryMetrics(vertexCount, sparseThreshold);
        return CachedGeometrySignature;
    }

    uint32_t GetMaxAffectedShapesPerVertex(size_t vertexCount, float sparseThreshold = 1e-6f) const {
        EnsureGeometryMetrics(vertexCount, sparseThreshold);
        return CachedMaxAffectedShapesPerVertex;
    }

private:
    void EnsureGeometryMetrics(size_t vertexCount, float sparseThreshold) const {
        if (CachedGeometryMetricsValid &&
            CachedGeometryMetricsVertexCount == vertexCount) {
            return;
        }

        constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
        constexpr uint64_t kFnvPrime = 1099511628211ULL;
        auto hashBytes = [&](uint64_t& hash, const void* data, size_t size) {
            if (!data || size == 0) {
                return;
            }
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; ++i) {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= kFnvPrime;
            }
        };

        uint64_t geometryHash = kFnvOffset;
        hashBytes(geometryHash, &vertexCount, sizeof(vertexCount));

        std::vector<uint32_t> affectedCounts(vertexCount, 0u);
        uint32_t maxAffected = 0;
        const float thresholdSq = sparseThreshold * sparseThreshold;

        for (size_t shapeIndex = 0; shapeIndex < Shapes.size(); ++shapeIndex) {
            const auto& shape = Shapes[shapeIndex];
            hashBytes(geometryHash, &shapeIndex, sizeof(shapeIndex));
            hashBytes(geometryHash, &shape.NameHash, sizeof(shape.NameHash));
            hashBytes(geometryHash, &shape.IsSparse, sizeof(shape.IsSparse));

            if (shape.IsSparse) {
                const uint32_t sparseCount = static_cast<uint32_t>(shape.SparseIndices.size());
                hashBytes(geometryHash, &sparseCount, sizeof(sparseCount));
                for (size_t i = 0; i < shape.SparseIndices.size(); ++i) {
                    const uint32_t vertexIndex = shape.SparseIndices[i];
                    hashBytes(geometryHash, &vertexIndex, sizeof(vertexIndex));
                    if (i < shape.SparseDeltaPos.size()) {
                        hashBytes(geometryHash, &shape.SparseDeltaPos[i], sizeof(glm::vec3));
                    }
                    if (i < shape.SparseDeltaNorm.size()) {
                        hashBytes(geometryHash, &shape.SparseDeltaNorm[i], sizeof(glm::vec3));
                    }
                    if (vertexIndex >= vertexCount) {
                        continue;
                    }
                    const uint32_t touched = ++affectedCounts[vertexIndex];
                    maxAffected = std::max(maxAffected, touched);
                }
                continue;
            }

            const size_t denseCount = std::min(vertexCount, shape.DeltaPos.size());
            hashBytes(geometryHash, &denseCount, sizeof(denseCount));
            for (size_t vertexIndex = 0; vertexIndex < denseCount; ++vertexIndex) {
                const glm::vec3 deltaPos = shape.DeltaPos[vertexIndex];
                const glm::vec3 deltaNorm =
                    vertexIndex < shape.DeltaNormal.size()
                        ? shape.DeltaNormal[vertexIndex]
                        : glm::vec3(0.0f);
                hashBytes(geometryHash, &deltaPos, sizeof(glm::vec3));
                hashBytes(geometryHash, &deltaNorm, sizeof(glm::vec3));
                if ((glm::dot(deltaPos, deltaPos) + glm::dot(deltaNorm, deltaNorm)) <= thresholdSq) {
                    continue;
                }
                const uint32_t touched = ++affectedCounts[vertexIndex];
                maxAffected = std::max(maxAffected, touched);
            }
        }

        CachedGeometryMetricsValid = true;
        CachedGeometryMetricsVertexCount = vertexCount;
        CachedGeometrySignature = geometryHash;
        CachedMaxAffectedShapesPerVertex = maxAffected;
    }
};

//------------------------------------------------------------------------------
// Unified Morphs (per-model grouped blendshapes):
// At the skeleton root (or model root when no skeleton), we can aggregate
// blendshapes that share the same Name across child meshes. This component
// holds the names and a single weight per shared name; applying a change
// should propagate to all child meshes that have a matching blendshape.
//------------------------------------------------------------------------------
struct UnifiedMorphComponent {
    // Ordered list of unique shared morph names for this model
    std::vector<std::string> Names;
    // Current weight per name (0..1)
    std::vector<float> Weights;
    // Entities (meshes) that belong to this model; only these receive propagation
    std::vector<EntityID> MemberMeshes;
    // Cached name index for fast propagation (name hash -> index)
    std::unordered_map<uint64_t, size_t> NameIndex;
    bool NameIndexDirty = true;
};

//------------------------------------------------------------------------------
// TintBlendMode: Blender-style color mixing modes for tint application
//------------------------------------------------------------------------------
enum class TintBlendMode : int {
    Normal = 0,     // Standard lerp: mix(base, tint, mask)
    Multiply,       // base * tint
    Overlay,        // Photoshop/Blender overlay
    Add,            // base + tint
    Screen,         // 1 - (1-base)*(1-tint)
    SoftLight,      // Soft light blend
    ColorDodge,     // base / (1 - tint)
    ColorBurn,      // 1 - (1 - base) / tint
    Difference,     // abs(base - tint)
    Detail,         // Preserve tint at 50% gray, brighten above, darken below
    Count
};

inline const char* TintBlendModeToString(TintBlendMode mode) {
    switch (mode) {
        case TintBlendMode::Normal:     return "Normal";
        case TintBlendMode::Multiply:   return "Multiply";
        case TintBlendMode::Overlay:    return "Overlay";
        case TintBlendMode::Add:        return "Add";
        case TintBlendMode::Screen:     return "Screen";
        case TintBlendMode::SoftLight:  return "Soft Light";
        case TintBlendMode::ColorDodge: return "Color Dodge";
        case TintBlendMode::ColorBurn:  return "Color Burn";
        case TintBlendMode::Difference: return "Difference";
        case TintBlendMode::Detail:     return "Detail";
        default:                        return "Unknown";
    }
}

//------------------------------------------------------------------------------
// TintTarget: Explicit target for tint application (entity + material slot)
//------------------------------------------------------------------------------
struct TintTarget {
    EntityID TargetEntity = (EntityID)-1;  // Target child entity
    int MaterialSlot = -1;                  // -1 = all slots, >=0 = specific slot
    TintBlendMode BlendMode = TintBlendMode::Normal;
    
    // Per-target color override (if UseTargetColor is true)
    bool UseTargetColor = false;
    glm::vec4 Color = glm::vec4(1.0f);
};

//------------------------------------------------------------------------------
// TintMaskController: Controls tint colors on child meshes
// 
// NEW: Uses explicit list of targets (entity + material slot) instead of
// string pattern matching. Each target can have its own blend mode.
//
// Usage:
//   - Add component to parent entity (e.g., character root)
//   - Add targets by selecting child entities and their material slots
//   - Set global TintColors or per-target overrides
//   - Choose blend mode per target (Normal, Overlay, Multiply, etc.)
//------------------------------------------------------------------------------
struct TintMaskController {
    // NEW: Explicit list of targets (replaces string pattern matching)
    std::vector<TintTarget> Targets;
    // When true, skinned meshes parented at runtime may be auto-added as targets.
    // Character-creator style workflows usually disable this and rebuild targets explicitly.
    bool AutoIncludeParentedSkinnedMeshes = true;
    
    // Global tint colors for mask channels (R, G, B, A channels of tint mask texture)
    glm::vec4 TintColor0 = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // Red channel
    glm::vec4 TintColor1 = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // Green channel
    glm::vec4 TintColor2 = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // Blue channel
    glm::vec4 TintColor3 = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);  // Alpha channel (fallback/base)
    
    // Combined RGBA tint (simpler single-color mode, applied to u_ColorTint)
    glm::vec4 BaseTint = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    
    // Global blend mode (used when target doesn't override)
    TintBlendMode GlobalBlendMode = TintBlendMode::Normal;
    
    // When true, use the multi-channel tint mask mode (TintColor0-3)
    // When false, use simple BaseTint mode
    bool UseTintMask = false;

    // PBR scalar overrides (optional, for performance toggling)
    bool UsePbrOverrides = false;
    float OverrideMetallic = 0.0f;
    float OverrideRoughness = 0.5f;
    float OverrideEmissionStrength = 0.0f;
    glm::vec3 OverrideEmissionColor = glm::vec3(1.0f);
    
    // DEPRECATED: Legacy pattern matching (kept for backward compatibility)
    std::string NamePattern;
    std::vector<EntityID> MatchingMeshes;
    bool NeedsRefresh = true;
    
    // OPTIMIZATION: Dirty flag to skip redundant per-frame property block writes
    // Set to true when any tint value changes, cleared after applying tints
    bool TintDirty = true;
    
    // Cached previous values to detect changes
    glm::vec4 PrevTintColor0 = glm::vec4(-1.0f);  // Invalid initial value to force first apply
    glm::vec4 PrevTintColor1 = glm::vec4(-1.0f);
    glm::vec4 PrevTintColor2 = glm::vec4(-1.0f);
    glm::vec4 PrevTintColor3 = glm::vec4(-1.0f);
    glm::vec4 PrevBaseTint = glm::vec4(-1.0f);
    bool PrevUsePbrOverrides = false;
    float PrevOverrideMetallic = -1.0f;
    float PrevOverrideRoughness = -1.0f;
    float PrevOverrideEmissionStrength = -1.0f;
    glm::vec3 PrevOverrideEmissionColor = glm::vec3(-1.0f);
    bool PbrOverridesApplied = false;
    
    // Helper to mark dirty when tint changes
    void MarkDirty() { TintDirty = true; }
    
    // Check if tint values have changed and update dirty flag
    bool CheckAndUpdateDirty() {
        bool changed = (TintColor0 != PrevTintColor0) ||
                       (TintColor1 != PrevTintColor1) ||
                       (TintColor2 != PrevTintColor2) ||
                       (TintColor3 != PrevTintColor3) ||
                       (BaseTint != PrevBaseTint) ||
                       (UsePbrOverrides != PrevUsePbrOverrides) ||
                       (OverrideMetallic != PrevOverrideMetallic) ||
                       (OverrideRoughness != PrevOverrideRoughness) ||
                       (OverrideEmissionStrength != PrevOverrideEmissionStrength) ||
                       (OverrideEmissionColor != PrevOverrideEmissionColor);
        if (changed) {
            PrevTintColor0 = TintColor0;
            PrevTintColor1 = TintColor1;
            PrevTintColor2 = TintColor2;
            PrevTintColor3 = TintColor3;
            PrevBaseTint = BaseTint;
            PrevUsePbrOverrides = UsePbrOverrides;
            PrevOverrideMetallic = OverrideMetallic;
            PrevOverrideRoughness = OverrideRoughness;
            PrevOverrideEmissionStrength = OverrideEmissionStrength;
            PrevOverrideEmissionColor = OverrideEmissionColor;
            TintDirty = true;
        }
        return TintDirty;
    }
};


//----------------------------------------------------------------------
// Skeleton Component
//----------------------------------------------------------------------
struct SkeletonComponent {
    std::vector<glm::mat4> InverseBindPoses;  // inverse bind matrices per bone
    std::vector<glm::mat4> BindPoseGlobals;
    std::vector<glm::mat4> BindPoseLocals;
    std::vector<glm::vec3> BindPoseLocalTranslations;
    std::vector<glm::quat> BindPoseLocalRotations;
    std::vector<glm::vec3> BindPoseLocalScales;

    std::vector<EntityID>  BoneEntities;      // entity for each bone (index matches InverseBindPoses)

    // Name → index lookup to enable fast sampling & editor display.
    std::unordered_map<std::string, int> BoneNameToIndex;
    std::vector<int> BoneParents; // index of parent bone (-1 for root)

    // Optional: stable names aligned by index (authoring or import time). If empty, derive from BoneNameToIndex.
    std::vector<std::string> BoneNames;

    // Optional: stable skeleton asset GUID and per-joint GUIDs (Hash64(skeletonGuid + "/" + fullPath))
    ClaymoreGUID SkeletonGuid; // zero when unknown
    std::vector<uint64_t> JointGuids; // size == number of bones when computed

    int GetBoneIndex(const std::string& name) const {
        auto it = BoneNameToIndex.find(name);
        return it != BoneNameToIndex.end() ? it->second : -1;
    }

    void EnsureBindPoseLocals() {
        const size_t boneCount = InverseBindPoses.size();
        if (boneCount == 0) {
            BindPoseLocals.clear();
            BindPoseLocalTranslations.clear();
            BindPoseLocalRotations.clear();
            BindPoseLocalScales.clear();
            return;
        }

        if (BindPoseGlobals.size() != boneCount) {
            BindPoseGlobals.resize(boneCount);
            for (size_t i = 0; i < boneCount; ++i) {
                BindPoseGlobals[i] = glm::inverse(InverseBindPoses[i]);
            }
        }

        if (BindPoseLocals.size() != boneCount) {
            BindPoseLocals.assign(boneCount, glm::mat4(1.0f));
            for (size_t i = 0; i < boneCount; ++i) {
                const int parentIndex = (i < BoneParents.size()) ? BoneParents[i] : -1;
                const glm::mat4 parentGlobal =
                    (parentIndex >= 0 && static_cast<size_t>(parentIndex) < BindPoseGlobals.size())
                    ? BindPoseGlobals[static_cast<size_t>(parentIndex)]
                    : glm::mat4(1.0f);
                BindPoseLocals[i] = glm::inverse(parentGlobal) * BindPoseGlobals[i];
            }
        }

        if (BindPoseLocalTranslations.size() != boneCount ||
            BindPoseLocalRotations.size() != boneCount ||
            BindPoseLocalScales.size() != boneCount) {
            BindPoseLocalTranslations.resize(boneCount);
            BindPoseLocalRotations.resize(boneCount);
            BindPoseLocalScales.resize(boneCount);
            for (size_t i = 0; i < boneCount; ++i) {
                const glm::mat4& local = BindPoseLocals[i];
                glm::vec3 x = glm::vec3(local[0]);
                glm::vec3 y = glm::vec3(local[1]);
                glm::vec3 z = glm::vec3(local[2]);
                const glm::vec3 scale(
                    glm::length(x),
                    glm::length(y),
                    glm::length(z));
                if (scale.x > 1e-6f) x /= scale.x;
                if (scale.y > 1e-6f) y /= scale.y;
                if (scale.z > 1e-6f) z /= scale.z;
                BindPoseLocalTranslations[i] = glm::vec3(local[3]);
                BindPoseLocalRotations[i] = glm::normalize(glm::quat_cast(glm::mat3(x, y, z)));
                BindPoseLocalScales[i] = scale;
            }
        }
    }

    // Optional humanoid avatar built for this skeleton
    std::unique_ptr<cm::animation::AvatarDefinition> Avatar;

    // Debug instrumentation (set when CLAYMORE_DEBUG_MODEL_DUMPS is active)
    std::string DebugSourcePath;
    std::string DebugStageHint;
    bool DebugDumpPending = false;

    //----------------------------------------------------------------------
    // Runtime-only: authoritative skeleton-local pose buffer for GPU skinning
    // (NOT serialized). Meshes authored in the skeleton root's space can bind
    // this directly; other meshes layer remap/correction metadata on top of it.
    // The normal animation pipeline may populate this ahead of SkinningSystem
    // so crowd characters can skip rebuilding the same pose from entities.
    //----------------------------------------------------------------------
    std::vector<glm::mat4> BonePalette;
    std::vector<glm::mat4> RuntimeLocalPose;
    std::vector<glm::vec3> RuntimeLocalTranslations;
    std::vector<glm::quat> RuntimeLocalRotations;
    std::vector<glm::vec3> RuntimeLocalScales;
    std::vector<glm::mat4> RuntimeBoneGlobals;
    uint32_t BoneCount = 0;
    uint32_t PoseFrameId = 0; // frame counter to avoid redundant uploads
    uint32_t RuntimeLocalPoseFrameId = 0;
    uint32_t RuntimeBoneGlobalsFrameId = 0;
    bool AnimatedPosePaletteValid = false;
    bool RuntimeLocalPoseValid = false;
    bool RuntimeLocalPoseTrsValid = false;
    bool RuntimeBoneGlobalsValid = false;
    bool LODEnabled = true;
    float LODNearDistance = 30.0f;
    float LODMediumDistance = 60.0f;
    float LODFarDistance = 120.0f;
    float LodAccumulatedTime = 0.0f;
    float LodLastDistance = 0.0f;
    bool LodMeshVisibleLastFrame = true;
    bool VisualWorkVisibleThisFrame = true;
    mutable uint64_t CachedCrowdPoseSkeletonSignature = 0;
    mutable bool CachedCrowdPoseSkeletonSignatureValid = false;
    mutable bool CachedLinearHierarchyResolved = false;
    mutable bool CachedCanResolveHierarchyLinearly = true;
    mutable size_t CachedLinearHierarchyBoneCount = 0;

    void EnsureRuntimeBonePaletteSize(size_t boneCount) {
        if (BonePalette.size() != boneCount) {
            BonePalette.resize(boneCount, glm::mat4(1.0f));
        }
    }

    void EnsureRuntimeBoneGlobalSize(size_t boneCount) {
        if (RuntimeBoneGlobals.size() != boneCount) {
            RuntimeBoneGlobals.resize(boneCount, glm::mat4(1.0f));
        }
    }

    void EnsureRuntimeLocalPoseTrsSize(size_t boneCount) {
        if (RuntimeLocalTranslations.size() != boneCount) {
            RuntimeLocalTranslations.resize(boneCount, glm::vec3(0.0f));
        }
        if (RuntimeLocalRotations.size() != boneCount) {
            RuntimeLocalRotations.resize(boneCount, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        }
        if (RuntimeLocalScales.size() != boneCount) {
            RuntimeLocalScales.resize(boneCount, glm::vec3(1.0f));
        }
    }

    glm::mat4 ComposeRuntimeLocalPoseMatrix(size_t boneIndex) const {
        glm::mat4 local = glm::mat4_cast(glm::normalize(RuntimeLocalRotations[boneIndex]));
        local[0] *= RuntimeLocalScales[boneIndex].x;
        local[1] *= RuntimeLocalScales[boneIndex].y;
        local[2] *= RuntimeLocalScales[boneIndex].z;
        local[3] = glm::vec4(RuntimeLocalTranslations[boneIndex], 1.0f);
        return local;
    }

    void StoreRuntimeLocalPose(const std::vector<glm::mat4>& localTransforms,
                               size_t boneCount,
                               uint32_t poseFrameId) {
        boneCount = std::min(boneCount, localTransforms.size());
        if (boneCount == 0) {
            RuntimeLocalPose.clear();
            RuntimeLocalPoseFrameId = poseFrameId;
            RuntimeLocalPoseValid = false;
            RuntimeBoneGlobalsFrameId = poseFrameId;
            RuntimeBoneGlobalsValid = false;
            return;
        }

        RuntimeLocalPose.resize(boneCount);
        std::copy_n(localTransforms.data(), boneCount, RuntimeLocalPose.data());
        RuntimeLocalPoseFrameId = poseFrameId;
        RuntimeLocalPoseValid = true;
        RuntimeLocalPoseTrsValid = false;
    }

    void StoreRuntimeLocalPoseTrs(const std::vector<glm::vec3>& translations,
                                  const std::vector<glm::quat>& rotations,
                                  const std::vector<glm::vec3>& scales,
                                  size_t boneCount,
                                  uint32_t poseFrameId) {
        boneCount = std::min(boneCount, translations.size());
        boneCount = std::min(boneCount, rotations.size());
        boneCount = std::min(boneCount, scales.size());
        if (boneCount == 0) {
            RuntimeLocalTranslations.clear();
            RuntimeLocalRotations.clear();
            RuntimeLocalScales.clear();
            RuntimeLocalPoseFrameId = poseFrameId;
            RuntimeLocalPoseTrsValid = false;
            RuntimeBoneGlobalsFrameId = poseFrameId;
            RuntimeBoneGlobalsValid = false;
            return;
        }

        EnsureRuntimeLocalPoseTrsSize(boneCount);
        std::copy_n(translations.data(), boneCount, RuntimeLocalTranslations.data());
        std::copy_n(rotations.data(), boneCount, RuntimeLocalRotations.data());
        std::copy_n(scales.data(), boneCount, RuntimeLocalScales.data());
        RuntimeLocalPoseFrameId = poseFrameId;
        RuntimeLocalPoseTrsValid = true;
    }

    void ClearRuntimeLocalPose(uint32_t poseFrameId = 0) {
        RuntimeLocalPoseFrameId = poseFrameId;
        RuntimeLocalPoseValid = false;
        RuntimeLocalPoseTrsValid = false;
        RuntimeBoneGlobalsFrameId = poseFrameId;
        RuntimeBoneGlobalsValid = false;
    }

    uint64_t GetCachedTopologySignature() const {
        if (CachedCrowdPoseSkeletonSignatureValid) {
            return CachedCrowdPoseSkeletonSignature;
        }

        uint64_t signature = 14695981039346656037ULL;
        signature ^= static_cast<uint64_t>(BoneEntities.size()) + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
        signature ^= static_cast<uint64_t>(BoneParents.size()) + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);

        if (SkeletonGuid.high != 0 || SkeletonGuid.low != 0) {
            signature ^= SkeletonGuid.high + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
            signature ^= SkeletonGuid.low + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
            CachedCrowdPoseSkeletonSignature = signature;
            CachedCrowdPoseSkeletonSignatureValid = true;
            return CachedCrowdPoseSkeletonSignature;
        }

        constexpr uint64_t kFnvPrime = 1099511628211ULL;
        for (const auto& boneName : BoneNames) {
            uint64_t nameHash = 14695981039346656037ULL;
            for (unsigned char c : boneName) {
                nameHash ^= static_cast<uint64_t>(c);
                nameHash *= kFnvPrime;
            }
            signature ^= nameHash + 0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
        }
        for (int parentIndex : BoneParents) {
            signature ^= static_cast<uint64_t>(static_cast<int64_t>(parentIndex) + 2) +
                         0x9e3779b97f4a7c15ULL + (signature << 6) + (signature >> 2);
        }

        CachedCrowdPoseSkeletonSignature = signature;
        CachedCrowdPoseSkeletonSignatureValid = true;
        return CachedCrowdPoseSkeletonSignature;
    }

    bool CanResolveHierarchyLinearly(size_t boneCount) {
        if (CachedLinearHierarchyResolved &&
            CachedLinearHierarchyBoneCount == boneCount) {
            return CachedCanResolveHierarchyLinearly;
        }

        CachedLinearHierarchyResolved = true;
        CachedLinearHierarchyBoneCount = boneCount;
        CachedCanResolveHierarchyLinearly = true;
        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            const int parentIndex = (boneIndex < BoneParents.size()) ? BoneParents[boneIndex] : -1;
            if (parentIndex >= 0 &&
                static_cast<size_t>(parentIndex) < boneCount &&
                static_cast<size_t>(parentIndex) >= boneIndex) {
                CachedCanResolveHierarchyLinearly = false;
                break;
            }
        }

        return CachedCanResolveHierarchyLinearly;
    }

    bool RebuildRuntimeBonePaletteFromLocalPose(uint32_t poseFrameId) {
        const size_t boneCount = std::min(
            std::min(RuntimeLocalPose.size(), InverseBindPoses.size()),
            static_cast<size_t>(RuntimeLocalPoseValid ? RuntimeLocalPose.size() : 0));
        if (boneCount == 0) {
            BonePalette.clear();
            BoneCount = 0;
            PoseFrameId = poseFrameId;
            AnimatedPosePaletteValid = false;
            RuntimeBoneGlobals.clear();
            RuntimeBoneGlobalsFrameId = poseFrameId;
            RuntimeBoneGlobalsValid = false;
            return false;
        }

        EnsureRuntimeBonePaletteSize(boneCount);
        EnsureRuntimeBoneGlobalSize(boneCount);
        thread_local std::vector<uint8_t> resolved;
        auto& skeletonLocalGlobals = RuntimeBoneGlobals;

        const bool canResolveLinearly = CanResolveHierarchyLinearly(boneCount);
        if (canResolveLinearly) {
            for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                const int parentIndex = (boneIndex < BoneParents.size()) ? BoneParents[boneIndex] : -1;
                if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < boneCount) {
                    skeletonLocalGlobals[boneIndex] =
                        skeletonLocalGlobals[static_cast<size_t>(parentIndex)] *
                        RuntimeLocalPose[boneIndex];
                } else {
                    skeletonLocalGlobals[boneIndex] = RuntimeLocalPose[boneIndex];
                }
                BonePalette[boneIndex] = skeletonLocalGlobals[boneIndex] * InverseBindPoses[boneIndex];
            }
            BoneCount = static_cast<uint32_t>(boneCount);
            PoseFrameId = poseFrameId;
            AnimatedPosePaletteValid = true;
            RuntimeBoneGlobalsFrameId = poseFrameId;
            RuntimeBoneGlobalsValid = true;
            return true;
        }

        if (resolved.size() < boneCount) {
            resolved.resize(boneCount);
        }
        std::fill_n(resolved.begin(), boneCount, uint8_t{0u});

        auto resolveGlobal = [&](auto&& self, size_t boneIndex) -> const glm::mat4& {
            if (resolved[boneIndex] != 0u) {
                return skeletonLocalGlobals[boneIndex];
            }

            const int parentIndex = (boneIndex < BoneParents.size()) ? BoneParents[boneIndex] : -1;
            if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < boneCount) {
                skeletonLocalGlobals[boneIndex] =
                    self(self, static_cast<size_t>(parentIndex)) *
                    RuntimeLocalPose[boneIndex];
            } else {
                skeletonLocalGlobals[boneIndex] = RuntimeLocalPose[boneIndex];
            }
            resolved[boneIndex] = 1u;
            return skeletonLocalGlobals[boneIndex];
        };

        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            const glm::mat4& skeletonLocal =
                resolved[boneIndex] != 0u ? skeletonLocalGlobals[boneIndex]
                                          : resolveGlobal(resolveGlobal, boneIndex);
            BonePalette[boneIndex] = skeletonLocal * InverseBindPoses[boneIndex];
        }

        BoneCount = static_cast<uint32_t>(boneCount);
        PoseFrameId = poseFrameId;
        AnimatedPosePaletteValid = true;
        RuntimeBoneGlobalsFrameId = poseFrameId;
        RuntimeBoneGlobalsValid = true;
        return true;
    }

    bool RebuildRuntimeBonePaletteFromLocalPoseTrs(uint32_t poseFrameId) {
        const size_t boneCount = std::min(
            std::min(RuntimeLocalTranslations.size(), InverseBindPoses.size()),
            std::min(RuntimeLocalRotations.size(), RuntimeLocalScales.size()));
        if (!RuntimeLocalPoseTrsValid || boneCount == 0) {
            BonePalette.clear();
            BoneCount = 0;
            PoseFrameId = poseFrameId;
            AnimatedPosePaletteValid = false;
            RuntimeBoneGlobals.clear();
            RuntimeBoneGlobalsFrameId = poseFrameId;
            RuntimeBoneGlobalsValid = false;
            return false;
        }

        EnsureRuntimeBonePaletteSize(boneCount);
        EnsureRuntimeBoneGlobalSize(boneCount);
        thread_local std::vector<uint8_t> resolved;
        auto& skeletonLocalGlobals = RuntimeBoneGlobals;

        const bool canResolveLinearly = CanResolveHierarchyLinearly(boneCount);
        if (canResolveLinearly) {
            for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
                const glm::mat4 local = ComposeRuntimeLocalPoseMatrix(boneIndex);
                const int parentIndex = (boneIndex < BoneParents.size()) ? BoneParents[boneIndex] : -1;
                if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < boneCount) {
                    skeletonLocalGlobals[boneIndex] =
                        skeletonLocalGlobals[static_cast<size_t>(parentIndex)] * local;
                } else {
                    skeletonLocalGlobals[boneIndex] = local;
                }
                BonePalette[boneIndex] = skeletonLocalGlobals[boneIndex] * InverseBindPoses[boneIndex];
            }
            BoneCount = static_cast<uint32_t>(boneCount);
            PoseFrameId = poseFrameId;
            AnimatedPosePaletteValid = true;
            RuntimeBoneGlobalsFrameId = poseFrameId;
            RuntimeBoneGlobalsValid = true;
            return true;
        }

        if (resolved.size() < boneCount) {
            resolved.resize(boneCount);
        }
        std::fill_n(resolved.begin(), boneCount, uint8_t{0u});

        auto resolveGlobal = [&](auto&& self, size_t boneIndex) -> const glm::mat4& {
            if (resolved[boneIndex] != 0u) {
                return skeletonLocalGlobals[boneIndex];
            }

            const glm::mat4 local = ComposeRuntimeLocalPoseMatrix(boneIndex);
            const int parentIndex = (boneIndex < BoneParents.size()) ? BoneParents[boneIndex] : -1;
            if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < boneCount) {
                skeletonLocalGlobals[boneIndex] =
                    self(self, static_cast<size_t>(parentIndex)) * local;
            } else {
                skeletonLocalGlobals[boneIndex] = local;
            }
            resolved[boneIndex] = 1u;
            return skeletonLocalGlobals[boneIndex];
        };

        for (size_t boneIndex = 0; boneIndex < boneCount; ++boneIndex) {
            const glm::mat4& skeletonLocal =
                resolved[boneIndex] != 0u ? skeletonLocalGlobals[boneIndex]
                                          : resolveGlobal(resolveGlobal, boneIndex);
            BonePalette[boneIndex] = skeletonLocal * InverseBindPoses[boneIndex];
        }

        BoneCount = static_cast<uint32_t>(boneCount);
        PoseFrameId = poseFrameId;
        AnimatedPosePaletteValid = true;
        RuntimeBoneGlobalsFrameId = poseFrameId;
        RuntimeBoneGlobalsValid = true;
        return true;
    }

    // Prevent copy to avoid expensive accidental duplication of runtime state.
    SkeletonComponent() = default;
    SkeletonComponent(const SkeletonComponent&) = delete;
    SkeletonComponent& operator=(const SkeletonComponent&) = delete;
    SkeletonComponent(SkeletonComponent&& other) noexcept 
        : InverseBindPoses(std::move(other.InverseBindPoses))
        , BindPoseGlobals(std::move(other.BindPoseGlobals))
        , BindPoseLocals(std::move(other.BindPoseLocals))
        , BindPoseLocalTranslations(std::move(other.BindPoseLocalTranslations))
        , BindPoseLocalRotations(std::move(other.BindPoseLocalRotations))
        , BindPoseLocalScales(std::move(other.BindPoseLocalScales))
        , BoneEntities(std::move(other.BoneEntities))
        , BoneNameToIndex(std::move(other.BoneNameToIndex))
        , BoneParents(std::move(other.BoneParents))
        , BoneNames(std::move(other.BoneNames))
        , SkeletonGuid(other.SkeletonGuid)
        , JointGuids(std::move(other.JointGuids))
        , Avatar(std::move(other.Avatar))
        , DebugSourcePath(std::move(other.DebugSourcePath))
        , DebugStageHint(std::move(other.DebugStageHint))
        , DebugDumpPending(other.DebugDumpPending)
        , BonePalette(std::move(other.BonePalette))
        , RuntimeLocalPose(std::move(other.RuntimeLocalPose))
        , RuntimeLocalTranslations(std::move(other.RuntimeLocalTranslations))
        , RuntimeLocalRotations(std::move(other.RuntimeLocalRotations))
        , RuntimeLocalScales(std::move(other.RuntimeLocalScales))
        , RuntimeBoneGlobals(std::move(other.RuntimeBoneGlobals))
        , BoneCount(other.BoneCount)
        , PoseFrameId(other.PoseFrameId)
        , RuntimeLocalPoseFrameId(other.RuntimeLocalPoseFrameId)
        , RuntimeBoneGlobalsFrameId(other.RuntimeBoneGlobalsFrameId)
        , AnimatedPosePaletteValid(other.AnimatedPosePaletteValid)
        , RuntimeLocalPoseValid(other.RuntimeLocalPoseValid)
        , RuntimeLocalPoseTrsValid(other.RuntimeLocalPoseTrsValid)
        , RuntimeBoneGlobalsValid(other.RuntimeBoneGlobalsValid)
        , LODEnabled(other.LODEnabled)
        , LODNearDistance(other.LODNearDistance)
        , LODMediumDistance(other.LODMediumDistance)
        , LODFarDistance(other.LODFarDistance)
        , LodAccumulatedTime(other.LodAccumulatedTime)
        , LodLastDistance(other.LodLastDistance)
        , LodMeshVisibleLastFrame(other.LodMeshVisibleLastFrame)
        , VisualWorkVisibleThisFrame(other.VisualWorkVisibleThisFrame)
        , CachedCrowdPoseSkeletonSignature(other.CachedCrowdPoseSkeletonSignature)
        , CachedCrowdPoseSkeletonSignatureValid(other.CachedCrowdPoseSkeletonSignatureValid)
        , CachedLinearHierarchyResolved(other.CachedLinearHierarchyResolved)
        , CachedCanResolveHierarchyLinearly(other.CachedCanResolveHierarchyLinearly)
        , CachedLinearHierarchyBoneCount(other.CachedLinearHierarchyBoneCount)
    {}
    SkeletonComponent& operator=(SkeletonComponent&& other) noexcept {
        if (this != &other) {
            InverseBindPoses = std::move(other.InverseBindPoses);
            BindPoseGlobals = std::move(other.BindPoseGlobals);
            BindPoseLocals = std::move(other.BindPoseLocals);
            BoneEntities = std::move(other.BoneEntities);
            BoneNameToIndex = std::move(other.BoneNameToIndex);
            BoneParents = std::move(other.BoneParents);
            BoneNames = std::move(other.BoneNames);
            SkeletonGuid = other.SkeletonGuid;
            JointGuids = std::move(other.JointGuids);
            Avatar = std::move(other.Avatar);
            DebugSourcePath = std::move(other.DebugSourcePath);
            DebugStageHint = std::move(other.DebugStageHint);
            DebugDumpPending = other.DebugDumpPending;
            BindPoseLocalTranslations = std::move(other.BindPoseLocalTranslations);
            BindPoseLocalRotations = std::move(other.BindPoseLocalRotations);
            BindPoseLocalScales = std::move(other.BindPoseLocalScales);
            BonePalette = std::move(other.BonePalette);
            RuntimeLocalPose = std::move(other.RuntimeLocalPose);
            RuntimeLocalTranslations = std::move(other.RuntimeLocalTranslations);
            RuntimeLocalRotations = std::move(other.RuntimeLocalRotations);
            RuntimeLocalScales = std::move(other.RuntimeLocalScales);
            RuntimeBoneGlobals = std::move(other.RuntimeBoneGlobals);
            BoneCount = other.BoneCount;
            PoseFrameId = other.PoseFrameId;
            RuntimeLocalPoseFrameId = other.RuntimeLocalPoseFrameId;
            RuntimeBoneGlobalsFrameId = other.RuntimeBoneGlobalsFrameId;
            AnimatedPosePaletteValid = other.AnimatedPosePaletteValid;
            RuntimeLocalPoseValid = other.RuntimeLocalPoseValid;
            RuntimeLocalPoseTrsValid = other.RuntimeLocalPoseTrsValid;
            RuntimeBoneGlobalsValid = other.RuntimeBoneGlobalsValid;
            LODEnabled = other.LODEnabled;
            LODNearDistance = other.LODNearDistance;
            LODMediumDistance = other.LODMediumDistance;
            LODFarDistance = other.LODFarDistance;
            LodAccumulatedTime = other.LodAccumulatedTime;
            LodLastDistance = other.LodLastDistance;
            LodMeshVisibleLastFrame = other.LodMeshVisibleLastFrame;
            VisualWorkVisibleThisFrame = other.VisualWorkVisibleThisFrame;
            CachedCrowdPoseSkeletonSignature = other.CachedCrowdPoseSkeletonSignature;
            CachedCrowdPoseSkeletonSignatureValid = other.CachedCrowdPoseSkeletonSignatureValid;
            CachedLinearHierarchyResolved = other.CachedLinearHierarchyResolved;
            CachedCanResolveHierarchyLinearly = other.CachedCanResolveHierarchyLinearly;
            CachedLinearHierarchyBoneCount = other.CachedLinearHierarchyBoneCount;
        }
        return *this;
    }
};


//----------------------------------------------------------------------
// Skinning Component: references shared skeleton for GPU skinning.
// The CPU owns one authoritative skeleton pose, and the renderer binds that
// pose through a shared atlas. Mesh-specific variation is expressed as source
// index remaps, optional bind-pose correction matrices, and a mesh-from-
// skeleton transform instead of rebuilding per-mesh bone palettes every frame.
//----------------------------------------------------------------------
struct SkinningComponent {
    static constexpr uint32_t MaxDirectVertexBones = 256;

    EntityID SkeletonRoot = -1;
    
    // ========================================================================
    // Skeleton Reassignment / Retargeting
    // ========================================================================
    // When true, this skinned mesh will automatically bind to the nearest
    // ancestor skeleton in the scene hierarchy (walk up parent chain).
    // This allows armor/clothing meshes to be parented under a character
    // and automatically use that character's skeleton.
    bool UseParentSkeleton = false;
    
    // Optional: explicit skeleton GUID override (for cross-hierarchy binding)
    // If set, this takes precedence over UseParentSkeleton and SkeletonRoot.
    ClaymoreGUID SkeletonOverrideGuid = {};
    
    // Cached skeleton root from hierarchy walk (runtime only, not serialized)
    EntityID ResolvedSkeletonRoot = INVALID_ENTITY_ID;
    
    // ========================================================================
    // Bone Remapping (for skeleton retargeting)
    // ========================================================================
    // Original bone names from the mesh's source skeleton (captured at import).
    // Used to build the remap table when binding to a different skeleton.
    std::vector<std::string> OriginalBoneNames;
    
    // Original inverse bind poses from the mesh's source skeleton.
    // Needed because the target skeleton may have different bind poses.
    std::vector<glm::mat4> OriginalInverseBindPoses;
    
    // Bone remap table: OriginalBoneIndex -> TargetSkeletonBoneIndex
    // Built when the skeleton changes. -1 means bone not found in target.
    std::vector<int> BoneRemap;
    
    // The skeleton root that the current BoneRemap was built for
    EntityID RemapBuiltForSkeleton = INVALID_ENTITY_ID;
    
    // When true, the armor's original skeleton is IDENTICAL to the parent skeleton
    // (same bone names, same order, same inverse bind poses within epsilon).
    // In this case, we skip retargeting entirely and use the fast path to avoid
    // floating-point precision issues that can cause visual artifacts.
    bool SkeletonsIdentical = false;
    
    // When true, the mesh's vertex bone indices use the FULL parent skeleton ordering
    // instead of the sparse armor bone ordering. This happens when Blender exports
    // the mesh with full skeleton indices but only includes a subset of bones.
    // In this case, we skip retargeting and use parent skeleton transforms directly.
    bool UsesFullSkeletonIndices = false;
    
    uint32_t BoneCount = 0;
    int CachedMaxBoneIndex = -1;
    bool CachedMaxBoneIndexValid = false;

    // Cached mesh->skeleton transform relation. Most meshes keep a stable
    // offset relative to their skeleton, so avoid recomputing inverse/world
    // products every skinning tick when neither transform changed.
    glm::mat4 CachedMeshWorld = glm::mat4(1.0f);
    glm::mat4 CachedSkeletonWorld = glm::mat4(1.0f);
    glm::mat4 CachedSkeletonToMesh = glm::mat4(1.0f);
    bool CachedSkeletonSpaceCompatible = false;
    bool CachedMeshToSkeletonValid = false;
    
    // Runtime-only GPU skinning atlas metadata. The shared skeleton pose is
    // streamed once, then each mesh contributes only lightweight draw metadata:
    // source-slot remaps, optional retarget correction matrices, and a single
    // skeleton-to-mesh transform.
    const SkeletonComponent* GpuSourceSkeleton = nullptr;
    glm::mat4 GpuMeshFromSkeleton = glm::mat4(1.0f);
    uint32_t GpuBoneAtlasBase = 0;
    uint32_t GpuBoneAtlasCount = 0;
    uint32_t GpuRemapAtlasBase = 0;
    uint32_t GpuRemapAtlasCount = 0;
    uint32_t GpuCorrectionAtlasBase = 0;
    uint32_t GpuCorrectionAtlasCount = 0;
    bool UseGpuSharedSkeletonSource = false;
    bool GpuBoneAtlasBindingValid = false;
    uint32_t GpuInstanceAtlasRecordIndex = 0;
    bool GpuInstanceAtlasRecordValid = false;
    std::vector<uint16_t> GpuBoneIndexRemap;
    std::vector<glm::mat4> GpuBoneCorrectionPalette;
    mutable uint64_t GpuBoneIndexRemapHash = 0;
    mutable bool GpuBoneIndexRemapHashValid = false;
    mutable uint64_t GpuBoneCorrectionPaletteHash = 0;
    mutable bool GpuBoneCorrectionPaletteHashValid = false;
    bool GpuMorphRuntimeAllowed = false;
    uint32_t GpuMorphRuntimeBatchSize = 0;
    uint64_t GpuMorphRuntimeBatchKey = 0;

    bool UsesGpuSharedSkeleton() const {
        return UseGpuSharedSkeletonSource && GpuSourceSkeleton != nullptr;
    }

    bool HasGpuSkinningBinding() const {
        return GpuBoneAtlasBindingValid && GpuBoneAtlasCount > 0;
    }

    bool HasGpuSkinningInstanceRecord() const {
        return GpuInstanceAtlasRecordValid;
    }

    void ResetGpuSkinningBinding() {
        GpuBoneAtlasBase = 0;
        GpuBoneAtlasCount = 0;
        GpuRemapAtlasBase = 0;
        GpuRemapAtlasCount = 0;
        GpuCorrectionAtlasBase = 0;
        GpuCorrectionAtlasCount = 0;
        GpuBoneAtlasBindingValid = false;
        GpuInstanceAtlasRecordIndex = 0;
        GpuInstanceAtlasRecordValid = false;
    }

    void ResetGpuSharedSkeletonSource() {
        GpuSourceSkeleton = nullptr;
        GpuMeshFromSkeleton = glm::mat4(1.0f);
        UseGpuSharedSkeletonSource = false;
        ResetGpuSkinningBinding();
    }

    void InvalidateGpuRetargetHashes() {
        GpuBoneIndexRemapHash = 0;
        GpuBoneIndexRemapHashValid = false;
        GpuBoneCorrectionPaletteHash = 0;
        GpuBoneCorrectionPaletteHashValid = false;
    }

    uint64_t GetGpuBoneIndexRemapHash() const {
        if (GpuBoneIndexRemapHashValid) {
            return GpuBoneIndexRemapHash;
        }
        GpuBoneIndexRemapHash = GpuBoneIndexRemap.empty()
            ? 0ull
            : HashBytes64Stable(GpuBoneIndexRemap.data(), GpuBoneIndexRemap.size() * sizeof(uint16_t));
        GpuBoneIndexRemapHashValid = true;
        return GpuBoneIndexRemapHash;
    }

    uint64_t GetGpuBoneCorrectionPaletteHash() const {
        if (GpuBoneCorrectionPaletteHashValid) {
            return GpuBoneCorrectionPaletteHash;
        }
        GpuBoneCorrectionPaletteHash = GpuBoneCorrectionPalette.empty()
            ? 0ull
            : HashBytes64Stable(
                GpuBoneCorrectionPalette.data(),
                GpuBoneCorrectionPalette.size() * sizeof(glm::mat4));
        GpuBoneCorrectionPaletteHashValid = true;
        return GpuBoneCorrectionPaletteHash;
    }

    void ClearGpuRetargetData() {
        GpuBoneIndexRemap.clear();
        GpuBoneCorrectionPalette.clear();
        InvalidateGpuRetargetHashes();
        ResetGpuSkinningBinding();
    }
    
    // Invalidate the bone remap (call when skeleton changes)
    void InvalidateRemap() {
        BoneRemap.clear();
        RemapBuiltForSkeleton = INVALID_ENTITY_ID;
        SkeletonsIdentical = false;
        UsesFullSkeletonIndices = false;
        CachedMaxBoneIndex = -1;
        CachedMaxBoneIndexValid = false;
        CachedSkeletonSpaceCompatible = false;
        CachedMeshToSkeletonValid = false;
        ClearGpuRetargetData();
        ResetGpuSharedSkeletonSource();
    }

    // Prevent copy to keep runtime-only skinning state unique per entity.
    SkinningComponent() = default;
    SkinningComponent(const SkinningComponent&) = delete;
    SkinningComponent& operator=(const SkinningComponent&) = delete;
    SkinningComponent(SkinningComponent&& other) noexcept
        : SkeletonRoot(other.SkeletonRoot)
        , UseParentSkeleton(other.UseParentSkeleton)
        , SkeletonOverrideGuid(other.SkeletonOverrideGuid)
        , ResolvedSkeletonRoot(other.ResolvedSkeletonRoot)
        , OriginalBoneNames(std::move(other.OriginalBoneNames))
        , OriginalInverseBindPoses(std::move(other.OriginalInverseBindPoses))
        , BoneRemap(std::move(other.BoneRemap))
        , RemapBuiltForSkeleton(other.RemapBuiltForSkeleton)
        , SkeletonsIdentical(other.SkeletonsIdentical)
        , UsesFullSkeletonIndices(other.UsesFullSkeletonIndices)
        , BoneCount(other.BoneCount)
        , CachedMaxBoneIndex(other.CachedMaxBoneIndex)
        , CachedMaxBoneIndexValid(other.CachedMaxBoneIndexValid)
        , CachedMeshWorld(other.CachedMeshWorld)
        , CachedSkeletonWorld(other.CachedSkeletonWorld)
        , CachedSkeletonToMesh(other.CachedSkeletonToMesh)
        , CachedSkeletonSpaceCompatible(other.CachedSkeletonSpaceCompatible)
        , CachedMeshToSkeletonValid(other.CachedMeshToSkeletonValid)
        , GpuSourceSkeleton(other.GpuSourceSkeleton)
        , GpuMeshFromSkeleton(other.GpuMeshFromSkeleton)
        , GpuBoneAtlasBase(other.GpuBoneAtlasBase)
        , GpuBoneAtlasCount(other.GpuBoneAtlasCount)
        , GpuRemapAtlasBase(other.GpuRemapAtlasBase)
        , GpuRemapAtlasCount(other.GpuRemapAtlasCount)
        , GpuCorrectionAtlasBase(other.GpuCorrectionAtlasBase)
        , GpuCorrectionAtlasCount(other.GpuCorrectionAtlasCount)
        , UseGpuSharedSkeletonSource(other.UseGpuSharedSkeletonSource)
        , GpuBoneAtlasBindingValid(other.GpuBoneAtlasBindingValid)
        , GpuInstanceAtlasRecordIndex(other.GpuInstanceAtlasRecordIndex)
        , GpuInstanceAtlasRecordValid(other.GpuInstanceAtlasRecordValid)
        , GpuBoneIndexRemap(std::move(other.GpuBoneIndexRemap))
        , GpuBoneCorrectionPalette(std::move(other.GpuBoneCorrectionPalette))
        , GpuBoneIndexRemapHash(other.GpuBoneIndexRemapHash)
        , GpuBoneIndexRemapHashValid(other.GpuBoneIndexRemapHashValid)
        , GpuBoneCorrectionPaletteHash(other.GpuBoneCorrectionPaletteHash)
        , GpuBoneCorrectionPaletteHashValid(other.GpuBoneCorrectionPaletteHashValid)
        , GpuMorphRuntimeAllowed(false)
        , GpuMorphRuntimeBatchSize(0)
        , GpuMorphRuntimeBatchKey(0)
    {
        other.CachedSkeletonSpaceCompatible = false;
        other.CachedMeshToSkeletonValid = false;
        other.ClearGpuRetargetData();
        other.ResetGpuSharedSkeletonSource();
    }
    SkinningComponent& operator=(SkinningComponent&& other) noexcept {
        if (this != &other) {
            SkeletonRoot = other.SkeletonRoot;
            UseParentSkeleton = other.UseParentSkeleton;
            SkeletonOverrideGuid = other.SkeletonOverrideGuid;
            ResolvedSkeletonRoot = other.ResolvedSkeletonRoot;
            OriginalBoneNames = std::move(other.OriginalBoneNames);
            OriginalInverseBindPoses = std::move(other.OriginalInverseBindPoses);
            BoneRemap = std::move(other.BoneRemap);
            RemapBuiltForSkeleton = other.RemapBuiltForSkeleton;
            SkeletonsIdentical = other.SkeletonsIdentical;
            UsesFullSkeletonIndices = other.UsesFullSkeletonIndices;
            BoneCount = other.BoneCount;
            CachedMaxBoneIndex = other.CachedMaxBoneIndex;
            CachedMaxBoneIndexValid = other.CachedMaxBoneIndexValid;
            CachedMeshWorld = other.CachedMeshWorld;
            CachedSkeletonWorld = other.CachedSkeletonWorld;
            CachedSkeletonToMesh = other.CachedSkeletonToMesh;
            CachedSkeletonSpaceCompatible = other.CachedSkeletonSpaceCompatible;
            CachedMeshToSkeletonValid = other.CachedMeshToSkeletonValid;
            GpuSourceSkeleton = other.GpuSourceSkeleton;
            GpuMeshFromSkeleton = other.GpuMeshFromSkeleton;
            GpuBoneAtlasBase = other.GpuBoneAtlasBase;
            GpuBoneAtlasCount = other.GpuBoneAtlasCount;
            GpuRemapAtlasBase = other.GpuRemapAtlasBase;
            GpuRemapAtlasCount = other.GpuRemapAtlasCount;
            GpuCorrectionAtlasBase = other.GpuCorrectionAtlasBase;
            GpuCorrectionAtlasCount = other.GpuCorrectionAtlasCount;
            UseGpuSharedSkeletonSource = other.UseGpuSharedSkeletonSource;
            GpuBoneAtlasBindingValid = other.GpuBoneAtlasBindingValid;
            GpuInstanceAtlasRecordIndex = other.GpuInstanceAtlasRecordIndex;
            GpuInstanceAtlasRecordValid = other.GpuInstanceAtlasRecordValid;
            GpuBoneIndexRemap = std::move(other.GpuBoneIndexRemap);
            GpuBoneCorrectionPalette = std::move(other.GpuBoneCorrectionPalette);
            GpuBoneIndexRemapHash = other.GpuBoneIndexRemapHash;
            GpuBoneIndexRemapHashValid = other.GpuBoneIndexRemapHashValid;
            GpuBoneCorrectionPaletteHash = other.GpuBoneCorrectionPaletteHash;
            GpuBoneCorrectionPaletteHashValid = other.GpuBoneCorrectionPaletteHashValid;
            GpuMorphRuntimeAllowed = false;
            GpuMorphRuntimeBatchSize = 0;
            GpuMorphRuntimeBatchKey = 0;
            other.CachedSkeletonSpaceCompatible = false;
            other.CachedMeshToSkeletonValid = false;
            other.ClearGpuRetargetData();
            other.ResetGpuSharedSkeletonSource();
        }
        return *this;
    }
};

//------------------------------------------------------------------------------
// BoneAttachmentComponent: Attach entity to a skeleton bone without parenting
//
// This component allows a non-skinned mesh (e.g., sword, shield) to follow a
// specific bone's transform without being a child of that bone. This avoids
// the transform sensitivity issues that arise when parenting to bones due to
// different bone scales.
//
// Skeleton Resolution (automatic when SkeletonEntity is not set):
//   1. Walk up parent hierarchy to find skeleton (entity is child of skeleton)
//   2. Check siblings (entity and skeleton share the same parent)
//   3. Check children of siblings (skeleton nested under a sibling)
//
// Usage:
//   1. Add BoneAttachmentComponent to entity (e.g., sword mesh)
//   2. Set TargetBoneName to the bone to follow (e.g., "Hand_R")
//   3. Optionally set LocalOffset for position/rotation adjustment
//   4. At runtime, the entity's world transform is computed from bone transform
//
// Performance:
//   - O(1) bone lookup after initial resolution (cached by index)
//   - Transform computed during standard transform update pass
//   - No hierarchy overhead or parenting costs
//------------------------------------------------------------------------------
struct BoneAttachmentComponent {
    // Target bone identifier (by name for serialization stability)
    std::string TargetBoneName;
    
    // Local offset from the bone's transform (position, rotation, scale)
    glm::vec3 LocalPosition = glm::vec3(0.0f);
    glm::vec3 LocalRotation = glm::vec3(0.0f);  // Euler degrees for inspector ease
    glm::vec3 LocalScale = glm::vec3(1.0f);
    
    // Optional: explicit skeleton entity reference (if not using automatic search)
    // When INVALID_ENTITY_ID, the system searches parents, siblings, and sibling children
    EntityID SkeletonEntity = INVALID_ENTITY_ID;
    
    // Runtime cached state (not serialized)
    EntityID ResolvedSkeletonEntity = INVALID_ENTITY_ID;  // Cached skeleton entity
    int ResolvedBoneIndex = -1;                           // Cached bone index in skeleton
    EntityID ResolvedBoneEntity = INVALID_ENTITY_ID;      // Cached bone entity ID for direct lookup
    bool ResolutionAttempted = false;                     // Avoid repeated resolution failures
    
    // Whether to inherit rotation from bone (true) or only position (false)
    bool InheritRotation = true;
    
    // Whether to inherit scale from bone
    bool InheritScale = false;
    
    // Enable/disable the attachment (allows toggling without removing component)
    bool Enabled = true;

    // Cached local offset matrix. Bone attachment offsets usually remain static
    // for long stretches, so avoid rebuilding yaw/pitch/roll matrices every pass.
    mutable glm::vec3 CachedOffsetPosition = glm::vec3(0.0f);
    mutable glm::vec3 CachedOffsetRotation = glm::vec3(0.0f);
    mutable glm::vec3 CachedOffsetScale = glm::vec3(1.0f);
    mutable glm::mat4 CachedLocalOffsetMatrix = glm::mat4(1.0f);
    mutable bool CachedLocalOffsetValid = false;
    
    // Compute the local offset matrix
    const glm::mat4& GetLocalOffsetMatrix() const {
        if (!CachedLocalOffsetValid ||
            CachedOffsetPosition != LocalPosition ||
            CachedOffsetRotation != LocalRotation ||
            CachedOffsetScale != LocalScale) {
            glm::mat4 t = glm::translate(glm::mat4(1.0f), LocalPosition);
            glm::mat4 r = glm::yawPitchRoll(
                glm::radians(LocalRotation.y),
                glm::radians(LocalRotation.x),
                glm::radians(LocalRotation.z)
            );
            glm::mat4 s = glm::scale(glm::mat4(1.0f), LocalScale);
            CachedLocalOffsetMatrix = t * r * s;
            CachedOffsetPosition = LocalPosition;
            CachedOffsetRotation = LocalRotation;
            CachedOffsetScale = LocalScale;
            CachedLocalOffsetValid = true;
        }
        return CachedLocalOffsetMatrix;
    }
    
    // Invalidate cached resolution (call when skeleton changes)
    void InvalidateResolution() {
        ResolvedSkeletonEntity = INVALID_ENTITY_ID;
        ResolvedBoneIndex = -1;
        ResolvedBoneEntity = INVALID_ENTITY_ID;
        ResolutionAttempted = false;
    }
};
