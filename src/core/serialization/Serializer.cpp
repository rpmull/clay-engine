#include "Serializer.h"
#include "EntityBinaryLoader.h"
#include <fstream>
#include <iostream>
#include "editor/import/ModelBuild.h"
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include "core/ecs/AnimationComponents.h"
#include "managed/interop/ScriptSystem.h"
#include "core/ecs/EntityData.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/ModelNodeIdentity.h"
#include "core/model/ModelDelta.h"
#include "core/model/ModelDeltaExtractor.h"
#include "core/model/ModelDeltaApplicator.h"
#include "core/rendering/TextureLoader.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/MaterialAssetCache.h"
#include "core/ecs/UIComponents.h"
#include "core/animation/AnimationPlayerComponent.h"
#include "core/animation/AnimationPreloader.h"
#include "core/vfs/FileSystem.h"
#include "core/assets/IAssetResolver.h"
#include "editor/Project.h"
#if !defined(CLAYMORE_RUNTIME)
#include "editor/pipeline/BinaryAssetCache.h"
#endif
#include <unordered_set>
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/PrefabDelta.h"
#include "core/prefab/PrefabPrewarm.h"
#include "core/ecs/ScenePostProcessing.h"
#include "core/physics/PhysicsLayerManager.h"

namespace {
#if !defined(CLAYMORE_RUNTIME)
bool TryEnsureBinaryForPlayMode(const std::string& sourcePath) {
    if (sourcePath.empty()) return false;
    if (Assets::GetLoadMode() != AssetLoadMode::PlayMode) return false;
    if (FileSystem::Instance().IsPakMounted()) return false;
    static thread_local bool s_EnsuringBinary = false;
    if (s_EnsuringBinary) return false;
    s_EnsuringBinary = true;
    const bool ensured = BinaryAssetCache::Instance().EnsureBinary(sourcePath);
    s_EnsuringBinary = false;
    return ensured;
}
#endif
}

#include "core/ecs/Scene.h"
#include "core/rendering/VertexTypes.h"
#include "core/rendering/Mesh.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/SkinnedPBRMaterial.h"
#include "core/rendering/MaterialAsset.h"
#include "editor/nodegraph/ShaderGraphMaterial.h"
#include "core/rendering/Terrain.h"
#include "core/rendering/River.h"
#include "core/resourcelayer/ResourceLayerTypes.h"
#include "core/resourcelayer/ClimateTypes.h"
#include "core/resourcelayer/EligibilityFilter.h"
#include "core/ecs/InstancerComponent.h"
#include <bgfx/bgfx.h>
#include "core/rendering/GlobalShaderProperties.h"
#include <cmath>
#include <optional>

#include "core/ecs/ComponentRegistry.h"

// Jobs for parallel scene deserialization
#include "core/jobs/Jobs.h"
#include "core/jobs/ParallelFor.h"

namespace fs = std::filesystem;

namespace {
inline bool NearlyEqual(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
inline bool NearlyEqual(const glm::vec2& a, const glm::vec2& b, float eps = 1e-4f) {
    return NearlyEqual(a.x, b.x, eps) && NearlyEqual(a.y, b.y, eps);
}
inline bool NearlyEqual(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
    return NearlyEqual(a.x, b.x, eps) && NearlyEqual(a.y, b.y, eps) && NearlyEqual(a.z, b.z, eps);
}

inline bool IsMeshLikeAssetPath(const std::string& path) {
    if (path.empty()) return false;
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".meshbin" || ext == ".meta";
}

constexpr int32_t kAudioAssetType = 100;

inline std::string NormalizePortableAssetPath(const std::string& path) {
    if (path.empty()) return path;

    std::string normalized = path;
    try { normalized = IVirtualFS::NormalizePath(normalized); } catch(...) {}

    fs::path assetPath = fs::path(normalized).lexically_normal();
    const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
    if (assetPath.is_absolute() && !projectRoot.empty()) {
        std::error_code ec;
        fs::path rel = fs::relative(assetPath, projectRoot, ec);
        if (!ec && !rel.empty()) {
            std::string relStr = rel.generic_string();
            if (relStr.find("../") == std::string::npos) {
                return relStr;
            }
        }
    }

    return assetPath.generic_string();
}

static bool IsImportedModelRoot(Scene& scene, EntityID id, std::string& outModelPath, ClaymoreGUID& outGuid);

static void SerializeExtendedPBRState(nlohmann::json& node, const std::shared_ptr<PBRMaterial>& pbr, const std::string& prefix) {
    if (!pbr) return;
    auto key = [&](const char* local) { return prefix + local; };
    auto putPath = [&](const char* local, const std::string& value) {
        if (!value.empty()) node[key(local)] = value;
    };
    auto putFloat = [&](const char* local, float value, float def) {
        if (!NearlyEqual(value, def)) node[key(local)] = value;
    };
    putPath("aoPath", pbr->GetAOPath());
    putPath("emissionPath", pbr->GetEmissionPath());
    putPath("displacementPath", pbr->GetDisplacementPath());
    putPath("tintMaskPath", pbr->GetTintMaskPath());
    putFloat("metallicScalar", pbr->GetMetallic(), PBRMaterial::kDefaultMetallic);
    putFloat("roughnessScalar", pbr->GetRoughness(), PBRMaterial::kDefaultRoughness);
    putFloat("normalScale", pbr->GetNormalScale(), PBRMaterial::kDefaultNormalScale);
    putFloat("aoScalar", pbr->GetAmbientOcclusion(), PBRMaterial::kDefaultAO);
    putFloat("emissionStrength", pbr->GetEmissionStrength(), PBRMaterial::kDefaultEmissionStrength);
    putFloat("displacementScale", pbr->GetDisplacementScale(), PBRMaterial::kDefaultDisplacementScale);
    if (!NearlyEqual(pbr->GetEmissionColor(), glm::vec3(1.0f))) {
        auto col = pbr->GetEmissionColor();
        node[key("emissionColor")] = { col.x, col.y, col.z };
    }
    if (!NearlyEqual(pbr->GetUVScale(), glm::vec2(1.0f))) {
        auto scale = pbr->GetUVScale();
        node[key("uvScale")] = { scale.x, scale.y };
    }
    if (!NearlyEqual(pbr->GetUVOffset(), glm::vec2(0.0f))) {
        auto offset = pbr->GetUVOffset();
        node[key("uvOffset")] = { offset.x, offset.y };
    }
    if (pbr->GetReceiveShadowsOverride()) {
        node[key("receiveShadowsOverride")] = true;
        node[key("receiveShadows")] = pbr->GetReceiveShadows();
    }
}

static void DeserializeExtendedPBRState(const nlohmann::json& node, const std::string& prefix, const std::shared_ptr<PBRMaterial>& pbr) {
    if (!pbr) return;
    auto key = [&](const char* local) { return prefix + local; };
    auto getFloat = [&](const char* local) -> std::optional<float> {
        auto full = key(local);
        if (node.contains(full)) return node.at(full).get<float>();
        return std::nullopt;
    };

    if (node.contains(key("aoPath"))) pbr->SetAmbientOcclusionTextureFromPath(node.at(key("aoPath")).get<std::string>());
    if (node.contains(key("emissionPath"))) pbr->SetEmissionTextureFromPath(node.at(key("emissionPath")).get<std::string>());
    if (node.contains(key("displacementPath"))) pbr->SetDisplacementTextureFromPath(node.at(key("displacementPath")).get<std::string>());
    if (node.contains(key("tintMaskPath"))) pbr->SetTintMaskTextureFromPath(node.at(key("tintMaskPath")).get<std::string>());

    bool metallicSet = false;
    if (auto metallic = getFloat("metallicScalar")) { pbr->SetMetallic(*metallic); metallicSet = true; }
    if (!metallicSet) {
        if (bgfx::isValid(pbr->m_MetallicRoughnessTex)) pbr->SetMetallic(1.0f);
        else pbr->SetMetallic(PBRMaterial::kDefaultMetallic);
    }

    bool roughSet = false;
    if (auto rough = getFloat("roughnessScalar")) { pbr->SetRoughness(*rough); roughSet = true; }
    if (!roughSet) {
        if (bgfx::isValid(pbr->m_MetallicRoughnessTex)) pbr->SetRoughness(1.0f);
        else pbr->SetRoughness(PBRMaterial::kDefaultRoughness);
    }

    if (auto normal = getFloat("normalScale")) pbr->SetNormalScale(*normal);
    else pbr->SetNormalScale(PBRMaterial::kDefaultNormalScale);

    if (auto ao = getFloat("aoScalar")) pbr->SetAmbientOcclusion(*ao);
    else pbr->SetAmbientOcclusion(PBRMaterial::kDefaultAO);

    if (auto emStrength = getFloat("emissionStrength")) pbr->SetEmissionStrength(*emStrength);
    else pbr->SetEmissionStrength(PBRMaterial::kDefaultEmissionStrength);

    if (auto dispScale = getFloat("displacementScale")) pbr->SetDisplacementScale(*dispScale);
    else pbr->SetDisplacementScale(PBRMaterial::kDefaultDisplacementScale);

    auto colorKey = key("emissionColor");
    if (node.contains(colorKey) && node.at(colorKey).is_array() && node.at(colorKey).size() == 3) {
        glm::vec3 col(node.at(colorKey)[0], node.at(colorKey)[1], node.at(colorKey)[2]);
        pbr->SetEmissionColor(col);
    } else {
        pbr->SetEmissionColor(glm::vec3(1.0f));
    }

    auto scaleKey = key("uvScale");
    if (node.contains(scaleKey) && node.at(scaleKey).is_array() && node.at(scaleKey).size() == 2) {
        glm::vec2 scale(node.at(scaleKey)[0], node.at(scaleKey)[1]);
        pbr->SetUVScale(scale);
    } else {
        pbr->SetUVScale(glm::vec2(1.0f));
    }

    auto offsetKey = key("uvOffset");
    if (node.contains(offsetKey) && node.at(offsetKey).is_array() && node.at(offsetKey).size() == 2) {
        glm::vec2 offset(node.at(offsetKey)[0], node.at(offsetKey)[1]);
        pbr->SetUVOffset(offset);
    } else {
        pbr->SetUVOffset(glm::vec2(0.0f));
    }

    bool receiveOverride = false;
    auto overrideKey = key("receiveShadowsOverride");
    if (node.contains(overrideKey)) {
        receiveOverride = node.at(overrideKey).get<bool>();
    }
    pbr->SetReceiveShadowsOverride(receiveOverride);
    if (receiveOverride) {
        auto receiveKey = key("receiveShadows");
        bool receive = false;
        if (node.contains(receiveKey)) {
            receive = node.at(receiveKey).get<bool>();
        }
        pbr->SetReceiveShadows(receive);
    }
}

static bool MeshDeserializeWantsSkinnedMaterial(const MeshComponent& mesh) {
    return mesh.mesh && mesh.mesh->HasSkinning();
}

static std::shared_ptr<PBRMaterial> EnsureEditablePBRMaterialForSlot(MeshComponent& mesh, size_t slot) {
    if (mesh.materials.size() <= slot) {
        mesh.materials.resize(slot + 1);
    }
    if (mesh.MaterialAssetPaths.size() < mesh.materials.size()) {
        mesh.MaterialAssetPaths.resize(mesh.materials.size());
    }
    if (mesh.OwnedMaterialSlots.size() < mesh.materials.size()) {
        mesh.OwnedMaterialSlots.resize(mesh.materials.size(), false);
    }

    if (!mesh.materials[slot]) {
        MaterialSource source;
        source.Skinned = MeshDeserializeWantsSkinnedMaterial(mesh);
        mesh.materials[slot] = AcquireMaterialFromSource(source, Scene::Get());
        mesh.OwnedMaterialSlots[slot] = true;
    } else if (!mesh.OwnedMaterialSlots[slot]) {
        if (auto clone = mesh.materials[slot]->Clone()) {
            mesh.materials[slot] = clone;
            mesh.OwnedMaterialSlots[slot] = true;
        }
    }

    if (slot == 0) {
        mesh.material = mesh.materials[slot];
    }

    return std::dynamic_pointer_cast<PBRMaterial>(mesh.materials[slot]);
}

// Compute sibling index for disambiguation when multiple siblings share the same name
static int ComputeSiblingIndex(Scene& scene, EntityID id) {
    auto* d = scene.GetEntityData(id);
    if (!d) return 0;
    
    if (d->Parent == INVALID_ENTITY_ID) {
        // Root entity - count roots with same name before this one
        int idx = 0;
        for (const auto& e : scene.GetEntities()) {
            if (e.GetID() == id) break;
            auto* rd = scene.GetEntityData(e.GetID());
            if (rd && rd->Parent == INVALID_ENTITY_ID && rd->Name == d->Name) {
                ++idx;
            }
        }
        return idx;
    }
    
    // Child entity - count siblings with same name before this one
    auto* parent = scene.GetEntityData(d->Parent);
    if (!parent) return 0;
    
    int idx = 0;
    for (EntityID sib : parent->Children) {
        if (sib == id) break;
        auto* sd = scene.GetEntityData(sib);
        if (sd && sd->Name == d->Name) {
            ++idx;
        }
    }
    return idx;
}

static std::string ComputeScenePath(Scene& scene, EntityID id) {
    if (id == INVALID_ENTITY_ID) return {};
    std::vector<std::pair<std::string, int>> parts; // (name, siblingIndex)
    EntityID cur = id;
    size_t guard = 0;
    while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
        auto* d = scene.GetEntityData(cur);
        if (!d) break;
        int sibIdx = ComputeSiblingIndex(scene, cur);
        parts.push_back({d->Name, sibIdx});
        if (d->Parent == INVALID_ENTITY_ID || d->Parent == cur) break;
        cur = d->Parent;
    }
    if (parts.empty()) return {};
    std::reverse(parts.begin(), parts.end());
    std::string path;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) path += "/";
        path += parts[i].first;
        // Append sibling index if > 0 to disambiguate (e.g., "Node[1]")
        if (parts[i].second > 0) {
            path += "[" + std::to_string(parts[i].second) + "]";
        }
    }
    return path;
}

struct PrefabReferenceSerializationContext {
    Scene* ScenePtr = nullptr;
    EntityID Root = INVALID_ENTITY_ID;
    const std::unordered_set<EntityID>* Subtree = nullptr;
};

static thread_local const PrefabReferenceSerializationContext* g_PrefabRefSerializationContext = nullptr;

class ScopedPrefabReferenceSerializationContext {
public:
    explicit ScopedPrefabReferenceSerializationContext(const PrefabReferenceSerializationContext& context)
        : m_Previous(g_PrefabRefSerializationContext) {
        g_PrefabRefSerializationContext = &context;
    }

    ~ScopedPrefabReferenceSerializationContext() {
        g_PrefabRefSerializationContext = m_Previous;
    }

private:
    const PrefabReferenceSerializationContext* m_Previous = nullptr;
};

static bool ComputePrefabNodePath(Scene& scene, EntityID root, EntityID id, std::string& outPath) {
    outPath.clear();
    if (root == INVALID_ENTITY_ID || id == INVALID_ENTITY_ID) {
        return false;
    }

    std::vector<std::pair<std::string, int>> parts;
    EntityID cur = id;
    size_t guard = 0;
    while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
        auto* d = scene.GetEntityData(cur);
        if (!d) {
            return false;
        }
        if (cur == root) {
            std::reverse(parts.begin(), parts.end());
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) outPath += "/";
                outPath += parts[i].first;
                if (parts[i].second > 0) {
                    outPath += "[" + std::to_string(parts[i].second) + "]";
                }
            }
            return true;
        }

        parts.push_back({d->Name, ComputeSiblingIndex(scene, cur)});
        if (d->Parent == cur) {
            return false;
        }
        cur = d->Parent;
    }

    return false;
}

static bool ComputeModelNodePathInfo(Scene& scene, EntityID id, EntityID& outModelRoot, ClaymoreGUID& outGuid, std::string& outRelPath) {
    outModelRoot = INVALID_ENTITY_ID;
    outGuid = ClaymoreGUID{};
    outRelPath.clear();
    if (id == INVALID_ENTITY_ID) return false;
    std::vector<EntityID> chain;
    EntityID cur = id;
    size_t guard = 0;
    while (cur != INVALID_ENTITY_ID && guard++ < 100000) {
        chain.push_back(cur);
        auto* d = scene.GetEntityData(cur);
        if (!d || d->Parent == INVALID_ENTITY_ID || d->Parent == cur) break;
        cur = d->Parent;
    }
    if (chain.empty()) return false;
    std::reverse(chain.begin(), chain.end());
    EntityID modelRoot = INVALID_ENTITY_ID;
    ClaymoreGUID modelGuid{};
    for (EntityID nid : chain) {
        if (modelRoot == INVALID_ENTITY_ID) {
            std::string tmp; ClaymoreGUID g{};
            if (IsImportedModelRoot(scene, nid, tmp, g)) {
                modelRoot = nid;
                modelGuid = g;
            }
        }
    }
    if (modelRoot == INVALID_ENTITY_ID) return false;
    std::vector<std::string> parts;
    bool started = false;
    for (EntityID nid : chain) {
        if (!started) {
            if (nid == modelRoot) {
                started = true;
                continue;
            } else {
                continue;
            }
        }
        auto* d = scene.GetEntityData(nid);
        if (!d) continue;
        parts.push_back(d->Name);
    }
    std::string relPath;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) relPath += "/";
        relPath += parts[i];
    }
    outModelRoot = modelRoot;
    outGuid = modelGuid;
    outRelPath = relPath;
    return true;
}

static std::string NormalizeModelNodeName(const std::string& name) {
    size_t underscore = name.find_last_of('_');
    if (underscore == std::string::npos) return name;
    if (underscore + 1 >= name.size()) return name;
    for (size_t i = underscore + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return name;
        }
    }
    return name.substr(0, underscore);
}

static EntityID FindChildByNameOrNormalized(Scene& scene, EntityID parent, const std::string& name) {
    auto* pd = scene.GetEntityData(parent);
    if (!pd) return INVALID_ENTITY_ID;
    for (EntityID c : pd->Children) {
        auto* cd = scene.GetEntityData(c);
        if (cd && cd->Name == name) return c;
    }
    const std::string normalized = NormalizeModelNodeName(name);
    for (EntityID c : pd->Children) {
        auto* cd = scene.GetEntityData(c);
        if (cd && NormalizeModelNodeName(cd->Name) == normalized) {
            return c;
        }
    }
    return INVALID_ENTITY_ID;
}

static nlohmann::json BuildEntityReferenceJson(Scene& scene, EntityID id) {
    nlohmann::json ref;
    if (id == INVALID_ENTITY_ID) return ref;
    auto* d = scene.GetEntityData(id);
    if (!d) return ref;

    bool isPrefabLocalRef = false;
    if (g_PrefabRefSerializationContext &&
        g_PrefabRefSerializationContext->ScenePtr == &scene &&
        g_PrefabRefSerializationContext->Root != INVALID_ENTITY_ID &&
        g_PrefabRefSerializationContext->Subtree &&
        g_PrefabRefSerializationContext->Subtree->find(id) != g_PrefabRefSerializationContext->Subtree->end()) {
        std::string prefabNodePath;
        if (ComputePrefabNodePath(scene, g_PrefabRefSerializationContext->Root, id, prefabNodePath)) {
            ref["prefabNodePath"] = prefabNodePath;
            if (auto* rootData = scene.GetEntityData(g_PrefabRefSerializationContext->Root)) {
                if (!(rootData->EntityGuid.high == 0 && rootData->EntityGuid.low == 0)) {
                    ref["prefabRootGuid"] = rootData->EntityGuid.ToString();
                }
            }
            isPrefabLocalRef = true;
        }
    }
    
    // Always include entity GUID when available (primary resolution key)
    if (!(d->EntityGuid.high == 0 && d->EntityGuid.low == 0)) {
        ref["guid"] = d->EntityGuid.ToString();
    }
    
    // Scene path (now includes sibling indices for disambiguation).
    // Prefab-local references get prefabNodePath instead; storing the original scene path
    // would make an authored prefab depend on the scene it happened to be saved from.
    if (!isPrefabLocalRef) {
        std::string scenePath = ComputeScenePath(scene, id);
        if (!scenePath.empty()) ref["scenePath"] = scenePath;
    }
    
    // Model hierarchy info
    EntityID modelRoot = INVALID_ENTITY_ID;
    ClaymoreGUID modelGuid{};
    std::string relPath;
    if (ComputeModelNodePathInfo(scene, id, modelRoot, modelGuid, relPath) &&
        !(modelGuid.high == 0 && modelGuid.low == 0)) {
        ref["modelGuid"] = modelGuid.ToString();
        
        // Include model node path (even if empty for model roots)
        ref["modelNodePath"] = relPath;
        
        // Add model root's entity GUID as instance hint
        // This allows resolving to the correct instance when multiple
        // instances of the same model exist in the scene
        if (modelRoot != INVALID_ENTITY_ID) {
            auto* rootData = scene.GetEntityData(modelRoot);
            if (rootData && !(rootData->EntityGuid.high == 0 && rootData->EntityGuid.low == 0)) {
                ref["modelRootGuid"] = rootData->EntityGuid.ToString();
            }
        }
    }
    return ref;
}

static void SerializePropertyBlock(const MaterialPropertyBlock& block,
                                   const std::unordered_map<std::string, std::string>& texturePaths,
                                   nlohmann::json& out,
                                   const char* vecKey,
                                   const char* texKey) {
    if (!block.Vec4Uniforms.empty()) {
        nlohmann::json jvec = nlohmann::json::object();
        for (const auto& kv : block.Vec4Uniforms) {
            const glm::vec4& v = kv.second;
            jvec[kv.first] = { v.x, v.y, v.z, v.w };
        }
        out[vecKey] = std::move(jvec);
    }
    if (!texturePaths.empty()) {
        nlohmann::json jtex = nlohmann::json::object();
        for (const auto& kv : texturePaths) {
            jtex[kv.first] = kv.second;
        }
        out[texKey] = std::move(jtex);
    }
}

static void DeserializePropertyBlock(const nlohmann::json& data,
                                     const char* vecKey,
                                     const char* texKey,
                                     MaterialPropertyBlock& block,
                                     std::unordered_map<std::string, std::string>& texturePaths,
                                     bool clearExisting = true) {
    if (clearExisting) {
        block.Clear();  // Clears both string and ID-based maps
        texturePaths.clear();
    }
    if (data.contains(vecKey) && data[vecKey].is_object()) {
        const auto& jvec = data[vecKey];
        for (auto it = jvec.begin(); it != jvec.end(); ++it) {
            if (!it.value().is_array() || it.value().size() != 4) continue;
            glm::vec4 v(
                it.value()[0].get<float>(),
                it.value()[1].get<float>(),
                it.value()[2].get<float>(),
                it.value()[3].get<float>());
            // Use SetVector to populate both string and ID-based maps
            block.SetVector(it.key(), v);
        }
    }
    if (data.contains(texKey) && data[texKey].is_object()) {
        const auto& jtex = data[texKey];
        for (auto it = jtex.begin(); it != jtex.end(); ++it) {
            std::string path = it.value().get<std::string>();
            if (path.empty()) continue;
            try { path = IVirtualFS::NormalizePath(path); } catch(...) {}
            TextureSpecifier spec;
            spec.Path = path;
            bgfx::TextureHandle handle = AcquireTextureHandle(spec, TextureColorSpace::Linear);
            if (!bgfx::isValid(handle)) continue;
            // Use SetTexture to populate both string and ID-based maps
            block.SetTexture(it.key(), handle);
            texturePaths[it.key()] = path;
        }
    }
}

static bool HasSerializedPropertyBlockOverrides(const nlohmann::json& data,
                                                const char* vecKey,
                                                const char* texKey) {
    if (!data.is_object()) return false;
    if (data.contains(vecKey) && data[vecKey].is_object() && !data[vecKey].empty()) {
        return true;
    }
    if (data.contains(texKey) && data[texKey].is_object() && !data[texKey].empty()) {
        return true;
    }
    return false;
}

static bool HasFreshInstantiatedMaterialSlots(const MeshComponent& mesh) {
    if (mesh.materials.empty()) return false;
    for (const auto& material : mesh.materials) {
        if (material) {
            return true;
        }
    }
    return false;
}

static const std::unordered_map<std::string, std::string> kEmptyTexturePaths;

static bool IsImportedModelRoot(Scene& scene, EntityID id, std::string& outModelPath, ClaymoreGUID& outGuid) {
    auto* ed = scene.GetEntityData(id);
    if (!ed) return false;

    // Prefab roots may wrap imported-model hierarchies internally, but they still need
    // to round-trip as prefab instances when saving scenes. Give prefab identity precedence.
    bool hasPrefabInstance = ed->PrefabInstance &&
        !(ed->PrefabInstance->PrefabAssetGuid.high == 0 && ed->PrefabInstance->PrefabAssetGuid.low == 0);
    bool hasLegacyPrefab = !ed->PrefabSource.empty();
    if (hasPrefabInstance || hasLegacyPrefab) {
        return false;
    }
    
    // Check if entity has ModelAssetGuid set (explicit model root marker)
    // This takes precedence over mesh checks and allows roots with meshes
    if (ed->ModelAssetGuid.high != 0 || ed->ModelAssetGuid.low != 0) {
        outGuid = ed->ModelAssetGuid;
        outModelPath = AssetLibrary::Instance().GetPathForGUID(outGuid);
        return true;
    }
    
    // Legacy behavior: reject if root has mesh (but no ModelAssetGuid)
    // This maintains backward compatibility for scenes without explicit model markers
    if (ed->Mesh) return false;
    
    std::unordered_set<EntityID> visited;
    std::function<bool(EntityID)> dfs = [&](EntityID e)->bool{
        // Cycle detection: prevent infinite recursion
        if (visited.count(e)) return false;
        visited.insert(e);
        
        auto* cd = scene.GetEntityData(e);
        if (!cd) return false;
        if (cd->Mesh && cd->Mesh->meshReference.IsValid()) {
            ClaymoreGUID g = cd->Mesh->meshReference.guid;
            std::string p = AssetLibrary::Instance().GetPathForGUID(g);
            if (!p.empty()) {
                outModelPath = p; outGuid = g; return true;
            }
        }
        for (EntityID c : cd->Children) if (dfs(c)) return true;
        return false;
    };
    for (EntityID c : ed->Children) if (dfs(c)) return true;
    return false;
}
} // namespace

// Helper to parse segment name and sibling index from path component like "Node" or "Node[2]"
static void ParsePathSegment(const std::string& seg, std::string& outName, int& outSiblingIdx) {
    outSiblingIdx = 0;
    size_t bracket = seg.find('[');
    if (bracket != std::string::npos && seg.back() == ']') {
        outName = seg.substr(0, bracket);
        std::string idxStr = seg.substr(bracket + 1, seg.length() - bracket - 2);
        try { outSiblingIdx = std::stoi(idxStr); } catch (...) { outSiblingIdx = 0; }
    } else {
        outName = seg;
    }
}

// Find child by name, optionally using sibling index for disambiguation
static EntityID FindChildByNameAndIndex(Scene& scene, EntityID parent, const std::string& name, int siblingIdx) {
    auto* pd = scene.GetEntityData(parent);
    if (!pd) return INVALID_ENTITY_ID;
    
    int matchCount = 0;
    for (EntityID c : pd->Children) {
        auto* cd = scene.GetEntityData(c);
        if (cd && cd->Name == name) {
            if (matchCount == siblingIdx) return c;
            ++matchCount;
        }
    }
    // Fallback: if sibling index didn't match, try first match
    for (EntityID c : pd->Children) {
        auto* cd = scene.GetEntityData(c);
        if (cd && cd->Name == name) return c;
    }
    return INVALID_ENTITY_ID;
}

static EntityID ResolvePrefabNodePathInScene(Scene& scene, const nlohmann::json& refJson) {
    if (!refJson.is_object() || !refJson.contains("prefabNodePath")) {
        return INVALID_ENTITY_ID;
    }

    EntityID root = INVALID_ENTITY_ID;
    if (refJson.contains("prefabRootGuid")) {
        try {
            ClaymoreGUID rootGuid = ClaymoreGUID::FromString(refJson["prefabRootGuid"].get<std::string>());
            if (rootGuid.high != 0 || rootGuid.low != 0) {
                for (const auto& e : scene.GetEntities()) {
                    auto* d = scene.GetEntityData(e.GetID());
                    if (d && d->EntityGuid == rootGuid) {
                        root = e.GetID();
                        break;
                    }
                }
            }
        } catch (...) {}
    }

    if (root == INVALID_ENTITY_ID) {
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID());
            if (d && d->Parent == INVALID_ENTITY_ID) {
                if (root != INVALID_ENTITY_ID) {
                    return INVALID_ENTITY_ID;
                }
                root = e.GetID();
            }
        }
    }

    if (root == INVALID_ENTITY_ID) {
        return INVALID_ENTITY_ID;
    }

    std::string path;
    try {
        path = refJson["prefabNodePath"].get<std::string>();
    } catch (...) {
        return INVALID_ENTITY_ID;
    }
    if (path.empty()) {
        return root;
    }

    EntityID cur = root;
    std::stringstream ss(path);
    std::string token;
    while (std::getline(ss, token, '/')) {
        if (token.empty()) continue;
        std::string name;
        int idx = 0;
        ParsePathSegment(token, name, idx);
        cur = FindChildByNameAndIndex(scene, cur, name, idx);
        if (cur == INVALID_ENTITY_ID) {
            return INVALID_ENTITY_ID;
        }
    }

    return cur;
}

static EntityID ResolveEntityReferenceJson(const nlohmann::json& jv, Scene& scene) {
    EntityID resolved = INVALID_ENTITY_ID;
    
    // Back-compat plain GUID string
    if (jv.is_string()) {
        try {
            std::string token = jv.get<std::string>();
            ClaymoreGUID g = ClaymoreGUID::FromString(token);
            if (!(g.high == 0 && g.low == 0)) {
                for (const auto& e : scene.GetEntities()) {
                    if (auto* d = scene.GetEntityData(e.GetID())) {
                        if (d->EntityGuid.high == g.high && d->EntityGuid.low == g.low) {
                            resolved = e.GetID();
                            break;
                        }
                    }
                }
            }
        } catch(...) {}
        return resolved;
    }
    if (!jv.is_object()) return resolved;
    
    // 1) Entity GUID (most reliable)
    try {
        if (jv.contains("guid")) {
            std::string gstr = jv["guid"].get<std::string>();
            ClaymoreGUID g = ClaymoreGUID::FromString(gstr);
            if (!(g.high == 0 && g.low == 0)) {
                for (const auto& e : scene.GetEntities()) {
                    if (auto* d = scene.GetEntityData(e.GetID())) {
                        if (d->EntityGuid.high == g.high && d->EntityGuid.low == g.low) {
                            resolved = e.GetID();
                            break;
                        }
                    }
                }
            }
        }
    } catch(...) {}
    
    // 2) Prefab-local node path. When present, prefer it before model fallbacks:
    // it identifies the node inside the saved prefab instance rather than the
    // first matching model asset elsewhere in the scene.
    if (resolved == INVALID_ENTITY_ID && jv.contains("prefabNodePath")) {
        resolved = ResolvePrefabNodePathInScene(scene, jv);
    }

    // 3) Model GUID + node path (with instance disambiguation via modelRootGuid)
    if (resolved == INVALID_ENTITY_ID) {
        try {
            if (jv.contains("modelGuid") && jv.contains("modelNodePath")) {
                std::string mgstr = jv["modelGuid"].get<std::string>();
                std::string nodePath = jv["modelNodePath"].get<std::string>();
                ClaymoreGUID mg = ClaymoreGUID::FromString(mgstr);
                
                if (!(mg.high == 0 && mg.low == 0)) {
                    EntityID modelRoot = INVALID_ENTITY_ID;
                    
                    // Try to find specific model instance via modelRootGuid hint
                    if (jv.contains("modelRootGuid")) {
                        std::string rootGuidStr = jv["modelRootGuid"].get<std::string>();
                        ClaymoreGUID rootGuid = ClaymoreGUID::FromString(rootGuidStr);
                        if (!(rootGuid.high == 0 && rootGuid.low == 0)) {
                            for (const auto& e : scene.GetEntities()) {
                                auto* d = scene.GetEntityData(e.GetID());
                                if (d && d->EntityGuid.high == rootGuid.high && d->EntityGuid.low == rootGuid.low) {
                                    // Verify this is actually a model root with matching asset GUID
                                    std::string p; ClaymoreGUID g{};
                                    if (IsImportedModelRoot(scene, e.GetID(), p, g) &&
                                        g.high == mg.high && g.low == mg.low) {
                                        modelRoot = e.GetID();
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    
                    // Fallback: find first model root with matching asset GUID
                    if (modelRoot == INVALID_ENTITY_ID) {
                        for (const auto& e : scene.GetEntities()) {
                            std::string p; ClaymoreGUID g{};
                            if (IsImportedModelRoot(scene, e.GetID(), p, g)) {
                                if (g.high == mg.high && g.low == mg.low) {
                                    modelRoot = e.GetID();
                                    break;
                                }
                            }
                        }
                    }
                    
                    if (modelRoot != INVALID_ENTITY_ID) {
                        // Handle model root reference (empty node path)
                        if (nodePath.empty()) {
                            resolved = modelRoot;
                        } else {
                            // Walk node path (with normalized fallback for duplicate instance suffixes)
                            EntityID cur = modelRoot;
                            std::stringstream ss(nodePath);
                            std::string seg;
                            while (std::getline(ss, seg, '/')) {
                                if (seg.empty()) { cur = INVALID_ENTITY_ID; break; }
                                if (cur == INVALID_ENTITY_ID) break;
                                EntityID next = FindChildByNameOrNormalized(scene, cur, seg);
                                if (next == INVALID_ENTITY_ID) { cur = INVALID_ENTITY_ID; break; }
                                cur = next;
                            }
                            if (cur != INVALID_ENTITY_ID) resolved = cur;
                        }
                    }
                }
            }
        } catch(...) {}
    }

    // 4) Scene path (now supports sibling indices like "Root/Child[1]/Node")
    if (resolved == INVALID_ENTITY_ID) {
        try {
            if (jv.contains("scenePath")) {
                std::string sp = jv["scenePath"].get<std::string>();
                if (!sp.empty()) {
                    // Parse path into segments with sibling indices
                    std::vector<std::pair<std::string, int>> segments;
                    std::stringstream ss(sp);
                    std::string seg;
                    while (std::getline(ss, seg, '/')) {
                        if (seg.empty()) continue;
                        std::string name; int idx;
                        ParsePathSegment(seg, name, idx);
                        segments.push_back({name, idx});
                    }
                    
                    if (!segments.empty()) {
                        // Find matching root entity by name and sibling index
                        const std::string& rootName = segments[0].first;
                        int rootSibIdx = segments[0].second;
                        
                        std::vector<EntityID> roots;
                        for (const auto& e : scene.GetEntities()) {
                            auto* d = scene.GetEntityData(e.GetID());
                            if (d && d->Parent == INVALID_ENTITY_ID && d->Name == rootName) {
                                roots.push_back(e.GetID());
                            }
                        }
                        
                        EntityID startRoot = INVALID_ENTITY_ID;
                        if (rootSibIdx < (int)roots.size()) {
                            startRoot = roots[rootSibIdx];
                        } else if (!roots.empty()) {
                            startRoot = roots[0]; // Fallback to first
                        }
                        
                        if (startRoot != INVALID_ENTITY_ID) {
                            EntityID cur = startRoot;
                            bool ok = true;
                            
                            // Walk remaining path segments
                            for (size_t i = 1; i < segments.size() && ok; ++i) {
                                const std::string& childName = segments[i].first;
                                int childSibIdx = segments[i].second;
                                
                                EntityID next = FindChildByNameAndIndex(scene, cur, childName, childSibIdx);
                                if (next == INVALID_ENTITY_ID) {
                                    ok = false;
                                } else {
                                    cur = next;
                                }
                            }
                            
                            if (ok) resolved = cur;
                        }
                    }
                }
            }
        } catch(...) {}
    }
    return resolved;
}

static bool LooksLikeEntityReferenceJson(const nlohmann::json& jv) {
    if (jv.is_object()) {
        return jv.contains("guid") || jv.contains("scenePath") ||
            jv.contains("modelGuid") || jv.contains("prefabNodePath");
    }
    if (jv.is_string()) {
        try {
            ClaymoreGUID g = ClaymoreGUID::FromString(jv.get<std::string>());
            return !(g.high == 0 && g.low == 0);
        } catch(...) {}
    }
    return false;
}

static bool TryResolveSkinningSkeletonRootRef(const nlohmann::json& skinningJson,
                                              SkinningComponent& skinning,
                                              Scene& scene) {
    if (!skinningJson.is_object() || !skinningJson.contains("skeletonRootRef")) {
        return false;
    }

    EntityID resolved = ResolveEntityReferenceJson(skinningJson["skeletonRootRef"], scene);
    EntityData* skeletonData = scene.GetEntityData(resolved);
    if (!skeletonData || !skeletonData->Skeleton) {
        return false;
    }

    skinning.SkeletonRoot = resolved;
    skinning.ResolvedSkeletonRoot = INVALID_ENTITY_ID;
    skinning.InvalidateRemap();
    return true;
}

// Helper functions
static inline float cm_roundf(float v, int decimals = 6) {
    // Round to a fixed number of decimals to improve deterministic JSON output
    // and avoid tiny float noise causing spurious diffs.
    const float scale = powf(10.0f, (float)decimals);
    return floorf(v * scale + 0.5f) / scale;
}

json Serializer::SerializeVec3(const glm::vec3& vec) {
    // Canonicalize with fixed precision
    return json{{"x", cm_roundf(vec.x)}, {"y", cm_roundf(vec.y)}, {"z", cm_roundf(vec.z)}};
}
// ---------------- Navigation ----------------
json Serializer::SerializeNavMesh(const nav::NavMeshComponent& n) {
    json j;
    j["enabled"] = n.Enabled;
    j["hash"] = n.BakeHash;
    j["boundsMin"] = SerializeVec3(n.AABB.min);
    j["boundsMax"] = SerializeVec3(n.AABB.max);
    j["bake"] = {
        {"cellSize", n.Bake.cellSize},
        {"cellHeight", n.Bake.cellHeight},
        {"agentRadius", n.Bake.agentRadius},
        {"agentHeight", n.Bake.agentHeight},
        {"agentMaxClimb", n.Bake.agentMaxClimb},
        {"agentMaxSlopeDeg", n.Bake.agentMaxSlopeDeg}
    };
    // Terrain + chunk settings
    j["terrainSampleStep"] = n.TerrainSampleStep;
    j["geometryIncludeRegexEnabled"] = n.GeometryIncludeRegexEnabled;
    if (!n.GeometryIncludeRegexPattern.empty()) {
        j["geometryIncludeRegexPattern"] = n.GeometryIncludeRegexPattern;
    }
    j["bakeVisibleChunksOnly"] = n.BakeVisibleChunksOnly;
    j["bakeVisibleChunkPadding"] = n.BakeVisibleChunkPadding;
    j["bakeMissingChunksOnly"] = n.BakeMissingChunksOnly;
    j["chunkedNavEnabled"] = n.ChunkedNavEnabled;
    j["chunkingMode"] = static_cast<int>(n.ChunkingMode);
    j["chunkWorldSize"] = n.ChunkWorldSize;
    j["chunkBakePadding"] = n.ChunkBakePadding;
    j["chunkStreamRadius"] = n.ChunkStreamRadius;
    if (!n.NavPackPath.empty()) {
        std::string packPathToSave = n.NavPackPath;
        fs::path pp = fs::path(packPathToSave).lexically_normal();
        const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
        if (pp.is_absolute() && !projectRoot.empty()) {
            std::error_code ec;
            fs::path rel = fs::relative(pp, projectRoot, ec);
            if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
                packPathToSave = rel.string();
            }
        }
        j["navPackPath"] = packPathToSave;
    }
    j["debugDrawOffset"] = n.DebugDrawOffset;
    j["agentPlacementOffset"] = n.AgentPlacementOffset;
    j["costAwareSmoothing"] = n.CostAwareSmoothing;
    
    // Stitching settings
    j["enableStitching"] = n.EnableStitching;
    j["stitchEpsilon"] = n.StitchEpsilon;
    j["stitchMaxNormalAngle"] = n.StitchMaxNormalAngleDeg;
    j["stitchMaxHeight"] = n.StitchMaxHeight;
    j["stitchMaxXZ"] = n.StitchMaxXZ;

    // Multi-navmesh domains + auto portals
    j["domainId"] = n.DomainId;
    j["domainPriority"] = n.DomainPriority;
    j["autoPortalEnabled"] = n.AutoPortalEnabled;
    j["autoPortalMaxXZ"] = n.AutoPortalMaxXZ;
    j["autoPortalMaxHeight"] = n.AutoPortalMaxHeight;
    
    // Asset persistence (similar to terrain) - always write project-relative paths for portability
    j["navMeshDataGuid"] = n.NavMeshDataGuid;
    std::string pathToSave = n.AssetPath;
    if (!pathToSave.empty()) {
        fs::path p = fs::path(pathToSave).lexically_normal();
        const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
        if (p.is_absolute() && !projectRoot.empty()) {
            std::error_code ec;
            fs::path rel = fs::relative(p, projectRoot, ec);
            if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
                pathToSave = rel.string();
            }
        }
    }
    j["assetPath"] = pathToSave;
    
    return j;
}

void Serializer::DeserializeNavMesh(const json& j, nav::NavMeshComponent& n) {
    if (!j.is_object()) return; // Guard against non-object JSON
    n.Enabled = j.value("enabled", true);
    n.BakeHash = j.value<uint64_t>("hash", 0);
    if (j.contains("boundsMin")) n.AABB.min = DeserializeVec3(j["boundsMin"]);
    if (j.contains("boundsMax")) n.AABB.max = DeserializeVec3(j["boundsMax"]);
    if (j.contains("bake") && j["bake"].is_object()) {
        const auto& b = j["bake"];
        n.Bake.cellSize = b.value("cellSize", n.Bake.cellSize);
        n.Bake.cellHeight = b.value("cellHeight", n.Bake.cellHeight);
        n.Bake.agentRadius = b.value("agentRadius", n.Bake.agentRadius);
        n.Bake.agentHeight = b.value("agentHeight", n.Bake.agentHeight);
        n.Bake.agentMaxClimb = b.value("agentMaxClimb", n.Bake.agentMaxClimb);
        n.Bake.agentMaxSlopeDeg = b.value("agentMaxSlopeDeg", n.Bake.agentMaxSlopeDeg);
    }
    // Terrain + chunk settings
    n.TerrainSampleStep = j.value("terrainSampleStep", 2u);
    n.GeometryIncludeRegexEnabled = j.value("geometryIncludeRegexEnabled", false);
    n.GeometryIncludeRegexPattern = j.value("geometryIncludeRegexPattern", std::string());
    if (!j.contains("geometryIncludeRegexEnabled") && !j.contains("geometryIncludeRegexPattern")) {
        const bool legacyEnabled = j.value("geometryRegexEnabled", false);
        const bool legacyExclude = j.value("geometryRegexExcludeMatches", true);
        const std::string legacyPattern = j.value("geometryRegexPattern", std::string());
        if (legacyEnabled && !legacyPattern.empty()) {
            // Legacy include mode maps directly; legacy exclude mode cannot be transformed without regex negation.
            if (!legacyExclude) {
                n.GeometryIncludeRegexEnabled = true;
                n.GeometryIncludeRegexPattern = legacyPattern;
            } else {
                n.GeometryIncludeRegexEnabled = false;
                n.GeometryIncludeRegexPattern.clear();
            }
        }
    }
    n.BakeVisibleChunksOnly = j.value("bakeVisibleChunksOnly", false);
    n.BakeVisibleChunkPadding = j.value("bakeVisibleChunkPadding", 0u);
    n.BakeMissingChunksOnly = j.value("bakeMissingChunksOnly", false);
    n.ChunkedNavEnabled = j.value("chunkedNavEnabled", false);
    if (j.contains("chunkingMode")) {
        n.ChunkingMode = static_cast<nav::NavChunkingMode>(j.value("chunkingMode", static_cast<int>(n.ChunkingMode)));
    }
    n.ChunkWorldSize = j.value("chunkWorldSize", n.ChunkWorldSize);
    n.ChunkBakePadding = j.value("chunkBakePadding", 1u);
    n.ChunkStreamRadius = j.value("chunkStreamRadius", 300.0f);
    std::string loadedPackPath = j.value("navPackPath", std::string());
    if (!loadedPackPath.empty()) {
        fs::path pp = fs::path(loadedPackPath).lexically_normal();
        const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
        if (pp.is_absolute() && !projectRoot.empty()) {
            std::error_code ec;
            fs::path rel = fs::relative(pp, projectRoot, ec);
            if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
                n.NavPackPath = rel.string();
            } else {
                n.NavPackPath.clear(); // Will be rebuilt from AssetPath on next bake
            }
        } else {
            n.NavPackPath = pp.string();
        }
    }
    n.DebugDrawOffset = j.value("debugDrawOffset", 0.15f);
    n.AgentPlacementOffset = j.value("agentPlacementOffset", 0.0f);
    n.CostAwareSmoothing = j.value("costAwareSmoothing", true);
    
    // Stitching settings
    n.EnableStitching = j.value("enableStitching", n.EnableStitching);
    n.StitchEpsilon = j.value("stitchEpsilon", 0.05f);
    n.StitchMaxNormalAngleDeg = j.value("stitchMaxNormalAngle", 45.0f);
    n.StitchMaxHeight = j.value("stitchMaxHeight", 0.35f);
    n.StitchMaxXZ = j.value("stitchMaxXZ", 0.5f);

    // Multi-navmesh domains + auto portals
    n.DomainId = j.value("domainId", n.DomainId);
    n.DomainPriority = j.value("domainPriority", n.DomainPriority);
    n.AutoPortalEnabled = j.value("autoPortalEnabled", n.AutoPortalEnabled);
    n.AutoPortalMaxXZ = j.value("autoPortalMaxXZ", n.AutoPortalMaxXZ);
    n.AutoPortalMaxHeight = j.value("autoPortalMaxHeight", n.AutoPortalMaxHeight);
    
    // Asset persistence (similar to terrain)
    if (j.contains("navMeshDataGuid")) {
        try { j.at("navMeshDataGuid").get_to(n.NavMeshDataGuid); } catch(...) {}
    }
    // Normalize the asset path to use platform-correct separators; convert absolute paths to project-relative for portability
    std::string loadedPath = j.value("assetPath", std::string());
    if (!loadedPath.empty()) {
        fs::path p = fs::path(loadedPath).lexically_normal();
        const fs::path& projectRoot = FileSystem::Instance().GetProjectRoot();
        if (p.is_absolute() && !projectRoot.empty()) {
            std::error_code ec;
            fs::path rel = fs::relative(p, projectRoot, ec);
            if (!ec && !rel.empty() && rel.string().find("..") == std::string::npos) {
                n.AssetPath = rel.string();
            } else {
                // Path from another machine; use default relative form
                n.AssetPath = nav::NavMeshComponent::BuildDefaultAssetPath(n.NavMeshDataGuid);
            }
        } else {
            n.AssetPath = p.string();
        }
        std::cout << "[Nav] Deserialized NavMesh: GUID=" << n.NavMeshDataGuid.ToString() 
                  << ", AssetPath='" << n.AssetPath << "'" << std::endl;
    } else {
        std::cout << "[Nav] Deserialized NavMesh: AssetPath was EMPTY in scene file" << std::endl;
    }
    
    // Legacy source/override fields are intentionally ignored in Recast-only mode.
}

json Serializer::SerializeNavAgent(const nav::NavAgentComponent& a) {
    json j;
    j["enabled"] = a.Enabled;
    j["navMeshEntity"] = a.NavMeshEntity;
    j["arriveThreshold"] = a.ArriveThreshold;
    j["repathInterval"] = a.RepathInterval;
    j["autoRepath"] = a.AutoRepath;
    j["avoidanceRadiusMul"] = a.AvoidanceRadiusMul;
    j["steeringSmoothness"] = a.SteeringSmoothness;
    j["arrivalSlowdownDist"] = a.ArrivalSlowdownDist;
    j["params"] = {
        {"radius", a.Params.radius},
        {"height", a.Params.height},
        {"maxSlopeDeg", a.Params.maxSlopeDeg},
        {"maxStep", a.Params.maxStep},
        {"maxSpeed", a.Params.maxSpeed},
        {"maxAccel", a.Params.maxAccel},
        {"preferredDomainId", a.Params.preferredDomainId}
    };
    return j;
}

void Serializer::DeserializeNavAgent(const json& j, nav::NavAgentComponent& a) {
    if (!j.is_object()) return; // Guard against non-object JSON
    a.Enabled = j.value("enabled", true);
    a.NavMeshEntity = j.value("navMeshEntity", (EntityID)0);
    a.ArriveThreshold = j.value("arriveThreshold", a.ArriveThreshold);
    a.RepathInterval = j.value("repathInterval", a.RepathInterval);
    a.AutoRepath = j.value("autoRepath", a.AutoRepath);
    a.AvoidanceRadiusMul = j.value("avoidanceRadiusMul", a.AvoidanceRadiusMul);
    a.SteeringSmoothness = j.value("steeringSmoothness", a.SteeringSmoothness);
    a.ArrivalSlowdownDist = j.value("arrivalSlowdownDist", a.ArrivalSlowdownDist);
    if (j.contains("params") && j["params"].is_object()) {
        const auto& p = j["params"];
        a.Params.radius = p.value("radius", a.Params.radius);
        a.Params.height = p.value("height", a.Params.height);
        a.Params.maxSlopeDeg = p.value("maxSlopeDeg", a.Params.maxSlopeDeg);
        a.Params.maxStep = p.value("maxStep", a.Params.maxStep);
        a.Params.maxSpeed = p.value("maxSpeed", a.Params.maxSpeed);
        a.Params.maxAccel = p.value("maxAccel", a.Params.maxAccel);
        a.Params.preferredDomainId = p.value("preferredDomainId", a.Params.preferredDomainId);
    }
}

json Serializer::SerializeNavLink(const nav::NavLinkComponent& l) {
    json j;
    j["enabled"] = l.Enabled;
    j["start"] = SerializeVec3(l.Start);
    j["end"] = SerializeVec3(l.End);
    j["radius"] = l.Radius;
    j["cost"] = l.Cost;
    j["flags"] = l.Flags;
    j["bidirectional"] = l.Bidirectional;
    j["useWorldSpace"] = l.UseWorldSpace;
    return j;
}

void Serializer::DeserializeNavLink(const json& j, nav::NavLinkComponent& l) {
    if (!j.is_object()) return;
    l.Enabled = j.value("enabled", true);
    if (j.contains("start")) l.Start = DeserializeVec3(j["start"]);
    if (j.contains("end")) l.End = DeserializeVec3(j["end"]);
    l.Radius = j.value("radius", l.Radius);
    l.Cost = j.value("cost", l.Cost);
    l.Flags = j.value("flags", l.Flags);
    l.Bidirectional = j.value("bidirectional", l.Bidirectional);
    l.UseWorldSpace = j.value("useWorldSpace", l.UseWorldSpace);
}

json Serializer::SerializePortal(const PortalComponent& p) {
    json j;
    j["enabled"] = p.Enabled;
    j["targetScene"] = p.TargetScenePath;
    if (p.TargetPortalGuid != ClaymoreGUID()) {
        j["targetPortalGuid"] = p.TargetPortalGuid;
    }
    if (!p.TargetPortalPath.empty()) {
        j["targetPortalPath"] = p.TargetPortalPath;
    }
    j["entryOffset"] = SerializeVec3(p.EntryOffset);
    j["exitOffset"] = SerializeVec3(p.ExitOffset);
    j["autoDetect"] = p.AutoDetect;
    j["triggerRadius"] = p.TriggerRadius;
    j["fireExitEvents"] = p.FireExitEvents;
    return j;
}

void Serializer::DeserializePortal(const json& j, PortalComponent& p) {
    if (!j.is_object()) return;
    p.Enabled = j.value("enabled", true);
    p.TargetScenePath = j.value("targetScene", std::string());
    if (!p.TargetScenePath.empty()) {
        try { p.TargetScenePath = IVirtualFS::NormalizePath(p.TargetScenePath); } catch(...) {}
    }
    if (j.contains("targetPortalGuid")) {
        try { j.at("targetPortalGuid").get_to(p.TargetPortalGuid); } catch(...) {}
    }
    p.TargetPortalPath = j.value("targetPortalPath", std::string());
    if (j.contains("entryOffset")) p.EntryOffset = DeserializeVec3(j["entryOffset"]);
    if (j.contains("exitOffset")) p.ExitOffset = DeserializeVec3(j["exitOffset"]);
    p.AutoDetect = j.value("autoDetect", true);
    p.TriggerRadius = j.value("triggerRadius", 1.0f);
    p.FireExitEvents = j.value("fireExitEvents", true);
    p.ResetRuntime();
}

//==============================================================================
// PrefabInstanceComponent Serialization
//==============================================================================

static json SerializePropertyOverride(const prefab::PropertyOverride& ov) {
    json j;
    j["targetGuid"] = ov.TargetEntityGuid;
    j["component"] = ov.ComponentKey;
    j["value"] = ov.Value;
    
    // Include resolution hints for fuzzy matching
    if (!ov.Hints.NodePath.empty()) j["hints"]["nodePath"] = ov.Hints.NodePath;
    if (!ov.Hints.NormalizedPath.empty()) j["hints"]["normalizedPath"] = ov.Hints.NormalizedPath;
    if (!ov.Hints.NormalizedName.empty()) j["hints"]["normalizedName"] = ov.Hints.NormalizedName;
    if (!ov.Hints.StableMeshName.empty()) j["hints"]["stableMeshName"] = ov.Hints.StableMeshName;
    if (!ov.Hints.ParentNormalizedName.empty()) j["hints"]["parentName"] = ov.Hints.ParentNormalizedName;
    if (ov.Hints.ContentHash != 0) j["hints"]["contentHash"] = ov.Hints.ContentHash;
    if (ov.Hints.MeshFileId >= 0) j["hints"]["meshFileId"] = ov.Hints.MeshFileId;
    
    return j;
}

static prefab::PropertyOverride DeserializePropertyOverride(const json& j) {
    prefab::PropertyOverride ov;
    if (j.contains("targetGuid")) j.at("targetGuid").get_to(ov.TargetEntityGuid);
    ov.ComponentKey = j.value("component", "");
    if (j.contains("value")) ov.Value = j["value"];
    
    // Parse resolution hints
    if (j.contains("hints") && j["hints"].is_object()) {
        const auto& h = j["hints"];
        ov.Hints.NodePath = h.value("nodePath", "");
        ov.Hints.NormalizedPath = h.value("normalizedPath", "");
        ov.Hints.NormalizedName = h.value("normalizedName", "");
        ov.Hints.StableMeshName = h.value("stableMeshName", "");
        ov.Hints.ParentNormalizedName = h.value("parentName", "");
        ov.Hints.ContentHash = h.value("contentHash", 0ULL);
        ov.Hints.MeshFileId = h.value("meshFileId", -1);
    }
    
    return ov;
}

static json SerializeAddedEntity(const prefab::AddedEntity& added) {
    json j;
    j["guid"] = added.Guid;
    j["name"] = added.Name;
    if (!(added.ParentGuid.high == 0 && added.ParentGuid.low == 0)) {
        j["parentGuid"] = added.ParentGuid;
    }
    j["components"] = added.Components;
    
    if (!added.Children.empty()) {
        j["children"] = json::array();
        for (const auto& child : added.Children) {
            j["children"].push_back(SerializeAddedEntity(child));
        }
    }
    
    return j;
}

static prefab::AddedEntity DeserializeAddedEntity(const json& j) {
    prefab::AddedEntity added;
    if (j.contains("guid")) j.at("guid").get_to(added.Guid);
    added.Name = j.value("name", "");
    if (j.contains("parentGuid")) j.at("parentGuid").get_to(added.ParentGuid);
    if (j.contains("components")) added.Components = j["components"];
    
    if (j.contains("children") && j["children"].is_array()) {
        for (const auto& child : j["children"]) {
            added.Children.push_back(DeserializeAddedEntity(child));
        }
    }
    
    return added;
}

static json SerializeRemovedEntity(const prefab::RemovedEntity& removed) {
    json j;
    j["guid"] = removed.Guid;
    if (!removed.NodePath.empty()) j["nodePath"] = removed.NodePath;
    if (!removed.NormalizedPath.empty()) j["normalizedPath"] = removed.NormalizedPath;
    if (!removed.NormalizedName.empty()) j["normalizedName"] = removed.NormalizedName;
    if (!removed.StableMeshName.empty()) j["stableMeshName"] = removed.StableMeshName;
    if (removed.ContentHash != 0) j["contentHash"] = removed.ContentHash;
    if (removed.MeshFileId >= 0) j["meshFileId"] = removed.MeshFileId;
    return j;
}

static prefab::RemovedEntity DeserializeRemovedEntity(const json& j) {
    prefab::RemovedEntity removed;
    if (j.contains("guid")) j.at("guid").get_to(removed.Guid);
    removed.NodePath = j.value("nodePath", "");
    removed.NormalizedPath = j.value("normalizedPath", "");
    removed.NormalizedName = j.value("normalizedName", "");
    removed.StableMeshName = j.value("stableMeshName", "");
    removed.ContentHash = j.value("contentHash", 0ULL);
    removed.MeshFileId = j.value("meshFileId", -1);
    return removed;
}

json Serializer::SerializePrefabInstance(const PrefabInstanceComponent& instance) {
    json j;
    
    // Source prefab identity
    j["prefabGuid"] = instance.PrefabAssetGuid;
    if (!instance.PrefabPath.empty()) {
        j["prefabPath"] = instance.PrefabPath;
    }
    
    // Owned entity GUIDs (still useful for instance membership tracking)
    if (!instance.OwnedEntityGuids.empty()) {
        j["owned"] = json::array();
        for (const auto& guid : instance.OwnedEntityGuids) {
            j["owned"].push_back(guid);
        }
    }
    
    // DEPRECATED: Overrides, AddedEntities, RemovedEntities are no longer serialized here.
    // They are now stored in the scene's 'children' array on the prefab instance root entity,
    // using the same pattern as model roots. This simplifies the prefab system.
    
    // Model asset tracking (for model-based prefabs)
    if (!(instance.ModelAssetGuid.high == 0 && instance.ModelAssetGuid.low == 0)) {
        j["modelGuid"] = instance.ModelAssetGuid;
    }
    if (!instance.ModelAssetPath.empty()) {
        j["modelPath"] = instance.ModelAssetPath;
    }
    
    // GUID remapping (prefab GUID → instance GUID)
    // This is critical for entity resolution within the instance
    if (!instance.PrefabToInstanceGuid.empty()) {
        j["guidRemap"] = json::array();
        for (const auto& [prefabGuid, instanceGuid] : instance.PrefabToInstanceGuid) {
            json entry;
            entry["prefab"] = prefabGuid;
            entry["instance"] = instanceGuid;
            j["guidRemap"].push_back(std::move(entry));
        }
    }
    
    return j;
}

void Serializer::DeserializePrefabInstance(const json& j, PrefabInstanceComponent& instance) {
    if (!j.is_object()) return;
    
    // Source prefab identity
    if (j.contains("prefabGuid")) j.at("prefabGuid").get_to(instance.PrefabAssetGuid);
    instance.PrefabPath = j.value("prefabPath", "");
    
    // Owned entity GUIDs
    instance.OwnedEntityGuids.clear();
    if (j.contains("owned") && j["owned"].is_array()) {
        for (const auto& guid : j["owned"]) {
            ClaymoreGUID g;
            guid.get_to(g);
            instance.OwnedEntityGuids.push_back(g);
        }
    }
    
    // Property overrides
    instance.Overrides.clear();
    if (j.contains("overrides") && j["overrides"].is_array()) {
        for (const auto& ov : j["overrides"]) {
            instance.Overrides.push_back(DeserializePropertyOverride(ov));
        }
    }
    
    // Added entities
    instance.AddedEntities.clear();
    if (j.contains("added") && j["added"].is_array()) {
        for (const auto& added : j["added"]) {
            instance.AddedEntities.push_back(DeserializeAddedEntity(added));
        }
    }
    
    // Removed entities
    instance.RemovedEntities.clear();
    if (j.contains("removed") && j["removed"].is_array()) {
        for (const auto& removed : j["removed"]) {
            instance.RemovedEntities.push_back(DeserializeRemovedEntity(removed));
        }
    }
    
    // Model asset tracking
    if (j.contains("modelGuid")) j.at("modelGuid").get_to(instance.ModelAssetGuid);
    instance.ModelAssetPath = j.value("modelPath", "");
    
    // GUID remapping
    instance.PrefabToInstanceGuid.clear();
    instance.InstanceToPrefabGuid.clear();
    if (j.contains("guidRemap") && j["guidRemap"].is_array()) {
        for (const auto& entry : j["guidRemap"]) {
            if (!entry.is_object()) continue;
            ClaymoreGUID prefabGuid, instanceGuid;
            if (entry.contains("prefab")) entry.at("prefab").get_to(prefabGuid);
            if (entry.contains("instance")) entry.at("instance").get_to(instanceGuid);
            if ((prefabGuid.high != 0 || prefabGuid.low != 0) && 
                (instanceGuid.high != 0 || instanceGuid.low != 0)) {
                instance.PrefabToInstanceGuid[prefabGuid] = instanceGuid;
                instance.InstanceToPrefabGuid[instanceGuid] = prefabGuid;
            }
        }
    }
}

// Helper: Restore saved instance GUIDs to prefab instance entities
// This ensures that GUID-based entity references remain stable across scene reloads
static void RestorePrefabInstanceGuids(Scene& scene, EntityID instanceRoot, 
                                        const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& freshInstanceToPrefab,
                                        PrefabInstanceComponent& instanceComponent) {
    if (!instanceComponent.PrefabToInstanceGuid.empty()) {
        // Build a map of all existing GUIDs to detect collisions
        std::unordered_map<ClaymoreGUID, EntityID> existingGuids;
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID());
            if (!d) continue;
            if (d->EntityGuid.high != 0 || d->EntityGuid.low != 0) {
                existingGuids[d->EntityGuid] = e.GetID();
            }
        }
        
        // Walk the instance subtree and restore saved GUIDs
        std::function<void(EntityID)> restoreGuids = [&](EntityID id) {
            auto* data = scene.GetEntityData(id);
            if (!data) return;
            
            // Look up this entity's prefab GUID using the fresh instance mapping
            auto itPrefabGuid = freshInstanceToPrefab.find(data->EntityGuid);
            if (itPrefabGuid != freshInstanceToPrefab.end()) {
                const ClaymoreGUID& prefabGuid = itPrefabGuid->second;
                
                // Check if we have a saved instance GUID for this prefab GUID
                auto itSavedGuid = instanceComponent.PrefabToInstanceGuid.find(prefabGuid);
                if (itSavedGuid != instanceComponent.PrefabToInstanceGuid.end()) {
                    const ClaymoreGUID& savedInstanceGuid = itSavedGuid->second;
                    
                    // Check for GUID collision with non-prefab entities
                    auto collisionIt = existingGuids.find(savedInstanceGuid);
                    if (collisionIt != existingGuids.end() && collisionIt->second != id) {
                        // GUID collision detected - this would overwrite another entity's GUID
                        // Skip restoration for this entity to prevent breaking references
                        std::cerr << "[Serializer] WARNING: Skipping GUID restoration for entity " << id 
                                  << " - saved GUID " << savedInstanceGuid.ToString() 
                                  << " conflicts with entity " << collisionIt->second << std::endl;
                    } else {
                        // Safe to restore - update the entity's GUID
                        data->EntityGuid = savedInstanceGuid;
                        
                        // Update the reverse mapping in the component
                        instanceComponent.InstanceToPrefabGuid[savedInstanceGuid] = prefabGuid;
                        
                        // Update existing GUIDs map
                        existingGuids[savedInstanceGuid] = id;
                    }
                }
            }
            
            // Recursively process children
            for (EntityID child : data->Children) {
                restoreGuids(child);
            }
        };
        
        restoreGuids(instanceRoot);
        
        // Rebuild OwnedEntityGuids from the updated subtree
        instanceComponent.OwnedEntityGuids.clear();
        std::function<void(EntityID)> collectGuids = [&](EntityID id) {
            auto* data = scene.GetEntityData(id);
            if (!data) return;
            instanceComponent.OwnedEntityGuids.push_back(data->EntityGuid);
            for (EntityID child : data->Children) {
                collectGuids(child);
            }
        };
        collectGuids(instanceRoot);
    }
}


glm::vec3 Serializer::DeserializeVec3(const json& data) {
    return glm::vec3{data["x"], data["y"], data["z"]};
}

json Serializer::SerializeMat4(const glm::mat4& mat) {
    json result = json::array();
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            result.push_back(mat[i][j]);
        }
    }
    return result;
}

glm::mat4 Serializer::DeserializeMat4(const json& data) {
    glm::mat4 mat(1.0f);
    if (data.is_array() && data.size() == 16) {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                mat[i][j] = data[i * 4 + j];
            }
        }
    }
    return mat;
}

// Component serialization
json Serializer::SerializeTransform(const TransformComponent& transform) {
    json data;
    data["position"] = SerializeVec3(transform.Position);
    data["rotation"] = SerializeVec3(transform.Rotation);
    data["scale"] = SerializeVec3(transform.Scale);
    // Preserve quaternion-based rotation when authoring uses it
    data["useQuatRotation"] = transform.UseQuatRotation;
    data["rotationQ"] = json::array({ transform.RotationQ.w, transform.RotationQ.x, transform.RotationQ.y, transform.RotationQ.z });
    data["localMatrix"] = SerializeMat4(transform.LocalMatrix);
    data["worldMatrix"] = SerializeMat4(transform.WorldMatrix);
    data["transformDirty"] = transform.TransformDirty;
    return data;
}

void Serializer::DeserializeTransform(const json& data, TransformComponent& transform) {
    if (data.contains("position")) transform.Position = DeserializeVec3(data["position"]);
    if (data.contains("rotation")) transform.Rotation = DeserializeVec3(data["rotation"]);
    if (data.contains("scale")) transform.Scale = DeserializeVec3(data["scale"]);
    // Quaternions (preferred if present)
    if (data.contains("rotationQ") && data["rotationQ"].is_array() && data["rotationQ"].size() == 4) {
        // Stored as [w,x,y,z] for readability
        transform.RotationQ = glm::quat(
            (float)data["rotationQ"][0],
            (float)data["rotationQ"][1],
            (float)data["rotationQ"][2],
            (float)data["rotationQ"][3]
        );
        transform.UseQuatRotation = true;
    }
    if (data.contains("useQuatRotation")) {
        bool uqr = data["useQuatRotation"].get<bool>();
        // If rotationQ missing but flag true, derive from Euler now
        if (uqr && !(data.contains("rotationQ") && data["rotationQ"].is_array())) {
            glm::mat4 r = glm::yawPitchRoll(
                glm::radians(transform.Rotation.y),
                glm::radians(transform.Rotation.x),
                glm::radians(transform.Rotation.z));
            transform.RotationQ = glm::quat_cast(r);
        }
        transform.UseQuatRotation = uqr;
    }
    if (data.contains("localMatrix")) transform.LocalMatrix = DeserializeMat4(data["localMatrix"]);
    if (data.contains("worldMatrix")) transform.WorldMatrix = DeserializeMat4(data["worldMatrix"]);
    if (data.contains("transformDirty")) transform.TransformDirty = data["transformDirty"];
}

// ---------------- BlendShape Weights ----------------
json Serializer::SerializeBlendShapeWeights(const BlendShapeComponent& blendShapes) {
    json j = json::array();
    for (const auto& shape : blendShapes.Shapes) {
        // Only serialize if weight is non-zero (non-default)
        if (shape.Weight != 0.0f) {
            j.push_back({{"name", shape.Name}, {"weight", shape.Weight}});
        }
    }
    return j;
}

void Serializer::DeserializeBlendShapeWeights(const json& j, BlendShapeComponent& blendShapes) {
    if (!j.is_array()) return;
    // Build a map for fast lookup by name
    std::unordered_map<std::string, float> weightMap;
    for (const auto& entry : j) {
        if (entry.is_object() && entry.contains("name") && entry.contains("weight")) {
            weightMap[entry["name"].get<std::string>()] = entry["weight"].get<float>();
        }
    }
    // Apply weights to matching shapes
    for (auto& shape : blendShapes.Shapes) {
        if (shape.NameHash == 0 && !shape.Name.empty()) {
            shape.UpdateNameHash();
        }
        auto it = weightMap.find(shape.Name);
        if (it != weightMap.end()) {
            shape.Weight = it->second;
        }
    }
    if (!weightMap.empty()) {
        blendShapes.Dirty = true;
    }
}

// ---------------- Unified Morph Weights ----------------
json Serializer::SerializeUnifiedMorphWeights(const UnifiedMorphComponent& unifiedMorph) {
    json j = json::array();
    for (size_t i = 0; i < unifiedMorph.Names.size() && i < unifiedMorph.Weights.size(); ++i) {
        // Only serialize if weight is non-zero (non-default)
        if (unifiedMorph.Weights[i] != 0.0f) {
            j.push_back({{"name", unifiedMorph.Names[i]}, {"weight", unifiedMorph.Weights[i]}});
        }
    }
    return j;
}

void Serializer::DeserializeUnifiedMorphWeights(const json& j, UnifiedMorphComponent& unifiedMorph) {
    if (!j.is_array()) return;
    // Build a map for fast lookup by name
    std::unordered_map<std::string, float> weightMap;
    for (const auto& entry : j) {
        if (entry.is_object() && entry.contains("name") && entry.contains("weight")) {
            weightMap[entry["name"].get<std::string>()] = entry["weight"].get<float>();
        }
    }
    // Apply weights to matching unified morphs
    for (size_t i = 0; i < unifiedMorph.Names.size() && i < unifiedMorph.Weights.size(); ++i) {
        auto it = weightMap.find(unifiedMorph.Names[i]);
        if (it != weightMap.end()) {
            unifiedMorph.Weights[i] = it->second;
        }
    }
}


// ---------------- TintMaskController ----------------
// NOTE: TintTarget.TargetEntity is stored by path relative to the model root,
// not by EntityID. This allows the target to be resolved after model reinstantiation.
// The path is stored as "entityPath" field. On deserialization, we store the path
// in a separate field and the caller must resolve it to EntityID using the scene graph.
json Serializer::SerializeTintController(const TintMaskController& tint) {
    json j;
    // Legacy field (backwards compatibility)
    j["namePattern"] = tint.NamePattern;
    
    // New: Explicit targets - stored by entity path for scene reload compatibility
    json targetsArr = json::array();
    for (const auto& target : tint.Targets) {
        json t;
        // Store entity by path instead of ID for scene reload compatibility
        // The entityPath is populated by SerializeTintControllerWithContext
        t["entity"] = static_cast<int64_t>(target.TargetEntity);  // Keep for backward compat
        t["slot"] = target.MaterialSlot;
        t["blendMode"] = static_cast<int>(target.BlendMode);
        t["useColor"] = target.UseTargetColor;
        t["color"] = {target.Color.x, target.Color.y, target.Color.z, target.Color.w};
        targetsArr.push_back(t);
    }
    j["targets"] = targetsArr;
    
    // Global settings
    j["globalBlendMode"] = static_cast<int>(tint.GlobalBlendMode);
    j["autoIncludeParentedSkinnedMeshes"] = tint.AutoIncludeParentedSkinnedMeshes;
    j["useTintMask"] = tint.UseTintMask;
    j["tintColor0"] = {tint.TintColor0.x, tint.TintColor0.y, tint.TintColor0.z, tint.TintColor0.w};
    j["tintColor1"] = {tint.TintColor1.x, tint.TintColor1.y, tint.TintColor1.z, tint.TintColor1.w};
    j["tintColor2"] = {tint.TintColor2.x, tint.TintColor2.y, tint.TintColor2.z, tint.TintColor2.w};
    j["tintColor3"] = {tint.TintColor3.x, tint.TintColor3.y, tint.TintColor3.z, tint.TintColor3.w};
    j["baseTint"] = {tint.BaseTint.x, tint.BaseTint.y, tint.BaseTint.z, tint.BaseTint.w};
    j["usePbrOverrides"] = tint.UsePbrOverrides;
    j["pbrMetallic"] = tint.OverrideMetallic;
    j["pbrRoughness"] = tint.OverrideRoughness;
    j["pbrEmissionStrength"] = tint.OverrideEmissionStrength;
    j["pbrEmissionColor"] = {tint.OverrideEmissionColor.x, tint.OverrideEmissionColor.y, tint.OverrideEmissionColor.z};
    return j;
}

void Serializer::DeserializeTintController(const json& j, TintMaskController& tint) {
    if (!j.is_object()) return;
    
    // Legacy field (backwards compatibility)
    tint.NamePattern = j.value("namePattern", "");
    
    // New: Explicit targets
    tint.Targets.clear();
    if (j.contains("targets") && j["targets"].is_array()) {
        for (const auto& t : j["targets"]) {
            TintTarget target;
            target.TargetEntity = static_cast<EntityID>(t.value("entity", -1));
            target.MaterialSlot = t.value("slot", -1);
            target.BlendMode = static_cast<TintBlendMode>(t.value("blendMode", 0));
            target.UseTargetColor = t.value("useColor", false);
            if (t.contains("color") && t["color"].is_array() && t["color"].size() >= 4) {
                target.Color = glm::vec4(t["color"][0], t["color"][1], t["color"][2], t["color"][3]);
            }
            tint.Targets.push_back(target);
        }
    }
    
    // Global settings
    tint.GlobalBlendMode = static_cast<TintBlendMode>(j.value("globalBlendMode", 0));
    tint.AutoIncludeParentedSkinnedMeshes = j.value("autoIncludeParentedSkinnedMeshes", true);
    tint.UseTintMask = j.value("useTintMask", false);
    tint.UsePbrOverrides = j.value("usePbrOverrides", false);
    tint.OverrideMetallic = j.value("pbrMetallic", tint.OverrideMetallic);
    tint.OverrideRoughness = j.value("pbrRoughness", tint.OverrideRoughness);
    tint.OverrideEmissionStrength = j.value("pbrEmissionStrength", tint.OverrideEmissionStrength);
    if (j.contains("pbrEmissionColor") && j["pbrEmissionColor"].is_array() && j["pbrEmissionColor"].size() >= 3) {
        tint.OverrideEmissionColor = glm::vec3(j["pbrEmissionColor"][0], j["pbrEmissionColor"][1], j["pbrEmissionColor"][2]);
    }
    if (j.contains("tintColor0") && j["tintColor0"].is_array() && j["tintColor0"].size() >= 4) {
        tint.TintColor0 = glm::vec4(j["tintColor0"][0], j["tintColor0"][1], j["tintColor0"][2], j["tintColor0"][3]);
    }
    if (j.contains("tintColor1") && j["tintColor1"].is_array() && j["tintColor1"].size() >= 4) {
        tint.TintColor1 = glm::vec4(j["tintColor1"][0], j["tintColor1"][1], j["tintColor1"][2], j["tintColor1"][3]);
    }
    if (j.contains("tintColor2") && j["tintColor2"].is_array() && j["tintColor2"].size() >= 4) {
        tint.TintColor2 = glm::vec4(j["tintColor2"][0], j["tintColor2"][1], j["tintColor2"][2], j["tintColor2"][3]);
    }
    if (j.contains("tintColor3") && j["tintColor3"].is_array() && j["tintColor3"].size() >= 4) {
        tint.TintColor3 = glm::vec4(j["tintColor3"][0], j["tintColor3"][1], j["tintColor3"][2], j["tintColor3"][3]);
    }
    if (j.contains("baseTint") && j["baseTint"].is_array() && j["baseTint"].size() >= 4) {
        tint.BaseTint = glm::vec4(j["baseTint"][0], j["baseTint"][1], j["baseTint"][2], j["baseTint"][3]);
    }
    tint.NeedsRefresh = true; // Force refresh of matching meshes on load (for legacy pattern matching)
}

json Serializer::SerializeMesh(const MeshComponent& mesh) {
    json data;
    
    // Serialize both the old name-based system and new asset reference system
    data["meshName"] = mesh.MeshName;
    data["meshReference"] = mesh.meshReference;
    // Persist model/mesh location hints for robust reloads
    if (mesh.meshReference.IsValid()) {
        std::string p = AssetLibrary::Instance().GetPathForGUID(mesh.meshReference.guid);
        if (!p.empty()) data["meshPath"] = p;
        data["fileID"] = mesh.meshReference.fileID;
    }

    // Persist materials list (names only) and primary for backward compat
    if (!mesh.materials.empty()) {
        json jlist = json::array();
        for (auto& m : mesh.materials) jlist.push_back(m ? m->GetName() : std::string(""));
        data["materials"] = std::move(jlist);
    }
    if (mesh.material) data["materialName"] = mesh.material->GetName();

    // Persist base material texture source paths when available (PBR)
    // Note: These are saved even when the material is not unique so scenes can restore visuals.
    try {
        std::shared_ptr<Material> baseMat = mesh.material;
        if (!baseMat && !mesh.materials.empty()) baseMat = mesh.materials[0];
        if (baseMat) {
            if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(baseMat)) {
                if (!pbr->GetAlbedoPath().empty()) data["mat_albedoPath"] = pbr->GetAlbedoPath();
                if (!pbr->GetMetallicRoughnessPath().empty()) data["mat_mrPath"] = pbr->GetMetallicRoughnessPath();
                if (!pbr->GetNormalPath().empty()) data["mat_normalPath"] = pbr->GetNormalPath();
                // Capture base tint if modified by importer (fallback to per-mesh load later)
                glm::vec4 tint(1.0f);
                if (pbr->TryGetUniform("u_ColorTint", tint)) {
                    if (tint != glm::vec4(1.0f)) {
                        data["mat_tint"] = { tint.x, tint.y, tint.z, tint.w };
                    }
                }
                SerializeExtendedPBRState(data, pbr, "mat_");
            }
        }
    } catch(...) {}

    // Persist per-slot PBR texture paths for multi-material meshes
    if (!mesh.materials.empty()) {
        json slotMats = json::array();
        for (const auto& m : mesh.materials) {
            json jslot;
            if (auto sgMat = std::dynamic_pointer_cast<shadergraph::ShaderGraphMaterial>(m)) {
                // Serialize shader graph material
                jslot["shaderGraphPath"] = sgMat->GetShaderGraphPath();
                jslot["shaderGraphName"] = sgMat->GetName();  // Save material name for restoration
                jslot["uvScale"] = { sgMat->GetUVScale().x, sgMat->GetUVScale().y };
                jslot["uvOffset"] = { sgMat->GetUVOffset().x, sgMat->GetUVOffset().y };
                // Save state flags (for alpha blend, etc.)
                jslot["stateFlags"] = sgMat->GetStateFlags();
                
                // Serialize parameters
                const auto& params = sgMat->GetParameters();
                if (!params.empty()) {
                    json jparams = json::array();
                    for (const auto& param : params) {
                        json jp;
                        jp["name"] = param.name;
                        jp["displayName"] = param.displayName;
                        jp["type"] = static_cast<int>(param.type);
                        jp["value"] = { param.value.x, param.value.y, param.value.z, param.value.w };
                        if (!param.texturePath.empty()) {
                            jp["texturePath"] = param.texturePath;
                        }
                        jp["textureSlot"] = param.textureSlot;  // Save texture slot
                        jparams.push_back(std::move(jp));
                    }
                    jslot["shaderGraphParams"] = std::move(jparams);
                }
            } else if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(m)) {
                if (!pbr->GetAlbedoPath().empty()) jslot["albedoPath"] = pbr->GetAlbedoPath();
                if (!pbr->GetMetallicRoughnessPath().empty()) jslot["mrPath"] = pbr->GetMetallicRoughnessPath();
                if (!pbr->GetNormalPath().empty()) jslot["normalPath"] = pbr->GetNormalPath();
                glm::vec4 tint(1.0f);
                if (pbr->TryGetUniform("u_ColorTint", tint) && tint != glm::vec4(1.0f)) {
                    jslot["tint"] = { tint.x, tint.y, tint.z, tint.w };
                }
                SerializeExtendedPBRState(jslot, pbr, "");
            }
            // Persist alpha blending toggle per slot (works for any material type)
            if (m) {
                bool alpha = (m->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
                jslot["alpha"] = alpha;
            }
            slotMats.push_back(std::move(jslot));
        }
        data["slotMaterials"] = std::move(slotMats);
    }

    // Persist material asset paths (.mat files) for custom shader materials
    if (!mesh.MaterialAssetPaths.empty()) {
        bool hasAny = false;
        for (const auto& p : mesh.MaterialAssetPaths) {
            if (!p.empty()) { hasAny = true; break; }
        }
        if (hasAny) {
            data["materialAssetPaths"] = mesh.MaterialAssetPaths;
        }
    }

    // Persist primary material alpha when not using slot list
    if (mesh.material) {
        bool alpha = (mesh.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
        data["materialAlpha"] = alpha;
    }

    // Persist per-slot property blocks
    if (!mesh.SlotPropertyBlocks.empty()) {
        json slots = json::array();
        for (size_t i = 0; i < mesh.SlotPropertyBlocks.size(); ++i) {
            const auto& pb = mesh.SlotPropertyBlocks[i];
            json jpb;
            const auto& texPaths = (i < mesh.SlotPropertyBlockTexturePaths.size())
                ? mesh.SlotPropertyBlockTexturePaths[i]
                : kEmptyTexturePaths;
            SerializePropertyBlock(pb, texPaths, jpb, "vec4", "textures");
            slots.push_back(std::move(jpb));
        }
        data["slotPropertyBlocks"] = std::move(slots);
    }

    // Persist combined submesh indices if present
    if (!mesh.CombinedSubmeshFileIDs.empty()) data["combinedSubmeshes"] = mesh.CombinedSubmeshFileIDs;

    // Persist Mesh rendering overrides
    data["renderOnTop"] = mesh.RenderOnTop;
    data["renderOrder"] = mesh.RenderOrder;
    data["showBackfaces"] = mesh.ShowBackfaces;
    data["skipFrustumCulling"] = mesh.SkipFrustumCulling;
    if (mesh.BoundsPadding != 1.0f) data["boundsPadding"] = mesh.BoundsPadding;

    // Persist PropertyBlock overrides
    SerializePropertyBlock(mesh.PropertyBlock, mesh.PropertyBlockTexturePaths, data, "propertyBlockVec4", "propertyBlockTextures");
    try {
        bool primaryAlpha = mesh.material ? ((mesh.material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0) : false;
        std::string entName = "<unknown>";
        try {
            // Best-effort: find owning entity by scanning (can be costly, guarded by try)
            if (auto* sc = &Scene::Get()) {
                for (const auto& e : sc->GetEntities()) {
                    auto* d = sc->GetEntityData(e.GetID()); if (!d || !d->Mesh) continue;
                    if (d->Mesh.get() == &mesh) { entName = d->Name; break; }
                }
            }
        } catch(...) {}
        // Debug log removed to avoid spam during delta computation
        // std::cout << "[SerializeMesh] entity='" << entName
        //     << "' slots=" << mesh.materials.size()
        //     << " primaryAlpha=" << (primaryAlpha?1:0) << std::endl;
    } catch(...) {}
    return data;
}

void Serializer::DeserializeMesh(const json& data, MeshComponent& mesh) {
    std::unique_ptr<RenderOverridesComponent> discard;
    DeserializeMesh(data, mesh, discard);
}

void Serializer::DeserializeMesh(const json& data, MeshComponent& mesh, std::unique_ptr<RenderOverridesComponent>& renderOverrides) {

    // Pre-reserve materials capacity to avoid vector churn
    {
        size_t pre = mesh.materials.size();
        auto updatePre = [&](const char* key) {
            if (data.contains(key) && data[key].is_array())
                pre = std::max(pre, (size_t)data[key].size());
            };
        updatePre("materials");
        updatePre("slotMaterials");
        if (pre == 0) pre = 1;
        if (mesh.materials.capacity() < pre) mesh.materials.reserve(pre);
    }

    bool alphaOverride = false;
    bool alphaEnable = false;
    auto requestAlpha = [&](bool enable) {
        alphaOverride = true;
        alphaEnable = enable;
    };
    auto hasMutablePbrOverrides = [](const json& node, const std::string& prefix) {
        auto key = [&](const char* local) { return prefix + local; };
        return node.contains(key("aoPath")) ||
               node.contains(key("emissionPath")) ||
               node.contains(key("displacementPath")) ||
               node.contains(key("tintMaskPath")) ||
               node.contains(key("metallicScalar")) ||
               node.contains(key("roughnessScalar")) ||
               node.contains(key("normalScale")) ||
               node.contains(key("aoScalar")) ||
               node.contains(key("emissionStrength")) ||
               node.contains(key("displacementScale")) ||
               node.contains(key("emissionColor")) ||
               node.contains(key("uvScale")) ||
               node.contains(key("uvOffset")) ||
               node.contains(key("receiveShadowsOverride")) ||
               node.contains(key("receiveShadows"));
    };

    // --- Load mesh reference ---
    if (data.contains("meshReference")) {
        AssetReference newRef = mesh.meshReference;
        data["meshReference"].get_to(newRef);

        bool refChanged =
            (newRef.guid != mesh.meshReference.guid) ||
            (newRef.fileID != mesh.meshReference.fileID) ||
            (newRef.type != mesh.meshReference.type);

        mesh.meshReference = newRef;

        if (!mesh.mesh || refChanged) {
            std::shared_ptr<Mesh> loaded = AssetLibrary::Instance().LoadMesh(mesh.meshReference);
            if (loaded) mesh.mesh = loaded;
        }
    }

    // --- Handle primitives ---
    if (!mesh.mesh) {
        AssetReference::PrimitiveType prim = AssetReference::PrimitiveTypeFromGuid(mesh.meshReference.guid);
        if (prim == AssetReference::PrimitiveType::Unknown && mesh.meshReference.guid == ClaymoreGUID{}) {
            prim = static_cast<AssetReference::PrimitiveType>(mesh.meshReference.fileID);
        }
        switch (prim) {
        case AssetReference::PrimitiveType::Cube: mesh.mesh = StandardMeshManager::Instance().GetCubeMesh(); break;
        case AssetReference::PrimitiveType::Sphere: mesh.mesh = StandardMeshManager::Instance().GetSphereMesh(); break;
        case AssetReference::PrimitiveType::Plane: mesh.mesh = StandardMeshManager::Instance().GetPlaneMesh(); break;
        case AssetReference::PrimitiveType::Capsule: mesh.mesh = StandardMeshManager::Instance().GetCapsuleMesh(); break;
        default: break;
        }
    }

    // --- Handle combined submeshes (unchanged, just wrapped) ---
    std::vector<int> combinedIds;
    if (data.contains("combinedSubmeshes") && data["combinedSubmeshes"].is_array()) {
        for (const auto& v : data["combinedSubmeshes"]) combinedIds.push_back(v.get<int>());
    }
    if (combinedIds.size() > 1) {
        // ... [keep your existing combined mesh construction code] ...
        // NOTE: unchanged, omitted for brevity
    }

    // --- Right-size materials ---
    // CRITICAL: The mesh may already have slots from a fresh model instantiation (e.g., after Blender
    // re-export added new material slots). We must NOT shrink the array below the existing size,
    // as that would lose the new slots. The scene file's override data may have fewer slots than
    // the current model - that's expected and we should preserve the model's authoritative slot count.
    bool preserveModelSlotDefaults = false;
    {
        size_t existingSlots = mesh.materials.size();  // Preserve this as minimum!
        const bool hasFreshInstantiatedSlots = HasFreshInstantiatedMaterialSlots(mesh);
        size_t required = 0;
        if (data.contains("slotMaterials") && data["slotMaterials"].is_array())
            required = data["slotMaterials"].size();
        else if (!mesh.CombinedSubmeshFileIDs.empty())
            required = mesh.CombinedSubmeshFileIDs.size();
        else if (data.contains("materials") && data["materials"].is_array())
            required = data["materials"].size();
        if (required == 0) required = existingSlots > 0 ? existingSlots : 1;
        if (required > 64) required = 64;

        // IMPORTANT: When a fresh model instantiation already supplied slot materials, that slot
        // count is authoritative in both directions. Saved scene arrays can be stale after model
        // reimport, and growing to the serialized count recreates blank/default extra slots.
        if (hasFreshInstantiatedSlots) {
            required = existingSlots;
            preserveModelSlotDefaults = !mesh.MaterialSources.empty();
        } else {
            required = std::max(required, existingSlots);
        }

        if (mesh.materials.size() < required) {
            size_t oldSize = mesh.materials.size();
            mesh.materials.resize(required);
            for (size_t i = oldSize; i < required; ++i) {
                bool wantSkinned = false;
                if (data.contains("materials") && data["materials"].is_array() && i < data["materials"].size()) {
                    try {
                        const auto& nameNode = data["materials"][i];
                        if (nameNode.is_string()) {
                            std::string savedName = nameNode.get<std::string>();
                            if (savedName.find("Skinned") != std::string::npos)
                                wantSkinned = true;
                        }
                    }
                    catch (...) {}
                }
                MaterialSource defaultSource;
                defaultSource.Skinned = wantSkinned;
                mesh.materials[i] = AcquireMaterialFromSource(defaultSource, Scene::Get());
            }
        }
        // NOTE: We no longer shrink - existing slots are preserved even if override has fewer
    }
    // Ensure slot property block containers match material count
    if (mesh.SlotPropertyBlocks.size() < mesh.materials.size()) {
        mesh.SlotPropertyBlocks.resize(mesh.materials.size());
    } else if (mesh.SlotPropertyBlocks.size() > mesh.materials.size()) {
        mesh.SlotPropertyBlocks.resize(mesh.materials.size());
    }
    if (mesh.SlotPropertyBlockTexturePaths.size() < mesh.materials.size()) {
        mesh.SlotPropertyBlockTexturePaths.resize(mesh.materials.size());
    } else if (mesh.SlotPropertyBlockTexturePaths.size() > mesh.materials.size()) {
        mesh.SlotPropertyBlockTexturePaths.resize(mesh.materials.size());
    }
    
    // --- Load material asset files (.mat) if present ---
    if (data.contains("materialAssetPaths") && data["materialAssetPaths"].is_array()) {
        const auto& paths = data["materialAssetPaths"];
        mesh.MaterialAssetPaths.resize(mesh.materials.size());
        if (mesh.OwnedMaterialSlots.size() < mesh.materials.size()) {
            mesh.OwnedMaterialSlots.resize(mesh.materials.size(), false);
        }
        for (size_t i = 0; i < paths.size() && i < mesh.materials.size(); ++i) {
            if (!paths[i].is_string()) continue;
            std::string matPath = paths[i].get<std::string>();
            if (matPath.empty()) continue;
            mesh.MaterialAssetPaths[i] = matPath;
            if (auto loadedMat = MaterialAssetCache::Acquire(matPath)) {
                mesh.materials[i] = loadedMat;
                mesh.OwnedMaterialSlots[i] = false;
            }
        }
    }

    // --- Decide primary ---
    if (!mesh.materials.empty() && mesh.materials[0])
        mesh.material = mesh.materials[0];
    else
        mesh.material = MaterialManager::Instance().CreateSceneDefaultMaterial(&Scene::Get());

    // Apply persisted single-material overrides when slot list is absent
    // NOTE: To avoid polluting shared/singleton materials, we migrate texture paths
    // to property blocks instead of setting them directly on the material.
    // This preserves per-instance texture overrides without affecting other meshes
    // that share the same base material.
    {
        // Migrate legacy mat_* texture paths to property block instead of material
        auto migrateTexture = [&](const char* key, const char* sampler) {
            if (!data.contains(key)) return;
            std::string path = data[key].get<std::string>();
            if (path.empty()) return;
            // Load texture and store in the mesh's property block
            TextureSpecifier spec;
            spec.Path = path;
            bgfx::TextureHandle tex = AcquireTextureHandle(spec, TextureColorSpace::Linear);
            if (bgfx::isValid(tex)) {
                // Ensure property block storage exists
                if (mesh.SlotPropertyBlocks.empty()) {
                    mesh.SlotPropertyBlocks.resize(1);
                    mesh.SlotPropertyBlockTexturePaths.resize(1);
                }
                mesh.SlotPropertyBlocks[0].SetTexture(sampler, tex);
                mesh.SlotPropertyBlockTexturePaths[0][sampler] = path;
            }
        };
        migrateTexture("mat_albedoPath", "s_albedo");
        migrateTexture("mat_mrPath", "s_metallicRoughness");
        migrateTexture("mat_normalPath", "s_normalMap");
        
        // Migrate tint to property block as well
        if (data.contains("mat_tint") && data["mat_tint"].is_array() && data["mat_tint"].size() == 4) {
            glm::vec4 tint(data["mat_tint"][0], data["mat_tint"][1], data["mat_tint"][2], data["mat_tint"][3]);
            if (tint != glm::vec4(1.0f)) {
                if (mesh.SlotPropertyBlocks.empty()) {
                    mesh.SlotPropertyBlocks.resize(1);
                    mesh.SlotPropertyBlockTexturePaths.resize(1);
                }
                mesh.SlotPropertyBlocks[0].SetVector("u_ColorTint", tint);
            }
        }
        
        // Apply extended PBR state to property block
        std::shared_ptr<Material> base = mesh.material;
        if (!base && !mesh.materials.empty()) base = mesh.materials[0];
        if (base && hasMutablePbrOverrides(data, "mat_")) {
            if (auto cloned = base->Clone()) {
                base = cloned;
                mesh.material = cloned;
                if (!mesh.materials.empty()) {
                    mesh.materials[0] = cloned;
                }
            }
        }
        if (auto pbr = std::dynamic_pointer_cast<PBRMaterial>(base)) {
            DeserializeExtendedPBRState(data, "mat_", pbr);
        }
    }

    // --- Apply slot material properties (textures, tint, alpha) ---
    if (data.contains("slotMaterials") && data["slotMaterials"].is_array()) {
        const auto& arr = data["slotMaterials"];
        for (size_t i = 0; i < arr.size() && i < mesh.materials.size(); ++i) {
            const auto& js = arr[i];
            if (!js.is_object()) continue;
            
            // Ensure property block storage exists for this slot
            while (mesh.SlotPropertyBlocks.size() <= i) {
                mesh.SlotPropertyBlocks.push_back(MaterialPropertyBlock{});
                mesh.SlotPropertyBlockTexturePaths.push_back({});
            }
            while (mesh.OwnedMaterialSlots.size() <= i) {
                mesh.OwnedMaterialSlots.push_back(false);
            }
            
            // Check if this slot has a shader graph material
            if (js.contains("shaderGraphPath") && js["shaderGraphPath"].is_string()) {
                std::string sgPath = js["shaderGraphPath"].get<std::string>();
                if (!sgPath.empty()) {
                    // Create shader graph material from serialized data
                    shadergraph::ShaderGraphMaterialDesc sgDesc;
                    sgDesc.name = js.value("shaderGraphName", "SlotMaterial_" + std::to_string(i));
                    sgDesc.shaderGraphPath = sgPath;
                    
                    if (js.contains("uvScale") && js["uvScale"].is_array() && js["uvScale"].size() == 2) {
                        sgDesc.uvScale = glm::vec2(js["uvScale"][0], js["uvScale"][1]);
                    }
                    if (js.contains("uvOffset") && js["uvOffset"].is_array() && js["uvOffset"].size() == 2) {
                        sgDesc.uvOffset = glm::vec2(js["uvOffset"][0], js["uvOffset"][1]);
                    }
                    
                    // Restore state flags (includes alpha blend)
                    if (js.contains("stateFlags")) {
                        sgDesc.stateFlags = js["stateFlags"].get<uint64_t>();
                    }
                    
                    // Restore parameters
                    if (js.contains("shaderGraphParams") && js["shaderGraphParams"].is_array()) {
                        for (const auto& jp : js["shaderGraphParams"]) {
                            if (!jp.is_object()) continue;
                            shadergraph::MaterialParameter param;
                            param.name = jp.value("name", "");
                            param.displayName = jp.value("displayName", param.name);
                            param.type = static_cast<shadergraph::ShaderValueType>(jp.value("type", 0));
                            if (jp.contains("value") && jp["value"].is_array() && jp["value"].size() >= 4) {
                                param.value = glm::vec4(jp["value"][0], jp["value"][1], jp["value"][2], jp["value"][3]);
                            }
                            param.texturePath = jp.value("texturePath", "");
                            param.textureSlot = jp.value("textureSlot", -1);
                            sgDesc.parameters.push_back(std::move(param));
                        }
                    }
                    
                    auto sgMat = shadergraph::ShaderGraphMaterial::CreateFromDesc(sgDesc);
                    if (sgMat) {
                        // Apply alpha blend state (check both stateFlags and explicit alpha field)
                        bool hasAlpha = (sgDesc.stateFlags & BGFX_STATE_BLEND_ALPHA) != 0;
                        if (!hasAlpha && js.contains("alpha") && js["alpha"].get<bool>()) {
                            sgMat->m_StateFlags |= BGFX_STATE_BLEND_ALPHA;
                            hasAlpha = true;
                        }
                        if (hasAlpha) {
                            requestAlpha(true);
                        }
                        mesh.materials[i] = sgMat;
                        mesh.OwnedMaterialSlots[i] = true;
                        if (i == 0) mesh.material = sgMat;
                    } else {
                        std::cerr << "[DeserializeMesh] FAILED to create ShaderGraphMaterial from path: " << sgPath << std::endl;
                    }
                    continue; // Skip PBR processing for this slot
                }
            }
            if (auto pbr = EnsureEditablePBRMaterialForSlot(mesh, i)) {
                if (js.contains("albedoPath")) {
                    pbr->SetAlbedoTextureFromPath(js["albedoPath"].get<std::string>());
                }
                if (js.contains("mrPath")) {
                    pbr->SetMetallicRoughnessTextureFromPath(js["mrPath"].get<std::string>());
                }
                if (js.contains("normalPath")) {
                    pbr->SetNormalTextureFromPath(js["normalPath"].get<std::string>());
                }
                if (js.contains("tint") && js["tint"].is_array() && js["tint"].size() == 4) {
                    glm::vec4 tint(js["tint"][0], js["tint"][1], js["tint"][2], js["tint"][3]);
                    pbr->SetUniform("u_ColorTint", tint);
                }
                DeserializeExtendedPBRState(js, "", pbr);
            }

            if (js.contains("alpha")) {
                bool alpha = js["alpha"].get<bool>();
                if (i < mesh.materials.size() && mesh.materials[i]) {
                    if (alpha) {
                        mesh.materials[i]->m_StateFlags |= BGFX_STATE_BLEND_ALPHA;
                    } else {
                        mesh.materials[i]->m_StateFlags &= ~BGFX_STATE_BLEND_ALPHA;
                    }
                }
                requestAlpha(alpha);
            }
        }

        mesh.UniqueMaterial = std::any_of(
            mesh.OwnedMaterialSlots.begin(),
            mesh.OwnedMaterialSlots.end(),
            [](bool owned) { return owned; });
    }

    if (data.contains("materialAlpha")) {
        bool alpha = data["materialAlpha"].get<bool>();
        requestAlpha(alpha);
    }
    if (data.contains("forcedAlpha")) {
        bool alpha = data["forcedAlpha"].get<bool>();
        requestAlpha(alpha);
    }

    // --- Restore property blocks ---
    mesh.PropertyBlock = MaterialPropertyBlock{};
    mesh.PropertyBlockTexturePaths.clear();
    DeserializePropertyBlock(data, "propertyBlockVec4", "propertyBlockTextures", mesh.PropertyBlock, mesh.PropertyBlockTexturePaths);

    if (data.contains("slotPropertyBlocks") && data["slotPropertyBlocks"].is_array()) {
        const auto& arr = data["slotPropertyBlocks"];
        for (size_t i = 0; i < arr.size() && i < mesh.SlotPropertyBlocks.size(); ++i) {
            if (!arr[i].is_object()) continue;
            if (!HasSerializedPropertyBlockOverrides(arr[i], "vec4", "textures")) {
                // Preserve the instantiated model defaults when the scene only stores an empty slot block.
                continue;
            }
            DeserializePropertyBlock(arr[i], "vec4", "textures", mesh.SlotPropertyBlocks[i], mesh.SlotPropertyBlockTexturePaths[i], !preserveModelSlotDefaults);
        }
    }

    // --- Restore other flags ---
    mesh.RenderOnTop = data.value("renderOnTop", mesh.RenderOnTop);
    mesh.RenderOrder = data.value("renderOrder", mesh.RenderOrder);
    mesh.ShowBackfaces = data.value("showBackfaces", mesh.ShowBackfaces);
    mesh.SkipFrustumCulling = data.value("skipFrustumCulling", mesh.SkipFrustumCulling);
    mesh.BoundsPadding = data.value("boundsPadding", mesh.BoundsPadding);

    if (alphaOverride) {
        if (!renderOverrides) {
            renderOverrides = std::make_unique<RenderOverridesComponent>();
        }
        renderOverrides->AlphaBlendEnabled = alphaEnable;
    }
}

json Serializer::SerializeMeshProxy(const MeshProxyComponent& proxy, Scene& scene) {
    json j;
    EntityID targetId = proxy.TargetMesh != INVALID_ENTITY_ID ? proxy.TargetMesh : proxy.SerializedTarget;
    json ref = BuildEntityReferenceJson(scene, targetId);
    if (!ref.empty()) j["target"] = std::move(ref);
    if (!proxy.SubmeshSlots.empty()) j["slots"] = proxy.SubmeshSlots;
    if (!proxy.SlotPropertyBlocks.empty()) {
        json slots = json::array();
        for (size_t i = 0; i < proxy.SlotPropertyBlocks.size(); ++i) {
            json entry;
            const auto& texPaths = (i < proxy.SlotPropertyBlockTexturePaths.size())
                ? proxy.SlotPropertyBlockTexturePaths[i]
                : kEmptyTexturePaths;
            SerializePropertyBlock(proxy.SlotPropertyBlocks[i], texPaths, entry, "vec4", "textures");
            slots.push_back(std::move(entry));
        }
        j["slotPropertyBlocks"] = std::move(slots);
    }
    SerializePropertyBlock(proxy.PropertyBlock, proxy.PropertyBlockTexturePaths, j, "propertyBlockVec4", "propertyBlockTextures");
    return j;
}

void Serializer::DeserializeMeshProxy(const json& data, MeshProxyComponent& proxy, Scene& scene) {
    // NOTE: Do NOT destroy texture handles - they're shared via global texture cache
    auto releaseBlock = [](MaterialPropertyBlock& block) {
        block.Clear();  // Clear all maps (both string and ID-based) - don't destroy textures
    };
    releaseBlock(proxy.PropertyBlock);
    for (auto& pb : proxy.SlotPropertyBlocks) releaseBlock(pb);

    proxy = MeshProxyComponent{};

    if (data.contains("slots") && data["slots"].is_array()) {
        proxy.SubmeshSlots.clear();
        for (const auto& v : data["slots"]) proxy.SubmeshSlots.push_back(v.get<uint32_t>());
    } else {
        proxy.SubmeshSlots.clear();
    }

    proxy.SlotPropertyBlocks.assign(proxy.SubmeshSlots.size(), {});
    proxy.SlotPropertyBlockTexturePaths.assign(proxy.SubmeshSlots.size(), {});
    proxy.SlotMaterialOverrides.assign(proxy.SubmeshSlots.size(), nullptr);
    proxy.SlotMaterialAssetPaths.assign(proxy.SubmeshSlots.size(), std::string());
    proxy.SlotIndexLookup.clear();
    for (size_t i = 0; i < proxy.SubmeshSlots.size(); ++i) {
        proxy.SlotIndexLookup[proxy.SubmeshSlots[i]] = i;
    }

    proxy.PropertyBlock = MaterialPropertyBlock{};
    proxy.PropertyBlockTexturePaths.clear();
    DeserializePropertyBlock(data, "propertyBlockVec4", "propertyBlockTextures", proxy.PropertyBlock, proxy.PropertyBlockTexturePaths);

    if (data.contains("slotPropertyBlocks") && data["slotPropertyBlocks"].is_array()) {
        const auto& arr = data["slotPropertyBlocks"];
        for (size_t i = 0; i < arr.size() && i < proxy.SlotPropertyBlocks.size(); ++i) {
            if (!arr[i].is_object()) continue;
            DeserializePropertyBlock(arr[i], "vec4", "textures", proxy.SlotPropertyBlocks[i], proxy.SlotPropertyBlockTexturePaths[i]);
        }
    }

    proxy.TargetMesh = INVALID_ENTITY_ID;
    proxy.SerializedTarget = INVALID_ENTITY_ID;
    proxy.TargetScenePathHint.clear();
    proxy.TargetModelPathHint.clear();
    proxy.TargetGuidHint = ClaymoreGUID{};
    proxy.TargetModelGuidHint = ClaymoreGUID{};

    if (data.contains("target") && data["target"].is_object()) {
        const auto& targetNode = data["target"];
        try {
            EntityID resolved = ResolveEntityReferenceJson(targetNode, scene);
            if (resolved != INVALID_ENTITY_ID) {
                proxy.TargetMesh = resolved;
                proxy.SerializedTarget = resolved;
            }
        } catch(...) {}
        try {
            if (targetNode.contains("scenePath")) proxy.TargetScenePathHint = targetNode["scenePath"].get<std::string>();
        } catch(...) {}
        try {
            if (targetNode.contains("modelNodePath")) proxy.TargetModelPathHint = targetNode["modelNodePath"].get<std::string>();
        } catch(...) {}
        try {
            if (targetNode.contains("guid")) proxy.TargetGuidHint = ClaymoreGUID::FromString(targetNode["guid"].get<std::string>());
        } catch(...) {}
        try {
            if (targetNode.contains("modelGuid")) proxy.TargetModelGuidHint = ClaymoreGUID::FromString(targetNode["modelGuid"].get<std::string>());
        } catch(...) {}
    }
}

json Serializer::SerializeLight(const LightComponent& light) {
    json data;
    data["type"] = static_cast<int>(light.Type);
    data["color"] = SerializeVec3(light.Color);
    data["intensity"] = light.Intensity;
    data["pointShadowsEnabled"] = light.PointShadowsEnabled;
    return data;
}

json Serializer::SerializeAudioSource(const AudioSourceComponent& source) {
    json data;
    if (source.AudioClip.IsValid()) {
        data["audioClipGuid"] = source.AudioClip.guid.ToString();
    }
    if (!source.AudioPath.empty()) {
        data["audioPath"] = source.AudioPath;
    }
    data["volume"] = source.Volume;
    data["pitch"] = source.Pitch;
    data["loop"] = source.Loop;
    data["playOnAwake"] = source.PlayOnAwake;
    data["mute"] = source.Mute;
    data["spatial"] = source.Spatial;
    data["minDistance"] = source.MinDistance;
    data["maxDistance"] = source.MaxDistance;
    data["dopplerFactor"] = source.DopplerFactor;
    data["rolloff"] = source.Rolloff;
    return data;
}

void Serializer::DeserializeAudioSource(const json& data, AudioSourceComponent& source) {
    source.AudioClip = AssetReference();
    if (data.contains("audioClipGuid") && data["audioClipGuid"].is_string()) {
        ClaymoreGUID guid = ClaymoreGUID::FromString(data["audioClipGuid"].get<std::string>());
        if (guid != ClaymoreGUID()) {
            source.AudioClip = AssetReference(guid, 0, kAudioAssetType);
        }
    } else if (data.contains("audioClip") && data["audioClip"].is_object()) {
        const auto& clip = data["audioClip"];
        if (clip.contains("guid") && clip["guid"].is_string()) {
            ClaymoreGUID guid = ClaymoreGUID::FromString(clip["guid"].get<std::string>());
            if (guid != ClaymoreGUID()) {
                source.AudioClip = AssetReference(guid, 0, kAudioAssetType);
            }
        }
    }
    if (data.contains("audioPath")) source.AudioPath = data["audioPath"].get<std::string>();
    if (data.contains("volume")) source.Volume = data["volume"];
    if (data.contains("pitch")) source.Pitch = data["pitch"];
    if (data.contains("loop")) source.Loop = data["loop"];
    if (data.contains("playOnAwake")) source.PlayOnAwake = data["playOnAwake"];
    if (data.contains("mute")) source.Mute = data["mute"];
    if (data.contains("spatial")) source.Spatial = data["spatial"];
    if (data.contains("minDistance")) source.MinDistance = data["minDistance"];
    if (data.contains("maxDistance")) source.MaxDistance = data["maxDistance"];
    if (data.contains("dopplerFactor")) source.DopplerFactor = data["dopplerFactor"];
    if (data.contains("rolloff")) source.Rolloff = data["rolloff"];
    source.SoundHandle = INVALID_AUDIO_HANDLE;
    source.IsPlaying = false;
    source.IsPaused = false;
    source.Initialized = false;
    source.LastPosition = glm::vec3(0.0f);
    source.PlayRequested = false;
    source.StopRequested = false;
    source.PauseRequested = false;
    source.ResumeRequested = false;
}

json Serializer::SerializeAudioListener(const AudioListenerComponent& listener) {
    json data;
    data["active"] = listener.Active;
    data["priority"] = listener.Priority;
    data["volumeMultiplier"] = listener.VolumeMultiplier;
    return data;
}

void Serializer::DeserializeAudioListener(const json& data, AudioListenerComponent& listener) {
    if (data.contains("active")) listener.Active = data["active"];
    if (data.contains("priority")) listener.Priority = data["priority"];
    if (data.contains("volumeMultiplier")) listener.VolumeMultiplier = data["volumeMultiplier"];
    listener.LastPosition = glm::vec3(0.0f);
    listener.Velocity = glm::vec3(0.0f);
    listener.WasActive = false;
}

// ---------------- Skeleton & Skinning ----------------
json Serializer::SerializeSkeleton(const SkeletonComponent& skeleton) {
    json j;
    // Matrices
    j["inverseBindPoses"] = json::array();
    for (const auto& m : skeleton.InverseBindPoses) j["inverseBindPoses"].push_back(SerializeMat4(m));
    j["bindPoseGlobals"] = json::array();
    for (const auto& m : skeleton.BindPoseGlobals) j["bindPoseGlobals"].push_back(SerializeMat4(m));

    // Bone parents
    if (!skeleton.BoneParents.empty()) j["boneParents"] = skeleton.BoneParents;

    // Bone names (index -> name)
    if (!skeleton.BoneNameToIndex.empty()) {
        // Emit as array aligned with indices for stability
        size_t count = 0;
        for (const auto& kv : skeleton.BoneNameToIndex) count = std::max(count, (size_t)std::max(0, kv.second+1));
        json names = json::array();
        for (size_t i = 0; i < count; ++i) names.push_back(nullptr);
        for (const auto& kv : skeleton.BoneNameToIndex) {
            int idx = kv.second; if (idx < 0) continue;
            while ((size_t)idx >= names.size()) names.push_back(nullptr);
            names[(size_t)idx] = kv.first;
        }
        j["boneNames"] = std::move(names);
    }
    // Stable GUIDs
    if (skeleton.SkeletonGuid.high != 0 || skeleton.SkeletonGuid.low != 0) j["skeletonGuid"] = skeleton.SkeletonGuid;
    if (!skeleton.JointGuids.empty()) {
        j["jointGuids"] = json::array();
        for (uint64_t g : skeleton.JointGuids) j["jointGuids"].push_back(g);
    }
    return j;
}

void Serializer::DeserializeSkeleton(const json& j, SkeletonComponent& skeleton) {
    skeleton.InverseBindPoses.clear(); skeleton.BindPoseGlobals.clear();
    skeleton.BoneParents.clear(); skeleton.BoneNameToIndex.clear();
    skeleton.BoneNames.clear(); skeleton.JointGuids.clear(); skeleton.SkeletonGuid = ClaymoreGUID{};
    if (!j.is_object()) return; // Guard against non-object JSON

    if (j.contains("inverseBindPoses") && j["inverseBindPoses"].is_array()) {
        for (const auto& m : j["inverseBindPoses"]) skeleton.InverseBindPoses.push_back(DeserializeMat4(m));
    }
    if (j.contains("bindPoseGlobals") && j["bindPoseGlobals"].is_array()) {
        for (const auto& m : j["bindPoseGlobals"]) skeleton.BindPoseGlobals.push_back(DeserializeMat4(m));
    }
    if (j.contains("boneParents") && j["boneParents"].is_array()) {
        skeleton.BoneParents.clear();
        for (const auto& v : j["boneParents"]) skeleton.BoneParents.push_back(v.get<int>());
    }
    if (j.contains("boneNames") && j["boneNames"].is_array()) {
        const auto& arr = j["boneNames"];
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_null()) skeleton.BoneNameToIndex[arr[i].get<std::string>()] = (int)i;
        }
        // Also store aligned bone names for convenience
        skeleton.BoneNames.resize(arr.size());
        for (size_t i = 0; i < arr.size(); ++i) {
            if (!arr[i].is_null()) skeleton.BoneNames[i] = arr[i].get<std::string>();
        }
    }
    // BoneEntities are scene-local; don't persist raw ids. Rebind later using names.
    skeleton.BoneEntities.assign(skeleton.InverseBindPoses.size(), (EntityID)-1);

    // Stable GUIDs
    try { if (j.contains("skeletonGuid")) j.at("skeletonGuid").get_to(skeleton.SkeletonGuid); } catch(...) {}
    if (j.contains("jointGuids") && j["jointGuids"].is_array()) {
        const auto& arr = j["jointGuids"];
        skeleton.JointGuids.resize(arr.size());
        for (size_t i = 0; i < arr.size(); ++i) skeleton.JointGuids[i] = arr[i].get<uint64_t>();
    }
}

json Serializer::SerializeSkinning(const SkinningComponent& skinning) {
    json j;
    // Do not serialize palette/runtime remap state. Persist authoring linkage and
    // original bind data so prefab JSON roundtrips preserve retargeting fidelity.
    j["skeletonRoot"] = skinning.SkeletonRoot; // may be -1; post-load fixup can relink.
    j["useParentSkeleton"] = skinning.UseParentSkeleton;
    if (!(skinning.SkeletonOverrideGuid.high == 0 && skinning.SkeletonOverrideGuid.low == 0)) {
        j["skeletonOverrideGuid"] = skinning.SkeletonOverrideGuid;
    }
    if (!skinning.OriginalBoneNames.empty()) {
        j["originalBoneNames"] = skinning.OriginalBoneNames;
    }
    if (!skinning.OriginalInverseBindPoses.empty()) {
        j["originalInverseBindPoses"] = json::array();
        for (const auto& mat : skinning.OriginalInverseBindPoses) {
            j["originalInverseBindPoses"].push_back(SerializeMat4(mat));
        }
    }
    return j;
}

void Serializer::DeserializeSkinning(const json& j, SkinningComponent& skinning) {
    skinning.BoneCount = 0;
    skinning.ResetGpuSharedSkeletonSource();
    skinning.ClearGpuRetargetData();
    if (!j.is_object()) return; // Guard against non-object JSON
    skinning.SkeletonRoot = j.value("skeletonRoot", (EntityID)-1);
    skinning.UseParentSkeleton = j.value("useParentSkeleton", false);
    if (j.contains("skeletonOverrideGuid")) {
        try { j.at("skeletonOverrideGuid").get_to(skinning.SkeletonOverrideGuid); } catch (...) {}
    } else {
        skinning.SkeletonOverrideGuid = {};
    }

    skinning.OriginalBoneNames.clear();
    if (j.contains("originalBoneNames") && j["originalBoneNames"].is_array()) {
        for (const auto& item : j["originalBoneNames"]) {
            if (item.is_string()) {
                skinning.OriginalBoneNames.push_back(item.get<std::string>());
            }
        }
    }

    skinning.OriginalInverseBindPoses.clear();
    if (j.contains("originalInverseBindPoses") && j["originalInverseBindPoses"].is_array()) {
        for (const auto& item : j["originalInverseBindPoses"]) {
            skinning.OriginalInverseBindPoses.push_back(DeserializeMat4(item));
        }
    }

    skinning.ResolvedSkeletonRoot = INVALID_ENTITY_ID;
    skinning.InvalidateRemap();
}

json Serializer::SerializeBoneAttachment(const BoneAttachmentComponent& ba) {
    json j;
    j["targetBoneName"] = ba.TargetBoneName;
    j["localPosition"] = SerializeVec3(ba.LocalPosition);
    j["localRotation"] = SerializeVec3(ba.LocalRotation);
    j["localScale"] = SerializeVec3(ba.LocalScale);
    j["skeletonEntity"] = ba.SkeletonEntity;
    j["inheritRotation"] = ba.InheritRotation;
    j["inheritScale"] = ba.InheritScale;
    j["enabled"] = ba.Enabled;
    return j;
}

void Serializer::DeserializeBoneAttachment(const json& j, BoneAttachmentComponent& ba) {
    if (!j.is_object()) return;
    ba.TargetBoneName = j.value("targetBoneName", std::string());
    if (j.contains("localPosition")) ba.LocalPosition = DeserializeVec3(j["localPosition"]);
    if (j.contains("localRotation")) ba.LocalRotation = DeserializeVec3(j["localRotation"]);
    if (j.contains("localScale")) ba.LocalScale = DeserializeVec3(j["localScale"]);
    ba.SkeletonEntity = j.value("skeletonEntity", INVALID_ENTITY_ID);
    ba.InheritRotation = j.value("inheritRotation", true);
    ba.InheritScale = j.value("inheritScale", false);
    ba.Enabled = j.value("enabled", true);
    // Invalidate resolution so it happens at runtime
    ba.InvalidateResolution();
}

void Serializer::DeserializeLight(const json& data, LightComponent& light) {
    if (!data.is_object()) return; // Guard against non-object JSON
    if (data.contains("type")) light.Type = static_cast<LightType>(data["type"]);
    if (data.contains("color")) light.Color = DeserializeVec3(data["color"]);
    if (data.contains("intensity")) light.Intensity = data["intensity"];
    light.PointShadowsEnabled = data.value("pointShadowsEnabled", false);
}

json Serializer::SerializeCollider(const ColliderComponent& collider) {
    json data;
    data["shapeType"] = static_cast<int>(collider.ShapeType);
    data["offset"] = SerializeVec3(collider.Offset);
    data["size"] = SerializeVec3(collider.Size);
    data["radius"] = collider.Radius;
    data["height"] = collider.Height;
    data["meshPath"] = collider.MeshPath;
    data["isTrigger"] = collider.IsTrigger;
    data["physicsLayer"] = collider.PhysicsLayerName; // Store by name for portability
    return data;
}

// ---------------- Areas ----------------
json Serializer::SerializeArea(const cm::physics::AreaComponent& a) {
    json j;
    j["enabled"] = a.Enabled;
    j["monitorBodies"] = a.MonitorBodies;
    j["monitorAreas"] = a.MonitorAreas;
    j["shapeType"] = (int)a.ShapeType;
    j["offset"] = Serializer::SerializeVec3(a.Offset);
    j["size"] = Serializer::SerializeVec3(a.Size);
    j["radius"] = a.Radius;
    j["height"] = a.Height;
    j["collisionLayer"] = a.CollisionLayer;
    j["collisionMask"] = a.CollisionMask;
    j["effects"] = (uint8_t)a.Effects;
    j["gravityOverride"] = a.GravityOverride;
    j["linearDamp"] = a.LinearDamp;
    j["angularDamp"] = a.AngularDamp;
    j["priority"] = a.Priority;
    return j;
}

void Serializer::DeserializeArea(const json& j, cm::physics::AreaComponent& a) {
    if (j.contains("enabled")) a.Enabled = j["enabled"];
    if (j.contains("monitorBodies")) a.MonitorBodies = j["monitorBodies"];
    if (j.contains("monitorAreas")) a.MonitorAreas = j["monitorAreas"];
    if (j.contains("shapeType")) a.ShapeType = (cm::physics::AreaShapeType)j["shapeType"].get<int>();
    if (j.contains("offset")) a.Offset = Serializer::DeserializeVec3(j["offset"]);
    if (j.contains("size")) a.Size = Serializer::DeserializeVec3(j["size"]);
    if (j.contains("radius")) a.Radius = j["radius"];
    if (j.contains("height")) a.Height = j["height"];
    if (j.contains("collisionLayer")) a.CollisionLayer = j["collisionLayer"];
    if (j.contains("collisionMask")) a.CollisionMask = j["collisionMask"];
    if (j.contains("effects")) a.Effects = (cm::physics::AreaSpaceEffect)((uint8_t)j["effects"]);
    if (j.contains("gravityOverride")) a.GravityOverride = j["gravityOverride"];
    if (j.contains("linearDamp")) a.LinearDamp = j["linearDamp"];
    if (j.contains("angularDamp")) a.AngularDamp = j["angularDamp"];
    if (j.contains("priority")) a.Priority = j["priority"];
}

void Serializer::DeserializeCollider(const json& data, ColliderComponent& collider) {
    if (data.contains("shapeType")) collider.ShapeType = static_cast<ColliderShape>(data["shapeType"]);
    if (data.contains("offset")) collider.Offset = DeserializeVec3(data["offset"]);
    if (data.contains("size")) collider.Size = DeserializeVec3(data["size"]);
    if (data.contains("radius")) collider.Radius = data["radius"];
    if (data.contains("height")) collider.Height = data["height"];
    if (data.contains("meshPath")) collider.MeshPath = data["meshPath"];
    if (data.contains("isTrigger")) collider.IsTrigger = data["isTrigger"];
    if (data.contains("physicsLayer")) {
        collider.PhysicsLayerName = data["physicsLayer"];
        // Resolve layer index from name
        int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(collider.PhysicsLayerName);
        collider.PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
    }
}

// RigidBody serialization
json Serializer::SerializeRigidBody(const RigidBodyComponent& rigidbody) {
    json data;
    data["mass"] = rigidbody.Mass;
    data["friction"] = rigidbody.Friction;
    data["restitution"] = rigidbody.Restitution;
    data["useGravity"] = rigidbody.UseGravity;
    data["isKinematic"] = rigidbody.IsKinematic;
    data["linearVelocity"] = SerializeVec3(rigidbody.LinearVelocity);
    data["angularVelocity"] = SerializeVec3(rigidbody.AngularVelocity);
    data["physicsLayer"] = rigidbody.PhysicsLayerName;
    data["collisionMask"] = rigidbody.CollisionMask;
    return data;
}

// Character Controller serialization
static nlohmann::json SerializeCharacterController(const CharacterControllerComponent& cc)
{
	nlohmann::json j;
	j["radius"] = cc.Radius;
	j["height"] = cc.Height;
	j["up"] = Serializer::SerializeVec3(cc.Up);
	j["offset"] = Serializer::SerializeVec3(cc.Offset);
	j["maxSlopeDeg"] = cc.MaxSlopeDegrees;
	j["stickToFloor"] = cc.StickToFloor;
	j["walkStairs"] = cc.EnableWalkStairs;
	j["jumpSpeed"] = cc.JumpSpeed;
	j["physicsLayer"] = cc.PhysicsLayerName;
	return j;
}

void Serializer::DeserializeCharacterController(const nlohmann::json& j, CharacterControllerComponent& cc)
{
	if (j.contains("radius")) cc.Radius = j["radius"];
	if (j.contains("height")) cc.Height = j["height"];
	if (j.contains("up")) cc.Up = Serializer::DeserializeVec3(j["up"]);
	if (j.contains("offset")) cc.Offset = Serializer::DeserializeVec3(j["offset"]);
	if (j.contains("maxSlopeDeg")) cc.MaxSlopeDegrees = j["maxSlopeDeg"];
	if (j.contains("stickToFloor")) cc.StickToFloor = j["stickToFloor"];
	if (j.contains("walkStairs")) cc.EnableWalkStairs = j["walkStairs"];
	if (j.contains("jumpSpeed")) cc.JumpSpeed = j["jumpSpeed"];
	if (j.contains("physicsLayer")) {
		cc.PhysicsLayerName = j["physicsLayer"];
		int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(cc.PhysicsLayerName);
		cc.PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 1; // Default to Player (1)
	}
}

// Grass Deformer Component serialization
static nlohmann::json SerializeGrassDeformer(const GrassDeformerComponent& deformer)
{
    nlohmann::json j;
    j["enabled"] = deformer.Enabled;
    j["radius"] = deformer.Radius;
    j["strength"] = deformer.Strength;
    j["heightOffset"] = deformer.HeightOffset;
    j["useVelocity"] = deformer.UseVelocity;
    return j;
}

void Serializer::DeserializeGrassDeformer(const nlohmann::json& j, GrassDeformerComponent& deformer)
{
    if (j.contains("enabled")) deformer.Enabled = j["enabled"];
    if (j.contains("radius")) deformer.Radius = j["radius"];
    if (j.contains("strength")) deformer.Strength = j["strength"];
    if (j.contains("heightOffset")) deformer.HeightOffset = j["heightOffset"];
    if (j.contains("useVelocity")) deformer.UseVelocity = j["useVelocity"];
}

void Serializer::DeserializeRigidBody(const json& data, RigidBodyComponent& rigidbody) {
    if (data.contains("mass")) rigidbody.Mass = data["mass"];
    if (data.contains("friction")) rigidbody.Friction = data["friction"];
    if (data.contains("restitution")) rigidbody.Restitution = data["restitution"];
    if (data.contains("useGravity")) rigidbody.UseGravity = data["useGravity"];
    if (data.contains("isKinematic")) rigidbody.IsKinematic = data["isKinematic"];
    if (data.contains("linearVelocity")) rigidbody.LinearVelocity = DeserializeVec3(data["linearVelocity"]);
    if (data.contains("angularVelocity")) rigidbody.AngularVelocity = DeserializeVec3(data["angularVelocity"]);
    if (data.contains("physicsLayer")) {
        rigidbody.PhysicsLayerName = data["physicsLayer"];
        int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(rigidbody.PhysicsLayerName);
        rigidbody.PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
    }
    if (data.contains("collisionMask")) rigidbody.CollisionMask = data["collisionMask"];
}

// StaticBody serialization
json Serializer::SerializeStaticBody(const StaticBodyComponent& staticbody) {
    json data;
    data["friction"] = staticbody.Friction;
    data["restitution"] = staticbody.Restitution;
    data["physicsLayer"] = staticbody.PhysicsLayerName;
    return data;
}

void Serializer::DeserializeStaticBody(const json& data, StaticBodyComponent& staticbody) {
    if (data.contains("friction")) staticbody.Friction = data["friction"];
    if (data.contains("restitution")) staticbody.Restitution = data["restitution"];
    if (data.contains("physicsLayer")) {
        staticbody.PhysicsLayerName = data["physicsLayer"];
        int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(staticbody.PhysicsLayerName);
        staticbody.PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
    }
}

json Serializer::SerializeSoftbody(const SoftbodyComponent& softbody) {
    json data;
    data["enabled"] = softbody.Enabled;
    data["solverIterations"] = softbody.SolverIterations;
    data["linearDamping"] = softbody.LinearDamping;
    data["friction"] = softbody.Friction;
    data["restitution"] = softbody.Restitution;
    data["pressure"] = softbody.Pressure;
    data["gravityFactor"] = softbody.GravityFactor;
    data["vertexRadius"] = softbody.VertexRadius;
    data["maxLinearVelocity"] = softbody.MaxLinearVelocity;
    data["edgeCompliance"] = softbody.EdgeCompliance;
    data["shearCompliance"] = softbody.ShearCompliance;
    data["bendCompliance"] = softbody.BendCompliance;
    data["enableLongRangeAttachments"] = softbody.EnableLongRangeAttachments;
    data["lraMaxDistanceMultiplier"] = softbody.LRAMaxDistanceMultiplier;
    data["bendMode"] = static_cast<uint32_t>(softbody.BendMode);
    data["facesDoubleSided"] = softbody.FacesDoubleSided;
    data["weightFloor"] = softbody.WeightFloor;
    data["physicsLayer"] = softbody.PhysicsLayerName;
    data["sourceVertexCount"] = softbody.SourceVertexCount;
    data["sourceIndexCount"] = softbody.SourceIndexCount;
    data["vertexWeights"] = softbody.VertexWeights;
    data["anchorVertices"] = softbody.AnchorVertices;
    return data;
}

void Serializer::DeserializeSoftbody(const json& data, SoftbodyComponent& softbody) {
    if (data.contains("enabled")) softbody.Enabled = data["enabled"];
    if (data.contains("solverIterations")) softbody.SolverIterations = data["solverIterations"];
    if (data.contains("linearDamping")) softbody.LinearDamping = data["linearDamping"];
    if (data.contains("friction")) softbody.Friction = data["friction"];
    if (data.contains("restitution")) softbody.Restitution = data["restitution"];
    if (data.contains("pressure")) softbody.Pressure = data["pressure"];
    if (data.contains("gravityFactor")) softbody.GravityFactor = data["gravityFactor"];
    if (data.contains("vertexRadius")) softbody.VertexRadius = data["vertexRadius"];
    if (data.contains("maxLinearVelocity")) softbody.MaxLinearVelocity = data["maxLinearVelocity"];
    if (data.contains("edgeCompliance")) softbody.EdgeCompliance = data["edgeCompliance"];
    if (data.contains("shearCompliance")) softbody.ShearCompliance = data["shearCompliance"];
    if (data.contains("bendCompliance")) softbody.BendCompliance = data["bendCompliance"];
    if (data.contains("enableLongRangeAttachments")) softbody.EnableLongRangeAttachments = data["enableLongRangeAttachments"];
    if (data.contains("lraMaxDistanceMultiplier")) softbody.LRAMaxDistanceMultiplier = data["lraMaxDistanceMultiplier"];
    if (data.contains("bendMode")) {
        softbody.BendMode = static_cast<SoftbodyBendMode>(data["bendMode"].get<uint32_t>());
    }
    if (data.contains("facesDoubleSided")) softbody.FacesDoubleSided = data["facesDoubleSided"];
    if (data.contains("weightFloor")) softbody.WeightFloor = data["weightFloor"];
    if (data.contains("physicsLayer")) {
        softbody.PhysicsLayerName = data["physicsLayer"];
        int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(softbody.PhysicsLayerName);
        softbody.PhysicsLayer = (idx >= 0) ? static_cast<uint32_t>(idx) : 0;
    }
    if (data.contains("sourceVertexCount")) softbody.SourceVertexCount = data["sourceVertexCount"];
    if (data.contains("sourceIndexCount")) softbody.SourceIndexCount = data["sourceIndexCount"];
    if (data.contains("vertexWeights") && data["vertexWeights"].is_array()) {
        softbody.VertexWeights = data["vertexWeights"].get<std::vector<float>>();
        for (float& weight : softbody.VertexWeights) {
            weight = glm::clamp(weight, 0.0f, 1.0f);
        }
    }
    if (data.contains("anchorVertices") && data["anchorVertices"].is_array()) {
        softbody.AnchorVertices = data["anchorVertices"].get<std::vector<uint8_t>>();
        for (uint8_t& anchor : softbody.AnchorVertices) {
            anchor = anchor != 0 ? 1 : 0;
        }
    }
}

// Camera serialization
json Serializer::SerializeCamera(const CameraComponent& camera) {
    json data;
    data["active"] = camera.Active;
    data["priority"] = camera.priority;
    data["fov"] = camera.FieldOfView;
    data["nearClip"] = camera.NearClip;
    data["farClip"] = camera.FarClip;
    data["isPerspective"] = camera.IsPerspective;
    data["layerMask"] = camera.LayerMask;
    return data;
}

void Serializer::DeserializeCamera(const json& data, CameraComponent& camera) {
    if (data.contains("active")) camera.Active = data["active"];
    if (data.contains("priority")) camera.priority = data["priority"];
    if (data.contains("fov")) camera.FieldOfView = data["fov"];
    if (data.contains("nearClip")) camera.NearClip = data["nearClip"];
    if (data.contains("farClip")) camera.FarClip = data["farClip"];
    if (data.contains("isPerspective")) camera.IsPerspective = data["isPerspective"];
    if (data.contains("layerMask")) camera.LayerMask = data["layerMask"];
}

// Terrain serialization (only essentials to reconstruct deterministically)
json Serializer::SerializeTerrain(const TerrainComponent& terrain) {
    json data;
    data["gridResolution"] = terrain.GridResolution;
    data["worldSize"] = { terrain.WorldSize.x, terrain.WorldSize.y };
    data["maxHeight"] = terrain.MaxHeight;
    data["chunkResolution"] = terrain.ChunkResolution;

    json brush;
    brush["mode"] = static_cast<int>(terrain.Brush.Mode);
    brush["radius"] = terrain.Brush.Radius;
    brush["strength"] = terrain.Brush.Strength;
    brush["textureStrength"] = terrain.Brush.TextureStrength;
    brush["grassStrength"] = terrain.Brush.GrassStrength;
    brush["instancerStrength"] = terrain.Brush.InstancerStrength;
    brush["falloff"] = terrain.Brush.Falloff;
    brush["alignToNormal"] = terrain.Brush.AlignToNormal;
    brush["activeLayer"] = terrain.Brush.ActiveLayer;
    brush["activeGrassLayer"] = terrain.Brush.ActiveGrassLayer;
    brush["activeInstancerLayer"] = terrain.Brush.ActiveInstancerLayer;
    brush["erosionNoiseScale"] = terrain.Brush.ErosionNoiseScale;
    brush["erosionNoiseOctaves"] = terrain.Brush.ErosionNoiseOctaves;
    brush["erosionNoisePersistence"] = terrain.Brush.ErosionNoisePersistence;
    brush["erosionNoiseStrength"] = terrain.Brush.ErosionNoiseStrength;
    brush["flattenTargetHeight"] = terrain.Brush.FlattenTargetHeight;
    brush["heightmapStampTexturePath"] = terrain.Brush.HeightmapStampTexturePath;
    brush["heightmapStampAdditive"] = terrain.Brush.HeightmapStampAdditive;
    brush["heightmapStampSubtractive"] = terrain.Brush.HeightmapStampSubtractive;
    brush["heightmapStampMinY"] = terrain.Brush.HeightmapStampMinY;
    brush["heightmapStampBaselineY"] = terrain.Brush.HeightmapStampBaselineY;
    brush["heightmapStampMaxY"] = terrain.Brush.HeightmapStampMaxY;
    brush["cliffHeight"] = terrain.Brush.CliffHeight;
    brush["cliffRoughness"] = terrain.Brush.CliffRoughness;
    brush["cliffLayering"] = terrain.Brush.CliffLayering;
    brush["mountainHeight"] = terrain.Brush.MountainHeight;
    brush["mountainRidgeScale"] = terrain.Brush.MountainRidgeScale;
    brush["mountainRockiness"] = terrain.Brush.MountainRockiness;
    brush["mountainSteepness"] = terrain.Brush.MountainSteepness;
    data["brush"] = std::move(brush);

    json layers = json::array();
    for (const auto& layer : terrain.Layers) {
        json jLayer;
        if (!layer.Name.empty()) jLayer["name"] = layer.Name;
        if (!layer.AlbedoPath.empty()) jLayer["albedoPath"] = layer.AlbedoPath;
        if (!layer.NormalPath.empty()) jLayer["normalPath"] = layer.NormalPath;
        jLayer["tiling"] = layer.Tiling;
        jLayer["navCost"] = layer.NavCost;
        jLayer["color"] = { layer.PlaceholderColor.r, layer.PlaceholderColor.g, layer.PlaceholderColor.b };
        layers.push_back(std::move(jLayer));
    }
    data["layers"] = std::move(layers);

   json grassLayers = json::array();
   for (const auto& layer : terrain.GrassLayers) {
       json gl;
       gl["guid"] = layer.Guid.ToString();
       if (!layer.Name.empty()) gl["name"] = layer.Name;
       gl["enabled"] = layer.Enabled;
       gl["useGPU"] = layer.UseGPU;
       gl["renderMode"] = static_cast<int>(layer.RenderMode);
       std::cout << "[Terrain::Serialize] Grass layer '" << layer.Name << "' RenderMode = " << static_cast<int>(layer.RenderMode) << std::endl;
       gl["mask"] = static_cast<int>(layer.Mask);
       gl["splatSeed"] = layer.SplatSeed;
       gl["splatNoiseScale"] = layer.SplatNoiseScale;
       gl["splatNoiseStrength"] = layer.SplatNoiseStrength;
       gl["splatThreshold"] = layer.SplatThreshold;
       gl["density"] = layer.DensityPerSquareMeter;
       gl["scale"] = { layer.ScaleRange.x, layer.ScaleRange.y };
       gl["yaw"] = layer.RandomYawDegrees;
       gl["heightRange"] = { layer.HeightRange.x, layer.HeightRange.y };
       gl["maxSlope"] = layer.MaxSlopeDegrees;
       gl["minDistance"] = layer.MinDistance;
       gl["maxDistance"] = layer.MaxDistance;
       gl["wind"] = layer.WindStrength;
       gl["windDirection"] = layer.WindDirectionDegrees;
       gl["baseColor"] = { layer.BaseColor.r, layer.BaseColor.g, layer.BaseColor.b };
       gl["colorVariance"] = { layer.ColorVariance.r, layer.ColorVariance.g, layer.ColorVariance.b };
       if (!layer.BillboardTexturePath.empty()) gl["texture"] = layer.BillboardTexturePath;
       if (layer.MeshAsset.IsValid()) gl["meshGuid"] = layer.MeshAsset.guid.ToString();
       if (!layer.MeshPath.empty()) gl["meshPath"] = layer.MeshPath;
       grassLayers.push_back(std::move(gl));
   }
   data["grassLayers"] = std::move(grassLayers);
   data["grassChunkResolution"] = terrain.GrassChunkResolution;
   data["grassSamplingMultiplier"] = terrain.GrassSamplingMultiplier;

   json instancerLayers = json::array();
   for (const auto& layer : terrain.InstancerLayers) {
       json il;
       il["guid"] = layer.Guid.ToString();
       if (!layer.Name.empty()) il["name"] = layer.Name;
       il["enabled"] = layer.Enabled;
       il["mask"] = static_cast<int>(layer.Mask);
       il["splatThreshold"] = layer.SplatThreshold;

       json collision;
       collision["enabled"] = layer.Collision.Enabled;
       collision["activationDistance"] = layer.Collision.ActivationDistance;
       collision["maxActiveBodies"] = layer.Collision.MaxActiveBodies;
       collision["physicsLayer"] = layer.Collision.PhysicsLayer;
       collision["physicsLayerName"] = layer.Collision.PhysicsLayerName;
       collision["useSharedMeshShape"] = layer.Collision.UseSharedMeshShape;
       il["collision"] = std::move(collision);
       il["instancer"] = SerializeInstancer(layer.Instancer);
       instancerLayers.push_back(std::move(il));
   }
   data["instancerLayers"] = std::move(instancerLayers);

    json asset;
    asset["path"] = terrain.AssetPath;
    asset["guid"] = terrain.TerrainDataGuid.ToString();
    asset["version"] = 1;
    data["asset"] = std::move(asset);
    
    // Clipmap settings (geometry clipmaps for efficient LOD rendering)
    json clipmap;
    clipmap["enabled"] = terrain.UseClipmaps;
    clipmap["levels"] = terrain.ClipmapLevels;
    clipmap["gridSize"] = terrain.ClipmapGridSize;
    clipmap["morphing"] = terrain.ClipmapMorphing;
    data["clipmap"] = std::move(clipmap);
    
    // Chunked terrain settings (Skyrim-style cells with unified textures)
    json chunked;
    chunked["enabled"] = terrain.UseChunkedTerrain;
    chunked["chunkVertexSize"] = terrain.ChunkVertexSize;
    chunked["morphing"] = terrain.ChunkMorphing;
    chunked["morphRegion"] = terrain.ChunkMorphRegion;
    chunked["streaming"] = terrain.ChunkStreaming;
    chunked["streamingLoadRadius"] = terrain.StreamingLoadRadius;
    chunked["streamingUnloadRadius"] = terrain.StreamingUnloadRadius;
    data["chunkedTerrain"] = std::move(chunked);
    
    // Layer texture array settings
    json texArraySettings;
    texArraySettings["resolution"] = terrain.LayerTextureResolution;
    texArraySettings["filter"] = static_cast<int>(terrain.LayerResizeFilter);
    data["textureArraySettings"] = std::move(texArraySettings);
    
    return data;
}

void Serializer::DeserializeTerrain(const json& data, TerrainComponent& terrain) {
    terrain.DestroyGpuResources();

    if (data.contains("gridResolution")) terrain.GridResolution = std::max(2u, data["gridResolution"].get<uint32_t>());
    else if (data.contains("size")) terrain.GridResolution = std::max(2u, data["size"].get<uint32_t>());

    if (data.contains("worldSize") && data["worldSize"].is_array() && data["worldSize"].size() >= 2) {
        terrain.WorldSize.x = data["worldSize"][0];
        terrain.WorldSize.y = data["worldSize"][1];
    } else if (data.contains("size")) {
        float legacySize = data["size"];
        terrain.WorldSize = glm::vec2(legacySize);
    }

    if (data.contains("maxHeight")) terrain.MaxHeight = std::max(0.1f, data["maxHeight"].get<float>());
    if (data.contains("chunkResolution")) terrain.ChunkResolution = std::max(2u, data["chunkResolution"].get<uint32_t>());

    if (data.contains("brush") && data["brush"].is_object()) {
        const json& brush = data["brush"];
        if (brush.contains("mode")) {
            int rawMode = brush["mode"].get<int>();
            int maxMode = static_cast<int>(TerrainBrushMode::Instancer);
            rawMode = std::clamp(rawMode, 0, maxMode);
            terrain.Brush.Mode = static_cast<TerrainBrushMode>(rawMode);
        }
        if (brush.contains("radius")) terrain.Brush.Radius = brush["radius"];
        if (brush.contains("strength")) terrain.Brush.Strength = brush["strength"];
        if (brush.contains("textureStrength")) terrain.Brush.TextureStrength = brush["textureStrength"];
        if (brush.contains("grassStrength")) terrain.Brush.GrassStrength = brush["grassStrength"];
        if (brush.contains("instancerStrength")) terrain.Brush.InstancerStrength = brush["instancerStrength"];
        if (brush.contains("falloff")) terrain.Brush.Falloff = std::max(0.01f, brush["falloff"].get<float>());
        if (brush.contains("alignToNormal")) terrain.Brush.AlignToNormal = brush["alignToNormal"];
        if (brush.contains("activeLayer")) terrain.Brush.ActiveLayer = brush["activeLayer"];
        if (brush.contains("activeGrassLayer")) terrain.Brush.ActiveGrassLayer = brush["activeGrassLayer"];
        if (brush.contains("activeInstancerLayer")) terrain.Brush.ActiveInstancerLayer = brush["activeInstancerLayer"];
        if (brush.contains("erosionNoiseScale")) terrain.Brush.ErosionNoiseScale = brush["erosionNoiseScale"];
        if (brush.contains("erosionNoiseOctaves")) terrain.Brush.ErosionNoiseOctaves = brush["erosionNoiseOctaves"];
        if (brush.contains("erosionNoisePersistence")) terrain.Brush.ErosionNoisePersistence = brush["erosionNoisePersistence"];
        if (brush.contains("erosionNoiseStrength")) terrain.Brush.ErosionNoiseStrength = brush["erosionNoiseStrength"];
        if (brush.contains("flattenTargetHeight")) terrain.Brush.FlattenTargetHeight = brush["flattenTargetHeight"];
        if (brush.contains("heightmapStampTexturePath")) terrain.Brush.HeightmapStampTexturePath = brush["heightmapStampTexturePath"].get<std::string>();
        if (brush.contains("heightmapStampAdditive")) terrain.Brush.HeightmapStampAdditive = brush["heightmapStampAdditive"];
        if (brush.contains("heightmapStampSubtractive")) terrain.Brush.HeightmapStampSubtractive = brush["heightmapStampSubtractive"];
        if (brush.contains("heightmapStampMinY")) terrain.Brush.HeightmapStampMinY = brush["heightmapStampMinY"];
        if (brush.contains("heightmapStampBaselineY")) terrain.Brush.HeightmapStampBaselineY = brush["heightmapStampBaselineY"];
        if (brush.contains("heightmapStampMaxY")) terrain.Brush.HeightmapStampMaxY = brush["heightmapStampMaxY"];
        if (brush.contains("cliffHeight")) terrain.Brush.CliffHeight = brush["cliffHeight"];
        if (brush.contains("cliffRoughness")) terrain.Brush.CliffRoughness = brush["cliffRoughness"];
        if (brush.contains("cliffLayering")) terrain.Brush.CliffLayering = brush["cliffLayering"];
        if (brush.contains("mountainHeight")) terrain.Brush.MountainHeight = brush["mountainHeight"];
        if (brush.contains("mountainRidgeScale")) terrain.Brush.MountainRidgeScale = brush["mountainRidgeScale"];
        if (brush.contains("mountainRockiness")) terrain.Brush.MountainRockiness = brush["mountainRockiness"];
        if (brush.contains("mountainSteepness")) terrain.Brush.MountainSteepness = brush["mountainSteepness"];
    }

    terrain.Brush.HeightmapStampTexture = BGFX_INVALID_HANDLE;
    terrain.Brush.HeightmapStampSamples.clear();
    terrain.Brush.HeightmapStampWidth = 0;
    terrain.Brush.HeightmapStampHeight = 0;
    terrain.Brush.HeightmapStampMinY = std::clamp(terrain.Brush.HeightmapStampMinY, 0.0f, terrain.MaxHeight);
    terrain.Brush.HeightmapStampMaxY = std::clamp(terrain.Brush.HeightmapStampMaxY, terrain.Brush.HeightmapStampMinY, terrain.MaxHeight);
    terrain.Brush.HeightmapStampBaselineY = std::clamp(
        terrain.Brush.HeightmapStampBaselineY,
        terrain.Brush.HeightmapStampMinY,
        terrain.Brush.HeightmapStampMaxY);
    if (terrain.Brush.HeightmapStampAdditive && terrain.Brush.HeightmapStampSubtractive) {
        terrain.Brush.HeightmapStampSubtractive = false;
    }
    if (!terrain.Brush.HeightmapStampTexturePath.empty()) {
        Terrain::LoadHeightmapTextureSamples(
            terrain.Brush.HeightmapStampTexturePath,
            terrain.Brush.HeightmapStampSamples,
            terrain.Brush.HeightmapStampWidth,
            terrain.Brush.HeightmapStampHeight);
        TextureSpecifier spec;
        spec.Path = terrain.Brush.HeightmapStampTexturePath;
        terrain.Brush.HeightmapStampTexture = AcquireTextureHandle(spec, TextureColorSpace::Linear);
    }

    terrain.EnsureMapSize();

    if (data.contains("asset") && data["asset"].is_object()) {
        const json& asset = data["asset"];
        if (asset.contains("guid")) {
            try { terrain.TerrainDataGuid = ClaymoreGUID::FromString(asset["guid"].get<std::string>()); } catch(...) {}
        }
        if (asset.contains("path")) {
            try { terrain.AssetPath = IVirtualFS::NormalizePath(asset["path"].get<std::string>()); }
            catch(...) { terrain.AssetPath = asset["path"].get<std::string>(); }
        }
    }
    if (terrain.AssetPath.empty()) {
        terrain.AssetPath = TerrainComponent::BuildDefaultAssetPath(terrain.TerrainDataGuid);
    }

    // Parse terrain texture layers from JSON
    if (data.contains("layers") && data["layers"].is_array()) {
        terrain.Layers.clear();
        for (const auto& entry : data["layers"]) {
            TerrainLayerDesc layer;
            if (entry.contains("name")) layer.Name = entry["name"].get<std::string>();
            if (entry.contains("albedoPath")) layer.AlbedoPath = entry["albedoPath"].get<std::string>();
            if (entry.contains("normalPath")) layer.NormalPath = entry["normalPath"].get<std::string>();
            if (entry.contains("tiling")) layer.Tiling = entry["tiling"];
            if (entry.contains("navCost")) layer.NavCost = entry["navCost"];
            if (entry.contains("color") && entry["color"].is_array() && entry["color"].size() >= 3) {
                layer.PlaceholderColor.r = entry["color"][0];
                layer.PlaceholderColor.g = entry["color"][1];
                layer.PlaceholderColor.b = entry["color"][2];
            }
            terrain.Layers.push_back(std::move(layer));
        }
    }
    if (terrain.Layers.empty()) {
        // Preserve at least one layer for texture painting
        TerrainLayerDesc layer;
        layer.Name = "Layer 0";
        terrain.Layers.push_back(std::move(layer));
    }

    // Parse grass layers from JSON BEFORE loading terrain asset so GUIDs are available for mask matching
    terrain.GrassLayers.clear();
    if (data.contains("grassLayers") && data["grassLayers"].is_array()) {
        for (const auto& entry : data["grassLayers"]) {
            TerrainGrassLayerDesc layer;
            if (entry.contains("guid")) {
                try { layer.Guid = ClaymoreGUID::FromString(entry["guid"].get<std::string>()); } catch(...) {}
            }
            layer.SplatSeed = static_cast<uint32_t>(layer.Guid.low ^ layer.Guid.high);
            if (entry.contains("name")) layer.Name = entry["name"].get<std::string>();
            if (entry.contains("enabled")) layer.Enabled = entry["enabled"].get<bool>();
            if (entry.contains("useGPU")) layer.UseGPU = entry["useGPU"].get<bool>();
            // Support both old "mode" and new "renderMode" keys for backwards compatibility
            if (entry.contains("renderMode")) {
                layer.RenderMode = static_cast<GrassRenderMode>(std::clamp(entry["renderMode"].get<int>(), 0, 2));
            } else if (entry.contains("mode")) {
                layer.RenderMode = static_cast<GrassRenderMode>(std::clamp(entry["mode"].get<int>(), 0, 2));
            }
            std::cout << "[Terrain::Deserialize] Grass layer '" << layer.Name << "' RenderMode = " << static_cast<int>(layer.RenderMode) << std::endl;
            if (entry.contains("mask")) layer.Mask = static_cast<GrassMaskSource>(std::clamp(entry["mask"].get<int>(), 0, 5));
            if (entry.contains("splatSeed")) layer.SplatSeed = entry["splatSeed"].get<uint32_t>();
            if (entry.contains("splatNoiseScale")) layer.SplatNoiseScale = entry["splatNoiseScale"].get<float>();
            if (entry.contains("splatNoiseStrength")) layer.SplatNoiseStrength = entry["splatNoiseStrength"].get<float>();
            if (entry.contains("splatThreshold")) layer.SplatThreshold = entry["splatThreshold"].get<float>();
            if (entry.contains("density")) layer.DensityPerSquareMeter = entry["density"];
            if (entry.contains("scale") && entry["scale"].is_array() && entry["scale"].size() >= 2) {
                layer.ScaleRange.x = entry["scale"][0];
                layer.ScaleRange.y = entry["scale"][1];
            }
            if (entry.contains("yaw")) layer.RandomYawDegrees = entry["yaw"];
            if (entry.contains("heightRange") && entry["heightRange"].is_array() && entry["heightRange"].size() >= 2) {
                layer.HeightRange.x = entry["heightRange"][0];
                layer.HeightRange.y = entry["heightRange"][1];
            }
            if (entry.contains("maxSlope")) layer.MaxSlopeDegrees = entry["maxSlope"];
            if (entry.contains("minDistance")) layer.MinDistance = entry["minDistance"];
            if (entry.contains("maxDistance")) layer.MaxDistance = entry["maxDistance"];
            if (entry.contains("wind")) layer.WindStrength = entry["wind"];
            if (entry.contains("windDirection")) layer.WindDirectionDegrees = entry["windDirection"];
            if (entry.contains("baseColor") && entry["baseColor"].is_array() && entry["baseColor"].size() >= 3) {
                layer.BaseColor.r = entry["baseColor"][0];
                layer.BaseColor.g = entry["baseColor"][1];
                layer.BaseColor.b = entry["baseColor"][2];
            }
            if (entry.contains("colorVariance") && entry["colorVariance"].is_array() && entry["colorVariance"].size() >= 3) {
                layer.ColorVariance.r = entry["colorVariance"][0];
                layer.ColorVariance.g = entry["colorVariance"][1];
                layer.ColorVariance.b = entry["colorVariance"][2];
            }
            if (entry.contains("texture")) layer.BillboardTexturePath = entry["texture"].get<std::string>();
            if (entry.contains("meshGuid")) {
                try { layer.MeshAsset.guid = ClaymoreGUID::FromString(entry["meshGuid"].get<std::string>()); }
                catch(...) {}
                layer.MeshAsset.type = static_cast<int>(AssetType::Mesh);
            }
            if (entry.contains("meshPath")) layer.MeshPath = entry["meshPath"].get<std::string>();
            layer.EnsureMaskSize(terrain.GridResolution);
            terrain.GrassLayers.push_back(std::move(layer));
        }
    }
    if (terrain.GrassLayers.empty()) {
        terrain.GrassLayers.emplace_back();
        terrain.GrassLayers.back().EnsureMaskSize(terrain.GridResolution);
    }

    terrain.InstancerLayers.clear();
    if (data.contains("instancerLayers") && data["instancerLayers"].is_array()) {
        for (const auto& entry : data["instancerLayers"]) {
            TerrainInstancerLayerDesc layer;
            if (entry.contains("guid")) {
                try { layer.Guid = ClaymoreGUID::FromString(entry["guid"].get<std::string>()); } catch(...) {}
            }
            if (entry.contains("name")) layer.Name = entry["name"].get<std::string>();
            if (entry.contains("enabled")) layer.Enabled = entry["enabled"].get<bool>();
            if (entry.contains("mask")) layer.Mask = static_cast<TerrainInstancerMaskSource>(std::clamp(entry["mask"].get<int>(), 0, 4));
            if (entry.contains("splatThreshold")) layer.SplatThreshold = entry["splatThreshold"].get<float>();
            if (entry.contains("collision") && entry["collision"].is_object()) {
                const json& collision = entry["collision"];
                if (collision.contains("enabled")) layer.Collision.Enabled = collision["enabled"].get<bool>();
                if (collision.contains("activationDistance")) layer.Collision.ActivationDistance = collision["activationDistance"].get<float>();
                if (collision.contains("maxActiveBodies")) layer.Collision.MaxActiveBodies = collision["maxActiveBodies"].get<uint32_t>();
                if (collision.contains("physicsLayer")) layer.Collision.PhysicsLayer = collision["physicsLayer"].get<uint32_t>();
                if (collision.contains("physicsLayerName")) layer.Collision.PhysicsLayerName = collision["physicsLayerName"].get<std::string>();
                if (collision.contains("useSharedMeshShape")) layer.Collision.UseSharedMeshShape = collision["useSharedMeshShape"].get<bool>();
                int32_t idx = PhysicsLayers::PhysicsLayerManager::Get().GetLayerIndex(layer.Collision.PhysicsLayerName);
                if (idx >= 0) layer.Collision.PhysicsLayer = static_cast<uint32_t>(idx);
            }
            if (entry.contains("instancer") && entry["instancer"].is_object()) {
                DeserializeInstancer(entry["instancer"], layer.Instancer);
            }
            layer.Instancer.SurfaceEntity = INVALID_ENTITY_ID;
            layer.EnsureMaskSize(terrain.GridResolution);
            layer.MarkRuntimeDirty();
            terrain.InstancerLayers.push_back(std::move(layer));
        }
    }
    if (terrain.InstancerLayers.empty()) {
        terrain.InstancerLayers.emplace_back();
        terrain.InstancerLayers.back().EnsureMaskSize(terrain.GridResolution);
    }

    if (data.contains("grassChunkResolution")) {
        terrain.GrassChunkResolution = std::max(8u, data["grassChunkResolution"].get<uint32_t>());
    }
    if (data.contains("grassSamplingMultiplier")) {
        terrain.GrassSamplingMultiplier = std::clamp(data["grassSamplingMultiplier"].get<uint32_t>(), 1u, 16u);
    }

    // Now load terrain asset (height map, splat map, grass masks) - grass layers must be populated first
    // so that LoadTerrainAsset can match grass masks by GUID
    bool loadedFromAsset = false;
    if (!terrain.AssetPath.empty()) {
        loadedFromAsset = Terrain::LoadTerrainAsset(terrain.AssetPath, terrain);
    }

    if (!loadedFromAsset) {
        if (data.contains("heightMap") && data["heightMap"].is_array()) {
            const auto& arr = data["heightMap"];
            size_t count = std::min(arr.size(), terrain.HeightMap.size());
            for (size_t i = 0; i < count; ++i) {
                terrain.HeightMap[i] = static_cast<uint16_t>(arr[i].get<uint32_t>());
            }
        }

        if (data.contains("splatMap") && data["splatMap"].is_array()) {
            const auto& arr = data["splatMap"];
            size_t count = std::min(arr.size(), terrain.SplatMap.size());
            for (size_t i = 0; i < count; ++i) {
                if (!arr[i].is_array() || arr[i].size() < 4) continue;
                glm::u8vec4 px(0);
                px.r = static_cast<uint8_t>(arr[i][0].get<int>());
                px.g = static_cast<uint8_t>(arr[i][1].get<int>());
                px.b = static_cast<uint8_t>(arr[i][2].get<int>());
                px.a = static_cast<uint8_t>(arr[i][3].get<int>());
                terrain.SplatMap[i] = px;
            }
        }
        terrain.AssetDirty = true;
    }

    if (!terrain.Layers.empty()) {
        int maxLayer = static_cast<int>(terrain.Layers.size()) - 1;
        terrain.Brush.ActiveLayer = std::clamp(terrain.Brush.ActiveLayer, 0, maxLayer);
    } else {
        terrain.Brush.ActiveLayer = 0;
    }
    if (!terrain.GrassLayers.empty()) {
        int maxGrass = static_cast<int>(terrain.GrassLayers.size()) - 1;
        terrain.Brush.ActiveGrassLayer = std::clamp(terrain.Brush.ActiveGrassLayer, 0, std::max(0, maxGrass));
    } else {
        terrain.Brush.ActiveGrassLayer = 0;
    }
    if (!terrain.InstancerLayers.empty()) {
        int maxInstancer = static_cast<int>(terrain.InstancerLayers.size()) - 1;
        terrain.Brush.ActiveInstancerLayer = std::clamp(terrain.Brush.ActiveInstancerLayer, 0, std::max(0, maxInstancer));
    } else {
        terrain.Brush.ActiveInstancerLayer = 0;
    }

    // Clipmap settings
    if (data.contains("clipmap") && data["clipmap"].is_object()) {
        const json& clipmap = data["clipmap"];
        if (clipmap.contains("enabled")) terrain.UseClipmaps = clipmap["enabled"].get<bool>();
        if (clipmap.contains("levels")) terrain.ClipmapLevels = std::clamp(clipmap["levels"].get<uint32_t>(), 1u, 8u);
        if (clipmap.contains("gridSize")) {
            uint32_t gridSize = clipmap["gridSize"].get<uint32_t>();
            // Ensure power of 2 and reasonable range
            gridSize = std::clamp(gridSize, 16u, 256u);
            // Round to nearest power of 2
            uint32_t pot = 1;
            while (pot < gridSize) pot *= 2;
            terrain.ClipmapGridSize = pot;
        }
        if (clipmap.contains("morphing")) terrain.ClipmapMorphing = clipmap["morphing"].get<bool>();
    }
    
    // Chunked terrain settings (Skyrim-style cells)
    if (data.contains("chunkedTerrain") && data["chunkedTerrain"].is_object()) {
        const json& chunked = data["chunkedTerrain"];
        if (chunked.contains("enabled")) terrain.UseChunkedTerrain = chunked["enabled"].get<bool>();
        if (chunked.contains("chunkVertexSize")) {
            uint32_t size = chunked["chunkVertexSize"].get<uint32_t>();
            // Ensure power of 2 + 1 and reasonable range (17, 33, 65, 129)
            size = std::clamp(size, 9u, 129u);
            uint32_t pot = 1;
            while (pot + 1 < size) pot *= 2;
            terrain.ChunkVertexSize = pot + 1;
        }
        if (chunked.contains("morphing")) terrain.ChunkMorphing = chunked["morphing"].get<bool>();
        if (chunked.contains("morphRegion")) terrain.ChunkMorphRegion = std::clamp(chunked["morphRegion"].get<float>(), 0.1f, 0.5f);
        if (chunked.contains("streaming")) terrain.ChunkStreaming = chunked["streaming"].get<bool>();
        if (chunked.contains("streamingLoadRadius")) terrain.StreamingLoadRadius = std::max(10.0f, chunked["streamingLoadRadius"].get<float>());
        if (chunked.contains("streamingUnloadRadius")) terrain.StreamingUnloadRadius = std::max(terrain.StreamingLoadRadius + 10.0f, chunked["streamingUnloadRadius"].get<float>());
    }
    
    // Layer texture array settings
    if (data.contains("textureArraySettings") && data["textureArraySettings"].is_object()) {
        const json& texSettings = data["textureArraySettings"];
        if (texSettings.contains("resolution")) {
            uint32_t res = texSettings["resolution"].get<uint32_t>();
            // Clamp to valid values (256, 512, 1024, 2048)
            if (res <= 256) terrain.LayerTextureResolution = 256;
            else if (res <= 512) terrain.LayerTextureResolution = 512;
            else if (res <= 1024) terrain.LayerTextureResolution = 1024;
            else terrain.LayerTextureResolution = 2048;
        }
        if (texSettings.contains("filter")) {
            int filterInt = texSettings["filter"].get<int>();
            terrain.LayerResizeFilter = static_cast<TerrainTextureFilter>(std::clamp(filterInt, 0, 2));
        }
    }

    terrain.MeshDirty = true;
    terrain.HeightDataDirty = true;
    terrain.SplatDataDirty = true;
    terrain.GrassStructureDirty = true;
    terrain.GrassMasksDirty = true;
    terrain.InstancerLayersDirty = true;
    for (auto& layer : terrain.InstancerLayers) {
        layer.MarkRuntimeDirty();
    }
    terrain.LayerTextureArraysDirty = true; // Rebuild arrays on load
    terrain.ChunkMeshDirty = true;  // Rebuild chunk mesh buffers
    terrain.Chunks.clear();
}

// ResourceLayers serialization
json Serializer::SerializeResourceLayers(const cm::resourcelayer::ResourceLayerComponent& layers) {
    using namespace cm::resourcelayer;
    json data;
    
    // Global settings
    data["globalSeed"] = layers.GlobalSeed;
    data["globalDensityMultiplier"] = layers.GlobalDensityMultiplier;
    data["globalSwapDistance"] = layers.GlobalSwapDistance;
    data["swapHysteresis"] = layers.SwapHysteresis;
    data["maxActivePrefabs"] = layers.MaxActivePrefabs;
    data["useClimateGradients"] = layers.UseClimateGradients;
    
    // Climate configuration
    json climate;
    climate["minAltitude"] = layers.Climate.MinAltitude;
    climate["maxAltitude"] = layers.Climate.MaxAltitude;
    climate["minLongitude"] = layers.Climate.MinLongitude;
    climate["maxLongitude"] = layers.Climate.MaxLongitude;
    
    // Vertical gradient
    json vertGrad = json::array();
    for (const auto& pt : layers.Climate.VerticalGradient.Points) {
        json gpt;
        gpt["pos"] = pt.Position;
        gpt["temp"] = pt.Temperature;
        gpt["moisture"] = pt.Moisture;
        gpt["wind"] = pt.WindExposure;
        vertGrad.push_back(gpt);
    }
    climate["verticalGradient"] = vertGrad;
    
    // Longitudinal gradient
    json longGrad = json::array();
    for (const auto& pt : layers.Climate.LongitudinalGradient.Points) {
        json gpt;
        gpt["pos"] = pt.Position;
        gpt["temp"] = pt.Temperature;
        gpt["moisture"] = pt.Moisture;
        gpt["wind"] = pt.WindExposure;
        longGrad.push_back(gpt);
    }
    climate["longitudinalGradient"] = longGrad;
    data["climate"] = climate;
    
    // Layers
    json layersArr = json::array();
    for (const auto& layer : layers.Layers) {
        json lj;
        lj["guid"] = layer.Guid.ToString();
        lj["name"] = layer.Name;
        lj["enabled"] = layer.Enabled;
        lj["prefabGuid"] = layer.PrefabAsset.guid.ToString();
        lj["prefabPath"] = layer.PrefabPath;
        
        // Distribution
        lj["density"] = layer.DensityPerSquareMeter;
        lj["minSpacing"] = layer.MinSpacing;
        lj["minScale"] = layer.MinScale;
        lj["maxScale"] = layer.MaxScale;
        lj["nonUniformScale"] = layer.NonUniformScale;
        lj["minScaleVec"] = { layer.MinScaleVec.x, layer.MinScaleVec.y, layer.MinScaleVec.z };
        lj["maxScaleVec"] = { layer.MaxScaleVec.x, layer.MaxScaleVec.y, layer.MaxScaleVec.z };
        lj["yawVariance"] = layer.YawVarianceDegrees;
        lj["pitchVariance"] = layer.PitchVarianceDegrees;
        lj["rollVariance"] = layer.RollVarianceDegrees;
        lj["alignToSlope"] = layer.AlignToSlope;
        lj["slopeAlignmentFactor"] = layer.SlopeAlignmentFactor;
        lj["heightOffset"] = layer.HeightOffset;
        lj["heightOffsetVariance"] = layer.HeightOffsetVariance;
        
        // Clustering
        lj["enableClustering"] = layer.EnableClustering;
        lj["clusterRadius"] = layer.ClusterRadius;
        lj["clusterMinCount"] = layer.ClusterMinCount;
        lj["clusterMaxCount"] = layer.ClusterMaxCount;
        lj["clusterFalloff"] = layer.ClusterFalloff;
        lj["clusterSpacing"] = layer.ClusterSpacing;
        
        // LOD
        lj["useImposter"] = layer.UseImposter;
        lj["imposterDistance"] = layer.ImposterDistance;
        lj["cullDistance"] = layer.CullDistance;
        lj["crossfadeRange"] = layer.CrossfadeRange;
        
        // Interaction
        lj["interactable"] = layer.Interactable;
        lj["preservePhysics"] = layer.PreservePhysics;
        lj["interactionRadius"] = layer.InteractionRadius;
        lj["interactionTag"] = layer.InteractionTag;
        
        // Preview
        lj["previewColor"] = { layer.PreviewColor.r, layer.PreviewColor.g, layer.PreviewColor.b };
        
        // Eligibility filters
        json eligibility;
        layer.Eligibility.Serialize(eligibility);
        lj["eligibility"] = eligibility;
        
        layersArr.push_back(lj);
    }
    data["layers"] = layersArr;
    
    // Regions
    json regionsArr = json::array();
    for (const auto& region : layers.Regions) {
        json rj;
        rj["guid"] = region.Guid.ToString();
        rj["name"] = region.Name;
        json pts = json::array();
        for (const auto& pt : region.Points) {
            pts.push_back({ pt.x, pt.y });
        }
        rj["points"] = pts;
        regionsArr.push_back(rj);
    }
    data["regions"] = regionsArr;
    
    // Roads
    json roadsArr = json::array();
    for (const auto& road : layers.Roads) {
        json roadPts = json::array();
        for (const auto& pt : road) {
            roadPts.push_back({ pt.x, pt.y, pt.z });
        }
        roadsArr.push_back(roadPts);
    }
    data["roads"] = roadsArr;
    
    // Persistent state
    json persistent;
    json destroyedArr = json::array();
    for (uint32_t id : layers.Persistent.DestroyedIDs) {
        destroyedArr.push_back(id);
    }
    persistent["destroyed"] = destroyedArr;
    
    json overridesObj;
    for (const auto& [id, state] : layers.Persistent.StateOverrides) {
        overridesObj[std::to_string(id)] = static_cast<int>(state);
    }
    persistent["stateOverrides"] = overridesObj;
    data["persistent"] = persistent;
    
    return data;
}

void Serializer::DeserializeResourceLayers(const json& data, cm::resourcelayer::ResourceLayerComponent& layers) {
    using namespace cm::resourcelayer;
    
    // Global settings
    if (data.contains("globalSeed")) layers.GlobalSeed = data["globalSeed"];
    if (data.contains("globalDensityMultiplier")) layers.GlobalDensityMultiplier = data["globalDensityMultiplier"];
    if (data.contains("globalSwapDistance")) layers.GlobalSwapDistance = data["globalSwapDistance"];
    if (data.contains("swapHysteresis")) layers.SwapHysteresis = data["swapHysteresis"];
    if (data.contains("maxActivePrefabs")) layers.MaxActivePrefabs = data["maxActivePrefabs"];
    if (data.contains("useClimateGradients")) layers.UseClimateGradients = data["useClimateGradients"];
    
    // Climate configuration
    if (data.contains("climate") && data["climate"].is_object()) {
        const json& climate = data["climate"];
        if (climate.contains("minAltitude")) layers.Climate.MinAltitude = climate["minAltitude"];
        if (climate.contains("maxAltitude")) layers.Climate.MaxAltitude = climate["maxAltitude"];
        if (climate.contains("minLongitude")) layers.Climate.MinLongitude = climate["minLongitude"];
        if (climate.contains("maxLongitude")) layers.Climate.MaxLongitude = climate["maxLongitude"];
        
        if (climate.contains("verticalGradient") && climate["verticalGradient"].is_array()) {
            layers.Climate.VerticalGradient.Points.clear();
            for (const auto& gpt : climate["verticalGradient"]) {
                ClimateGradient::ControlPoint pt;
                if (gpt.contains("pos")) pt.Position = gpt["pos"];
                if (gpt.contains("temp")) pt.Temperature = gpt["temp"];
                if (gpt.contains("moisture")) pt.Moisture = gpt["moisture"];
                if (gpt.contains("wind")) pt.WindExposure = gpt["wind"];
                layers.Climate.VerticalGradient.Points.push_back(pt);
            }
        }
        
        if (climate.contains("longitudinalGradient") && climate["longitudinalGradient"].is_array()) {
            layers.Climate.LongitudinalGradient.Points.clear();
            for (const auto& gpt : climate["longitudinalGradient"]) {
                ClimateGradient::ControlPoint pt;
                if (gpt.contains("pos")) pt.Position = gpt["pos"];
                if (gpt.contains("temp")) pt.Temperature = gpt["temp"];
                if (gpt.contains("moisture")) pt.Moisture = gpt["moisture"];
                if (gpt.contains("wind")) pt.WindExposure = gpt["wind"];
                layers.Climate.LongitudinalGradient.Points.push_back(pt);
            }
        }
    }
    
    // Layers
    layers.Layers.clear();
    if (data.contains("layers") && data["layers"].is_array()) {
        for (const auto& lj : data["layers"]) {
            ProceduralResourceLayer layer;
            if (lj.contains("guid")) {
                try { layer.Guid = ClaymoreGUID::FromString(lj["guid"]); } catch(...) {}
            }
            if (lj.contains("name")) layer.Name = lj["name"];
            if (lj.contains("enabled")) layer.Enabled = lj["enabled"];
            if (lj.contains("prefabGuid")) {
                try { layer.PrefabAsset.guid = ClaymoreGUID::FromString(lj["prefabGuid"]); } catch(...) {}
            }
            if (lj.contains("prefabPath")) layer.PrefabPath = lj["prefabPath"];
            
            // Distribution
            if (lj.contains("density")) layer.DensityPerSquareMeter = lj["density"];
            if (lj.contains("minSpacing")) layer.MinSpacing = lj["minSpacing"];
            if (lj.contains("minScale")) layer.MinScale = lj["minScale"];
            if (lj.contains("maxScale")) layer.MaxScale = lj["maxScale"];
            if (lj.contains("nonUniformScale")) layer.NonUniformScale = lj["nonUniformScale"];
            if (lj.contains("minScaleVec") && lj["minScaleVec"].is_array() && lj["minScaleVec"].size() >= 3) {
                layer.MinScaleVec = glm::vec3(lj["minScaleVec"][0], lj["minScaleVec"][1], lj["minScaleVec"][2]);
            }
            if (lj.contains("maxScaleVec") && lj["maxScaleVec"].is_array() && lj["maxScaleVec"].size() >= 3) {
                layer.MaxScaleVec = glm::vec3(lj["maxScaleVec"][0], lj["maxScaleVec"][1], lj["maxScaleVec"][2]);
            }
            if (lj.contains("yawVariance")) layer.YawVarianceDegrees = lj["yawVariance"];
            if (lj.contains("pitchVariance")) layer.PitchVarianceDegrees = lj["pitchVariance"];
            if (lj.contains("rollVariance")) layer.RollVarianceDegrees = lj["rollVariance"];
            if (lj.contains("alignToSlope")) layer.AlignToSlope = lj["alignToSlope"];
            if (lj.contains("slopeAlignmentFactor")) layer.SlopeAlignmentFactor = lj["slopeAlignmentFactor"];
            if (lj.contains("heightOffset")) layer.HeightOffset = lj["heightOffset"];
            if (lj.contains("heightOffsetVariance")) layer.HeightOffsetVariance = lj["heightOffsetVariance"];
            
            // Clustering
            if (lj.contains("enableClustering")) layer.EnableClustering = lj["enableClustering"];
            if (lj.contains("clusterRadius")) layer.ClusterRadius = lj["clusterRadius"];
            if (lj.contains("clusterMinCount")) layer.ClusterMinCount = lj["clusterMinCount"];
            if (lj.contains("clusterMaxCount")) layer.ClusterMaxCount = lj["clusterMaxCount"];
            if (lj.contains("clusterFalloff")) layer.ClusterFalloff = lj["clusterFalloff"];
            if (lj.contains("clusterSpacing")) layer.ClusterSpacing = lj["clusterSpacing"];
            
            // LOD
            if (lj.contains("useImposter")) layer.UseImposter = lj["useImposter"];
            if (lj.contains("imposterDistance")) layer.ImposterDistance = lj["imposterDistance"];
            if (lj.contains("cullDistance")) layer.CullDistance = lj["cullDistance"];
            if (lj.contains("crossfadeRange")) layer.CrossfadeRange = lj["crossfadeRange"];
            
            // Interaction
            if (lj.contains("interactable")) layer.Interactable = lj["interactable"];
            if (lj.contains("preservePhysics")) layer.PreservePhysics = lj["preservePhysics"];
            if (lj.contains("interactionRadius")) layer.InteractionRadius = lj["interactionRadius"];
            if (lj.contains("interactionTag")) layer.InteractionTag = lj["interactionTag"].get<std::string>();
            
            // Preview
            if (lj.contains("previewColor") && lj["previewColor"].is_array() && lj["previewColor"].size() >= 3) {
                layer.PreviewColor = glm::vec3(lj["previewColor"][0], lj["previewColor"][1], lj["previewColor"][2]);
            }
            
            // Eligibility
            if (lj.contains("eligibility")) {
                layer.Eligibility.Deserialize(lj["eligibility"]);
            }
            
            layers.Layers.push_back(std::move(layer));
        }
    }
    
    // Regions
    layers.Regions.clear();
    if (data.contains("regions") && data["regions"].is_array()) {
        for (const auto& rj : data["regions"]) {
            RegionPolygon region;
            if (rj.contains("guid")) {
                try { region.Guid = ClaymoreGUID::FromString(rj["guid"]); } catch(...) {}
            }
            if (rj.contains("name")) region.Name = rj["name"];
            if (rj.contains("points") && rj["points"].is_array()) {
                for (const auto& pt : rj["points"]) {
                    if (pt.is_array() && pt.size() >= 2) {
                        region.Points.push_back(glm::vec2(pt[0], pt[1]));
                    }
                }
                region.UpdateBounds();
            }
            layers.Regions.push_back(std::move(region));
        }
    }
    
    // Roads
    layers.Roads.clear();
    if (data.contains("roads") && data["roads"].is_array()) {
        for (const auto& roadPts : data["roads"]) {
            if (!roadPts.is_array()) continue;
            std::vector<glm::vec3> road;
            for (const auto& pt : roadPts) {
                if (pt.is_array() && pt.size() >= 3) {
                    road.push_back(glm::vec3(pt[0], pt[1], pt[2]));
                }
            }
            if (!road.empty()) {
                layers.Roads.push_back(std::move(road));
            }
        }
    }
    
    // Persistent state
    if (data.contains("persistent") && data["persistent"].is_object()) {
        const json& persistent = data["persistent"];
        if (persistent.contains("destroyed") && persistent["destroyed"].is_array()) {
            layers.Persistent.DestroyedIDs.clear();
            for (const auto& id : persistent["destroyed"]) {
                layers.Persistent.DestroyedIDs.push_back(id.get<uint32_t>());
            }
        }
        if (persistent.contains("stateOverrides") && persistent["stateOverrides"].is_object()) {
            layers.Persistent.StateOverrides.clear();
            for (auto& [key, val] : persistent["stateOverrides"].items()) {
                uint32_t id = std::stoul(key);
                layers.Persistent.StateOverrides[id] = static_cast<ResourceState>(val.get<int>());
            }
        }
    }
    
    layers.NeedsFullRegeneration = true;
}

// Instancer serialization
json Serializer::SerializeInstancer(const cm::instancer::InstancerComponent& instancer) {
    using namespace cm::instancer;
    json data;
    
    // Asset references
    data["meshGuid"] = instancer.MeshAsset.guid.ToString();
    data["meshPath"] = instancer.MeshPath;
    data["prefabGuid"] = instancer.PrefabAsset.guid.ToString();
    data["prefabPath"] = instancer.PrefabPath;
    data["surfaceEntity"] = static_cast<int64_t>(instancer.SurfaceEntity);
    
    // Distribution settings
    json dist;
    dist["seed"] = instancer.Distribution.Seed;
    dist["density"] = instancer.Distribution.DensityPerSquareMeter;
    dist["minSpacing"] = instancer.Distribution.MinSpacing;
    dist["minScale"] = instancer.Distribution.MinScale;
    dist["maxScale"] = instancer.Distribution.MaxScale;
    dist["nonUniformScale"] = instancer.Distribution.NonUniformScale;
    dist["minScaleVec"] = { instancer.Distribution.MinScaleVec.x, instancer.Distribution.MinScaleVec.y, instancer.Distribution.MinScaleVec.z };
    dist["maxScaleVec"] = { instancer.Distribution.MaxScaleVec.x, instancer.Distribution.MaxScaleVec.y, instancer.Distribution.MaxScaleVec.z };
    dist["yawVariance"] = instancer.Distribution.YawVarianceDegrees;
    dist["pitchVariance"] = instancer.Distribution.PitchVarianceDegrees;
    dist["rollVariance"] = instancer.Distribution.RollVarianceDegrees;
    dist["alignToSlope"] = instancer.Distribution.AlignToSlope;
    dist["slopeAlignmentFactor"] = instancer.Distribution.SlopeAlignmentFactor;
    dist["minSlope"] = instancer.Distribution.MinSlopeDegrees;
    dist["maxSlope"] = instancer.Distribution.MaxSlopeDegrees;
    dist["heightOffset"] = instancer.Distribution.HeightOffset;
    dist["heightOffsetVariance"] = instancer.Distribution.HeightOffsetVariance;
    data["distribution"] = dist;
    
    // Distribution area
    data["distributionRadius"] = instancer.DistributionRadius;
    data["distributionAreaMin"] = { instancer.DistributionAreaMin.x, instancer.DistributionAreaMin.y };
    data["distributionAreaMax"] = { instancer.DistributionAreaMax.x, instancer.DistributionAreaMax.y };
    data["useRadiusMode"] = instancer.UseRadiusMode;
    
    // Manual points
    data["useManualPoints"] = instancer.UseManualPoints;
    json manualPts = json::array();
    for (const auto& pt : instancer.ManualPoints) {
        manualPts.push_back({ pt.x, pt.y, pt.z });
    }
    data["manualPoints"] = manualPts;
    
    // Swap settings
    json swap;
    swap["swapDistance"] = instancer.Swap.SwapDistance;
    swap["swapHysteresis"] = instancer.Swap.SwapHysteresis;
    swap["cullDistance"] = instancer.Swap.CullDistance;
    swap["maxActivePrefabs"] = instancer.Swap.MaxActivePrefabs;
    data["swap"] = swap;
    
    // Flags
    data["enabled"] = instancer.Enabled;
    data["previewColor"] = { instancer.PreviewColor.r, instancer.PreviewColor.g, instancer.PreviewColor.b };
    data["showDebugMarkers"] = instancer.ShowDebugMarkers;
    data["showBounds"] = instancer.ShowBounds;
    
    // Rendering options
    data["useAlphaCutout"] = instancer.UseAlphaCutout;
    data["alphaCutoutThreshold"] = instancer.AlphaCutoutThreshold;
    
    // Persistent state
    json persistent;
    json destroyedArr = json::array();
    for (uint32_t id : instancer.Persistent.DestroyedIDs) {
        destroyedArr.push_back(id);
    }
    persistent["destroyed"] = destroyedArr;
    
    json overridesObj;
    for (const auto& [id, state] : instancer.Persistent.StateOverrides) {
        overridesObj[std::to_string(id)] = static_cast<int>(state);
    }
    persistent["stateOverrides"] = overridesObj;
    data["persistent"] = persistent;
    
    return data;
}

void Serializer::DeserializeInstancer(const json& data, cm::instancer::InstancerComponent& instancer) {
    using namespace cm::instancer;
    
    // Asset references
    if (data.contains("meshGuid")) {
        try { instancer.MeshAsset.guid = ClaymoreGUID::FromString(data["meshGuid"].get<std::string>()); }
        catch (...) {}
    }
    if (data.contains("meshPath")) instancer.MeshPath = data["meshPath"].get<std::string>();
    if (data.contains("prefabGuid")) {
        try { instancer.PrefabAsset.guid = ClaymoreGUID::FromString(data["prefabGuid"].get<std::string>()); }
        catch (...) {}
    }
    if (data.contains("prefabPath")) instancer.PrefabPath = data["prefabPath"].get<std::string>();
    if (data.contains("surfaceEntity")) instancer.SurfaceEntity = static_cast<EntityID>(data["surfaceEntity"].get<int64_t>());
    
    // Distribution settings
    if (data.contains("distribution") && data["distribution"].is_object()) {
        const json& dist = data["distribution"];
        if (dist.contains("seed")) instancer.Distribution.Seed = dist["seed"].get<uint32_t>();
        if (dist.contains("density")) instancer.Distribution.DensityPerSquareMeter = dist["density"].get<float>();
        if (dist.contains("minSpacing")) instancer.Distribution.MinSpacing = dist["minSpacing"].get<float>();
        if (dist.contains("minScale")) instancer.Distribution.MinScale = dist["minScale"].get<float>();
        if (dist.contains("maxScale")) instancer.Distribution.MaxScale = dist["maxScale"].get<float>();
        if (dist.contains("nonUniformScale")) instancer.Distribution.NonUniformScale = dist["nonUniformScale"].get<bool>();
        if (dist.contains("minScaleVec") && dist["minScaleVec"].is_array() && dist["minScaleVec"].size() >= 3) {
            instancer.Distribution.MinScaleVec = glm::vec3(
                dist["minScaleVec"][0].get<float>(),
                dist["minScaleVec"][1].get<float>(),
                dist["minScaleVec"][2].get<float>());
        }
        if (dist.contains("maxScaleVec") && dist["maxScaleVec"].is_array() && dist["maxScaleVec"].size() >= 3) {
            instancer.Distribution.MaxScaleVec = glm::vec3(
                dist["maxScaleVec"][0].get<float>(),
                dist["maxScaleVec"][1].get<float>(),
                dist["maxScaleVec"][2].get<float>());
        }
        if (dist.contains("yawVariance")) instancer.Distribution.YawVarianceDegrees = dist["yawVariance"].get<float>();
        if (dist.contains("pitchVariance")) instancer.Distribution.PitchVarianceDegrees = dist["pitchVariance"].get<float>();
        if (dist.contains("rollVariance")) instancer.Distribution.RollVarianceDegrees = dist["rollVariance"].get<float>();
        if (dist.contains("alignToSlope")) instancer.Distribution.AlignToSlope = dist["alignToSlope"].get<bool>();
        if (dist.contains("slopeAlignmentFactor")) instancer.Distribution.SlopeAlignmentFactor = dist["slopeAlignmentFactor"].get<float>();
        if (dist.contains("minSlope")) instancer.Distribution.MinSlopeDegrees = dist["minSlope"].get<float>();
        if (dist.contains("maxSlope")) instancer.Distribution.MaxSlopeDegrees = dist["maxSlope"].get<float>();
        if (dist.contains("heightOffset")) instancer.Distribution.HeightOffset = dist["heightOffset"].get<float>();
        if (dist.contains("heightOffsetVariance")) instancer.Distribution.HeightOffsetVariance = dist["heightOffsetVariance"].get<float>();
    }
    
    // Distribution area
    if (data.contains("distributionRadius")) instancer.DistributionRadius = data["distributionRadius"].get<float>();
    if (data.contains("distributionAreaMin") && data["distributionAreaMin"].is_array() && data["distributionAreaMin"].size() >= 2) {
        instancer.DistributionAreaMin = glm::vec2(
            data["distributionAreaMin"][0].get<float>(),
            data["distributionAreaMin"][1].get<float>());
    }
    if (data.contains("distributionAreaMax") && data["distributionAreaMax"].is_array() && data["distributionAreaMax"].size() >= 2) {
        instancer.DistributionAreaMax = glm::vec2(
            data["distributionAreaMax"][0].get<float>(),
            data["distributionAreaMax"][1].get<float>());
    }
    if (data.contains("useRadiusMode")) instancer.UseRadiusMode = data["useRadiusMode"].get<bool>();
    
    // Manual points
    if (data.contains("useManualPoints")) instancer.UseManualPoints = data["useManualPoints"].get<bool>();
    instancer.ManualPoints.clear();
    if (data.contains("manualPoints") && data["manualPoints"].is_array()) {
        for (const auto& pt : data["manualPoints"]) {
            if (pt.is_array() && pt.size() >= 3) {
                instancer.ManualPoints.push_back(glm::vec3(
                    pt[0].get<float>(),
                    pt[1].get<float>(),
                    pt[2].get<float>()));
            }
        }
    }
    
    // Swap settings
    if (data.contains("swap") && data["swap"].is_object()) {
        const json& swap = data["swap"];
        if (swap.contains("swapDistance")) instancer.Swap.SwapDistance = swap["swapDistance"].get<float>();
        if (swap.contains("swapHysteresis")) instancer.Swap.SwapHysteresis = swap["swapHysteresis"].get<float>();
        if (swap.contains("cullDistance")) instancer.Swap.CullDistance = swap["cullDistance"].get<float>();
        if (swap.contains("maxActivePrefabs")) instancer.Swap.MaxActivePrefabs = swap["maxActivePrefabs"].get<uint32_t>();
    }
    
    // Flags
    if (data.contains("enabled")) instancer.Enabled = data["enabled"].get<bool>();
    if (data.contains("previewColor") && data["previewColor"].is_array() && data["previewColor"].size() >= 3) {
        instancer.PreviewColor = glm::vec3(
            data["previewColor"][0].get<float>(),
            data["previewColor"][1].get<float>(),
            data["previewColor"][2].get<float>());
    }
    if (data.contains("showDebugMarkers")) instancer.ShowDebugMarkers = data["showDebugMarkers"].get<bool>();
    if (data.contains("showBounds")) instancer.ShowBounds = data["showBounds"].get<bool>();
    
    // Rendering options
    if (data.contains("useAlphaCutout")) instancer.UseAlphaCutout = data["useAlphaCutout"].get<bool>();
    if (data.contains("alphaCutoutThreshold")) instancer.AlphaCutoutThreshold = data["alphaCutoutThreshold"].get<float>();
    
    // Persistent state
    if (data.contains("persistent") && data["persistent"].is_object()) {
        const json& persistent = data["persistent"];
        if (persistent.contains("destroyed") && persistent["destroyed"].is_array()) {
            instancer.Persistent.DestroyedIDs.clear();
            for (const auto& id : persistent["destroyed"]) {
                instancer.Persistent.DestroyedIDs.push_back(id.get<uint32_t>());
            }
        }
        if (persistent.contains("stateOverrides") && persistent["stateOverrides"].is_object()) {
            instancer.Persistent.StateOverrides.clear();
            for (auto& [key, val] : persistent["stateOverrides"].items()) {
                uint32_t id = std::stoul(key);
                instancer.Persistent.StateOverrides[id] = static_cast<InstanceState>(val.get<int>());
            }
        }
    }
    
    instancer.NeedsRegeneration = true;
    instancer.NeedsMeshReload = true;
}

// River serialization
json Serializer::SerializeRiver(const RiverComponent& river) {
    json data;
    
    // Asset path for binary mesh data
    data["meshAssetPath"] = river.MeshAssetPath;
    data["meshAssetGuid"] = river.MeshAssetGuid.ToString();
    
    // Path points
    json pathPoints = json::array();
    for (const auto& pt : river.PathPoints) {
        json point;
        point["position"] = { pt.Position.x, pt.Position.y, pt.Position.z };
        point["normal"] = { pt.Normal.x, pt.Normal.y, pt.Normal.z };
        pathPoints.push_back(point);
    }
    data["pathPoints"] = pathPoints;
    
    // Settings
    data["width"] = river.Width;
    data["depth"] = river.Depth;
    data["bankSmoothing"] = river.BankSmoothing;
    data["splineSubdivision"] = river.SplineSubdivision;
    data["waterHeight"] = river.WaterHeight;
    
    // Material settings
    data["materialName"] = river.MaterialName;
    data["waterColor"] = { river.WaterColor.r, river.WaterColor.g, river.WaterColor.b, river.WaterColor.a };
    data["flowSpeed"] = river.FlowSpeed;
    
    // Entity reference (will be relinked on load)
    data["meshEntity"] = static_cast<int64_t>(river.MeshEntity);
    data["meshGenerated"] = river.MeshGenerated;
    
    return data;
}

void Serializer::DeserializeRiver(const json& data, RiverComponent& river) {
    // Asset path for binary mesh data
    if (data.contains("meshAssetPath")) river.MeshAssetPath = data["meshAssetPath"];
    if (data.contains("meshAssetGuid")) {
        try { river.MeshAssetGuid = ClaymoreGUID::FromString(data["meshAssetGuid"].get<std::string>()); }
        catch (...) {}
    }
    
    // Path points
    river.PathPoints.clear();
    if (data.contains("pathPoints") && data["pathPoints"].is_array()) {
        for (const auto& pt : data["pathPoints"]) {
            RiverPathPoint point;
            if (pt.contains("position") && pt["position"].is_array() && pt["position"].size() >= 3) {
                point.Position.x = pt["position"][0];
                point.Position.y = pt["position"][1];
                point.Position.z = pt["position"][2];
            }
            if (pt.contains("normal") && pt["normal"].is_array() && pt["normal"].size() >= 3) {
                point.Normal.x = pt["normal"][0];
                point.Normal.y = pt["normal"][1];
                point.Normal.z = pt["normal"][2];
            }
            river.PathPoints.push_back(point);
        }
    }
    
    // Settings
    if (data.contains("width")) river.Width = data["width"];
    if (data.contains("depth")) river.Depth = data["depth"];
    if (data.contains("bankSmoothing")) river.BankSmoothing = data["bankSmoothing"];
    if (data.contains("splineSubdivision")) river.SplineSubdivision = data["splineSubdivision"];
    if (data.contains("waterHeight")) river.WaterHeight = data["waterHeight"];
    
    // Material settings
    if (data.contains("materialName")) river.MaterialName = data["materialName"];
    if (data.contains("waterColor") && data["waterColor"].is_array() && data["waterColor"].size() >= 4) {
        river.WaterColor.r = data["waterColor"][0];
        river.WaterColor.g = data["waterColor"][1];
        river.WaterColor.b = data["waterColor"][2];
        river.WaterColor.a = data["waterColor"][3];
    }
    if (data.contains("flowSpeed")) river.FlowSpeed = data["flowSpeed"];
    
    // Entity reference (will need relinking after scene load)
    if (data.contains("meshEntity")) river.MeshEntity = static_cast<EntityID>(data["meshEntity"].get<int64_t>());
    if (data.contains("meshGenerated")) river.MeshGenerated = data["meshGenerated"];
    
    // Mark for regeneration if mesh asset needs to be loaded
    river.NeedsRegeneration = river.MeshGenerated && !river.MeshAssetPath.empty();
}

// Spline serialization
json Serializer::SerializeSpline(const SplineComponent& spline) {
    json data;
    json points = json::array();
    for (const auto& pt : spline.ControlPoints) {
        json point;
        point["position"] = { pt.Position.x, pt.Position.y, pt.Position.z };
        point["normal"] = { pt.Normal.x, pt.Normal.y, pt.Normal.z };
        points.push_back(point);
    }
    data["controlPoints"] = points;
    data["splineSubdivision"] = spline.SplineSubdivision;
    data["closed"] = spline.Closed;
    return data;
}

void Serializer::DeserializeSpline(const json& data, SplineComponent& spline) {
    spline.ControlPoints.clear();
    if (data.contains("controlPoints") && data["controlPoints"].is_array()) {
        for (const auto& pt : data["controlPoints"]) {
            SplinePathPoint point;
            if (pt.contains("position") && pt["position"].is_array() && pt["position"].size() >= 3) {
                point.Position.x = pt["position"][0];
                point.Position.y = pt["position"][1];
                point.Position.z = pt["position"][2];
            }
            if (pt.contains("normal") && pt["normal"].is_array() && pt["normal"].size() >= 3) {
                point.Normal.x = pt["normal"][0];
                point.Normal.y = pt["normal"][1];
                point.Normal.z = pt["normal"][2];
            }
            spline.ControlPoints.push_back(point);
        }
    }
    if (data.contains("splineSubdivision")) spline.SplineSubdivision = data["splineSubdivision"];
    if (data.contains("closed")) spline.Closed = data["closed"];
}

// Particle emitter serialization - comprehensive modern particle system
json Serializer::SerializeParticleEmitter(const ParticleEmitterComponent& emitter) {
    json data;
    
    // ===== Basic Settings =====
    data["enabled"] = emitter.Enabled;
    data["maxParticles"] = emitter.MaxParticles;
    if (!emitter.SpritePath.empty()) data["spritePath"] = emitter.SpritePath;
    
    // ===== Duration & Looping =====
    data["duration"] = emitter.Duration;
    data["looping"] = emitter.Looping;
    data["prewarm"] = emitter.Prewarm;
    data["playOnAwake"] = emitter.PlayOnAwake;
    data["destroyOnComplete"] = emitter.DestroyOnComplete;
    data["stopEmittingOnComplete"] = emitter.StopEmittingOnComplete;
    
    // ===== Emission =====
    data["emissionRate"] = emitter.EmissionRate;
    data["rateOverDistance"] = emitter.RateOverDistance;
    
    // Burst settings
    data["burstEnabled"] = emitter.BurstEnabled;
    data["burstCount"] = emitter.BurstCount;
    data["burstTime"] = emitter.BurstTime;
    data["burstCycles"] = emitter.BurstCycles;
    data["burstInterval"] = emitter.BurstInterval;
    
    // ===== Shape =====
    data["shape"] = static_cast<int>(emitter.Shape);
    data["shapeRadius"] = emitter.ShapeRadius;
    data["shapeRadiusThickness"] = emitter.ShapeRadiusThickness;
    data["shapeAngle"] = emitter.ShapeAngle;
    data["shapeArc"] = emitter.ShapeArc;
    data["shapeScale"] = { emitter.ShapeScale.x, emitter.ShapeScale.y, emitter.ShapeScale.z };
    data["shapeLength"] = emitter.ShapeLength;
    data["shapeEmitFromEdge"] = emitter.ShapeEmitFromEdge;
    data["shapeRandomizeDirection"] = emitter.ShapeRandomizeDirection;
    
    // ===== Lifetime =====
    data["lifetimeMin"] = emitter.Lifetime.Min;
    data["lifetimeMax"] = emitter.Lifetime.Max;
    
    // ===== Start Values =====
    data["startSpeedMin"] = emitter.StartSpeed.Min;
    data["startSpeedMax"] = emitter.StartSpeed.Max;
    data["startSizeMin"] = emitter.StartSize.Min;
    data["startSizeMax"] = emitter.StartSize.Max;
    data["startRotationMin"] = emitter.StartRotation.Min;
    data["startRotationMax"] = emitter.StartRotation.Max;
    data["startColor"] = { emitter.StartColor.r, emitter.StartColor.g, emitter.StartColor.b, emitter.StartColor.a };
    data["startColorRandom"] = emitter.StartColorRandom;
    data["startColorMin"] = { emitter.StartColorMin.r, emitter.StartColorMin.g, emitter.StartColorMin.b, emitter.StartColorMin.a };
    data["startColorMax"] = { emitter.StartColorMax.r, emitter.StartColorMax.g, emitter.StartColorMax.b, emitter.StartColorMax.a };
    
    // ===== Physics =====
    data["gravityModifier"] = emitter.GravityModifier;
    data["simulationSpace"] = static_cast<int>(emitter.SimulationSpace);
    data["inheritVelocity"] = emitter.InheritVelocity;
    data["dragCoefficient"] = emitter.DragCoefficient;
    
    // ===== Velocity Over Lifetime =====
    data["velocityOverLifetimeEnabled"] = emitter.VelocityOverLifetimeEnabled;
    data["linearVelocity"] = { emitter.LinearVelocity.x, emitter.LinearVelocity.y, emitter.LinearVelocity.z };
    data["orbitalVelocity"] = emitter.OrbitalVelocity;
    data["radialVelocity"] = emitter.RadialVelocity;
    
    // ===== Size Over Lifetime =====
    data["sizeOverLifetimeEnabled"] = emitter.SizeOverLifetimeEnabled;
    data["sizeOverLifetimeCurve"] = static_cast<int>(emitter.SizeOverLifetime.CurveType);
    data["sizeOverLifetimeStart"] = emitter.SizeOverLifetime.StartValue;
    data["sizeOverLifetimeEnd"] = emitter.SizeOverLifetime.EndValue;
    
    // ===== Color Over Lifetime =====
    data["colorOverLifetimeEnabled"] = emitter.ColorOverLifetimeEnabled;
    json gradientKeys = json::array();
    for (const auto& key : emitter.ColorGradient)
    {
        gradientKeys.push_back({
            {"time", key.Time},
            {"color", { key.Color.r, key.Color.g, key.Color.b, key.Color.a }}
        });
    }
    data["colorGradient"] = gradientKeys;
    
    // ===== Rotation Over Lifetime =====
    data["rotationOverLifetimeEnabled"] = emitter.RotationOverLifetimeEnabled;
    data["angularVelocity"] = emitter.AngularVelocity;
    data["alignWithTrajectory"] = emitter.AlignWithTrajectory;
    
    // ===== Rendering =====
    data["blendMode"] = static_cast<int>(emitter.BlendMode);
    data["renderOrder"] = emitter.RenderOrder;
    data["faceCamera"] = emitter.FaceCamera;
    
    // ===== Texture Sheet (future) =====
    data["textureSheetEnabled"] = emitter.TextureSheetEnabled;
    data["textureSheetTilesX"] = emitter.TextureSheetTilesX;
    data["textureSheetTilesY"] = emitter.TextureSheetTilesY;
    data["textureSheetFrameRate"] = emitter.TextureSheetFrameRate;
    data["textureSheetRandomStart"] = emitter.TextureSheetRandomStart;
    
    return data;
}

void Serializer::DeserializeParticleEmitter(const json& data, ParticleEmitterComponent& emitter) {
    // ===== Basic Settings =====
    if (data.contains("enabled")) emitter.Enabled = data["enabled"];
    if (data.contains("maxParticles")) emitter.MaxParticles = data["maxParticles"];
    if (data.contains("spritePath")) emitter.SpritePath = data["spritePath"];
    
    // ===== Duration & Looping =====
    if (data.contains("duration")) emitter.Duration = data["duration"];
    if (data.contains("looping")) emitter.Looping = data["looping"];
    if (data.contains("prewarm")) emitter.Prewarm = data["prewarm"];
    if (data.contains("playOnAwake")) emitter.PlayOnAwake = data["playOnAwake"];
    if (data.contains("destroyOnComplete")) emitter.DestroyOnComplete = data["destroyOnComplete"];
    if (data.contains("stopEmittingOnComplete")) emitter.StopEmittingOnComplete = data["stopEmittingOnComplete"];
    
    // ===== Emission =====
    if (data.contains("emissionRate")) emitter.EmissionRate = data["emissionRate"];
    if (data.contains("rateOverDistance")) emitter.RateOverDistance = data["rateOverDistance"];
    
    // Burst settings
    if (data.contains("burstEnabled")) emitter.BurstEnabled = data["burstEnabled"];
    if (data.contains("burstCount")) emitter.BurstCount = data["burstCount"];
    if (data.contains("burstTime")) emitter.BurstTime = data["burstTime"];
    if (data.contains("burstCycles")) emitter.BurstCycles = data["burstCycles"];
    if (data.contains("burstInterval")) emitter.BurstInterval = data["burstInterval"];
    
    // ===== Shape =====
    if (data.contains("shape")) emitter.Shape = static_cast<ParticleEmissionShape>(data["shape"].get<int>());
    if (data.contains("shapeRadius")) emitter.ShapeRadius = data["shapeRadius"];
    if (data.contains("shapeRadiusThickness")) emitter.ShapeRadiusThickness = data["shapeRadiusThickness"];
    if (data.contains("shapeAngle")) emitter.ShapeAngle = data["shapeAngle"];
    if (data.contains("shapeArc")) emitter.ShapeArc = data["shapeArc"];
    if (data.contains("shapeScale") && data["shapeScale"].is_array() && data["shapeScale"].size() >= 3)
    {
        emitter.ShapeScale.x = data["shapeScale"][0];
        emitter.ShapeScale.y = data["shapeScale"][1];
        emitter.ShapeScale.z = data["shapeScale"][2];
    }
    if (data.contains("shapeLength")) emitter.ShapeLength = data["shapeLength"];
    if (data.contains("shapeEmitFromEdge")) emitter.ShapeEmitFromEdge = data["shapeEmitFromEdge"];
    if (data.contains("shapeRandomizeDirection")) emitter.ShapeRandomizeDirection = data["shapeRandomizeDirection"];
    
    // ===== Lifetime =====
    if (data.contains("lifetimeMin")) emitter.Lifetime.Min = data["lifetimeMin"];
    if (data.contains("lifetimeMax")) emitter.Lifetime.Max = data["lifetimeMax"];
    
    // ===== Start Values =====
    if (data.contains("startSpeedMin")) emitter.StartSpeed.Min = data["startSpeedMin"];
    if (data.contains("startSpeedMax")) emitter.StartSpeed.Max = data["startSpeedMax"];
    if (data.contains("startSizeMin")) emitter.StartSize.Min = data["startSizeMin"];
    if (data.contains("startSizeMax")) emitter.StartSize.Max = data["startSizeMax"];
    if (data.contains("startRotationMin")) emitter.StartRotation.Min = data["startRotationMin"];
    if (data.contains("startRotationMax")) emitter.StartRotation.Max = data["startRotationMax"];
    if (data.contains("startColor") && data["startColor"].is_array() && data["startColor"].size() >= 4)
    {
        emitter.StartColor.r = data["startColor"][0];
        emitter.StartColor.g = data["startColor"][1];
        emitter.StartColor.b = data["startColor"][2];
        emitter.StartColor.a = data["startColor"][3];
    }
    if (data.contains("startColorRandom")) emitter.StartColorRandom = data["startColorRandom"];
    if (data.contains("startColorMin") && data["startColorMin"].is_array() && data["startColorMin"].size() >= 4)
    {
        emitter.StartColorMin.r = data["startColorMin"][0];
        emitter.StartColorMin.g = data["startColorMin"][1];
        emitter.StartColorMin.b = data["startColorMin"][2];
        emitter.StartColorMin.a = data["startColorMin"][3];
    }
    if (data.contains("startColorMax") && data["startColorMax"].is_array() && data["startColorMax"].size() >= 4)
    {
        emitter.StartColorMax.r = data["startColorMax"][0];
        emitter.StartColorMax.g = data["startColorMax"][1];
        emitter.StartColorMax.b = data["startColorMax"][2];
        emitter.StartColorMax.a = data["startColorMax"][3];
    }
    
    // ===== Physics =====
    if (data.contains("gravityModifier")) emitter.GravityModifier = data["gravityModifier"];
    if (data.contains("simulationSpace")) emitter.SimulationSpace = static_cast<ParticleSimulationSpace>(data["simulationSpace"].get<int>());
    if (data.contains("inheritVelocity")) emitter.InheritVelocity = data["inheritVelocity"];
    if (data.contains("dragCoefficient")) emitter.DragCoefficient = data["dragCoefficient"];
    
    // ===== Velocity Over Lifetime =====
    if (data.contains("velocityOverLifetimeEnabled")) emitter.VelocityOverLifetimeEnabled = data["velocityOverLifetimeEnabled"];
    if (data.contains("linearVelocity") && data["linearVelocity"].is_array() && data["linearVelocity"].size() >= 3)
    {
        emitter.LinearVelocity.x = data["linearVelocity"][0];
        emitter.LinearVelocity.y = data["linearVelocity"][1];
        emitter.LinearVelocity.z = data["linearVelocity"][2];
    }
    if (data.contains("orbitalVelocity")) emitter.OrbitalVelocity = data["orbitalVelocity"];
    if (data.contains("radialVelocity")) emitter.RadialVelocity = data["radialVelocity"];
    
    // ===== Size Over Lifetime =====
    if (data.contains("sizeOverLifetimeEnabled")) emitter.SizeOverLifetimeEnabled = data["sizeOverLifetimeEnabled"];
    if (data.contains("sizeOverLifetimeCurve")) emitter.SizeOverLifetime.CurveType = static_cast<ParticleCurveType>(data["sizeOverLifetimeCurve"].get<int>());
    if (data.contains("sizeOverLifetimeStart")) emitter.SizeOverLifetime.StartValue = data["sizeOverLifetimeStart"];
    if (data.contains("sizeOverLifetimeEnd")) emitter.SizeOverLifetime.EndValue = data["sizeOverLifetimeEnd"];
    
    // ===== Color Over Lifetime =====
    if (data.contains("colorOverLifetimeEnabled")) emitter.ColorOverLifetimeEnabled = data["colorOverLifetimeEnabled"];
    if (data.contains("colorGradient") && data["colorGradient"].is_array())
    {
        emitter.ColorGradient.clear();
        for (const auto& keyJson : data["colorGradient"])
        {
            ParticleColorKey key;
            if (keyJson.contains("time")) key.Time = keyJson["time"];
            if (keyJson.contains("color") && keyJson["color"].is_array() && keyJson["color"].size() >= 4)
            {
                key.Color.r = keyJson["color"][0];
                key.Color.g = keyJson["color"][1];
                key.Color.b = keyJson["color"][2];
                key.Color.a = keyJson["color"][3];
            }
            emitter.ColorGradient.push_back(key);
        }
    }
    
    // ===== Rotation Over Lifetime =====
    if (data.contains("rotationOverLifetimeEnabled")) emitter.RotationOverLifetimeEnabled = data["rotationOverLifetimeEnabled"];
    if (data.contains("angularVelocity")) emitter.AngularVelocity = data["angularVelocity"];
    if (data.contains("alignWithTrajectory")) emitter.AlignWithTrajectory = data["alignWithTrajectory"];
    
    // ===== Rendering =====
    if (data.contains("blendMode")) emitter.BlendMode = static_cast<ParticleBlendMode>(data["blendMode"].get<int>());
    if (data.contains("renderOrder")) emitter.RenderOrder = data["renderOrder"];
    if (data.contains("faceCamera")) emitter.FaceCamera = data["faceCamera"];
    
    // ===== Texture Sheet (future) =====
    if (data.contains("textureSheetEnabled")) emitter.TextureSheetEnabled = data["textureSheetEnabled"];
    if (data.contains("textureSheetTilesX")) emitter.TextureSheetTilesX = data["textureSheetTilesX"];
    if (data.contains("textureSheetTilesY")) emitter.TextureSheetTilesY = data["textureSheetTilesY"];
    if (data.contains("textureSheetFrameRate")) emitter.TextureSheetFrameRate = data["textureSheetFrameRate"];
    if (data.contains("textureSheetRandomStart")) emitter.TextureSheetRandomStart = data["textureSheetRandomStart"];
    
    // Legacy compatibility: handle old particlesPerSecond field
    if (data.contains("particlesPerSecond") && !data.contains("emissionRate"))
    {
        emitter.EmissionRate = static_cast<float>(data["particlesPerSecond"].get<uint32_t>());
    }
}

// UI serialization
json Serializer::SerializeCanvas(const CanvasComponent& canvas) {
    json data;
    data["width"] = canvas.Width;
    data["height"] = canvas.Height;
    data["dpiScale"] = canvas.DPIScale;
    data["space"] = static_cast<int>(canvas.Space);
    data["sortOrder"] = canvas.SortOrder;
    data["opacity"] = canvas.Opacity;
    data["blockSceneInput"] = canvas.BlockSceneInput;
    data["billboard"] = canvas.Billboard;
    // Reference resolution for resolution-independent UI
    data["referenceWidth"] = canvas.ReferenceWidth;
    data["referenceHeight"] = canvas.ReferenceHeight;
    data["referenceScaleMode"] = static_cast<int>(canvas.ReferenceScaleMode);
    return data;
}

void Serializer::DeserializeCanvas(const json& data, CanvasComponent& canvas) {
    if (data.contains("width")) canvas.Width = data["width"];
    if (data.contains("height")) canvas.Height = data["height"];
    if (data.contains("dpiScale")) canvas.DPIScale = data["dpiScale"];
    if (data.contains("space")) canvas.Space = static_cast<CanvasComponent::RenderSpace>(data["space"]);
    if (data.contains("sortOrder")) canvas.SortOrder = data["sortOrder"];
    if (data.contains("opacity")) canvas.Opacity = data["opacity"];
    if (data.contains("blockSceneInput")) canvas.BlockSceneInput = data["blockSceneInput"];
    if (data.contains("billboard")) canvas.Billboard = data["billboard"];
    // Reference resolution for resolution-independent UI
    if (data.contains("referenceWidth")) canvas.ReferenceWidth = data["referenceWidth"];
    if (data.contains("referenceHeight")) canvas.ReferenceHeight = data["referenceHeight"];
    if (data.contains("referenceScaleMode")) canvas.ReferenceScaleMode = static_cast<CanvasComponent::ScaleMode>(data["referenceScaleMode"]);
}

json Serializer::SerializePanel(const PanelComponent& panel) {
    json data;
    data["position"] = { panel.Position.x, panel.Position.y };
    data["size"] = { panel.Size.x, panel.Size.y };
    data["scale"] = { panel.Scale.x, panel.Scale.y };
    data["pivot"] = { panel.Pivot.x, panel.Pivot.y };
    data["rotation"] = panel.Rotation;
    data["texture"] = panel.Texture;
    data["uvRect"] = { panel.UVRect.x, panel.UVRect.y, panel.UVRect.z, panel.UVRect.w };
    data["tintColor"] = { panel.TintColor.r, panel.TintColor.g, panel.TintColor.b, panel.TintColor.a };
    data["opacity"] = panel.Opacity;
    data["driveChildrenOpacity"] = panel.DriveChildrenOpacity;
    data["visible"] = panel.Visible;
    data["allowDrag"] = panel.AllowDrag;
    data["allowDrop"] = panel.AllowDrop;
    data["zOrder"] = panel.ZOrder;
    data["anchorEnabled"] = panel.AnchorEnabled;
    data["anchorToParentUI"] = panel.AnchorToParentUI;
    data["anchor"] = (int)panel.Anchor;
    data["anchorOffset"] = { panel.AnchorOffset.x, panel.AnchorOffset.y };
    data["fillMode"] = (int)panel.Mode;
    data["tileRepeat"] = { panel.TileRepeat.x, panel.TileRepeat.y };
    data["sliceUV"] = { panel.SliceUV.x, panel.SliceUV.y, panel.SliceUV.z, panel.SliceUV.w };
    data["sliceBorder"] = { panel.SliceBorder.x, panel.SliceBorder.y, panel.SliceBorder.z, panel.SliceBorder.w };
    return data;
}

void Serializer::DeserializePanel(const json& data, PanelComponent& panel) {
    if (data.contains("position") && data["position"].is_array() && data["position"].size() == 2) {
        panel.Position.x = data["position"][0];
        panel.Position.y = data["position"][1];
    }
    if (data.contains("size") && data["size"].is_array() && data["size"].size() == 2) {
        panel.Size.x = data["size"][0];
        panel.Size.y = data["size"][1];
    }
    if (data.contains("scale") && data["scale"].is_array() && data["scale"].size() == 2) {
        panel.Scale.x = data["scale"][0];
        panel.Scale.y = data["scale"][1];
    }
    if (data.contains("pivot") && data["pivot"].is_array() && data["pivot"].size() == 2) {
        panel.Pivot.x = data["pivot"][0];
        panel.Pivot.y = data["pivot"][1];
    }
    if (data.contains("rotation")) panel.Rotation = data["rotation"];
    if (data.contains("texture")) data["texture"].get_to(panel.Texture);
    if (data.contains("uvRect") && data["uvRect"].is_array() && data["uvRect"].size() == 4) {
        panel.UVRect.x = data["uvRect"][0];
        panel.UVRect.y = data["uvRect"][1];
        panel.UVRect.z = data["uvRect"][2];
        panel.UVRect.w = data["uvRect"][3];
    }
    if (data.contains("tintColor") && data["tintColor"].is_array() && data["tintColor"].size() == 4) {
        panel.TintColor.r = data["tintColor"][0];
        panel.TintColor.g = data["tintColor"][1];
        panel.TintColor.b = data["tintColor"][2];
        panel.TintColor.a = data["tintColor"][3];
    }
    if (data.contains("opacity")) panel.Opacity = data["opacity"];
    if (data.contains("driveChildrenOpacity")) panel.DriveChildrenOpacity = data["driveChildrenOpacity"];
    if (data.contains("visible")) panel.Visible = data["visible"];
    if (data.contains("allowDrag")) panel.AllowDrag = data["allowDrag"];
    if (data.contains("allowDrop")) panel.AllowDrop = data["allowDrop"];
    if (data.contains("zOrder")) panel.ZOrder = data["zOrder"];
    if (data.contains("anchorEnabled")) panel.AnchorEnabled = data["anchorEnabled"];
    if (data.contains("anchorToParentUI")) panel.AnchorToParentUI = data["anchorToParentUI"];
    if (data.contains("anchor")) panel.Anchor = (UIAnchorPreset)((int)data["anchor"]);
    if (data.contains("anchorOffset") && data["anchorOffset"].is_array() && data["anchorOffset"].size() == 2) {
        panel.AnchorOffset.x = data["anchorOffset"][0];
        panel.AnchorOffset.y = data["anchorOffset"][1];
    }
    if (data.contains("fillMode")) panel.Mode = (PanelComponent::FillMode)((int)data["fillMode"]);
    if (data.contains("tileRepeat") && data["tileRepeat"].is_array() && data["tileRepeat"].size() == 2) {
        panel.TileRepeat.x = data["tileRepeat"][0];
        panel.TileRepeat.y = data["tileRepeat"][1];
    }
    if (data.contains("sliceUV") && data["sliceUV"].is_array() && data["sliceUV"].size() == 4) {
        panel.SliceUV.x = data["sliceUV"][0];
        panel.SliceUV.y = data["sliceUV"][1];
        panel.SliceUV.z = data["sliceUV"][2];
        panel.SliceUV.w = data["sliceUV"][3];
    }
    if (data.contains("sliceBorder") && data["sliceBorder"].is_array() && data["sliceBorder"].size() == 4) {
        panel.SliceBorder.x = data["sliceBorder"][0];
        panel.SliceBorder.y = data["sliceBorder"][1];
        panel.SliceBorder.z = data["sliceBorder"][2];
        panel.SliceBorder.w = data["sliceBorder"][3];
    }
}

json Serializer::SerializeButton(const ButtonComponent& button) {
    json data;
    data["interactable"] = button.Interactable;
    data["toggle"] = button.Toggle;
    data["toggled"] = button.Toggled;
    data["normalTint"] = { button.NormalTint.r, button.NormalTint.g, button.NormalTint.b, button.NormalTint.a };
    data["hoverTint"] = { button.HoverTint.r, button.HoverTint.g, button.HoverTint.b, button.HoverTint.a };
    data["pressedTint"] = { button.PressedTint.r, button.PressedTint.g, button.PressedTint.b, button.PressedTint.a };
    data["disabledTint"] = { button.DisabledTint.r, button.DisabledTint.g, button.DisabledTint.b, button.DisabledTint.a };
    data["hoverSound"] = button.HoverSound;
    data["clickSound"] = button.ClickSound;
    return data;
}

json Serializer::SerializeSlider(const SliderComponent& s) {
    json data;
    data["minValue"] = s.MinValue;
    data["maxValue"] = s.MaxValue;
    data["value"] = s.Value;
    data["step"] = s.Step;
    data["direction"] = (int)s.SliderDirection;
    data["handleSize"] = { s.HandleSize.x, s.HandleSize.y };
    data["handleNormalTint"] = { s.HandleNormalTint.r, s.HandleNormalTint.g, s.HandleNormalTint.b, s.HandleNormalTint.a };
    data["handleHoverTint"] = { s.HandleHoverTint.r, s.HandleHoverTint.g, s.HandleHoverTint.b, s.HandleHoverTint.a };
    data["handlePressedTint"] = { s.HandlePressedTint.r, s.HandlePressedTint.g, s.HandlePressedTint.b, s.HandlePressedTint.a };
    data["handleDisabledTint"] = { s.HandleDisabledTint.r, s.HandleDisabledTint.g, s.HandleDisabledTint.b, s.HandleDisabledTint.a };
    data["showFill"] = s.ShowFill;
    data["fillColor"] = { s.FillColor.r, s.FillColor.g, s.FillColor.b, s.FillColor.a };
    data["interactable"] = s.Interactable;
    data["wholeNumbers"] = s.WholeNumbers;
    data["handleTexture"] = s.HandleTexture;
    data["fillTexture"] = s.FillTexture;
    data["opacity"] = s.Opacity;
    data["visible"] = s.Visible;
    return data;
}

json Serializer::SerializeProgressBar(const ProgressBarComponent& p) {
    json data;
    data["value"] = p.Value;
    data["minValue"] = p.MinValue;
    data["maxValue"] = p.MaxValue;
    data["direction"] = (int)p.Direction;
    data["fillColor"] = { p.FillColor.r, p.FillColor.g, p.FillColor.b, p.FillColor.a };
    data["useGradient"] = p.UseGradient;
    data["gradientLowColor"] = { p.GradientLowColor.r, p.GradientLowColor.g, p.GradientLowColor.b, p.GradientLowColor.a };
    data["gradientHighColor"] = { p.GradientHighColor.r, p.GradientHighColor.g, p.GradientHighColor.b, p.GradientHighColor.a };
    data["padding"] = { p.Padding.x, p.Padding.y, p.Padding.z, p.Padding.w };
    data["usePanelBorderAsPadding"] = p.UsePanelBorderAsPadding;
    data["animate"] = p.Animate;
    data["animationSpeed"] = p.AnimationSpeed;
    data["fillTexture"] = p.FillTexture;
    data["opacity"] = p.Opacity;
    data["visible"] = p.Visible;
    return data;
}

json Serializer::SerializeToggle(const ToggleComponent& t) {
    json data;
    data["isOn"] = t.IsOn;
    data["interactable"] = t.Interactable;
    data["checkmarkSize"] = { t.CheckmarkSize.x, t.CheckmarkSize.y };
    data["checkmarkOffset"] = { t.CheckmarkOffset.x, t.CheckmarkOffset.y };
    data["checkmarkTint"] = { t.CheckmarkTint.r, t.CheckmarkTint.g, t.CheckmarkTint.b, t.CheckmarkTint.a };
    data["offTint"] = { t.OffTint.r, t.OffTint.g, t.OffTint.b, t.OffTint.a };
    data["onTint"] = { t.OnTint.r, t.OnTint.g, t.OnTint.b, t.OnTint.a };
    data["hoverTint"] = { t.HoverTint.r, t.HoverTint.g, t.HoverTint.b, t.HoverTint.a };
    data["disabledTint"] = { t.DisabledTint.r, t.DisabledTint.g, t.DisabledTint.b, t.DisabledTint.a };
    data["groupID"] = t.GroupID;
    data["checkmarkTexture"] = t.CheckmarkTexture;
    data["opacity"] = t.Opacity;
    data["visible"] = t.Visible;
    return data;
}

json Serializer::SerializeScrollView(const ScrollViewComponent& s) {
    json data;
    data["contentSize"] = { s.ContentSize.x, s.ContentSize.y };
    data["horizontalScroll"] = s.HorizontalScroll;
    data["verticalScroll"] = s.VerticalScroll;
    data["scrollSensitivity"] = s.ScrollSensitivity;
    data["showScrollbars"] = s.ShowScrollbars;
    data["scrollbarWidth"] = s.ScrollbarWidth;
    data["scrollbarTrackColor"] = { s.ScrollbarTrackColor.r, s.ScrollbarTrackColor.g, s.ScrollbarTrackColor.b, s.ScrollbarTrackColor.a };
    data["scrollbarThumbColor"] = { s.ScrollbarThumbColor.r, s.ScrollbarThumbColor.g, s.ScrollbarThumbColor.b, s.ScrollbarThumbColor.a };
    data["scrollbarThumbHoverColor"] = { s.ScrollbarThumbHoverColor.r, s.ScrollbarThumbHoverColor.g, s.ScrollbarThumbHoverColor.b, s.ScrollbarThumbHoverColor.a };
    data["useInertia"] = s.UseInertia;
    data["inertiaDeceleration"] = s.InertiaDeceleration;
    data["elastic"] = s.Elastic;
    data["elasticAmount"] = s.ElasticAmount;
    data["scrollbarTrackTexture"] = s.ScrollbarTrackTexture;
    data["scrollbarThumbTexture"] = s.ScrollbarThumbTexture;
    data["opacity"] = s.Opacity;
    data["visible"] = s.Visible;
    return data;
}

json Serializer::SerializeLayoutGroup(const LayoutGroupComponent& l) {
    json data;
    data["direction"] = (int)l.Direction;
    data["padding"] = { l.Padding.x, l.Padding.y, l.Padding.z, l.Padding.w };
    data["spacing"] = l.Spacing;
    data["childAlignment"] = (int)l.ChildAlignment;
    data["crossAlignment"] = (int)l.CrossAlignment;
    data["controlChildWidth"] = l.ControlChildWidth;
    data["controlChildHeight"] = l.ControlChildHeight;
    data["childForceExpandWidth"] = l.ChildForceExpandWidth;
    data["childForceExpandHeight"] = l.ChildForceExpandHeight;
    data["reverseOrder"] = l.ReverseOrder;
    data["columns"] = l.Columns;
    data["rows"] = l.Rows;
    data["cellSize"] = { l.CellSize.x, l.CellSize.y };
    return data;
}

json Serializer::SerializeInputField(const InputFieldComponent& i) {
    json data;
    data["text"] = i.Text;
    data["placeholderText"] = i.PlaceholderText;
    data["maxLength"] = i.MaxLength;
    data["multiline"] = i.Multiline;
    data["readOnly"] = i.ReadOnly;
    data["contentType"] = (int)i.Type;
    data["passwordChar"] = std::string(1, i.PasswordChar);
    data["textColor"] = { i.TextColor.r, i.TextColor.g, i.TextColor.b, i.TextColor.a };
    data["placeholderColor"] = { i.PlaceholderColor.r, i.PlaceholderColor.g, i.PlaceholderColor.b, i.PlaceholderColor.a };
    data["selectionColor"] = { i.SelectionColor.r, i.SelectionColor.g, i.SelectionColor.b, i.SelectionColor.a };
    data["cursorColor"] = { i.CursorColor.r, i.CursorColor.g, i.CursorColor.b, i.CursorColor.a };
    data["cursorWidth"] = i.CursorWidth;
    data["interactable"] = i.Interactable;
    data["opacity"] = i.Opacity;
    data["visible"] = i.Visible;
    return data;
}

json Serializer::SerializeDropdown(const DropdownComponent& d) {
    json data;
    data["options"] = d.Options;
    data["selectedIndex"] = d.SelectedIndex;
    data["interactable"] = d.Interactable;
    data["optionHeight"] = d.OptionHeight;
    data["maxVisibleOptions"] = d.MaxVisibleOptions;
    data["optionNormalColor"] = { d.OptionNormalColor.r, d.OptionNormalColor.g, d.OptionNormalColor.b, d.OptionNormalColor.a };
    data["optionHoverColor"] = { d.OptionHoverColor.r, d.OptionHoverColor.g, d.OptionHoverColor.b, d.OptionHoverColor.a };
    data["optionSelectedColor"] = { d.OptionSelectedColor.r, d.OptionSelectedColor.g, d.OptionSelectedColor.b, d.OptionSelectedColor.a };
    data["showArrow"] = d.ShowArrow;
    data["arrowSize"] = { d.ArrowSize.x, d.ArrowSize.y };
    data["arrowTint"] = { d.ArrowTint.r, d.ArrowTint.g, d.ArrowTint.b, d.ArrowTint.a };
    data["caption"] = d.Caption;
    data["arrowTexture"] = d.ArrowTexture;
    data["opacity"] = d.Opacity;
    data["visible"] = d.Visible;
    return data;
}

json Serializer::SerializeRenderOverrides(const RenderOverridesComponent& ro) {
    json j;
    // Visibility and shadows
    j["visible"] = ro.Visible;
    j["castShadows"] = ro.CastShadows;
    j["receiveShadows"] = ro.ReceiveShadows;
    // Blending and depth
    j["alphaBlend"] = ro.AlphaBlendEnabled;
    j["useAlphaCutout"] = ro.UseAlphaCutout;
    j["alphaCutoutThreshold"] = ro.AlphaCutoutThreshold;
    j["depthWrite"] = ro.DepthWriteEnabled;
    j["depthTest"] = ro.DepthTestEnabled;
    j["showOnTop"] = ro.ShowOnTop;
    // Sorting
    j["sortingOrder"] = ro.SortingOrder;
    // Shader override
    j["shaderOverride"] = ro.ShaderOverrideName;
    return j;
}

void Serializer::DeserializeRenderOverrides(const json& j, RenderOverridesComponent& ro) {
    if (!j.is_object()) return; // Guard against non-object JSON (e.g. null, array)
    // Visibility and shadows (defaults match component defaults)
    ro.Visible = j.value("visible", true);
    ro.CastShadows = j.value("castShadows", true);
    ro.ReceiveShadows = j.value("receiveShadows", true);
    // Blending and depth
    ro.AlphaBlendEnabled = j.value("alphaBlend", false);
    ro.UseAlphaCutout = j.value("useAlphaCutout", false);
    ro.AlphaCutoutThreshold = j.value("alphaCutoutThreshold", 0.5f);
    ro.DepthWriteEnabled = j.value("depthWrite", true);
    ro.DepthTestEnabled = j.value("depthTest", true);
    ro.ShowOnTop = j.value("showOnTop", false);
    // Sorting
    ro.SortingOrder = j.value("sortingOrder", 0);
    // Shader override
    ro.ShaderOverrideName = j.value("shaderOverride", std::string(""));
}

json Serializer::SerializeText(const TextRendererComponent& t) {
    json j;
    j["text"] = t.Text;
    j["pixelSize"] = t.PixelSize;
    j["colorAbgr"] = t.ColorAbgr;
    j["worldSpace"] = t.WorldSpace;
    j["billboard"] = t.Billboard;
    j["anchorToParentUI"] = t.AnchorToParentUI;
    j["anchorEnabled"] = t.AnchorEnabled;
    j["anchor"] = (int)t.Anchor;
    j["anchorOffset"] = { t.AnchorOffset.x, t.AnchorOffset.y };
    j["visible"] = t.Visible;
    j["zOrder"] = t.ZOrder;
    j["opacity"] = t.Opacity;
    j["rectSize"] = { t.RectSize.x, t.RectSize.y };
    j["wordWrap"] = t.WordWrap;
    if (!t.FontPath.empty()) j["fontPath"] = t.FontPath;
    j["textAlignment"] = (int)t.TextAlignment;
    j["outlineEnabled"] = t.OutlineEnabled;
    j["outlineColorAbgr"] = t.OutlineColorAbgr;
    j["outlineThickness"] = t.OutlineThickness;
    j["shadowEnabled"] = t.ShadowEnabled;
    j["shadowColorAbgr"] = t.ShadowColorAbgr;
    j["shadowOffset"] = { t.ShadowOffset.x, t.ShadowOffset.y };
    return j;
}

void Serializer::DeserializeText(const json& j, TextRendererComponent& t) {
    if (j.contains("text")) t.Text = j["text"];
    if (j.contains("pixelSize")) t.PixelSize = j["pixelSize"];
    if (j.contains("colorAbgr")) t.ColorAbgr = j["colorAbgr"];
    if (j.contains("worldSpace")) t.WorldSpace = j["worldSpace"];
    if (j.contains("billboard")) t.Billboard = j["billboard"];
    if (j.contains("anchorToParentUI")) t.AnchorToParentUI = j["anchorToParentUI"];
    if (j.contains("anchorEnabled")) t.AnchorEnabled = j["anchorEnabled"];
    if (j.contains("anchor")) t.Anchor = (UIAnchorPreset)(int)j["anchor"];
    if (j.contains("anchorOffset") && j["anchorOffset"].is_array() && j["anchorOffset"].size() == 2) {
        t.AnchorOffset.x = j["anchorOffset"][0];
        t.AnchorOffset.y = j["anchorOffset"][1];
    }
    if (j.contains("visible")) t.Visible = j["visible"];
    if (j.contains("zOrder")) t.ZOrder = j["zOrder"];
    if (j.contains("opacity")) t.Opacity = j["opacity"];
    if (j.contains("rectSize") && j["rectSize"].is_array() && j["rectSize"].size() == 2) {
        t.RectSize.x = j["rectSize"][0];
        t.RectSize.y = j["rectSize"][1];
    }
    if (j.contains("wordWrap")) t.WordWrap = j["wordWrap"];
    if (j.contains("fontPath")) t.FontPath = j["fontPath"];
    if (j.contains("textAlignment")) t.TextAlignment = (TextRendererComponent::Alignment)(int)j["textAlignment"];
    else if (j.contains("alignment")) t.TextAlignment = (TextRendererComponent::Alignment)(int)j["alignment"];
    if (j.contains("outlineEnabled")) t.OutlineEnabled = j["outlineEnabled"];
    if (j.contains("outlineColorAbgr")) t.OutlineColorAbgr = j["outlineColorAbgr"];
    if (j.contains("outlineThickness")) t.OutlineThickness = j["outlineThickness"];
    if (j.contains("shadowEnabled")) t.ShadowEnabled = j["shadowEnabled"];
    if (j.contains("shadowColorAbgr")) t.ShadowColorAbgr = j["shadowColorAbgr"];
    if (j.contains("shadowOffset") && j["shadowOffset"].is_array() && j["shadowOffset"].size() == 2) {
        t.ShadowOffset.x = j["shadowOffset"][0];
        t.ShadowOffset.y = j["shadowOffset"][1];
    }
}

void Serializer::DeserializeButton(const json& data, ButtonComponent& button) {
    if (data.contains("interactable")) button.Interactable = data["interactable"];
    if (data.contains("toggle")) button.Toggle = data["toggle"];
    if (data.contains("toggled")) button.Toggled = data["toggled"];
    if (data.contains("normalTint") && data["normalTint"].is_array() && data["normalTint"].size() == 4) {
        button.NormalTint.r = data["normalTint"][0];
        button.NormalTint.g = data["normalTint"][1];
        button.NormalTint.b = data["normalTint"][2];
        button.NormalTint.a = data["normalTint"][3];
    }
    if (data.contains("hoverTint") && data["hoverTint"].is_array() && data["hoverTint"].size() == 4) {
        button.HoverTint.r = data["hoverTint"][0];
        button.HoverTint.g = data["hoverTint"][1];
        button.HoverTint.b = data["hoverTint"][2];
        button.HoverTint.a = data["hoverTint"][3];
    }
    if (data.contains("pressedTint") && data["pressedTint"].is_array() && data["pressedTint"].size() == 4) {
        button.PressedTint.r = data["pressedTint"][0];
        button.PressedTint.g = data["pressedTint"][1];
        button.PressedTint.b = data["pressedTint"][2];
        button.PressedTint.a = data["pressedTint"][3];
    }
    if (data.contains("disabledTint") && data["disabledTint"].is_array() && data["disabledTint"].size() == 4) {
        button.DisabledTint.r = data["disabledTint"][0];
        button.DisabledTint.g = data["disabledTint"][1];
        button.DisabledTint.b = data["disabledTint"][2];
        button.DisabledTint.a = data["disabledTint"][3];
    }
    if (data.contains("hoverSound")) data["hoverSound"].get_to(button.HoverSound);
    if (data.contains("clickSound")) data["clickSound"].get_to(button.ClickSound);
}

void Serializer::DeserializeSlider(const json& data, SliderComponent& s) {
    if (data.contains("minValue")) s.MinValue = data["minValue"];
    if (data.contains("maxValue")) s.MaxValue = data["maxValue"];
    if (data.contains("value")) s.Value = data["value"];
    if (data.contains("step")) s.Step = data["step"];
    if (data.contains("direction")) s.SliderDirection = (SliderComponent::Direction)(int)data["direction"];
    if (data.contains("handleSize") && data["handleSize"].is_array() && data["handleSize"].size() == 2) {
        s.HandleSize.x = data["handleSize"][0]; s.HandleSize.y = data["handleSize"][1];
    }
    auto readVec4 = [&](const char* key, glm::vec4& v) {
        if (data.contains(key) && data[key].is_array() && data[key].size() == 4) {
            v.r = data[key][0]; v.g = data[key][1]; v.b = data[key][2]; v.a = data[key][3];
        }
    };
    readVec4("handleNormalTint", s.HandleNormalTint);
    readVec4("handleHoverTint", s.HandleHoverTint);
    readVec4("handlePressedTint", s.HandlePressedTint);
    readVec4("handleDisabledTint", s.HandleDisabledTint);
    if (data.contains("showFill")) s.ShowFill = data["showFill"];
    readVec4("fillColor", s.FillColor);
    if (data.contains("interactable")) s.Interactable = data["interactable"];
    if (data.contains("wholeNumbers")) s.WholeNumbers = data["wholeNumbers"];
    if (data.contains("handleTexture")) data["handleTexture"].get_to(s.HandleTexture);
    if (data.contains("fillTexture")) data["fillTexture"].get_to(s.FillTexture);
    if (data.contains("opacity")) s.Opacity = data["opacity"];
    if (data.contains("visible")) s.Visible = data["visible"];
}

void Serializer::DeserializeProgressBar(const json& data, ProgressBarComponent& p) {
    if (data.contains("value")) p.Value = data["value"];
    if (data.contains("minValue")) p.MinValue = data["minValue"];
    if (data.contains("maxValue")) p.MaxValue = data["maxValue"];
    if (data.contains("direction")) p.Direction = (ProgressBarComponent::FillDirection)(int)data["direction"];
    auto readVec4 = [&](const char* key, glm::vec4& v) {
        if (data.contains(key) && data[key].is_array() && data[key].size() == 4) {
            v.r = data[key][0]; v.g = data[key][1]; v.b = data[key][2]; v.a = data[key][3];
        }
    };
    readVec4("fillColor", p.FillColor);
    if (data.contains("useGradient")) p.UseGradient = data["useGradient"];
    readVec4("gradientLowColor", p.GradientLowColor);
    readVec4("gradientHighColor", p.GradientHighColor);
    if (data.contains("padding") && data["padding"].is_array() && data["padding"].size() == 4) {
        p.Padding.x = data["padding"][0]; p.Padding.y = data["padding"][1];
        p.Padding.z = data["padding"][2]; p.Padding.w = data["padding"][3];
    }
    if (data.contains("usePanelBorderAsPadding")) p.UsePanelBorderAsPadding = data["usePanelBorderAsPadding"];
    if (data.contains("animate")) p.Animate = data["animate"];
    if (data.contains("animationSpeed")) p.AnimationSpeed = data["animationSpeed"];
    if (data.contains("fillTexture")) data["fillTexture"].get_to(p.FillTexture);
    if (data.contains("opacity")) p.Opacity = data["opacity"];
    if (data.contains("visible")) p.Visible = data["visible"];
}

void Serializer::DeserializeToggle(const json& data, ToggleComponent& t) {
    if (data.contains("isOn")) t.IsOn = data["isOn"];
    if (data.contains("interactable")) t.Interactable = data["interactable"];
    if (data.contains("checkmarkSize") && data["checkmarkSize"].is_array() && data["checkmarkSize"].size() == 2) {
        t.CheckmarkSize.x = data["checkmarkSize"][0]; t.CheckmarkSize.y = data["checkmarkSize"][1];
    }
    if (data.contains("checkmarkOffset") && data["checkmarkOffset"].is_array() && data["checkmarkOffset"].size() == 2) {
        t.CheckmarkOffset.x = data["checkmarkOffset"][0]; t.CheckmarkOffset.y = data["checkmarkOffset"][1];
    }
    auto readVec4 = [&](const char* key, glm::vec4& v) {
        if (data.contains(key) && data[key].is_array() && data[key].size() == 4) {
            v.r = data[key][0]; v.g = data[key][1]; v.b = data[key][2]; v.a = data[key][3];
        }
    };
    readVec4("checkmarkTint", t.CheckmarkTint);
    readVec4("offTint", t.OffTint);
    readVec4("onTint", t.OnTint);
    readVec4("hoverTint", t.HoverTint);
    readVec4("disabledTint", t.DisabledTint);
    if (data.contains("groupID")) t.GroupID = data["groupID"];
    if (data.contains("checkmarkTexture")) data["checkmarkTexture"].get_to(t.CheckmarkTexture);
    if (data.contains("opacity")) t.Opacity = data["opacity"];
    if (data.contains("visible")) t.Visible = data["visible"];
}

void Serializer::DeserializeScrollView(const json& data, ScrollViewComponent& s) {
    if (data.contains("contentSize") && data["contentSize"].is_array() && data["contentSize"].size() == 2) {
        s.ContentSize.x = data["contentSize"][0]; s.ContentSize.y = data["contentSize"][1];
    }
    if (data.contains("horizontalScroll")) s.HorizontalScroll = data["horizontalScroll"];
    if (data.contains("verticalScroll")) s.VerticalScroll = data["verticalScroll"];
    if (data.contains("scrollSensitivity")) s.ScrollSensitivity = data["scrollSensitivity"];
    if (data.contains("showScrollbars")) s.ShowScrollbars = data["showScrollbars"];
    if (data.contains("scrollbarWidth")) s.ScrollbarWidth = data["scrollbarWidth"];
    auto readVec4 = [&](const char* key, glm::vec4& v) {
        if (data.contains(key) && data[key].is_array() && data[key].size() == 4) {
            v.r = data[key][0]; v.g = data[key][1]; v.b = data[key][2]; v.a = data[key][3];
        }
    };
    readVec4("scrollbarTrackColor", s.ScrollbarTrackColor);
    readVec4("scrollbarThumbColor", s.ScrollbarThumbColor);
    readVec4("scrollbarThumbHoverColor", s.ScrollbarThumbHoverColor);
    if (data.contains("useInertia")) s.UseInertia = data["useInertia"];
    if (data.contains("inertiaDeceleration")) s.InertiaDeceleration = data["inertiaDeceleration"];
    if (data.contains("elastic")) s.Elastic = data["elastic"];
    if (data.contains("elasticAmount")) s.ElasticAmount = data["elasticAmount"];
    if (data.contains("scrollbarTrackTexture")) data["scrollbarTrackTexture"].get_to(s.ScrollbarTrackTexture);
    if (data.contains("scrollbarThumbTexture")) data["scrollbarThumbTexture"].get_to(s.ScrollbarThumbTexture);
    if (data.contains("opacity")) s.Opacity = data["opacity"];
    if (data.contains("visible")) s.Visible = data["visible"];
}

void Serializer::DeserializeLayoutGroup(const json& data, LayoutGroupComponent& l) {
    if (data.contains("direction")) l.Direction = (LayoutGroupComponent::LayoutDirection)(int)data["direction"];
    if (data.contains("padding") && data["padding"].is_array() && data["padding"].size() == 4) {
        l.Padding.x = data["padding"][0]; l.Padding.y = data["padding"][1];
        l.Padding.z = data["padding"][2]; l.Padding.w = data["padding"][3];
    }
    if (data.contains("spacing")) l.Spacing = data["spacing"];
    if (data.contains("childAlignment")) l.ChildAlignment = (LayoutGroupComponent::Alignment)(int)data["childAlignment"];
    if (data.contains("crossAlignment")) l.CrossAlignment = (LayoutGroupComponent::Alignment)(int)data["crossAlignment"];
    if (data.contains("controlChildWidth")) l.ControlChildWidth = data["controlChildWidth"];
    if (data.contains("controlChildHeight")) l.ControlChildHeight = data["controlChildHeight"];
    if (data.contains("childForceExpandWidth")) l.ChildForceExpandWidth = data["childForceExpandWidth"];
    if (data.contains("childForceExpandHeight")) l.ChildForceExpandHeight = data["childForceExpandHeight"];
    if (data.contains("reverseOrder")) l.ReverseOrder = data["reverseOrder"];
    if (data.contains("columns")) l.Columns = data["columns"];
    if (data.contains("rows")) l.Rows = data["rows"];
    if (data.contains("cellSize") && data["cellSize"].is_array() && data["cellSize"].size() == 2) {
        l.CellSize.x = data["cellSize"][0]; l.CellSize.y = data["cellSize"][1];
    }
}

void Serializer::DeserializeInputField(const json& data, InputFieldComponent& i) {
    if (data.contains("text")) i.Text = data["text"];
    if (data.contains("placeholderText")) i.PlaceholderText = data["placeholderText"];
    if (data.contains("maxLength")) i.MaxLength = data["maxLength"];
    if (data.contains("multiline")) i.Multiline = data["multiline"];
    if (data.contains("readOnly")) i.ReadOnly = data["readOnly"];
    if (data.contains("contentType")) i.Type = (InputFieldComponent::ContentType)(int)data["contentType"];
    if (data.contains("passwordChar")) {
        std::string pc = data["passwordChar"];
        if (!pc.empty()) i.PasswordChar = pc[0];
    }
    auto readVec4 = [&](const char* key, glm::vec4& v) {
        if (data.contains(key) && data[key].is_array() && data[key].size() == 4) {
            v.r = data[key][0]; v.g = data[key][1]; v.b = data[key][2]; v.a = data[key][3];
        }
    };
    readVec4("textColor", i.TextColor);
    readVec4("placeholderColor", i.PlaceholderColor);
    readVec4("selectionColor", i.SelectionColor);
    readVec4("cursorColor", i.CursorColor);
    if (data.contains("cursorWidth")) i.CursorWidth = data["cursorWidth"];
    if (data.contains("interactable")) i.Interactable = data["interactable"];
    if (data.contains("opacity")) i.Opacity = data["opacity"];
    if (data.contains("visible")) i.Visible = data["visible"];
}

void Serializer::DeserializeDropdown(const json& data, DropdownComponent& d) {
    if (data.contains("options") && data["options"].is_array()) {
        d.Options.clear();
        for (const auto& opt : data["options"]) {
            if (opt.is_string()) d.Options.push_back(opt);
        }
    }
    if (data.contains("selectedIndex")) d.SelectedIndex = data["selectedIndex"];
    if (data.contains("interactable")) d.Interactable = data["interactable"];
    if (data.contains("optionHeight")) d.OptionHeight = data["optionHeight"];
    if (data.contains("maxVisibleOptions")) d.MaxVisibleOptions = data["maxVisibleOptions"];
    auto readVec4 = [&](const char* key, glm::vec4& v) {
        if (data.contains(key) && data[key].is_array() && data[key].size() == 4) {
            v.r = data[key][0]; v.g = data[key][1]; v.b = data[key][2]; v.a = data[key][3];
        }
    };
    readVec4("optionNormalColor", d.OptionNormalColor);
    readVec4("optionHoverColor", d.OptionHoverColor);
    readVec4("optionSelectedColor", d.OptionSelectedColor);
    if (data.contains("showArrow")) d.ShowArrow = data["showArrow"];
    if (data.contains("arrowSize") && data["arrowSize"].is_array() && data["arrowSize"].size() == 2) {
        d.ArrowSize.x = data["arrowSize"][0]; d.ArrowSize.y = data["arrowSize"][1];
    }
    readVec4("arrowTint", d.ArrowTint);
    if (data.contains("caption")) d.Caption = data["caption"];
    if (data.contains("arrowTexture")) data["arrowTexture"].get_to(d.ArrowTexture);
    if (data.contains("opacity")) d.Opacity = data["opacity"];
    if (data.contains("visible")) d.Visible = data["visible"];
}

json Serializer::SerializeUIRect(const UIRectComponent& rect) {
    json data;
    data["anchorToParent"] = rect.AnchorToParent;
    data["horizontalAnchor"] = rect.HorizontalAnchor;
    data["verticalAnchor"] = rect.VerticalAnchor;
    data["pivot"] = { rect.Pivot.x, rect.Pivot.y };
    data["offset"] = { rect.Offset.x, rect.Offset.y };
    data["size"] = { rect.Size.x, rect.Size.y };
    return data;
}

void Serializer::DeserializeUIRect(const json& data, UIRectComponent& rect) {
    if (data.contains("anchorToParent")) rect.AnchorToParent = data["anchorToParent"];
    if (data.contains("horizontalAnchor")) rect.HorizontalAnchor = data["horizontalAnchor"];
    if (data.contains("verticalAnchor")) rect.VerticalAnchor = data["verticalAnchor"];
    if (data.contains("pivot") && data["pivot"].is_array() && data["pivot"].size() == 2) {
        rect.Pivot.x = data["pivot"][0];
        rect.Pivot.y = data["pivot"][1];
    }
    if (data.contains("offset") && data["offset"].is_array() && data["offset"].size() == 2) {
        rect.Offset.x = data["offset"][0];
        rect.Offset.y = data["offset"][1];
    }
    if (data.contains("size") && data["size"].is_array() && data["size"].size() == 2) {
        rect.Size.x = data["size"][0];
        rect.Size.y = data["size"][1];
    }
}

json Serializer::SerializeFitToContent(const FitToContentComponent& ftc) {
    json data;
    data["enabled"] = ftc.Enabled;
    data["fitWidth"] = ftc.FitWidth;
    data["fitHeight"] = ftc.FitHeight;
    data["padding"] = { ftc.Padding.x, ftc.Padding.y, ftc.Padding.z, ftc.Padding.w };
    data["minSize"] = { ftc.MinSize.x, ftc.MinSize.y };
    data["maxSize"] = { ftc.MaxSize.x, ftc.MaxSize.y };
    data["directChildrenOnly"] = ftc.DirectChildrenOnly;
    return data;
}

void Serializer::DeserializeFitToContent(const json& data, FitToContentComponent& ftc) {
    if (data.contains("enabled")) ftc.Enabled = data["enabled"];
    if (data.contains("fitWidth")) ftc.FitWidth = data["fitWidth"];
    if (data.contains("fitHeight")) ftc.FitHeight = data["fitHeight"];
    if (data.contains("padding") && data["padding"].is_array() && data["padding"].size() == 4) {
        ftc.Padding.x = data["padding"][0];
        ftc.Padding.y = data["padding"][1];
        ftc.Padding.z = data["padding"][2];
        ftc.Padding.w = data["padding"][3];
    }
    if (data.contains("minSize") && data["minSize"].is_array() && data["minSize"].size() == 2) {
        ftc.MinSize.x = data["minSize"][0];
        ftc.MinSize.y = data["minSize"][1];
    }
    if (data.contains("maxSize") && data["maxSize"].is_array() && data["maxSize"].size() == 2) {
        ftc.MaxSize.x = data["maxSize"][0];
        ftc.MaxSize.y = data["maxSize"][1];
    }
    if (data.contains("directChildrenOnly")) ftc.DirectChildrenOnly = data["directChildrenOnly"];
}

json Serializer::SerializeUISceneCapture(const UISceneCaptureComponent& cap) {
    json data;
    data["enabled"] = cap.Enabled;
    data["autoFrame"] = cap.AutoFrame;
    data["includeChildren"] = cap.IncludeChildren;
    data["boundsPadding"] = cap.BoundsPadding;
    data["fieldOfView"] = cap.FieldOfView;
    data["nearClip"] = cap.NearClip;
    data["farClip"] = cap.FarClip;
    data["viewDirection"] = { cap.ViewDirection.x, cap.ViewDirection.y, cap.ViewDirection.z };
    data["upDirection"] = { cap.UpDirection.x, cap.UpDirection.y, cap.UpDirection.z };
    data["focusOffset"] = { cap.FocusOffset.x, cap.FocusOffset.y, cap.FocusOffset.z };
    data["lockViewToTarget"] = cap.LockViewToTarget;
    data["targetEntity"] = cap.TargetEntity;
    data["targetGuidHigh"] = cap.TargetGuidHigh;
    data["targetGuidLow"] = cap.TargetGuidLow;
    data["renderWidth"] = cap.RenderWidth;
    data["renderHeight"] = cap.RenderHeight;
    data["clearColor"] = cap.ClearColor;
    data["showGrid"] = cap.ShowGrid;
    return data;
}

void Serializer::DeserializeUISceneCapture(const json& data, UISceneCaptureComponent& cap) {
    if (data.contains("enabled")) cap.Enabled = data["enabled"];
    if (data.contains("autoFrame")) cap.AutoFrame = data["autoFrame"];
    if (data.contains("includeChildren")) cap.IncludeChildren = data["includeChildren"];
    if (data.contains("boundsPadding")) cap.BoundsPadding = data["boundsPadding"];
    if (data.contains("fieldOfView")) cap.FieldOfView = data["fieldOfView"];
    if (data.contains("nearClip")) cap.NearClip = data["nearClip"];
    if (data.contains("farClip")) cap.FarClip = data["farClip"];
    if (data.contains("viewDirection") && data["viewDirection"].is_array() && data["viewDirection"].size() == 3) {
        cap.ViewDirection.x = data["viewDirection"][0];
        cap.ViewDirection.y = data["viewDirection"][1];
        cap.ViewDirection.z = data["viewDirection"][2];
    }
    if (data.contains("upDirection") && data["upDirection"].is_array() && data["upDirection"].size() == 3) {
        cap.UpDirection.x = data["upDirection"][0];
        cap.UpDirection.y = data["upDirection"][1];
        cap.UpDirection.z = data["upDirection"][2];
    }
    if (data.contains("focusOffset") && data["focusOffset"].is_array() && data["focusOffset"].size() == 3) {
        cap.FocusOffset.x = data["focusOffset"][0];
        cap.FocusOffset.y = data["focusOffset"][1];
        cap.FocusOffset.z = data["focusOffset"][2];
    }
    if (data.contains("lockViewToTarget")) cap.LockViewToTarget = data["lockViewToTarget"];
    if (data.contains("targetEntity")) cap.TargetEntity = data["targetEntity"];
    if (data.contains("targetGuidHigh")) cap.TargetGuidHigh = data["targetGuidHigh"];
    if (data.contains("targetGuidLow")) cap.TargetGuidLow = data["targetGuidLow"];
    if (data.contains("renderWidth")) cap.RenderWidth = data["renderWidth"];
    if (data.contains("renderHeight")) cap.RenderHeight = data["renderHeight"];
    if (data.contains("clearColor")) cap.ClearColor = data["clearColor"];
    if (data.contains("showGrid")) cap.ShowGrid = data["showGrid"];
}

void Serializer::ReleasePBTextures(Scene& scene) {
    // NOTE: Do NOT destroy texture handles - they're shared via global texture cache
    // (s_textureCache in MaterialCache.cpp). Destroying them here would invalidate
    // cached handles, causing crashes when other entities/materials use the same textures.
    // Just clear the property block maps without destroying the underlying textures.
    for (const auto& e : scene.GetEntities()) {
        auto* d = scene.GetEntityData(e.GetID()); if (!d) continue;
        if (d->Mesh) {
            d->Mesh->PropertyBlock.Clear();
            for (auto& pb : d->Mesh->SlotPropertyBlocks) {
                pb.Clear();
            }
        }
        if (d->MeshProxy) {
            d->MeshProxy->PropertyBlock.Clear();
            for (auto& pb : d->MeshProxy->SlotPropertyBlocks) {
                pb.Clear();
            }
        }
    }
}

// ---------------- Animator (AnimationPlayerComponent) ----------------
json Serializer::SerializeAnimator(const cm::animation::AnimationPlayerComponent& a)
{
    json j;
    // Note: "mode" field removed - all animations now go through controller infrastructure
    // Old "player" mode scenes will use auto-generated controllers based on singleClipPath
    j["playbackSpeed"] = a.PlaybackSpeed;
    
    // Root motion target configuration (where to route extracted motion)
    j["motionTarget"] = static_cast<int>(a.MotionTarget);
    if (a.ExplicitTargetEntityId != INVALID_ENTITY_ID) {
        j["explicitTargetEntity"] = static_cast<int>(a.ExplicitTargetEntityId);
    }
    
    // Write paths as VFS-relative paths for portability
    // If path is already VFS-relative (starts with "assets/"), keep as-is
    // Otherwise try to extract VFS path or make relative
    auto makeVFSPath = [](const std::string& p)->std::string{
        if (p.empty()) return p;
        std::string normalized = IVirtualFS::NormalizePath(p);
        
        // If already a VFS path (starts with "assets/"), keep it
        if (normalized.find("assets/") == 0) {
            return normalized;
        }
        
        // Look for "/assets/" in the path and extract from there
        size_t assetsPos = normalized.find("/assets/");
        if (assetsPos != std::string::npos) {
            return normalized.substr(assetsPos + 1); // Skip the leading '/'
        }
        
        // If path is relative and doesn't start with "../", keep it
        fs::path pp(normalized);
        if (!pp.is_absolute() && normalized.find("../") == std::string::npos) {
            return normalized;
        }
        
        // Try to make relative to project directory
        try {
            fs::path base = Project::GetProjectDirectory();
            if (!base.empty() && pp.is_absolute()) {
                std::error_code ec;
                fs::path rel = fs::relative(pp, base, ec);
                if (!ec) {
                    std::string relStr = IVirtualFS::NormalizePath(rel.string());
                    // Only use if it doesn't go up directories
                    if (relStr.find("../") == std::string::npos) {
                        return relStr;
                    }
                }
            }
        } catch(...) {}
        
        return normalized;
    };
    j["controllerPath"] = makeVFSPath(a.ControllerPath);
    j["controllerOverridePath"] = makeVFSPath(a.ControllerOverridePath);
    j["singleClipPath"] = makeVFSPath(a.SingleClipPath);
    j["playOnStart"] = a.PlayOnStart;
    j["crowdThrottleEnabled"] = a.CrowdThrottleEnabled;
    j["lodEnabled"] = a.LODEnabled;
    j["lodNearDistance"] = a.LODNearDistance;
    j["lodMediumDistance"] = a.LODMediumDistance;
    j["lodFarDistance"] = a.LODFarDistance;
    j["lodMediumInterval"] = a.LODMediumInterval;
    j["lodFarInterval"] = a.LODFarInterval;
    j["lodVeryFarInterval"] = a.LODVeryFarInterval;
    j["offscreenDormancyEnabled"] = a.OffscreenDormancyEnabled;
    j["offscreenNearInterval"] = a.OffscreenNearInterval;
    j["offscreenMediumInterval"] = a.OffscreenMediumInterval;
    j["offscreenFarInterval"] = a.OffscreenFarInterval;
    j["offscreenVeryFarInterval"] = a.OffscreenVeryFarInterval;
    j["loop"] = (!a.ActiveStates.empty() ? a.ActiveStates.front().Loop : true);
    return j;
}

void Serializer::DeserializeAnimator(const json& j, cm::animation::AnimationPlayerComponent& a)
{
    if (!j.is_object()) return; // Guard against non-object JSON
    // Note: "mode" field is now ignored - all animations go through controller infrastructure
    // Old "player" mode scenes will have SingleClipPath set and auto-generate a controller at runtime
    a.PlaybackSpeed = j.value("playbackSpeed", 1.0f);
    
    // Root motion target configuration
    a.MotionTarget = static_cast<cm::animation::RootMotionTarget>(
        j.value("motionTarget", static_cast<int>(cm::animation::RootMotionTarget::FindCharacterController))
    );
    a.ExplicitTargetEntityId = static_cast<EntityID>(j.value("explicitTargetEntity", static_cast<int>(INVALID_ENTITY_ID)));
    
    // Read and normalize VFS paths - they should already be VFS-relative
    auto normalizeVFSPath = [](const std::string& p)->std::string{
        if (p.empty()) return p;
        std::string normalized = IVirtualFS::NormalizePath(p);
        
        // If path has weird relative components (../), try to fix it
        if (normalized.find("../") != std::string::npos) {
            // Look for "/assets/" and extract from there
            size_t assetsPos = normalized.find("/assets/");
            if (assetsPos != std::string::npos) {
                return normalized.substr(assetsPos + 1);
            }
            // Look for "assets/" pattern
            assetsPos = normalized.find("assets/");
            if (assetsPos != std::string::npos && assetsPos > 0) {
                return normalized.substr(assetsPos);
            }
        }
        
        return normalized;
    };
    a.ControllerPath = normalizeVFSPath(j.value("controllerPath", ""));
    a.ControllerOverridePath = normalizeVFSPath(j.value("controllerOverridePath", ""));
    a.SingleClipPath = normalizeVFSPath(j.value("singleClipPath", ""));
    a.PlayOnStart = j.value("playOnStart", true);
    a.CrowdThrottleEnabled = j.value("crowdThrottleEnabled", true);
    a.LODEnabled = j.value("lodEnabled", true);
    a.LODNearDistance = j.value("lodNearDistance", 25.0f);
    a.LODMediumDistance = j.value("lodMediumDistance", 55.0f);
    a.LODFarDistance = j.value("lodFarDistance", 110.0f);
    a.LODMediumInterval = j.value("lodMediumInterval", 0.03333334f);
    a.LODFarInterval = j.value("lodFarInterval", 0.06666667f);
    a.LODVeryFarInterval = j.value("lodVeryFarInterval", 0.13333334f);
    a.OffscreenDormancyEnabled = j.value("offscreenDormancyEnabled", true);
    a.OffscreenNearInterval = j.value("offscreenNearInterval", 0.20000000f);
    a.OffscreenMediumInterval = j.value("offscreenMediumInterval", 0.33333334f);
    a.OffscreenFarInterval = j.value("offscreenFarInterval", 0.50000000f);
    a.OffscreenVeryFarInterval = j.value("offscreenVeryFarInterval", 1.00000000f);
    if (a.ActiveStates.empty()) a.ActiveStates.push_back({});
    a.ActiveStates.front().Loop = j.value("loop", true);
    a.IsPlaying = false;
    a._InitApplied = false;
    a.InvalidateTargetCache();  // Clear cached target on load
    cm::animation::PreloadAnimatorComponent(a);
}

json Serializer::SerializeScripts(Scene& scene, const std::vector<ScriptInstance>& scripts) {
    // Helper to serialize a single entity reference with full resolution info
    // Use BuildEntityReferenceJson to ensure modelRootGuid is included for multi-instance disambiguation
    auto serializeEntityRef = [&](int entityId) -> json {
        if (entityId < 0) {
            return ""; // Invalid/unassigned entity
        }
        json ref = BuildEntityReferenceJson(scene, entityId);
        return ref.empty() ? json("") : ref;
    };
    
    // Helper to check if a type is entity-like
    auto isEntityLikeType = [](PropertyType t) -> bool {
        return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
    };

    json scriptArray = json::array();
    for (const auto& script : scripts) {
        json scriptData;
        scriptData["className"] = script.ClassName;
        // Persist per-entity overrides for reflected fields
        if (!script.Values.empty()) {
            json props = json::object();
            bool any = false;
            for (const auto& kv : script.Values) {
                const std::string& name = kv.first;
                const PropertyValue& v = kv.second;

                // Prefer reflection metadata when available to determine property kind;
                // if metadata is missing (e.g., during a script reload), fall back to
                // treating int-valued fields as entity references and stringify others.
                bool hasMeta = ScriptReflection::HasProperties(script.ClassName);
                PropertyType t = PropertyType::Int;
                PropertyType listElemType = PropertyType::Int;
                bool isEntityLike = false;
                bool isList = false;
                bool isListWithEntityElements = false;
                
                if (hasMeta) {
                    auto& propsMeta = ScriptReflection::GetScriptProperties(script.ClassName);
                    auto pit = std::find_if(propsMeta.begin(), propsMeta.end(), [&](const PropertyInfo& p){ return p.name == name; });
                    if (pit == propsMeta.end()) {
                        // Metadata temporarily missing for this field (e.g., hot reload);
                        // preserve the value by falling back to type inference.
                        isEntityLike = std::holds_alternative<int>(v);
                        // For lists without metadata, try to infer entity elements from list data
                        if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(v)) {
                            auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(v);
                            if (listPtr) {
                                isList = true;
                                isListWithEntityElements = isEntityLikeType(listPtr->elementType);
                            }
                        }
                    } else {
                        t = pit->type;
                        isEntityLike = isEntityLikeType(t);
                        isList = (t == PropertyType::List);
                        if (isList) {
                            listElemType = pit->listElementType;
                            isListWithEntityElements = isEntityLikeType(listElemType);
                        }
                    }
                } else {
                    // No metadata: infer entity-like by variant holding an int
                    isEntityLike = std::holds_alternative<int>(v);
                    // For lists without metadata, try to infer from list data
                    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(v)) {
                        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(v);
                        if (listPtr) {
                            isList = true;
                            isListWithEntityElements = isEntityLikeType(listPtr->elementType);
                        }
                    }
                }

                if (isList && isListWithEntityElements) {
                    // Serialize list of entity references with full resolution info
                    json listJson = json::array();
                    
                    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(v)) {
                        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(v);
                        if (listPtr) {
                            for (const auto& elem : listPtr->elements) {
                                if (std::holds_alternative<int>(elem)) {
                                    int entityId = std::get<int>(elem);
                                    listJson.push_back(serializeEntityRef(entityId));
                                } else if (std::holds_alternative<std::string>(elem)) {
                                    // String element - might be a GUID or serialized entity ID
                                    const std::string& str = std::get<std::string>(elem);
                                    try {
                                        int entityId = std::stoi(str);
                                        listJson.push_back(serializeEntityRef(entityId));
                                    } catch (...) {
                                        // Not a number, preserve as-is
                                        listJson.push_back(str);
                                    }
                                } else {
                                    // Other element types - serialize to string
                                    listJson.push_back(ScriptReflection::PropertyValueToString(elem));
                                }
                            }
                        }
                    } else if (std::holds_alternative<std::string>(v)) {
                        // List was serialized as pipe-separated string - parse and re-serialize
                        const std::string& serialized = std::get<std::string>(v);
                        if (!serialized.empty()) {
                            std::istringstream iss(serialized);
                            std::string part;
                            while (std::getline(iss, part, '|')) {
                                if (part.empty()) {
                                    listJson.push_back("");
                                    continue;
                                }
                                try {
                                    int entityId = std::stoi(part);
                                    listJson.push_back(serializeEntityRef(entityId));
                                } catch (...) {
                                    listJson.push_back(part);
                                }
                            }
                        }
                    }
                    
                    json listWrapper;
                    listWrapper["__entityList"] = true;
                    listWrapper["elementType"] = static_cast<int>(listElemType);
                    listWrapper["elements"] = std::move(listJson);
                    props[name] = std::move(listWrapper);
                    any = true;
                } else if (isEntityLike && std::holds_alternative<int>(v)) {
                    int id = std::get<int>(v);
                    props[name] = serializeEntityRef(id);
                    any = true;
                } else {
                    // Non-entity or non-int values: serialize to string form
                    props[name] = ScriptReflection::PropertyValueToString(v);
                    any = true;
                }
            }
            if (!props.empty() && any) {
                scriptData["properties"] = std::move(props);
            }
        }
        scriptArray.push_back(scriptData);
    }
    return scriptArray;
}

void Serializer::DeserializeScripts(const json& data, std::vector<ScriptInstance>& scripts, Scene& scene, bool createInstances) {
    scripts.clear();
    if (data.is_array()) {
        for (const auto& scriptData : data) {
            if (scriptData.contains("className")) {
                ScriptInstance instance;
                instance.ClassName = scriptData["className"];
                
                bool keepInstance = true;
                if (createInstances) {
                    // Create the script instance
                    auto created = ScriptSystem::Instance().Create(instance.ClassName);
                    if (created) {
                        instance.Instance = created;
                    } else {
                        std::cerr << "[Serializer] Failed to create script of type '" << instance.ClassName << "'\n";
                        keepInstance = false;
                    }
                }
                
                if (!keepInstance) continue;
                
                // Load persisted overrides; prefer metadata but fall back to heuristics for entity refs
                if (scriptData.contains("properties") && scriptData["properties"].is_object()) {
                    const auto& propsJson = scriptData["properties"];
                    const bool hasMeta = ScriptReflection::HasProperties(instance.ClassName);
                    std::vector<PropertyInfo>* propsMeta = hasMeta ? &ScriptReflection::GetScriptProperties(instance.ClassName) : nullptr;
                    
                    auto fillEntityMetaFromJson = [&](const nlohmann::json& refJson, ScriptEntityRefMetadata& meta, EntityID resolved) {
                        meta.entityId = (resolved == (EntityID)-1) ? -1 : static_cast<int32_t>(resolved);
                        if (refJson.is_object()) {
                            if (refJson.contains("guid")) {
                                try { meta.guid = ClaymoreGUID::FromString(refJson["guid"].get<std::string>()); } catch(...) {}
                            }
                            if (refJson.contains("modelGuid")) {
                                try { meta.modelGuid = ClaymoreGUID::FromString(refJson["modelGuid"].get<std::string>()); } catch(...) {}
                            }
                            if (refJson.contains("modelRootGuid")) {
                                try { meta.modelRootGuid = ClaymoreGUID::FromString(refJson["modelRootGuid"].get<std::string>()); } catch(...) {}
                            }
                            if (refJson.contains("modelNodePath")) {
                                try { meta.modelNodePath = refJson["modelNodePath"].get<std::string>(); } catch(...) {}
                            }
                        } else if (refJson.is_string()) {
                            try { meta.guid = ClaymoreGUID::FromString(refJson.get<std::string>()); } catch(...) {}
                        }
                    };
                    
                    auto isEntityLikeType = [](PropertyType t) -> bool {
                        return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
                    };
                    
                    for (auto it = propsJson.begin(); it != propsJson.end(); ++it) {
                        const std::string key = it.key();
                        const nlohmann::json& value = it.value();
                        const PropertyInfo* meta = nullptr;
                        PropertyType declaredType = PropertyType::Int;
                        PropertyType listElemType = PropertyType::Int;
                        bool entityLike = false;
                        bool isList = false;
                        
                        if (propsMeta) {
                            auto& metaVec = *propsMeta;
                            auto pit = std::find_if(metaVec.begin(), metaVec.end(), [&](const PropertyInfo& p){ return p.name == key; });
                            if (pit != metaVec.end()) {
                                meta = &(*pit);
                                declaredType = pit->type;
                                entityLike = isEntityLikeType(declaredType);
                                isList = (declaredType == PropertyType::List);
                                if (isList) {
                                    listElemType = pit->listElementType;
                                }
                            } else {
                                static std::unordered_set<std::string> s_UnknownScriptProps;
                                std::string propKey = instance.ClassName + "::" + key;
                                if (s_UnknownScriptProps.insert(propKey).second) {
                                    std::cerr << "[Serializer] Unknown script property '" << key
                                              << "' on script '" << instance.ClassName << "'\n";
                                }
                                entityLike = LooksLikeEntityReferenceJson(value);
                            }
                        } else {
                            entityLike = LooksLikeEntityReferenceJson(value);
                        }

                        // Check for entity list format (new format with __entityList marker)
                        bool isEntityListFormat = value.is_object() && value.contains("__entityList") && 
                                                  value["__entityList"].get<bool>() == true;
                        
                        if (isEntityListFormat) {
                            // Deserialize entity list with full reference resolution
                            auto listPtr = std::make_shared<ListPropertyValue>();
                            
                            // Get element type from JSON or metadata
                            if (value.contains("elementType")) {
                                listPtr->elementType = static_cast<PropertyType>(value["elementType"].get<int>());
                            } else if (meta && isList) {
                                listPtr->elementType = listElemType;
                            }
                            
                            bool listElemEntityLike = isEntityLikeType(listPtr->elementType);
                            if (listElemEntityLike && value.contains("elements") && value["elements"].is_array()) {
                                listPtr->entityRefs.resize(value["elements"].size());
                            }
                            
                            if (value.contains("elements") && value["elements"].is_array()) {
                                for (size_t i = 0; i < value["elements"].size(); ++i) {
                                    const auto& elem = value["elements"][i];
                                    // Each element is either an entity reference object or empty string
                                    if (elem.is_object() || (elem.is_string() && LooksLikeEntityReferenceJson(elem))) {
                                        EntityID resolved = ResolveEntityReferenceJson(elem, scene);
                                        listPtr->elements.push_back((int)(resolved == (EntityID)-1 ? -1 : resolved));
                                        if (listElemEntityLike && i < listPtr->entityRefs.size()) {
                                            ScriptEntityRefMetadata elemMeta;
                                            fillEntityMetaFromJson(elem, elemMeta, resolved);
                                            listPtr->entityRefs[i] = elemMeta;
                                        }
                                    } else if (elem.is_string()) {
                                        // Empty string or non-entity string
                                        const std::string& str = elem.get<std::string>();
                                        if (str.empty()) {
                                            listPtr->elements.push_back(-1);
                                            if (listElemEntityLike && i < listPtr->entityRefs.size()) {
                                                listPtr->entityRefs[i].entityId = -1;
                                            }
                                        } else {
                                            // Try to parse as int (legacy format)
                                            try {
                                                int parsed = std::stoi(str);
                                                listPtr->elements.push_back(parsed);
                                                if (listElemEntityLike && i < listPtr->entityRefs.size()) {
                                                    listPtr->entityRefs[i].entityId = parsed;
                                                }
                                            } catch (...) {
                                                listPtr->elements.push_back(-1);
                                                if (listElemEntityLike && i < listPtr->entityRefs.size()) {
                                                    listPtr->entityRefs[i].entityId = -1;
                                                }
                                            }
                                        }
                                    } else if (elem.is_number_integer()) {
                                        int parsed = elem.get<int>();
                                        listPtr->elements.push_back(parsed);
                                        if (listElemEntityLike && i < listPtr->entityRefs.size()) {
                                            listPtr->entityRefs[i].entityId = parsed;
                                        }
                                    }
                                }
                            }
                            
                            instance.Values[key] = listPtr;
                            continue;
                        }
                        
                        if (entityLike) {
                            EntityID resolved = ResolveEntityReferenceJson(value, scene);
                            instance.Values[key] = (int)((resolved == (EntityID)-1) ? -1 : resolved);
                            
                            // Preserve metadata for parity with binary path
                            ScriptEntityRefMetadata metaValue;
                            fillEntityMetaFromJson(value, metaValue, resolved);
                            if (metaValue.entityId != -1 || metaValue.guid.high != 0 || metaValue.guid.low != 0 ||
                                metaValue.modelGuid.high != 0 || metaValue.modelGuid.low != 0 ||
                                metaValue.modelRootGuid.high != 0 || metaValue.modelRootGuid.low != 0 ||
                                !metaValue.modelNodePath.empty()) {
                                instance.EntityRefMetadata[key] = metaValue;
                            }
                            
                            continue;
                        }

                        if (meta) {
                            try {
                                if (value.is_string()) {
                                    instance.Values[key] = ScriptReflection::StringToPropertyValue(value.get<std::string>(), declaredType);
                                } else if (value.is_number_integer() && declaredType == PropertyType::Int) {
                                    instance.Values[key] = value.get<int>();
                                } else if (value.is_number_float() && declaredType == PropertyType::Float) {
                                    instance.Values[key] = value.get<float>();
                                } else if (value.is_boolean() && declaredType == PropertyType::Bool) {
                                    instance.Values[key] = value.get<bool>();
                                } else {
                                    instance.Values[key] = ScriptReflection::StringToPropertyValue(value.dump(), declaredType);
                                }
                            } catch(...) {}
                        }
                    }
                }
                scripts.push_back(instance);
            }
        }
    }
}

// Entity serialization
json Serializer::SerializeEntity(EntityID id, Scene& scene) {
   EntityData* entityData = scene.GetEntityData(id);  // <-- This is the correct call
   if (!entityData) return json{};

   json data;
   data["id"] = id;
   data["name"] = entityData->Name;
   data["layer"] = entityData->Layer;
   data["tag"] = entityData->Tag;
   data["visible"] = entityData->Visible;
   data["active"] = entityData->Active;
   data["parent"] = entityData->Parent;
   data["children"] = entityData->Children;
   // Stable GUIDs and prefab link
   data["guid"] = entityData->EntityGuid;
   
   // New unified prefab instance component
   if (entityData->PrefabInstance) {
       data["prefabInstance"] = SerializePrefabInstance(*entityData->PrefabInstance);
   }
   
   // Legacy prefab fields (kept for backward compatibility during migration)
   if (!(entityData->PrefabGuid.high == 0 && entityData->PrefabGuid.low == 0)) {
       data["prefabGuid"] = entityData->PrefabGuid;
   }
   if (!entityData->PrefabSource.empty()) {
       std::string v = IVirtualFS::NormalizePath(entityData->PrefabSource);
       auto pos = v.find("assets/");
       if (pos != std::string::npos) v = v.substr(pos);
       data["prefabSource"] = v;
   }
   // Model asset identity (GUID only, path resolved by Editor via AssetLibrary)
   if (!(entityData->ModelAssetGuid.high == 0 && entityData->ModelAssetGuid.low == 0)) {
       data["modelAssetGuid"] = entityData->ModelAssetGuid;
   }
   
   // Model node deletions (user-intentional deletions that should persist across reimports)
   if (!entityData->DeletedModelNodes.empty()) {
      json deletedNodes = json::array();
      for (const auto& path : entityData->DeletedModelNodes) {
          if (!path.empty()) {
              deletedNodes.push_back(path);
          }
      }
      if (!deletedNodes.empty()) {
          data["deletedModelNodes"] = std::move(deletedNodes);
      }
   }

   // Serialize components
   data["transform"] = SerializeTransform(entityData->Transform);

   if (entityData->Mesh) {
      data["mesh"] = SerializeMesh(*entityData->Mesh);
      }
   if (entityData->MeshProxy) {
      data["meshProxy"] = SerializeMeshProxy(*entityData->MeshProxy, scene);
   }

   if (entityData->Light) {
      data["light"] = SerializeLight(*entityData->Light);
      }

   // Skeleton & Skinning
   if (entityData->Skeleton) {
       data["skeleton"] = SerializeSkeleton(*entityData->Skeleton);
   }
   if (entityData->Skinning) {
       json skinningJson = SerializeSkinning(*entityData->Skinning);
       EntityID skeletonRoot = entityData->Skinning->SkeletonRoot;
       if (skeletonRoot != INVALID_ENTITY_ID &&
           skeletonRoot != static_cast<EntityID>(-1) &&
           scene.GetEntityData(skeletonRoot)) {
           skinningJson["skeletonRootRef"] = BuildEntityReferenceJson(scene, skeletonRoot);
       }
       data["skinning"] = std::move(skinningJson);
   }
   
   // Bone attachment
   if (entityData->BoneAttachment) {
       data["boneAttachment"] = SerializeBoneAttachment(*entityData->BoneAttachment);
   }

   // Blend shape weights (only weights, geometry comes from mesh file)
   if (entityData->BlendShapes && !entityData->BlendShapes->Shapes.empty()) {
       json bsWeights = SerializeBlendShapeWeights(*entityData->BlendShapes);
       if (!bsWeights.empty()) {
           data["blendShapeWeights"] = std::move(bsWeights);
       }
   }

   // Unified morph weights (aggregated blend shapes at model root)
   if (entityData->UnifiedMorph && !entityData->UnifiedMorph->Names.empty()) {
       json umWeights = SerializeUnifiedMorphWeights(*entityData->UnifiedMorph);
       if (!umWeights.empty()) {
           data["unifiedMorphWeights"] = std::move(umWeights);
       }
   }

   // TintMaskController
   if (entityData->TintController) {
       data["tintController"] = SerializeTintController(*entityData->TintController);
   }

   if (entityData->Collider) {
      data["collider"] = SerializeCollider(*entityData->Collider);
      }

   if (entityData->RigidBody) {
      data["rigidbody"] = SerializeRigidBody(*entityData->RigidBody);
      }

   if (entityData->StaticBody) {
      data["staticbody"] = SerializeStaticBody(*entityData->StaticBody);
      }

   if (entityData->Softbody) {
      data["softbody"] = SerializeSoftbody(*entityData->Softbody);
      }

   if (entityData->CharacterController) {
      data["characterController"] = SerializeCharacterController(*entityData->CharacterController);
   }

   if (entityData->GrassDeformer) {
      data["grassDeformer"] = SerializeGrassDeformer(*entityData->GrassDeformer);
   }

    // Serialize scripts
   if (!entityData->Scripts.empty()) {
      data["scripts"] = SerializeScripts(scene, entityData->Scripts);
      }

    // Animator
    if (entityData->AnimationPlayer) {
        data["animator"] = SerializeAnimator(*entityData->AnimationPlayer);
    }

   if (entityData->Camera) {
       data["camera"] = SerializeCamera(*entityData->Camera);
   }
   if (entityData->AudioSource) {
       data["audioSource"] = SerializeAudioSource(*entityData->AudioSource);
   }
   if (entityData->AudioListener) {
       data["audioListener"] = SerializeAudioListener(*entityData->AudioListener);
   }
   if (entityData->Terrain) {
       data["terrain"] = SerializeTerrain(*entityData->Terrain);
   }
   if (entityData->ResourceLayers) {
       data["resourceLayers"] = SerializeResourceLayers(*entityData->ResourceLayers);
   }
   if (entityData->Instancer) {
       data["instancer"] = SerializeInstancer(*entityData->Instancer);
   }
   if (entityData->River) {
       data["river"] = SerializeRiver(*entityData->River);
   }
   if (entityData->Spline) {
       data["spline"] = SerializeSpline(*entityData->Spline);
   }
   if (entityData->Emitter) {
       data["emitter"] = SerializeParticleEmitter(*entityData->Emitter);
   }

   // Area
   if (entityData->Area) {
       data["area"] = Serializer::SerializeArea(*entityData->Area);
   }

   // UI Components
   if (entityData->Canvas) {
       data["canvas"] = SerializeCanvas(*entityData->Canvas);
   }
   if (entityData->Panel) {
       data["panel"] = SerializePanel(*entityData->Panel);
   }
   if (entityData->Button) {
       data["button"] = SerializeButton(*entityData->Button);
   }
   if (entityData->Slider) {
       data["slider"] = SerializeSlider(*entityData->Slider);
   }
   if (entityData->ProgressBar) {
       data["progressBar"] = SerializeProgressBar(*entityData->ProgressBar);
   }
   if (entityData->Toggle) {
       data["toggle"] = SerializeToggle(*entityData->Toggle);
   }
   if (entityData->ScrollView) {
       data["scrollView"] = SerializeScrollView(*entityData->ScrollView);
   }
   if (entityData->LayoutGroup) {
       data["layoutGroup"] = SerializeLayoutGroup(*entityData->LayoutGroup);
   }
   if (entityData->InputField) {
       data["inputField"] = SerializeInputField(*entityData->InputField);
   }
   if (entityData->Dropdown) {
       data["dropdown"] = SerializeDropdown(*entityData->Dropdown);
   }
   if (entityData->UIRect) {
       data["uiRect"] = SerializeUIRect(*entityData->UIRect);
   }
   if (entityData->FitToContent) {
       data["fitToContent"] = SerializeFitToContent(*entityData->FitToContent);
   }
   if (entityData->UISceneCapture) {
       data["uiSceneCapture"] = SerializeUISceneCapture(*entityData->UISceneCapture);
   }
   if (entityData->Text) {
       data["text"] = SerializeText(*entityData->Text);
   }

  // Render overrides
  if (entityData->Mesh) {
      // Preserve alpha blending state - check BOTH material state AND RenderOverrides
      // User may have enabled alpha via UI (RenderOverrides) OR via material properties
      bool materialHasAlpha = false;
      try {
          if (entityData->Mesh->material) {
              materialHasAlpha = (entityData->Mesh->material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
          } else if (!entityData->Mesh->materials.empty() && entityData->Mesh->materials[0]) {
              materialHasAlpha = (entityData->Mesh->materials[0]->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
          }
      } catch(...) {}
      // Only create RenderOverrides if alpha is enabled (from either source)
      bool existingAlphaOverride = entityData->RenderOverrides && entityData->RenderOverrides->AlphaBlendEnabled;
      bool alphaNow = materialHasAlpha || existingAlphaOverride;
      if (alphaNow) {
          if (!entityData->RenderOverrides) entityData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
          entityData->RenderOverrides->AlphaBlendEnabled = true;
      }
  }
  if (entityData->RenderOverrides) {
      data["renderOverrides"] = SerializeRenderOverrides(*entityData->RenderOverrides);
  }

   // Navigation components
   if (entityData->Navigation) {
       data["navmesh"] = SerializeNavMesh(*entityData->Navigation);
   }
   if (entityData->NavAgent) {
       data["navagent"] = SerializeNavAgent(*entityData->NavAgent);
   }
   if (entityData->NavLink) {
       data["navlink"] = SerializeNavLink(*entityData->NavLink);
   }
   if (entityData->Portal) {
       data["portal"] = SerializePortal(*entityData->Portal);
   }

   // Dynamic module components (sidecar)
   try {
       if (!entityData->Dynamic.empty()) {
           data["Dynamic"] = SerializeDynamic(entityData->Dynamic);
       }
   } catch(...) { std::cerr << "[Serializer] Failed to serialize Dynamic for entity=" << id << "\n"; }

   // Emit IK authored blocks if present (as Extra["ik"]) to keep stable names across reimports
   if (!entityData->IKs.empty()) {
       nlohmann::json jIk = nlohmann::json::array();
       for (const auto& k : entityData->IKs) {
           nlohmann::json e;
           e["enabled"] = k.Enabled;
           e["target"] = k.TargetEntity;
           e["pole"] = k.PoleEntity;
           
           // Serialize target/pole entity GUIDs for robust cross-session and prefab restoration
           if (k.TargetEntity != 0) {
               auto* targetData = scene.GetEntityData(k.TargetEntity);
               if (targetData) {
                   e["targetGuidHigh"] = targetData->EntityGuid.high;
                   e["targetGuidLow"] = targetData->EntityGuid.low;
               }
           }
           if (k.PoleEntity != 0) {
               auto* poleData = scene.GetEntityData(k.PoleEntity);
               if (poleData) {
                   e["poleGuidHigh"] = poleData->EntityGuid.high;
                   e["poleGuidLow"] = poleData->EntityGuid.low;
               }
           }
           
           e["weight"] = k.Weight;
           e["maxIterations"] = k.MaxIterations;
           e["tolerance"] = k.Tolerance;
           e["damping"] = k.Damping;
           e["useTwoBone"] = k.UseTwoBone;
           e["lockAxisX"] = k.LockAxisX;
           e["lockAxisY"] = k.LockAxisY;
           e["lockAxisZ"] = k.LockAxisZ;
           e["rootBone"] = k.ChainRootHint;
           e["tipBone"] = k.ChainEffectorHint;
           e["visualize"] = k.Visualize;
           e["chain"] = nlohmann::json::array();
           for (auto b : k.Chain) e["chain"].push_back((int)b);
           if (!k.Constraints.empty()) {
               e["constraints"] = nlohmann::json::array();
               for (const auto& c : k.Constraints) {
                   nlohmann::json cj;
                   cj["useTwist"] = c.useTwist; cj["useHinge"] = c.useHinge;
                   cj["twistMinDeg"] = c.twistMinDeg; cj["twistMaxDeg"] = c.twistMaxDeg;
                   cj["hingeMinDeg"] = c.hingeMinDeg; cj["hingeMaxDeg"] = c.hingeMaxDeg;
                   e["constraints"].push_back(std::move(cj));
               }
           }
           jIk.push_back(std::move(e));
       }
       data["ik"] = std::move(jIk);
   }

   // Emit LookAt constraint blocks if present
   if (!entityData->LookAtConstraints.empty()) {
       nlohmann::json jLookAt = nlohmann::json::array();
       for (const auto& lac : entityData->LookAtConstraints) {
           nlohmann::json e;
           e["enabled"] = lac.Enabled;
           e["mode"] = static_cast<uint8_t>(lac.Mode);
           e["target"] = lac.TargetEntity;
           
           // Serialize target entity GUID for robust cross-session restoration
           if (lac.TargetEntity != 0) {
               auto* targetData = scene.GetEntityData(lac.TargetEntity);
               if (targetData) {
                   e["targetGuidHigh"] = targetData->EntityGuid.high;
                   e["targetGuidLow"] = targetData->EntityGuid.low;
               }
           }
           
           e["weight"] = lac.Weight;
           e["axes"] = static_cast<uint8_t>(lac.Axes);
           e["space"] = static_cast<uint8_t>(lac.Space);
           e["distribution"] = static_cast<uint8_t>(lac.Distribution);
           e["maxYawDeg"] = lac.MaxYawDeg;
           e["maxPitchDeg"] = lac.MaxPitchDeg;
           e["maxRollDeg"] = lac.MaxRollDeg;
           e["smoothingSpeed"] = lac.SmoothingSpeed;
           e["visualize"] = lac.Visualize;
           e["boneChain"] = nlohmann::json::array();
           for (auto b : lac.BoneChain) e["boneChain"].push_back((int)b);
           if (!lac.BoneWeights.empty()) {
               e["boneWeights"] = nlohmann::json::array();
               for (auto w : lac.BoneWeights) e["boneWeights"].push_back(w);
           }
           jLookAt.push_back(std::move(e));
       }
       data["lookat"] = std::move(jLookAt);
   }

   // Merge unknown/extra fields to preserve forward-compatibility
   if (entityData->Extra.is_object()) {
       for (auto it = entityData->Extra.begin(); it != entityData->Extra.end(); ++it) {
           if (!data.contains(it.key())) data[it.key()] = it.value();
       }
   }
   return data;
   }


EntityID Serializer::DeserializeEntity(const json& data, Scene& scene) {
    if (!data.contains("name")) return 0;

    std::string name = data["name"];
    // Use exact-name creation during deserialization to avoid suffix-based clones
    Entity entity = scene.CreateEntityExact(name);
    EntityID id = entity.GetID();
    
    auto* entityData = scene.GetEntityData(id);
    if (!entityData) return 0;

    // Deserialize basic properties
    if (data.contains("layer")) entityData->Layer = data["layer"];
    if (data.contains("tag")) entityData->Tag = data["tag"];
    if (data.contains("visible")) entityData->Visible = data["visible"];
    if (data.contains("active")) entityData->Active = data["active"];
    if (data.contains("parent")) entityData->Parent = data["parent"];
    if (data.contains("children")) {
       entityData->Children.clear();
       for (const auto& child : data["children"]) {
          entityData->Children.push_back(child.get<EntityID>());
          }
       }
    // GUID & prefab source and prefab link
    if (data.contains("guid")) {
        try { data.at("guid").get_to(entityData->EntityGuid); } catch(...) {}
    } else {
        entityData->EntityGuid = ClaymoreGUID::Generate();
    }
    
    // New unified prefab instance component
    if (data.contains("prefabInstance")) {
        entityData->PrefabInstance = std::make_unique<PrefabInstanceComponent>();
        DeserializePrefabInstance(data["prefabInstance"], *entityData->PrefabInstance);
    }
    
    // Legacy prefab fields (kept for backward compatibility)
    if (data.contains("prefabGuid")) {
        try { data.at("prefabGuid").get_to(entityData->PrefabGuid); } catch(...) {}
    } else {
        entityData->PrefabGuid = {};
    }
    if (data.contains("prefabSource")) {
        try { entityData->PrefabSource = IVirtualFS::NormalizePath(data.at("prefabSource").get<std::string>()); } catch(...) {}
    }
    // Model asset identity (GUID only, Core doesn't resolve paths)
    if (data.contains("modelAssetGuid")) {
        try { data.at("modelAssetGuid").get_to(entityData->ModelAssetGuid); } catch(...) {}
    } else {
        entityData->ModelAssetGuid = {};
    }
    
    // Model node deletions (user-intentional deletions)
    entityData->DeletedModelNodes.clear();
    if (data.contains("deletedModelNodes") && data["deletedModelNodes"].is_array()) {
        try {
            for (const auto& path : data["deletedModelNodes"]) {
                if (path.is_string()) {
                    std::string p = path.get<std::string>();
                    if (!p.empty()) {
                        entityData->DeletedModelNodes.push_back(std::move(p));
                    }
                }
            }
        } catch(...) {}
    }

    // Deserialize transform
    if (data.contains("transform")) {
        DeserializeTransform(data["transform"], entityData->Transform);
    }

    // Deserialize components
    if (data.contains("mesh")) {
        MeshComponent tmpMesh; // build off to the side to avoid dangling refs during reallocations
        DeserializeMesh(data["mesh"], tmpMesh, entityData->RenderOverrides);
        entityData->Mesh = std::make_unique<MeshComponent>(std::move(tmpMesh));
    }
    if (data.contains("meshProxy")) {
        if (!entityData->MeshProxy) entityData->MeshProxy = std::make_unique<MeshProxyComponent>();
        DeserializeMeshProxy(data["meshProxy"], *entityData->MeshProxy, scene);
    }

    if (data.contains("light")) {
        entityData->Light = std::make_unique<LightComponent>();
        DeserializeLight(data["light"], *entityData->Light); 
    }
     
    if (data.contains("collider")) {
        entityData->Collider = std::make_unique<ColliderComponent>();
        DeserializeCollider(data["collider"], *entityData->Collider);
    }

    if (data.contains("rigidbody")) {
        entityData->RigidBody = std::make_unique<RigidBodyComponent>();
        DeserializeRigidBody(data["rigidbody"], *entityData->RigidBody);
    }

    if (data.contains("staticbody")) {
        entityData->StaticBody = std::make_unique<StaticBodyComponent>();
        DeserializeStaticBody(data["staticbody"], *entityData->StaticBody);
    }

    if (data.contains("softbody")) {
        entityData->Softbody = std::make_unique<SoftbodyComponent>();
        DeserializeSoftbody(data["softbody"], *entityData->Softbody);
    }

    if (data.contains("characterController")) {
        entityData->CharacterController = std::make_unique<CharacterControllerComponent>();
        DeserializeCharacterController(data["characterController"], *entityData->CharacterController);
    }

    if (data.contains("grassDeformer")) {
        entityData->GrassDeformer = std::make_unique<GrassDeformerComponent>();
        DeserializeGrassDeformer(data["grassDeformer"], *entityData->GrassDeformer);
    }

    if (data.contains("area")) {
        entityData->Area = std::make_unique<cm::physics::AreaComponent>();
        Serializer::DeserializeArea(data["area"], *entityData->Area);
    }

    if (data.contains("camera")) {
        entityData->Camera = std::make_unique<CameraComponent>();
        DeserializeCamera(data["camera"], *entityData->Camera);
    }
    if (data.contains("audioSource")) {
        entityData->AudioSource = std::make_unique<AudioSourceComponent>();
        DeserializeAudioSource(data["audioSource"], *entityData->AudioSource);
    }
    if (data.contains("audioListener")) {
        entityData->AudioListener = std::make_unique<AudioListenerComponent>();
        DeserializeAudioListener(data["audioListener"], *entityData->AudioListener);
    }
    // Navigation components
    if (data.contains("navmesh")) {
        entityData->Navigation = std::make_unique<nav::NavMeshComponent>();
        DeserializeNavMesh(data["navmesh"], *entityData->Navigation);
    }
    if (data.contains("navagent")) {
        entityData->NavAgent = std::make_unique<nav::NavAgentComponent>();
        DeserializeNavAgent(data["navagent"], *entityData->NavAgent);
    }
    if (data.contains("navlink")) {
        entityData->NavLink = std::make_unique<nav::NavLinkComponent>();
        DeserializeNavLink(data["navlink"], *entityData->NavLink);
    }
    if (data.contains("portal")) {
        entityData->Portal = std::make_unique<PortalComponent>();
        DeserializePortal(data["portal"], *entityData->Portal);
    }
    // Animation-related
    if (data.contains("skeleton")) {
        entityData->Skeleton = std::make_unique<SkeletonComponent>();
        DeserializeSkeleton(data["skeleton"], *entityData->Skeleton);
    }
    if (data.contains("skinning")) {
        entityData->Skinning = std::make_unique<SkinningComponent>();
        DeserializeSkinning(data["skinning"], *entityData->Skinning);
        TryResolveSkinningSkeletonRootRef(data["skinning"], *entityData->Skinning, scene);
    }
    // Bone attachment
    if (data.contains("boneAttachment")) {
        entityData->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
        DeserializeBoneAttachment(data["boneAttachment"], *entityData->BoneAttachment);
    }
    // Store pending blend shape weights to apply when BlendShapes is populated
    if (data.contains("blendShapeWeights") && data["blendShapeWeights"].is_array()) {
        for (const auto& entry : data["blendShapeWeights"]) {
            if (entry.is_object() && entry.contains("name") && entry.contains("weight")) {
                entityData->PendingBlendShapeWeights[entry["name"].get<std::string>()] = entry["weight"].get<float>();
            }
        }
    }
    // Store pending unified morph weights to apply when UnifiedMorph is populated
    if (data.contains("unifiedMorphWeights") && data["unifiedMorphWeights"].is_array()) {
        for (const auto& entry : data["unifiedMorphWeights"]) {
            if (entry.is_object() && entry.contains("name") && entry.contains("weight")) {
                entityData->PendingUnifiedMorphWeights[entry["name"].get<std::string>()] = entry["weight"].get<float>();
            }
        }
    }
    // TintMaskController
    if (data.contains("tintController") && data["tintController"].is_object()) {
        entityData->TintController = std::make_unique<TintMaskController>();
        DeserializeTintController(data["tintController"], *entityData->TintController);
    }
    // Ensure skinned material for skinned meshes (respect scene shader preset) without losing flags
    if (entityData->Skinning && entityData->Mesh) {
        bool meshIsSkinned = (entityData->Mesh->mesh && entityData->Mesh->mesh->HasSkinning());
        if (meshIsSkinned) {
            auto oldMat = entityData->Mesh->material;
            bool needsSkinned = true;
            if (oldMat) {
                std::string n = oldMat->GetName();
                // Consider any material with name containing "Skinned" as skinned-capable
                if (std::dynamic_pointer_cast<SkinnedPBRMaterial>(oldMat) || n.find("Skinned") != std::string::npos) {
                    needsSkinned = false;
                }
            }
            if (needsSkinned) {
                auto newMat = MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&Scene::Get());
                if (oldMat) {
                    // Preserve blend/depth/cull flags and common tint
                    newMat->m_StateFlags = oldMat->GetStateFlags();
                    glm::vec4 tint(1.0f);
                    if (oldMat->TryGetUniform("u_ColorTint", tint)) newMat->SetUniform("u_ColorTint", tint);
                }
                entityData->Mesh->material = newMat;
                // Keep primary aligned with slot 0 if slots exist
                if (!entityData->Mesh->materials.empty()) {
                    entityData->Mesh->materials[0] = newMat;
                }
            }
        }
    }
    if (data.contains("terrain")) {
        entityData->Terrain = std::make_unique<TerrainComponent>();
        DeserializeTerrain(data["terrain"], *entityData->Terrain);
    }
    if (data.contains("resourceLayers")) {
        entityData->ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>();
        DeserializeResourceLayers(data["resourceLayers"], *entityData->ResourceLayers);
    }
    if (data.contains("instancer")) {
        entityData->Instancer = std::make_unique<cm::instancer::InstancerComponent>();
        DeserializeInstancer(data["instancer"], *entityData->Instancer);
    }
    if (data.contains("river")) {
        entityData->River = std::make_unique<RiverComponent>();
        DeserializeRiver(data["river"], *entityData->River);
    }
    if (data.contains("spline")) {
        entityData->Spline = std::make_unique<SplineComponent>();
        DeserializeSpline(data["spline"], *entityData->Spline);
    }
    if (data.contains("emitter")) {
        entityData->Emitter = std::make_unique<ParticleEmitterComponent>();
        DeserializeParticleEmitter(data["emitter"], *entityData->Emitter);
    }

    // UI Components
    if (data.contains("canvas")) {
        entityData->Canvas = std::make_unique<CanvasComponent>();
        DeserializeCanvas(data["canvas"], *entityData->Canvas);
    }
    if (data.contains("panel")) {
        entityData->Panel = std::make_unique<PanelComponent>();
        DeserializePanel(data["panel"], *entityData->Panel);
    }
    if (data.contains("button")) {
        entityData->Button = std::make_unique<ButtonComponent>();
        DeserializeButton(data["button"], *entityData->Button);
    }
    if (data.contains("slider")) {
        entityData->Slider = std::make_unique<SliderComponent>();
        DeserializeSlider(data["slider"], *entityData->Slider);
    }
    if (data.contains("progressBar")) {
        entityData->ProgressBar = std::make_unique<ProgressBarComponent>();
        DeserializeProgressBar(data["progressBar"], *entityData->ProgressBar);
    }
    if (data.contains("toggle")) {
        entityData->Toggle = std::make_unique<ToggleComponent>();
        DeserializeToggle(data["toggle"], *entityData->Toggle);
    }
    if (data.contains("scrollView")) {
        entityData->ScrollView = std::make_unique<ScrollViewComponent>();
        DeserializeScrollView(data["scrollView"], *entityData->ScrollView);
    }
    if (data.contains("layoutGroup")) {
        entityData->LayoutGroup = std::make_unique<LayoutGroupComponent>();
        DeserializeLayoutGroup(data["layoutGroup"], *entityData->LayoutGroup);
    }
    if (data.contains("inputField")) {
        entityData->InputField = std::make_unique<InputFieldComponent>();
        DeserializeInputField(data["inputField"], *entityData->InputField);
    }
    if (data.contains("dropdown")) {
        entityData->Dropdown = std::make_unique<DropdownComponent>();
        DeserializeDropdown(data["dropdown"], *entityData->Dropdown);
    }
    if (data.contains("uiRect")) {
        entityData->UIRect = std::make_unique<UIRectComponent>();
        DeserializeUIRect(data["uiRect"], *entityData->UIRect);
    }
    if (data.contains("fitToContent")) {
        entityData->FitToContent = std::make_unique<FitToContentComponent>();
        DeserializeFitToContent(data["fitToContent"], *entityData->FitToContent);
    }
    if (data.contains("uiSceneCapture")) {
        entityData->UISceneCapture = std::make_unique<UISceneCaptureComponent>();
        DeserializeUISceneCapture(data["uiSceneCapture"], *entityData->UISceneCapture);
    }
    if (data.contains("text")) {
        entityData->Text = std::make_unique<TextRendererComponent>();
        DeserializeText(data["text"], *entityData->Text);
    }

    if (data.contains("renderOverrides") && data["renderOverrides"].is_object()) {
        entityData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        DeserializeRenderOverrides(data["renderOverrides"], *entityData->RenderOverrides);
    }

    // Deserialize scripts
    if (data.contains("scripts")) {
        DeserializeScripts(data["scripts"], entityData->Scripts, scene);
    }

    // Dynamic module components (sidecar)
    try {
        if (data.contains("Dynamic") && data["Dynamic"].is_array()) {
            DeserializeDynamic(data["Dynamic"], entityData->Dynamic);
        }
    } catch(...) { std::cerr << "[Serializer] Failed to deserialize Dynamic for entity '" << name << "'\n"; }

    // Animator
    if (data.contains("animator")) {
        if (!entityData->AnimationPlayer) entityData->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
        DeserializeAnimator(data["animator"], *entityData->AnimationPlayer);
    }
    // IK authored blocks
    if (data.contains("ik") && data["ik"].is_array()) {
        entityData->IKs.clear();
        for (const auto& j : data["ik"]) {
            if (!j.is_object()) continue; // Guard against non-object array elements
            cm::animation::ik::IKComponent c;
            c.Enabled = j.value("enabled", true);
            c.TargetEntity = j.value("target", (EntityID)0);
            c.PoleEntity = j.value("pole", (EntityID)0);
            
            // Load target/pole entity GUIDs for post-load resolution
            c.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
            c.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
            c.PoleEntityGuidHigh = j.value("poleGuidHigh", (uint64_t)0);
            c.PoleEntityGuidLow = j.value("poleGuidLow", (uint64_t)0);
            
            
            c.Weight = j.value("weight", 1.0f);
            c.MaxIterations = j.value("maxIterations", 12.0f);
            c.Tolerance = j.value("tolerance", 0.001f);
            c.Damping = j.value("damping", 0.2f);
            c.UseTwoBone = j.value("useTwoBone", true);
            c.LockAxisX = j.value("lockAxisX", false);
            c.LockAxisY = j.value("lockAxisY", false);
            c.LockAxisZ = j.value("lockAxisZ", false);
            c.Visualize = j.value("visualize", false);
            std::vector<cm::animation::ik::BoneId> chainBones;
            if (j.contains("chain") && j["chain"].is_array()) {
                for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
            }
            c.SetChain(chainBones);
            c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
            c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
            if (j.contains("constraints") && j["constraints"].is_array()) {
                for (auto& cj : j["constraints"]) {
                    if (!cj.is_object()) continue; // Guard against non-object elements
                    cm::animation::ik::IKComponent::Constraint cc;
                    cc.useHinge=cj.value("useHinge",false); cc.useTwist=cj.value("useTwist",false);
                    cc.hingeMinDeg=cj.value("hingeMinDeg",0.0f); cc.hingeMaxDeg=cj.value("hingeMaxDeg",0.0f);
                    cc.twistMinDeg=cj.value("twistMinDeg",0.0f); cc.twistMaxDeg=cj.value("twistMaxDeg",0.0f);
                    c.Constraints.push_back(cc);
                }
            }
            entityData->IKs.push_back(std::move(c));
        }
    }
    // LookAt constraint blocks
    if (data.contains("lookat") && data["lookat"].is_array()) {
        entityData->LookAtConstraints.clear();
        for (const auto& j : data["lookat"]) {
            if (!j.is_object()) continue;
            cm::animation::lookat::LookAtConstraintComponent lac;
            lac.Enabled = j.value("enabled", true);
            lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
            lac.TargetEntity = j.value("target", (EntityID)0);
            
            // Load target entity GUID for post-load resolution
            lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
            lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
            
            
            lac.Weight = j.value("weight", 1.0f);
            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
            lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
            lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
            lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
            lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
            lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
            lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
            lac.Visualize = j.value("visualize", false);
            if (j.contains("boneChain") && j["boneChain"].is_array()) {
                for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
            }
            if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
            }
            entityData->LookAtConstraints.push_back(std::move(lac));
        }
    }
    // Preserve unknown fields not recognized by this serializer
    try {
        static const std::unordered_set<std::string> kKnown = {
            "id","name","layer","tag","parent","children","guid","prefabSource","active",
            "transform","mesh","light","collider","rigidbody","staticbody","camera",
            "terrain","emitter","canvas","panel","button","scripts","animator","asset",
            "skeleton","skinning","ik","lookat","navmesh","navagent","navlink","portal","text","blendShapeWeights",
            "unifiedMorphWeights","tintController",
            // Also treat these as known so they aren't duplicated under Extra
            "characterController","area"
        };
        entityData->Extra = nlohmann::json::object();
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (kKnown.find(it.key()) == kKnown.end()) {
                entityData->Extra[it.key()] = it.value();
            }
        }
    } catch(...) {}

    return id;
}

// Apply component data from JSON to an existing entity (for prefab refresh/revert)
void Serializer::DeserializeEntityData(const json& data, EntityData* entityData, Scene& scene, bool createScriptInstances) {
    if (!entityData) return;
    
    // Basic properties (only if present in JSON)
    if (data.contains("name")) entityData->Name = data["name"];
    if (data.contains("layer")) entityData->Layer = data["layer"];
    if (data.contains("tag")) entityData->Tag = data["tag"];
    if (data.contains("visible")) entityData->Visible = data["visible"];
    if (data.contains("active")) entityData->Active = data["active"];
    
    // Transform
    if (data.contains("transform")) {
        DeserializeTransform(data["transform"], entityData->Transform);
        entityData->Transform.TransformDirty = true;
    }
    
    // Components - only deserialize if present in JSON
    if (data.contains("mesh")) {
        MeshComponent tmpMesh;
        DeserializeMesh(data["mesh"], tmpMesh, entityData->RenderOverrides);
        entityData->Mesh = std::make_unique<MeshComponent>(std::move(tmpMesh));
    }
    if (data.contains("meshProxy")) {
        if (!entityData->MeshProxy) entityData->MeshProxy = std::make_unique<MeshProxyComponent>();
        DeserializeMeshProxy(data["meshProxy"], *entityData->MeshProxy, scene);
    }
    if (data.contains("light")) {
        if (!entityData->Light) entityData->Light = std::make_unique<LightComponent>();
        DeserializeLight(data["light"], *entityData->Light);
    }
    if (data.contains("collider")) {
        if (!entityData->Collider) entityData->Collider = std::make_unique<ColliderComponent>();
        DeserializeCollider(data["collider"], *entityData->Collider);
    }
    if (data.contains("rigidbody")) {
        if (!entityData->RigidBody) entityData->RigidBody = std::make_unique<RigidBodyComponent>();
        DeserializeRigidBody(data["rigidbody"], *entityData->RigidBody);
    }
    if (data.contains("staticbody")) {
        if (!entityData->StaticBody) entityData->StaticBody = std::make_unique<StaticBodyComponent>();
        DeserializeStaticBody(data["staticbody"], *entityData->StaticBody);
    }
    if (data.contains("softbody")) {
        if (!entityData->Softbody) entityData->Softbody = std::make_unique<SoftbodyComponent>();
        DeserializeSoftbody(data["softbody"], *entityData->Softbody);
    }
    if (data.contains("characterController")) {
        if (!entityData->CharacterController) entityData->CharacterController = std::make_unique<CharacterControllerComponent>();
        DeserializeCharacterController(data["characterController"], *entityData->CharacterController);
    }
    if (data.contains("camera")) {
        if (!entityData->Camera) entityData->Camera = std::make_unique<CameraComponent>();
        DeserializeCamera(data["camera"], *entityData->Camera);
    }
    if (data.contains("terrain")) {
        if (!entityData->Terrain) entityData->Terrain = std::make_unique<TerrainComponent>();
        DeserializeTerrain(data["terrain"], *entityData->Terrain);
    }
    if (data.contains("emitter")) {
        if (!entityData->Emitter) entityData->Emitter = std::make_unique<ParticleEmitterComponent>();
        DeserializeParticleEmitter(data["emitter"], *entityData->Emitter);
    }
    if (data.contains("skeleton")) {
        if (!entityData->Skeleton) entityData->Skeleton = std::make_unique<SkeletonComponent>();
        DeserializeSkeleton(data["skeleton"], *entityData->Skeleton);
    }
    if (data.contains("skinning")) {
        if (!entityData->Skinning) entityData->Skinning = std::make_unique<SkinningComponent>();
        DeserializeSkinning(data["skinning"], *entityData->Skinning);
        TryResolveSkinningSkeletonRootRef(data["skinning"], *entityData->Skinning, scene);
    }
    if (data.contains("animator")) {
        if (!entityData->AnimationPlayer) entityData->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
        DeserializeAnimator(data["animator"], *entityData->AnimationPlayer);
    }
    if (data.contains("area")) {
        if (!entityData->Area) entityData->Area = std::make_unique<cm::physics::AreaComponent>();
        DeserializeArea(data["area"], *entityData->Area);
    }
    if (data.contains("grassDeformer")) {
        if (!entityData->GrassDeformer) entityData->GrassDeformer = std::make_unique<GrassDeformerComponent>();
        DeserializeGrassDeformer(data["grassDeformer"], *entityData->GrassDeformer);
    }
    if (data.contains("river")) {
        if (!entityData->River) entityData->River = std::make_unique<RiverComponent>();
        DeserializeRiver(data["river"], *entityData->River);
    }
    if (data.contains("spline")) {
        if (!entityData->Spline) entityData->Spline = std::make_unique<SplineComponent>();
        DeserializeSpline(data["spline"], *entityData->Spline);
    }
    if (data.contains("boneAttachment")) {
        if (!entityData->BoneAttachment) entityData->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
        DeserializeBoneAttachment(data["boneAttachment"], *entityData->BoneAttachment);
    }
    if (data.contains("tintController") && data["tintController"].is_object()) {
        if (!entityData->TintController) entityData->TintController = std::make_unique<TintMaskController>();
        DeserializeTintController(data["tintController"], *entityData->TintController);
    }
    
    // UI Components
    if (data.contains("canvas")) {
        if (!entityData->Canvas) entityData->Canvas = std::make_unique<CanvasComponent>();
        DeserializeCanvas(data["canvas"], *entityData->Canvas);
    }
    if (data.contains("panel")) {
        if (!entityData->Panel) entityData->Panel = std::make_unique<PanelComponent>();
        DeserializePanel(data["panel"], *entityData->Panel);
    }
    if (data.contains("button")) {
        if (!entityData->Button) entityData->Button = std::make_unique<ButtonComponent>();
        DeserializeButton(data["button"], *entityData->Button);
    }
    if (data.contains("slider")) {
        if (!entityData->Slider) entityData->Slider = std::make_unique<SliderComponent>();
        DeserializeSlider(data["slider"], *entityData->Slider);
    }
    if (data.contains("progressBar")) {
        if (!entityData->ProgressBar) entityData->ProgressBar = std::make_unique<ProgressBarComponent>();
        DeserializeProgressBar(data["progressBar"], *entityData->ProgressBar);
    }
    if (data.contains("toggle")) {
        if (!entityData->Toggle) entityData->Toggle = std::make_unique<ToggleComponent>();
        DeserializeToggle(data["toggle"], *entityData->Toggle);
    }
    if (data.contains("scrollView")) {
        if (!entityData->ScrollView) entityData->ScrollView = std::make_unique<ScrollViewComponent>();
        DeserializeScrollView(data["scrollView"], *entityData->ScrollView);
    }
    if (data.contains("layoutGroup")) {
        if (!entityData->LayoutGroup) entityData->LayoutGroup = std::make_unique<LayoutGroupComponent>();
        DeserializeLayoutGroup(data["layoutGroup"], *entityData->LayoutGroup);
    }
    if (data.contains("inputField")) {
        if (!entityData->InputField) entityData->InputField = std::make_unique<InputFieldComponent>();
        DeserializeInputField(data["inputField"], *entityData->InputField);
    }
    if (data.contains("dropdown")) {
        if (!entityData->Dropdown) entityData->Dropdown = std::make_unique<DropdownComponent>();
        DeserializeDropdown(data["dropdown"], *entityData->Dropdown);
    }
    if (data.contains("text")) {
        if (!entityData->Text) entityData->Text = std::make_unique<TextRendererComponent>();
        DeserializeText(data["text"], *entityData->Text);
    }
    if (data.contains("uiRect")) {
        if (!entityData->UIRect) entityData->UIRect = std::make_unique<UIRectComponent>();
        DeserializeUIRect(data["uiRect"], *entityData->UIRect);
    }
    if (data.contains("fitToContent")) {
        if (!entityData->FitToContent) entityData->FitToContent = std::make_unique<FitToContentComponent>();
        DeserializeFitToContent(data["fitToContent"], *entityData->FitToContent);
    }
    if (data.contains("uiSceneCapture")) {
        if (!entityData->UISceneCapture) entityData->UISceneCapture = std::make_unique<UISceneCaptureComponent>();
        DeserializeUISceneCapture(data["uiSceneCapture"], *entityData->UISceneCapture);
    }
    
    // Navigation
    if (data.contains("navmesh")) {
        if (!entityData->Navigation) entityData->Navigation = std::make_unique<nav::NavMeshComponent>();
        DeserializeNavMesh(data["navmesh"], *entityData->Navigation);
    }
    if (data.contains("navagent")) {
        if (!entityData->NavAgent) entityData->NavAgent = std::make_unique<nav::NavAgentComponent>();
        DeserializeNavAgent(data["navagent"], *entityData->NavAgent);
    }
    if (data.contains("navlink")) {
        if (!entityData->NavLink) entityData->NavLink = std::make_unique<nav::NavLinkComponent>();
        DeserializeNavLink(data["navlink"], *entityData->NavLink);
    }
    if (data.contains("portal")) {
        if (!entityData->Portal) entityData->Portal = std::make_unique<PortalComponent>();
        DeserializePortal(data["portal"], *entityData->Portal);
    }
    
    // Render overrides
    if (data.contains("renderOverrides") && data["renderOverrides"].is_object()) {
        if (!entityData->RenderOverrides) entityData->RenderOverrides = std::make_unique<RenderOverridesComponent>();
        DeserializeRenderOverrides(data["renderOverrides"], *entityData->RenderOverrides);
    }
    
    // Scripts
    if (data.contains("scripts")) {
        DeserializeScripts(data["scripts"], entityData->Scripts, scene, createScriptInstances);
    }
}

// Scene serialization - THE SINGLE SOURCE OF TRUTH
// When rootFilter is set, only entities under that root are serialized (for prefabs)
json Serializer::SerializeScene(Scene& scene, EntityID rootFilter) {
    json sceneData;
    sceneData["version"] = "1.0";
    sceneData["entities"] = json::array();
    
    // Build set of entities to include when filtering to a subtree
    std::unordered_set<EntityID> subtreeEntities;
    if (rootFilter != INVALID_ENTITY_ID) {
        std::function<void(EntityID)> collectSubtree = [&](EntityID id) {
            subtreeEntities.insert(id);
            auto* d = scene.GetEntityData(id);
            if (d) {
                for (EntityID child : d->Children) {
                    collectSubtree(child);
                }
            }
        };
        collectSubtree(rootFilter);
        
        // For prefabs, add metadata
        auto* rootData = scene.GetEntityData(rootFilter);
        if (rootData) {
            sceneData["type"] = "prefab";
            sceneData["name"] = rootData->Name;
            sceneData["guid"] = rootData->EntityGuid;
            sceneData["rootGuid"] = rootData->EntityGuid;
        }
    }

    PrefabReferenceSerializationContext prefabRefContext{&scene, rootFilter, &subtreeEntities};
    std::unique_ptr<ScopedPrefabReferenceSerializationContext> prefabRefScope;
    if (rootFilter != INVALID_ENTITY_ID) {
        prefabRefScope = std::make_unique<ScopedPrefabReferenceSerializationContext>(prefabRefContext);
    }
    
    // Scene-only settings (skip for prefabs)
    if (rootFilter == INVALID_ENTITY_ID) {
        // Scene default shader preset
        try {
            sceneData["defaultShaderPreset"] = (int)scene.GetDefaultShaderPreset();
        } catch(...) {}
    // Environment
    try {
        const Environment& env = scene.GetEnvironment();
        json jenv;
        jenv["ambientMode"] = (env.Ambient == Environment::AmbientMode::FlatColor) ? "FlatColor" : "Skybox";
        jenv["ambientColor"] = SerializeVec3(env.AmbientColor);
        jenv["ambientIntensity"] = env.AmbientIntensity;
        jenv["useSkybox"] = env.UseSkybox;
        // Skybox texture path not serialized yet (TextureCube asset system pending)
        jenv["exposure"] = env.Exposure;
        jenv["fogEnabled"] = env.EnableFog;
        jenv["fogColor"] = SerializeVec3(env.FogColor);
        jenv["fogDensity"] = env.FogDensity;
        jenv["proceduralSky"] = env.ProceduralSky;
        // Sky parameters
        jenv["skyTopColor"] = SerializeVec3(env.SkyTopColor);
        jenv["skyHorizonColor"] = SerializeVec3(env.SkyHorizonColor);
        jenv["skyGroundColor"] = SerializeVec3(env.SkyGroundColor);
        // Legacy aliases for backward compatibility
        jenv["skyTint"] = SerializeVec3(env.SkyTopColor);
        jenv["groundColor"] = SerializeVec3(env.SkyGroundColor);
        jenv["sunSize"] = env.SunSize;
        jenv["sunSizeConvergence"] = env.SunSizeConvergence;
        jenv["sunIntensity"] = env.SunIntensity;
        jenv["atmosphereThickness"] = env.AtmosphereThickness;
        jenv["horizonFade"] = env.HorizonFade;
        jenv["skyExposure"] = env.SkyExposure;
        // Cosmetic outline
        jenv["outlineEnabled"] = env.OutlineEnabled;
        jenv["outlineColor"] = SerializeVec3(env.OutlineColor);
        jenv["outlineThickness"] = env.OutlineThickness;
        // Shadows
        jenv["shadowsEnabled"] = env.ShadowsEnabled;
        jenv["shadowMapResolution"] = env.ShadowMapResolution;
        jenv["shadowDistance"] = env.ShadowDistance;
        jenv["shadowBias"] = env.ShadowBias;
        jenv["shadowNormalBias"] = env.ShadowNormalBias;
        jenv["shadowSoftness"] = env.ShadowSoftness;
        jenv["shadowSamples"] = env.ShadowSamples;
        jenv["shadowStrength"] = env.ShadowStrength;
        // Texture filtering preference
        jenv["textureFilter"] = (env.TextureFilter == Environment::TextureFilterMode::Linear) ? "Linear" : "Point";
        sceneData["environment"] = std::move(jenv);
    } catch(...) {}

    // Project-wide global shader properties
    try {
        sceneData["globalShaderProperties"] = GlobalShaderProperties::Instance().ToJson<json>();
    } catch(...) {}
    // Persist editor-only viewport state so the camera restores on load
    try {
        if (scene.HasEditorViewportState()) {
            const Scene::EditorViewportState& cam = scene.GetEditorViewportState();
            json camJson;
            camJson["target"] = SerializeVec3(cam.Target);
            camJson["yaw"] = cam.Yaw;
            camJson["pitch"] = cam.Pitch;
            camJson["distance"] = cam.Distance;
            camJson["fov"] = cam.FieldOfView;
            camJson["nearClip"] = cam.NearClip;
            camJson["farClip"] = cam.FarClip;
            sceneData["editorState"]["camera"] = std::move(camJson);
        }
    } catch(...) {}

    } // End scene-only settings (rootFilter == INVALID_ENTITY_ID)
    
    // Asset map - include for BOTH scenes AND prefabs so GUIDs can be resolved.
    // This is critical for prefab mesh loading to work correctly.
    try {
        auto all = AssetLibrary::Instance().GetAllAssets();
        if (!all.empty()) {
            json amap = json::array();
            std::unordered_set<std::string> emittedGuids;
            for (const auto& rec : all) {
                const std::string& path = std::get<0>(rec);
                const ClaymoreGUID& guid = std::get<1>(rec);
                AssetType type = std::get<2>(rec);
                if (path.empty() || (guid.high == 0 && guid.low == 0)) continue;
                if (type != AssetType::Mesh) continue;
                std::string portablePath = NormalizePortableAssetPath(path);
                if (!IsMeshLikeAssetPath(portablePath)) continue;
                std::string guidStr = guid.ToString();
                if (!emittedGuids.insert(guidStr).second) continue;
                json j;
                j["guid"] = std::move(guidStr);
                j["path"] = std::move(portablePath);
                amap.push_back(std::move(j));
            }
            if (!amap.empty()) sceneData["assetMap"] = std::move(amap);
        }
    } catch(...) {}

    // Build skip set for descendants of imported model roots and prefab roots; collect per-node overrides for models and prefabs
    // NOTE: Skip set is ONLY used for full scene serialization, NOT for prefabs
    // Prefabs use parentIndex for hierarchy - they're just flat entity arrays like scenes
    std::unordered_set<EntityID> skip;
    std::unordered_map<EntityID, nlohmann::json> rootOverrides;
    auto computeNodePath = [&](EntityID root, EntityID node) -> std::string {
        std::vector<std::string> parts; EntityID cur = node;
        while (cur != -1) {
            auto* d = scene.GetEntityData(cur); if (!d) break;
            if (cur == root) { parts.push_back(d->Name); break; }
            parts.push_back(d->Name); cur = d->Parent;
        }
        std::reverse(parts.begin(), parts.end());
        if (!parts.empty()) parts.erase(parts.begin()); // make path relative to model root
        std::string s; for (size_t i=0;i<parts.size();++i){ s += parts[i]; if (i+1<parts.size()) s += "/"; }
        return s;
    };
    
    // Build skip set and collect overrides for model compaction
    // This applies to BOTH scenes AND prefabs - they should serialize identically
    for (const auto& e : scene.GetEntities()) {
        // For prefabs, only process entities in the subtree
        if (rootFilter != INVALID_ENTITY_ID && subtreeEntities.find(e.GetID()) == subtreeEntities.end()) continue;
        
        std::string path; ClaymoreGUID g{}; 
        if (IsImportedModelRoot(scene, e.GetID(), path, g)) {
            rootOverrides[e.GetID()] = nlohmann::json::array();
            // Walk descendants. Skip serializing them fully; store override blobs under the root instead
            std::function<void(EntityID)> walk = [&](EntityID id){
                auto* d = scene.GetEntityData(id); if (!d) return; 
                for (EntityID c : d->Children) {
                    // Check if this child is itself a nested model root BEFORE adding to skip set
                    // Nested models should be serialized separately with their own asset record, not as overrides
                    auto* cd = scene.GetEntityData(c);
                    bool isNestedModelRoot = cd && (cd->ModelAssetGuid.high != 0 || cd->ModelAssetGuid.low != 0);
                    if (isNestedModelRoot) {
                        // Don't add to skip or rootOverrides - this nested model will be serialized
                        // as its own entity with its own asset record. Skip recursion too.
                        continue;
                    }
                    
                    skip.insert(c);
                    
                    nlohmann::json childJ = SerializeEntity(c, scene);
                    
                    // Force-emit alpha override for children whose material indicates alpha blending
                    try {
                        auto* cd = scene.GetEntityData(c);
                        if (childJ.contains("mesh") && cd && cd->Mesh) {
                            bool alphaNow = false;
                            if (cd->Mesh->material) alphaNow = (cd->Mesh->material->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
                            else if (!cd->Mesh->materials.empty() && cd->Mesh->materials[0]) alphaNow = (cd->Mesh->materials[0]->GetStateFlags() & BGFX_STATE_BLEND_ALPHA) != 0;
                            if (alphaNow) {
                                if (!cd->RenderOverrides) cd->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                                cd->RenderOverrides->AlphaBlendEnabled = true;
                                childJ["renderOverrides"] = SerializeRenderOverrides(*cd->RenderOverrides);
                                // Also force the mesh alpha fields so it persists even without RenderOverrides
                                try {
                                    childJ["mesh"]["materialAlpha"] = true;
                                    if (!childJ["mesh"].contains("slotMaterials") || !childJ["mesh"]["slotMaterials"].is_array()) {
                                        childJ["mesh"]["slotMaterials"] = nlohmann::json::array();
                                    }
                                    auto& slots = childJ["mesh"]["slotMaterials"];
                                    if (slots.empty()) slots.push_back(nlohmann::json::object());
                                    if (!slots[0].is_object()) slots[0] = nlohmann::json::object();
                                    slots[0]["alpha"] = true;
                                } catch(...) {}
                                std::cout << "[EmitChildRO] path='" << computeNodePath(e.GetID(), c) << "' alphaBlend=1" << std::endl;
                            }
                        }
                    } catch(...) {}
                    childJ["_modelNodePath"] = computeNodePath(e.GetID(), c);
                    
                    // Add stable node identity for robust matching after model re-export
                    try {
                        auto* cd = scene.GetEntityData(c);
                        if (cd) {
                            std::string nodePath = computeNodePath(e.GetID(), c);
                            childJ["_normalizedPath"] = ModelNodeIdentity::NormalizePath(nodePath);
                            childJ["_normalizedName"] = ModelNodeIdentity::NormalizeName(cd->Name);
                            
                            // Always preserve entity GUID - critical for matching user-added entities
                            if (!(cd->EntityGuid.high == 0 && cd->EntityGuid.low == 0)) {
                                childJ["guid"] = cd->EntityGuid;
                            }
                            
                            auto isBoneNode = [&](EntityID childId, const EntityData* childData) -> bool {
                                if (!childData) return false;

                                EntityID ancestor = childData->Parent;
                                while (ancestor != INVALID_ENTITY_ID) {
                                    auto* ancestorData = scene.GetEntityData(ancestor);
                                    if (!ancestorData) break;
                                    if (ancestorData->Skeleton && !ancestorData->Skeleton->BoneEntities.empty()) {
                                        for (EntityID boneId : ancestorData->Skeleton->BoneEntities) {
                                            if (boneId == childId) {
                                                return true;
                                            }
                                        }
                                        break;
                                    }
                                    ancestor = ancestorData->Parent;
                                }

                                const std::string& name = childData->Name;
                                return (name == "SkeletonRoot") ||
                                       (name.find("Bone") != std::string::npos) ||
                                       (name.find("bone") != std::string::npos) ||
                                       (name.find("mixamorig") != std::string::npos) ||
                                       (name.find("Armature") != std::string::npos) ||
                                       (name.find("armature") != std::string::npos) ||
                                       (name.find("Bip001") != std::string::npos) ||
                                       (name.find("_End") != std::string::npos);
                            };

                            const bool isNestedModelRoot = cm::model::ModelDeltaExtractor::IsModelRoot(cd);

                            bool isSameModelMesh = false;
                            if ((g.high != 0 || g.low != 0) && cd->Mesh) {
                                const ClaymoreGUID& meshGuid = cd->Mesh->meshReference.guid;
                                isSameModelMesh = (meshGuid.high == g.high && meshGuid.low == g.low);
                            }

                            const bool hasUserComponents =
                                cd->Camera || cd->Light || cd->Collider || cd->RigidBody ||
                                cd->StaticBody || cd->Softbody || cd->CharacterController || cd->Emitter ||
                                cd->Canvas || cd->Panel || cd->Button || cd->Text ||
                                cd->Navigation || cd->NavAgent || cd->Area || !cd->Scripts.empty() ||
                                cd->Terrain || (cd->Extra.is_object() && !cd->Extra.empty());

                            bool isUserAdded = false;
                            if (!isNestedModelRoot && !isSameModelMesh && !cd->Skeleton && !cd->Skinning && !cd->BlendShapes) {
                                if (hasUserComponents || cd->Mesh || !isBoneNode(c, cd)) {
                                    isUserAdded = true;
                                }
                            }
                            
                            if (isUserAdded) {
                                childJ["_added"] = true;
                            } else {
                                // Strip numeric suffix from model-internal children's names during serialization
                                // Model internal children should preserve their original names without index suffixes
                                // Only user-added children or children entering a conflicting sibling space should have suffixes
                                if (childJ.contains("name") && childJ["name"].is_string()) {
                                    std::string name = childJ["name"].get<std::string>();
                                    // Check if name has a numeric suffix pattern (e.g., "SkeletonRoot_97")
                                    size_t us = name.find_last_of('_');
                                    if (us != std::string::npos && us > 0 && us < name.size() - 1) {
                                        bool allDigits = true;
                                        for (size_t i = us + 1; i < name.size(); ++i) {
                                            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                                allDigits = false;
                                                break;
                                            }
                                        }
                                        // If it's a numeric suffix, strip it for model-internal children
                                        if (allDigits) {
                                            std::string baseName = name.substr(0, us);
                                            childJ["name"] = baseName;
                                        }
                                    }
                                }
                            }
                            
                            // Compute content hash for renamed node matching
                            if (cd->Mesh) {
                                std::vector<int> meshIndices = { cd->Mesh->meshReference.fileID };
                                const float* xfData = &cd->Transform.LocalMatrix[0][0];
                                int vertHint = cd->Mesh->mesh ? static_cast<int>(cd->Mesh->mesh->Vertices.size()) : 0;
                                childJ["_contentHash"] = ModelNodeIdentity::ComputeContentHash(meshIndices, xfData, vertHint);
                            }
                            
                            // Convert TintController target EntityIDs to paths for child entities
                            if (cd->TintController && childJ.contains("tintController") && childJ["tintController"].is_object()) {
                                // Build EntityID -> path map for the model subtree
                                // IMPORTANT: Paths are relative to model root, NOT including the model root name
                                // This matches how ApplyModelDelta resolves paths
                                std::unordered_map<EntityID, std::string> entityToPath;
                                entityToPath[e.GetID()] = "";  // Model root is empty path
                                std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID nodeId, const std::string& parentPath) {
                                    auto* data = scene.GetEntityData(nodeId);
                                    if (!data) return;
                                    std::string path = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                                    entityToPath[nodeId] = path;
                                    for (EntityID child : data->Children) buildPathMap(child, path);
                                };
                                // Start from children of model root with empty parent path
                                auto* modelRootData = scene.GetEntityData(e.GetID());
                                if (modelRootData) {
                                    for (EntityID child : modelRootData->Children) {
                                        buildPathMap(child, "");
                                    }
                                }
                                
                                // Update TintController targets with entityPath
                                if (childJ["tintController"].contains("targets") && childJ["tintController"]["targets"].is_array()) {
                                    for (auto& targetJ : childJ["tintController"]["targets"]) {
                                        if (targetJ.contains("entity")) {
                                            EntityID targetId = static_cast<EntityID>(targetJ["entity"].get<int64_t>());
                                            auto pathIt = entityToPath.find(targetId);
                                            if (pathIt != entityToPath.end()) {
                                                targetJ["entityPath"] = pathIt->second;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } catch (...) {}
                    
                    // Keep name to persist renames; strip relational/id-only fields
                    childJ.erase("id"); childJ.erase("parent"); childJ.erase("children"); childJ.erase("asset");
                    if (!childJ.empty()) rootOverrides[e.GetID()].push_back(std::move(childJ));
                    
                    // Recurse into non-nested-model children
                    walk(c);
                }
            };
            walk(e.GetID());
            // BUG FIX: Only erase from skip if this entity was NOT already in skip before we started.
            // If it was already in skip, it's a child of another model root and should remain skipped.
            // We need to track which entities we're processing as roots to avoid erasing children.
            // Simple fix: check if this entity has a parent that is also a model root.
            bool isChildOfAnotherModelRoot = false;
            {
                auto* ed = scene.GetEntityData(e.GetID());
                if (ed && ed->Parent != INVALID_ENTITY_ID) {
                    std::string tmpPath; ClaymoreGUID tmpGuid{};
                    // Walk up the parent chain and check if any ancestor is a model root
                    EntityID parentId = ed->Parent;
                    while (parentId != INVALID_ENTITY_ID) {
                        if (IsImportedModelRoot(scene, parentId, tmpPath, tmpGuid)) {
                            isChildOfAnotherModelRoot = true;
                            break;
                        }
                        auto* parentData = scene.GetEntityData(parentId);
                        if (!parentData) break;
                        parentId = parentData->Parent;
                    }
                }
            }
            if (!isChildOfAnotherModelRoot) {
                skip.erase(e.GetID());
            }
        }
    }
    // Mark descendants of prefab roots to be skipped and collect per-node overrides under prefab roots
    // This handles both legacy PrefabSource and new PrefabInstanceComponent
    for (const auto& e : scene.GetEntities()) {
        // For prefabs, only process entities in the subtree
        if (rootFilter != INVALID_ENTITY_ID && subtreeEntities.find(e.GetID()) == subtreeEntities.end()) continue;
        
        auto* d = scene.GetEntityData(e.GetID()); if (!d) continue;
        
        // Check for legacy PrefabSource OR new PrefabInstanceComponent
        bool hasPrefabInstance = d->PrefabInstance &&
            !(d->PrefabInstance->PrefabAssetGuid.high == 0 && d->PrefabInstance->PrefabAssetGuid.low == 0);
        bool hasLegacyPrefab = !d->PrefabSource.empty();
        bool isPrefabRoot = hasLegacyPrefab || hasPrefabInstance;
        
        // Prefab roots take precedence over imported-model classification. This matters for
        // prefabs whose root entity also carries ModelAssetGuid because the prefab contains a model.
        bool isModelRoot = !isPrefabRoot && (d->ModelAssetGuid.high != 0 || d->ModelAssetGuid.low != 0);
        if (isModelRoot) continue;  // Skip - already handled by model root processing above

        if (isPrefabRoot) {
            // When serializing a prefab subtree, the root should be treated as authored data,
            // not as a prefab instance of itself.
            if (rootFilter != INVALID_ENTITY_ID && e.GetID() == rootFilter) {
                continue;
            }
            // Collect overrides for children under this prefab root
            rootOverrides[e.GetID()] = nlohmann::json::array();
            std::function<void(EntityID)> walk = [&](EntityID id){
                auto* dd = scene.GetEntityData(id); if (!dd) return;
                for (EntityID c : dd->Children) {
                    // Check if this child is itself a nested model root BEFORE adding to skip set
                    // Nested models should be serialized separately with their own asset record, not as overrides
                    auto* cd = scene.GetEntityData(c);
                    bool isNestedModelRoot = cd && (cd->ModelAssetGuid.high != 0 || cd->ModelAssetGuid.low != 0);
                    if (isNestedModelRoot) {
                        // Don't add to skip or rootOverrides - this nested model will be serialized
                        // as its own entity with its own asset record. Skip recursion too.
                        continue;
                    }
                    
                    // Check if this child is itself a nested PREFAB root
                    // Nested prefabs should be serialized separately with their own prefab reference, not flattened as overrides
                    bool isNestedPrefabRoot = cd && (cd->PrefabInstance != nullptr || !cd->PrefabSource.empty());
                    if (isNestedPrefabRoot) {
                        // Don't add to skip or rootOverrides - this nested prefab will be serialized
                        // as its own entity with its own prefab reference. Skip recursion too.
                        continue;
                    }
                    
                    skip.insert(c);
                    
                    nlohmann::json childJ = SerializeEntity(c, scene);
                    // Build relative path by names under the prefab root
                    std::string rel = computeNodePath(e.GetID(), c);
                    childJ["_prefabNodePath"] = rel;
                    // Also store entity GUID for robust override matching (names can change)
                    // This allows fallback to GUID-based matching when path-based matching fails
                    if (cd && (cd->EntityGuid.high != 0 || cd->EntityGuid.low != 0)) {
                        childJ["_prefabEntityGuid"] = cd->EntityGuid;
                    }
                    // Keep name; drop relational/id-only fields
                    childJ.erase("id"); childJ.erase("parent"); childJ.erase("children"); childJ.erase("asset");
                    if (!childJ.empty()) rootOverrides[e.GetID()].push_back(std::move(childJ));
                    
                    // Recurse into non-nested-model children
                    walk(c);
                }
            };
            walk(e.GetID());
            // BUG FIX: Same as model roots - don't erase if this is a child of another model/prefab root
            bool isChildOfAnotherRoot = false;
            {
                auto* ed = scene.GetEntityData(e.GetID());
                if (ed && ed->Parent != INVALID_ENTITY_ID) {
                    std::string tmpPath; ClaymoreGUID tmpGuid{};
                    EntityID parentId = ed->Parent;
                    while (parentId != INVALID_ENTITY_ID) {
                        if (IsImportedModelRoot(scene, parentId, tmpPath, tmpGuid)) {
                            isChildOfAnotherRoot = true;
                            break;
                        }
                        auto* parentData = scene.GetEntityData(parentId);
                        if (!parentData) break;
                        if (!parentData->PrefabSource.empty()) {
                            isChildOfAnotherRoot = true;
                            break;
                        }
                        parentId = parentData->Parent;
                    }
                }
            }
            if (!isChildOfAnotherRoot) {
                skip.erase(e.GetID());
            }
        }
    }

    // UNIFIED SERIALIZATION: Prefabs now use the EXACT same format as scenes
    // No special parentIndex conversion - prefabs ARE mini-scenes with id/parent fields
    // This ensures:
    // 1. Model roots have "asset" blocks that trigger InstantiateModel() on load
    // 2. Model overrides are in "children" array (not erased)
    // 3. Nested model parent resolution works via _parentModelGuid/_parentPath
    // 4. Single code path for both scene and prefab loading
    
    
    // Fix duplicate GUIDs before serialization
    // This ensures that duplicated entities get unique GUIDs when saving
    std::unordered_map<ClaymoreGUID, std::vector<EntityID>> guidToEntities;
    for (const auto& entity : scene.GetEntities()) {
        EntityID eid = entity.GetID();
        // Skip entities not in subtree when filtering for prefab
        if (rootFilter != INVALID_ENTITY_ID && subtreeEntities.find(eid) == subtreeEntities.end()) {
            continue;
        }
        // Skip model descendants - they're serialized in the children array of model roots
        if (skip.find(eid) != skip.end()) {
            continue;
        }
        auto* d = scene.GetEntityData(eid);
        if (!d) continue;
        // Only check non-zero GUIDs (zero GUIDs will be regenerated on load)
        if (!(d->EntityGuid.high == 0 && d->EntityGuid.low == 0)) {
            guidToEntities[d->EntityGuid].push_back(eid);
        }
    }
    
    // Regenerate GUIDs for duplicates (keep first occurrence, regenerate others)
    for (auto& kv : guidToEntities) {
        if (kv.second.size() > 1) {
            std::cout << "[Serializer] Found " << kv.second.size() << " entities with duplicate GUID " 
                      << kv.first.ToString() << ", regenerating GUIDs for duplicates" << std::endl;
            // Keep the first entity's GUID, regenerate for the rest
            for (size_t i = 1; i < kv.second.size(); ++i) {
                EntityID dupId = kv.second[i];
                auto* dupData = scene.GetEntityData(dupId);
                if (dupData) {
                    dupData->EntityGuid = ClaymoreGUID::Generate();
                    std::cout << "[Serializer] Regenerated GUID for entity id=" << dupId 
                              << " name='" << dupData->Name << "'" << std::endl;
                }
            }
        }
    }
    
    for (const auto& entity : scene.GetEntities()) {
        EntityID eid = entity.GetID();
        // Skip entities not in subtree when filtering for prefab
        if (rootFilter != INVALID_ENTITY_ID && subtreeEntities.find(eid) == subtreeEntities.end()) {
            continue;
        }
        // Skip model descendants - they're serialized in the children array of model roots
        if (skip.find(eid) != skip.end()) {
            continue;
        }
        
        json entityData = SerializeEntity(eid, scene);
        
        // NOTE: Prefabs now use the same id/parent fields as scenes
        // The loading code handles ID remapping just like scene loading
        // This is TRUE PARITY - prefabs ARE mini-scenes
        
        // If this is an imported model root, attach compact asset record
        std::string path; ClaymoreGUID g{};
        if (IsImportedModelRoot(scene, eid, path, g)) {
            json asset;
            asset["type"] = "model";
            // save virtual path
            std::string v = path; for(char& c: v) if (c=='\\') c='/';
            auto pos = v.find("assets/"); if (pos != std::string::npos) v = v.substr(pos);
            asset["path"] = v;
            asset["guid"] = g.ToString();
            entityData["asset"] = std::move(asset);
            // attach collected per-node overrides (if any)
            auto it = rootOverrides.find(eid);
            entityData["children"] = (it != rootOverrides.end()) ? it->second : nlohmann::json::array();
            
            // BUG FIX: Find UnifiedMorphComponent in model hierarchy (usually on skeleton root)
            // and include its weights in the compact asset node so they persist on scene save/load.
            // The UnifiedMorphComponent may be on a child entity (skeleton root), not the model root.
            std::function<UnifiedMorphComponent*(EntityID)> findUnifiedMorph = [&](EntityID id) -> UnifiedMorphComponent* {
                auto* d = scene.GetEntityData(id);
                if (!d) return nullptr;
                if (d->UnifiedMorph && !d->UnifiedMorph->Names.empty()) return d->UnifiedMorph.get();
                for (EntityID child : d->Children) {
                    if (auto* found = findUnifiedMorph(child)) return found;
                }
                return nullptr;
            };
            if (auto* unifiedMorph = findUnifiedMorph(eid)) {
                json umWeights = SerializeUnifiedMorphWeights(*unifiedMorph);
                if (!umWeights.empty()) {
                    entityData["unifiedMorphWeights"] = std::move(umWeights);
                }
            }
            
            // BUG FIX: Convert TintController target EntityIDs to relative paths
            // so they can be resolved correctly when the model is re-instantiated on scene load.
            if (entityData.contains("tintController") && entityData["tintController"].is_object()) {
                // Build EntityID -> relative path map for this model subtree
                std::unordered_map<EntityID, std::string> entityToPath;
                std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID id, const std::string& parentPath) {
                    auto* data = scene.GetEntityData(id);
                    if (!data) return;
                    std::string nodePath = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                    entityToPath[id] = nodePath;
                    for (EntityID c : data->Children) buildPathMap(c, nodePath);
                };
                buildPathMap(eid, "");  // Start from model root
                entityToPath[eid] = "";  // Root is empty path
                
                // Update TintController targets with entityPath
                if (entityData["tintController"].contains("targets") && entityData["tintController"]["targets"].is_array()) {
                    for (auto& target : entityData["tintController"]["targets"]) {
                        if (target.contains("entity")) {
                            EntityID targetId = static_cast<EntityID>(target["entity"].get<int64_t>());
                            auto pathIt = entityToPath.find(targetId);
                            if (pathIt != entityToPath.end()) {
                                target["entityPath"] = pathIt->second;
                            }
                        }
                    }
                }
            }
            
            // BUG FIX: If this nested model has a parent inside another model's hierarchy,
            // store path-based parent reference so it can be resolved after both models instantiate.
            // The raw parent ID won't be in idMapping because internal model nodes aren't serialized as entities.
            auto* nestedData = scene.GetEntityData(eid);
            if (nestedData && nestedData->Parent != INVALID_ENTITY_ID && nestedData->Parent != (EntityID)-1) {
                // Check if the parent is inside another model's hierarchy
                EntityID parentId = nestedData->Parent;
                auto* parentData = scene.GetEntityData(parentId);
                EntityID ancestorModelRoot = (EntityID)-1;
                std::string ancestorModelGuid;
                
                // Walk up to find the ancestor model root
                EntityID cur = parentId;
                while (cur != (EntityID)-1 && cur != (EntityID)0) {
                    auto* curData = scene.GetEntityData(cur);
                    if (!curData) break;
                    if (curData->ModelAssetGuid.high != 0 || curData->ModelAssetGuid.low != 0) {
                        // Found an ancestor model root
                        ancestorModelRoot = cur;
                        ancestorModelGuid = curData->ModelAssetGuid.ToString();
                        break;
                    }
                    cur = curData->Parent;
                }
                
                // If we found an ancestor model root and it's not this entity itself
                if (ancestorModelRoot != (EntityID)-1 && ancestorModelRoot != eid) {
                    // Helper to strip trailing numeric suffix (e.g., "SkeletonRoot_97" -> "SkeletonRoot")
                    auto stripNumericSuffix = [](const std::string& name) -> std::string {
                        size_t us = name.find_last_of('_');
                        if (us == std::string::npos || us == 0) return name;
                        bool allDigits = true;
                        for (size_t i = us + 1; i < name.size(); ++i) {
                            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                allDigits = false;
                                break;
                            }
                        }
                        return allDigits ? name.substr(0, us) : name;
                    };
                    
                    // Compute path from ancestorModelRoot to the parent node
                    // Strip numeric suffixes so paths match freshly instantiated models
                    std::vector<std::string> pathParts;
                    EntityID walker = parentId;
                    while (walker != (EntityID)-1 && walker != ancestorModelRoot) {
                        auto* walkerData = scene.GetEntityData(walker);
                        if (!walkerData) break;
                        pathParts.push_back(stripNumericSuffix(walkerData->Name));
                        walker = walkerData->Parent;
                    }
                    std::reverse(pathParts.begin(), pathParts.end());
                    std::string parentPath;
                    for (size_t i = 0; i < pathParts.size(); ++i) {
                        if (i > 0) parentPath += "/";
                        parentPath += pathParts[i];
                    }
                    
                    // Store the path-based parent reference
                    // Include both GUID (for asset matching) and old entity ID (for disambiguation when multiple instances)
                    entityData["_parentModelGuid"] = ancestorModelGuid;
                    entityData["_parentModelId"] = ancestorModelRoot;  // Old entity ID for disambiguation
                    entityData["_parentPath"] = parentPath;
                    try { std::cout << "[Serialize] Nested model '" << nestedData->Name << "' has parent inside model ID=" << ancestorModelRoot << " GUID=" << ancestorModelGuid << " path='" << parentPath << "'" << std::endl; } catch(...) {}
                }
            }
        }
        // If this is a prefab root, attach compact prefab asset record and attach collected overrides
        // Handle both legacy PrefabSource and new PrefabInstanceComponent
        // IMPORTANT: Skip if already serialized as a model - model roots take precedence
        if (auto* d = scene.GetEntityData(eid); d) {
            if (rootFilter != INVALID_ENTITY_ID && eid == rootFilter) {
                entityData.erase("prefabInstance");
                entityData.erase("prefabGuid");
                entityData.erase("prefabSource");
            } else {
            bool hasPrefabInstance = d->PrefabInstance && 
                !(d->PrefabInstance->PrefabAssetGuid.high == 0 && d->PrefabInstance->PrefabAssetGuid.low == 0);
            bool hasLegacyPrefab = !d->PrefabSource.empty();
            
            // Prefab roots take precedence over imported-model classification when both markers
            // exist on the same entity.
            bool isModelRoot = !(hasPrefabInstance || hasLegacyPrefab) &&
                (d->ModelAssetGuid.high != 0 || d->ModelAssetGuid.low != 0);
            
            if (hasPrefabInstance || hasLegacyPrefab) {
                json asset;
                asset["type"] = "prefab";
                
                if (hasPrefabInstance) {
                    // New unified system: use GUID and path from PrefabInstanceComponent
                    asset["guid"] = d->PrefabInstance->PrefabAssetGuid.ToString();
                    if (!d->PrefabInstance->PrefabPath.empty()) {
                        std::string v = d->PrefabInstance->PrefabPath; 
                        for (char& c : v) if (c=='\\') c='/';
                        asset["path"] = v;
                    }
                    
                    // Store simplified prefabInstance with just GUID mapping (no overrides - they're in children)
                    json simpleInstance;
                    simpleInstance["prefabGuid"] = d->PrefabInstance->PrefabAssetGuid;
                    if (!d->PrefabInstance->PrefabPath.empty()) {
                        simpleInstance["prefabPath"] = d->PrefabInstance->PrefabPath;
                    }
                    // Include GUID remapping for entity resolution within instance
                    if (!d->PrefabInstance->PrefabToInstanceGuid.empty()) {
                        simpleInstance["guidRemap"] = json::array();
                        for (const auto& [prefabGuid, instanceGuid] : d->PrefabInstance->PrefabToInstanceGuid) {
                            json entry;
                            entry["prefab"] = prefabGuid;
                            entry["instance"] = instanceGuid;
                            simpleInstance["guidRemap"].push_back(std::move(entry));
                        }
                    }
                    entityData["prefabInstance"] = std::move(simpleInstance);
                } else {
                    // Legacy system: use path from PrefabSource
                    std::string v = d->PrefabSource; 
                    for (char& c : v) if (c=='\\') c='/';
                    asset["path"] = v;
                }
                
                entityData["asset"] = std::move(asset);
                auto it = rootOverrides.find(eid);
                entityData["children"] = (it != rootOverrides.end()) ? it->second : nlohmann::json::array();
            }
            }
        }
        if (!entityData.empty()) sceneData["entities"].push_back(entityData);
    }

    return sceneData;
}

bool Serializer::DeserializeScene(const json& data, Scene& scene) {
    if (!data.contains("entities")) return false;
    // Default shader preset
    try {
        if (data.contains("defaultShaderPreset")) {
            int v = data["defaultShaderPreset"].get<int>();
            scene.SetDefaultShaderPreset((Scene::ShaderPreset)v);
        }
    } catch(...) {}

    try {
        std::cout << "[DeserializeBegin] version=" << data.value("version", "")
                  << " entities=" << (data.contains("entities") && data["entities"].is_array() ? data["entities"].size() : 0)
                  << std::endl;
    } catch(...) {}

    // If the scene carries an assetMap, pre-register GUID→path so asset references resolve
    try {
        if (data.contains("assetMap") && data["assetMap"].is_array()) {
            for (const auto& rec : data["assetMap"]) {
                std::string gstr = rec.value("guid", "");
                std::string vpath = rec.value("path", "");
                if (gstr.empty() || vpath.empty()) continue;
                vpath = NormalizePortableAssetPath(vpath);
                if (!IsMeshLikeAssetPath(vpath)) continue;
                ClaymoreGUID g = ClaymoreGUID::FromString(gstr);
                AssetLibrary::Instance().RegisterAsset(AssetReference(g, 0, (int)AssetType::Mesh), AssetType::Mesh, vpath, vpath);
            }
        }
    } catch(...) {}

    scene.ClearEditorViewportState();
    try {
        if (data.contains("editorState") && data["editorState"].is_object()) {
            const json& editorState = data["editorState"];
            if (editorState.contains("camera") && editorState["camera"].is_object()) {
                const json& camJson = editorState["camera"];
                Scene::EditorViewportState state;
                if (camJson.contains("target")) state.Target = DeserializeVec3(camJson["target"]);
                state.Yaw = camJson.value("yaw", state.Yaw);
                state.Pitch = camJson.value("pitch", state.Pitch);
                state.Distance = camJson.value("distance", state.Distance);
                state.FieldOfView = camJson.value("fov", state.FieldOfView);
                state.NearClip = camJson.value("nearClip", state.NearClip);
                state.FarClip = camJson.value("farClip", state.FarClip);
                scene.SetEditorViewportState(state);
            }
        }
    } catch(...) {}

    // Telemetry: count components and unknown blocks before mutating scene
    try {
        const auto& ents = data["entities"];
        size_t numEntities = ents.is_array() ? ents.size() : 0;
        size_t componentCount = 0;
        size_t unknownBlocks = 0;
        std::unordered_set<std::string> known = {
            "id","name","layer","tag","parent","children","guid","prefabSource",
            "transform","mesh","meshProxy","light","collider","rigidbody","staticbody","camera",
            "terrain","emitter","canvas","panel","button","scripts","animator","asset",
            "skeleton","skinning","ik","navmesh","navagent","navlink","portal"
        };
        std::unordered_set<std::string> guidSeen;
        size_t guidMissing = 0, guidDup = 0;
        for (const auto& e : ents) {
            if (!e.is_object()) continue;
            for (auto it = e.begin(); it != e.end(); ++it) {
                const std::string& k = it.key();
                if (k == "transform" || k == "mesh" || k == "meshProxy" || k == "light" || k == "collider" || k == "rigidbody" || k == "staticbody" || k == "camera" || k == "terrain" || k == "emitter" || k == "canvas" || k == "panel" || k == "button" || k == "scripts" || k == "animator" || k == "navlink" || k == "portal")
                    componentCount++;
                if (known.find(k) == known.end()) unknownBlocks++;
            }
            if (e.contains("guid")) {
                try {
                    ClaymoreGUID g; e.at("guid").get_to(g);
                    std::string gs = g.ToString();
                    if (!guidSeen.insert(gs).second) guidDup++;
                } catch(...) {}
            } else {
                guidMissing++;
            }
        }
        std::cout << "[Deserialize] version=" << data.value("version", "")
                  << " entities=" << numEntities
                  << " components=" << componentCount
                  << " unknown_blocks=" << unknownBlocks
                  << " guid_missing=" << guidMissing
                  << " guid_dupes=" << guidDup << std::endl;
    } catch(...) {}

    // Apply environment if present
    if (data.contains("environment") && data["environment"].is_object()) {
        try {
            Environment& env = scene.GetEnvironment();
            const json& jenv = data["environment"];
            std::string mode = jenv.value("ambientMode", "FlatColor");
            env.Ambient = (mode == "Skybox") ? Environment::AmbientMode::Skybox : Environment::AmbientMode::FlatColor;
            if (jenv.contains("ambientColor")) env.AmbientColor = DeserializeVec3(jenv["ambientColor"]);
            env.AmbientIntensity = jenv.value("ambientIntensity", env.AmbientIntensity);
            env.UseSkybox = jenv.value("useSkybox", env.UseSkybox);
            env.Exposure = jenv.value("exposure", env.Exposure);
            env.EnableFog = jenv.value("fogEnabled", env.EnableFog);
            if (jenv.contains("fogColor")) env.FogColor = DeserializeVec3(jenv["fogColor"]);
            env.FogDensity = jenv.value("fogDensity", env.FogDensity);
            env.ProceduralSky = jenv.value("proceduralSky", env.ProceduralSky);
            // Sky parameters (with defaults if not present)
            if (jenv.contains("skyTopColor")) env.SkyTopColor = DeserializeVec3(jenv["skyTopColor"]);
            else if (jenv.contains("skyTint")) env.SkyTopColor = DeserializeVec3(jenv["skyTint"]);
            if (jenv.contains("skyHorizonColor")) env.SkyHorizonColor = DeserializeVec3(jenv["skyHorizonColor"]);
            if (jenv.contains("skyGroundColor")) env.SkyGroundColor = DeserializeVec3(jenv["skyGroundColor"]);
            else if (jenv.contains("groundColor")) env.SkyGroundColor = DeserializeVec3(jenv["groundColor"]);
            env.SkyTint = env.SkyTopColor;
            env.GroundColor = env.SkyGroundColor;
            env.SunSize = jenv.value("sunSize", env.SunSize);
            env.SunSizeConvergence = jenv.value("sunSizeConvergence", env.SunSizeConvergence);
            env.SunIntensity = jenv.value("sunIntensity", env.SunIntensity);
            env.AtmosphereThickness = jenv.value("atmosphereThickness", env.AtmosphereThickness);
            env.HorizonFade = jenv.value("horizonFade", env.HorizonFade);
            env.SkyExposure = jenv.value("skyExposure", env.SkyExposure);
            // Cosmetic outline
            env.OutlineEnabled = jenv.value("outlineEnabled", env.OutlineEnabled);
            if (jenv.contains("outlineColor")) env.OutlineColor = DeserializeVec3(jenv["outlineColor"]);
            env.OutlineThickness = jenv.value("outlineThickness", env.OutlineThickness);
            // Shadows
            env.ShadowsEnabled = jenv.value("shadowsEnabled", env.ShadowsEnabled);
            env.ShadowMapResolution = jenv.value("shadowMapResolution", env.ShadowMapResolution);
            env.ShadowDistance = jenv.value("shadowDistance", env.ShadowDistance);
            env.ShadowBias = jenv.value("shadowBias", env.ShadowBias);
            env.ShadowNormalBias = jenv.value("shadowNormalBias", env.ShadowNormalBias);
            env.ShadowSoftness = jenv.value("shadowSoftness", env.ShadowSoftness);
            env.ShadowSamples = jenv.value("shadowSamples", env.ShadowSamples);
            env.ShadowStrength = jenv.value("shadowStrength", env.ShadowStrength);
            // Texture filtering preference
            try {
                std::string tf = jenv.value("textureFilter", std::string("Linear"));
                env.TextureFilter = (tf == "Point")
                    ? Environment::TextureFilterMode::Point
                    : Environment::TextureFilterMode::Linear;
            } catch(...) {}
        } catch(...) {}
    }

    // Apply global shader properties if present
    try {
        if (data.contains("globalShaderProperties") && data["globalShaderProperties"].is_object()) {
            GlobalShaderProperties::Instance().FromJson<json>(data["globalShaderProperties"]);
        }
    } catch(...) {}

    std::unordered_set<EntityID> persistentEntities;
    if (scene.m_IsPlaying) {
        try { persistentEntities = scene.CollectPersistentEntities(); } catch(...) {}
    }

    if (persistentEntities.empty()) {
        // Release PB texture handles before clearing entities to avoid leaks
        try { ReleasePBTextures(scene); } catch(...) {}
        // Clear existing scene by removing all entities
        std::vector<EntityID> entitiesToRemove;
        for (const auto& entity : scene.GetEntities()) {
            entitiesToRemove.push_back(entity.GetID());
        }
        
        for (EntityID id : entitiesToRemove) {
            scene.RemoveEntity(id);
        }
        // Reset ID counter so names don't receive incremental suffixes across reloads
        scene.ResetEntityIdCounter(1);
    } else {
        // Release PB texture handles for entities that are being removed
        for (const auto& entity : scene.GetEntities()) {
            EntityID id = entity.GetID();
            if (persistentEntities.find(id) != persistentEntities.end()) continue;
            auto* d = scene.GetEntityData(id);
            if (!d) continue;
            if (d->Mesh) {
                d->Mesh->PropertyBlock.Clear();
                for (auto& pb : d->Mesh->SlotPropertyBlocks) {
                    pb.Clear();
                }
            }
            if (d->MeshProxy) {
                d->MeshProxy->PropertyBlock.Clear();
                for (auto& pb : d->MeshProxy->SlotPropertyBlocks) {
                    pb.Clear();
                }
            }
        }

        std::vector<EntityID> entitiesToRemove;
        for (const auto& entity : scene.GetEntities()) {
            EntityID id = entity.GetID();
            if (persistentEntities.find(id) == persistentEntities.end()) {
                entitiesToRemove.push_back(id);
            }
        }
        for (EntityID id : entitiesToRemove) {
            scene.RemoveEntity(id);
        }

        EntityID maxId = 0;
        for (const auto& entity : scene.GetEntities()) {
            maxId = std::max(maxId, entity.GetID());
        }
        scene.ResetEntityIdCounter(maxId + 1);
    }
    
    // First pass: Create all entities
    std::unordered_map<EntityID, EntityID> idMapping; // old ID -> new ID
    // Keep track of roots that were instantiated from compact asset nodes (e.g., models).
    // Their internal hierarchy should remain intact; skip child clearing/parent fixup for them.
    std::unordered_set<EntityID> opaqueRoots;
    
    // Pre-scan: map oldId -> parentOld and set of all model-asset entity ids
    std::unordered_map<EntityID, EntityID> oldToParent;
    std::unordered_set<EntityID> modelAssetIds;
    for (const auto& ent : data["entities"]) {
        if (ent.contains("id") && ent.contains("parent")) {
            oldToParent[ ent["id"].get<EntityID>() ] = ent["parent"].get<EntityID>();
        }
        if (ent.contains("asset") && ent["asset"].is_object()) {
            const auto& a = ent["asset"]; if (a.value("type", "") == std::string("model") && ent.contains("id")) {
                modelAssetIds.insert(ent["id"].get<EntityID>());
            }
        }
    }

    auto isDescendantOfModelAsset = [&](EntityID oldId) -> bool {
        if (modelAssetIds.empty()) return false;
        EntityID cur = oldId;
        size_t guard = 0;
        while (cur != (EntityID)0 && cur != (EntityID)-1 && guard++ < 100000) {
            auto itp = oldToParent.find(cur);
            if (itp == oldToParent.end()) break;
            EntityID p = itp->second;
            if (modelAssetIds.count(p)) return true;
            cur = p;
        }
        return false;
    };

    auto looksModelNode = [&](const json& j) -> bool {
        auto has = [&](const char* k){ return j.contains(k); };
        // Model nodes have mesh, skeleton, skinning, or blendShapes
        bool hasModelComp = has("mesh") || has("skeleton") || has("skinning") || has("blendShapes");
        bool hasUserComp = has("camera") || has("light") || has("collider") || has("rigidbody") || has("staticbody") ||
                           has("emitter") || has("canvas") || has("panel") || has("button") || has("text") || has("scripts") || has("terrain") ||
                           has("navmesh") || has("navagent") || has("navlink") || has("portal") || has("area") || has("grassDeformer") || has("characterController") ||
                           (has("animator") && !j["animator"].is_null());
        return hasModelComp && !hasUserComp;
    };

    auto readStringField = [](const json& j, const char* key) -> std::string {
        try {
            if (j.contains(key) && j[key].is_string()) {
                return j[key].get<std::string>();
            }
        } catch (...) {}
        return {};
    };

    auto isNonZeroGuidString = [](const std::string& value) -> bool {
        if (value.empty()) return false;
        try {
            ClaymoreGUID guid = ClaymoreGUID::FromString(value);
            return !(guid.high == 0 && guid.low == 0);
        } catch (...) {
            return false;
        }
    };

    for (const auto& entityData : data["entities"]) {
        // Preserve names exactly as authored. Create with base name without suffixing.
        EntityID newId = 0;
        if (entityData.contains("name")) {
            // If this entry is a descendant of a model asset root and looks like an original model node,
            // skip creating it now to avoid duplicates. The importer will create the canonical node.
            if (entityData.contains("id")) {
                EntityID oldId = entityData["id"].get<EntityID>();
                if (!entityData.contains("asset") && isDescendantOfModelAsset(oldId) && looksModelNode(entityData)) {
                    std::cout << "[Skip] Model descendant original node id=" << oldId << " name=" << entityData["name"].get<std::string>() << std::endl;
                    // Intentionally do not map this oldId so parenting that targets it will be detected as unresolved
                    goto NEXT_ENTITY;
                }
            }
            // Handle compact asset node: instantiate model or prefab instead of raw entity
            json effectiveAsset = entityData.contains("asset") && entityData["asset"].is_object()
                ? entityData["asset"]
                : json::object();

            std::string prefabGuidStr = readStringField(entityData, "prefabGuid");
            std::string prefabPath = readStringField(entityData, "prefabSource");
            if (entityData.contains("prefabInstance") && entityData["prefabInstance"].is_object()) {
                const auto& prefabInstance = entityData["prefabInstance"];
                if (prefabGuidStr.empty()) prefabGuidStr = readStringField(prefabInstance, "prefabGuid");
                if (prefabPath.empty()) prefabPath = readStringField(prefabInstance, "prefabPath");
            }

            const bool hasPrefabIdentity = isNonZeroGuidString(prefabGuidStr) || !prefabPath.empty();
            if (hasPrefabIdentity) {
                if (effectiveAsset.value("type", "") == "model") {
                    try {
                        std::cout << "[SceneLoad] Migrating hybrid asset root '" << entityData["name"].get<std::string>()
                                  << "' from model to prefab using saved prefab markers\n";
                    } catch (...) {}
                }
                effectiveAsset["type"] = "prefab";
                if (isNonZeroGuidString(prefabGuidStr)) effectiveAsset["guid"] = prefabGuidStr;
                if (!prefabPath.empty()) effectiveAsset["path"] = prefabPath;
            } else {
                std::string modelGuidStr = readStringField(entityData, "modelAssetGuid");
                if (!modelGuidStr.empty()) {
                    effectiveAsset["type"] = "model";
                    effectiveAsset["guid"] = modelGuidStr;
                    if (effectiveAsset.value("path", "").empty() && isNonZeroGuidString(modelGuidStr)) {
                        try {
                            ClaymoreGUID modelGuid = ClaymoreGUID::FromString(modelGuidStr);
                            std::string resolvedPath = AssetLibrary::Instance().GetPathForGUID(modelGuid);
                            if (!resolvedPath.empty()) {
                                effectiveAsset["path"] = resolvedPath;
                                std::cout << "[SceneLoad] Resolved model asset path from GUID for '"
                                          << entityData["name"].get<std::string>() << "'\n";
                            }
                        } catch (...) {}
                    }
                }
            }

            if (effectiveAsset.is_object() && !effectiveAsset.empty()) {
                const auto& a = effectiveAsset;
                std::string type = a.value("type", "");
                if (type == "model") {
                    // Check if this nested model is duplicated in an ancestor model's children array.
                    // In the NEW format (2024+), nested models are NOT in parent's children array and should
                    // be instantiated normally. In OLD format, they were duplicated and should be skipped.
                    // We detect old format by checking if any ancestor model's children array contains
                    // an entry with matching modelAssetGuid or _modelNodePath.
                    bool shouldSkip = false;
                    if (entityData.contains("id") && entityData.contains("parent")) {
                        EntityID oldId = entityData["id"].get<EntityID>();
                        EntityID curParent = entityData["parent"].get<EntityID>();
                        while (curParent != (EntityID)0 && curParent != (EntityID)-1 && !shouldSkip) {
                            if (modelAssetIds.count(curParent)) {
                                // Parent (or ancestor) is a model asset root.
                                // Check if this entity is in the ancestor's children array (old format)
                                for (const auto& ancestorEntity : data["entities"]) {
                                    if (!ancestorEntity.contains("id")) continue;
                                    if (ancestorEntity["id"].get<EntityID>() != curParent) continue;
                                    if (!ancestorEntity.contains("children") || !ancestorEntity["children"].is_array()) continue;
                                    for (const auto& childOverride : ancestorEntity["children"]) {
                                        // Check if this child override represents our nested model
                                        if (childOverride.contains("modelAssetGuid")) {
                                            try {
                                                ClaymoreGUID mag;
                                                childOverride.at("modelAssetGuid").get_to(mag);
                                                // Also check the entity's modelAssetGuid
                                                ClaymoreGUID entityMag;
                                                if (entityData.contains("modelAssetGuid")) {
                                                    entityData.at("modelAssetGuid").get_to(entityMag);
                                                }
                                                if (mag == entityMag && !(mag.high == 0 && mag.low == 0)) {
                                                    // This nested model IS in the ancestor's children - skip (old format)
                                                    shouldSkip = true;
                                                    std::cout << "[Skip] Nested model in old format (duplicate in ancestor children)\n";
                                                    break;
                                                }
                                            } catch (...) {}
                                        }
                                    }
                                    break; // Found the ancestor entity
                                }
                            }
                            auto itp = oldToParent.find(curParent);
                            if (itp == oldToParent.end()) break;
                            curParent = itp->second;
                        }
                    }
                    if (shouldSkip) {
                        goto NEXT_ENTITY;
                    }
                    std::string p = a.value("path", "");
                    // Use project-root relative virtual path; prefer cached .meta fast path if present
                    std::string resolved = p;
                    if (!resolved.empty() && !fs::exists(resolved)) {
                        resolved = (Project::GetProjectDirectory() / p).string();
                    }
                    // Normalize slashes
                    for (char& c : resolved) if (c=='\\') c = '/';
                    // Register this model asset mapping so subsequent serialization/deserialization can resolve by GUID
                    try {
                        std::string gstr = a.value("guid", "");
                        if (!gstr.empty()) {
                            ClaymoreGUID g = ClaymoreGUID::FromString(gstr);
                            if (!(g.high == 0 && g.low == 0)) {
                                std::string v = p; for (char& ch : v) if (ch=='\\') ch = '/';
                                AssetLibrary::Instance().RegisterAsset(AssetReference(g, 0, (int)AssetType::Mesh), AssetType::Mesh, v, v);
                                if (!resolved.empty()) AssetLibrary::Instance().RegisterPathAlias(g, resolved);
                            }
                        }
                    } catch(...) {}
                    // Determine spawn position
                    glm::vec3 pos(0.0f);
                    if (entityData.contains("transform")) {
                        auto t = entityData["transform"];
                        if (t.contains("position")) pos = DeserializeVec3(t["position"]);
                    }
                    // Prefer sibling .meta (fast path)
                    std::string metaTry = resolved;
                    std::string ext = fs::path(resolved).extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".meta") {
                        fs::path rp(resolved);
                        fs::path metaPath = rp.parent_path() / (rp.stem().string() + ".meta");
                        if (fs::exists(metaPath)) metaTry = metaPath.string();
                    }
                    if (!metaTry.empty() && fs::path(metaTry).extension() == ".meta") {
                        // Deterministic, synchronous instantiation during deserialization to avoid
                        // race conditions that previously led to duplicate children and missing textures.
                        newId = scene.InstantiateModelFast(metaTry, pos, /*synchronous*/ true);
                        if (newId == (EntityID)0 || newId == (EntityID)-1) {
                            // Fallback to slow path if fast path failed
                            newId = scene.InstantiateModel(resolved, pos);
                        }
                    } else {
                        newId = scene.InstantiateModel(resolved, pos);
                    }
                    if (newId != 0) {
                        opaqueRoots.insert(newId);
                        // Apply transform fully to the root entity
                        if (auto* ed = scene.GetEntityData(newId)) {
                            if (entityData.contains("name")) { ed->Name = entityData["name"].get<std::string>(); }
                            if (entityData.contains("transform")) DeserializeTransform(entityData["transform"], ed->Transform);
                            
                            // Process deleted model nodes: restore the deletion list and remove those nodes
                            if (entityData.contains("deletedModelNodes") && entityData["deletedModelNodes"].is_array()) {
                                ed->DeletedModelNodes.clear();
                                for (const auto& path : entityData["deletedModelNodes"]) {
                                    if (path.is_string()) {
                                        std::string p = path.get<std::string>();
                                        if (!p.empty()) {
                                            ed->DeletedModelNodes.push_back(std::move(p));
                                        }
                                    }
                                }
                                // Build path-to-EntityID map for deletion (paths are RELATIVE to model root, not including root name)
                                std::unordered_map<std::string, EntityID> pathToEntity;
                                std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID id, const std::string& parentPath) {
                                    auto* data = scene.GetEntityData(id);
                                    if (!data) return;
                                    std::string nodePath = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                                    pathToEntity[nodePath] = id;
                                    for (EntityID c : data->Children) buildPathMap(c, nodePath);
                                };
                                // Start from children of root (paths exclude root name)
                                for (EntityID rootChild : ed->Children) {
                                    buildPathMap(rootChild, "");
                                }
                                // Delete nodes in the deletion list
                                for (const std::string& delPath : ed->DeletedModelNodes) {
                                    if (delPath.empty()) continue;
                                    auto it = pathToEntity.find(delPath);
                                    if (it != pathToEntity.end()) {
                                        scene.RemoveEntity(it->second);
                                        std::cout << "[SceneLoad] Removed deleted model node: " << delPath << std::endl;
                                    } else {
                                        std::cout << "[SceneLoad] WARNING: Could not find deleted node path: " << delPath << std::endl;
                                    }
                                }
                            }
                            // Apply scripts on root if any
                            if (entityData.contains("scripts")) DeserializeScripts(entityData["scripts"], ed->Scripts, scene);
                            if (entityData.contains("animator")) {
                                if (!ed->AnimationPlayer) ed->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
                                DeserializeAnimator(entityData["animator"], *ed->AnimationPlayer);
                            }
                            // Apply additional root-level components authored on the model root
                            if (entityData.contains("collider")) { if (!ed->Collider) ed->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(entityData["collider"], *ed->Collider); }
                            if (entityData.contains("rigidbody")) { if (!ed->RigidBody) ed->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(entityData["rigidbody"], *ed->RigidBody); }
                            if (entityData.contains("staticbody")) { if (!ed->StaticBody) ed->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(entityData["staticbody"], *ed->StaticBody); }
                            if (entityData.contains("characterController")) { if (!ed->CharacterController) ed->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(entityData["characterController"], *ed->CharacterController); }
                            if (entityData.contains("area")) { if (!ed->Area) ed->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(entityData["area"], *ed->Area); }
                            if (entityData.contains("camera")) { if (!ed->Camera) ed->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(entityData["camera"], *ed->Camera); }
                            if (entityData.contains("light")) { if (!ed->Light) ed->Light = std::make_unique<LightComponent>(); DeserializeLight(entityData["light"], *ed->Light); }
                            if (entityData.contains("audioSource")) { if (!ed->AudioSource) ed->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(entityData["audioSource"], *ed->AudioSource); }
                            if (entityData.contains("audioListener")) { if (!ed->AudioListener) ed->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(entityData["audioListener"], *ed->AudioListener); }
                            if (entityData.contains("terrain")) { if (!ed->Terrain) ed->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(entityData["terrain"], *ed->Terrain); }
                            if (entityData.contains("emitter")) { if (!ed->Emitter) ed->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(entityData["emitter"], *ed->Emitter); }
                            if (entityData.contains("canvas")) { if (!ed->Canvas) ed->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(entityData["canvas"], *ed->Canvas); }
                            if (entityData.contains("panel")) { if (!ed->Panel) ed->Panel = std::make_unique<PanelComponent>(); DeserializePanel(entityData["panel"], *ed->Panel); }
                            if (entityData.contains("button")) { if (!ed->Button) ed->Button = std::make_unique<ButtonComponent>(); DeserializeButton(entityData["button"], *ed->Button); }
                            if (entityData.contains("slider")) { if (!ed->Slider) ed->Slider = std::make_unique<SliderComponent>(); DeserializeSlider(entityData["slider"], *ed->Slider); }
                            if (entityData.contains("progressBar")) { if (!ed->ProgressBar) ed->ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(entityData["progressBar"], *ed->ProgressBar); }
                            if (entityData.contains("toggle")) { if (!ed->Toggle) ed->Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(entityData["toggle"], *ed->Toggle); }
                            if (entityData.contains("scrollView")) { if (!ed->ScrollView) ed->ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(entityData["scrollView"], *ed->ScrollView); }
                            if (entityData.contains("layoutGroup")) { if (!ed->LayoutGroup) ed->LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(entityData["layoutGroup"], *ed->LayoutGroup); }
                            if (entityData.contains("inputField")) { if (!ed->InputField) ed->InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(entityData["inputField"], *ed->InputField); }
                            if (entityData.contains("dropdown")) { if (!ed->Dropdown) ed->Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(entityData["dropdown"], *ed->Dropdown); }
                            if (entityData.contains("uiRect")) { if (!ed->UIRect) ed->UIRect = std::make_unique<UIRectComponent>(); DeserializeUIRect(entityData["uiRect"], *ed->UIRect); }
                            if (entityData.contains("fitToContent")) { if (!ed->FitToContent) ed->FitToContent = std::make_unique<FitToContentComponent>(); DeserializeFitToContent(entityData["fitToContent"], *ed->FitToContent); }
                            if (entityData.contains("uiSceneCapture")) { if (!ed->UISceneCapture) ed->UISceneCapture = std::make_unique<UISceneCaptureComponent>(); DeserializeUISceneCapture(entityData["uiSceneCapture"], *ed->UISceneCapture); }
                            if (entityData.contains("text")) { if (!ed->Text) ed->Text = std::make_unique<TextRendererComponent>(); DeserializeText(entityData["text"], *ed->Text); }
                            if (entityData.contains("navmesh")) { if (!ed->Navigation) ed->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(entityData["navmesh"], *ed->Navigation); }
                            if (entityData.contains("navagent")) { if (!ed->NavAgent) ed->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(entityData["navagent"], *ed->NavAgent); }
                            if (entityData.contains("navlink")) { if (!ed->NavLink) ed->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(entityData["navlink"], *ed->NavLink); }
                            if (entityData.contains("portal")) { if (!ed->Portal) ed->Portal = std::make_unique<PortalComponent>(); DeserializePortal(entityData["portal"], *ed->Portal); }
                            if (entityData.contains("portal")) { if (!ed->Portal) ed->Portal = std::make_unique<PortalComponent>(); DeserializePortal(entityData["portal"], *ed->Portal); }
                            if (entityData.contains("grassDeformer")) { if (!ed->GrassDeformer) ed->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(entityData["grassDeformer"], *ed->GrassDeformer); }
                            if (entityData.contains("renderOverrides")) { if (!ed->RenderOverrides) ed->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(entityData["renderOverrides"], *ed->RenderOverrides); }
                            // IK constraint blocks on model root
                            if (entityData.contains("ik") && entityData["ik"].is_array()) {
                                ed->IKs.clear();
                                for (const auto& j : entityData["ik"]) {
                                    if (!j.is_object()) continue;
                                    cm::animation::ik::IKComponent c;
                                    c.Enabled = j.value("enabled", true);
                                    c.TargetEntity = j.value("target", (EntityID)0);
                                    c.PoleEntity = j.value("pole", (EntityID)0);
                                    c.Weight = j.value("weight", 1.0f);
                                    c.MaxIterations = j.value("maxIterations", 12.0f);
                                    c.Tolerance = j.value("tolerance", 0.001f);
                                    c.Damping = j.value("damping", 0.2f);
                                    c.UseTwoBone = j.value("useTwoBone", true);
                                    c.LockAxisX = j.value("lockAxisX", false);
                                    c.LockAxisY = j.value("lockAxisY", false);
                                    c.LockAxisZ = j.value("lockAxisZ", false);
                                    c.Visualize = j.value("visualize", false);
                                    std::vector<cm::animation::ik::BoneId> chainBones;
                                    if (j.contains("chain") && j["chain"].is_array()) {
                                        for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                                    }
                                    c.SetChain(chainBones);
                                    c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                                    c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                                    if (j.contains("constraints") && j["constraints"].is_array()) {
                                        for (const auto& cj : j["constraints"]) {
                                            if (!cj.is_object()) continue;
                                            cm::animation::ik::IKComponent::Constraint cc;
                                            cc.useHinge = cj.value("useHinge", false);
                                            cc.useTwist = cj.value("useTwist", false);
                                            cc.hingeMinDeg = cj.value("hingeMinDeg", 0.0f);
                                            cc.hingeMaxDeg = cj.value("hingeMaxDeg", 0.0f);
                                            cc.twistMinDeg = cj.value("twistMinDeg", 0.0f);
                                            cc.twistMaxDeg = cj.value("twistMaxDeg", 0.0f);
                                            c.Constraints.push_back(cc);
                                        }
                                    }
                                    ed->IKs.push_back(std::move(c));
                                }
                            }
                            // LookAt constraint blocks on model root
                            if (entityData.contains("lookat") && entityData["lookat"].is_array()) {
                                ed->LookAtConstraints.clear();
                                for (const auto& j : entityData["lookat"]) {
                                    if (!j.is_object()) continue;
                                    cm::animation::lookat::LookAtConstraintComponent lac;
                                    lac.Enabled = j.value("enabled", true);
                                    lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                                    lac.TargetEntity = j.value("target", (EntityID)0);
                                    // Load target entity GUID for post-load resolution
                                    lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                                    lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                                    lac.Weight = j.value("weight", 1.0f);
                                    lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                                    lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                                    lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                                    lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                                    lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                                    lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                                    lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                                    lac.Visualize = j.value("visualize", false);
                                    if (j.contains("boneChain") && j["boneChain"].is_array()) {
                                        for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                                    }
                                    if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                                        for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                                    }
                                    ed->LookAtConstraints.push_back(std::move(lac));
                                }
                            }
                            // Apply visibility override for the model root
                            if (entityData.contains("visible")) { ed->Visible = entityData["visible"].get<bool>(); }
                            // Post-instantiate: if skeleton exists but BoneEntities unresolved, rebuild by name/path
                            std::function<SkeletonComponent*(EntityID, EntityID&)> findSkel = [&](EntityID id, EntityID& out)->SkeletonComponent*{
                                if (auto* d = scene.GetEntityData(id)) {
                                    if (d->Skeleton) { out = id; return d->Skeleton.get(); }
                                    for (EntityID c : d->Children) { if (auto* s = findSkel(c, out)) return s; }
                                }
                                return nullptr;
                            };
                            EntityID skelEntity = (EntityID)-1; if (auto* sk = findSkel(newId, skelEntity)) {
                                bool needsRebind = sk->BoneEntities.size() != sk->InverseBindPoses.size();
                                if (!needsRebind) {
                                    for (const auto& id : sk->BoneEntities) { if (id == (EntityID)-1) { needsRebind = true; break; } }
                                }
                                if (needsRebind) {
                                    std::unordered_map<std::string, EntityID> pathMap;
                                    std::function<void(EntityID, const std::string&)> dfs = [&](EntityID id, const std::string& path){
                                        if (auto* d = scene.GetEntityData(id)) {
                                            pathMap[path] = id;
                                            for (EntityID c : d->Children) if (auto* cd = scene.GetEntityData(c)) dfs(c, path.empty() ? cd->Name : (path + "/" + cd->Name));
                                        }
                                    };
                                    if (auto* rd = scene.GetEntityData(newId)) dfs(newId, rd->Name);
                                    const size_t n = sk->InverseBindPoses.size();
                                    sk->BoneEntities.assign(n, (EntityID)-1);
                                    // Build index->name list
                                    std::vector<std::string> boneNames(n, std::string());
                                    for (const auto& kv : sk->BoneNameToIndex) { int idx = kv.second; if (idx >= 0 && (size_t)idx < n) boneNames[(size_t)idx] = kv.first; }
                                    for (size_t i = 0; i < n; ++i) {
                                        const std::string& bname = boneNames[i];
                                        if (bname.empty()) continue;
                                        for (const auto& kv : pathMap) {
                                            const std::string& full = kv.first; size_t s = full.find_last_of('/');
                                            std::string last = (s == std::string::npos) ? full : full.substr(s+1);
                                            if (last == bname) { sk->BoneEntities[i] = kv.second; break; }
                                        }
                                    }
                                }
                            }
                            // Apply unified morph weights to the skeleton root entity (or model root if no skeleton)
                            if (entityData.contains("unifiedMorphWeights") && entityData["unifiedMorphWeights"].is_array()) {
                                // Find the entity with UnifiedMorphComponent (usually skeleton root)
                                EntityID morphTarget = (skelEntity != (EntityID)-1) ? skelEntity : newId;
                                auto* morphData = scene.GetEntityData(morphTarget);
                                if (morphData && morphData->UnifiedMorph) {
                                    for (const auto& entry : entityData["unifiedMorphWeights"]) {
                                        if (!entry.is_object() || !entry.contains("name") || !entry.contains("weight")) continue;
                                        std::string name = entry["name"].get<std::string>();
                                        float weight = entry["weight"].get<float>();
                                        for (size_t i = 0; i < morphData->UnifiedMorph->Names.size(); ++i) {
                                            if (morphData->UnifiedMorph->Names[i] == name) {
                                                morphData->UnifiedMorph->Weights[i] = weight;
                                                break;
                                            }
                                        }
                                    }
                                    // Propagate to child blend shapes
                                    for (EntityID meshId : morphData->UnifiedMorph->MemberMeshes) {
                                        auto* meshData = scene.GetEntityData(meshId);
                                        if (!meshData || !meshData->BlendShapes) continue;
                                        for (auto& shape : meshData->BlendShapes->Shapes) {
                                            for (size_t i = 0; i < morphData->UnifiedMorph->Names.size(); ++i) {
                                                if (shape.Name == morphData->UnifiedMorph->Names[i]) {
                                                    shape.Weight = morphData->UnifiedMorph->Weights[i];
                                                    meshData->BlendShapes->Dirty = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            // Apply tint controller to model root with path-to-EntityID resolution
                            if (entityData.contains("tintController") && entityData["tintController"].is_object()) {
                                if (!ed->TintController) ed->TintController = std::make_unique<TintMaskController>();
                                DeserializeTintController(entityData["tintController"], *ed->TintController);
                                
                                // Build path-to-EntityID map for the new model subtree
                                // IMPORTANT: Paths are relative to model root, NOT including the model root name
                                // This matches how serialization stores paths
                                std::unordered_map<std::string, EntityID> newPathMap;
                                newPathMap[""] = newId;  // Empty path = model root
                                std::function<void(EntityID, const std::string&)> buildNewPathMap = [&](EntityID nodeId, const std::string& parentPath) {
                                    auto* data = scene.GetEntityData(nodeId);
                                    if (!data) return;
                                    std::string path = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                                    newPathMap[path] = nodeId;
                                    for (EntityID c : data->Children) buildNewPathMap(c, path);
                                };
                                // Start from children of model root with empty parent path
                                // Also build normalized path map for fuzzy matching
                                std::unordered_map<std::string, EntityID> normalizedPathMap;
                                normalizedPathMap[""] = newId;  // Empty path = model root
                                if (ed) {
                                    for (EntityID child : ed->Children) {
                                        buildNewPathMap(child, "");
                                    }
                                    // Build normalized path map (strips _<digits> suffixes)
                                    for (const auto& [path, id] : newPathMap) {
                                        std::string normPath = prefab::NormalizePath(path);
                                        normalizedPathMap[normPath] = id;
                                    }
                                }
                                
                                // Resolve TintTarget paths to new EntityIDs
                                const auto& tintJson = entityData["tintController"];
                                if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
                                    size_t idx = 0;
                                    for (const auto& target : tintJson["targets"]) {
                                        if (idx < ed->TintController->Targets.size() && target.contains("entityPath")) {
                                            std::string path = target["entityPath"].get<std::string>();
                                            // Try exact path first
                                            auto pathIt = newPathMap.find(path);
                                            if (pathIt != newPathMap.end()) {
                                                ed->TintController->Targets[idx].TargetEntity = pathIt->second;
                                            } else {
                                                // Try normalized path (handles renamed entities like _97 -> _275)
                                                auto normIt = normalizedPathMap.find(prefab::NormalizePath(path));
                                                if (normIt != normalizedPathMap.end()) {
                                                    ed->TintController->Targets[idx].TargetEntity = normIt->second;
                                                } else {
                                                    // Path not found - mark as invalid
                                                    ed->TintController->Targets[idx].TargetEntity = (EntityID)-1;
                                                }
                                            }
                                        }
                                        idx++;
                                    }
                                }
                                ed->TintController->NeedsRefresh = true;
                            }
                        }
                    }
                    if (newId != 0 && entityData.contains("id")) {
                        idMapping[entityData["id"].get<EntityID>()] = newId;
                    }
                    continue; // handled compact node
                } else if (type == "prefab") {
                    // Try GUID-based instantiation first (new unified system)
                    std::string guidStr = a.value("guid", "");
                    std::string p = a.value("path", "");
                    
                    // Determine spawn position
                    glm::vec3 pos(0.0f);
                    if (entityData.contains("transform")) {
                        auto t = entityData["transform"]; if (t.contains("position")) pos = DeserializeVec3(t["position"]);
                    }
                    
                    // Instantiate prefab into scene
                    EntityID newId = (EntityID)-1;
                    
                    // Try GUID-based instantiation first
                    if (!guidStr.empty()) {
                        ClaymoreGUID prefabGuid = ClaymoreGUID::FromString(guidStr);
                        if (!(prefabGuid.high == 0 && prefabGuid.low == 0)) {
                            newId = InstantiatePrefab(prefabGuid, scene);
                        }
                    }
                    
                    // Fallback to path-based instantiation
                    if (newId == (EntityID)-1 && !p.empty()) {
                        std::string resolved = p;
                        if (!resolved.empty() && !fs::exists(resolved)) {
                            resolved = (Project::GetProjectDirectory() / p).string();
                        }
                        for (char& c : resolved) if (c=='\\') c = '/';
                        
                        std::string ext = fs::path(resolved).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext == ".prefab" || ext == ".json") {
                            newId = InstantiatePrefabFromPath(resolved, scene);
                        }
                    }
                    if (newId != (EntityID)-1 && newId != (EntityID)0) {
                        opaqueRoots.insert(newId);
                        // Apply authored root data
                        if (auto* ed = scene.GetEntityData(newId)) {
                            if (entityData.contains("name")) { ed->Name = entityData["name"].get<std::string>(); }
                            if (entityData.contains("transform")) DeserializeTransform(entityData["transform"], ed->Transform);
                            
                            // CRITICAL: Restore prefab instance GUIDs BEFORE deserializing scripts
                            // This ensures script entity references resolve correctly using saved GUIDs
                            if (entityData.contains("prefabInstance")) {
                                // Capture the fresh GUID mappings from instantiation BEFORE overwriting
                                std::unordered_map<ClaymoreGUID, ClaymoreGUID> freshInstanceToPrefab;
                                if (ed->PrefabInstance) {
                                    freshInstanceToPrefab = ed->PrefabInstance->InstanceToPrefabGuid;
                                }
                                
                                if (!ed->PrefabInstance) {
                                    ed->PrefabInstance = std::make_unique<PrefabInstanceComponent>();
                                }
                                
                                // Deserialize the saved prefabInstance data (this overwrites the fresh mappings)
                                DeserializePrefabInstance(entityData["prefabInstance"], *ed->PrefabInstance);
                                
                                // Restore saved instance GUIDs to match the scene's serialized references
                                // This ensures GUID-based reference resolution works correctly
                                if (!freshInstanceToPrefab.empty() && !ed->PrefabInstance->PrefabToInstanceGuid.empty()) {
                                    RestorePrefabInstanceGuids(scene, newId, freshInstanceToPrefab, *ed->PrefabInstance);
                                }
                            }
                            
                            if (entityData.contains("scripts")) DeserializeScripts(entityData["scripts"], ed->Scripts, scene);
                            if (entityData.contains("animator")) { if (!ed->AnimationPlayer) ed->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(entityData["animator"], *ed->AnimationPlayer); }
                            // Apply additional root-level components authored on the prefab root
                            if (entityData.contains("collider")) { if (!ed->Collider) ed->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(entityData["collider"], *ed->Collider); }
                            if (entityData.contains("rigidbody")) { if (!ed->RigidBody) ed->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(entityData["rigidbody"], *ed->RigidBody); }
                            if (entityData.contains("staticbody")) { if (!ed->StaticBody) ed->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(entityData["staticbody"], *ed->StaticBody); }
                            if (entityData.contains("characterController")) { if (!ed->CharacterController) ed->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(entityData["characterController"], *ed->CharacterController); }
                            if (entityData.contains("area")) { if (!ed->Area) ed->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(entityData["area"], *ed->Area); }
                            if (entityData.contains("camera")) { if (!ed->Camera) ed->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(entityData["camera"], *ed->Camera); }
                            if (entityData.contains("light")) { if (!ed->Light) ed->Light = std::make_unique<LightComponent>(); DeserializeLight(entityData["light"], *ed->Light); }
                            if (entityData.contains("audioSource")) { if (!ed->AudioSource) ed->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(entityData["audioSource"], *ed->AudioSource); }
                            if (entityData.contains("audioListener")) { if (!ed->AudioListener) ed->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(entityData["audioListener"], *ed->AudioListener); }
                            if (entityData.contains("terrain")) { if (!ed->Terrain) ed->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(entityData["terrain"], *ed->Terrain); }
                            if (entityData.contains("emitter")) { if (!ed->Emitter) ed->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(entityData["emitter"], *ed->Emitter); }
                            if (entityData.contains("canvas")) { if (!ed->Canvas) ed->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(entityData["canvas"], *ed->Canvas); }
                            if (entityData.contains("panel")) { if (!ed->Panel) ed->Panel = std::make_unique<PanelComponent>(); DeserializePanel(entityData["panel"], *ed->Panel); }
                            if (entityData.contains("button")) { if (!ed->Button) ed->Button = std::make_unique<ButtonComponent>(); DeserializeButton(entityData["button"], *ed->Button); }
                            if (entityData.contains("slider")) { if (!ed->Slider) ed->Slider = std::make_unique<SliderComponent>(); DeserializeSlider(entityData["slider"], *ed->Slider); }
                            if (entityData.contains("progressBar")) { if (!ed->ProgressBar) ed->ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(entityData["progressBar"], *ed->ProgressBar); }
                            if (entityData.contains("toggle")) { if (!ed->Toggle) ed->Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(entityData["toggle"], *ed->Toggle); }
                            if (entityData.contains("scrollView")) { if (!ed->ScrollView) ed->ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(entityData["scrollView"], *ed->ScrollView); }
                            if (entityData.contains("layoutGroup")) { if (!ed->LayoutGroup) ed->LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(entityData["layoutGroup"], *ed->LayoutGroup); }
                            if (entityData.contains("inputField")) { if (!ed->InputField) ed->InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(entityData["inputField"], *ed->InputField); }
                            if (entityData.contains("dropdown")) { if (!ed->Dropdown) ed->Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(entityData["dropdown"], *ed->Dropdown); }
                            if (entityData.contains("uiRect")) { if (!ed->UIRect) ed->UIRect = std::make_unique<UIRectComponent>(); DeserializeUIRect(entityData["uiRect"], *ed->UIRect); }
                            if (entityData.contains("fitToContent")) { if (!ed->FitToContent) ed->FitToContent = std::make_unique<FitToContentComponent>(); DeserializeFitToContent(entityData["fitToContent"], *ed->FitToContent); }
                            if (entityData.contains("uiSceneCapture")) { if (!ed->UISceneCapture) ed->UISceneCapture = std::make_unique<UISceneCaptureComponent>(); DeserializeUISceneCapture(entityData["uiSceneCapture"], *ed->UISceneCapture); }
                            if (entityData.contains("text")) { if (!ed->Text) ed->Text = std::make_unique<TextRendererComponent>(); DeserializeText(entityData["text"], *ed->Text); }
                            if (entityData.contains("navmesh")) { if (!ed->Navigation) ed->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(entityData["navmesh"], *ed->Navigation); }
                            if (entityData.contains("navagent")) { if (!ed->NavAgent) ed->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(entityData["navagent"], *ed->NavAgent); }
                            if (entityData.contains("grassDeformer")) { if (!ed->GrassDeformer) ed->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(entityData["grassDeformer"], *ed->GrassDeformer); }
                            if (entityData.contains("renderOverrides")) { if (!ed->RenderOverrides) ed->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(entityData["renderOverrides"], *ed->RenderOverrides); }
                            if (entityData.contains("visible")) { ed->Visible = entityData["visible"].get<bool>(); }
                            if (entityData.contains("tintController")) { if (!ed->TintController) ed->TintController = std::make_unique<TintMaskController>(); DeserializeTintController(entityData["tintController"], *ed->TintController); }
                            // IK constraint blocks on prefab root
                            if (entityData.contains("ik") && entityData["ik"].is_array()) {
                                ed->IKs.clear();
                                for (const auto& j : entityData["ik"]) {
                                    if (!j.is_object()) continue;
                                    cm::animation::ik::IKComponent c;
                                    c.Enabled = j.value("enabled", true);
                                    c.TargetEntity = j.value("target", (EntityID)0);
                                    c.PoleEntity = j.value("pole", (EntityID)0);
                                    c.Weight = j.value("weight", 1.0f);
                                    c.MaxIterations = j.value("maxIterations", 12.0f);
                                    c.Tolerance = j.value("tolerance", 0.001f);
                                    c.Damping = j.value("damping", 0.2f);
                                    c.UseTwoBone = j.value("useTwoBone", true);
                                    c.LockAxisX = j.value("lockAxisX", false);
                                    c.LockAxisY = j.value("lockAxisY", false);
                                    c.LockAxisZ = j.value("lockAxisZ", false);
                                    c.Visualize = j.value("visualize", false);
                                    std::vector<cm::animation::ik::BoneId> chainBones;
                                    if (j.contains("chain") && j["chain"].is_array()) {
                                        for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                                    }
                                    c.SetChain(chainBones);
                                    c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                                    c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                                    if (j.contains("constraints") && j["constraints"].is_array()) {
                                        for (const auto& cj : j["constraints"]) {
                                            if (!cj.is_object()) continue;
                                            cm::animation::ik::IKComponent::Constraint cc;
                                            cc.useHinge = cj.value("useHinge", false);
                                            cc.useTwist = cj.value("useTwist", false);
                                            cc.hingeMinDeg = cj.value("hingeMinDeg", 0.0f);
                                            cc.hingeMaxDeg = cj.value("hingeMaxDeg", 0.0f);
                                            cc.twistMinDeg = cj.value("twistMinDeg", 0.0f);
                                            cc.twistMaxDeg = cj.value("twistMaxDeg", 0.0f);
                                            c.Constraints.push_back(cc);
                                        }
                                    }
                                    ed->IKs.push_back(std::move(c));
                                }
                            }
                            // LookAt constraint blocks on prefab root
                            if (entityData.contains("lookat") && entityData["lookat"].is_array()) {
                                ed->LookAtConstraints.clear();
                                for (const auto& j : entityData["lookat"]) {
                                    if (!j.is_object()) continue;
                                    cm::animation::lookat::LookAtConstraintComponent lac;
                                    lac.Enabled = j.value("enabled", true);
                                    lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                                    lac.TargetEntity = j.value("target", (EntityID)0);
                                    // Load target entity GUID for post-load resolution
                                    lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                                    lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                                    lac.Weight = j.value("weight", 1.0f);
                                    lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                                    lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                                    lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                                    lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                                    lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                                    lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                                    lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                                    lac.Visualize = j.value("visualize", false);
                                    if (j.contains("boneChain") && j["boneChain"].is_array()) {
                                        for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                                    }
                                    if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                                        for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                                    }
                                    ed->LookAtConstraints.push_back(std::move(lac));
                                }
                            }
                            // Record prefab source for compact reserialization
                            try { std::string v = p; for (char& ch : v) if (ch=='\\') ch = '/'; ed->PrefabSource = v; } catch(...) {}
                            
                            // NOTE: PrefabInstanceComponent GUID restoration was moved earlier (before script deserialization)
                            // to ensure script entity references resolve correctly. Overrides are still applied later
                            // in the prefab children override loop below (same pattern as model children)
                        }
                        if (entityData.contains("id")) {
                            idMapping[entityData["id"].get<EntityID>()] = newId;
                        }
                    }
                    continue; // handled compact node
                }
            }
            // Create a temporary entity and immediately set the exact name
            Entity temp = scene.CreateEntityExact(entityData["name"]);
            newId = temp.GetID();
            auto* ed = scene.GetEntityData(newId);
            if (ed) ed->Name = entityData["name"]; // avoid auto-suffix pattern
            try {
                std::cout << "[Create] guid=" << ed->EntityGuid.ToString() << " name=" << ed->Name << " src=Deserialize" << std::endl;
            } catch(...) {}
            // Fill remaining fields by deserializing into this entity
            // Reuse DeserializeEntity by overriding name to avoid creating a second entity
            // We'll simulate by building a new json without name to skip creation
            json copy = entityData;
            copy.erase("name");
            // Write id to ensure relationships mapping still works
            // Then call DeserializeEntity on a synthetic structure that keeps properties
            // but our DeserializeEntity currently always creates a new entity.
            // Instead, manually apply properties here for the created entity:
            if (copy.contains("layer")) ed->Layer = copy["layer"];
            if (copy.contains("tag")) ed->Tag = copy["tag"];
            if (copy.contains("visible")) ed->Visible = copy["visible"];
            if (copy.contains("parent")) ed->Parent = copy["parent"];
            if (copy.contains("children")) {
                ed->Children.clear();
                for (const auto& child : copy["children"]) ed->Children.push_back(child.get<EntityID>());
            }
            // GUID & prefab source
            if (copy.contains("guid")) {
                try { copy.at("guid").get_to(ed->EntityGuid); } catch(...) {}
            } else {
                ed->EntityGuid = ClaymoreGUID::Generate();
            }
            if (copy.contains("prefabSource")) {
                try { ed->PrefabSource = IVirtualFS::NormalizePath(copy.at("prefabSource").get<std::string>()); } catch(...) {}
            }
            if (copy.contains("transform")) DeserializeTransform(copy["transform"], ed->Transform);
            if (copy.contains("mesh")) { MeshComponent tmp; DeserializeMesh(copy["mesh"], tmp, ed->RenderOverrides); ed->Mesh = std::make_unique<MeshComponent>(std::move(tmp)); }
            if (copy.contains("meshProxy")) { if (!ed->MeshProxy) ed->MeshProxy = std::make_unique<MeshProxyComponent>(); DeserializeMeshProxy(copy["meshProxy"], *ed->MeshProxy, scene); }
            if (copy.contains("light")) { ed->Light = std::make_unique<LightComponent>(); DeserializeLight(copy["light"], *ed->Light); }
            if (copy.contains("audioSource")) { ed->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(copy["audioSource"], *ed->AudioSource); }
            if (copy.contains("audioListener")) { ed->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(copy["audioListener"], *ed->AudioListener); }
            if (copy.contains("collider")) { ed->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(copy["collider"], *ed->Collider); }
            if (copy.contains("rigidbody")) { ed->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(copy["rigidbody"], *ed->RigidBody); }
            if (copy.contains("staticbody")) { ed->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(copy["staticbody"], *ed->StaticBody); }
            if (copy.contains("characterController")) { ed->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(copy["characterController"], *ed->CharacterController); }
            if (copy.contains("camera")) { ed->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(copy["camera"], *ed->Camera); }
            if (copy.contains("terrain")) { ed->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(copy["terrain"], *ed->Terrain); }
            if (copy.contains("resourceLayers")) { ed->ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>(); DeserializeResourceLayers(copy["resourceLayers"], *ed->ResourceLayers); }
            if (copy.contains("instancer")) { ed->Instancer = std::make_unique<cm::instancer::InstancerComponent>(); DeserializeInstancer(copy["instancer"], *ed->Instancer); }
            if (copy.contains("river")) { ed->River = std::make_unique<RiverComponent>(); DeserializeRiver(copy["river"], *ed->River); }
            if (copy.contains("spline")) { ed->Spline = std::make_unique<SplineComponent>(); DeserializeSpline(copy["spline"], *ed->Spline); }
            if (copy.contains("emitter")) { ed->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(copy["emitter"], *ed->Emitter); }
            if (copy.contains("canvas")) { ed->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(copy["canvas"], *ed->Canvas); }
            if (copy.contains("panel")) { ed->Panel = std::make_unique<PanelComponent>(); DeserializePanel(copy["panel"], *ed->Panel); }
            if (copy.contains("button")) { ed->Button = std::make_unique<ButtonComponent>(); DeserializeButton(copy["button"], *ed->Button); }
            if (copy.contains("slider")) { ed->Slider = std::make_unique<SliderComponent>(); DeserializeSlider(copy["slider"], *ed->Slider); }
            if (copy.contains("progressBar")) { ed->ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(copy["progressBar"], *ed->ProgressBar); }
            if (copy.contains("toggle")) { ed->Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(copy["toggle"], *ed->Toggle); }
            if (copy.contains("scrollView")) { ed->ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(copy["scrollView"], *ed->ScrollView); }
            if (copy.contains("layoutGroup")) { ed->LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(copy["layoutGroup"], *ed->LayoutGroup); }
            if (copy.contains("inputField")) { ed->InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(copy["inputField"], *ed->InputField); }
            if (copy.contains("dropdown")) { ed->Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(copy["dropdown"], *ed->Dropdown); }
            if (copy.contains("uiSceneCapture")) { ed->UISceneCapture = std::make_unique<UISceneCaptureComponent>(); DeserializeUISceneCapture(copy["uiSceneCapture"], *ed->UISceneCapture); }
            if (copy.contains("text")) { ed->Text = std::make_unique<TextRendererComponent>(); DeserializeText(copy["text"], *ed->Text); }
            if (copy.contains("scripts")) { DeserializeScripts(copy["scripts"], ed->Scripts, scene); }
            if (copy.contains("animator")) { if (!ed->AnimationPlayer) ed->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(copy["animator"], *ed->AnimationPlayer); }
            if (copy.contains("area")) { ed->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(copy["area"], *ed->Area); }
            if (copy.contains("navmesh")) { ed->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(copy["navmesh"], *ed->Navigation); }
            if (copy.contains("navagent")) { ed->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(copy["navagent"], *ed->NavAgent); }
            if (copy.contains("navlink")) { ed->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(copy["navlink"], *ed->NavLink); }
            if (copy.contains("portal")) { ed->Portal = std::make_unique<PortalComponent>(); DeserializePortal(copy["portal"], *ed->Portal); }
            if (copy.contains("grassDeformer")) { ed->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(copy["grassDeformer"], *ed->GrassDeformer); }
            if (copy.contains("renderOverrides")) { ed->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(copy["renderOverrides"], *ed->RenderOverrides); }
            if (copy.contains("tintController")) { ed->TintController = std::make_unique<TintMaskController>(); DeserializeTintController(copy["tintController"], *ed->TintController); }
            // Preserve unknown fields
            try {
                static const std::unordered_set<std::string> kKnown = {
                    "id","name","layer","tag","parent","children","guid","prefabSource","visible",
                    "transform","mesh","meshProxy","light","collider","rigidbody","staticbody","camera",
                    "terrain","resourceLayers","instancer","river","emitter","canvas","panel","button","text","scripts","animator","asset",
                    "skeleton","skinning","blendShapeWeights","unifiedMorphWeights","tintController",
                    "navmesh","navagent","navlink","portal","area","grassDeformer","renderOverrides","characterController",
                    "slider","progressBar","toggle","scrollView","layoutGroup","inputField","dropdown","uiSceneCapture"
                };
                ed->Extra = nlohmann::json::object();
                for (auto it = entityData.begin(); it != entityData.end(); ++it) {
                    if (kKnown.find(it.key()) == kKnown.end()) {
                        ed->Extra[it.key()] = it.value();
                    }
                }
            } catch(...) {}
        } else {
            newId = DeserializeEntity(entityData, scene);
        }
        if (newId != 0 && entityData.contains("id")) {
            EntityID oldId = entityData["id"];
            idMapping[oldId] = newId;
        }
        NEXT_ENTITY: ;
    }

    // Parallelize component population for non-opaque roots (safe, no bgfx calls here)
    if (!idMapping.empty()) {
        std::vector<const json*> work;
        work.reserve(idMapping.size());
        for (const auto& entityData : data["entities"]) {
            if (!entityData.contains("id")) continue;
            EntityID oldId = entityData["id"].get<EntityID>();
            auto it = idMapping.find(oldId);
            if (it == idMapping.end()) continue;
            EntityID nid = it->second;
            if (opaqueRoots.find(nid) != opaqueRoots.end()) continue; // skip compact model asset roots
            // Also skip if this JSON node was a compact model node (handled already)
            if (entityData.contains("asset") && entityData["asset"].is_object()) continue;
            work.push_back(&entityData);
        }
        if (!work.empty()) {
            auto& js = Jobs();
            const size_t chunk = 32;
            parallel_for(js, size_t(0), work.size(), chunk, [&](size_t s, size_t c){
                for (size_t off = 0; off < c; ++off) {
                    const json& entityData = *work[s + off];
                    EntityID oldId = entityData.value("id", (EntityID)0);
                    auto it = idMapping.find(oldId); if (it == idMapping.end()) continue;
                    EntityID nid = it->second;
                    auto* ed = scene.GetEntityData(nid); if (!ed) continue;
                    // Transform
                    if (entityData.contains("transform")) { DeserializeTransform(entityData["transform"], ed->Transform); }
                    // Component shells + JSON decode (no GPU work)
                    if (entityData.contains("mesh")) { MeshComponent tmp; DeserializeMesh(entityData["mesh"], tmp, ed->RenderOverrides); if (!ed->Mesh) ed->Mesh = std::make_unique<MeshComponent>(std::move(tmp)); else *ed->Mesh = std::move(tmp); }
                    if (entityData.contains("meshProxy")) { if (!ed->MeshProxy) ed->MeshProxy = std::make_unique<MeshProxyComponent>(); DeserializeMeshProxy(entityData["meshProxy"], *ed->MeshProxy, scene); }
                    if (entityData.contains("light")) { if (!ed->Light) ed->Light = std::make_unique<LightComponent>(); DeserializeLight(entityData["light"], *ed->Light); }
                    if (entityData.contains("audioSource")) { if (!ed->AudioSource) ed->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(entityData["audioSource"], *ed->AudioSource); }
                    if (entityData.contains("audioListener")) { if (!ed->AudioListener) ed->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(entityData["audioListener"], *ed->AudioListener); }
                    if (entityData.contains("collider")) { if (!ed->Collider) ed->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(entityData["collider"], *ed->Collider); }
                    if (entityData.contains("rigidbody")) { if (!ed->RigidBody) ed->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(entityData["rigidbody"], *ed->RigidBody); }
                    if (entityData.contains("staticbody")) { if (!ed->StaticBody) ed->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(entityData["staticbody"], *ed->StaticBody); }
                    if (entityData.contains("characterController")) { if (!ed->CharacterController) ed->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(entityData["characterController"], *ed->CharacterController); }
                    if (entityData.contains("camera")) { if (!ed->Camera) ed->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(entityData["camera"], *ed->Camera); }
                    if (entityData.contains("terrain")) { if (!ed->Terrain) ed->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(entityData["terrain"], *ed->Terrain); }
                    if (entityData.contains("emitter")) { if (!ed->Emitter) ed->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(entityData["emitter"], *ed->Emitter); }
                    if (entityData.contains("canvas")) { if (!ed->Canvas) ed->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(entityData["canvas"], *ed->Canvas); }
                    if (entityData.contains("panel")) { if (!ed->Panel) ed->Panel = std::make_unique<PanelComponent>(); DeserializePanel(entityData["panel"], *ed->Panel); }
                    if (entityData.contains("button")) { if (!ed->Button) ed->Button = std::make_unique<ButtonComponent>(); DeserializeButton(entityData["button"], *ed->Button); }
                    if (entityData.contains("slider")) { if (!ed->Slider) ed->Slider = std::make_unique<SliderComponent>(); DeserializeSlider(entityData["slider"], *ed->Slider); }
                    if (entityData.contains("progressBar")) { if (!ed->ProgressBar) ed->ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(entityData["progressBar"], *ed->ProgressBar); }
                    if (entityData.contains("toggle")) { if (!ed->Toggle) ed->Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(entityData["toggle"], *ed->Toggle); }
                    if (entityData.contains("scrollView")) { if (!ed->ScrollView) ed->ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(entityData["scrollView"], *ed->ScrollView); }
                    if (entityData.contains("layoutGroup")) { if (!ed->LayoutGroup) ed->LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(entityData["layoutGroup"], *ed->LayoutGroup); }
                    if (entityData.contains("inputField")) { if (!ed->InputField) ed->InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(entityData["inputField"], *ed->InputField); }
                    if (entityData.contains("dropdown")) { if (!ed->Dropdown) ed->Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(entityData["dropdown"], *ed->Dropdown); }
                    if (entityData.contains("uiRect")) { if (!ed->UIRect) ed->UIRect = std::make_unique<UIRectComponent>(); DeserializeUIRect(entityData["uiRect"], *ed->UIRect); }
                    if (entityData.contains("fitToContent")) { if (!ed->FitToContent) ed->FitToContent = std::make_unique<FitToContentComponent>(); DeserializeFitToContent(entityData["fitToContent"], *ed->FitToContent); }
                    if (entityData.contains("uiSceneCapture")) { if (!ed->UISceneCapture) ed->UISceneCapture = std::make_unique<UISceneCaptureComponent>(); DeserializeUISceneCapture(entityData["uiSceneCapture"], *ed->UISceneCapture); }
                    if (entityData.contains("text")) { if (!ed->Text) ed->Text = std::make_unique<TextRendererComponent>(); DeserializeText(entityData["text"], *ed->Text); }
                    // Navigation components
                    if (entityData.contains("navmesh")) { if (!ed->Navigation) ed->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(entityData["navmesh"], *ed->Navigation); }
                    if (entityData.contains("navagent")) { if (!ed->NavAgent) ed->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(entityData["navagent"], *ed->NavAgent); }
                    if (entityData.contains("navlink")) { if (!ed->NavLink) ed->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(entityData["navlink"], *ed->NavLink); }
                    if (entityData.contains("portal")) { if (!ed->Portal) ed->Portal = std::make_unique<PortalComponent>(); DeserializePortal(entityData["portal"], *ed->Portal); }
                    if (entityData.contains("area")) { if (!ed->Area) ed->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(entityData["area"], *ed->Area); }
                    if (entityData.contains("grassDeformer")) { if (!ed->GrassDeformer) ed->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(entityData["grassDeformer"], *ed->GrassDeformer); }
                    if (entityData.contains("renderOverrides")) { if (!ed->RenderOverrides) ed->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(entityData["renderOverrides"], *ed->RenderOverrides); }
                    if (entityData.contains("tintController")) { if (!ed->TintController) ed->TintController = std::make_unique<TintMaskController>(); DeserializeTintController(entityData["tintController"], *ed->TintController); }
                    if (entityData.contains("scripts")) { DeserializeScripts(entityData["scripts"], ed->Scripts, scene); }
                    if (entityData.contains("animator")) { if (!ed->AnimationPlayer) ed->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(entityData["animator"], *ed->AnimationPlayer); }
                    if (entityData.contains("skeleton")) { if (!ed->Skeleton) ed->Skeleton = std::make_unique<SkeletonComponent>(); DeserializeSkeleton(entityData["skeleton"], *ed->Skeleton); }
                    if (entityData.contains("skinning")) { if (!ed->Skinning) ed->Skinning = std::make_unique<SkinningComponent>(); DeserializeSkinning(entityData["skinning"], *ed->Skinning); TryResolveSkinningSkeletonRootRef(entityData["skinning"], *ed->Skinning, scene); }
                    // IK constraint blocks
                    if (entityData.contains("ik") && entityData["ik"].is_array()) {
                        ed->IKs.clear();
                        for (const auto& j : entityData["ik"]) {
                            if (!j.is_object()) continue;
                            cm::animation::ik::IKComponent c;
                            c.Enabled = j.value("enabled", true);
                            c.TargetEntity = j.value("target", (EntityID)0);
                            c.PoleEntity = j.value("pole", (EntityID)0);
                            c.Weight = j.value("weight", 1.0f);
                            c.MaxIterations = j.value("maxIterations", 12.0f);
                            c.Tolerance = j.value("tolerance", 0.001f);
                            c.Damping = j.value("damping", 0.2f);
                            c.UseTwoBone = j.value("useTwoBone", true);
                            c.LockAxisX = j.value("lockAxisX", false);
                            c.LockAxisY = j.value("lockAxisY", false);
                            c.LockAxisZ = j.value("lockAxisZ", false);
                            c.Visualize = j.value("visualize", false);
                            std::vector<cm::animation::ik::BoneId> chainBones;
                            if (j.contains("chain") && j["chain"].is_array()) {
                                for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                            }
                            c.SetChain(chainBones);
                            c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                            c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                            if (j.contains("constraints") && j["constraints"].is_array()) {
                                for (const auto& cj : j["constraints"]) {
                                    if (!cj.is_object()) continue;
                                    cm::animation::ik::IKComponent::Constraint cc;
                                    cc.useHinge = cj.value("useHinge", false);
                                    cc.useTwist = cj.value("useTwist", false);
                                    cc.hingeMinDeg = cj.value("hingeMinDeg", 0.0f);
                                    cc.hingeMaxDeg = cj.value("hingeMaxDeg", 0.0f);
                                    cc.twistMinDeg = cj.value("twistMinDeg", 0.0f);
                                    cc.twistMaxDeg = cj.value("twistMaxDeg", 0.0f);
                                    c.Constraints.push_back(cc);
                                }
                            }
                            ed->IKs.push_back(std::move(c));
                        }
                    }
                    // LookAt constraint blocks
                    if (entityData.contains("lookat") && entityData["lookat"].is_array()) {
                        ed->LookAtConstraints.clear();
                        for (const auto& j : entityData["lookat"]) {
                            if (!j.is_object()) continue;
                            cm::animation::lookat::LookAtConstraintComponent lac;
                            lac.Enabled = j.value("enabled", true);
                            lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                            lac.TargetEntity = j.value("target", (EntityID)0);
                            // Load target entity GUID for post-load resolution
                            lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                            lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                            lac.Weight = j.value("weight", 1.0f);
                            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                            lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                            lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                            lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                            lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                            lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                            lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                            lac.Visualize = j.value("visualize", false);
                            if (j.contains("boneChain") && j["boneChain"].is_array()) {
                                for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                            }
                            if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                                for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                            }
                            ed->LookAtConstraints.push_back(std::move(lac));
                        }
                    }
                }
            });
        }
    }

    // Reset children vectors to avoid duplicates for non-opaque roots, and clear Parent to drop stale old-ID links
    // before running the parent fix-up pass.
    for (const auto& [oldId, newId] : idMapping) {
        if (opaqueRoots.find(newId) != opaqueRoots.end()) continue;
        if (auto* ed = scene.GetEntityData(newId)) {
            ed->Children.clear();
            ed->Parent = (EntityID)-1; // ensure no accidental self/old-id parent persists
        }
    }
    // Second pass: Fix up parent-child relationships with stable sibling ordering.
    // Build parent -> children list from parent field (authoritative for membership).
    std::unordered_map<EntityID, std::vector<EntityID>> parentToChildrenOld;
    parentToChildrenOld.reserve(idMapping.size());
    for (const auto& entityData : data["entities"]) {
        if (!entityData.contains("id") || !entityData.contains("parent")) continue;
        EntityID oldId = entityData["id"];
        if (idMapping.find(oldId) == idMapping.end()) continue;
        EntityID oldParent = entityData["parent"];
        parentToChildrenOld[oldParent].push_back(oldId);
    }
    // Capture explicit children order when the array is a list of ids.
    std::unordered_map<EntityID, std::vector<EntityID>> parentOrder;
    parentOrder.reserve(parentToChildrenOld.size());
    for (const auto& entityData : data["entities"]) {
        if (!entityData.contains("id") || !entityData.contains("children")) continue;
        const auto& children = entityData["children"];
        if (!children.is_array()) continue;
        bool allIds = true;
        for (const auto& child : children) {
            if (!child.is_number_integer()) { allIds = false; break; }
        }
        if (!allIds) continue;
        EntityID oldParent = entityData["id"];
        auto& order = parentOrder[oldParent];
        order.reserve(children.size());
        for (const auto& child : children) {
            order.push_back(child.get<EntityID>());
        }
    }
    // Apply parenting in order: explicit children order first, then remaining.
    for (auto& kv : parentToChildrenOld) {
        EntityID oldParent = kv.first;
        auto itParent = idMapping.find(oldParent);
        if (itParent == idMapping.end()) continue;
        EntityID parentNew = itParent->second;
        if (parentNew == INVALID_ENTITY_ID) continue;

        std::vector<EntityID> ordered;
        ordered.reserve(kv.second.size());
        std::unordered_set<EntityID> used;
        used.reserve(kv.second.size());

        auto itOrder = parentOrder.find(oldParent);
        if (itOrder != parentOrder.end()) {
            for (EntityID childOld : itOrder->second) {
                if (std::find(kv.second.begin(), kv.second.end(), childOld) == kv.second.end()) continue;
                ordered.push_back(childOld);
                used.insert(childOld);
            }
        }
        for (EntityID childOld : kv.second) {
            if (used.find(childOld) != used.end()) continue;
            ordered.push_back(childOld);
        }

        for (EntityID oldChild : ordered) {
            auto itChild = idMapping.find(oldChild);
            if (itChild == idMapping.end()) continue;
            EntityID childNew = itChild->second;
            if (childNew == INVALID_ENTITY_ID) continue;
            bool childOpaque = opaqueRoots.find(childNew) != opaqueRoots.end();
            bool parentOpaque = opaqueRoots.find(parentNew) != opaqueRoots.end();
            if (childOpaque && parentOpaque) continue;
            scene.SetParent(childNew, parentNew);
        }
    }
    
    // Third pass: Fix up nested models with path-based parent references
    // These are models whose parent is inside another model's hierarchy (e.g., armor parented under skeleton root)
    for (const auto& entityData : data["entities"]) {
        if (!entityData.contains("_parentModelGuid") || !entityData.contains("_parentPath")) continue;
        if (!entityData.contains("id")) continue;
        
        EntityID oldId = entityData["id"].get<EntityID>();
        auto itChild = idMapping.find(oldId);
        if (itChild == idMapping.end()) continue;
        EntityID childNew = itChild->second;
        if (childNew == INVALID_ENTITY_ID || childNew == (EntityID)-1) continue;
        
        std::string parentModelGuid = entityData["_parentModelGuid"].get<std::string>();
        std::string parentPath = entityData["_parentPath"].get<std::string>();
        
        // Get old parent model ID for disambiguation (when multiple models have same GUID)
        EntityID oldParentModelId = (EntityID)-1;
        if (entityData.contains("_parentModelId")) {
            oldParentModelId = entityData["_parentModelId"].get<EntityID>();
        }
        
        // Find the model root with matching GUID in our newly instantiated models
        // Prefer matching by old ID if available (for disambiguation with multiple instances)
        EntityID parentModelRoot = (EntityID)-1;
        EntityID guidOnlyMatch = (EntityID)-1;  // Fallback if no ID match
        
        for (EntityID opaqueRoot : opaqueRoots) {
            auto* rootData = scene.GetEntityData(opaqueRoot);
            if (!rootData) continue;
            if (rootData->ModelAssetGuid.ToString() == parentModelGuid) {
                // GUID matches - check if old ID also matches
                if (oldParentModelId != (EntityID)-1) {
                    auto it = idMapping.find(oldParentModelId);
                    if (it != idMapping.end() && it->second == opaqueRoot) {
                        // Perfect match: both GUID and old ID match
                        parentModelRoot = opaqueRoot;
                        break;
                    }
                }
                // Store as fallback if no perfect match found yet
                if (guidOnlyMatch == (EntityID)-1) {
                    guidOnlyMatch = opaqueRoot;
                }
            }
        }
        
        // Use GUID-only match as fallback
        if (parentModelRoot == (EntityID)-1) {
            parentModelRoot = guidOnlyMatch;
        }
        
        if (parentModelRoot == (EntityID)-1) {
            std::cout << "[PathParent] Could not find model root with GUID=" << parentModelGuid << std::endl;
            continue;
        }
        
        // Helper to strip trailing numeric suffix (e.g., "SkeletonRoot_97" -> "SkeletonRoot")
        auto stripNumericSuffix = [](const std::string& name) -> std::string {
            size_t us = name.find_last_of('_');
            if (us == std::string::npos || us == 0) return name;
            bool allDigits = true;
            for (size_t i = us + 1; i < name.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                    allDigits = false;
                    break;
                }
            }
            return allDigits ? name.substr(0, us) : name;
        };
        
        // Resolve the path inside the model hierarchy
        auto resolvePathInModel = [&](EntityID modelRoot, const std::string& path) -> EntityID {
            if (path.empty()) return modelRoot;
            
            std::vector<std::string> parts;
            std::string current;
            for (char ch : path) {
                if (ch == '/') {
                    if (!current.empty()) parts.push_back(current);
                    current.clear();
                } else {
                    current += ch;
                }
            }
            if (!current.empty()) parts.push_back(current);
            
            EntityID cur = modelRoot;
            auto* curData = scene.GetEntityData(cur);
            if (!curData) return (EntityID)-1;
            
            // Start searching from model root's children
            for (const std::string& part : parts) {
                bool found = false;
                std::string partBase = stripNumericSuffix(part);
                
                // First try exact match
                for (EntityID child : curData->Children) {
                    auto* childData = scene.GetEntityData(child);
                    if (childData && childData->Name == part) {
                        cur = child;
                        curData = childData;
                        found = true;
                        break;
                    }
                }
                
                // If not found, try matching with stripped suffixes
                if (!found) {
                    for (EntityID child : curData->Children) {
                        auto* childData = scene.GetEntityData(child);
                        if (childData) {
                            std::string childBase = stripNumericSuffix(childData->Name);
                            if (childBase == partBase) {
                                cur = child;
                                curData = childData;
                                found = true;
                                std::cout << "[PathParent] Fuzzy matched '" << part << "' to '" << childData->Name << "'" << std::endl;
                                break;
                            }
                        }
                    }
                }
                
                if (!found) {
                    std::cout << "[PathParent] Could not find path part '" << part << "' (base='" << partBase << "') in model hierarchy" << std::endl;
                    return (EntityID)-1;
                }
            }
            return cur;
        };
        
        EntityID resolvedParent = resolvePathInModel(parentModelRoot, parentPath);
        if (resolvedParent != (EntityID)-1) {
            scene.SetParent(childNew, resolvedParent);
            try {
                auto* childData = scene.GetEntityData(childNew);
                auto* parentData = scene.GetEntityData(resolvedParent);
                std::cout << "[PathParent] Parented '" << (childData ? childData->Name : "?") 
                          << "' under '" << (parentData ? parentData->Name : "?") 
                          << "' via path='" << parentPath << "'" << std::endl;
            } catch(...) {}
        }
    }

    // Fix up NavAgent NavMeshEntity references
    for (const auto& [oldId, newId] : idMapping) {
        auto* ed = scene.GetEntityData(newId);
        if (!ed || !ed->NavAgent) continue;
        if (ed->NavAgent->NavMeshEntity == 0) continue;
        auto it = idMapping.find(ed->NavAgent->NavMeshEntity);
        if (it != idMapping.end()) {
            ed->NavAgent->NavMeshEntity = it->second;
        } else {
            // Old ID not found in mapping - clear to trigger auto-binding
            std::cout << "[NavAgent] Clearing stale NavMeshEntity ref (old ID " << ed->NavAgent->NavMeshEntity << " not found in mapping)\n";
            ed->NavAgent->NavMeshEntity = 0;
        }
    }

    // Deferred pass: resolve script Entity references now that all entities exist
    auto deferred_resolve_script_entity_refs = [&]() {
        try {
            // Helper to check if a type is entity-like
            auto isEntityLikeType = [](PropertyType t) -> bool {
                return t == PropertyType::Entity || t == PropertyType::ComponentRef || t == PropertyType::ScriptRef;
            };

            for (const auto& entityData : data["entities"]) {
                if (!entityData.contains("id")) continue;
                EntityID oldId = entityData["id"].get<EntityID>();
                auto itMap = idMapping.find(oldId); if (itMap == idMapping.end()) continue;
                EntityID nid = itMap->second;
                auto* ed = scene.GetEntityData(nid); if (!ed) continue;
                if (!entityData.contains("scripts") || !entityData["scripts"].is_array()) continue;
                for (const auto& scriptData : entityData["scripts"]) {
                    if (!scriptData.contains("className")) continue;
                    std::string cls = scriptData["className"].get<std::string>();
                    // Find instance
                    ScriptInstance* inst = nullptr;
                    for (auto& s : ed->Scripts) { if (s.ClassName == cls) { inst = &s; break; } }
                    // If no instance exists yet (e.g., managed factory not available at load time), create a placeholder
                    if (!inst) { ed->Scripts.push_back(ScriptInstance{}); ed->Scripts.back().ClassName = cls; inst = &ed->Scripts.back(); }
                    const json* props = nullptr;
                    if (scriptData.contains("properties") && scriptData["properties"].is_object()) props = &scriptData["properties"];
                    if (!props) continue;
                    // Meta-aware when available; fallback to generic entity-like resolution when reflection is missing
                    bool hasMeta = ScriptReflection::HasProperties(cls);
                    std::vector<PropertyInfo>* metaPtr = hasMeta ? &ScriptReflection::GetScriptProperties(cls) : nullptr;
                    for (auto it = props->begin(); it != props->end(); ++it) {
                        const std::string key = it.key();
                        const auto& jv = it.value();
                        
                        bool treatAsEntityLike = false;
                        bool treatAsEntityList = false;
                        PropertyType listElemType = PropertyType::Int;
                        
                        if (metaPtr) {
                            auto& meta = *metaPtr;
                            auto pit = std::find_if(meta.begin(), meta.end(), [&](const PropertyInfo& p){ return p.name == key; });
                            if (pit != meta.end()) {
                                treatAsEntityLike = isEntityLikeType(pit->type);
                                if (pit->type == PropertyType::List) {
                                    listElemType = pit->listElementType;
                                    treatAsEntityList = isEntityLikeType(listElemType);
                                }
                            } else {
                                // Unknown field under meta: check for entity list format
                                if (jv.is_object() && jv.contains("__entityList") && jv["__entityList"].get<bool>()) {
                                    treatAsEntityList = true;
                                }
                                continue;
                            }
                        } else {
                            // No meta: infer by shape of stored value
                            if (jv.is_object() && (jv.contains("guid") || jv.contains("scenePath") || jv.contains("modelGuid"))) {
                                treatAsEntityLike = true;
                            } else if (jv.is_object() && jv.contains("__entityList") && jv["__entityList"].get<bool>()) {
                                treatAsEntityList = true;
                            }
                        }

                        // Handle entity list deferred resolution
                        if (treatAsEntityList) {
                            // Check if we already have a list in Values that needs resolution
                            auto vit = inst->Values.find(key);
                            if (vit != inst->Values.end() && std::holds_alternative<std::shared_ptr<ListPropertyValue>>(vit->second)) {
                                auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(vit->second);
                                if (!listPtr) continue;
                                
                                // Check if elements need resolution (value is -1)
                                bool needsResolution = false;
                                for (const auto& elem : listPtr->elements) {
                                    if (std::holds_alternative<int>(elem) && std::get<int>(elem) == -1) {
                                        needsResolution = true;
                                        break;
                                    }
                                }
                                
                                if (needsResolution && jv.contains("elements") && jv["elements"].is_array()) {
                                    const auto& elementsJson = jv["elements"];
                                    for (size_t i = 0; i < listPtr->elements.size() && i < elementsJson.size(); ++i) {
                                        if (std::holds_alternative<int>(listPtr->elements[i]) && std::get<int>(listPtr->elements[i]) == -1) {
                                            EntityID r = ResolveEntityReferenceJson(elementsJson[i], scene);
                                            if (r != INVALID_ENTITY_ID) {
                                                listPtr->elements[i] = (int)r;
                                            }
                                        }
                                    }
                                }
                            }
                            continue;
                        }

                        if (!treatAsEntityLike) continue;
                        // Skip if already resolved
                        int curVal = -1; auto vit = inst->Values.find(key);
                        if (vit != inst->Values.end() && std::holds_alternative<int>(vit->second)) curVal = std::get<int>(vit->second);
                        if (curVal != -1) continue;
                        EntityID r = ResolveEntityReferenceJson(jv, scene);
                        if (r != INVALID_ENTITY_ID) inst->Values[key] = (int)r;
                    }
                }
            }
        } catch(...) { std::cerr << "[Serializer] Deferred script entity ref resolution failed" << std::endl; }
    };

    auto relink_mesh_proxies = [&]() {
        try {
            for (const auto& entity : scene.GetEntities()) {
                auto* d = scene.GetEntityData(entity.GetID()); if (!d || !d->MeshProxy) continue;
                auto& proxy = *d->MeshProxy;
                if (proxy.TargetMesh == INVALID_ENTITY_ID) {
                    nlohmann::json ref;
                    if (!(proxy.TargetGuidHint.high == 0 && proxy.TargetGuidHint.low == 0)) {
                        ref["guid"] = proxy.TargetGuidHint.ToString();
                    }
                    if (!proxy.TargetScenePathHint.empty()) {
                        ref["scenePath"] = proxy.TargetScenePathHint;
                    }
                    if (!(proxy.TargetModelGuidHint.high == 0 && proxy.TargetModelGuidHint.low == 0) && !proxy.TargetModelPathHint.empty()) {
                        ref["modelGuid"] = proxy.TargetModelGuidHint.ToString();
                        ref["modelNodePath"] = proxy.TargetModelPathHint;
                    }
                    if (!ref.empty()) {
                        EntityID resolved = ResolveEntityReferenceJson(ref, scene);
                        if (resolved != INVALID_ENTITY_ID) {
                            proxy.TargetMesh = resolved;
                            proxy.SerializedTarget = resolved;
                        }
                    }
                }
                if (proxy.TargetMesh == INVALID_ENTITY_ID) continue;
                auto* targetData = scene.GetEntityData(proxy.TargetMesh);
                if (!targetData || !targetData->Mesh || !targetData->Mesh->mesh) continue;
                if (targetData->Mesh->SubmeshOwners.size() < targetData->Mesh->mesh->Submeshes.size()) {
                    targetData->Mesh->SubmeshOwners.resize(targetData->Mesh->mesh->Submeshes.size(), INVALID_ENTITY_ID);
                }
                for (uint32_t slot : proxy.SubmeshSlots) {
                    if (slot < targetData->Mesh->SubmeshOwners.size()) {
                        targetData->Mesh->SubmeshOwners[slot] = entity.GetID();
                    }
                }
                proxy.TargetGuidHint = targetData->EntityGuid;
                proxy.TargetScenePathHint = ComputeScenePath(scene, proxy.TargetMesh);
                EntityID modelRoot = INVALID_ENTITY_ID; ClaymoreGUID mg{}; std::string relPath;
                if (ComputeModelNodePathInfo(scene, proxy.TargetMesh, modelRoot, mg, relPath) && !relPath.empty()) {
                    proxy.TargetModelGuidHint = mg;
                    proxy.TargetModelPathHint = relPath;
                } else {
                    proxy.TargetModelGuidHint = ClaymoreGUID{};
                    proxy.TargetModelPathHint.clear();
                }
                proxy.SlotIndexLookup.clear();
                for (size_t i = 0; i < proxy.SubmeshSlots.size(); ++i) {
                    proxy.SlotIndexLookup[proxy.SubmeshSlots[i]] = i;
                }
            }
        } catch(...) { std::cerr << "[Serializer] Mesh proxy relink failed\n"; }
    };

    // Apply per-node overrides under compact model roots
    for (const auto& entityData : data["entities"]) {
        if (!(entityData.contains("asset") && entityData["asset"].is_object())) continue;
        const auto& a = entityData["asset"]; if (a.value("type", "") != std::string("model")) continue;
        if (!entityData.contains("id")) continue;
        auto itMap = idMapping.find(entityData["id"].get<EntityID>()); if (itMap == idMapping.end()) continue;
        EntityID rootNew = itMap->second;
        if (!entityData.contains("children") || !entityData["children"].is_array()) continue;
        // Collect and sort overrides by path depth so parents are processed before children
        struct OverrideItem { std::string relPath; const nlohmann::json* j; int depth; };
        std::vector<OverrideItem> items;
        for (const auto& childOverride : entityData["children"]) {
            if (!childOverride.contains("_modelNodePath")) continue;
            
            // Skip child overrides that are nested model roots - they are handled separately
            // as their own entities with asset records. This handles legacy scene files that
            // may have incorrectly serialized nested models as child overrides.
            if (childOverride.contains("modelAssetGuid")) {
                try {
                    ClaymoreGUID mag;
                    childOverride.at("modelAssetGuid").get_to(mag);
                    if (mag.high != 0 || mag.low != 0) {
                        std::cout << "[LoadScene] Skipping nested model override '" 
                                  << childOverride.value("name", "?") << "' - handled as separate entity\n";
                        continue;
                    }
                } catch (...) {}
            }
            
            std::string relPath = childOverride["_modelNodePath"].get<std::string>();
            int depth = 0; for (char c : relPath) if (c=='/') ++depth;
            items.push_back({std::move(relPath), &childOverride, depth});
        }
        std::stable_sort(items.begin(), items.end(), [](const OverrideItem& a, const OverrideItem& b){ return a.depth < b.depth; });

        auto resolveByPath = [&](const std::string& path) -> EntityID {
            EntityID target = rootNew;
            if (path.empty()) return target;
            std::stringstream ss(path); std::string part;
            while (std::getline(ss, part, '/')) {
                auto* d = scene.GetEntityData(target); if (!d) return (EntityID)-1;
                auto normalize = [](const std::string& name) -> std::string {
                    // strip trailing _<digits>
                    size_t us = name.find_last_of('_');
                    if (us == std::string::npos) return name;
                    bool digits = true; for (size_t i = us + 1; i < name.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; } }
                    if (!digits) return name;
                    return name.substr(0, us);
                };
                const std::string partNorm = normalize(part);
                auto lower = [](std::string s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };

                // Accept paths that either start at the current root's children or redundantly
                // include the current root's own name after a model re-export/root reshuffle.
                if (lower(d->Name) == lower(part) ||
                    lower(normalize(d->Name)) == lower(partNorm) ||
                    d->Name == part ||
                    normalize(d->Name) == partNorm) {
                    continue;
                }

                EntityID next = (EntityID)-1;
                for (EntityID c : d->Children) {
                    auto* cd = scene.GetEntityData(c); if (!cd) continue;
                    const std::string childName = cd->Name;
                    if (childName == part || normalize(childName) == partNorm || lower(childName) == lower(part) || lower(normalize(childName)) == lower(partNorm)) { next = c; break; }
                }
                if (next == (EntityID)-1) return (EntityID)-1; target = next;
            }
            return target;
        };

        auto findByMeshFileId = [&](int fileID) -> EntityID {
            std::function<EntityID(EntityID)> dfs = [&](EntityID id)->EntityID{
                auto* d = scene.GetEntityData(id); if (!d) return (EntityID)-1;
                // Must have valid mesh data (not just MeshComponent with matching fileID)
                // Otherwise we could match a node whose mesh failed to load
                if (d->Mesh && d->Mesh->mesh && d->Mesh->meshReference.fileID == fileID) return id;
                for (EntityID c : d->Children) { EntityID r = dfs(c); if (r != (EntityID)-1) return r; }
                return (EntityID)-1;
            };
            return dfs(rootNew);
        };

        // Build additional lookup structures for enhanced matching
        std::unordered_map<std::string, EntityID> nodesByNormalizedPath;
        std::unordered_map<std::string, EntityID> nodesByNormalizedName;
        std::unordered_map<uint64_t, std::vector<EntityID>> nodesByContentHash;
        std::unordered_map<ClaymoreGUID, EntityID> nodesByGuid;
        {
            std::function<void(EntityID, const std::string&)> buildLookups = [&](EntityID id, const std::string& parentPath) {
                auto* d = scene.GetEntityData(id); if (!d) return;
                std::string path = parentPath.empty() ? d->Name : (parentPath + "/" + d->Name);
                std::string normPath = ModelNodeIdentity::NormalizePath(path);
                std::string normName = ModelNodeIdentity::NormalizeName(d->Name);
                
                nodesByNormalizedPath[normPath] = id;
                nodesByNormalizedName[normName] = id;
                
                if (d->EntityGuid.high != 0 || d->EntityGuid.low != 0) {
                    nodesByGuid[d->EntityGuid] = id;
                }
                // Compute content hash for this node
                if (d->Mesh) {
                    std::vector<int> meshIndices = { d->Mesh->meshReference.fileID };
                    const float* xfData = &d->Transform.LocalMatrix[0][0];
                    int vertHint = d->Mesh->mesh ? static_cast<int>(d->Mesh->mesh->Vertices.size()) : 0;
                    uint64_t hash = ModelNodeIdentity::ComputeContentHash(meshIndices, xfData, vertHint);
                    nodesByContentHash[hash].push_back(id);
                }
                
                for (EntityID c : d->Children) {
                    auto* cd = scene.GetEntityData(c);
                    if (cd) buildLookups(c, path);
                }
            };
            auto* rootData = scene.GetEntityData(rootNew);
            if (rootData) buildLookups(rootNew, "");
        }
        
        for (const auto& it : items) {
            const auto& childOverride = *it.j;
            const std::string& relPath = it.relPath;

            // Check if this entity was explicitly marked as user-added during save.
            // For explicit user-added nodes, avoid matching against model hierarchy by path.
            bool explicitlyUserAdded = childOverride.contains("_added") && childOverride["_added"].get<bool>();
            if (explicitlyUserAdded && childOverride.contains("skeleton")) {
                // Skeleton nodes should still match model hierarchy even if marked _added
                // (prevents duplicate SkeletonRoot nodes from legacy prefab saves)
                explicitlyUserAdded = false;
            }

            EntityID target = explicitlyUserAdded ? INVALID_ENTITY_ID : resolveByPath(relPath);
            bool matchedByPath = (target != INVALID_ENTITY_ID);  // Track if we matched by exact path
            if (explicitlyUserAdded && target == INVALID_ENTITY_ID && childOverride.contains("guid")) {
                try {
                    ClaymoreGUID savedGuid;
                    childOverride.at("guid").get_to(savedGuid);
                    auto itGuid = nodesByGuid.find(savedGuid);
                    if (itGuid != nodesByGuid.end()) {
                        target = itGuid->second;
                        matchedByPath = true;
                    }
                } catch (...) {}
            }

            bool allowModelFallback = !explicitlyUserAdded;
            
            // Fallback 1: try normalized path from saved override (safe for user-added - path is unique)
            if (target == INVALID_ENTITY_ID && childOverride.contains("_normalizedPath") && allowModelFallback) {
                std::string normPath = childOverride["_normalizedPath"].get<std::string>();
                auto itNorm = nodesByNormalizedPath.find(normPath);
                if (itNorm != nodesByNormalizedPath.end()) {
                    target = itNorm->second;
                }
            }
            
            // Fallback 2: try to resolve by mesh fileID when path-based lookup fails (handles renamed nodes with meshes)
            // SKIP for user-added entities to avoid matching user's cube/primitive to model mesh
            // ALSO validate that the found entity's name is similar to avoid incorrect matches after model reimport
            // (fileIDs can change when model is re-exported, causing wrong entity matches)
            if (target == INVALID_ENTITY_ID && allowModelFallback && childOverride.contains("mesh") && childOverride["mesh"].contains("fileID")) {
                int fid = childOverride["mesh"]["fileID"].get<int>();
                EntityID candidateByFileId = findByMeshFileId(fid);
                if (candidateByFileId != INVALID_ENTITY_ID && childOverride.contains("_normalizedName")) {
                    // Only use fileID match if the entity's normalized name matches
                    auto* candidateData = scene.GetEntityData(candidateByFileId);
                    if (candidateData) {
                        std::string expectedNormName = childOverride["_normalizedName"].get<std::string>();
                        std::string actualNormName = ModelNodeIdentity::NormalizeName(candidateData->Name);
                        if (actualNormName == expectedNormName) {
                            target = candidateByFileId;
                            try { std::cout << "[ResolveByFileID] fileID=" << fid << " -> target=" << (int)target << " (name validated)" << std::endl; } catch(...) {}
                        } else {
                            try { std::cout << "[ResolveByFileID] fileID=" << fid << " REJECTED: expected name '" << expectedNormName << "' but found '" << actualNormName << "'" << std::endl; } catch(...) {}
                        }
                    }
                } else if (candidateByFileId != INVALID_ENTITY_ID) {
                    // No normalized name to validate against, use fileID match with warning
                    target = candidateByFileId;
                    try { std::cout << "[ResolveByFileID] fileID=" << fid << " -> target=" << (int)target << " (no name validation)" << std::endl; } catch(...) {}
                }
            }
            
            // Fallback 3: try content hash matching (handles renamed nodes)
            // SKIP for user-added entities to avoid matching based on mesh content
            if (target == INVALID_ENTITY_ID && allowModelFallback && childOverride.contains("_contentHash")) {
                uint64_t hash = childOverride["_contentHash"].get<uint64_t>();
                auto itHash = nodesByContentHash.find(hash);
                if (itHash != nodesByContentHash.end() && !itHash->second.empty()) {
                    // If multiple matches, prefer one with matching normalized name
                    if (childOverride.contains("_normalizedName")) {
                        std::string normName = childOverride["_normalizedName"].get<std::string>();
                        for (EntityID candidate : itHash->second) {
                            auto* cd = scene.GetEntityData(candidate);
                            if (cd && ModelNodeIdentity::NormalizeName(cd->Name) == normName) {
                                target = candidate;
                                break;
                            }
                        }
                    }
                    if (target == INVALID_ENTITY_ID) {
                        target = itHash->second.front(); // Use first match
                    }
                    try { std::cout << "[ResolveByHash] hash=" << hash << " -> target=" << (int)target << std::endl; } catch(...) {}
                }
            }
            
            // Fallback 4: fuzzy name match
            // SKIP for user-added entities to avoid matching "Cube" or generic names to model nodes
            if (target == INVALID_ENTITY_ID && allowModelFallback && childOverride.contains("_normalizedName")) {
                std::string normName = childOverride["_normalizedName"].get<std::string>();
                auto itName = nodesByNormalizedName.find(normName);
                if (itName != nodesByNormalizedName.end()) {
                    target = itName->second;
                    try { std::cout << "[ResolveByFuzzyName] name='" << normName << "' -> target=" << (int)target << std::endl; } catch(...) {}
                }
            }
            
            // Handle reparenting: if we matched by fallback (not by path), the node might need to be
            // moved to the intended parent location. The _modelNodePath reflects where the user
            // placed the node; if we matched by fileID/hash/name, the node is at its original
            // model location and needs reparenting.
            if (target != INVALID_ENTITY_ID && !matchedByPath && allowModelFallback) {
                // Parse intended parent from relPath
                std::string intendedParentPath;
                auto slashPos = relPath.find_last_of('/');
                if (slashPos != std::string::npos) {
                    intendedParentPath = relPath.substr(0, slashPos);
                }
                // intendedParentPath is empty if node should be direct child of model root
                
                EntityID intendedParent = intendedParentPath.empty() ? rootNew : resolveByPath(intendedParentPath);
                if (intendedParent != INVALID_ENTITY_ID) {
                    auto* td = scene.GetEntityData(target);
                    if (td && td->Parent != intendedParent) {
                        // Node is not under its intended parent - reparent it
                        scene.SetParent(target, intendedParent);
                        try { std::cout << "[Reparent] Moved '" << td->Name << "' to intended parent path='" << intendedParentPath << "'" << std::endl; } catch(...) {}
                    }
                }
            }

            if (target == INVALID_ENTITY_ID) {
                // Heuristic: avoid creating duplicates for original model nodes.
                // Only create when the override clearly represents a user-added node
                // (e.g., has non-model components). If it looks like a model node (has mesh) or
                // contains only transform/name, skip creation.
                auto has = [&](const char* k){ return childOverride.contains(k); };
                bool looksUserAdded = has("camera") || has("light") || has("collider") || has("rigidbody") || has("staticbody")
                    || has("emitter") || has("canvas") || has("panel") || has("button") || has("text") || has("scripts") || has("terrain")
                    || has("navmesh") || has("navagent") || has("navlink") || has("portal") || has("area") || has("grassDeformer") || has("characterController")
                    || (has("animator") && !childOverride["animator"].is_null());
                
                // Also treat as user-added if the entity has a GUID that doesn't exist in the fresh model
                // This handles empty organizing entities (no components) that users add under skeleton bones
                if (!looksUserAdded && has("guid")) {
                    try {
                        ClaymoreGUID savedGuid;
                        childOverride.at("guid").get_to(savedGuid);
                        // Check if this GUID exists anywhere in the fresh model hierarchy
                        bool foundInFreshModel = false;
                        std::function<bool(EntityID)> checkGuid = [&](EntityID id) -> bool {
                            auto* d = scene.GetEntityData(id);
                            if (!d) return false;
                            if (d->EntityGuid == savedGuid) return true;
                            for (EntityID c : d->Children) {
                                if (checkGuid(c)) return true;
                            }
                            return false;
                        };
                        foundInFreshModel = checkGuid(rootNew);
                        if (!foundInFreshModel) {
                            looksUserAdded = true;
                            std::cout << "[SceneLoad] Entity with GUID " << savedGuid.ToString() << " not found in fresh model - treating as user-added\n";
                        }
                    } catch (...) {}
                }
                
                // Also treat as user-added if explicitly marked with _added flag (for prefab delta compatibility)
                if (!looksUserAdded && has("_added") && childOverride["_added"].get<bool>()) {
                    looksUserAdded = true;
                }
                
                // Model nodes have mesh, skeleton, skinning, or blendShapes - these should never be duplicated
                // even if GUID check marked them as "user-added" due to model re-export
                bool looksModelNode = has("mesh") || has("skeleton") || has("skinning") || has("blendShapes");
                
                // NEVER duplicate model nodes - they should exist in the fresh model instantiation
                // This prevents duplicate SkeletonRoot/mesh entities when model is re-exported with different GUIDs
                if (looksModelNode) {
                    std::cout << "[SkipModelNode] path='" << relPath << "' - model node should exist in fresh model\n";
                    continue;
                }
                
                // Prefer updating an existing child with the same (normalized/case-insensitive) name under the intended parent
                // Skip this for explicitly user-added nodes to avoid binding them to model nodes with the same name
                if (!explicitlyUserAdded) {
                    std::string parentPath;
                    std::string leafName;
                    auto ppos = relPath.find_last_of('/');
                    if (ppos != std::string::npos) { parentPath = relPath.substr(0, ppos); leafName = relPath.substr(ppos+1); }
                    else { leafName = relPath; }
                    EntityID parentTarget = resolveByPath(parentPath);
                    if (parentTarget != INVALID_ENTITY_ID) {
                        auto* pd = scene.GetEntityData(parentTarget);
                        if (pd) {
                            auto normalize = [](const std::string& name)->std::string{
                                size_t us = name.find_last_of('_');
                                if (us == std::string::npos) return name;
                                bool digits = true; for (size_t i = us + 1; i < name.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; } }
                                return digits ? name.substr(0, us) : name;
                            };
                            auto lower = [](std::string s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
                            const std::string leafNorm = normalize(leafName);
                            for (EntityID c : pd->Children) {
                                if (auto* cd = scene.GetEntityData(c)) {
                                    if (normalize(cd->Name) == leafNorm || lower(normalize(cd->Name)) == lower(leafNorm)) { target = c; break; }
                                }
                            }
                        }
                    }
                }
                if (explicitlyUserAdded && target != INVALID_ENTITY_ID) {
                    target = INVALID_ENTITY_ID;
                }
                if (!looksUserAdded) {
                    continue; // skip creating; assume it's an original model child we failed to resolve
                }
                // Treat as an added node: attach under parent path
                EntityID parentTarget = rootNew;
                std::string parentPath;
                std::string leafName;
                auto pos = relPath.find_last_of('/');
                if (pos != std::string::npos) { parentPath = relPath.substr(0, pos); leafName = relPath.substr(pos+1); }
                else { parentPath.clear(); leafName = relPath; }
                parentTarget = resolveByPath(parentPath);
                if (parentTarget == (EntityID)-1) continue;
                if (target != (EntityID)-1) {
                    // Apply override to the matched existing child
                    auto* td = scene.GetEntityData(target); if (!td) continue;
                    if (childOverride.contains("transform")) { DeserializeTransform(childOverride["transform"], td->Transform); scene.MarkTransformDirty(target); }
                    if (childOverride.contains("mesh")) {
                        // Merge into existing MeshComponent preserving mesh pointer from fresh instantiation
                        if (td->Mesh && td->Mesh->mesh) {
                            nlohmann::json meshOverride = childOverride["mesh"];
                            meshOverride.erase("meshReference");
                            meshOverride.erase("fileID");
                            DeserializeMesh(meshOverride, *td->Mesh, td->RenderOverrides);
                        } else if (td->Mesh) {
                            DeserializeMesh(childOverride["mesh"], *td->Mesh, td->RenderOverrides);
                        } else {
                            MeshComponent tmp; DeserializeMesh(childOverride["mesh"], tmp, td->RenderOverrides);
                            td->Mesh = std::make_unique<MeshComponent>(std::move(tmp));
                        }
                    }
                    if (childOverride.contains("camera")) { if (!td->Camera) td->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(childOverride["camera"], *td->Camera); }
                    if (childOverride.contains("light")) { if (!td->Light) td->Light = std::make_unique<LightComponent>(); DeserializeLight(childOverride["light"], *td->Light); }
                    if (childOverride.contains("audioSource")) { if (!td->AudioSource) td->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(childOverride["audioSource"], *td->AudioSource); }
                    if (childOverride.contains("audioListener")) { if (!td->AudioListener) td->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(childOverride["audioListener"], *td->AudioListener); }
                    if (childOverride.contains("collider")) { if (!td->Collider) td->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(childOverride["collider"], *td->Collider); }
                    if (childOverride.contains("rigidbody")) { if (!td->RigidBody) td->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(childOverride["rigidbody"], *td->RigidBody); }
                    if (childOverride.contains("staticbody")) { if (!td->StaticBody) td->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(childOverride["staticbody"], *td->StaticBody); }
                    if (childOverride.contains("characterController")) { if (!td->CharacterController) td->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(childOverride["characterController"], *td->CharacterController); }
                    if (childOverride.contains("emitter")) { if (!td->Emitter) td->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(childOverride["emitter"], *td->Emitter); }
                    if (childOverride.contains("canvas")) { if (!td->Canvas) td->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(childOverride["canvas"], *td->Canvas); }
                    if (childOverride.contains("panel")) { if (!td->Panel) td->Panel = std::make_unique<PanelComponent>(); DeserializePanel(childOverride["panel"], *td->Panel); }
                    if (childOverride.contains("button")) { if (!td->Button) td->Button = std::make_unique<ButtonComponent>(); DeserializeButton(childOverride["button"], *td->Button); }
                    if (childOverride.contains("slider")) { if (!td->Slider) td->Slider = std::make_unique<SliderComponent>(); DeserializeSlider(childOverride["slider"], *td->Slider); }
                    if (childOverride.contains("progressBar")) { if (!td->ProgressBar) td->ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(childOverride["progressBar"], *td->ProgressBar); }
                    if (childOverride.contains("toggle")) { if (!td->Toggle) td->Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(childOverride["toggle"], *td->Toggle); }
                    if (childOverride.contains("scrollView")) { if (!td->ScrollView) td->ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(childOverride["scrollView"], *td->ScrollView); }
                    if (childOverride.contains("layoutGroup")) { if (!td->LayoutGroup) td->LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(childOverride["layoutGroup"], *td->LayoutGroup); }
                    if (childOverride.contains("inputField")) { if (!td->InputField) td->InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(childOverride["inputField"], *td->InputField); }
                    if (childOverride.contains("dropdown")) { if (!td->Dropdown) td->Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(childOverride["dropdown"], *td->Dropdown); }
                    if (childOverride.contains("uiSceneCapture")) { if (!td->UISceneCapture) td->UISceneCapture = std::make_unique<UISceneCaptureComponent>(); DeserializeUISceneCapture(childOverride["uiSceneCapture"], *td->UISceneCapture); }
                    if (childOverride.contains("text")) { if (!td->Text) td->Text = std::make_unique<TextRendererComponent>(); DeserializeText(childOverride["text"], *td->Text); }
                    if (childOverride.contains("terrain")) { if (!td->Terrain) td->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(childOverride["terrain"], *td->Terrain); }
                    if (childOverride.contains("area")) { if (!td->Area) td->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(childOverride["area"], *td->Area); }
                    if (childOverride.contains("navmesh")) { if (!td->Navigation) td->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(childOverride["navmesh"], *td->Navigation); }
                    if (childOverride.contains("navagent")) { if (!td->NavAgent) td->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(childOverride["navagent"], *td->NavAgent); }
                    if (childOverride.contains("navlink")) { if (!td->NavLink) td->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(childOverride["navlink"], *td->NavLink); }
                    if (childOverride.contains("portal")) { if (!td->Portal) td->Portal = std::make_unique<PortalComponent>(); DeserializePortal(childOverride["portal"], *td->Portal); }
                    if (childOverride.contains("grassDeformer")) { if (!td->GrassDeformer) td->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(childOverride["grassDeformer"], *td->GrassDeformer); }
                    if (childOverride.contains("renderOverrides")) { if (!td->RenderOverrides) td->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(childOverride["renderOverrides"], *td->RenderOverrides); }
                    if (childOverride.contains("boneAttachment")) { if (!td->BoneAttachment) td->BoneAttachment = std::make_unique<BoneAttachmentComponent>(); DeserializeBoneAttachment(childOverride["boneAttachment"], *td->BoneAttachment); }
                    if (childOverride.contains("scripts")) { DeserializeScripts(childOverride["scripts"], td->Scripts, scene); }
                    if (childOverride.contains("animator")) { if (!td->AnimationPlayer) td->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(childOverride["animator"], *td->AnimationPlayer); }
                    // IK constraint blocks
                    if (childOverride.contains("ik") && childOverride["ik"].is_array()) {
                        td->IKs.clear();
                        for (const auto& j : childOverride["ik"]) {
                            if (!j.is_object()) continue;
                            cm::animation::ik::IKComponent c;
                            c.Enabled = j.value("enabled", true);
                            c.TargetEntity = j.value("target", (EntityID)0);
                            c.PoleEntity = j.value("pole", (EntityID)0);
                            // Load GUIDs for post-load resolution
                            c.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                            c.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                            c.PoleEntityGuidHigh = j.value("poleGuidHigh", (uint64_t)0);
                            c.PoleEntityGuidLow = j.value("poleGuidLow", (uint64_t)0);
                            c.Weight = j.value("weight", 1.0f);
                            c.MaxIterations = j.value("maxIterations", 12.0f);
                            c.Tolerance = j.value("tolerance", 0.001f);
                            c.Damping = j.value("damping", 0.2f);
                            c.UseTwoBone = j.value("useTwoBone", true);
                            c.LockAxisX = j.value("lockAxisX", false);
                            c.LockAxisY = j.value("lockAxisY", false);
                            c.LockAxisZ = j.value("lockAxisZ", false);
                            c.Visualize = j.value("visualize", false);
                            std::vector<cm::animation::ik::BoneId> chainBones;
                            if (j.contains("chain") && j["chain"].is_array()) {
                                for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                            }
                            c.SetChain(chainBones);
                            c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                            c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                            if (j.contains("constraints") && j["constraints"].is_array()) {
                                for (const auto& cj : j["constraints"]) {
                                    if (!cj.is_object()) continue;
                                    cm::animation::ik::IKComponent::Constraint cc;
                                    cc.useHinge = cj.value("useHinge", false);
                                    cc.useTwist = cj.value("useTwist", false);
                                    cc.hingeMinDeg = cj.value("hingeMinDeg", 0.0f);
                                    cc.hingeMaxDeg = cj.value("hingeMaxDeg", 0.0f);
                                    cc.twistMinDeg = cj.value("twistMinDeg", 0.0f);
                                    cc.twistMaxDeg = cj.value("twistMaxDeg", 0.0f);
                                    c.Constraints.push_back(cc);
                                }
                            }
                            td->IKs.push_back(std::move(c));
                        }
                    }
                    // LookAt constraint blocks
                    if (childOverride.contains("lookat") && childOverride["lookat"].is_array()) {
                        td->LookAtConstraints.clear();
                        for (const auto& j : childOverride["lookat"]) {
                            if (!j.is_object()) continue;
                            cm::animation::lookat::LookAtConstraintComponent lac;
                            lac.Enabled = j.value("enabled", true);
                            lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                            lac.TargetEntity = j.value("target", (EntityID)0);
                            // Load target entity GUID for post-load resolution
                            lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                            lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                            lac.Weight = j.value("weight", 1.0f);
                            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                            lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                            lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                            lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                            lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                            lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                            lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                            lac.Visualize = j.value("visualize", false);
                            if (j.contains("boneChain") && j["boneChain"].is_array()) {
                                for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                            }
                            if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                                for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                            }
                            td->LookAtConstraints.push_back(std::move(lac));
                        }
                    }
                    if (childOverride.contains("name")) {
                        std::string name = childOverride["name"].get<std::string>();
                        // Strip numeric suffix from model-internal children's names during deserialization
                        // Only strip if NOT user-added (user-added children keep their suffixes)
                        if (!childOverride.contains("_added") || !childOverride["_added"].get<bool>()) {
                            size_t us = name.find_last_of('_');
                            if (us != std::string::npos && us > 0 && us < name.size() - 1) {
                                bool allDigits = true;
                                for (size_t i = us + 1; i < name.size(); ++i) {
                                    if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                        allDigits = false;
                                        break;
                                    }
                                }
                                if (allDigits) {
                                    name = name.substr(0, us);
                                }
                            }
                        }
                        td->Name = name;
                    }
                    if (childOverride.contains("visible")) { td->Visible = childOverride["visible"].get<bool>(); }
                } else {
                    // Create entity from override json and parent it under parentTarget
                    nlohmann::json jcopy = childOverride;
                    if (!jcopy.contains("name")) jcopy["name"] = leafName;
                    // Strip numeric suffix from model-internal children's names before creating entity
                    // Only strip if NOT user-added (user-added children keep their suffixes)
                    if (jcopy.contains("name") && jcopy["name"].is_string()) {
                        if (!jcopy.contains("_added") || !jcopy["_added"].get<bool>()) {
                            std::string name = jcopy["name"].get<std::string>();
                            size_t us = name.find_last_of('_');
                            if (us != std::string::npos && us > 0 && us < name.size() - 1) {
                                bool allDigits = true;
                                for (size_t i = us + 1; i < name.size(); ++i) {
                                    if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                        allDigits = false;
                                        break;
                                    }
                                }
                                if (allDigits) {
                                    jcopy["name"] = name.substr(0, us);
                                }
                            }
                        }
                    }
                    EntityID newChild = DeserializeEntity(jcopy, scene);
                    if (newChild != 0 && newChild != (EntityID)-1) {
                        scene.SetParent(newChild, parentTarget);
                    }
                }
                continue;
            }

            // Apply overrides to existing node
            auto* td = scene.GetEntityData(target); if (!td) continue;
            if (childOverride.contains("transform")) { DeserializeTransform(childOverride["transform"], td->Transform); scene.MarkTransformDirty(target); }
            if (childOverride.contains("mesh")) {
                // Merge into existing MeshComponent when present so fields omitted in overrides
                // (e.g., renderOnTop, alpha, property blocks) are preserved instead of reset.
                // CRITICAL: For model children, preserve the meshReference from the fresh instantiation.
                // The scene file's fileID may be stale after Blender reimport (mesh indices can change
                // when materials are added/removed). The entity was matched by name path, so the
                // freshly assigned meshReference is correct - only override materials/properties.
                // HOWEVER: If the existing mesh pointer is null (mesh failed to load during instantiation),
                // we MUST use the override's meshReference to try loading it.
                if (td->Mesh && td->Mesh->mesh) {
                    nlohmann::json meshOverride = childOverride["mesh"];
                    // Strip meshReference to prevent loading wrong mesh after model reimport
                    meshOverride.erase("meshReference");
                    meshOverride.erase("fileID"); // legacy field name
                    DeserializeMesh(meshOverride, *td->Mesh, td->RenderOverrides);
                } else if (td->Mesh) {
                    // MeshComponent exists but mesh is null - try loading from override's meshReference
                    DeserializeMesh(childOverride["mesh"], *td->Mesh, td->RenderOverrides);
                } else {
                    MeshComponent tmp; DeserializeMesh(childOverride["mesh"], tmp, td->RenderOverrides);
                    td->Mesh = std::make_unique<MeshComponent>(std::move(tmp));
                }
            }
            if (childOverride.contains("renderOverrides") && childOverride["renderOverrides"].is_object()) {
                if (!td->RenderOverrides) td->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                DeserializeRenderOverrides(childOverride["renderOverrides"], *td->RenderOverrides);
            }
            if (childOverride.contains("light")) {
                if (!td->Light) td->Light = std::make_unique<LightComponent>();
                DeserializeLight(childOverride["light"], *td->Light);
            }
            if (childOverride.contains("audioSource")) {
                if (!td->AudioSource) td->AudioSource = std::make_unique<AudioSourceComponent>();
                DeserializeAudioSource(childOverride["audioSource"], *td->AudioSource);
            }
            if (childOverride.contains("audioListener")) {
                if (!td->AudioListener) td->AudioListener = std::make_unique<AudioListenerComponent>();
                DeserializeAudioListener(childOverride["audioListener"], *td->AudioListener);
            }
            if (childOverride.contains("collider")) {
                if (!td->Collider) td->Collider = std::make_unique<ColliderComponent>();
                DeserializeCollider(childOverride["collider"], *td->Collider);
            }
            if (childOverride.contains("area")) {
                if (!td->Area) td->Area = std::make_unique<cm::physics::AreaComponent>();
                Serializer::DeserializeArea(childOverride["area"], *td->Area);
            }
            if (childOverride.contains("rigidbody")) {
                if (!td->RigidBody) td->RigidBody = std::make_unique<RigidBodyComponent>();
                DeserializeRigidBody(childOverride["rigidbody"], *td->RigidBody);
            }
            if (childOverride.contains("staticbody")) {
                if (!td->StaticBody) td->StaticBody = std::make_unique<StaticBodyComponent>();
                DeserializeStaticBody(childOverride["staticbody"], *td->StaticBody);
            }
            if (childOverride.contains("characterController")) {
                if (!td->CharacterController) td->CharacterController = std::make_unique<CharacterControllerComponent>();
                DeserializeCharacterController(childOverride["characterController"], *td->CharacterController);
            }
            if (childOverride.contains("camera")) {
                if (!td->Camera) td->Camera = std::make_unique<CameraComponent>();
                DeserializeCamera(childOverride["camera"], *td->Camera);
            }
            if (childOverride.contains("terrain")) {
                if (!td->Terrain) td->Terrain = std::make_unique<TerrainComponent>();
                DeserializeTerrain(childOverride["terrain"], *td->Terrain);
            }
            if (childOverride.contains("emitter")) {
                if (!td->Emitter) td->Emitter = std::make_unique<ParticleEmitterComponent>();
                DeserializeParticleEmitter(childOverride["emitter"], *td->Emitter);
            }
            if (childOverride.contains("canvas")) {
                if (!td->Canvas) td->Canvas = std::make_unique<CanvasComponent>();
                DeserializeCanvas(childOverride["canvas"], *td->Canvas);
            }
            if (childOverride.contains("panel")) {
                if (!td->Panel) td->Panel = std::make_unique<PanelComponent>();
                DeserializePanel(childOverride["panel"], *td->Panel);
            }
            if (childOverride.contains("button")) {
                if (!td->Button) td->Button = std::make_unique<ButtonComponent>();
                DeserializeButton(childOverride["button"], *td->Button);
            }
            if (childOverride.contains("slider")) {
                if (!td->Slider) td->Slider = std::make_unique<SliderComponent>();
                DeserializeSlider(childOverride["slider"], *td->Slider);
            }
            if (childOverride.contains("progressBar")) {
                if (!td->ProgressBar) td->ProgressBar = std::make_unique<ProgressBarComponent>();
                DeserializeProgressBar(childOverride["progressBar"], *td->ProgressBar);
            }
            if (childOverride.contains("toggle")) {
                if (!td->Toggle) td->Toggle = std::make_unique<ToggleComponent>();
                DeserializeToggle(childOverride["toggle"], *td->Toggle);
            }
            if (childOverride.contains("scrollView")) {
                if (!td->ScrollView) td->ScrollView = std::make_unique<ScrollViewComponent>();
                DeserializeScrollView(childOverride["scrollView"], *td->ScrollView);
            }
            if (childOverride.contains("layoutGroup")) {
                if (!td->LayoutGroup) td->LayoutGroup = std::make_unique<LayoutGroupComponent>();
                DeserializeLayoutGroup(childOverride["layoutGroup"], *td->LayoutGroup);
            }
            if (childOverride.contains("inputField")) {
                if (!td->InputField) td->InputField = std::make_unique<InputFieldComponent>();
                DeserializeInputField(childOverride["inputField"], *td->InputField);
            }
            if (childOverride.contains("dropdown")) {
                if (!td->Dropdown) td->Dropdown = std::make_unique<DropdownComponent>();
                DeserializeDropdown(childOverride["dropdown"], *td->Dropdown);
            }
            if (childOverride.contains("text")) {
                if (!td->Text) td->Text = std::make_unique<TextRendererComponent>();
                DeserializeText(childOverride["text"], *td->Text);
            }
            if (childOverride.contains("navmesh")) {
                if (!td->Navigation) td->Navigation = std::make_unique<nav::NavMeshComponent>();
                DeserializeNavMesh(childOverride["navmesh"], *td->Navigation);
            }
            if (childOverride.contains("navagent")) {
                if (!td->NavAgent) td->NavAgent = std::make_unique<nav::NavAgentComponent>();
                DeserializeNavAgent(childOverride["navagent"], *td->NavAgent);
            }
            if (childOverride.contains("navlink")) {
                if (!td->NavLink) td->NavLink = std::make_unique<nav::NavLinkComponent>();
                DeserializeNavLink(childOverride["navlink"], *td->NavLink);
            }
            if (childOverride.contains("portal")) {
                if (!td->Portal) td->Portal = std::make_unique<PortalComponent>();
                DeserializePortal(childOverride["portal"], *td->Portal);
            }
            if (childOverride.contains("grassDeformer")) {
                if (!td->GrassDeformer) td->GrassDeformer = std::make_unique<GrassDeformerComponent>();
                DeserializeGrassDeformer(childOverride["grassDeformer"], *td->GrassDeformer);
            }
            if (childOverride.contains("renderOverrides")) {
                if (!td->RenderOverrides) td->RenderOverrides = std::make_unique<RenderOverridesComponent>();
                DeserializeRenderOverrides(childOverride["renderOverrides"], *td->RenderOverrides);
            }
            if (childOverride.contains("boneAttachment")) {
                if (!td->BoneAttachment) td->BoneAttachment = std::make_unique<BoneAttachmentComponent>();
                DeserializeBoneAttachment(childOverride["boneAttachment"], *td->BoneAttachment);
            }
            if (childOverride.contains("scripts")) { DeserializeScripts(childOverride["scripts"], td->Scripts, scene); }
            if (childOverride.contains("animator")) {
                if (!td->AnimationPlayer) td->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
                DeserializeAnimator(childOverride["animator"], *td->AnimationPlayer);
            }
            // IK constraint blocks on child entities (e.g., skeleton root)
            if (childOverride.contains("ik") && childOverride["ik"].is_array()) {
                td->IKs.clear();
                for (const auto& j : childOverride["ik"]) {
                    if (!j.is_object()) continue;
                    cm::animation::ik::IKComponent c;
                    c.Enabled = j.value("enabled", true);
                    c.TargetEntity = j.value("target", (EntityID)0);
                    c.PoleEntity = j.value("pole", (EntityID)0);
                    // Load GUIDs for post-load resolution
                    c.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                    c.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                    c.PoleEntityGuidHigh = j.value("poleGuidHigh", (uint64_t)0);
                    c.PoleEntityGuidLow = j.value("poleGuidLow", (uint64_t)0);
                    c.Weight = j.value("weight", 1.0f);
                    c.MaxIterations = j.value("maxIterations", 12.0f);
                    c.Tolerance = j.value("tolerance", 0.001f);
                    c.Damping = j.value("damping", 0.2f);
                    c.UseTwoBone = j.value("useTwoBone", true);
                    c.LockAxisX = j.value("lockAxisX", false);
                    c.LockAxisY = j.value("lockAxisY", false);
                    c.LockAxisZ = j.value("lockAxisZ", false);
                    c.Visualize = j.value("visualize", false);
                    std::vector<cm::animation::ik::BoneId> chainBones;
                    if (j.contains("chain") && j["chain"].is_array()) {
                        for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
                    }
                    c.SetChain(chainBones);
                    c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
                    c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
                    if (j.contains("constraints") && j["constraints"].is_array()) {
                        for (const auto& cj : j["constraints"]) {
                            if (!cj.is_object()) continue;
                            cm::animation::ik::IKComponent::Constraint cc;
                            cc.useHinge = cj.value("useHinge", false);
                            cc.useTwist = cj.value("useTwist", false);
                            cc.hingeMinDeg = cj.value("hingeMinDeg", 0.0f);
                            cc.hingeMaxDeg = cj.value("hingeMaxDeg", 0.0f);
                            cc.twistMinDeg = cj.value("twistMinDeg", 0.0f);
                            cc.twistMaxDeg = cj.value("twistMaxDeg", 0.0f);
                            c.Constraints.push_back(cc);
                        }
                    }
                    td->IKs.push_back(std::move(c));
                }
            }
            // LookAt constraint blocks on child entities (e.g., skeleton root)
            if (childOverride.contains("lookat") && childOverride["lookat"].is_array()) {
                td->LookAtConstraints.clear();
                for (const auto& j : childOverride["lookat"]) {
                    if (!j.is_object()) continue;
                    cm::animation::lookat::LookAtConstraintComponent lac;
                    lac.Enabled = j.value("enabled", true);
                    lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
                    lac.TargetEntity = j.value("target", (EntityID)0);
                    // Load GUIDs for post-load resolution
                    lac.TargetEntityGuidHigh = j.value("targetGuidHigh", (uint64_t)0);
                    lac.TargetEntityGuidLow = j.value("targetGuidLow", (uint64_t)0);
                    lac.Weight = j.value("weight", 1.0f);
                    lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
                    lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
                    lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
                    lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
                    lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
                    lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
                    lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
                    lac.Visualize = j.value("visualize", false);
                    if (j.contains("boneChain") && j["boneChain"].is_array()) {
                        for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
                    }
                    if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                        for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
                    }
                    td->LookAtConstraints.push_back(std::move(lac));
                }
            }
            // TintController on child entities (e.g., skeleton root)
            if (childOverride.contains("tintController") && childOverride["tintController"].is_object()) {
                if (!td->TintController) td->TintController = std::make_unique<TintMaskController>();
                DeserializeTintController(childOverride["tintController"], *td->TintController);
                
                // Resolve TintTarget entityPath to new EntityIDs
                const auto& tintJson = childOverride["tintController"];
                if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
                    size_t idx = 0;
                    for (const auto& targetJ : tintJson["targets"]) {
                        if (idx < td->TintController->Targets.size() && targetJ.contains("entityPath")) {
                            std::string entityPath = targetJ["entityPath"].get<std::string>();
                            // Resolve path relative to the model root (rootNew)
                            EntityID resolvedTarget = (EntityID)-1;
                            if (entityPath.empty()) {
                                resolvedTarget = rootNew;
                            } else {
                                // Build path from model root to find target
                                auto itPath = nodesByNormalizedPath.find(ModelNodeIdentity::NormalizePath(entityPath));
                                if (itPath != nodesByNormalizedPath.end()) {
                                    resolvedTarget = itPath->second;
                                } else {
                                    // Try exact path resolution
                                    resolvedTarget = resolveByPath(entityPath);
                                }
                            }
                            td->TintController->Targets[idx].TargetEntity = resolvedTarget;
                        }
                        idx++;
                    }
                }
                td->TintController->NeedsRefresh = true;
            }
            // UnifiedMorph on child entities (e.g., skeleton root)
            if (childOverride.contains("unifiedMorphWeights") && childOverride["unifiedMorphWeights"].is_array()) {
                if (td->UnifiedMorph) {
                    for (const auto& entry : childOverride["unifiedMorphWeights"]) {
                        if (!entry.is_object() || !entry.contains("name") || !entry.contains("weight")) continue;
                        std::string name = entry["name"].get<std::string>();
                        float weight = entry["weight"].get<float>();
                        for (size_t mi = 0; mi < td->UnifiedMorph->Names.size(); ++mi) {
                            if (td->UnifiedMorph->Names[mi] == name) {
                                td->UnifiedMorph->Weights[mi] = weight;
                                break;
                            }
                        }
                    }
                }
            }
            if (childOverride.contains("name")) {
                td->Name = childOverride["name"].get<std::string>();
            }
            if (childOverride.contains("visible")) {
                td->Visible = childOverride["visible"].get<bool>();
            }
        }
    }

    // Apply per-node overrides under prefab instance roots (same pattern as model roots)
    for (const auto& entityData : data["entities"]) {
        if (!(entityData.contains("asset") && entityData["asset"].is_object())) continue;
        const auto& a = entityData["asset"]; if (a.value("type", "") != std::string("prefab")) continue;
        if (!entityData.contains("id")) continue;
        auto itMap = idMapping.find(entityData["id"].get<EntityID>()); if (itMap == idMapping.end()) continue;
        EntityID rootNew = itMap->second;
        
        // === MIGRATION: Convert old prefabInstance.overrides to children array ===
        // Old format: prefabInstance: { overrides: [...], added: [...] }
        // New format: children: [{ _prefabNodePath: "...", ... }, ...]
        nlohmann::json childrenToProcess;
        if (entityData.contains("children") && entityData["children"].is_array()) {
            childrenToProcess = entityData["children"];
        } else if (entityData.contains("prefabInstance") && entityData["prefabInstance"].is_object()) {
            // Old format - migrate overrides to children format
            const auto& pi = entityData["prefabInstance"];
            childrenToProcess = nlohmann::json::array();
            
            // Migrate property overrides
            if (pi.contains("overrides") && pi["overrides"].is_array()) {
                for (const auto& ov : pi["overrides"]) {
                    if (!ov.is_object()) continue;
                    nlohmann::json child;
                    
                    // Convert target GUID to path (requires lookup in instance)
                    // For now, copy the override data and let GUID-based resolution handle it
                    if (ov.contains("targetGuid")) {
                        child["_prefabEntityGuid"] = ov["targetGuid"];
                    }
                    if (ov.contains("hints") && ov["hints"].is_object()) {
                        const auto& hints = ov["hints"];
                        if (hints.contains("nodePath")) {
                            child["_prefabNodePath"] = hints["nodePath"].get<std::string>();
                        }
                    }
                    
                    // Copy component value
                    if (ov.contains("component") && ov.contains("value")) {
                        std::string compKey = ov["component"].get<std::string>();
                        child[compKey] = ov["value"];
                    }
                    
                    if (!child.empty()) {
                        childrenToProcess.push_back(std::move(child));
                    }
                }
                std::cout << "[Migration] Converted " << childrenToProcess.size() << " old prefab overrides to children format\n";
            }
        }
        
        if (childrenToProcess.empty() || !childrenToProcess.is_array()) continue;
        
        // Build path resolution helper for prefab hierarchy
        auto resolveByPath = [&](const std::string& path) -> EntityID {
            EntityID target = rootNew;
            if (path.empty()) return target;
            std::stringstream ss(path); std::string part;
            while (std::getline(ss, part, '/')) {
                auto* d = scene.GetEntityData(target); if (!d) return (EntityID)-1;
                auto normalize = [](const std::string& name) -> std::string {
                    size_t us = name.find_last_of('_');
                    if (us == std::string::npos) return name;
                    bool digits = true; for (size_t i = us + 1; i < name.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; } }
                    if (!digits) return name;
                    return name.substr(0, us);
                };
                const std::string partNorm = normalize(part);
                auto lower = [](std::string s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };
                EntityID next = (EntityID)-1;
                for (EntityID c : d->Children) {
                    auto* cd = scene.GetEntityData(c); if (!cd) continue;
                    const std::string childName = cd->Name;
                    if (childName == part || normalize(childName) == partNorm || lower(childName) == lower(part) || lower(normalize(childName)) == lower(partNorm)) { next = c; break; }
                }
                if (next == (EntityID)-1) return (EntityID)-1; target = next;
            }
            return target;
        };
        
        // Build GUID lookup map for fallback resolution
        std::unordered_map<ClaymoreGUID, EntityID> guidToEntity;
        {
            std::function<void(EntityID)> collectGuids = [&](EntityID id) {
                auto* d = scene.GetEntityData(id); if (!d) return;
                if (!(d->EntityGuid.high == 0 && d->EntityGuid.low == 0)) {
                    guidToEntity[d->EntityGuid] = id;
                }
                for (EntityID c : d->Children) collectGuids(c);
            };
            collectGuids(rootNew);
        }
        
        // Process children overrides
        for (const auto& childOverride : childrenToProcess) {
            // Check for prefab node path (or fallback to model node path for backward compat)
            std::string relPath;
            if (childOverride.contains("_prefabNodePath")) {
                relPath = childOverride["_prefabNodePath"].get<std::string>();
            } else if (childOverride.contains("_modelNodePath")) {
                relPath = childOverride["_modelNodePath"].get<std::string>();
            } else {
                continue; // No path info, skip
            }
            
            // Resolve target entity by GUID first (most stable), then fallback to path
            EntityID target = INVALID_ENTITY_ID;
            if (childOverride.contains("_prefabEntityGuid")) {
                ClaymoreGUID guid;
                try { childOverride.at("_prefabEntityGuid").get_to(guid); } catch (...) {}
                auto it = guidToEntity.find(guid);
                if (it != guidToEntity.end()) {
                    target = it->second;
                }
            }
            if (target == INVALID_ENTITY_ID) {
                target = resolveByPath(relPath);
            }
            
            // Handle user-added entities that don't exist in fresh prefab
            if (target == INVALID_ENTITY_ID) {
                // Find parent by parsing path
                std::string parentPath;
                auto slashPos = relPath.find_last_of('/');
                if (slashPos != std::string::npos) {
                    parentPath = relPath.substr(0, slashPos);
                }
                EntityID parentTarget = parentPath.empty() ? rootNew : resolveByPath(parentPath);
                if (parentTarget == INVALID_ENTITY_ID) parentTarget = rootNew;
                
                // Create new entity for this added node
                std::string name = childOverride.value("name", "AddedNode");
                Entity newEnt = scene.CreateEntity(name);
                target = newEnt.GetID();
                scene.SetParent(target, parentTarget);
                
                // Restore GUID if provided
                if (childOverride.contains("guid")) {
                    auto* td = scene.GetEntityData(target);
                    if (td) {
                        try { childOverride.at("guid").get_to(td->EntityGuid); } catch (...) {}
                    }
                }
            }
            
            // Apply overrides to the target entity
            auto* td = scene.GetEntityData(target); if (!td) continue;
            
            // Apply component overrides (same pattern as model children)
            if (childOverride.contains("transform")) { DeserializeTransform(childOverride["transform"], td->Transform); scene.MarkTransformDirty(target); }
            if (childOverride.contains("mesh")) {
                if (!td->Mesh) td->Mesh = std::make_unique<MeshComponent>();
                json meshOverride = childOverride["mesh"];
                if (meshOverride.is_object() && td->Mesh->mesh) {
                    // Keep the freshly instantiated model mesh identity. Stored prefab child
                    // overrides may still carry stale meshReference/fileID values after reimport.
                    meshOverride.erase("meshReference");
                    meshOverride.erase("fileID");
                }
                DeserializeMesh(meshOverride, *td->Mesh, td->RenderOverrides);
            }
            if (childOverride.contains("renderOverrides")) { if (!td->RenderOverrides) td->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(childOverride["renderOverrides"], *td->RenderOverrides); }
            if (childOverride.contains("light")) { if (!td->Light) td->Light = std::make_unique<LightComponent>(); DeserializeLight(childOverride["light"], *td->Light); }
            if (childOverride.contains("collider")) { if (!td->Collider) td->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(childOverride["collider"], *td->Collider); }
            if (childOverride.contains("area")) { if (!td->Area) td->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(childOverride["area"], *td->Area); }
            if (childOverride.contains("rigidbody")) { if (!td->RigidBody) td->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(childOverride["rigidbody"], *td->RigidBody); }
            if (childOverride.contains("staticbody")) { if (!td->StaticBody) td->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(childOverride["staticbody"], *td->StaticBody); }
            if (childOverride.contains("characterController")) { if (!td->CharacterController) td->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(childOverride["characterController"], *td->CharacterController); }
            if (childOverride.contains("camera")) { if (!td->Camera) td->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(childOverride["camera"], *td->Camera); }
            if (childOverride.contains("terrain")) { if (!td->Terrain) td->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(childOverride["terrain"], *td->Terrain); }
            if (childOverride.contains("emitter")) { if (!td->Emitter) td->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(childOverride["emitter"], *td->Emitter); }
            if (childOverride.contains("canvas")) { if (!td->Canvas) td->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(childOverride["canvas"], *td->Canvas); }
            if (childOverride.contains("panel")) { if (!td->Panel) td->Panel = std::make_unique<PanelComponent>(); DeserializePanel(childOverride["panel"], *td->Panel); }
            if (childOverride.contains("button")) { if (!td->Button) td->Button = std::make_unique<ButtonComponent>(); DeserializeButton(childOverride["button"], *td->Button); }
            if (childOverride.contains("slider")) { if (!td->Slider) td->Slider = std::make_unique<SliderComponent>(); DeserializeSlider(childOverride["slider"], *td->Slider); }
            if (childOverride.contains("progressBar")) { if (!td->ProgressBar) td->ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(childOverride["progressBar"], *td->ProgressBar); }
            if (childOverride.contains("toggle")) { if (!td->Toggle) td->Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(childOverride["toggle"], *td->Toggle); }
            if (childOverride.contains("scrollView")) { if (!td->ScrollView) td->ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(childOverride["scrollView"], *td->ScrollView); }
            if (childOverride.contains("layoutGroup")) { if (!td->LayoutGroup) td->LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(childOverride["layoutGroup"], *td->LayoutGroup); }
            if (childOverride.contains("inputField")) { if (!td->InputField) td->InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(childOverride["inputField"], *td->InputField); }
            if (childOverride.contains("dropdown")) { if (!td->Dropdown) td->Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(childOverride["dropdown"], *td->Dropdown); }
            if (childOverride.contains("text")) { if (!td->Text) td->Text = std::make_unique<TextRendererComponent>(); DeserializeText(childOverride["text"], *td->Text); }
            if (childOverride.contains("navmesh")) { if (!td->Navigation) td->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(childOverride["navmesh"], *td->Navigation); }
            if (childOverride.contains("navagent")) { if (!td->NavAgent) td->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(childOverride["navagent"], *td->NavAgent); }
            if (childOverride.contains("navlink")) { if (!td->NavLink) td->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(childOverride["navlink"], *td->NavLink); }
            if (childOverride.contains("portal")) { if (!td->Portal) td->Portal = std::make_unique<PortalComponent>(); DeserializePortal(childOverride["portal"], *td->Portal); }
            if (childOverride.contains("grassDeformer")) { if (!td->GrassDeformer) td->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(childOverride["grassDeformer"], *td->GrassDeformer); }
            if (childOverride.contains("boneAttachment")) { if (!td->BoneAttachment) td->BoneAttachment = std::make_unique<BoneAttachmentComponent>(); DeserializeBoneAttachment(childOverride["boneAttachment"], *td->BoneAttachment); }
            if (childOverride.contains("tintController")) { if (!td->TintController) td->TintController = std::make_unique<TintMaskController>(); DeserializeTintController(childOverride["tintController"], *td->TintController); }
            if (childOverride.contains("scripts")) { DeserializeScripts(childOverride["scripts"], td->Scripts, scene); }
            if (childOverride.contains("animator")) { if (!td->AnimationPlayer) td->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(childOverride["animator"], *td->AnimationPlayer); }
            if (childOverride.contains("name")) {
                std::string name = childOverride["name"].get<std::string>();
                // Strip numeric suffix from model-internal children's names during deserialization
                // Only strip if NOT user-added (user-added children keep their suffixes)
                if (!childOverride.contains("_added") || !childOverride["_added"].get<bool>()) {
                    size_t us = name.find_last_of('_');
                    if (us != std::string::npos && us > 0 && us < name.size() - 1) {
                        bool allDigits = true;
                        for (size_t i = us + 1; i < name.size(); ++i) {
                            if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                allDigits = false;
                                break;
                            }
                        }
                        if (allDigits) {
                            name = name.substr(0, us);
                        }
                    }
                }
                td->Name = name;
            }
            if (childOverride.contains("active")) { td->Active = childOverride["active"].get<bool>(); }
            if (childOverride.contains("visible")) { td->Visible = childOverride["visible"].get<bool>(); }
            if (childOverride.contains("layer")) { 
                try {
                    if (childOverride["layer"].is_string()) {
                        // Handle string layer (legacy format or prefab serialization quirk)
                        std::string layerStr = childOverride["layer"].get<std::string>();
                        td->Layer = std::stoi(layerStr);
                    } else {
                        td->Layer = childOverride["layer"].get<int>();
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[Serializer] ERROR deserializing layer for entity '" << td->Name << "': " << e.what() << std::endl;
                }
            }
            if (childOverride.contains("tag")) { 
                try {
                    if (childOverride["tag"].is_string()) {
                        // Handle string tag (legacy format or prefab serialization quirk)
                        std::string tagStr = childOverride["tag"].get<std::string>();
                        td->Tag = std::stoi(tagStr);
                    } else {
                        td->Tag = childOverride["tag"].get<int>();
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[Serializer] ERROR deserializing tag for entity '" << td->Name << "': " << e.what() << std::endl;
                }
            }
        }
    }

    // Ensure transforms are dirty and updated after load
    for (const auto& entity : scene.GetEntities()) {
        scene.MarkTransformDirty(entity.GetID());
    }
    scene.UpdateTransforms();

    // Final sweep: resolve any script entity references that may have become valid
    // after model/prefab overrides, renames, or transform updates.
    deferred_resolve_script_entity_refs();
    relink_mesh_proxies();

    // Unify scene-load fixups with prefab/runtime behavior. Deserialized scenes can
    // contain prefab instances and model-authored skeletons, so runtime-only caches
    // like BoneEntities and SkeletonRoot need the same authoritative rebuild pass.
    cm::PostProcessEntities(scene);

    // Post pass: validate and rebind skeletons for all skinned meshes
    try {
        // Build name maps for skeleton roots
        std::unordered_map<EntityID, std::unordered_map<std::string, EntityID>> skelMaps;
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID()); if (!d) continue;
            if (d->Skeleton) {
                std::unordered_map<std::string, EntityID> byName;
                std::function<void(EntityID)> dfs = [&](EntityID id){
                    auto* nd = scene.GetEntityData(id); if (!nd) return;
                    byName[nd->Name] = id;
                    for (EntityID c : nd->Children) dfs(c);
                };
                dfs(e.GetID());
                skelMaps[e.GetID()] = std::move(byName);

                // Ensure BindPoseGlobals is computed from InverseBindPoses (skinning fallback)
                size_t n = d->Skeleton->InverseBindPoses.size();
                if (d->Skeleton->BindPoseGlobals.size() != n) {
                    d->Skeleton->BindPoseGlobals.resize(n);
                    for (size_t i = 0; i < n; ++i) {
                        d->Skeleton->BindPoseGlobals[i] = glm::inverse(d->Skeleton->InverseBindPoses[i]);
                    }
                }
            }
        }
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID()); if (!d || !d->Skinning) continue;
            EntityID skelRoot = d->Skinning->SkeletonRoot;
            auto it = skelMaps.find(skelRoot);
            if (it == skelMaps.end()) continue;
            // Find skeleton component at this root
            auto* sk = scene.GetEntityData(skelRoot);
            if (!sk || !sk->Skeleton) continue;
            auto& map = it->second;
            // If BoneEntities missing or wrong size, rebuild by BoneNames
            size_t n = sk->Skeleton->InverseBindPoses.size();
            bool bad = sk->Skeleton->BoneEntities.size() != n;
            if (!bad) {
                for (auto id : sk->Skeleton->BoneEntities) { if (id == (EntityID)-1) { bad = true; break; } }
            }
            if (bad) {
                sk->Skeleton->BoneEntities.assign(n, (EntityID)-1);
                // Build index->name array
                std::vector<std::string> names(n);
                if (!sk->Skeleton->BoneNames.empty()) names = sk->Skeleton->BoneNames;
                else {
                    for (const auto& kv : sk->Skeleton->BoneNameToIndex) {
                        int idx = kv.second; if (idx>=0 && (size_t)idx < n) names[(size_t)idx] = kv.first;
                    }
                }
                for (size_t i=0;i<n;++i) {
                    const std::string& bn = names[i]; if (bn.empty()) continue;
                    auto itn = map.find(bn);
                    if (itn != map.end()) sk->Skeleton->BoneEntities[i] = itn->second;
                }
            }
            // Reset runtime-only skinning caches; SkinningSystem rebuilds the
            // authoritative GPU skinning bindings from the linked skeleton.
            d->Skinning->BoneCount = 0;
            d->Skinning->ResetGpuSharedSkeletonSource();
            d->Skinning->ClearGpuRetargetData();
        }
    } catch(...) {}

    // -------------------------------------------------------------------------------------
    // Post-load de-duplication pass
    // Goal: eliminate accidental duplicates created during deserialization without touching
    //       instantiated model hierarchies (opaque roots and their descendants).
    // Currently handles Cameras (most common offender) using a structural signature.
    // -------------------------------------------------------------------------------------
    {
        // Build the protected set = opaque roots + all their descendants
        std::unordered_set<EntityID> protectedIds;
        auto addDescendants = [&](auto&& self, EntityID id) -> void {
            if (protectedIds.count(id)) return;
            protectedIds.insert(id);
            if (auto* d = scene.GetEntityData(id)) {
                for (EntityID c : d->Children) self(self, c);
            }
        };
        for (EntityID root : opaqueRoots) addDescendants(addDescendants, root);

        auto normalizeName = [](const std::string& name) -> std::string {
            size_t us = name.find_last_of('_');
            if (us == std::string::npos) return name;
            bool digits = true;
            for (size_t i = us + 1; i < name.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; }
            }
            return digits ? name.substr(0, us) : name;
        };

        auto round3 = [](float v) -> int { return static_cast<int>(std::round(v * 1000.0f)); };

        struct SignatureKey {
            std::string key;
            EntityID keptId = (EntityID)-1;
        };

        std::unordered_map<std::string, EntityID> signatureToEntity;
        std::vector<EntityID> entitiesToRemove;

        for (const auto& e : scene.GetEntities()) {
            EntityID id = e.GetID();
            if (protectedIds.count(id)) continue; // never dedup model-instantiated hierarchies
            auto* d = scene.GetEntityData(id);
            if (!d) continue;

            // Only dedup Cameras for now
            if (!d->Camera) continue;

            // Build a structural signature of the camera + transform
            const auto& cam = *d->Camera;
            const auto& t = d->Transform;
            std::ostringstream oss;
            oss << "type=camera";
            oss << "|name=" << normalizeName(d->Name);
            oss << "|layer=" << d->Layer << "|tag=" << d->Tag;
            oss << "|active=" << cam.Active << "|prio=" << cam.priority
                << "|fov=" << round3(cam.FieldOfView) << "|near=" << round3(cam.NearClip)
                << "|far=" << round3(cam.FarClip) << "|persp=" << cam.IsPerspective;
            // Position/rotation/scale rounded to avoid float noise
            oss << "|px=" << round3(t.Position.x) << "|py=" << round3(t.Position.y) << "|pz=" << round3(t.Position.z);
            oss << "|rx=" << round3(t.Rotation.x) << "|ry=" << round3(t.Rotation.y) << "|rz=" << round3(t.Rotation.z);
            oss << "|sx=" << round3(t.Scale.x) << "|sy=" << round3(t.Scale.y) << "|sz=" << round3(t.Scale.z);

            std::string sig = oss.str();
            auto it = signatureToEntity.find(sig);
            if (it == signatureToEntity.end()) {
                signatureToEntity.emplace(std::move(sig), id);
            } else {
                // Duplicate found: remove the later one
                entitiesToRemove.push_back(id);
            }
        }

        // Remove duplicates after iteration
        for (EntityID rid : entitiesToRemove) {
            scene.RemoveEntity(rid);
        }
        
        // Also remove orphaned skeleton/model nodes that ended up outside their intended model hierarchy
        // These occur when the child override matching fails and creates a duplicate at scene root
        std::vector<EntityID> orphanedModelNodes;
        for (const auto& e : scene.GetEntities()) {
            EntityID id = e.GetID();
            if (protectedIds.count(id)) continue; // skip properly instantiated model nodes
            auto* d = scene.GetEntityData(id);
            if (!d) continue;
            
            // Check if this looks like a model node (has skeleton, skinning, or mesh from model)
            bool looksLikeModelNode = d->Skeleton || d->Skinning || d->BlendShapes;
            if (!looksLikeModelNode && d->Mesh && d->Mesh->mesh) {
                // Mesh nodes from models usually have a meshReference with a valid fileID
                looksLikeModelNode = d->Mesh->meshReference.fileID >= 0;
            }
            
            if (!looksLikeModelNode) continue;
            
            // Check if there's already a properly instantiated entity with the same name
            // in one of the opaque root hierarchies
            std::string normName = normalizeName(d->Name);
            for (EntityID rootId : opaqueRoots) {
                std::function<bool(EntityID)> findInHierarchy = [&](EntityID cur) -> bool {
                    auto* cd = scene.GetEntityData(cur);
                    if (!cd) return false;
                    if (normalizeName(cd->Name) == normName) return true;
                    for (EntityID c : cd->Children) {
                        if (findInHierarchy(c)) return true;
                    }
                    return false;
                };
                if (findInHierarchy(rootId)) {
                    // Found a properly instantiated version - this one is a duplicate
                    orphanedModelNodes.push_back(id);
                    std::cout << "[DeduplicateOrphan] Removing orphaned model node '" << d->Name << "' (id=" << id << ")" << std::endl;
                    break;
                }
            }
        }
        for (EntityID rid : orphanedModelNodes) {
            scene.RemoveEntity(rid);
        }
    }

    try {
        // Dump GUID -> hierarchy path map
        auto computePath = [&](EntityID id) -> std::string {
            return ComputeScenePath(scene, id);
        };
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID()); if (!d) continue;
            }
        std::cout << "[DeserializeEnd] entities=" << scene.GetEntities().size() << std::endl;
    } catch(...) {}
    
    // Post-load: Resolve IK constraint target/pole entity GUIDs to EntityIDs
    try {
        // Build GUID to EntityID lookup for efficiency
        // Use full GUID (high, low) as key to avoid hash collisions
        struct GuidKey {
            uint64_t high;
            uint64_t low;
            bool operator==(const GuidKey& other) const {
                return high == other.high && low == other.low;
            }
        };
        struct GuidKeyHash {
            size_t operator()(const GuidKey& k) const {
                // Combine high and low with a good hash function
                return std::hash<uint64_t>{}(k.high) ^ (std::hash<uint64_t>{}(k.low) << 1);
            }
        };
        std::unordered_map<GuidKey, EntityID, GuidKeyHash> guidToEntity;
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID());
            if (!d) continue;
            if (d->EntityGuid.high != 0 || d->EntityGuid.low != 0) {
                guidToEntity[{d->EntityGuid.high, d->EntityGuid.low}] = e.GetID();
            }
        }
        
        auto resolveGuid = [&](uint64_t high, uint64_t low) -> EntityID {
            if (high == 0 && low == 0) return 0;
            auto it = guidToEntity.find({high, low});
            if (it != guidToEntity.end()) return it->second;
            return 0;
        };
        
        for (const auto& e : scene.GetEntities()) {
            auto* d = scene.GetEntityData(e.GetID());
            if (!d) continue;
            
            // Resolve IK targets
            for (auto& ik : d->IKs) {
                if (ik.TargetEntityGuidHigh != 0 || ik.TargetEntityGuidLow != 0) {
                    EntityID resolved = resolveGuid(ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
                    if (resolved != 0) {
                        ik.TargetEntity = resolved;
                    } else if (ik.TargetEntity == 0 || !scene.GetEntityData(ik.TargetEntity)) {
                        ik.TargetEntity = 0;
                    }
                }
                if (ik.PoleEntityGuidHigh != 0 || ik.PoleEntityGuidLow != 0) {
                    EntityID resolved = resolveGuid(ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
                    if (resolved != 0) {
                        ik.PoleEntity = resolved;
                    } else if (ik.PoleEntity == 0 || !scene.GetEntityData(ik.PoleEntity)) {
                        ik.PoleEntity = 0;
                    }
                }
            }
            
            // Resolve LookAt targets
            for (auto& lac : d->LookAtConstraints) {
                if (lac.TargetEntityGuidHigh != 0 || lac.TargetEntityGuidLow != 0) {
                    EntityID resolved = resolveGuid(lac.TargetEntityGuidHigh, lac.TargetEntityGuidLow);
                    if (resolved != 0) {
                        lac.TargetEntity = resolved;
                    } else if (lac.TargetEntity == 0 || !scene.GetEntityData(lac.TargetEntity)) {
                        lac.TargetEntity = 0;
                    }
                }
            }
        }
    } catch(...) {
        std::cerr << "[Serializer] IK/LookAt GUID resolution failed" << std::endl;
    }

    // Restore river meshes from binary assets
    try {
        River::RestoreRiverMeshes(scene);
    } catch(const std::exception& e) {
        std::cerr << "[Serializer] River mesh restoration failed: " << e.what() << std::endl;
    } catch(...) {
        std::cerr << "[Serializer] River mesh restoration failed" << std::endl;
    }

    return true;
}

bool Serializer::SaveSceneToFile(Scene& scene, const std::string& filepath) {
    try {
        // Ensure terrain assets are stored per-scene when using default/shared paths.
        // This allows duplicated scenes to maintain independent terrain data.
        auto buildSceneTerrainPath = [&](const std::string& scenePath, const ClaymoreGUID& guid) -> std::string {
            std::filesystem::path p(scenePath);
            std::string stem = p.stem().string();
            // Sanitize stem to be filesystem-friendly
            for (char& c : stem) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
            }
            return ".bin/terrain/scenes/" + stem + "_" + guid.ToString() + ".terrainbin";
        };
        auto startsWith = [](const std::string& s, const char* prefix) -> bool {
            return s.rfind(prefix, 0) == 0;
        };
        for (const auto& entity : scene.GetEntities()) {
            if (auto* data = scene.GetEntityData(entity.GetID())) {
                if (data->Terrain) {
                    TerrainComponent& terrain = *data->Terrain;
                    const std::string defaultPath = TerrainComponent::BuildDefaultAssetPath(terrain.TerrainDataGuid);
                    const bool isDefaultPath = terrain.AssetPath.empty() || terrain.AssetPath == defaultPath ||
                                               startsWith(terrain.AssetPath, ".bin/terrain/terrain_");
                    const bool isScenePath = startsWith(terrain.AssetPath, ".bin/terrain/scenes/");
                    if (isDefaultPath && !isScenePath) {
                        terrain.AssetPath = buildSceneTerrainPath(filepath, terrain.TerrainDataGuid);
                        terrain.AssetDirty = true;
                    }
                }
            }
        }

        json sceneData = SerializeScene(scene);
        for (const auto& entity : scene.GetEntities()) {
            if (auto* data = scene.GetEntityData(entity.GetID())) {
                if (data->Terrain) {
                    Terrain::SaveTerrainAsset(*data->Terrain);
                }
            }
        }
        
        // Ensure directory exists
        fs::path path(filepath);
        fs::create_directories(path.parent_path());
        
        std::ofstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "[Serializer] Failed to open file for writing: " << filepath << std::endl;
            return false;
        }
        
        file << sceneData.dump(4); // Pretty print with 4 spaces
        file.close();
        
        std::cout << "[Serializer] Scene saved to: " << filepath << std::endl;
        // Clear editor dirty flag on successful save
        scene.ClearDirty();
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[Serializer] Error saving scene: " << e.what() << std::endl;
        return false;
    }
}

bool Serializer::LoadSceneFromFile(const std::string& filepath, Scene& scene) {
    try {
        // In play mode or runtime, try binary scene format first
        if (Assets::ShouldLoadBinary()) {
            std::string sourcePath = filepath;
            std::filesystem::path srcPath(sourcePath);
            if (srcPath.extension() == ".sceneb") {
                srcPath.replace_extension(".scene");
                sourcePath = srcPath.string();
            }

            std::string binaryPath = Assets::GetBinaryPath(sourcePath);
            if (!binaryPath.empty()) {
                if (binary::EntityBinaryLoader::Load(binaryPath, scene)) {
                    std::cout << "[Serializer] Loaded binary scene: " << binaryPath << std::endl;
                    prefab::QueueScenePrefabs(scene);
                    return true;
                }
            }
            
            // Also try with .sceneb extension directly
            std::filesystem::path binPath(sourcePath);
            binPath.replace_extension(".sceneb");
            if (binary::EntityBinaryLoader::Load(binPath.string(), scene)) {
                std::cout << "[Serializer] Loaded binary scene: " << binPath << std::endl;
                prefab::QueueScenePrefabs(scene);
                return true;
            }
            
            // If filepath already ends with .sceneb, try it directly
            if (filepath.find(".sceneb") != std::string::npos) {
                if (binary::EntityBinaryLoader::Load(filepath, scene)) {
                    std::cout << "[Serializer] Loaded binary scene: " << filepath << std::endl;
                    prefab::QueueScenePrefabs(scene);
                    return true;
                }
            }

#if !defined(CLAYMORE_RUNTIME)
            if (TryEnsureBinaryForPlayMode(sourcePath)) {
                std::string compiledPath = Assets::GetBinaryPath(sourcePath);
                if (!compiledPath.empty()) {
                    if (binary::EntityBinaryLoader::Load(compiledPath, scene)) {
                        std::cout << "[Serializer] Loaded compiled binary scene: " << compiledPath << std::endl;
                        prefab::QueueScenePrefabs(scene);
                        return true;
                    }
                }
                std::filesystem::path compiledExt(sourcePath);
                compiledExt.replace_extension(".sceneb");
                if (binary::EntityBinaryLoader::Load(compiledExt.string(), scene)) {
                    std::cout << "[Serializer] Loaded compiled binary scene: " << compiledExt << std::endl;
                    prefab::QueueScenePrefabs(scene);
                    return true;
                }
            }
#endif
        }
        if (Assets::ShouldLoadBinary() && !Assets::AllowSourceFallback()) {
            std::cerr << "[Serializer] Missing binary scene (binary-only mode): " << filepath << std::endl;
            return false;
        }

        // Fall back to JSON loading (editor mode or when binary not available)
        json sceneData;
        // Virtual filesystem first; no direct OS reads for runtime
        {
            std::string sceneText;
            if (FileSystem::Instance().ReadTextFile(filepath, sceneText)) {
                sceneData = json::parse(sceneText);
            } else {
                std::vector<uint8_t> bytes;
                if (FileSystem::Instance().ReadFile(filepath, bytes)) {
                    sceneData = json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                } else {
                    std::cerr << "[Serializer] Scene file does not exist or cannot be read: " << filepath << std::endl;
                    return false;
                }
            }
        }
        const std::string version = sceneData.value("version", "");
        std::cout << "[SceneLoad] Version=" << version << " Entities=" << (sceneData.contains("entities") ? sceneData["entities"].size() : 0) << std::endl;
        
        bool success = DeserializeScene(sceneData, scene);
        if (success) {
            std::cout << "[Serializer] Scene loaded from: " << filepath << std::endl;
            prefab::QueueScenePrefabs(scene);
        }
        return success;
    }
    catch (const std::exception& e) {
        std::cerr << "[Serializer] Error loading scene: " << e.what() << std::endl;
        // Try to extract more context about the error
        if (auto* jsonErr = dynamic_cast<const nlohmann::json::type_error*>(&e)) {
            std::cerr << "[Serializer] JSON type error - this usually means a field expected a number but got a string (or vice versa)" << std::endl;
            std::cerr << "[Serializer] Common causes: entity ID/parent fields serialized as strings, or layer/tag fields as strings" << std::endl;
        }
        return false;
    } catch (const nlohmann::json::type_error& e) {
        std::cerr << "[Serializer] JSON type error loading scene: " << e.what() << std::endl;
        std::cerr << "[Serializer] This usually means a field expected a number but got a string (or vice versa)" << std::endl;
        return false;
    }
}

// Prefab serialization
json Serializer::SerializePrefab(const EntityData& entityData, Scene& scene) {
    json prefabData;
    prefabData["version"] = "1.0";
    prefabData["type"] = "prefab";
    
    // Create a temporary entity to serialize (simplified direct copy of components)
    json entityJson;
    entityJson["name"] = entityData.Name;
    entityJson["layer"] = entityData.Layer;
    entityJson["tag"] = entityData.Tag;
    entityJson["transform"] = SerializeTransform(entityData.Transform);
    
    if (entityData.Mesh) {
        entityJson["mesh"] = SerializeMesh(*entityData.Mesh);
    }
    
    if (entityData.Light) {
        entityJson["light"] = SerializeLight(*entityData.Light);
    }
    
    if (entityData.Collider) {
        entityJson["collider"] = SerializeCollider(*entityData.Collider);
    }
    if (entityData.RigidBody) {
        entityJson["rigidbody"] = SerializeRigidBody(*entityData.RigidBody);
    }
    if (entityData.StaticBody) {
        entityJson["staticbody"] = SerializeStaticBody(*entityData.StaticBody);
    }
    if (entityData.Camera) {
        entityJson["camera"] = SerializeCamera(*entityData.Camera);
    }
    if (entityData.AudioSource) {
        entityJson["audioSource"] = SerializeAudioSource(*entityData.AudioSource);
    }
    if (entityData.AudioListener) {
        entityJson["audioListener"] = SerializeAudioListener(*entityData.AudioListener);
    }
    if (entityData.Terrain) {
        entityJson["terrain"] = SerializeTerrain(*entityData.Terrain);
    }
    if (entityData.ResourceLayers) {
        entityJson["resourceLayers"] = SerializeResourceLayers(*entityData.ResourceLayers);
    }
    if (entityData.Instancer) {
        entityJson["instancer"] = SerializeInstancer(*entityData.Instancer);
    }
    if (entityData.Emitter) {
        entityJson["emitter"] = SerializeParticleEmitter(*entityData.Emitter);
    }
    // Character Controller
    if (entityData.CharacterController) {
        entityJson["characterController"] = SerializeCharacterController(*entityData.CharacterController);
    }

    if (!entityData.Scripts.empty()) {
        entityJson["scripts"] = SerializeScripts(scene, entityData.Scripts);
    }
    if (entityData.AnimationPlayer) {
        entityJson["animator"] = SerializeAnimator(*entityData.AnimationPlayer);
    }
    if (entityData.Skeleton) {
        entityJson["skeleton"] = SerializeSkeleton(*entityData.Skeleton);
    }
    if (entityData.Skinning) {
        entityJson["skinning"] = SerializeSkinning(*entityData.Skinning);
    }

    // Blend shape weights (only weights, geometry comes from mesh file)
    if (entityData.BlendShapes && !entityData.BlendShapes->Shapes.empty()) {
        json bsWeights = SerializeBlendShapeWeights(*entityData.BlendShapes);
        if (!bsWeights.empty()) {
            entityJson["blendShapeWeights"] = std::move(bsWeights);
        }
    }

    // Unified morph weights (aggregated blend shapes at model root)
    if (entityData.UnifiedMorph && !entityData.UnifiedMorph->Names.empty()) {
        json umWeights = SerializeUnifiedMorphWeights(*entityData.UnifiedMorph);
        if (!umWeights.empty()) {
            entityJson["unifiedMorphWeights"] = std::move(umWeights);
        }
    }

    // TintMaskController
    if (entityData.TintController) {
        entityJson["tintController"] = SerializeTintController(*entityData.TintController);
    }

    // UI components in prefabs
    if (entityData.Canvas) entityJson["canvas"] = SerializeCanvas(*entityData.Canvas);
    if (entityData.Panel)  entityJson["panel"]  = SerializePanel(*entityData.Panel);
    if (entityData.Button) entityJson["button"] = SerializeButton(*entityData.Button);
    if (entityData.Slider) entityJson["slider"] = SerializeSlider(*entityData.Slider);
    if (entityData.ProgressBar) entityJson["progressBar"] = SerializeProgressBar(*entityData.ProgressBar);
    if (entityData.Toggle) entityJson["toggle"] = SerializeToggle(*entityData.Toggle);
    if (entityData.ScrollView) entityJson["scrollView"] = SerializeScrollView(*entityData.ScrollView);
    if (entityData.LayoutGroup) entityJson["layoutGroup"] = SerializeLayoutGroup(*entityData.LayoutGroup);
    if (entityData.InputField) entityJson["inputField"] = SerializeInputField(*entityData.InputField);
    if (entityData.Dropdown) entityJson["dropdown"] = SerializeDropdown(*entityData.Dropdown);
    if (entityData.Text)   entityJson["text"]   = SerializeText(*entityData.Text);

    // Areas
    if (entityData.Area) entityJson["area"] = Serializer::SerializeArea(*entityData.Area);

    // Navigation components in prefabs
    if (entityData.Navigation) entityJson["navmesh"] = SerializeNavMesh(*entityData.Navigation);
    if (entityData.NavAgent)   entityJson["navagent"] = SerializeNavAgent(*entityData.NavAgent);
    if (entityData.NavLink)    entityJson["navlink"] = SerializeNavLink(*entityData.NavLink);
    if (entityData.Portal)     entityJson["portal"] = SerializePortal(*entityData.Portal);

    // IK authored blocks (mirror SerializeEntity)
    if (!entityData.IKs.empty()) {
        nlohmann::json jIk = nlohmann::json::array();
        for (const auto& k : entityData.IKs) {
            nlohmann::json e;
            e["enabled"] = k.Enabled;
            e["target"] = k.TargetEntity;
            e["pole"] = k.PoleEntity;
            e["weight"] = k.Weight;
            e["maxIterations"] = k.MaxIterations;
            e["tolerance"] = k.Tolerance;
            e["damping"] = k.Damping;
            e["useTwoBone"] = k.UseTwoBone;
            e["lockAxisX"] = k.LockAxisX;
            e["lockAxisY"] = k.LockAxisY;
            e["lockAxisZ"] = k.LockAxisZ;
            e["rootBone"] = k.ChainRootHint;
            e["tipBone"] = k.ChainEffectorHint;
            e["visualize"] = k.Visualize;
            e["chain"] = nlohmann::json::array();
            for (auto b : k.Chain) e["chain"].push_back((int)b);
            if (!k.Constraints.empty()) {
                e["constraints"] = nlohmann::json::array();
                for (const auto& c : k.Constraints) {
                    nlohmann::json cj;
                    cj["useTwist"] = c.useTwist; cj["useHinge"] = c.useHinge;
                    cj["twistMinDeg"] = c.twistMinDeg; cj["twistMaxDeg"] = c.twistMaxDeg;
                    cj["hingeMinDeg"] = c.hingeMinDeg; cj["hingeMaxDeg"] = c.hingeMaxDeg;
                    e["constraints"].push_back(std::move(cj));
                }
            }
            jIk.push_back(std::move(e));
        }
        entityJson["ik"] = std::move(jIk);
    }

    // LookAt constraint blocks (mirror SerializeEntity)
    if (!entityData.LookAtConstraints.empty()) {
        nlohmann::json jLookAt = nlohmann::json::array();
        for (const auto& lac : entityData.LookAtConstraints) {
            nlohmann::json e;
            e["enabled"] = lac.Enabled;
            e["mode"] = static_cast<uint8_t>(lac.Mode);
            e["target"] = lac.TargetEntity;
            e["weight"] = lac.Weight;
            e["axes"] = static_cast<uint8_t>(lac.Axes);
            e["space"] = static_cast<uint8_t>(lac.Space);
            e["distribution"] = static_cast<uint8_t>(lac.Distribution);
            e["maxYawDeg"] = lac.MaxYawDeg;
            e["maxPitchDeg"] = lac.MaxPitchDeg;
            e["maxRollDeg"] = lac.MaxRollDeg;
            e["smoothingSpeed"] = lac.SmoothingSpeed;
            e["visualize"] = lac.Visualize;
            e["boneChain"] = nlohmann::json::array();
            for (auto b : lac.BoneChain) e["boneChain"].push_back((int)b);
            if (!lac.BoneWeights.empty()) {
                e["boneWeights"] = nlohmann::json::array();
                for (auto w : lac.BoneWeights) e["boneWeights"].push_back(w);
            }
            jLookAt.push_back(std::move(e));
        }
        entityJson["lookat"] = std::move(jLookAt);
    }

    // Preserve unknown/extra fields to keep parity with entity serialization
    if (entityData.Extra.is_object()) {
        for (auto it = entityData.Extra.begin(); it != entityData.Extra.end(); ++it) {
            if (!entityJson.contains(it.key())) entityJson[it.key()] = it.value();
        }
    }

    prefabData["entity"] = entityJson;
    return prefabData;
}

bool Serializer::DeserializePrefab(const json& data, EntityData& entityData, Scene& scene) {
    if (!data.contains("entity")) return false;

    const json& entityJson = data["entity"];
    
    // Reset the entity data
    entityData = EntityData{};
    
    // Deserialize basic properties
    if (entityJson.contains("name")) entityData.Name = entityJson["name"];
    if (entityJson.contains("layer")) entityData.Layer = entityJson["layer"];
    if (entityJson.contains("tag")) entityData.Tag = entityJson["tag"];
    if (entityJson.contains("visible")) entityData.Visible = entityJson["visible"];

    // Deserialize transform
    if (entityJson.contains("transform")) {
        DeserializeTransform(entityJson["transform"], entityData.Transform);
    }

    // Deserialize components
    if (entityJson.contains("mesh")) {
        MeshComponent tmp; DeserializeMesh(entityJson["mesh"], tmp, entityData.RenderOverrides);
        entityData.Mesh = std::make_unique<MeshComponent>(std::move(tmp));
    }

    if (entityJson.contains("light")) {
        entityData.Light = std::make_unique<LightComponent>();
        DeserializeLight(entityJson["light"], *entityData.Light);
    }

    if (entityJson.contains("collider")) {
        entityData.Collider = std::make_unique<ColliderComponent>();
        DeserializeCollider(entityJson["collider"], *entityData.Collider);
    }
    if (entityJson.contains("rigidbody")) {
        entityData.RigidBody = std::make_unique<RigidBodyComponent>();
        DeserializeRigidBody(entityJson["rigidbody"], *entityData.RigidBody);
    }
    if (entityJson.contains("staticbody")) {
        entityData.StaticBody = std::make_unique<StaticBodyComponent>();
        DeserializeStaticBody(entityJson["staticbody"], *entityData.StaticBody);
    }
    if (entityJson.contains("camera")) {
        entityData.Camera = std::make_unique<CameraComponent>();
        DeserializeCamera(entityJson["camera"], *entityData.Camera);
    }
    if (entityJson.contains("audioSource")) {
        entityData.AudioSource = std::make_unique<AudioSourceComponent>();
        DeserializeAudioSource(entityJson["audioSource"], *entityData.AudioSource);
    }
    if (entityJson.contains("audioListener")) {
        entityData.AudioListener = std::make_unique<AudioListenerComponent>();
        DeserializeAudioListener(entityJson["audioListener"], *entityData.AudioListener);
    }
    if (entityJson.contains("terrain")) {
        entityData.Terrain = std::make_unique<TerrainComponent>();
        DeserializeTerrain(entityJson["terrain"], *entityData.Terrain);
    }
    if (entityJson.contains("resourceLayers")) {
        entityData.ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>();
        DeserializeResourceLayers(entityJson["resourceLayers"], *entityData.ResourceLayers);
    }
    if (entityJson.contains("instancer")) {
        entityData.Instancer = std::make_unique<cm::instancer::InstancerComponent>();
        DeserializeInstancer(entityJson["instancer"], *entityData.Instancer);
    }
    if (entityJson.contains("emitter")) {
        entityData.Emitter = std::make_unique<ParticleEmitterComponent>();
        DeserializeParticleEmitter(entityJson["emitter"], *entityData.Emitter);
    }

    if (entityJson.contains("characterController")) {
        entityData.CharacterController = std::make_unique<CharacterControllerComponent>();
        DeserializeCharacterController(entityJson["characterController"], *entityData.CharacterController);
    }

    // Deserialize scripts
    if (entityJson.contains("scripts")) {
        DeserializeScripts(entityJson["scripts"], entityData.Scripts, scene);
    }
    if (entityJson.contains("animator")) {
        entityData.AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();
        DeserializeAnimator(entityJson["animator"], *entityData.AnimationPlayer);
    }
    if (entityJson.contains("skeleton")) {
        entityData.Skeleton = std::make_unique<SkeletonComponent>();
        DeserializeSkeleton(entityJson["skeleton"], *entityData.Skeleton);
    }
    if (entityJson.contains("skinning")) {
        entityData.Skinning = std::make_unique<SkinningComponent>();
        DeserializeSkinning(entityJson["skinning"], *entityData.Skinning);
        TryResolveSkinningSkeletonRootRef(entityJson["skinning"], *entityData.Skinning, scene);
    }
    // Store pending blend shape weights to apply when BlendShapes is populated
    if (entityJson.contains("blendShapeWeights") && entityJson["blendShapeWeights"].is_array()) {
        for (const auto& entry : entityJson["blendShapeWeights"]) {
            if (entry.is_object() && entry.contains("name") && entry.contains("weight")) {
                entityData.PendingBlendShapeWeights[entry["name"].get<std::string>()] = entry["weight"].get<float>();
            }
        }
    }
    // Store pending unified morph weights to apply when UnifiedMorph is populated
    if (entityJson.contains("unifiedMorphWeights") && entityJson["unifiedMorphWeights"].is_array()) {
        for (const auto& entry : entityJson["unifiedMorphWeights"]) {
            if (entry.is_object() && entry.contains("name") && entry.contains("weight")) {
                entityData.PendingUnifiedMorphWeights[entry["name"].get<std::string>()] = entry["weight"].get<float>();
            }
        }
    }
    // TintMaskController
    if (entityJson.contains("tintController") && entityJson["tintController"].is_object()) {
        entityData.TintController = std::make_unique<TintMaskController>();
        DeserializeTintController(entityJson["tintController"], *entityData.TintController);
    }

    // UI components in prefabs
    if (entityJson.contains("canvas")) { entityData.Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(entityJson["canvas"], *entityData.Canvas); }
    if (entityJson.contains("panel"))  { entityData.Panel  = std::make_unique<PanelComponent>();  DeserializePanel(entityJson["panel"],  *entityData.Panel ); }
    if (entityJson.contains("button")) { entityData.Button = std::make_unique<ButtonComponent>(); DeserializeButton(entityJson["button"], *entityData.Button); }
    if (entityJson.contains("slider")) { entityData.Slider = std::make_unique<SliderComponent>(); DeserializeSlider(entityJson["slider"], *entityData.Slider); }
    if (entityJson.contains("progressBar")) { entityData.ProgressBar = std::make_unique<ProgressBarComponent>(); DeserializeProgressBar(entityJson["progressBar"], *entityData.ProgressBar); }
    if (entityJson.contains("toggle")) { entityData.Toggle = std::make_unique<ToggleComponent>(); DeserializeToggle(entityJson["toggle"], *entityData.Toggle); }
    if (entityJson.contains("scrollView")) { entityData.ScrollView = std::make_unique<ScrollViewComponent>(); DeserializeScrollView(entityJson["scrollView"], *entityData.ScrollView); }
    if (entityJson.contains("layoutGroup")) { entityData.LayoutGroup = std::make_unique<LayoutGroupComponent>(); DeserializeLayoutGroup(entityJson["layoutGroup"], *entityData.LayoutGroup); }
    if (entityJson.contains("inputField")) { entityData.InputField = std::make_unique<InputFieldComponent>(); DeserializeInputField(entityJson["inputField"], *entityData.InputField); }
    if (entityJson.contains("dropdown")) { entityData.Dropdown = std::make_unique<DropdownComponent>(); DeserializeDropdown(entityJson["dropdown"], *entityData.Dropdown); }
    if (entityJson.contains("text"))   { entityData.Text   = std::make_unique<TextRendererComponent>(); DeserializeText(entityJson["text"], *entityData.Text); }

    // Areas
    if (entityJson.contains("area")) {
        entityData.Area = std::make_unique<cm::physics::AreaComponent>();
        Serializer::DeserializeArea(entityJson["area"], *entityData.Area);
    }

    // Navigation components in prefabs
    if (entityJson.contains("navmesh")) {
        entityData.Navigation = std::make_unique<nav::NavMeshComponent>();
        DeserializeNavMesh(entityJson["navmesh"], *entityData.Navigation);
    }
    if (entityJson.contains("navagent")) {
        entityData.NavAgent = std::make_unique<nav::NavAgentComponent>();
        DeserializeNavAgent(entityJson["navagent"], *entityData.NavAgent);
    }
    if (entityJson.contains("navlink")) {
        entityData.NavLink = std::make_unique<nav::NavLinkComponent>();
        DeserializeNavLink(entityJson["navlink"], *entityData.NavLink);
    }
    if (entityJson.contains("portal")) {
        entityData.Portal = std::make_unique<PortalComponent>();
        DeserializePortal(entityJson["portal"], *entityData.Portal);
    }

    // IK authored blocks
    if (entityJson.contains("ik") && entityJson["ik"].is_array()) {
        entityData.IKs.clear();
        for (const auto& j : entityJson["ik"]) {
            if (!j.is_object()) continue; // Guard against non-object array elements
            cm::animation::ik::IKComponent c;
            c.Enabled = j.value("enabled", true);
            c.TargetEntity = j.value("target", (EntityID)0);
            c.PoleEntity = j.value("pole", (EntityID)0);
            c.Weight = j.value("weight", 1.0f);
            c.MaxIterations = j.value("maxIterations", 12.0f);
            c.Tolerance = j.value("tolerance", 0.001f);
            c.Damping = j.value("damping", 0.2f);
            c.UseTwoBone = j.value("useTwoBone", true);
            c.LockAxisX = j.value("lockAxisX", false);
            c.LockAxisY = j.value("lockAxisY", false);
            c.LockAxisZ = j.value("lockAxisZ", false);
            c.Visualize = j.value("visualize", false);
            std::vector<cm::animation::ik::BoneId> chainBones;
            if (j.contains("chain") && j["chain"].is_array()) {
                for (auto& b : j["chain"]) chainBones.push_back((cm::animation::ik::BoneId)b.get<int>());
            }
            c.SetChain(chainBones);
            c.ChainRootHint = j.value("rootBone", c.ChainRootHint);
            c.ChainEffectorHint = j.value("tipBone", c.ChainEffectorHint);
            if (j.contains("constraints") && j["constraints"].is_array()) {
                for (auto& cj : j["constraints"]) {
                    if (!cj.is_object()) continue; // Guard against non-object elements
                    cm::animation::ik::IKComponent::Constraint cc;
                    cc.useHinge=cj.value("useHinge",false); cc.useTwist=cj.value("useTwist",false);
                    cc.hingeMinDeg=cj.value("hingeMinDeg",0.0f); cc.hingeMaxDeg=cj.value("hingeMaxDeg",0.0f);
                    cc.twistMinDeg=cj.value("twistMinDeg",0.0f); cc.twistMaxDeg=cj.value("twistMaxDeg",0.0f);
                    c.Constraints.push_back(cc);
                }
            }
            entityData.IKs.push_back(std::move(c));
        }
    }

    // LookAt constraint blocks
    if (entityJson.contains("lookat") && entityJson["lookat"].is_array()) {
        entityData.LookAtConstraints.clear();
        for (const auto& j : entityJson["lookat"]) {
            if (!j.is_object()) continue;
            cm::animation::lookat::LookAtConstraintComponent lac;
            lac.Enabled = j.value("enabled", true);
            lac.Mode = static_cast<cm::animation::lookat::LookAtMode>(j.value("mode", (uint8_t)cm::animation::lookat::LookAtMode::LookAtPosition));
            lac.TargetEntity = j.value("target", (EntityID)0);
            lac.Weight = j.value("weight", 1.0f);
            lac.Axes = static_cast<cm::animation::lookat::AxisMask>(j.value("axes", (uint8_t)cm::animation::lookat::AxisMask::YawPitch));
            lac.Space = static_cast<cm::animation::lookat::LookAtSpace>(j.value("space", (uint8_t)cm::animation::lookat::LookAtSpace::Component));
            lac.Distribution = static_cast<cm::animation::lookat::DistributionMode>(j.value("distribution", (uint8_t)cm::animation::lookat::DistributionMode::Linear));
            lac.MaxYawDeg = j.value("maxYawDeg", 70.0f);
            lac.MaxPitchDeg = j.value("maxPitchDeg", 45.0f);
            lac.MaxRollDeg = j.value("maxRollDeg", 15.0f);
            lac.SmoothingSpeed = j.value("smoothingSpeed", 10.0f);
            lac.Visualize = j.value("visualize", false);
            if (j.contains("boneChain") && j["boneChain"].is_array()) {
                for (auto& b : j["boneChain"]) lac.BoneChain.push_back((cm::animation::lookat::BoneId)b.get<int>());
            }
            if (j.contains("boneWeights") && j["boneWeights"].is_array()) {
                for (auto& w : j["boneWeights"]) lac.BoneWeights.push_back(w.get<float>());
            }
            entityData.LookAtConstraints.push_back(std::move(lac));
        }
    }

    // Preserve unknown fields into Extra (parity with entity deserialization)
    try {
        static const std::unordered_set<std::string> kKnown = {
            "name","layer","tag","transform","mesh","light","collider","rigidbody","staticbody","camera",
            "terrain","emitter","canvas","panel","button","scripts","animator","asset",
            "skeleton","skinning","ik","lookat","navmesh","navagent","navlink","portal","text","characterController","area",
            "blendShapeWeights","unifiedMorphWeights","tintController"
        };
        entityData.Extra = nlohmann::json::object();
        for (auto it = entityJson.begin(); it != entityJson.end(); ++it) {
            if (kKnown.find(it.key()) == kKnown.end()) {
                entityData.Extra[it.key()] = it.value();
            }
        }
    } catch(...) {}

    return true;
}

bool Serializer::SavePrefabToFile(const EntityData& entityData, Scene& scene, const std::string& filepath) {
    try {
        json prefabData = SerializePrefab(entityData, scene);
        if (entityData.Terrain) {
            TerrainComponent& terrain = *const_cast<TerrainComponent*>(entityData.Terrain.get());
            Terrain::SaveTerrainAsset(terrain, true);
        }
        
        // Ensure directory exists
        fs::path path(filepath);
        fs::create_directories(path.parent_path());
        
        std::ofstream file(filepath);
        if (!file.is_open()) {
            std::cerr << "[Serializer] Failed to open file for writing: " << filepath << std::endl;
            return false;
        }
        
        file << prefabData.dump(4); // Pretty print with 4 spaces
        file.close();
        
        std::cout << "[Serializer] Prefab saved to: " << filepath << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "[Serializer] Error saving prefab: " << e.what() << std::endl;
        return false;
    }
}

bool Serializer::LoadPrefabFromFile(const std::string& filepath, EntityData& entityData, Scene& scene) {
    try {
        if (Assets::ShouldLoadBinary() && !Assets::AllowSourceFallback()) {
            std::cerr << "[Serializer] Missing binary prefab (binary-only mode): " << filepath << std::endl;
            return false;
        }
        json prefabData;
        std::string text;
        if (FileSystem::Instance().ReadTextFile(filepath, text)) {
            prefabData = json::parse(text);
        } else {
            std::vector<uint8_t> bytes;
            if (FileSystem::Instance().ReadFile(filepath, bytes)) {
                prefabData = json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            } else {
                std::cerr << "[Serializer] Prefab file does not exist or cannot be read: " << filepath << std::endl;
                return false;
            }
        }
        
        bool success = DeserializePrefab(prefabData, entityData, scene);
        if (success) {
            std::cout << "[Serializer] Prefab loaded from: " << filepath << std::endl;
        }
        return success;
    }
    catch (const std::exception& e) {
        std::cerr << "[Serializer] Error loading prefab: " << e.what() << std::endl;
        return false;
    }
}

// New: serialize an entity and all its descendants as a prefab subtree
json Serializer::SerializePrefabSubtree(EntityID rootId, Scene& scene) {
    json prefab;
    prefab["version"] = "2.0";
    prefab["type"] = "prefab";
    prefab["entities"] = json::array();
    if (!scene.GetEntityData(rootId)) return prefab;

    // Collect subtree ids in DFS order
    std::vector<EntityID> order;
    std::function<void(EntityID)> dfs = [&](EntityID id){
        order.push_back(id);
        if (auto* d = scene.GetEntityData(id)) {
            for (EntityID c : d->Children) dfs(c);
        }
    };
    dfs(rootId);

    // Identify imported model roots within the subtree and collect per-node overrides; skip their descendants
    std::unordered_set<EntityID> skip;
    std::unordered_map<EntityID, nlohmann::json> rootOverrides;
    std::unordered_set<EntityID> modelRoots;        // model roots at or above emission; nested roots stay skipped
    auto computeNodePath = [&](EntityID root, EntityID node) -> std::string {
        std::vector<std::string> parts; EntityID cur = node;
        while (cur != -1) {
            auto* d = scene.GetEntityData(cur); if (!d) break;
            parts.push_back(d->Name);
            if (cur == root) break;
            cur = d->Parent;
        }
        std::reverse(parts.begin(), parts.end());
        if (!parts.empty()) parts.erase(parts.begin()); // make path relative to model root
        std::string s; for (size_t i=0;i<parts.size();++i){ s += parts[i]; if (i+1<parts.size()) s += "/"; }
        return s;
    };
    for (EntityID id : order) {
        std::string path; ClaymoreGUID g{};
        if (IsImportedModelRoot(scene, id, path, g)) {
            // Determine if this root is nested under a previously-identified model root
            bool isNested = false;
            {
                EntityID cur = id;
                size_t guard = 0;
                while (cur != (EntityID)-1 && cur != (EntityID)0 && guard++ < 100000) {
                    auto* d = scene.GetEntityData(cur); if (!d) break;
                    if (modelRoots.count(cur)) { isNested = true; break; }
                    cur = d->Parent;
                }
            }
            // Collect overrides for children under this model root
            rootOverrides[id] = nlohmann::json::array();
            
            // Build EntityID -> path map for TintController target path conversion
            // IMPORTANT: Paths are relative to model root, NOT including the model root name
            // This matches how ApplyModelDelta resolves paths
            std::unordered_map<EntityID, std::string> entityToPath;
            entityToPath[id] = "";  // Model root is empty path
            std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID nodeId, const std::string& parentPath) {
                auto* data = scene.GetEntityData(nodeId);
                if (!data) return;
                std::string nodePath = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                entityToPath[nodeId] = nodePath;
                for (EntityID child : data->Children) buildPathMap(child, nodePath);
            };
            // Start from children of model root with empty parent path
            auto* modelRootData = scene.GetEntityData(id);
            if (modelRootData) {
                for (EntityID child : modelRootData->Children) {
                    buildPathMap(child, "");
                }
            }
            
            std::function<void(EntityID)> walk = [&](EntityID cur){
                auto* d = scene.GetEntityData(cur); if (!d) return;
                for (EntityID c : d->Children) {
                    // Check if this child is itself a nested model root BEFORE adding to skip set
                    // Nested models should be serialized separately with their own asset record, not as overrides
                    auto* cd = scene.GetEntityData(c);
                    bool isNestedModelRoot = cd && (cd->ModelAssetGuid.high != 0 || cd->ModelAssetGuid.low != 0);
                    if (isNestedModelRoot) {
                        // Don't add to skip or rootOverrides - this nested model will be serialized
                        // as its own entity with its own asset record. Skip recursion too.
                        continue;
                    }
                    
                    skip.insert(c);
                    
                    nlohmann::json childJ = SerializeEntity(c, scene);
                    childJ["_modelNodePath"] = computeNodePath(id, c);
                    
                    // Convert TintController target EntityIDs to paths for child entities
                    if (cd && cd->TintController && childJ.contains("tintController") && childJ["tintController"].is_object()) {
                        if (childJ["tintController"].contains("targets") && childJ["tintController"]["targets"].is_array()) {
                            for (auto& targetJ : childJ["tintController"]["targets"]) {
                                if (targetJ.contains("entity")) {
                                    EntityID targetId = static_cast<EntityID>(targetJ["entity"].get<int64_t>());
                                    auto pathIt = entityToPath.find(targetId);
                                    if (pathIt != entityToPath.end()) {
                                        targetJ["entityPath"] = pathIt->second;
                                    }
                                }
                            }
                        }
                    }
                    
                    // Keep name; drop relational/id-only fields
                    childJ.erase("id"); childJ.erase("parent"); childJ.erase("children"); childJ.erase("asset");
                    if (!childJ.empty()) rootOverrides[id].push_back(std::move(childJ));
                    
                    // Recurse into non-nested-model children
                    walk(c);
                }
            };
            walk(id);
            // Only emit this root if it is not nested under another model root
            if (!isNested) {
                modelRoots.insert(id);
                skip.erase(id);
            }
        }
    }

    // Build emission list excluding skipped nodes
    std::vector<EntityID> emit;
    emit.reserve(order.size());
    for (EntityID id : order) {
        if (skip.find(id) == skip.end()) emit.push_back(id);
    }
    std::unordered_map<EntityID, int> idToEmitIndex;
    for (int i = 0; i < (int)emit.size(); ++i) idToEmitIndex[emit[i]] = i;

    // Emit compact subtree
    for (int i = 0; i < (int)emit.size(); ++i) {
        EntityID eid = emit[i];
        json e = SerializeEntity(eid, scene);
        e.erase("id");
        e.erase("guid");
        // Parent index among emitted nodes only
        int parentIndex = -1;
        if (auto* d = scene.GetEntityData(eid)) {
            if (d->Parent != (EntityID)-1) {
                auto it = idToEmitIndex.find(d->Parent);
                if (it != idToEmitIndex.end()) parentIndex = it->second;
            }
        }
        e["parentIndex"] = parentIndex;
        e.erase("children");
        // Attach asset compact record and collected overrides if this was a model root
        std::string modelPath; ClaymoreGUID guid{};
        if (IsImportedModelRoot(scene, eid, modelPath, guid)) {
            // Rebuild entry as a compact asset stub to avoid persisting model internals twice
            json minimal;
            // Preserve authored name and top-level authored data: transform, scripts, animator, tintController, unifiedMorphWeights
            if (auto* d = scene.GetEntityData(eid)) minimal["name"] = d->Name;
            if (e.contains("transform")) minimal["transform"] = e["transform"];
            if (e.contains("scripts")) minimal["scripts"] = e["scripts"];
            if (e.contains("animator")) minimal["animator"] = e["animator"];
            
            // TintController: Convert EntityIDs to relative paths for scene reload compatibility
            // IMPORTANT: Paths are relative to model root, NOT including the model root name
            // This matches how ApplyModelDelta resolves paths
            if (e.contains("tintController") && e["tintController"].is_object()) {
                json tintJson = e["tintController"];
                // Build EntityID -> relative path map for this model subtree
                std::unordered_map<EntityID, std::string> entityToPath;
                entityToPath[eid] = "";  // Root is empty path
                std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID nodeId, const std::string& parentPath) {
                    auto* data = scene.GetEntityData(nodeId);
                    if (!data) return;
                    std::string path = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                    entityToPath[nodeId] = path;
                    for (EntityID c : data->Children) buildPathMap(c, path);
                };
                // Start from children of model root with empty parent path
                auto* modelRootData = scene.GetEntityData(eid);
                if (modelRootData) {
                    for (EntityID child : modelRootData->Children) {
                        buildPathMap(child, "");
                    }
                }
                
                // Update targets with paths
                if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
                    for (auto& target : tintJson["targets"]) {
                        if (target.contains("entity")) {
                            EntityID targetId = static_cast<EntityID>(target["entity"].get<int64_t>());
                            auto pathIt = entityToPath.find(targetId);
                            if (pathIt != entityToPath.end()) {
                                target["entityPath"] = pathIt->second;
                            }
                        }
                    }
                }
                minimal["tintController"] = std::move(tintJson);
            }
            
            if (e.contains("unifiedMorphWeights")) minimal["unifiedMorphWeights"] = e["unifiedMorphWeights"];
            if (e.contains("deletedModelNodes")) minimal["deletedModelNodes"] = e["deletedModelNodes"];
            json asset;
            asset["type"] = "model";
            std::string v = modelPath; for(char& c: v) if (c=='\\') c='/';
            auto pos = v.find("assets/"); if (pos != std::string::npos) v = v.substr(pos);
            asset["path"] = v;
            asset["guid"] = guid.ToString();
            minimal["asset"] = std::move(asset);
            auto it = rootOverrides.find(eid);
            minimal["children"] = (it != rootOverrides.end()) ? it->second : nlohmann::json::array();
            e = std::move(minimal);
        }
        prefab["entities"].push_back(std::move(e));
    }
    return prefab;
}

bool Serializer::SavePrefabSubtreeToFile(Scene& scene, EntityID rootId, const std::string& filepath) {
    try {
        json j = SerializePrefabSubtree(rootId, scene);
        fs::path p(filepath);
        fs::create_directories(p.parent_path());
        std::ofstream out(filepath);
        if (!out) return false;
        out << j.dump(4);
        out.close();
        std::cout << "[Serializer] Prefab subtree saved to: " << filepath << std::endl;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Serializer] Error saving prefab subtree: " << e.what() << std::endl;
        return false;
    }
}

EntityID Serializer::LoadPrefabToScene(const std::string& filepath, Scene& scene) {
    try {
        if (Assets::ShouldLoadBinary() && !Assets::AllowSourceFallback()) {
            std::cerr << "[Serializer] Missing binary prefab (binary-only mode): " << filepath << std::endl;
            return (EntityID)-1;
        }
        json data;
        std::string text;
        if (FileSystem::Instance().ReadTextFile(filepath, text)) {
            data = json::parse(text);
        } else {
            std::vector<uint8_t> bytes;
            if (FileSystem::Instance().ReadFile(filepath, bytes)) {
                data = json::parse(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            } else {
                std::cerr << "[Serializer] Prefab file does not exist or cannot be read: " << filepath << std::endl;
                return (EntityID)-1;
            }
        }
        // Support both legacy and subtree formats
        if (data.contains("entities") && data["entities"].is_array()) {
            // Subtree format: instantiate compact asset nodes (models) and create pure-serialized nodes; then apply overrides.
            const auto& ents = data["entities"];
            std::vector<EntityID> idxToNew(ents.size(), (EntityID)-1);
            std::unordered_set<EntityID> opaqueRoots; // model-instantiated roots that carry their own hierarchy

            // Pre-scan: mark which indices are model asset nodes and utility to test ancestry by parentIndex
            std::vector<uint8_t> isModelAsset(ents.size(), 0);
            struct ModelRootInfo { int parentIndex; std::string name; };
            std::vector<ModelRootInfo> modelRoots; modelRoots.reserve(ents.size());
            for (size_t i = 0; i < ents.size(); ++i) {
                const json& je = ents[i];
                if (je.contains("asset") && je["asset"].is_object()) {
                    const auto& a = je["asset"]; std::string type = a.value("type", "");
                    if (type == "model") {
                        isModelAsset[i] = 1;
                        int pidx = je.value("parentIndex", -1);
                        std::string nm = je.value("name", std::string());
                        modelRoots.push_back({ pidx, nm });
                    }
                }
            }
            auto normalizeName = [](const std::string& name) -> std::string {
                size_t us = name.find_last_of('_');
                if (us == std::string::npos) return name;
                bool digits = true; for (size_t i = us + 1; i < name.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; } }
                return digits ? name.substr(0, us) : name;
            };
            auto isDescendantOfModelAssetIdx = [&](int idx) -> bool {
                if (idx < 0 || idx >= (int)ents.size()) return false;
                int cur = idx;
                int guard = 0;
                while (cur >= 0 && cur < (int)ents.size() && guard++ < 100000) {
                    const json& je = ents[(size_t)cur];
                    if (!je.contains("parentIndex")) break;
                    int pidx = je["parentIndex"].get<int>();
                    if (pidx < 0) break;
                    if (pidx >= 0 && pidx < (int)ents.size() && isModelAsset[(size_t)pidx]) return true;
                    cur = pidx;
                }
                return false;
            };
            auto looksModelNode = [&](const json& j) -> bool {
                auto has = [&](const char* k){ return j.contains(k); };
                // Model nodes have mesh, skeleton, skinning, or blendShapes
                bool hasModelComp = has("mesh") || has("skeleton") || has("skinning") || has("blendShapes");
                bool hasUserComp = has("camera") || has("light") || has("collider") || has("rigidbody") || has("staticbody") || has("softbody") ||
                                   has("emitter") || has("canvas") || has("panel") || has("button") || has("text") || has("scripts") || has("terrain") ||
                                   has("navmesh") || has("navagent") || has("navlink") || has("portal") || has("area") || has("grassDeformer") || has("characterController") ||
                                   (has("animator") && !j["animator"].is_null());
                return hasModelComp && !hasUserComp;
            };

            // First pass: create entities or instantiate models
            for (size_t i = 0; i < ents.size(); ++i) {
                const json& je = ents[i];
                // Model asset node?
                if (je.contains("asset") && je["asset"].is_object()) {
                    const auto& a = je["asset"];
                    std::string type = a.value("type", "");
                    if (type == "model") {
                        // Resolve path relative to project
                        std::string p = a.value("path", "");
                        std::string resolved = p;
                        if (!resolved.empty() && !fs::exists(resolved)) resolved = (Project::GetProjectDirectory() / p).string();
                        for (char& c : resolved) if (c=='\\') c = '/';
                        // Register GUID mapping hint if present
                        try {
                            std::string gstr = a.value("guid", "");
                            if (!gstr.empty()) {
                                ClaymoreGUID g = ClaymoreGUID::FromString(gstr);
                                if (!(g.high == 0 && g.low == 0)) {
                                    std::string v = p; for (char& ch : v) if (ch=='\\') ch = '/';
                                    AssetLibrary::Instance().RegisterAsset(AssetReference(g, 0, (int)AssetType::Mesh), AssetType::Mesh, v, v);
                                    if (!resolved.empty()) AssetLibrary::Instance().RegisterPathAlias(g, resolved);
                                }
                            }
                        } catch(...) {}
                        // Determine spawn position
                        glm::vec3 pos(0.0f);
                        if (je.contains("transform")) { auto t = je["transform"]; if (t.contains("position")) pos = DeserializeVec3(t["position"]); }
                        // Prefer fast path via .meta next to model
                        std::string metaTry = resolved;
                        std::string ext = fs::path(resolved).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext != ".meta") { fs::path rp(resolved); fs::path mp = rp.parent_path() / (rp.stem().string() + ".meta"); if (fs::exists(mp)) metaTry = mp.string(); }
                        EntityID nid = (EntityID)-1;
                        if (!metaTry.empty() && fs::path(metaTry).extension() == ".meta") {
                            // Deterministic path for bulk array deserialization too
                            nid = scene.InstantiateModelFast(metaTry, pos, /*synchronous*/ true);
                            if (nid == (EntityID)0 || nid == (EntityID)-1) nid = scene.InstantiateModel(resolved, pos);
                        } else {
                            nid = scene.InstantiateModel(resolved, pos);
                        }
                        if (nid != (EntityID)-1 && nid != (EntityID)0) {
                            idxToNew[i] = nid;
                            opaqueRoots.insert(nid);
                            // Apply transform, scripts, animator on the root
                            if (auto* ed = scene.GetEntityData(nid)) {
                                if (je.contains("name")) { ed->Name = je["name"].get<std::string>(); }
                                if (je.contains("transform")) DeserializeTransform(je["transform"], ed->Transform);
                                if (je.contains("scripts")) DeserializeScripts(je["scripts"], ed->Scripts, scene);
                                if (je.contains("animator")) { if (!ed->AnimationPlayer) ed->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); Serializer::DeserializeAnimator(je["animator"], *ed->AnimationPlayer); }
                                
                                // Process deleted model nodes: restore the deletion list and remove those nodes
                                if (je.contains("deletedModelNodes") && je["deletedModelNodes"].is_array()) {
                                    ed->DeletedModelNodes.clear();
                                    for (const auto& path : je["deletedModelNodes"]) {
                                        if (path.is_string()) {
                                            std::string p = path.get<std::string>();
                                            if (!p.empty()) {
                                                ed->DeletedModelNodes.push_back(std::move(p));
                                            }
                                        }
                                    }
                                    // Build path-to-EntityID map for deletion (paths are RELATIVE to model root, not including root name)
                                    std::unordered_map<std::string, EntityID> delPathToEntity;
                                    std::function<void(EntityID, const std::string&)> buildDelPathMap = [&](EntityID id, const std::string& parentPath) {
                                        auto* data = scene.GetEntityData(id);
                                        if (!data) return;
                                        std::string nodePath = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                                        delPathToEntity[nodePath] = id;
                                        for (EntityID c : data->Children) buildDelPathMap(c, nodePath);
                                    };
                                    // Start from children of root (paths exclude root name)
                                    for (EntityID rootChild : ed->Children) {
                                        buildDelPathMap(rootChild, "");
                                    }
                                    // Delete nodes in the deletion list
                                    for (const std::string& delPath : ed->DeletedModelNodes) {
                                        if (delPath.empty()) continue;
                                        auto it = delPathToEntity.find(delPath);
                                        if (it != delPathToEntity.end()) {
                                            scene.RemoveEntity(it->second);
                                            std::cout << "[PrefabLoad] Removed deleted model node: " << delPath << std::endl;
                                        } else {
                                            std::cout << "[PrefabLoad] WARNING: Could not find deleted node path: " << delPath << std::endl;
                                        }
                                    }
                                }
                                
                                // Apply tint controller with path-to-EntityID resolution
                                if (je.contains("tintController") && je["tintController"].is_object()) {
                                    if (!ed->TintController) ed->TintController = std::make_unique<TintMaskController>();
                                    DeserializeTintController(je["tintController"], *ed->TintController);
                                    
                                    // Resolve entityPath to new EntityIDs in model subtree
                                    std::unordered_map<std::string, EntityID> pathToEntity;
                                    std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID id, const std::string& parentPath) {
                                        auto* data = scene.GetEntityData(id);
                                        if (!data) return;
                                        std::string path = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                                        pathToEntity[path] = id;
                                        for (EntityID c : data->Children) buildPathMap(c, path);
                                    };
                                    buildPathMap(nid, "");  // nid is the new model root
                                    pathToEntity[""] = nid;  // Empty path = root
                                    
                                    // Resolve TintTarget paths to new EntityIDs
                                    const auto& tintJson = je["tintController"];
                                    if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
                                        size_t idx = 0;
                                        for (const auto& target : tintJson["targets"]) {
                                            if (idx < ed->TintController->Targets.size() && target.contains("entityPath")) {
                                                std::string path = target["entityPath"].get<std::string>();
                                                auto pathIt = pathToEntity.find(path);
                                                if (pathIt != pathToEntity.end()) {
                                                    ed->TintController->Targets[idx].TargetEntity = pathIt->second;
                                                } else {
                                                    // Path not found - mark as invalid
                                                    ed->TintController->Targets[idx].TargetEntity = (EntityID)-1;
                                                }
                                            }
                                            idx++;
                                        }
                                    }
                                    ed->TintController->NeedsRefresh = true;
                                }
                                // Apply unified morph weights - find skeleton root
                                if (je.contains("unifiedMorphWeights") && je["unifiedMorphWeights"].is_array()) {
                                    std::function<EntityID(EntityID)> findUnifiedMorphEntity = [&](EntityID id) -> EntityID {
                                        if (auto* d = scene.GetEntityData(id)) {
                                            if (d->UnifiedMorph) return id;
                                            for (EntityID c : d->Children) { EntityID found = findUnifiedMorphEntity(c); if (found != (EntityID)-1) return found; }
                                        }
                                        return (EntityID)-1;
                                    };
                                    EntityID morphEntity = findUnifiedMorphEntity(nid);
                                    if (morphEntity != (EntityID)-1) {
                                        auto* morphData = scene.GetEntityData(morphEntity);
                                        if (morphData && morphData->UnifiedMorph) {
                                            for (const auto& entry : je["unifiedMorphWeights"]) {
                                                if (!entry.is_object() || !entry.contains("name") || !entry.contains("weight")) continue;
                                                std::string name = entry["name"].get<std::string>();
                                                float weight = entry["weight"].get<float>();
                                                for (size_t mi = 0; mi < morphData->UnifiedMorph->Names.size(); ++mi) {
                                                    if (morphData->UnifiedMorph->Names[mi] == name) {
                                                        morphData->UnifiedMorph->Weights[mi] = weight;
                                                        break;
                                                    }
                                                }
                                            }
                                            // Propagate to child blend shapes
                                            for (EntityID meshId : morphData->UnifiedMorph->MemberMeshes) {
                                                auto* meshData = scene.GetEntityData(meshId);
                                                if (!meshData || !meshData->BlendShapes) continue;
                                                for (auto& shape : meshData->BlendShapes->Shapes) {
                                                    for (size_t mi = 0; mi < morphData->UnifiedMorph->Names.size(); ++mi) {
                                                        if (shape.Name == morphData->UnifiedMorph->Names[mi]) {
                                                            shape.Weight = morphData->UnifiedMorph->Weights[mi];
                                                            meshData->BlendShapes->Dirty = true;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        continue; // done with this entry
                    }
                    else if (type == "prefab") {
                        // Instantiate prefab as a single opaque root (like models) and mark as opaque
                        std::string p = a.value("path", "");
                        std::string resolved = p; for (char& c : resolved) if (c=='\\') c='/';
                        if (!resolved.empty() && !fs::exists(resolved)) resolved = (Project::GetProjectDirectory() / p).string();
                        for (char& c : resolved) if (c=='\\') c = '/';
                        EntityID nid = (EntityID)-1;
                        std::string ext = fs::path(resolved).extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        // .prefab and .json both map to authoring JSON instantiation
                        if (ext == ".prefab") nid = InstantiatePrefabFromPath(resolved, scene);
                        else if (ext == ".json") nid = InstantiatePrefabFromPath(resolved, scene);
                        if (nid != (EntityID)-1 && nid != (EntityID)0) {
                            idxToNew[i] = nid;
                            opaqueRoots.insert(nid);
                            if (auto* ed = scene.GetEntityData(nid)) {
                                if (je.contains("name")) { ed->Name = je["name"].get<std::string>(); }
                                if (je.contains("transform")) DeserializeTransform(je["transform"], ed->Transform);
                                if (je.contains("scripts")) DeserializeScripts(je["scripts"], ed->Scripts, scene);
                                if (je.contains("animator")) { if (!ed->AnimationPlayer) ed->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); Serializer::DeserializeAnimator(je["animator"], *ed->AnimationPlayer); }
                                try { std::string v = p; for (char& ch : v) if (ch=='\\') ch = '/'; ed->PrefabSource = v; } catch(...) {}
                                
                                // Restore PrefabInstanceComponent if serialized (simplified - just GUID mapping)
                                // Overrides are now stored in the scene's 'children' array
                                if (je.contains("prefabInstance")) {
                                    if (!ed->PrefabInstance) {
                                        ed->PrefabInstance = std::make_unique<PrefabInstanceComponent>();
                                    }
                                    DeserializePrefabInstance(je["prefabInstance"], *ed->PrefabInstance);
                                    // NOTE: Overrides are NOT applied here - they're in the 'children' array
                                    // and will be applied by the prefab children override loop below
                                }
                            }
                        }
                        continue; // handled compact prefab node
                    }
                }

                // Regular serialized node (skip all descendants under an instantiated model root; they will be handled via overrides)
                std::string name = je.value("name", "Entity");
                if (isDescendantOfModelAssetIdx((int)i)) {
                    continue;
                }
                // If a model asset node with the same parentIndex and same (normalized) name exists, skip this duplicate root entry
                {
                    int pidx = je.value("parentIndex", -1);
                    std::string norm = normalizeName(name);
                    bool dupOfModelRoot = false;
                    for (const auto& mr : modelRoots) {
                        if (mr.parentIndex == pidx && !mr.name.empty()) {
                            if (normalizeName(mr.name) == norm) { dupOfModelRoot = true; break; }
                        }
                    }
                    if (dupOfModelRoot) continue;
                }
                Entity e = scene.CreateEntityExact(name);
                EntityID nid = e.GetID();
                idxToNew[i] = nid;
                auto* d = scene.GetEntityData(nid);
                if (!d) continue;
                if (je.contains("layer")) d->Layer = je["layer"];
                if (je.contains("tag")) d->Tag = je["tag"];
                if (je.contains("visible")) d->Visible = je["visible"];
                if (je.contains("transform")) Serializer::DeserializeTransform(je["transform"], d->Transform);
                if (je.contains("mesh")) {
                    if (!d->Mesh) d->Mesh = std::make_unique<MeshComponent>();
                    // Prefer unified builder over legacy deserializer
                    ClaymoreGUID meshGuid{}; int fileId = 0; ClaymoreGUID skelGuid{};
                    try {
                        if (je["mesh"].contains("meshReference")) { AssetReference tmp; je["mesh"]["meshReference"].get_to(tmp); meshGuid = tmp.guid; fileId = tmp.fileID; }
                    } catch(...) {}
                    // Fallback for primitives/name-based meshes saved without meshReference
                    try {
                        if ((meshGuid.high == 0 && meshGuid.low == 0) && je["mesh"].contains("meshName")) {
                            std::string mname = je["mesh"]["meshName"].get<std::string>();
                            if (!mname.empty()) {
                                d->Mesh->MeshName = mname;
                                if (mname == "Cube" || mname == "DebugCube") d->Mesh->mesh = StandardMeshManager::Instance().GetCubeMesh();
                                else if (mname == "Sphere") d->Mesh->mesh = StandardMeshManager::Instance().GetSphereMesh();
                                else if (mname == "Plane") d->Mesh->mesh = StandardMeshManager::Instance().GetPlaneMesh();
                                else if (mname == "Capsule") d->Mesh->mesh = StandardMeshManager::Instance().GetCapsuleMesh();
                            }
                        }
                    } catch(...) {}
                    try { if (je.contains("skeleton") && je["skeleton"].contains("skeletonGuid")) je["skeleton"]["skeletonGuid"].get_to(skelGuid); } catch(...) {}
                    BuildModelParams bp{ meshGuid, fileId, skelGuid, nullptr, nid, &scene };
                    BuildResult br = BuildRendererFromAssets(bp);
                    if (!br.ok) {
                        std::cerr << "[Serializer] ERROR: Prefab node renderer build failed for entity '" << d->Name << "'" << std::endl;
                    }
                    // Apply mesh overrides (boundsPadding, renderOnTop, etc.) after building renderer
                    if (d->Mesh) {
                        nlohmann::json meshOverride = je["mesh"];
                        meshOverride.erase("meshReference");
                        meshOverride.erase("fileID");
                        DeserializeMesh(meshOverride, *d->Mesh, d->RenderOverrides);
                    }
                }
                if (je.contains("light")) { d->Light = std::make_unique<LightComponent>(); Serializer::DeserializeLight(je["light"], *d->Light); }
                if (je.contains("audioSource")) { d->AudioSource = std::make_unique<AudioSourceComponent>(); Serializer::DeserializeAudioSource(je["audioSource"], *d->AudioSource); }
                if (je.contains("audioListener")) { d->AudioListener = std::make_unique<AudioListenerComponent>(); Serializer::DeserializeAudioListener(je["audioListener"], *d->AudioListener); }
                if (je.contains("collider")) { d->Collider = std::make_unique<ColliderComponent>(); Serializer::DeserializeCollider(je["collider"], *d->Collider); }
                if (je.contains("rigidbody")) { d->RigidBody = std::make_unique<RigidBodyComponent>(); Serializer::DeserializeRigidBody(je["rigidbody"], *d->RigidBody); }
                if (je.contains("staticbody")) { d->StaticBody = std::make_unique<StaticBodyComponent>(); Serializer::DeserializeStaticBody(je["staticbody"], *d->StaticBody); }
                if (je.contains("softbody")) { d->Softbody = std::make_unique<SoftbodyComponent>(); Serializer::DeserializeSoftbody(je["softbody"], *d->Softbody); }
                if (je.contains("characterController")) { d->CharacterController = std::make_unique<CharacterControllerComponent>(); Serializer::DeserializeCharacterController(je["characterController"], *d->CharacterController); }
                if (je.contains("camera")) { d->Camera = std::make_unique<CameraComponent>(); Serializer::DeserializeCamera(je["camera"], *d->Camera); }
                if (je.contains("terrain")) { d->Terrain = std::make_unique<TerrainComponent>(); Serializer::DeserializeTerrain(je["terrain"], *d->Terrain); }
                if (je.contains("resourceLayers")) { d->ResourceLayers = std::make_unique<cm::resourcelayer::ResourceLayerComponent>(); Serializer::DeserializeResourceLayers(je["resourceLayers"], *d->ResourceLayers); }
                if (je.contains("instancer")) { d->Instancer = std::make_unique<cm::instancer::InstancerComponent>(); Serializer::DeserializeInstancer(je["instancer"], *d->Instancer); }
                if (je.contains("emitter")) { d->Emitter = std::make_unique<ParticleEmitterComponent>();  Serializer::DeserializeParticleEmitter(je["emitter"], *d->Emitter); }
                if (je.contains("canvas")) { d->Canvas = std::make_unique<CanvasComponent>();  Serializer::DeserializeCanvas(je["canvas"], *d->Canvas); }
                if (je.contains("panel")) { d->Panel = std::make_unique<PanelComponent>();  Serializer::DeserializePanel(je["panel"], *d->Panel); }
                if (je.contains("button")) { d->Button = std::make_unique<ButtonComponent>(); Serializer::DeserializeButton(je["button"], *d->Button); }
                if (je.contains("text")) { d->Text = std::make_unique<TextRendererComponent>(); Serializer::DeserializeText(je["text"], *d->Text); }
                if (je.contains("navmesh")) { d->Navigation = std::make_unique<nav::NavMeshComponent>(); Serializer::DeserializeNavMesh(je["navmesh"], *d->Navigation); }
                if (je.contains("navagent")) { d->NavAgent = std::make_unique<nav::NavAgentComponent>(); Serializer::DeserializeNavAgent(je["navagent"], *d->NavAgent); }
                if (je.contains("navlink")) { d->NavLink = std::make_unique<nav::NavLinkComponent>(); Serializer::DeserializeNavLink(je["navlink"], *d->NavLink); }
                if (je.contains("portal")) { d->Portal = std::make_unique<PortalComponent>(); Serializer::DeserializePortal(je["portal"], *d->Portal); }
                if (je.contains("area")) { d->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(je["area"], *d->Area); }
                if (je.contains("grassDeformer")) { d->GrassDeformer = std::make_unique<GrassDeformerComponent>(); Serializer::DeserializeGrassDeformer(je["grassDeformer"], *d->GrassDeformer); }
                if (je.contains("renderOverrides")) { d->RenderOverrides = std::make_unique<RenderOverridesComponent>(); Serializer::DeserializeRenderOverrides(je["renderOverrides"], *d->RenderOverrides); }
                if (je.contains("tintController")) { d->TintController = std::make_unique<TintMaskController>(); Serializer::DeserializeTintController(je["tintController"], *d->TintController); }
                if (je.contains("scripts")) { Serializer::DeserializeScripts(je["scripts"], d->Scripts, scene); }
                if (je.contains("animator")) { if (!d->AnimationPlayer) d->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>();  Serializer::DeserializeAnimator(je["animator"], *d->AnimationPlayer); }
                if (je.contains("skeleton")) { if (!d->Skeleton) d->Skeleton = std::make_unique<SkeletonComponent>();  Serializer::DeserializeSkeleton(je["skeleton"], *d->Skeleton); }
                if (je.contains("skinning")) { if (!d->Skinning) d->Skinning = std::make_unique<SkinningComponent>();  Serializer::DeserializeSkinning(je["skinning"], *d->Skinning); TryResolveSkinningSkeletonRootRef(je["skinning"], *d->Skinning, scene); }
            }

            // Second pass: parent fixup (skip opaque roots)
            for (size_t i = 0; i < ents.size(); ++i) {
                const json& je = ents[i];
                EntityID nid = idxToNew[i];
                if (nid == (EntityID)-1) continue;
                if (je.contains("parentIndex")) {
                    int pidx = je["parentIndex"].get<int>();
                    if (pidx >= 0 && pidx < (int)idxToNew.size()) {
                        EntityID pid = idxToNew[pidx];
                        // Allow parenting opaque roots under their intended parent so they don't stay as siblings
                        if (pid != (EntityID)-1) {
                            scene.SetParent(nid, pid);
                        }
                    }
                }
            }

            // Apply per-node overrides under compact model roots
            auto resolveByPath = [&](EntityID rootNew, const std::string& path) -> EntityID {
                EntityID target = rootNew;
                if (path.empty()) return target;
                std::stringstream ss(path); std::string part;
                auto normalize = [](const std::string& name) -> std::string {
                    size_t us = name.find_last_of('_');
                    if (us == std::string::npos) return name;
                    bool digits = true; for (size_t i = us + 1; i < name.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; } }
                    return digits ? name.substr(0, us) : name;
                };
                while (std::getline(ss, part, '/')) {
                    auto* d = scene.GetEntityData(target); if (!d) return (EntityID)-1;
                    const std::string partNorm = normalize(part);
                    auto lower = [](std::string s){ for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; };

                    // Accept paths that either start at the current root's children or redundantly
                    // include the current root's own name after a model re-export/root reshuffle.
                    if (lower(d->Name) == lower(part) ||
                        lower(normalize(d->Name)) == lower(partNorm) ||
                        d->Name == part ||
                        normalize(d->Name) == partNorm) {
                        continue;
                    }

                    EntityID next = (EntityID)-1;
                    for (EntityID c : d->Children) {
                        auto* cd = scene.GetEntityData(c);
                        if (!cd) continue;
                        const std::string childName = cd->Name;
                        if (childName == part ||
                            normalize(childName) == partNorm ||
                            lower(childName) == lower(part) ||
                            lower(normalize(childName)) == lower(partNorm)) {
                            next = c;
                            break;
                        }
                    }
                    if (next == (EntityID)-1) return (EntityID)-1; target = next;
                }
                return target;
            };
                auto findByMeshFileId = [&](EntityID rootNew, int fileID) -> EntityID {
                    std::function<EntityID(EntityID)> dfs = [&](EntityID id)->EntityID{
                        auto* d = scene.GetEntityData(id); if (!d) return (EntityID)-1;
                        // Must have valid mesh data (not just MeshComponent with matching fileID)
                        // Otherwise we could match a node whose mesh failed to load
                    if (d->Mesh && d->Mesh->mesh && d->Mesh->meshReference.fileID == fileID) return id;
                    for (EntityID c : d->Children) { EntityID r = dfs(c); if (r != (EntityID)-1) return r; }
                    return (EntityID)-1;
                };
                return dfs(rootNew);
            };

            for (size_t i = 0; i < ents.size(); ++i) {
                const json& je = ents[i];
                if (!(je.contains("asset") && je["asset"].is_object())) continue;
                const auto& a = je["asset"]; if (a.value("type", "") != std::string("model")) continue;
                EntityID rootNew = idxToNew[i]; if (rootNew == (EntityID)-1) continue;
                if (!je.contains("children") || !je["children"].is_array()) continue;
                // Sort overrides by depth so parents first
                struct OverrideItem { std::string relPath; const nlohmann::json* j; int depth; };
                std::vector<OverrideItem> items;
                for (const auto& childOverride : je["children"]) {
                    if (!childOverride.contains("_modelNodePath")) continue;
                    
                    // Skip child overrides that are nested model roots - they are handled separately
                    if (childOverride.contains("modelAssetGuid")) {
                        try {
                            ClaymoreGUID mag;
                            childOverride.at("modelAssetGuid").get_to(mag);
                            if (mag.high != 0 || mag.low != 0) continue;
                        } catch (...) {}
                    }
                    
                    std::string relPath = childOverride["_modelNodePath"].get<std::string>();
                    int depth = 0; for (char c : relPath) if (c=='/') ++depth;
                    items.push_back({std::move(relPath), &childOverride, depth});
                }
                std::stable_sort(items.begin(), items.end(), [](const OverrideItem& a, const OverrideItem& b){ return a.depth < b.depth; });

                for (const auto& it : items) {
                    const auto& childOverride = *it.j;
                    const std::string& relPath = it.relPath;
                    EntityID target = resolveByPath(rootNew, relPath);
                    bool matchedByPath = (target != (EntityID)-1);  // Track if we matched by exact path
                    // Fallback: try fileID match but validate name to avoid incorrect matches after model reimport
                    if (target == (EntityID)-1 && childOverride.contains("mesh") && childOverride["mesh"].contains("fileID")) {
                        int fid = childOverride["mesh"]["fileID"].get<int>();
                        EntityID candidateByFileId = findByMeshFileId(rootNew, fid);
                        if (candidateByFileId != (EntityID)-1 && childOverride.contains("_normalizedName")) {
                            // Only use fileID match if entity's normalized name matches
                            auto* candidateData = scene.GetEntityData(candidateByFileId);
                            if (candidateData) {
                                std::string expectedNormName = childOverride["_normalizedName"].get<std::string>();
                                std::string actualNormName = ModelNodeIdentity::NormalizeName(candidateData->Name);
                                if (actualNormName == expectedNormName) {
                                    target = candidateByFileId;
                                }
                            }
                        }
                    }
                    if (target == (EntityID)-1) {
                        // Create a node only for clearly user-authored additions. If a model-authored
                        // mesh child cannot be resolved after re-export, preserve the fresh imported
                        // hierarchy instead of recreating a stale fileID-mapped mesh.
                        auto has = [&](const char* k){ return childOverride.contains(k); };
                        bool looksUserAdded = has("camera") || has("light") || has("collider") || has("rigidbody") || has("staticbody") || has("softbody")
                            || has("emitter") || has("canvas") || has("panel") || has("button") || has("text") || has("scripts") || has("terrain")
                            || has("navmesh") || has("navagent") || has("navlink") || has("portal") || has("area") || has("grassDeformer") || has("characterController")
                            || (has("animator") && !childOverride["animator"].is_null());
                        bool looksModelNode = has("mesh") || has("skeleton") || has("skinning") || has("blendShapes");
                        if (!looksUserAdded && looksModelNode) {
                            continue;
                        }
                        if (looksUserAdded) {
                            // Determine parent by trimming the last path component
                            std::string parentPath;
                            if (!relPath.empty()) {
                                size_t slash = relPath.find_last_of('/');
                                if (slash != std::string::npos) parentPath = relPath.substr(0, slash);
                            }
                            EntityID parentEntity = parentPath.empty() ? rootNew : resolveByPath(rootNew, parentPath);
                            if (parentEntity != (EntityID)-1) {
                                std::string newName = childOverride.value("name", std::string("Entity"));
                                // Strip numeric suffix from model-internal children's names before creating entity
                                // Only strip if NOT user-added (user-added children keep their suffixes)
                                if (!childOverride.contains("_added") || !childOverride["_added"].get<bool>()) {
                                    size_t us = newName.find_last_of('_');
                                    if (us != std::string::npos && us > 0 && us < newName.size() - 1) {
                                        bool allDigits = true;
                                        for (size_t i = us + 1; i < newName.size(); ++i) {
                                            if (!std::isdigit(static_cast<unsigned char>(newName[i]))) {
                                                allDigits = false;
                                                break;
                                            }
                                        }
                                        if (allDigits) {
                                            newName = newName.substr(0, us);
                                        }
                                    }
                                }
                                Entity ne = scene.CreateEntityExact(newName);
                                target = ne.GetID();
                                scene.SetParent(target, parentEntity);
                            }
                        }
                    }
                    if (target == (EntityID)-1) continue;
                    auto* td = scene.GetEntityData(target); if (!td) continue;
                    
                    // Handle reparenting: if we matched by fallback (not by path), the node might need to be
                    // moved to the intended parent location. The _modelNodePath reflects where the user
                    // placed the node; if we matched by fileID, the node is at its original model location.
                    if (!matchedByPath) {
                        std::string intendedParentPath;
                        auto slashPos = relPath.find_last_of('/');
                        if (slashPos != std::string::npos) {
                            intendedParentPath = relPath.substr(0, slashPos);
                        }
                        EntityID intendedParent = intendedParentPath.empty() ? rootNew : resolveByPath(rootNew, intendedParentPath);
                        if (intendedParent != (EntityID)-1 && td->Parent != intendedParent) {
                            scene.SetParent(target, intendedParent);
                            try { std::cout << "[Reparent] Moved '" << td->Name << "' to intended parent path='" << intendedParentPath << "'" << std::endl; } catch(...) {}
                        }
                    }
                    if (childOverride.contains("transform")) { DeserializeTransform(childOverride["transform"], td->Transform); scene.MarkTransformDirty(target); }
                    if (childOverride.contains("mesh")) {
                        bool meshExisted = (td->Mesh != nullptr);  // Track if this is an existing model child
                        if (!td->Mesh) td->Mesh = std::make_unique<MeshComponent>();
                        ClaymoreGUID meshGuid{}; int fileId = 0; ClaymoreGUID skelGuid{};
                        try {
                            if (childOverride["mesh"].contains("meshReference")) { AssetReference tmp; childOverride["mesh"]["meshReference"].get_to(tmp); meshGuid = tmp.guid; fileId = tmp.fileID; }
                        } catch(...) {}
                        // Fallback for primitives/name-based meshes saved without meshReference
                        try {
                            if ((meshGuid.high == 0 && meshGuid.low == 0) && childOverride["mesh"].contains("meshName")) {
                                std::string mname = childOverride["mesh"]["meshName"].get<std::string>();
                                if (!mname.empty()) {
                                    td->Mesh->MeshName = mname;
                                    if (mname == "Cube" || mname == "DebugCube") td->Mesh->mesh = StandardMeshManager::Instance().GetCubeMesh();
                                    else if (mname == "Sphere") td->Mesh->mesh = StandardMeshManager::Instance().GetSphereMesh();
                                    else if (mname == "Plane") td->Mesh->mesh = StandardMeshManager::Instance().GetPlaneMesh();
                                    else if (mname == "Capsule") td->Mesh->mesh = StandardMeshManager::Instance().GetCapsuleMesh();
                                }
                            }
                        } catch(...) {}
                        try { if (childOverride.contains("skeleton") && childOverride["skeleton"].contains("skeletonGuid")) childOverride["skeleton"]["skeletonGuid"].get_to(skelGuid); } catch(...) {}
                        // Build renderer from assets if:
                        // 1. This is a new mesh (MeshComponent didn't exist), OR
                        // 2. Existing MeshComponent has null mesh pointer (mesh failed to load during instantiation)
                        bool needsBuild = !meshExisted || (td->Mesh && !td->Mesh->mesh);
                        if (needsBuild && (meshGuid.high != 0 || meshGuid.low != 0 || fileId != 0)) {
                            BuildModelParams bp{ meshGuid, fileId, skelGuid, nullptr, target, &scene };
                            BuildResult br = BuildRendererFromAssets(bp);
                            if (!br.ok) {
                                std::cerr << "[Serializer] ERROR: Override renderer build failed at path under model root." << std::endl;
                            }
                        }
                        // For existing model children with valid mesh, strip meshReference to preserve the correct mesh
                        // from the fresh instantiation (Blender reimport can change mesh indices)
                        nlohmann::json meshJson = childOverride["mesh"];
                        if (meshExisted && td->Mesh && td->Mesh->mesh) {
                            meshJson.erase("meshReference");
                            meshJson.erase("fileID");
                        }
                        DeserializeMesh(meshJson, *td->Mesh, td->RenderOverrides);
                    }
                    if (childOverride.contains("light")) { if (!td->Light) td->Light = std::make_unique<LightComponent>(); DeserializeLight(childOverride["light"], *td->Light); }
                    if (childOverride.contains("audioSource")) { if (!td->AudioSource) td->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(childOverride["audioSource"], *td->AudioSource); }
                    if (childOverride.contains("audioListener")) { if (!td->AudioListener) td->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(childOverride["audioListener"], *td->AudioListener); }
                    if (childOverride.contains("collider")) { if (!td->Collider) td->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(childOverride["collider"], *td->Collider); }
                    if (childOverride.contains("rigidbody")) { if (!td->RigidBody) td->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(childOverride["rigidbody"], *td->RigidBody); }
                    if (childOverride.contains("staticbody")) { if (!td->StaticBody) td->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(childOverride["staticbody"], *td->StaticBody); }
                    if (childOverride.contains("softbody")) { if (!td->Softbody) td->Softbody = std::make_unique<SoftbodyComponent>(); DeserializeSoftbody(childOverride["softbody"], *td->Softbody); }
                    if (childOverride.contains("characterController")) { if (!td->CharacterController) td->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(childOverride["characterController"], *td->CharacterController); }
                    if (childOverride.contains("camera")) { if (!td->Camera) td->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(childOverride["camera"], *td->Camera); }
                    if (childOverride.contains("terrain")) { if (!td->Terrain) td->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(childOverride["terrain"], *td->Terrain); }
                    if (childOverride.contains("emitter")) { if (!td->Emitter) td->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(childOverride["emitter"], *td->Emitter); }
                    if (childOverride.contains("canvas")) { if (!td->Canvas) td->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(childOverride["canvas"], *td->Canvas); }
                    if (childOverride.contains("panel")) { if (!td->Panel) td->Panel = std::make_unique<PanelComponent>(); DeserializePanel(childOverride["panel"], *td->Panel); }
                    if (childOverride.contains("button")) { if (!td->Button) td->Button = std::make_unique<ButtonComponent>(); DeserializeButton(childOverride["button"], *td->Button); }
                    if (childOverride.contains("text")) { if (!td->Text) td->Text = std::make_unique<TextRendererComponent>(); DeserializeText(childOverride["text"], *td->Text); }
                    if (childOverride.contains("terrain")) { if (!td->Terrain) td->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(childOverride["terrain"], *td->Terrain); }
                    if (childOverride.contains("area")) { if (!td->Area) td->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(childOverride["area"], *td->Area); }
                    if (childOverride.contains("navmesh")) { if (!td->Navigation) td->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(childOverride["navmesh"], *td->Navigation); }
                    if (childOverride.contains("navagent")) { if (!td->NavAgent) td->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(childOverride["navagent"], *td->NavAgent); }
                    if (childOverride.contains("navlink")) { if (!td->NavLink) td->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(childOverride["navlink"], *td->NavLink); }
                    if (childOverride.contains("portal")) { if (!td->Portal) td->Portal = std::make_unique<PortalComponent>(); DeserializePortal(childOverride["portal"], *td->Portal); }
                    if (childOverride.contains("grassDeformer")) { if (!td->GrassDeformer) td->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(childOverride["grassDeformer"], *td->GrassDeformer); }
                    if (childOverride.contains("renderOverrides")) { if (!td->RenderOverrides) td->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(childOverride["renderOverrides"], *td->RenderOverrides); }
                    if (childOverride.contains("boneAttachment")) { if (!td->BoneAttachment) td->BoneAttachment = std::make_unique<BoneAttachmentComponent>(); DeserializeBoneAttachment(childOverride["boneAttachment"], *td->BoneAttachment); }
                    if (childOverride.contains("scripts")) { DeserializeScripts(childOverride["scripts"], td->Scripts, scene); }
                    if (childOverride.contains("animator")) { if (!td->AnimationPlayer) td->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(childOverride["animator"], *td->AnimationPlayer); }
                    // TintController on child entities (e.g., skeleton root) - prefab loading path
                    if (childOverride.contains("tintController") && childOverride["tintController"].is_object()) {
                        if (!td->TintController) td->TintController = std::make_unique<TintMaskController>();
                        DeserializeTintController(childOverride["tintController"], *td->TintController);
                        
                        // Build path-to-EntityID map for target resolution
                        std::unordered_map<std::string, EntityID> pathToEntity;
                        std::function<void(EntityID, const std::string&)> buildPathMap = [&](EntityID id, const std::string& parentPath) {
                            auto* data = scene.GetEntityData(id);
                            if (!data) return;
                            std::string path = parentPath.empty() ? data->Name : (parentPath + "/" + data->Name);
                            pathToEntity[path] = id;
                            pathToEntity[ModelNodeIdentity::NormalizePath(path)] = id;  // Also normalized
                            for (EntityID c : data->Children) buildPathMap(c, path);
                        };
                        buildPathMap(rootNew, "");
                        pathToEntity[""] = rootNew;
                        
                        // Resolve TintTarget entityPath to new EntityIDs
                        const auto& tintJson = childOverride["tintController"];
                        if (tintJson.contains("targets") && tintJson["targets"].is_array()) {
                            size_t idx = 0;
                            for (const auto& targetJ : tintJson["targets"]) {
                                if (idx < td->TintController->Targets.size() && targetJ.contains("entityPath")) {
                                    std::string entityPath = targetJ["entityPath"].get<std::string>();
                                    auto pathIt = pathToEntity.find(entityPath);
                                    if (pathIt != pathToEntity.end()) {
                                        td->TintController->Targets[idx].TargetEntity = pathIt->second;
                                    } else {
                                        // Try normalized path
                                        std::string normPath = ModelNodeIdentity::NormalizePath(entityPath);
                                        auto normIt = pathToEntity.find(normPath);
                                        if (normIt != pathToEntity.end()) {
                                            td->TintController->Targets[idx].TargetEntity = normIt->second;
                                        } else {
                                            td->TintController->Targets[idx].TargetEntity = (EntityID)-1;
                                        }
                                    }
                                }
                                idx++;
                            }
                        }
                        td->TintController->NeedsRefresh = true;
                    }
                    if (childOverride.contains("name")) {
                        std::string name = childOverride["name"].get<std::string>();
                        // Strip numeric suffix from model-internal children's names during deserialization
                        // Only strip if NOT user-added (user-added children keep their suffixes)
                        if (!childOverride.contains("_added") || !childOverride["_added"].get<bool>()) {
                            size_t us = name.find_last_of('_');
                            if (us != std::string::npos && us > 0 && us < name.size() - 1) {
                                bool allDigits = true;
                                for (size_t i = us + 1; i < name.size(); ++i) {
                                    if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                        allDigits = false;
                                        break;
                                    }
                                }
                                if (allDigits) {
                                    name = name.substr(0, us);
                                }
                            }
                        }
                        td->Name = name;
                    }
                    if (childOverride.contains("visible")) { td->Visible = childOverride["visible"].get<bool>(); }
                }
            }

            // Apply per-node overrides under prefab roots (compact prefab asset nodes)
            for (size_t i = 0; i < ents.size(); ++i) {
                const json& je = ents[i];
                if (!(je.contains("asset") && je["asset"].is_object())) continue;
                const auto& a = je["asset"]; if (a.value("type", "") != std::string("prefab")) continue;
                EntityID rootNew = idxToNew[i]; if (rootNew == (EntityID)-1) continue;
                if (!je.contains("children") || !je["children"].is_array()) continue;

                struct OverrideItem { std::string relPath; const nlohmann::json* j; int depth; };
                std::vector<OverrideItem> items;
                for (const auto& childOverride : je["children"]) {
                    if (!childOverride.contains("_prefabNodePath")) continue;
                    std::string relPath = childOverride["_prefabNodePath"].get<std::string>();
                    int depth = 0; for (char c : relPath) if (c=='/') ++depth;
                    items.push_back({std::move(relPath), &childOverride, depth});
                }
                std::stable_sort(items.begin(), items.end(), [](const OverrideItem& a, const OverrideItem& b){ return a.depth < b.depth; });

                // Build GUID -> EntityID map for GUID-based fallback
                std::unordered_map<uint64_t, EntityID> guidToEntityId;
                {
                    std::function<void(EntityID)> buildGuidMap = [&](EntityID id) {
                        auto* d = scene.GetEntityData(id);
                        if (!d) return;
                        if (d->EntityGuid.high != 0 || d->EntityGuid.low != 0) {
                            // Use simple hash for lookup (collision-resistant version)
                            uint64_t h = d->EntityGuid.high ^ (d->EntityGuid.low * 0x9e3779b97f4a7c15ULL);
                            guidToEntityId[h] = id;
                        }
                        for (EntityID c : d->Children) buildGuidMap(c);
                    };
                    buildGuidMap(rootNew);
                }

                for (const auto& it : items) {
                    const auto& childOverride = *it.j;
                    const std::string& relPath = it.relPath;
                    EntityID target = resolveByPath(rootNew, relPath);
                    
                    // Fallback: try GUID-based matching if path-based fails
                    // This handles cases where nodes were renamed after override was saved
                    if (target == (EntityID)-1 && childOverride.contains("_prefabEntityGuid")) {
                        try {
                            ClaymoreGUID savedGuid;
                            childOverride.at("_prefabEntityGuid").get_to(savedGuid);
                            if (savedGuid.high != 0 || savedGuid.low != 0) {
                                uint64_t h = savedGuid.high ^ (savedGuid.low * 0x9e3779b97f4a7c15ULL);
                                auto itGuid = guidToEntityId.find(h);
                                if (itGuid != guidToEntityId.end()) {
                                    target = itGuid->second;
                                    std::cout << "[Serializer] Matched prefab override by GUID fallback for path: " << relPath << std::endl;
                                }
                            }
                        } catch (...) {}
                    }
                    
                    if (target == (EntityID)-1) {
                        // If this override looks user-authored (not a pure model node), create entity under intended parent
                        auto has = [&](const char* k){ return childOverride.contains(k); };
                        bool looksUserAdded = has("camera") || has("light") || has("collider") || has("rigidbody") || has("staticbody") || has("softbody")
                            || has("emitter") || has("canvas") || has("panel") || has("button") || has("text") || has("scripts") || has("terrain")
                            || has("navmesh") || has("navagent") || has("navlink") || has("portal") || has("area") || has("grassDeformer") || has("characterController")
                            || (has("animator") && !childOverride["animator"].is_null());
                        if (looksUserAdded) {
                            std::string parentPath;
                            if (!relPath.empty()) {
                                size_t slash = relPath.find_last_of('/');
                                if (slash != std::string::npos) parentPath = relPath.substr(0, slash);
                            }
                            EntityID parentEntity = parentPath.empty() ? rootNew : resolveByPath(rootNew, parentPath);
                            if (parentEntity != (EntityID)-1) {
                                std::string newName = childOverride.value("name", std::string("Entity"));
                                Entity ne = scene.CreateEntityExact(newName);
                                target = ne.GetID();
                                scene.SetParent(target, parentEntity);
                            }
                        }
                    }
                    if (target == (EntityID)-1) continue;
                    auto* td = scene.GetEntityData(target); if (!td) continue;
                    if (childOverride.contains("transform")) { DeserializeTransform(childOverride["transform"], td->Transform); scene.MarkTransformDirty(target); }
                    if (childOverride.contains("mesh")) {
                        // Merge into existing component to preserve fields omitted by override JSON.
                        // When the model was freshly instantiated, keep that mesh identity even if
                        // the serialized override still carries old mesh indices from before reimport.
                        if (td->Mesh) {
                            json meshOverride = childOverride["mesh"];
                            if (meshOverride.is_object() && td->Mesh->mesh) {
                                meshOverride.erase("meshReference");
                                meshOverride.erase("fileID");
                            }
                            DeserializeMesh(meshOverride, *td->Mesh, td->RenderOverrides);
                        } else {
                            MeshComponent tmp; DeserializeMesh(childOverride["mesh"], tmp, td->RenderOverrides);
                            td->Mesh = std::make_unique<MeshComponent>(std::move(tmp));
                        }
                    }
            if (childOverride.contains("light")) { if (!td->Light) td->Light = std::make_unique<LightComponent>(); DeserializeLight(childOverride["light"], *td->Light); }
            if (childOverride.contains("audioSource")) { if (!td->AudioSource) td->AudioSource = std::make_unique<AudioSourceComponent>(); DeserializeAudioSource(childOverride["audioSource"], *td->AudioSource); }
            if (childOverride.contains("audioListener")) { if (!td->AudioListener) td->AudioListener = std::make_unique<AudioListenerComponent>(); DeserializeAudioListener(childOverride["audioListener"], *td->AudioListener); }
            if (childOverride.contains("collider")) { if (!td->Collider) td->Collider = std::make_unique<ColliderComponent>(); DeserializeCollider(childOverride["collider"], *td->Collider); }
                    if (childOverride.contains("rigidbody")) { if (!td->RigidBody) td->RigidBody = std::make_unique<RigidBodyComponent>(); DeserializeRigidBody(childOverride["rigidbody"], *td->RigidBody); }
                    if (childOverride.contains("staticbody")) { if (!td->StaticBody) td->StaticBody = std::make_unique<StaticBodyComponent>(); DeserializeStaticBody(childOverride["staticbody"], *td->StaticBody); }
                    if (childOverride.contains("softbody")) { if (!td->Softbody) td->Softbody = std::make_unique<SoftbodyComponent>(); DeserializeSoftbody(childOverride["softbody"], *td->Softbody); }
                    if (childOverride.contains("characterController")) { if (!td->CharacterController) td->CharacterController = std::make_unique<CharacterControllerComponent>(); DeserializeCharacterController(childOverride["characterController"], *td->CharacterController); }
            if (childOverride.contains("camera")) { if (!td->Camera) td->Camera = std::make_unique<CameraComponent>(); DeserializeCamera(childOverride["camera"], *td->Camera); }
                    if (childOverride.contains("terrain")) { if (!td->Terrain) td->Terrain = std::make_unique<TerrainComponent>(); DeserializeTerrain(childOverride["terrain"], *td->Terrain); }
                    if (childOverride.contains("emitter")) { if (!td->Emitter) td->Emitter = std::make_unique<ParticleEmitterComponent>(); DeserializeParticleEmitter(childOverride["emitter"], *td->Emitter); }
                    if (childOverride.contains("canvas")) { if (!td->Canvas) td->Canvas = std::make_unique<CanvasComponent>(); DeserializeCanvas(childOverride["canvas"], *td->Canvas); }
                    if (childOverride.contains("panel")) { if (!td->Panel) td->Panel = std::make_unique<PanelComponent>(); DeserializePanel(childOverride["panel"], *td->Panel); }
                    if (childOverride.contains("button")) { if (!td->Button) td->Button = std::make_unique<ButtonComponent>(); DeserializeButton(childOverride["button"], *td->Button); }
                    if (childOverride.contains("text")) { if (!td->Text) td->Text = std::make_unique<TextRendererComponent>(); DeserializeText(childOverride["text"], *td->Text); }
                    if (childOverride.contains("area")) { if (!td->Area) td->Area = std::make_unique<cm::physics::AreaComponent>(); Serializer::DeserializeArea(childOverride["area"], *td->Area); }
                    if (childOverride.contains("navmesh")) { if (!td->Navigation) td->Navigation = std::make_unique<nav::NavMeshComponent>(); DeserializeNavMesh(childOverride["navmesh"], *td->Navigation); }
                    if (childOverride.contains("navagent")) { if (!td->NavAgent) td->NavAgent = std::make_unique<nav::NavAgentComponent>(); DeserializeNavAgent(childOverride["navagent"], *td->NavAgent); }
                    if (childOverride.contains("navlink")) { if (!td->NavLink) td->NavLink = std::make_unique<nav::NavLinkComponent>(); DeserializeNavLink(childOverride["navlink"], *td->NavLink); }
                    if (childOverride.contains("portal")) { if (!td->Portal) td->Portal = std::make_unique<PortalComponent>(); DeserializePortal(childOverride["portal"], *td->Portal); }
                    if (childOverride.contains("grassDeformer")) { if (!td->GrassDeformer) td->GrassDeformer = std::make_unique<GrassDeformerComponent>(); DeserializeGrassDeformer(childOverride["grassDeformer"], *td->GrassDeformer); }
                    if (childOverride.contains("renderOverrides")) { if (!td->RenderOverrides) td->RenderOverrides = std::make_unique<RenderOverridesComponent>(); DeserializeRenderOverrides(childOverride["renderOverrides"], *td->RenderOverrides); }
                    if (childOverride.contains("scripts")) { DeserializeScripts(childOverride["scripts"], td->Scripts, scene); }
                    if (childOverride.contains("animator")) { if (!td->AnimationPlayer) td->AnimationPlayer = std::make_unique<cm::animation::AnimationPlayerComponent>(); DeserializeAnimator(childOverride["animator"], *td->AnimationPlayer); }
                    if (childOverride.contains("name")) {
                        std::string name = childOverride["name"].get<std::string>();
                        // Strip numeric suffix from model-internal children's names during deserialization
                        // Only strip if NOT user-added (user-added children keep their suffixes)
                        if (!childOverride.contains("_added") || !childOverride["_added"].get<bool>()) {
                            size_t us = name.find_last_of('_');
                            if (us != std::string::npos && us > 0 && us < name.size() - 1) {
                                bool allDigits = true;
                                for (size_t i = us + 1; i < name.size(); ++i) {
                                    if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                                        allDigits = false;
                                        break;
                                    }
                                }
                                if (allDigits) {
                                    name = name.substr(0, us);
                                }
                            }
                        }
                        td->Name = name;
                    }
                    if (childOverride.contains("visible")) { td->Visible = childOverride["visible"].get<bool>(); }
                }
            }

            // Post-fixups: skeleton links for any serialized skel/skinning nodes created directly (non-opaque areas)
            for (EntityID nid : idxToNew) {
                if (nid == (EntityID)-1) continue;
                auto* d = scene.GetEntityData(nid);
                if (!d) continue;
                if (d->Skinning && d->Mesh) {
                    bool meshIsSkinned = (d->Mesh->mesh && d->Mesh->mesh->HasSkinning());
                    if (meshIsSkinned && !std::dynamic_pointer_cast<SkinnedPBRMaterial>(d->Mesh->material)) {
                        auto oldMat = d->Mesh->material;
                        auto newMat = MaterialManager::Instance().CreateSceneSkinnedDefaultMaterial(&scene);
                        if (oldMat) {
                            // Preserve blend/depth/cull flags and common tint
                            newMat->m_StateFlags = oldMat->GetStateFlags();
                            glm::vec4 tint(1.0f);
                            if (oldMat->TryGetUniform("u_ColorTint", tint)) newMat->SetUniform("u_ColorTint", tint);
                        }
                        d->Mesh->material = newMat;
                        if (!d->Mesh->materials.empty()) {
                            d->Mesh->materials[0] = newMat;
                        }
                    }
                }
                if (d->Skinning && d->Skinning->SkeletonRoot == (EntityID)-1) {
                    EntityID cur = nid; EntityID found = (EntityID)-1; size_t guard = 0;
                    while (cur != (EntityID)-1 && guard++ < 100000) { auto* cd = scene.GetEntityData(cur); if (cd && cd->Skeleton) { found = cur; break; } if (!cd) break; cur = cd->Parent; }
                    d->Skinning->SkeletonRoot = found;
                }
            }

            // Post-load de-duplication: under any parent, if there exists both an opaque model/prefab root child and a
            // regular child with the same (normalized) name, drop the regular duplicate.
            {
                auto normalize = [](const std::string& name) -> std::string {
                    size_t us = name.find_last_of('_');
                    if (us == std::string::npos) return name;
                    bool digits = true; for (size_t i = us + 1; i < name.size(); ++i) { if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; } }
                    return digits ? name.substr(0, us) : name;
                };
                // Build parent -> children list from the newly created ids only
                std::unordered_map<EntityID, std::vector<EntityID>> parentToChildren;
                for (EntityID nid : idxToNew) {
                    if (nid == (EntityID)-1) continue;
                    if (auto* d = scene.GetEntityData(nid)) parentToChildren[d->Parent].push_back(nid);
                }
                for (auto& kv : parentToChildren) {
                    const auto& kids = kv.second;
                    std::unordered_map<std::string, std::vector<EntityID>> byName;
                    for (EntityID c : kids) {
                        if (auto* d = scene.GetEntityData(c)) byName[normalize(d->Name)].push_back(c);
                    }
                    for (auto& g : byName) {
                        const auto& group = g.second;
                        bool hasOpaque = false; for (EntityID c : group) if (opaqueRoots.count(c)) { hasOpaque = true; break; }
                        if (!hasOpaque || group.size() < 2) continue;
                        for (EntityID c : group) {
                            if (opaqueRoots.count(c)) continue;
                            // Consider this a duplicate if it has no Mesh and no user-only components (camera/light/collider etc.)
                            if (auto* d = scene.GetEntityData(c)) {
                                bool userComp = d->Camera || d->Light || d->Collider || d->RigidBody || d->StaticBody || d->CharacterController || d->Emitter || d->Canvas || d->Panel || d->Button;
                                bool hasMesh = (bool)d->Mesh;
                                if (!hasMesh && !userComp) {
                                    scene.RemoveEntity(c);
                                }
                            }
                        }
                    }
                }
            }

            EntityID rootNew = idxToNew.empty() ? (EntityID)-1 : idxToNew[0];
            return rootNew;
        }
        // Legacy single-entity format
        EntityData ed;
        if (!DeserializePrefab(data, ed, scene)) return (EntityID)-1;
        Entity e = scene.CreateEntity(ed.Name.empty() ? "Prefab" : ed.Name);
        EntityData* dst = scene.GetEntityData(e.GetID());
        if (!dst) return (EntityID)-1;
        *dst = ed.DeepCopy(e.GetID(), &scene);
        // Fix up legacy single-entity prefab: resolve skeleton links and skinnings
        if (dst->Skinning && dst->Skinning->SkeletonRoot == (EntityID)-1) {
            // Anchor to self if this entity has a skeleton
            if (dst->Skeleton) dst->Skinning->SkeletonRoot = e.GetID();
        }
        if (dst->Skeleton) {
            // Rebuild BoneEntities by name within this entity subtree
            std::unordered_map<std::string, EntityID> nameToId;
            std::function<void(EntityID)> build = [&](EntityID id){
                auto* ed2 = scene.GetEntityData(id); if (!ed2) return;
                nameToId[ed2->Name] = id; for (EntityID c : ed2->Children) build(c);
            };
            build(e.GetID());
            const size_t n = dst->Skeleton->InverseBindPoses.size();
            dst->Skeleton->BoneEntities.assign(n, (EntityID)-1);
            std::vector<std::string> boneNames(n);
            for (const auto& kv : dst->Skeleton->BoneNameToIndex) { int idx = kv.second; if (idx >= 0 && (size_t)idx < n) boneNames[(size_t)idx] = kv.first; }
            for (size_t i = 0; i < n; ++i) { const std::string& nm = boneNames[i]; if (nm.empty()) continue; auto it = nameToId.find(nm); if (it != nameToId.end()) dst->Skeleton->BoneEntities[i] = it->second; }
        }
        return e.GetID();
    } catch (const std::exception& e) {
        std::cerr << "[Serializer] Error loading prefab to scene: " << e.what() << std::endl;
        return (EntityID)-1;
    }
}

json Serializer::SerializeDynamic(const std::unordered_map<cm::TypeId, cm::ModuleComponent, cm::TypeIdHasher>& dyn) {
    json arr = json::array();
    for (const auto& kv : dyn) {
        const cm::ModuleComponent& c = kv.second;
        json j;
        j["typeId"] = c.GetTypeId().ToHex();
        j["version"] = c.GetVersion();
        j["fields"] = c.SerializeJson()["fields"];
        arr.push_back(std::move(j));
    }
    return arr;
}

void Serializer::DeserializeDynamic(const json& arr, std::unordered_map<cm::TypeId, cm::ModuleComponent, cm::TypeIdHasher>& dyn) {
    if (!arr.is_array()) return;
    for (const auto& j : arr) {
        if (!j.is_object()) continue; // Guard against non-object array elements
        try {
            cm::TypeId id{};
            std::string idStr = j.value("typeId", std::string());
            if (!idStr.empty()) {
                cm::TypeId::TryParseHex(idStr, id);
            }
            // Fallback to name: derive id from typeName if provided
            if ((id.hi==0 && id.lo==0) && j.contains("typeName")) {
                id = cm::TypeId::FromName(j.value("typeName", std::string()));
            }
            if (id.hi==0 && id.lo==0) { std::cerr << "[Serializer] Dynamic missing valid typeId" << std::endl; continue; }
            uint32_t ver = j.value("version", 1u);
            cm::ModuleComponent mc(id, ver);
            // Schema must be defined from registry before applying fields
            if (const auto* d = cm::ComponentRegistry::Instance().Find(id)) {
                mc.DefineFields(d->fields);
                // Reuse ModuleComponent value reader
                json tmp = j;
                // Build a compact object like ModuleComponent expects
                json cobj; cobj["fields"] = j.value("fields", json::object());
                mc.DeserializeJson(cobj);
            } else {
                // Unknown schema: keep empty; still store for missing-module UI
                std::cerr << "[Serializer] Unknown dynamic schema for typeId=" << id << ". Module may be disabled." << std::endl;
            }
            dyn[id] = std::move(mc);
        } catch(...) {
            std::cerr << "[Serializer] Exception deserializing Dynamic component" << std::endl;
        }
    }
}
