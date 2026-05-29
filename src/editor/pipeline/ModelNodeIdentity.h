#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "core/assets/AssetReference.h"
#include <nlohmann/json.hpp>

/// @brief Stable identity for a node within an imported model
/// 
/// This structure provides multiple ways to identify a node across model re-exports:
/// 1. NodePath - hierarchical path from model root (e.g., "Armature/Hips/Spine")
/// 2. NormalizedPath - path with trailing _### suffixes stripped
/// 3. ContentHash - hash of node's mesh indices and local transform
/// 4. DerivedGUID - stable GUID computed from path + parent + content
/// 
/// When matching nodes during reimport, we try these identifiers in order
/// to handle cases where the artist renamed nodes or restructured hierarchy.
struct ModelNodeIdentity {
    /// Hierarchical path from model root (e.g., "Armature/Hips/Spine")
    std::string NodePath;
    
    /// Path with trailing _### suffixes stripped for fuzzy matching
    std::string NormalizedPath;
    
    /// Node name only (leaf of path)
    std::string NodeName;
    
    /// Normalized node name (trailing _### stripped)
    std::string NormalizedName;
    
    /// Hash of node's structural content (mesh indices, vertex count hints)
    uint64_t ContentHash = 0;
    
    /// Stable GUID derived from path + content
    /// This remains stable as long as the node's position in hierarchy doesn't change
    ClaymoreGUID DerivedGUID;
    
    /// Index of mesh in the source file (-1 if not a mesh node)
    int MeshFileId = -1;
    
    /// Whether this node is skinned
    bool Skinned = false;
    
    /// Depth in hierarchy (0 = root)
    int Depth = 0;
    
    /// Parent's derived GUID (zero for root)
    ClaymoreGUID ParentGUID;
    
    /// JSON serialization
    nlohmann::json ToJson() const;
    static ModelNodeIdentity FromJson(const nlohmann::json& j);
    
    /// Compute normalized path by stripping _### suffixes from each segment
    static std::string NormalizePath(const std::string& path);
    
    /// Compute normalized name by stripping trailing _### suffix
    static std::string NormalizeName(const std::string& name);
    
    /// Compute content hash from mesh indices and local transform
    static uint64_t ComputeContentHash(const std::vector<int>& meshIndices, 
                                       const float* localMatrix16,
                                       int vertexCountHint);
    
    /// Generate a stable GUID from path and content
    static ClaymoreGUID GenerateDerivedGUID(const std::string& nodePath,
                                            uint64_t contentHash,
                                            const ClaymoreGUID& parentGUID);
};

/// @brief Collection of node identities for a model, used for reimport matching
struct ModelIdentityMap {
    /// Source model path (virtual path)
    std::string SourcePath;
    
    /// Model GUID
    ClaymoreGUID ModelGUID;
    
    /// All node identities in this model
    std::vector<ModelNodeIdentity> Nodes;
    
    /// Quick lookup by derived GUID
    std::unordered_map<uint64_t, size_t> GuidToIndex;
    
    /// Quick lookup by normalized path
    std::unordered_map<std::string, size_t> PathToIndex;
    
    /// Quick lookup by content hash
    std::unordered_map<uint64_t, std::vector<size_t>> HashToIndices;
    
    /// Build lookup tables after populating Nodes
    void BuildLookups();
    
    /// Find node by trying identifiers in order of reliability
    /// Returns nullptr if no match found
    const ModelNodeIdentity* FindNode(const ModelNodeIdentity& query) const;
    
    /// Find node by derived GUID only
    const ModelNodeIdentity* FindByGUID(const ClaymoreGUID& guid) const;
    
    /// Find node by normalized path
    const ModelNodeIdentity* FindByPath(const std::string& normalizedPath) const;
    
    /// Find nodes by content hash (may return multiple candidates)
    std::vector<const ModelNodeIdentity*> FindByHash(uint64_t hash) const;
    
    /// JSON serialization (stored in .meta file)
    nlohmann::json ToJson() const;
    static ModelIdentityMap FromJson(const nlohmann::json& j);
};

/// @brief Result of matching old identities to new model structure
struct NodeMatchResult {
    /// Derived GUID from old identity
    ClaymoreGUID OldGUID;
    
    /// Matched derived GUID in new model (zero if not found)
    ClaymoreGUID NewGUID;
    
    /// How the match was found
    enum class MatchType {
        ExactGUID,      ///< Same derived GUID
        ExactPath,      ///< Same normalized path
        ContentHash,    ///< Same content hash (node may have been renamed)
        FuzzyName,      ///< Same name at different location
        NotFound        ///< No match found
    };
    MatchType Type = MatchType::NotFound;
    
    /// Confidence score (1.0 = certain, lower = less confident)
    float Confidence = 0.0f;
};

/// @brief Match nodes from old identity map to new one
/// @param oldMap Identity map from previous import (from saved .meta)
/// @param newMap Identity map from current import
/// @return Vector of match results for each node in oldMap
std::vector<NodeMatchResult> MatchNodeIdentities(
    const ModelIdentityMap& oldMap,
    const ModelIdentityMap& newMap);

