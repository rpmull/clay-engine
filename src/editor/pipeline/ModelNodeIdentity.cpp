#include "ModelNodeIdentity.h"
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cctype>

// FNV-1a hash for stable hashing
static uint64_t fnv1a_64(const void* data, size_t len) {
    const uint64_t FNV_PRIME = 0x100000001b3ULL;
    const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    
    uint64_t hash = FNV_OFFSET;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint64_t fnv1a_string(const std::string& s) {
    return fnv1a_64(s.data(), s.size());
}

// Pack GUID for map key
static uint64_t packGUID(const ClaymoreGUID& g) {
    return g.high ^ (g.low << 1);
}

std::string ModelNodeIdentity::NormalizePath(const std::string& path) {
    if (path.empty()) return path;
    
    std::string result;
    result.reserve(path.size());
    
    std::stringstream ss(path);
    std::string segment;
    bool first = true;
    
    while (std::getline(ss, segment, '/')) {
        std::string normalized = NormalizeName(segment);
        if (!first) result += '/';
        result += normalized;
        first = false;
    }
    
    return result;
}

std::string ModelNodeIdentity::NormalizeName(const std::string& name) {
    if (name.empty()) return name;
    
    // Find last underscore
    size_t lastUnderscore = name.find_last_of('_');
    if (lastUnderscore == std::string::npos || lastUnderscore == name.size() - 1) {
        return name;
    }
    
    // Check if everything after underscore is digits
    bool allDigits = true;
    for (size_t i = lastUnderscore + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            allDigits = false;
            break;
        }
    }
    
    if (allDigits) {
        return name.substr(0, lastUnderscore);
    }
    return name;
}

uint64_t ModelNodeIdentity::ComputeContentHash(
    const std::vector<int>& meshIndices,
    const float* localMatrix16,
    int vertexCountHint)
{
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV offset
    
    // Hash mesh indices
    for (int idx : meshIndices) {
        hash = fnv1a_64(&idx, sizeof(idx));
    }
    
    // Hash local transform (quantized to avoid floating point noise)
    if (localMatrix16) {
        for (int i = 0; i < 16; ++i) {
            // Quantize to 3 decimal places
            int32_t quantized = static_cast<int32_t>(localMatrix16[i] * 1000.0f);
            hash ^= fnv1a_64(&quantized, sizeof(quantized));
        }
    }
    
    // Include vertex count hint
    hash ^= fnv1a_64(&vertexCountHint, sizeof(vertexCountHint));
    
    return hash;
}

ClaymoreGUID ModelNodeIdentity::GenerateDerivedGUID(
    const std::string& nodePath,
    uint64_t contentHash,
    const ClaymoreGUID& parentGUID)
{
    // Combine path, content hash, and parent for unique identity
    uint64_t pathHash = fnv1a_string(nodePath);
    
    ClaymoreGUID result;
    result.high = pathHash ^ contentHash;
    result.low = parentGUID.high ^ parentGUID.low ^ (contentHash >> 32);
    
    // Ensure non-zero
    if (result.high == 0 && result.low == 0) {
        result.high = pathHash;
        result.low = contentHash;
    }
    
    return result;
}

nlohmann::json ModelNodeIdentity::ToJson() const {
    nlohmann::json j;
    j["path"] = NodePath;
    j["normalizedPath"] = NormalizedPath;
    j["name"] = NodeName;
    j["normalizedName"] = NormalizedName;
    j["contentHash"] = ContentHash;
    j["derivedGuid"] = DerivedGUID.ToString();
    j["meshFileId"] = MeshFileId;
    j["skinned"] = Skinned;
    j["depth"] = Depth;
    if (!(ParentGUID.high == 0 && ParentGUID.low == 0)) {
        j["parentGuid"] = ParentGUID.ToString();
    }
    return j;
}

ModelNodeIdentity ModelNodeIdentity::FromJson(const nlohmann::json& j) {
    ModelNodeIdentity node;
    node.NodePath = j.value("path", std::string());
    node.NormalizedPath = j.value("normalizedPath", NormalizePath(node.NodePath));
    node.NodeName = j.value("name", std::string());
    node.NormalizedName = j.value("normalizedName", NormalizeName(node.NodeName));
    node.ContentHash = j.value("contentHash", 0ULL);
    
    if (j.contains("derivedGuid") && j["derivedGuid"].is_string()) {
        node.DerivedGUID = ClaymoreGUID::FromString(j["derivedGuid"].get<std::string>());
    }
    
    node.MeshFileId = j.value("meshFileId", -1);
    node.Skinned = j.value("skinned", false);
    node.Depth = j.value("depth", 0);
    
    if (j.contains("parentGuid") && j["parentGuid"].is_string()) {
        node.ParentGUID = ClaymoreGUID::FromString(j["parentGuid"].get<std::string>());
    }
    
    return node;
}

void ModelIdentityMap::BuildLookups() {
    GuidToIndex.clear();
    PathToIndex.clear();
    HashToIndices.clear();
    
    for (size_t i = 0; i < Nodes.size(); ++i) {
        const auto& node = Nodes[i];
        
        // GUID lookup
        uint64_t packedGuid = packGUID(node.DerivedGUID);
        GuidToIndex[packedGuid] = i;
        
        // Path lookup (normalized)
        if (!node.NormalizedPath.empty()) {
            PathToIndex[node.NormalizedPath] = i;
        }
        
        // Hash lookup (can have multiple nodes with same hash)
        if (node.ContentHash != 0) {
            HashToIndices[node.ContentHash].push_back(i);
        }
    }
}

const ModelNodeIdentity* ModelIdentityMap::FindNode(const ModelNodeIdentity& query) const {
    // 1. Try exact GUID match
    if (auto* node = FindByGUID(query.DerivedGUID)) {
        return node;
    }
    
    // 2. Try normalized path match
    if (!query.NormalizedPath.empty()) {
        if (auto* node = FindByPath(query.NormalizedPath)) {
            return node;
        }
    }
    
    // 3. Try content hash match (pick first if multiple)
    if (query.ContentHash != 0) {
        auto candidates = FindByHash(query.ContentHash);
        if (candidates.size() == 1) {
            return candidates[0];
        }
        // If multiple candidates, try to disambiguate by name
        for (auto* candidate : candidates) {
            if (candidate->NormalizedName == query.NormalizedName) {
                return candidate;
            }
        }
        // Just return first if can't disambiguate
        if (!candidates.empty()) {
            return candidates[0];
        }
    }
    
    return nullptr;
}

const ModelNodeIdentity* ModelIdentityMap::FindByGUID(const ClaymoreGUID& guid) const {
    if (guid.high == 0 && guid.low == 0) return nullptr;
    
    uint64_t packed = packGUID(guid);
    auto it = GuidToIndex.find(packed);
    if (it != GuidToIndex.end() && it->second < Nodes.size()) {
        return &Nodes[it->second];
    }
    return nullptr;
}

const ModelNodeIdentity* ModelIdentityMap::FindByPath(const std::string& normalizedPath) const {
    auto it = PathToIndex.find(normalizedPath);
    if (it != PathToIndex.end() && it->second < Nodes.size()) {
        return &Nodes[it->second];
    }
    return nullptr;
}

std::vector<const ModelNodeIdentity*> ModelIdentityMap::FindByHash(uint64_t hash) const {
    std::vector<const ModelNodeIdentity*> result;
    auto it = HashToIndices.find(hash);
    if (it != HashToIndices.end()) {
        for (size_t idx : it->second) {
            if (idx < Nodes.size()) {
                result.push_back(&Nodes[idx]);
            }
        }
    }
    return result;
}

nlohmann::json ModelIdentityMap::ToJson() const {
    nlohmann::json j;
    j["sourcePath"] = SourcePath;
    j["modelGuid"] = ModelGUID.ToString();
    j["nodes"] = nlohmann::json::array();
    for (const auto& node : Nodes) {
        j["nodes"].push_back(node.ToJson());
    }
    return j;
}

ModelIdentityMap ModelIdentityMap::FromJson(const nlohmann::json& j) {
    ModelIdentityMap map;
    map.SourcePath = j.value("sourcePath", std::string());
    
    if (j.contains("modelGuid") && j["modelGuid"].is_string()) {
        map.ModelGUID = ClaymoreGUID::FromString(j["modelGuid"].get<std::string>());
    }
    
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const auto& nodeJ : j["nodes"]) {
            map.Nodes.push_back(ModelNodeIdentity::FromJson(nodeJ));
        }
    }
    
    map.BuildLookups();
    return map;
}

std::vector<NodeMatchResult> MatchNodeIdentities(
    const ModelIdentityMap& oldMap,
    const ModelIdentityMap& newMap)
{
    std::vector<NodeMatchResult> results;
    results.reserve(oldMap.Nodes.size());
    
    for (const auto& oldNode : oldMap.Nodes) {
        NodeMatchResult result;
        result.OldGUID = oldNode.DerivedGUID;
        
        // 1. Try exact GUID match
        if (auto* match = newMap.FindByGUID(oldNode.DerivedGUID)) {
            result.NewGUID = match->DerivedGUID;
            result.Type = NodeMatchResult::MatchType::ExactGUID;
            result.Confidence = 1.0f;
            results.push_back(result);
            continue;
        }
        
        // 2. Try exact path match
        if (!oldNode.NormalizedPath.empty()) {
            if (auto* match = newMap.FindByPath(oldNode.NormalizedPath)) {
                result.NewGUID = match->DerivedGUID;
                result.Type = NodeMatchResult::MatchType::ExactPath;
                result.Confidence = 0.95f;
                results.push_back(result);
                continue;
            }
        }
        
        // 3. Try content hash match
        if (oldNode.ContentHash != 0) {
            auto candidates = newMap.FindByHash(oldNode.ContentHash);
            if (candidates.size() == 1) {
                result.NewGUID = candidates[0]->DerivedGUID;
                result.Type = NodeMatchResult::MatchType::ContentHash;
                result.Confidence = 0.85f;
                results.push_back(result);
                continue;
            }
            
            // Multiple candidates - try to find by name
            for (auto* candidate : candidates) {
                if (candidate->NormalizedName == oldNode.NormalizedName) {
                    result.NewGUID = candidate->DerivedGUID;
                    result.Type = NodeMatchResult::MatchType::ContentHash;
                    result.Confidence = 0.9f;
                    results.push_back(result);
                    goto next_node;
                }
            }
        }
        
        // 4. Last resort: fuzzy name match anywhere in new model
        for (const auto& newNode : newMap.Nodes) {
            if (newNode.NormalizedName == oldNode.NormalizedName) {
                result.NewGUID = newNode.DerivedGUID;
                result.Type = NodeMatchResult::MatchType::FuzzyName;
                result.Confidence = 0.5f;
                results.push_back(result);
                goto next_node;
            }
        }
        
        // No match found
        result.Type = NodeMatchResult::MatchType::NotFound;
        result.Confidence = 0.0f;
        results.push_back(result);
        
        next_node:;
    }
    
    return results;
}

