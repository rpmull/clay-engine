#include "ModelDelta.h"
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cctype>
#include <chrono>

namespace cm::model {

// =============================================================================
// FNV-1a Hash Utilities
// =============================================================================

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

static uint64_t fnv1a_combine(uint64_t h1, uint64_t h2) {
    const uint64_t FNV_PRIME = 0x100000001b3ULL;
    uint64_t hash = h1;
    hash ^= h2;
    hash *= FNV_PRIME;
    return hash;
}

static uint64_t fnv1a_string(const std::string& s) {
    return fnv1a_64(s.data(), s.size());
}

// =============================================================================
// StableNodeId Implementation
// =============================================================================

std::string StableNodeId::ComputeNormalizedPath(const std::string& path) {
    if (path.empty()) return path;
    
    std::string result;
    result.reserve(path.size());
    
    std::stringstream ss(path);
    std::string segment;
    bool first = true;
    
    while (std::getline(ss, segment, '/')) {
        std::string normalized = ComputeNormalizedName(segment);
        if (!first) result += '/';
        result += normalized;
        first = false;
    }
    
    return result;
}

std::string StableNodeId::ComputeNormalizedName(const std::string& name) {
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

uint64_t StableNodeId::ComputeContentHash(int meshFileId, const glm::mat4& localMatrix, int vertexCountHint) {
    uint64_t hash = 0xcbf29ce484222325ULL; // FNV offset
    
    // Hash mesh file ID
    hash = fnv1a_combine(hash, fnv1a_64(&meshFileId, sizeof(meshFileId)));
    
    // Hash local transform (quantized to avoid floating point noise)
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            int32_t quantized = static_cast<int32_t>(localMatrix[i][j] * 1000.0f);
            hash = fnv1a_combine(hash, fnv1a_64(&quantized, sizeof(quantized)));
        }
    }
    
    // Include vertex count hint
    hash = fnv1a_combine(hash, fnv1a_64(&vertexCountHint, sizeof(vertexCountHint)));
    
    return hash;
}

uint64_t StableNodeId::ComputeStructuralHash(const std::string& parentNormalizedPath, int siblingIndex, NodeType type) {
    uint64_t hash = fnv1a_string(parentNormalizedPath);
    hash = fnv1a_combine(hash, fnv1a_64(&siblingIndex, sizeof(siblingIndex)));
    hash = fnv1a_combine(hash, fnv1a_64(&type, sizeof(type)));
    return hash;
}

bool StableNodeId::operator==(const StableNodeId& other) const {
    return path == other.path &&
           normalizedPath == other.normalizedPath &&
           contentHash == other.contentHash &&
           type == other.type;
}

StableNodeId::MatchConfidence StableNodeId::GetMatchConfidence(const StableNodeId& other) const {
    // For user-added entities, GUID is authoritative
    if (type == NodeType::UserAdded && 
        !(entityGuid.high == 0 && entityGuid.low == 0) &&
        entityGuid.high == other.entityGuid.high && 
        entityGuid.low == other.entityGuid.low) {
        return MatchConfidence::ExactGuid;
    }
    
    // Exact path match is highest confidence for model nodes
    if (!path.empty() && path == other.path) {
        return MatchConfidence::ExactPath;
    }
    
    // Normalized path match
    if (!normalizedPath.empty() && normalizedPath == other.normalizedPath) {
        return MatchConfidence::NormalizedPath;
    }
    
    // Structural hash match (same position in hierarchy)
    if (structuralHash != 0 && structuralHash == other.structuralHash) {
        return MatchConfidence::StructuralHash;
    }
    
    // Content hash match (same geometry)
    if (contentHash != 0 && contentHash == other.contentHash) {
        return MatchConfidence::ContentHash;
    }
    
    // Fuzzy name match (last resort)
    if (!normalizedName.empty() && normalizedName == other.normalizedName) {
        return MatchConfidence::FuzzyName;
    }
    
    return MatchConfidence::None;
}

nlohmann::json StableNodeId::ToJson() const {
    nlohmann::json j;
    j["path"] = path;
    j["normalizedPath"] = normalizedPath;
    j["nodeName"] = nodeName;
    j["normalizedName"] = normalizedName;
    j["contentHash"] = contentHash;
    j["structuralHash"] = structuralHash;
    j["type"] = static_cast<uint8_t>(type);
    j["meshFileId"] = meshFileId;
    j["depth"] = depth;
    if (!(entityGuid.high == 0 && entityGuid.low == 0)) {
        j["entityGuid"] = entityGuid.ToString();
    }
    return j;
}

StableNodeId StableNodeId::FromJson(const nlohmann::json& j) {
    StableNodeId id;
    id.path = j.value("path", std::string());
    id.normalizedPath = j.value("normalizedPath", ComputeNormalizedPath(id.path));
    id.nodeName = j.value("nodeName", std::string());
    id.normalizedName = j.value("normalizedName", ComputeNormalizedName(id.nodeName));
    id.contentHash = j.value("contentHash", 0ULL);
    id.structuralHash = j.value("structuralHash", 0ULL);
    id.type = static_cast<NodeType>(j.value("type", static_cast<uint8_t>(NodeType::Unknown)));
    id.meshFileId = j.value("meshFileId", -1);
    id.depth = j.value("depth", 0);
    
    if (j.contains("entityGuid") && j["entityGuid"].is_string()) {
        id.entityGuid = ClaymoreGUID::FromString(j["entityGuid"].get<std::string>());
    }
    
    return id;
}

// =============================================================================
// TransformDelta Implementation
// =============================================================================

nlohmann::json TransformDelta::ToJson() const {
    nlohmann::json j;
    if (positionModified) {
        j["position"] = { position.x, position.y, position.z };
    }
    if (rotationModified) {
        j["rotation"] = { rotation.w, rotation.x, rotation.y, rotation.z };
    }
    if (scaleModified) {
        j["scale"] = { scale.x, scale.y, scale.z };
    }
    return j;
}

TransformDelta TransformDelta::FromJson(const nlohmann::json& j) {
    TransformDelta delta;
    
    if (j.contains("position") && j["position"].is_array() && j["position"].size() >= 3) {
        delta.position = glm::vec3(j["position"][0], j["position"][1], j["position"][2]);
        delta.positionModified = true;
    }
    
    if (j.contains("rotation") && j["rotation"].is_array() && j["rotation"].size() >= 4) {
        delta.rotation = glm::quat(j["rotation"][0], j["rotation"][1], j["rotation"][2], j["rotation"][3]);
        delta.rotationModified = true;
    }
    
    if (j.contains("scale") && j["scale"].is_array() && j["scale"].size() >= 3) {
        delta.scale = glm::vec3(j["scale"][0], j["scale"][1], j["scale"][2]);
        delta.scaleModified = true;
    }
    
    return delta;
}

// =============================================================================
// MeshMaterialDelta Implementation
// =============================================================================

nlohmann::json MeshMaterialDelta::ToJson() const {
    nlohmann::json j;
    j["slotIndex"] = slotIndex;
    if (!materialAssetPath.empty()) {
        j["materialAssetPath"] = materialAssetPath;
    }
    if (!texturePaths.empty()) {
        j["texturePaths"] = texturePaths;
    }
    if (alphaBlendEnabled) {
        j["alphaBlendEnabled"] = true;
    }
    // PropertyBlock serialization - serialize texture paths and uniforms
    // (Full PropertyBlock serialization would be more complex)
    return j;
}

MeshMaterialDelta MeshMaterialDelta::FromJson(const nlohmann::json& j) {
    MeshMaterialDelta delta;
    delta.slotIndex = j.value("slotIndex", 0u);
    delta.materialAssetPath = j.value("materialAssetPath", std::string());
    delta.alphaBlendEnabled = j.value("alphaBlendEnabled", false);
    
    if (j.contains("texturePaths") && j["texturePaths"].is_object()) {
        for (auto& [key, val] : j["texturePaths"].items()) {
            if (val.is_string()) {
                delta.texturePaths[key] = val.get<std::string>();
            }
        }
    }
    
    return delta;
}

// =============================================================================
// MeshDelta Implementation
// =============================================================================

nlohmann::json MeshDelta::ToJson() const {
    nlohmann::json j;
    
    if (!materialOverrides.empty()) {
        j["materialOverrides"] = nlohmann::json::array();
        for (const auto& mo : materialOverrides) {
            j["materialOverrides"].push_back(mo.ToJson());
        }
    }
    
    if (showBackfaces) j["showBackfaces"] = true;
    if (renderOnTop) j["renderOnTop"] = true;
    if (renderOrder != 0) j["renderOrder"] = renderOrder;
    if (boundsPadding != 1.0f) j["boundsPadding"] = boundsPadding;
    if (uniqueMaterial) j["uniqueMaterial"] = true;
    
    return j;
}

MeshDelta MeshDelta::FromJson(const nlohmann::json& j) {
    MeshDelta delta;
    
    if (j.contains("materialOverrides") && j["materialOverrides"].is_array()) {
        for (const auto& mo : j["materialOverrides"]) {
            delta.materialOverrides.push_back(MeshMaterialDelta::FromJson(mo));
        }
    }
    
    delta.showBackfaces = j.value("showBackfaces", false);
    delta.renderOnTop = j.value("renderOnTop", false);
    delta.renderOrder = j.value("renderOrder", 0);
    delta.boundsPadding = j.value("boundsPadding", 1.0f);
    delta.uniqueMaterial = j.value("uniqueMaterial", false);
    
    return delta;
}

// =============================================================================
// RenderOverridesDelta Implementation
// =============================================================================

nlohmann::json RenderOverridesDelta::ToJson() const {
    nlohmann::json j;
    j["alphaBlendEnabled"] = alphaBlendEnabled;
    j["useAlphaCutout"] = useAlphaCutout;
    j["alphaCutoutThreshold"] = alphaCutoutThreshold;
    j["depthWriteEnabled"] = depthWriteEnabled;
    j["castShadows"] = castShadows;
    j["receiveShadows"] = receiveShadows;
    j["visible"] = visible;
    j["sortingOrder"] = sortingOrder;
    return j;
}

RenderOverridesDelta RenderOverridesDelta::FromJson(const nlohmann::json& j) {
    RenderOverridesDelta delta;
    delta.alphaBlendEnabled = j.value("alphaBlendEnabled", false);
    delta.useAlphaCutout = j.value("useAlphaCutout", false);
    delta.alphaCutoutThreshold = j.value("alphaCutoutThreshold", 0.5f);
    delta.depthWriteEnabled = j.value("depthWriteEnabled", true);
    delta.castShadows = j.value("castShadows", true);
    delta.receiveShadows = j.value("receiveShadows", true);
    delta.visible = j.value("visible", true);
    delta.sortingOrder = j.value("sortingOrder", 0);
    return delta;
}

// =============================================================================
// StableEntityRef Implementation
// =============================================================================

nlohmann::json StableEntityRef::ToJson() const {
    nlohmann::json j;
    if (!(targetGuid.high == 0 && targetGuid.low == 0)) {
        j["guid"] = targetGuid.ToString();
    }
    if (!modelNodePath.empty()) {
        j["modelNodePath"] = modelNodePath;
    }
    if (!(modelGuid.high == 0 && modelGuid.low == 0)) {
        j["modelGuid"] = modelGuid.ToString();
    }
    if (!scenePath.empty()) {
        j["scenePath"] = scenePath;
    }
    return j;
}

StableEntityRef StableEntityRef::FromJson(const nlohmann::json& j) {
    StableEntityRef ref;
    
    if (j.contains("guid") && j["guid"].is_string()) {
        ref.targetGuid = ClaymoreGUID::FromString(j["guid"].get<std::string>());
    }
    ref.modelNodePath = j.value("modelNodePath", std::string());
    if (j.contains("modelGuid") && j["modelGuid"].is_string()) {
        ref.modelGuid = ClaymoreGUID::FromString(j["modelGuid"].get<std::string>());
    }
    ref.scenePath = j.value("scenePath", std::string());
    
    return ref;
}

// =============================================================================
// LookAtConstraintDelta Implementation
// =============================================================================

nlohmann::json LookAtConstraintDelta::ToJson() const {
    nlohmann::json j;
    j["enabled"] = enabled;
    j["weight"] = weight;
    j["mode"] = mode;
    j["axes"] = axes;
    j["space"] = space;
    j["target"] = target.ToJson();
    j["smoothingSpeed"] = smoothingSpeed;
    j["maxYawDeg"] = maxYawDeg;
    j["maxPitchDeg"] = maxPitchDeg;
    if (!boneChain.empty()) {
        j["boneChain"] = boneChain;
    }
    return j;
}

LookAtConstraintDelta LookAtConstraintDelta::FromJson(const nlohmann::json& j) {
    LookAtConstraintDelta delta;
    delta.enabled = j.value("enabled", true);
    delta.weight = j.value("weight", 1.0f);
    delta.mode = j.value("mode", static_cast<uint8_t>(0));
    delta.axes = j.value("axes", static_cast<uint8_t>(0));
    delta.space = j.value("space", static_cast<uint8_t>(0));
    if (j.contains("target") && j["target"].is_object()) {
        delta.target = StableEntityRef::FromJson(j["target"]);
    }
    delta.smoothingSpeed = j.value("smoothingSpeed", 10.0f);
    delta.maxYawDeg = j.value("maxYawDeg", 90.0f);
    delta.maxPitchDeg = j.value("maxPitchDeg", 60.0f);
    if (j.contains("boneChain") && j["boneChain"].is_array()) {
        for (const auto& b : j["boneChain"]) {
            delta.boneChain.push_back(b.get<int32_t>());
        }
    }
    return delta;
}

// =============================================================================
// IKConstraintDelta Implementation
// =============================================================================

nlohmann::json IKConstraintDelta::ToJson() const {
    nlohmann::json j;
    j["enabled"] = enabled;
    j["weight"] = weight;
    j["target"] = target.ToJson();
    j["pole"] = pole.ToJson();
    j["useTwoBone"] = useTwoBone;
    j["maxIterations"] = maxIterations;
    j["tolerance"] = tolerance;
    j["damping"] = damping;
    j["lockAxisX"] = lockAxisX;
    j["lockAxisY"] = lockAxisY;
    j["lockAxisZ"] = lockAxisZ;
    if (!chain.empty()) {
        j["chain"] = chain;
    }
    if (chainRootHint >= 0) j["chainRootHint"] = chainRootHint;
    if (chainEffectorHint >= 0) j["chainEffectorHint"] = chainEffectorHint;
    return j;
}

IKConstraintDelta IKConstraintDelta::FromJson(const nlohmann::json& j) {
    IKConstraintDelta delta;
    delta.enabled = j.value("enabled", true);
    delta.weight = j.value("weight", 1.0f);
    if (j.contains("target") && j["target"].is_object()) {
        delta.target = StableEntityRef::FromJson(j["target"]);
    }
    if (j.contains("pole") && j["pole"].is_object()) {
        delta.pole = StableEntityRef::FromJson(j["pole"]);
    }
    delta.useTwoBone = j.value("useTwoBone", true);
    delta.maxIterations = j.value("maxIterations", 12.0f);
    delta.tolerance = j.value("tolerance", 0.001f);
    delta.damping = j.value("damping", 0.2f);
    delta.lockAxisX = j.value("lockAxisX", false);
    delta.lockAxisY = j.value("lockAxisY", false);
    delta.lockAxisZ = j.value("lockAxisZ", false);
    if (j.contains("chain") && j["chain"].is_array()) {
        for (const auto& b : j["chain"]) {
            delta.chain.push_back(b.get<int32_t>());
        }
    }
    delta.chainRootHint = j.value("chainRootHint", -1);
    delta.chainEffectorHint = j.value("chainEffectorHint", -1);
    return delta;
}

// =============================================================================
// BoneAttachmentDelta Implementation
// =============================================================================

nlohmann::json BoneAttachmentDelta::ToJson() const {
    nlohmann::json j;
    j["targetBoneName"] = targetBoneName;
    j["skeletonRef"] = skeletonRef.ToJson();
    j["localPosition"] = { localPosition.x, localPosition.y, localPosition.z };
    j["localRotation"] = { localRotation.x, localRotation.y, localRotation.z };
    j["localScale"] = { localScale.x, localScale.y, localScale.z };
    j["inheritRotation"] = inheritRotation;
    j["inheritScale"] = inheritScale;
    j["enabled"] = enabled;
    return j;
}

BoneAttachmentDelta BoneAttachmentDelta::FromJson(const nlohmann::json& j) {
    BoneAttachmentDelta delta;
    delta.targetBoneName = j.value("targetBoneName", std::string());
    if (j.contains("skeletonRef") && j["skeletonRef"].is_object()) {
        delta.skeletonRef = StableEntityRef::FromJson(j["skeletonRef"]);
    }
    if (j.contains("localPosition") && j["localPosition"].is_array() && j["localPosition"].size() >= 3) {
        delta.localPosition = glm::vec3(j["localPosition"][0], j["localPosition"][1], j["localPosition"][2]);
    }
    if (j.contains("localRotation") && j["localRotation"].is_array() && j["localRotation"].size() >= 3) {
        delta.localRotation = glm::vec3(j["localRotation"][0], j["localRotation"][1], j["localRotation"][2]);
    }
    if (j.contains("localScale") && j["localScale"].is_array() && j["localScale"].size() >= 3) {
        delta.localScale = glm::vec3(j["localScale"][0], j["localScale"][1], j["localScale"][2]);
    }
    delta.inheritRotation = j.value("inheritRotation", true);
    delta.inheritScale = j.value("inheritScale", false);
    delta.enabled = j.value("enabled", true);
    return delta;
}

// =============================================================================
// ScriptDelta Implementation
// =============================================================================

nlohmann::json ScriptDelta::ToJson() const {
    nlohmann::json j;
    j["className"] = className;
    if (!properties.empty()) {
        j["properties"] = properties;
    }
    return j;
}

ScriptDelta ScriptDelta::FromJson(const nlohmann::json& j) {
    ScriptDelta delta;
    delta.className = j.value("className", std::string());
    if (j.contains("properties") && j["properties"].is_object()) {
        for (auto& [key, val] : j["properties"].items()) {
            delta.properties[key] = val;
        }
    }
    return delta;
}

// =============================================================================
// BlendShapeDelta Implementation
// =============================================================================

nlohmann::json BlendShapeDelta::ToJson() const {
    nlohmann::json j;
    if (!weights.empty()) {
        j["weights"] = weights;
    }
    return j;
}

BlendShapeDelta BlendShapeDelta::FromJson(const nlohmann::json& j) {
    BlendShapeDelta delta;
    if (j.contains("weights") && j["weights"].is_object()) {
        for (auto& [key, val] : j["weights"].items()) {
            if (val.is_number()) {
                delta.weights[key] = val.get<float>();
            }
        }
    }
    return delta;
}

// =============================================================================
// AddedLightDelta Implementation
// =============================================================================

nlohmann::json AddedLightDelta::ToJson() const {
    nlohmann::json j;
    j["type"] = type;
    j["color"] = { color.r, color.g, color.b };
    j["intensity"] = intensity;
    return j;
}

AddedLightDelta AddedLightDelta::FromJson(const nlohmann::json& j) {
    AddedLightDelta delta;
    delta.type = j.value("type", 0);
    if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3) {
        delta.color = glm::vec3(j["color"][0], j["color"][1], j["color"][2]);
    }
    delta.intensity = j.value("intensity", 1.0f);
    return delta;
}

// =============================================================================
// AddedColliderDelta Implementation
// =============================================================================

nlohmann::json AddedColliderDelta::ToJson() const {
    nlohmann::json j;
    j["shapeType"] = shapeType;
    j["offset"] = { offset.x, offset.y, offset.z };
    j["size"] = { size.x, size.y, size.z };
    j["radius"] = radius;
    j["height"] = height;
    j["isTrigger"] = isTrigger;
    j["physicsLayerName"] = physicsLayerName;
    return j;
}

AddedColliderDelta AddedColliderDelta::FromJson(const nlohmann::json& j) {
    AddedColliderDelta delta;
    delta.shapeType = j.value("shapeType", 0);
    if (j.contains("offset") && j["offset"].is_array() && j["offset"].size() >= 3) {
        delta.offset = glm::vec3(j["offset"][0], j["offset"][1], j["offset"][2]);
    }
    if (j.contains("size") && j["size"].is_array() && j["size"].size() >= 3) {
        delta.size = glm::vec3(j["size"][0], j["size"][1], j["size"][2]);
    }
    delta.radius = j.value("radius", 0.5f);
    delta.height = j.value("height", 1.0f);
    delta.isTrigger = j.value("isTrigger", false);
    delta.physicsLayerName = j.value("physicsLayerName", std::string("Default"));
    return delta;
}

// =============================================================================
// AddedCameraDelta Implementation
// =============================================================================

nlohmann::json AddedCameraDelta::ToJson() const {
    nlohmann::json j;
    j["fieldOfView"] = fieldOfView;
    j["nearClip"] = nearClip;
    j["farClip"] = farClip;
    j["isPerspective"] = isPerspective;
    j["active"] = active;
    j["priority"] = priority;
    return j;
}

AddedCameraDelta AddedCameraDelta::FromJson(const nlohmann::json& j) {
    AddedCameraDelta delta;
    delta.fieldOfView = j.value("fieldOfView", 60.0f);
    delta.nearClip = j.value("nearClip", 0.1f);
    delta.farClip = j.value("farClip", 1000.0f);
    delta.isPerspective = j.value("isPerspective", true);
    delta.active = j.value("active", false);
    delta.priority = j.value("priority", 0);
    return delta;
}

// =============================================================================
// AddedEmitterDelta Implementation
// =============================================================================

nlohmann::json AddedEmitterDelta::ToJson() const {
    nlohmann::json j;
    j["spritePath"] = spritePath;
    j["maxParticles"] = maxParticles;
    j["emissionRate"] = emissionRate;
    j["enabled"] = enabled;
    if (!fullData.empty()) {
        j["fullData"] = fullData;
    }
    return j;
}

AddedEmitterDelta AddedEmitterDelta::FromJson(const nlohmann::json& j) {
    AddedEmitterDelta delta;
    delta.spritePath = j.value("spritePath", std::string());
    delta.maxParticles = j.value("maxParticles", 1024u);
    delta.emissionRate = j.value("emissionRate", 100.0f);
    delta.enabled = j.value("enabled", true);
    if (j.contains("fullData")) {
        delta.fullData = j["fullData"];
    }
    return delta;
}

// =============================================================================
// NodeDelta Implementation
// =============================================================================

bool NodeDelta::HasModifications() const {
    return !name.empty() ||
           layer.has_value() ||
           tag.has_value() ||
           visible.has_value() ||
           active.has_value() ||
           (transform.has_value() && transform->HasModifications()) ||
           (mesh.has_value() && mesh->HasModifications()) ||
           renderOverrides.has_value() ||
           blendShapes.has_value() ||
           boneAttachment.has_value() ||
           !lookAtConstraints.empty() ||
           !ikConstraints.empty() ||
           !scripts.empty() ||
           addedLight.has_value() ||
           addedCollider.has_value() ||
           addedCamera.has_value() ||
           addedEmitter.has_value() ||
           tintController.has_value() ||
           !extra.empty();
}

nlohmann::json NodeDelta::ToJson() const {
    nlohmann::json j;
    
    // Identity
    j["nodeId"] = nodeId.ToJson();
    
    // Entity properties
    if (!name.empty()) j["name"] = name;
    if (layer.has_value()) j["layer"] = *layer;
    if (tag.has_value()) j["tag"] = *tag;
    if (visible.has_value()) j["visible"] = *visible;
    if (active.has_value()) j["active"] = *active;
    
    // Transform
    if (transform.has_value()) {
        j["transform"] = transform->ToJson();
    }
    
    // Mesh
    if (mesh.has_value()) {
        j["mesh"] = mesh->ToJson();
    }
    
    // Render overrides
    if (renderOverrides.has_value()) {
        j["renderOverrides"] = renderOverrides->ToJson();
    }
    
    // Blend shapes
    if (blendShapes.has_value()) {
        j["blendShapes"] = blendShapes->ToJson();
    }
    
    // Bone attachment
    if (boneAttachment.has_value()) {
        j["boneAttachment"] = boneAttachment->ToJson();
    }
    
    // Constraints
    if (!lookAtConstraints.empty()) {
        j["lookAtConstraints"] = nlohmann::json::array();
        for (const auto& lac : lookAtConstraints) {
            j["lookAtConstraints"].push_back(lac.ToJson());
        }
    }
    
    if (!ikConstraints.empty()) {
        j["ikConstraints"] = nlohmann::json::array();
        for (const auto& ik : ikConstraints) {
            j["ikConstraints"].push_back(ik.ToJson());
        }
    }
    
    // Scripts
    if (!scripts.empty()) {
        j["scripts"] = nlohmann::json::array();
        for (const auto& s : scripts) {
            j["scripts"].push_back(s.ToJson());
        }
    }
    
    // Added components
    if (addedLight.has_value()) j["addedLight"] = addedLight->ToJson();
    if (addedCollider.has_value()) j["addedCollider"] = addedCollider->ToJson();
    if (addedCamera.has_value()) j["addedCamera"] = addedCamera->ToJson();
    if (addedEmitter.has_value()) j["addedEmitter"] = addedEmitter->ToJson();
    
    // TintController
    if (tintController.has_value()) j["tintController"] = *tintController;
    
    // Extra data
    if (!extra.empty()) {
        j["extra"] = extra;
    }
    
    return j;
}

NodeDelta NodeDelta::FromJson(const nlohmann::json& j) {
    NodeDelta delta;
    
    // Identity
    if (j.contains("nodeId") && j["nodeId"].is_object()) {
        delta.nodeId = StableNodeId::FromJson(j["nodeId"]);
    }
    
    // Entity properties
    if (j.contains("name") && j["name"].is_string()) {
        delta.name = j["name"].get<std::string>();
    }
    if (j.contains("layer")) delta.layer = j["layer"].get<int>();
    if (j.contains("tag")) delta.tag = j["tag"].get<std::string>();
    if (j.contains("visible")) delta.visible = j["visible"].get<bool>();
    if (j.contains("active")) delta.active = j["active"].get<bool>();
    
    // Transform
    if (j.contains("transform") && j["transform"].is_object()) {
        delta.transform = TransformDelta::FromJson(j["transform"]);
    }
    
    // Mesh
    if (j.contains("mesh") && j["mesh"].is_object()) {
        delta.mesh = MeshDelta::FromJson(j["mesh"]);
    }
    
    // Render overrides
    if (j.contains("renderOverrides") && j["renderOverrides"].is_object()) {
        delta.renderOverrides = RenderOverridesDelta::FromJson(j["renderOverrides"]);
    }
    
    // Blend shapes
    if (j.contains("blendShapes") && j["blendShapes"].is_object()) {
        delta.blendShapes = BlendShapeDelta::FromJson(j["blendShapes"]);
    }
    
    // Bone attachment
    if (j.contains("boneAttachment") && j["boneAttachment"].is_object()) {
        delta.boneAttachment = BoneAttachmentDelta::FromJson(j["boneAttachment"]);
    }
    
    // Constraints
    if (j.contains("lookAtConstraints") && j["lookAtConstraints"].is_array()) {
        for (const auto& lac : j["lookAtConstraints"]) {
            delta.lookAtConstraints.push_back(LookAtConstraintDelta::FromJson(lac));
        }
    }
    
    if (j.contains("ikConstraints") && j["ikConstraints"].is_array()) {
        for (const auto& ik : j["ikConstraints"]) {
            delta.ikConstraints.push_back(IKConstraintDelta::FromJson(ik));
        }
    }
    
    // Scripts
    if (j.contains("scripts") && j["scripts"].is_array()) {
        for (const auto& s : j["scripts"]) {
            delta.scripts.push_back(ScriptDelta::FromJson(s));
        }
    }
    
    // Added components
    if (j.contains("addedLight") && j["addedLight"].is_object()) {
        delta.addedLight = AddedLightDelta::FromJson(j["addedLight"]);
    }
    if (j.contains("addedCollider") && j["addedCollider"].is_object()) {
        delta.addedCollider = AddedColliderDelta::FromJson(j["addedCollider"]);
    }
    if (j.contains("addedCamera") && j["addedCamera"].is_object()) {
        delta.addedCamera = AddedCameraDelta::FromJson(j["addedCamera"]);
    }
    if (j.contains("addedEmitter") && j["addedEmitter"].is_object()) {
        delta.addedEmitter = AddedEmitterDelta::FromJson(j["addedEmitter"]);
    }
    
    // TintController
    if (j.contains("tintController") && j["tintController"].is_object()) {
        delta.tintController = j["tintController"];
    }
    
    // Extra data
    if (j.contains("extra")) {
        delta.extra = j["extra"];
    }
    
    return delta;
}

// =============================================================================
// AddedChildDelta Implementation
// =============================================================================

nlohmann::json AddedChildDelta::ToJson() const {
    nlohmann::json j;
    j["childId"] = childId.ToJson();
    j["parentNodeId"] = parentNodeId.ToJson();
    j["entityData"] = entityData;
    j["isNestedModel"] = isNestedModel;
    
    if (nestedModelAsset.has_value()) {
        nlohmann::json assetJ;
        assetJ["guid"] = nestedModelAsset->guid.ToString();
        assetJ["fileID"] = nestedModelAsset->fileID;
        assetJ["type"] = static_cast<int>(nestedModelAsset->type);
        j["nestedModelAsset"] = assetJ;
    }
    
    return j;
}

AddedChildDelta AddedChildDelta::FromJson(const nlohmann::json& j) {
    AddedChildDelta delta;
    
    if (j.contains("childId") && j["childId"].is_object()) {
        delta.childId = StableNodeId::FromJson(j["childId"]);
    }
    if (j.contains("parentNodeId") && j["parentNodeId"].is_object()) {
        delta.parentNodeId = StableNodeId::FromJson(j["parentNodeId"]);
    }
    if (j.contains("entityData")) {
        delta.entityData = j["entityData"];
    }
    delta.isNestedModel = j.value("isNestedModel", false);
    
    if (j.contains("nestedModelAsset") && j["nestedModelAsset"].is_object()) {
        AssetReference ref;
        const auto& a = j["nestedModelAsset"];
        if (a.contains("guid") && a["guid"].is_string()) {
            ref.guid = ClaymoreGUID::FromString(a["guid"].get<std::string>());
        }
        ref.fileID = a.value("fileID", -1);
        ref.type = a.value("type", 0);
        delta.nestedModelAsset = ref;
    }
    
    return delta;
}

// =============================================================================
// ModelDelta Implementation
// =============================================================================

bool ModelDelta::IsEmpty() const {
    return nodeDeltas.empty() && 
           deletedNodePaths.empty() && 
           addedChildren.empty() &&
           (!rootDelta.has_value() || !rootDelta->HasModifications());
}

bool ModelDelta::IsPotentiallyStale(uint32_t currentModelVersion) const {
    return modelStructureVersion != 0 && modelStructureVersion != currentModelVersion;
}

const NodeDelta* ModelDelta::FindNodeDelta(const std::string& nodePath) const {
    for (const auto& nd : nodeDeltas) {
        if (nd.nodeId.path == nodePath) {
            return &nd;
        }
    }
    return nullptr;
}

NodeDelta* ModelDelta::FindNodeDelta(const std::string& nodePath) {
    for (auto& nd : nodeDeltas) {
        if (nd.nodeId.path == nodePath) {
            return &nd;
        }
    }
    return nullptr;
}

const NodeDelta* ModelDelta::FindNodeDeltaByStableId(const StableNodeId& id) const {
    // First try exact path match
    const NodeDelta* pathMatch = FindNodeDelta(id.path);
    if (pathMatch) return pathMatch;
    
    // Then try other matching strategies
    const NodeDelta* bestMatch = nullptr;
    StableNodeId::MatchConfidence bestConfidence = StableNodeId::MatchConfidence::None;
    
    for (const auto& nd : nodeDeltas) {
        auto confidence = nd.nodeId.GetMatchConfidence(id);
        if (confidence > bestConfidence) {
            bestConfidence = confidence;
            bestMatch = &nd;
        }
    }
    
    return bestMatch;
}

NodeDelta* ModelDelta::FindNodeDeltaByStableId(const StableNodeId& id) {
    // Use const version and cast away const
    const NodeDelta* result = static_cast<const ModelDelta*>(this)->FindNodeDeltaByStableId(id);
    return const_cast<NodeDelta*>(result);
}

NodeDelta& ModelDelta::GetOrCreateNodeDelta(const StableNodeId& nodeId) {
    // First try to find existing
    NodeDelta* existing = FindNodeDeltaByStableId(nodeId);
    if (existing) return *existing;
    
    // Create new
    NodeDelta newDelta;
    newDelta.nodeId = nodeId;
    nodeDeltas.push_back(std::move(newDelta));
    return nodeDeltas.back();
}

nlohmann::json ModelDelta::ToJson() const {
    nlohmann::json j;
    
    // Header
    j["version"] = DELTA_FORMAT_VERSION;
    j["modelAssetGuid"] = modelAssetGuid.ToString();
    j["modelAssetPath"] = modelAssetPath;
    j["modelStructureVersion"] = modelStructureVersion;
    j["createdTimestamp"] = createdTimestamp;
    
    // Root delta
    if (rootDelta.has_value()) {
        j["rootDelta"] = rootDelta->ToJson();
    }
    
    // Node deltas
    if (!nodeDeltas.empty()) {
        j["nodeDeltas"] = nlohmann::json::array();
        for (const auto& nd : nodeDeltas) {
            j["nodeDeltas"].push_back(nd.ToJson());
        }
    }
    
    // Deleted nodes
    if (!deletedNodePaths.empty()) {
        j["deletedNodePaths"] = deletedNodePaths;
    }
    
    // Added children
    if (!addedChildren.empty()) {
        j["addedChildren"] = nlohmann::json::array();
        for (const auto& ac : addedChildren) {
            j["addedChildren"].push_back(ac.ToJson());
        }
    }
    
    return j;
}

ModelDelta ModelDelta::FromJson(const nlohmann::json& j) {
    ModelDelta delta;
    
    // Header
    // uint32_t version = j.value("version", 1u); // For future version handling
    if (j.contains("modelAssetGuid") && j["modelAssetGuid"].is_string()) {
        delta.modelAssetGuid = ClaymoreGUID::FromString(j["modelAssetGuid"].get<std::string>());
    }
    delta.modelAssetPath = j.value("modelAssetPath", std::string());
    delta.modelStructureVersion = j.value("modelStructureVersion", 0u);
    delta.createdTimestamp = j.value("createdTimestamp", 0ULL);
    
    // Root delta
    if (j.contains("rootDelta") && j["rootDelta"].is_object()) {
        delta.rootDelta = NodeDelta::FromJson(j["rootDelta"]);
    }
    
    // Node deltas
    if (j.contains("nodeDeltas") && j["nodeDeltas"].is_array()) {
        for (const auto& nd : j["nodeDeltas"]) {
            delta.nodeDeltas.push_back(NodeDelta::FromJson(nd));
        }
    }
    
    // Deleted nodes
    if (j.contains("deletedNodePaths") && j["deletedNodePaths"].is_array()) {
        for (const auto& p : j["deletedNodePaths"]) {
            if (p.is_string()) {
                std::string path = p.get<std::string>();
                if (!path.empty()) {
                    delta.deletedNodePaths.push_back(std::move(path));
                }
            }
        }
    }
    
    // Added children
    if (j.contains("addedChildren") && j["addedChildren"].is_array()) {
        for (const auto& ac : j["addedChildren"]) {
            delta.addedChildren.push_back(AddedChildDelta::FromJson(ac));
        }
    }
    
    return delta;
}

// =============================================================================
// Binary Serialization
// =============================================================================

std::vector<uint8_t> ModelDelta::ToBinary() const {
    // For now, serialize to JSON then store as binary
    // This is simpler and allows for forward compatibility
    // A more optimized binary format can be implemented later if needed
    
    std::vector<uint8_t> result;
    
    // Magic + version header
    uint32_t magic = DELTA_BINARY_MAGIC;
    uint32_t version = DELTA_FORMAT_VERSION;
    
    result.resize(8);
    std::memcpy(result.data(), &magic, 4);
    std::memcpy(result.data() + 4, &version, 4);
    
    // JSON data
    std::string jsonStr = ToJson().dump();
    uint32_t jsonLen = static_cast<uint32_t>(jsonStr.size());
    
    size_t oldSize = result.size();
    result.resize(oldSize + 4 + jsonLen);
    std::memcpy(result.data() + oldSize, &jsonLen, 4);
    std::memcpy(result.data() + oldSize + 4, jsonStr.data(), jsonLen);
    
    return result;
}

ModelDelta ModelDelta::FromBinary(const uint8_t* data, size_t size) {
    if (size < 12) {
        return ModelDelta{};
    }
    
    // Check magic
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    if (magic != DELTA_BINARY_MAGIC) {
        return ModelDelta{};
    }
    
    // Check version
    uint32_t version;
    std::memcpy(&version, data + 4, 4);
    if (version > DELTA_FORMAT_VERSION) {
        // Future version - can't parse
        return ModelDelta{};
    }
    
    // Read JSON length
    uint32_t jsonLen;
    std::memcpy(&jsonLen, data + 8, 4);
    
    if (12 + jsonLen > size) {
        return ModelDelta{};
    }
    
    // Parse JSON
    std::string jsonStr(reinterpret_cast<const char*>(data + 12), jsonLen);
    try {
        return FromJson(nlohmann::json::parse(jsonStr));
    } catch (...) {
        return ModelDelta{};
    }
}

} // namespace cm::model

