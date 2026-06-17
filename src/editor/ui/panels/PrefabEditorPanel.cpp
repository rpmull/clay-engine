#include "PrefabEditorPanel.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_clay_inspector.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <bgfx/bgfx.h>
#include <algorithm>
#include <functional>
#include <unordered_set>
#include "core/rendering/Renderer.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/ParticleEmitterSystem.h"
#include "../UILayer.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/AssetRegistry.h"
#include "editor/pipeline/AssetEventBus.h"
#include "editor/pipeline/ModelNodeIdentity.h"
#include "core/assets/AssetMetadata.h"
#include "editor/Project.h"
#include "core/vfs/VirtualFS.h"
#include "core/rendering/MaterialManager.h"
#include "core/rendering/StandardMeshManager.h"
#include "editor/pipeline/AssetPipeline.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "core/prefab/PrefabAsset.h"
#include "core/prefab/PrefabAPI.h"
#include "core/prefab/PrefabDelta.h"
#include "core/prefab/RuntimePrefabInstantiator.h"
#include "editor/prefab/PrefabEditorAPI.h"

namespace fs = std::filesystem;

// Prefab editor styling constants
static const ImVec4 kPrefabHeaderBg = ImVec4(0.12f, 0.28f, 0.45f, 1.0f);
static const ImVec4 kPrefabHeaderText = ImVec4(0.9f, 0.95f, 1.0f, 1.0f);
static const ImVec4 kPrefabDirtyColor = ImVec4(1.0f, 0.7f, 0.3f, 1.0f);

namespace {

nlohmann::json CaptureComponentSignature(Scene& scene, EntityID id);
void PrewarmPrefabAssetDependencies(const PrefabAsset& asset);
std::string NormalizeAssetPath(const std::string& rawPath);
std::string ResolveAbsoluteAssetPath(const std::string& rawPath);

static constexpr uint16_t kPrefabViewBase = 220;
static constexpr uint16_t kPrefabViewStride = 2; // base + ui
static constexpr uint16_t kPrefabViewMax = 254;
static std::unordered_set<uint16_t> s_PrefabViewIds;
static uint16_t s_NextPrefabViewId = kPrefabViewBase;

static uint16_t AllocatePrefabViewId() {
    for (uint16_t candidate = s_NextPrefabViewId; candidate <= kPrefabViewMax; candidate = (uint16_t)(candidate + kPrefabViewStride)) {
        if (s_PrefabViewIds.insert(candidate).second) {
            s_NextPrefabViewId = (uint16_t)(candidate + kPrefabViewStride);
            return candidate;
        }
    }
    for (uint16_t candidate = kPrefabViewBase; candidate <= kPrefabViewMax; candidate = (uint16_t)(candidate + kPrefabViewStride)) {
        if (s_PrefabViewIds.insert(candidate).second) {
            s_NextPrefabViewId = (uint16_t)(candidate + kPrefabViewStride);
            return candidate;
        }
    }
    std::cerr << "[PrefabEditor] ERROR: Ran out of prefab view IDs. Reusing base view.\n";
    return kPrefabViewBase;
}

static void ReleasePrefabViewId(uint16_t viewIdBase) {
    s_PrefabViewIds.erase(viewIdBase);
}

// Collision-resistant GUID packing (FNV-1a) to avoid XOR collisions
static uint64_t PackGuidStable(const ClaymoreGUID& g) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME = 1099511628211ULL;
    uint64_t hash = FNV_OFFSET;
    for (int i = 0; i < 8; ++i) {
        hash ^= (g.high >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }
    for (int i = 0; i < 8; ++i) {
        hash ^= (g.low >> (i * 8)) & 0xFF;
        hash *= FNV_PRIME;
    }
    return hash;
}

// Keys used for delta detection (must mirror Serializer::SerializeEntity output)
static const std::vector<std::string> kDeltaComponentKeys = {
    "name",
    "parent",
    "children",
    "prefabInstance",
    "prefabGuid",
    "prefabSource",
    "modelAssetGuid",
    "deletedModelNodes",
    "mesh",
    "meshProxy",
    "skeleton",
    "skinning",
    "boneAttachment",
    "blendShapeWeights",
    "unifiedMorphWeights",
    "animator",
    "scripts",
    "camera",
    "light",
    "collider",
    "rigidbody",
    "staticbody",
    "softbody",
    "characterController",
    "area",
    "terrain",
    "resourceLayers",
    "instancer",
    "river",
    "spline",
    "emitter",
    "grassDeformer",
    "canvas",
    "panel",
    "button",
    "slider",
    "progressBar",
    "toggle",
    "scrollView",
    "layoutGroup",
    "inputField",
    "dropdown",
    "uiRect",
    "fitToContent",
    "uiSceneCapture",
    "text",
    "navmesh",
    "navagent",
    "navlink",
    "portal",
    "renderOverrides",
    "tintController",
    "ik",
    "lookat",
    "active",
    "visible",
    "layer",
    "tag",
    // Dynamic sidecar components (editor/runtime extensions)
    "Dynamic"
};

struct RefreshLookup {
    std::unordered_map<uint64_t, EntityID> SceneGuidToId;
    std::unordered_map<std::string, EntityID> PathToId;
    std::unordered_map<std::string, EntityID> NormalizedPathToId;
    std::unordered_map<std::string, EntityID> NormalizedNameToId;
    std::unordered_map<uint64_t, std::vector<EntityID>> ContentHashToIds;
    std::unordered_map<int, EntityID> MeshFileIdToId;
    std::unordered_map<std::string, std::vector<std::pair<EntityID, std::string>>> StableMeshNameToIds;
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>* PrefabToInstanceGuid = nullptr;
};

static prefab::PropertyOverride::ResolutionHints BuildResolutionHints(
    Scene& scene,
    EntityID root,
    EntityID id) {
    prefab::PropertyOverride::ResolutionHints hints;
    auto* data = scene.GetEntityData(id);
    if (!data) {
        return hints;
    }

    hints.NodePath = prefab::ComputeNodePath(scene, root, id);
    hints.NormalizedPath = prefab::NormalizePath(hints.NodePath);
    hints.NormalizedName = prefab::NormalizeName(data->Name);
    hints.ContentHash = prefab::ComputeNodeContentHash(scene, id);
    hints.MeshFileId = (data->Mesh && data->Mesh->meshReference.fileID >= 0)
        ? data->Mesh->meshReference.fileID
        : -1;
    hints.StableMeshName = data->Mesh
        ? (data->Mesh->MeshName.empty() ? data->Name : data->Mesh->MeshName)
        : data->Name;
    if (data->Parent != INVALID_ENTITY_ID) {
        if (auto* parentData = scene.GetEntityData(data->Parent)) {
            hints.ParentNormalizedName = prefab::NormalizeName(parentData->Name);
        }
    }
    return hints;
}

static prefab::PropertyOverride::ResolutionHints BuildResolutionHintsFromJson(const nlohmann::json& hintJson) {
    prefab::PropertyOverride::ResolutionHints hints;
    if (!hintJson.is_object()) {
        return hints;
    }
    hints.NodePath = hintJson.value("nodePath", std::string());
    hints.NormalizedPath = hintJson.value("normalizedPath", std::string());
    hints.NormalizedName = hintJson.value("normalizedName", std::string());
    hints.StableMeshName = hintJson.value("stableMeshName", std::string());
    hints.ParentNormalizedName = hintJson.value("parentName", std::string());
    hints.ContentHash = hintJson.value("contentHash", 0ULL);
    hints.MeshFileId = hintJson.value("meshFileId", -1);
    return hints;
}

static prefab::PropertyOverride::ResolutionHints BuildResolutionHintsFromAddedParent(const prefab::AddedEntity& added) {
    prefab::PropertyOverride::ResolutionHints hints;
    hints.NodePath = added.ParentNodePath;
    hints.NormalizedPath = added.ParentNormalizedPath;
    hints.NormalizedName = added.ParentNormalizedName;
    hints.StableMeshName = added.ParentStableMeshName;
    const size_t lastSlash = added.ParentNormalizedPath.find_last_of('/');
    if (lastSlash != std::string::npos) {
        const size_t prevSlash = lastSlash > 0
            ? added.ParentNormalizedPath.find_last_of('/', lastSlash - 1)
            : std::string::npos;
        const size_t start = (prevSlash == std::string::npos) ? 0 : (prevSlash + 1);
        hints.ParentNormalizedName = added.ParentNormalizedPath.substr(start, lastSlash - start);
    }
    hints.ContentHash = added.ParentContentHash;
    hints.MeshFileId = added.ParentMeshFileId;
    return hints;
}

static prefab::PropertyOverride::ResolutionHints BuildResolutionHintsFromRemoved(const prefab::RemovedEntity& removed) {
    prefab::PropertyOverride::ResolutionHints hints;
    hints.NodePath = removed.NodePath;
    hints.NormalizedPath = removed.NormalizedPath;
    hints.NormalizedName = removed.NormalizedName;
    hints.StableMeshName = removed.StableMeshName;
    hints.ParentNormalizedName = removed.ParentNormalizedName;
    hints.ContentHash = removed.ContentHash;
    hints.MeshFileId = removed.MeshFileId;
    return hints;
}

static void BuildRefreshLookup(Scene& scene, EntityID root, RefreshLookup& lookup) {
    lookup.SceneGuidToId.clear();
    lookup.PathToId.clear();
    lookup.NormalizedPathToId.clear();
    lookup.NormalizedNameToId.clear();
    lookup.ContentHashToIds.clear();
    lookup.MeshFileIdToId.clear();
    lookup.StableMeshNameToIds.clear();

    std::function<void(EntityID, const std::string&, const std::string&)> visit =
        [&](EntityID id, const std::string& path, const std::string& parentNormalizedName) {
            auto* data = scene.GetEntityData(id);
            if (!data) return;

            const std::string normalizedPath = prefab::NormalizePath(path);
            const std::string normalizedName = prefab::NormalizeName(data->Name);
            const std::string stableMeshName = data->Mesh
                ? (data->Mesh->MeshName.empty() ? data->Name : data->Mesh->MeshName)
                : data->Name;

            lookup.SceneGuidToId[PackGuidStable(data->EntityGuid)] = id;
            lookup.PathToId[path] = id;
            lookup.NormalizedPathToId[normalizedPath] = id;
            lookup.NormalizedNameToId[normalizedName] = id;
            lookup.StableMeshNameToIds[prefab::NormalizeName(stableMeshName)].push_back({ id, parentNormalizedName });

            const uint64_t contentHash = prefab::ComputeNodeContentHash(scene, id);
            if (contentHash != 0) {
                lookup.ContentHashToIds[contentHash].push_back(id);
            }
            if (data->Mesh && data->Mesh->meshReference.fileID >= 0) {
                lookup.MeshFileIdToId[data->Mesh->meshReference.fileID] = id;
            }

            for (EntityID child : data->Children) {
                auto* childData = scene.GetEntityData(child);
                if (!childData) continue;
                const std::string childPath = path.empty() ? childData->Name : (path + "/" + childData->Name);
                visit(child, childPath, normalizedName);
            }
        };

    visit(root, std::string(), std::string());
}

static EntityID ResolveEntity(const RefreshLookup& lookup,
                              const ClaymoreGUID& authoredGuid,
                              const prefab::PropertyOverride::ResolutionHints& hints) {
    if (authoredGuid.high != 0 || authoredGuid.low != 0) {
        auto direct = lookup.SceneGuidToId.find(PackGuidStable(authoredGuid));
        if (direct != lookup.SceneGuidToId.end()) {
            return direct->second;
        }
        if (lookup.PrefabToInstanceGuid) {
            auto remap = lookup.PrefabToInstanceGuid->find(authoredGuid);
            if (remap != lookup.PrefabToInstanceGuid->end()) {
                direct = lookup.SceneGuidToId.find(PackGuidStable(remap->second));
                if (direct != lookup.SceneGuidToId.end()) {
                    return direct->second;
                }
            }
        }
    }

    if (!hints.NodePath.empty()) {
        auto it = lookup.PathToId.find(hints.NodePath);
        if (it != lookup.PathToId.end()) {
            return it->second;
        }
    }
    if (!hints.StableMeshName.empty()) {
        const auto stableIt = lookup.StableMeshNameToIds.find(prefab::NormalizeName(hints.StableMeshName));
        if (stableIt != lookup.StableMeshNameToIds.end() && !stableIt->second.empty()) {
            if (stableIt->second.size() == 1) {
                return stableIt->second.front().first;
            }
            for (const auto& candidate : stableIt->second) {
                if (candidate.second == hints.ParentNormalizedName) {
                    return candidate.first;
                }
            }
            return stableIt->second.front().first;
        }
    }
    if (!hints.NormalizedPath.empty()) {
        auto it = lookup.NormalizedPathToId.find(hints.NormalizedPath);
        if (it != lookup.NormalizedPathToId.end()) {
            return it->second;
        }
    }
    if (hints.MeshFileId >= 0) {
        auto it = lookup.MeshFileIdToId.find(hints.MeshFileId);
        if (it != lookup.MeshFileIdToId.end()) {
            return it->second;
        }
    }
    if (hints.ContentHash != 0) {
        auto it = lookup.ContentHashToIds.find(hints.ContentHash);
        if (it != lookup.ContentHashToIds.end() && !it->second.empty()) {
            return it->second.front();
        }
    }
    if (!hints.NormalizedName.empty()) {
        auto it = lookup.NormalizedNameToId.find(hints.NormalizedName);
        if (it != lookup.NormalizedNameToId.end()) {
            return it->second;
        }
    }
    return INVALID_ENTITY_ID;
}

static void RemapGuidIfPresent(const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap,
                               ClaymoreGUID& guid) {
    auto it = guidMap.find(guid);
    if (it != guidMap.end()) {
        guid = it->second;
    }
}

static void RemapGuidIfPresent(const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap,
                               uint64_t& high,
                               uint64_t& low) {
    ClaymoreGUID guid{ high, low };
    auto it = guidMap.find(guid);
    if (it != guidMap.end()) {
        high = it->second.high;
        low = it->second.low;
    }
}

static void RemapPropertyValueEntityMetadata(
    PropertyValue& value,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap) {
    if (std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) {
        auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
        if (!listPtr) return;
        for (auto& meta : listPtr->entityRefs) {
            RemapGuidIfPresent(guidMap, meta.guid);
            RemapGuidIfPresent(guidMap, meta.modelRootGuid);
        }
        for (auto& elem : listPtr->elements) {
            RemapPropertyValueEntityMetadata(elem, guidMap);
        }
        return;
    }

    if (std::holds_alternative<std::shared_ptr<StructPropertyValue>>(value)) {
        auto structPtr = std::get<std::shared_ptr<StructPropertyValue>>(value);
        if (!structPtr) return;
        for (auto& field : structPtr->fields) {
            RemapPropertyValueEntityMetadata(field.second, guidMap);
        }
        return;
    }

    if (std::holds_alternative<std::shared_ptr<DictionaryPropertyValue>>(value)) {
        auto dictPtr = std::get<std::shared_ptr<DictionaryPropertyValue>>(value);
        if (!dictPtr) return;
        for (auto& entry : dictPtr->entries) {
            RemapPropertyValueEntityMetadata(entry.first, guidMap);
            RemapPropertyValueEntityMetadata(entry.second, guidMap);
        }
    }
}

static void RemapSubtreeAuthoringIdentity(
    Scene& scene,
    EntityID root,
    const std::unordered_map<ClaymoreGUID, ClaymoreGUID>& guidMap) {
    std::unordered_set<EntityID> visited;
    std::function<void(EntityID)> remap = [&](EntityID id) {
        if (id == INVALID_ENTITY_ID || !visited.insert(id).second) {
            return;
        }

        EntityData* data = scene.GetEntityData(id);
        if (!data) {
            return;
        }

        RemapGuidIfPresent(guidMap, data->EntityGuid);

        for (auto& lookAt : data->LookAtConstraints) {
            RemapGuidIfPresent(guidMap, lookAt.TargetEntityGuidHigh, lookAt.TargetEntityGuidLow);
        }

        for (auto& ik : data->IKs) {
            RemapGuidIfPresent(guidMap, ik.TargetEntityGuidHigh, ik.TargetEntityGuidLow);
            RemapGuidIfPresent(guidMap, ik.PoleEntityGuidHigh, ik.PoleEntityGuidLow);
        }

        if (data->UISceneCapture) {
            RemapGuidIfPresent(guidMap, data->UISceneCapture->TargetGuidHigh, data->UISceneCapture->TargetGuidLow);
        }

        for (auto& script : data->Scripts) {
            for (auto& [propertyName, meta] : script.EntityRefMetadata) {
                (void)propertyName;
                RemapGuidIfPresent(guidMap, meta.guid);
                RemapGuidIfPresent(guidMap, meta.modelRootGuid);
            }

            for (auto& [propertyName, propertyValue] : script.Values) {
                (void)propertyName;
                RemapPropertyValueEntityMetadata(propertyValue, guidMap);
            }
        }

        for (EntityID child : data->Children) {
            remap(child);
        }
    };

    remap(root);
}

} // namespace

PrefabEditorPanel::PrefabEditorPanel(const std::string& prefabPath, UILayer* uiLayer)
    : m_PrefabPath(prefabPath),
      m_UILayer(uiLayer),
      m_ViewportPanel(m_Scene, &m_SelectedEntity, true)  // useInternalCamera=true for isolated camera
{
    m_ViewIdBase = AllocatePrefabViewId();
    // COMPLETE ISOLATION: This prefab editor has its own:
    // - Scene instance (m_Scene) - completely separate from main scene
    // - Camera instance (via ViewportPanel with useInternalCamera=true) - separate from main viewport camera
    // - Render context (dedicated view ID 220, offscreen framebuffer) - never interferes with main viewport
    // - Particle emitters (cleared on destruction, synced per-scene)
    // This ensures the prefab editor can be open side-by-side with the main scene without any interference.
    
    LoadPrefab(prefabPath);
    RebuildBaseline();
    
    // Subscribe to asset events so we can refresh when models change
    m_AssetEventSubscription = AssetEventBus::Instance().Subscribe(
        AssetType::Mesh,
        [this](AssetEvent event, const std::string& path, ClaymoreGUID guid) {
            OnAssetEvent(static_cast<int>(event), path, guid);
        }
    );
}

PrefabEditorPanel::~PrefabEditorPanel()
{
    // Unsubscribe from asset events
    if (m_AssetEventSubscription != 0) {
        AssetEventBus::Instance().Unsubscribe(m_AssetEventSubscription);
        m_AssetEventSubscription = 0;
    }
    
    // Clear particle emitters owned by this prefab scene
    ecs::ParticleEmitterSystem::Get().ClearSceneEmitters(&m_Scene);
    
    // Reset viewport panel render target size to avoid affecting main scene viewport
    // This ensures the prefab editor's viewport state doesn't leak to other viewports
    m_ViewportPanel.SetRenderTargetSize(0, 0);
    
    if (m_ViewIdBase != 0) {
        Renderer::Get().ReleaseOffscreenTarget(m_ViewIdBase);
        ReleasePrefabViewId(m_ViewIdBase);
        m_ViewIdBase = 0;
    }
}

namespace {

nlohmann::json CaptureComponentSignature(Scene& scene, EntityID id) {
    nlohmann::json entity = Serializer::SerializeEntity(id, scene);
    nlohmann::json signature = nlohmann::json::object();
    auto* data = scene.GetEntityData(id);
    std::unordered_set<std::string> handledKeys;
    handledKeys.reserve(kDeltaComponentKeys.size() + 8);
    auto markHandled = [&](const std::string& key) { handledKeys.insert(key); };

    // Capture all component types for delta detection
    // Note: Transform is handled separately in ComputeDeltas
    for (const auto& key : kDeltaComponentKeys) {
        if (key == "parent") {
            std::string parentGuid;
            if (data && data->Parent != INVALID_ENTITY_ID) {
                if (auto* pd = scene.GetEntityData(data->Parent)) {
                    parentGuid = pd->EntityGuid.ToString();
                }
            }
            signature[key] = parentGuid;
            markHandled(key);
            continue;
        }
        if (key == "children") {
            if (data && !data->Children.empty()) {
                nlohmann::json children = nlohmann::json::array();
                for (EntityID child : data->Children) {
                    if (auto* cd = scene.GetEntityData(child)) {
                        children.push_back(cd->EntityGuid.ToString());
                    }
                }
                if (!children.empty()) {
                    std::sort(children.begin(), children.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                        return a.dump() < b.dump();
                    });
                }
                signature[key] = std::move(children);
            }
            markHandled(key);
            continue;
        }

        markHandled(key);
        if (!entity.contains(key) || entity[key].is_null()) continue;

        if (key == "scripts" && entity[key].is_array()) {
            // Sort scripts for stable comparison
            auto scripts = entity[key];
            if (!scripts.empty()) {
                std::vector<nlohmann::json> sorted = scripts.get<std::vector<nlohmann::json>>();
                std::sort(sorted.begin(), sorted.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                    std::string ca = a.value("className", "");
                    std::string cb = b.value("className", "");
                    if (ca == cb) return a.dump() < b.dump();
                    return ca < cb;
                });
                signature[key] = std::move(sorted);
            }
        } else if (key == "skeleton" && entity[key].is_object()) {
            // For skeleton comparison, only use stable identity fields to avoid
            // false positives from floating-point matrix differences when the same
            // model is loaded twice. The matrices (inverseBindPoses, bindPoseGlobals)
            // may have tiny precision differences between loads.
            nlohmann::json skelSig = nlohmann::json::object();
            const auto& skel = entity[key];
            // Only compare structural identity, not matrix data
            if (skel.contains("skeletonGuid")) skelSig["skeletonGuid"] = skel["skeletonGuid"];
            if (skel.contains("boneNames")) skelSig["boneNames"] = skel["boneNames"];
            if (skel.contains("boneParents")) skelSig["boneParents"] = skel["boneParents"];
            if (skel.contains("jointGuids")) skelSig["jointGuids"] = skel["jointGuids"];
            // Only mark as having skeleton if it's non-empty after filtering
            if (!skelSig.empty()) {
                signature[key] = std::move(skelSig);
            }
        } else if (key == "skinning" && entity[key].is_object()) {
            // For skinning, only compare identity (guid), not vertex weight data
            nlohmann::json skinSig = nlohmann::json::object();
            const auto& skin = entity[key];
            if (skin.contains("meshGuid")) skinSig["meshGuid"] = skin["meshGuid"];
            // Only mark as having skinning if it has identity
            if (!skinSig.empty()) {
                signature[key] = std::move(skinSig);
            }
        } else if (key == "prefabInstance" && entity[key].is_object()) {
            nlohmann::json prefabSig = nlohmann::json::object();
            const auto& inst = entity[key];
            if (inst.contains("prefabGuid")) prefabSig["prefabGuid"] = inst["prefabGuid"];
            if (inst.contains("prefabPath")) prefabSig["prefabPath"] = inst["prefabPath"];
            if (inst.contains("modelGuid")) prefabSig["modelGuid"] = inst["modelGuid"];
            if (inst.contains("modelPath")) prefabSig["modelPath"] = inst["modelPath"];
            if (inst.contains("owned") && inst["owned"].is_array()) {
                std::vector<nlohmann::json> owned = inst["owned"].get<std::vector<nlohmann::json>>();
                std::sort(owned.begin(), owned.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                    return a.dump() < b.dump();
                });
                prefabSig["owned"] = std::move(owned);
            }
            if (inst.contains("guidRemap") && inst["guidRemap"].is_array()) {
                std::vector<nlohmann::json> remap = inst["guidRemap"].get<std::vector<nlohmann::json>>();
                std::sort(remap.begin(), remap.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                    std::string ap = a.contains("prefab") ? a["prefab"].dump() : std::string();
                    std::string bp = b.contains("prefab") ? b["prefab"].dump() : std::string();
                    if (ap == bp) {
                        std::string ai = a.contains("instance") ? a["instance"].dump() : std::string();
                        std::string bi = b.contains("instance") ? b["instance"].dump() : std::string();
                        return ai < bi;
                    }
                    return ap < bp;
                });
                prefabSig["guidRemap"] = std::move(remap);
            }
            if (!prefabSig.empty()) {
                signature[key] = std::move(prefabSig);
            }
        } else {
            signature[key] = entity[key];
        }
    }

    // Include any additional serialized fields not covered by the explicit list.
    // This ensures new/unknown components still participate in delta detection,
    // while skipping unstable or internal metadata.
    static const std::unordered_set<std::string> kIgnoredKeys = {
        "id", "guid", "transform", "parent", "children", "asset"
    };
    for (auto it = entity.begin(); it != entity.end(); ++it) {
        const std::string& key = it.key();
        if (handledKeys.count(key) > 0) continue;
        if (kIgnoredKeys.count(key) > 0) continue;
        if (!key.empty() && key[0] == '_') continue; // internal metadata
        if (it.value().is_null()) continue;
        signature[key] = it.value();
    }
    return signature;
}

std::string NormalizeAssetPath(const std::string& rawPath) {
    if (rawPath.empty()) return {};
    std::string norm = IVirtualFS::NormalizePath(rawPath);
    std::string vpath = VFS::StripToKnownPrefix(norm);
    return vpath.empty() ? norm : vpath;
}

std::string ResolveAbsoluteAssetPath(const std::string& rawPath) {
    if (rawPath.empty()) return {};
    fs::path candidate(rawPath);
    std::error_code ec;
    if (candidate.is_absolute() && fs::exists(candidate, ec)) {
        return candidate.string();
    }
    try {
        fs::path proj = Project::GetProjectDirectory();
        if (!proj.empty()) {
            candidate = proj / rawPath;
            if (fs::exists(candidate, ec)) return candidate.string();
        }
    } catch(...) {}
    return {};
}

void RegisterAssetBlock(const nlohmann::json& assetBlock) {
    if (!assetBlock.is_object()) return;
    std::string type = assetBlock.value("type", std::string());
    std::string guidStr = assetBlock.value("guid", std::string());
    std::string path = assetBlock.value("path", std::string());
    if (guidStr.empty() || path.empty()) return;

    ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
    if (guid.high == 0 && guid.low == 0) return;

    std::string normalizedPath = NormalizeAssetPath(path);
    if (normalizedPath.empty()) normalizedPath = path;
    std::string assetName = fs::path(normalizedPath).filename().string();

    AssetType assetType = AssetType::Mesh;
    if (type == "prefab") assetType = AssetType::Prefab;

    AssetReference ref(guid, 0, (int)assetType);
    AssetLibrary::Instance().RegisterAsset(ref, assetType, normalizedPath, assetName.empty() ? normalizedPath : assetName);

    auto registerAlias = [&](const std::string& candidate) {
        if (!candidate.empty()) AssetLibrary::Instance().RegisterPathAlias(guid, candidate);
    };

    registerAlias(normalizedPath);
    registerAlias(ResolveAbsoluteAssetPath(normalizedPath));

    fs::path metaRel(normalizedPath);
    if (metaRel.extension() != ".meta") {
        metaRel.replace_extension(".meta");
        std::string metaRelStr = metaRel.string();
        registerAlias(metaRelStr);
        registerAlias(ResolveAbsoluteAssetPath(metaRelStr));
    }
}

// Scan project for .meta files and register any that match needed GUIDs
void ScanAndRegisterMeshGuids(const std::vector<ClaymoreGUID>& neededGuids) {
    if (neededGuids.empty()) return;
    
    try {
        fs::path projectDir = Project::GetProjectDirectory();
        fs::path assetsDir = projectDir / "assets";
        if (!fs::exists(assetsDir)) return;
        
        std::unordered_set<uint64_t> neededSet;
        auto pack = [](const ClaymoreGUID& g) -> uint64_t { return PackGuidStable(g); };
        for (const auto& g : neededGuids) {
            if (g.high != 0 || g.low != 0) neededSet.insert(pack(g));
        }
        
        // Scan for .meta files
        for (auto& entry : fs::recursive_directory_iterator(assetsDir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".meta") continue;
            
            try {
                std::ifstream in(entry.path().string());
                if (!in) continue;
                nlohmann::json j; in >> j; in.close();
                
                if (!j.contains("guid")) continue;
                ClaymoreGUID fileGuid = j.at("guid").get<ClaymoreGUID>();
                
                // Check if this GUID is one we need
                if (neededSet.find(pack(fileGuid)) == neededSet.end()) continue;
                
                // Register this asset
                std::string metaPath = entry.path().string();
                std::string vpath = metaPath;
                std::replace(vpath.begin(), vpath.end(), '\\', '/');
                size_t pos = vpath.find("assets/");
                if (pos != std::string::npos) vpath = vpath.substr(pos);
                
                std::string name = entry.path().stem().string();
                AssetReference ref(fileGuid, 0, (int)AssetType::Mesh);
                AssetLibrary::Instance().RegisterAsset(ref, AssetType::Mesh, vpath, name);
                AssetLibrary::Instance().RegisterPathAlias(fileGuid, metaPath);
                
                // Also register without .meta extension
                std::string basePath = metaPath;
                if (basePath.size() > 5 && basePath.substr(basePath.size() - 5) == ".meta") {
                    basePath = basePath.substr(0, basePath.size() - 5);
                    AssetLibrary::Instance().RegisterPathAlias(fileGuid, basePath);
                }
                
                std::cout << "[PrefabEditor] Registered mesh asset: " << name << " (GUID: " << fileGuid.ToString() << ")" << std::endl;
            } catch (...) {}
        }
    } catch (...) {}
}

void PrewarmPrefabAssetDependencies(const PrefabAsset& asset) {
    std::vector<ClaymoreGUID> neededGuids;
    
    // In unified format, entities are JSON objects with components at root level
    for (const auto& entityJson : asset.Entities) {
        if (!entityJson.is_object()) continue;
        
        // Handle embedded model asset blocks
        if (entityJson.contains("asset")) {
            try {
                RegisterAssetBlock(entityJson.at("asset"));
            } catch(...) {}
        }
        // Collect mesh GUIDs that need to be resolved
        if (entityJson.contains("mesh")) {
            try {
                const auto& mesh = entityJson.at("mesh");
                if (mesh.contains("meshReference")) {
                    const auto& ref = mesh.at("meshReference");
                    std::string guidStr = ref.value("guid", std::string());
                    if (!guidStr.empty()) {
                        ClaymoreGUID guid = ClaymoreGUID::FromString(guidStr);
                        if (!(guid.high == 0 && guid.low == 0)) {
                            // Check if already registered
                            AssetEntry* existing = AssetLibrary::Instance().GetAsset(guid);
                            if (!existing) {
                                neededGuids.push_back(guid);
                            }
                        }
                    }
                }
            } catch(...) {}
        }
    }
    
    // Scan for any missing assets
    if (!neededGuids.empty()) {
        ScanAndRegisterMeshGuids(neededGuids);
    }
}

} // namespace


void PrefabEditorPanel::ClearLoadedPrefabScene(bool preserveEditorLight) {
    const EntityID preservedLight = preserveEditorLight ? m_EditorLight : INVALID_ENTITY_ID;

    std::vector<EntityID> removableRoots;
    removableRoots.reserve(m_Scene.GetEntities().size());
    for (const auto& entity : m_Scene.GetEntities()) {
        auto* data = m_Scene.GetEntityData(entity.GetID());
        if (!data) continue;
        if (entity.GetID() == preservedLight) continue;
        if (data->Parent == INVALID_ENTITY_ID) {
            removableRoots.push_back(entity.GetID());
        }
    }
    for (EntityID id : removableRoots) {
        if (m_Scene.GetEntityData(id)) {
            m_Scene.RemoveEntity(id);
        }
    }

    if (!preserveEditorLight) {
        m_EditorLight = INVALID_ENTITY_ID;
    }

    m_PrefabRoot = INVALID_ENTITY_ID;
    m_SelectedEntity = INVALID_ENTITY_ID;
    m_PrefabAssetGuid = {};
    m_PrefabRootGuid = {};
    m_DependentModelGuids.clear();
    m_DependentModelAssetGuids.clear();
    m_DependentModelPaths.clear();
    m_BaselineGuids.clear();
    m_BaselineNames.clear();
    m_BaselinePaths.clear();
    m_BaselineTransforms.clear();
    m_BaselineComponents.clear();
    m_AddedDescriptions.clear();
    m_RemovedDescriptions.clear();
    m_ModifiedDescriptions.clear();
    m_AddedGUIDs.clear();
    m_ModifiedGUIDs.clear();
    m_IsDirty = false;
    m_DeltasStale = true;
    m_LastDeltaSceneRevision = 0;
    m_DeltaFrameCounter = 0;
    m_PendingCloseConfirmation = false;
}

size_t PrefabEditorPanel::GetEditableEntityCount() const {
    size_t count = 0;
    for (const auto& entity : m_Scene.GetEntities()) {
        if (entity.GetID() == m_EditorLight) continue;
        if (const_cast<Scene&>(m_Scene).GetEntityData(entity.GetID())) {
            ++count;
        }
    }
    return count;
}

void PrefabEditorPanel::SetStatusMessage(const std::string& message, StatusLevel level) {
    m_StatusText = message;
    m_StatusLevel = level;
}

void PrefabEditorPanel::RefreshDiagnostics(const PrefabAsset* asset) {
    m_DiagnosticErrors.clear();
    m_DiagnosticWarnings.clear();

    prefab_editor::PrefabDiagnostics diagnostics;
    if (asset) {
        diagnostics = prefab_editor::ValidatePrefab(*asset);
    } else if (!m_PrefabPath.empty() && fs::exists(m_PrefabPath)) {
        diagnostics = prefab_editor::ValidatePrefabFile(m_PrefabPath);
    } else {
        return;
    }

    m_DiagnosticErrors = diagnostics.Errors;
    m_DiagnosticWarnings = diagnostics.Warnings;
}

bool PrefabEditorPanel::LoadPrefab(const std::string& path)
{
    ClearLoadedPrefabScene(true);

    if (!fs::exists(path)) {
        SetStatusMessage("Prefab file not found.", StatusLevel::Error);
        std::cerr << "[PrefabEditor] Prefab file not found: " << path << std::endl;
        return false;
    }

    PrefabAsset warmAsset;
    if (!PrefabIO::LoadPrefabSource(path, warmAsset)) {
        SetStatusMessage("Failed to parse prefab file.", StatusLevel::Error);
        std::cerr << "[PrefabEditor] Failed to parse prefab JSON: " << path << std::endl;
        return false;
    }

    PrewarmPrefabAssetDependencies(warmAsset);
    m_PrefabAssetGuid = warmAsset.Guid;
    m_PrefabRootGuid = warmAsset.RootGuid;
    RefreshDiagnostics(&warmAsset);

    for (const auto& entityJson : warmAsset.Entities) {
        if (!entityJson.is_object()) continue;
        if (!entityJson.contains("asset") || !entityJson["asset"].is_object()) continue;

        const auto& assetBlock = entityJson["asset"];
        if (assetBlock.value("type", std::string()) != "model") continue;

        const std::string modelPath = NormalizeAssetPath(assetBlock.value("path", std::string()));
        if (!modelPath.empty()) {
            m_DependentModelPaths.insert(modelPath);
        }

        const std::string guidStr = assetBlock.value("guid", std::string());
        if (!guidStr.empty()) {
            try {
                const ClaymoreGUID modelGuid = ClaymoreGUID::FromString(guidStr);
                if (modelGuid.high != 0 || modelGuid.low != 0) {
                    m_DependentModelAssetGuids.insert(PackGuidStable(modelGuid));
                }
            } catch (...) {}
        }
    }

    EntityID root = InstantiatePrefabFromPath(path, m_Scene);
    if (root == INVALID_ENTITY_ID || root == 0) {
        SetStatusMessage("Failed to instantiate prefab into editor scene.", StatusLevel::Error);
        std::cerr << "[PrefabEditor] Failed to load prefab into scene: " << path << std::endl;
        return false;
    }

    m_PrefabRoot = root;
    m_SelectedEntity = root;

    m_Scene.MarkTransformDirty(m_PrefabRoot);
    m_Scene.UpdateTransforms();
    EnsureEditorLighting();
    InitializeCameraToBounds();

    int meshCount = 0;
    int loadedCount = 0;
    for (const auto& entity : m_Scene.GetEntities()) {
        auto* data = m_Scene.GetEntityData(entity.GetID());
        if (!data || !data->Mesh) continue;
        ++meshCount;
        if (data->Mesh->mesh) {
            ++loadedCount;
        } else {
            std::cerr << "[PrefabEditor] Mesh not loaded for: " << data->Name
                      << " (guid=" << data->Mesh->meshReference.guid.ToString() << ")" << std::endl;
        }
        if (data->Mesh->meshReference.IsValid()) {
            m_DependentModelGuids.insert(PackGuidStable(data->Mesh->meshReference.guid));
        }
    }

    m_Scene.ClearDirty();
    m_LastDeltaSceneRevision = m_Scene.GetDirtyRevision();
    m_IsDirty = false;
    m_DeltasStale = true;

    if (!m_DiagnosticErrors.empty()) {
        SetStatusMessage("Loaded prefab with validation errors.", StatusLevel::Error);
    } else if (!m_DiagnosticWarnings.empty()) {
        SetStatusMessage("Loaded prefab with warnings.", StatusLevel::Warning);
    } else {
        SetStatusMessage("Prefab loaded.", StatusLevel::Info);
    }

    std::cout << "[PrefabEditor] Loaded " << GetEditableEntityCount()
              << " editable entities (" << loadedCount << "/" << meshCount << " meshes resolved)" << std::endl;
    return true;
}

bool PrefabEditorPanel::SavePrefab()
{
    if (m_PrefabRoot == -1) return false;
    
    // Don't allow saving during play mode
    if (m_InPlayMode) {
        SetStatusMessage("Saving is disabled during play mode.", StatusLevel::Warning);
        std::cerr << "[PrefabEditor] Cannot save prefab during play mode\n";
        return false;
    }
    // Build authoring asset and save as .prefab (GUID JSON)
    PrefabAsset asset;
    if (!prefab_editor::BuildPrefabAssetFromScene(m_Scene, m_PrefabRoot, asset)) {
        SetStatusMessage("Failed to build prefab from editor scene.", StatusLevel::Error);
        return false;
    }
    if (m_PrefabAssetGuid.high != 0 || m_PrefabAssetGuid.low != 0) {
        asset.Guid = m_PrefabAssetGuid;
        if (asset.Raw.is_object()) {
            asset.Raw["guid"] = m_PrefabAssetGuid;
        }
    }
    if (m_PrefabRootGuid.high != 0 || m_PrefabRootGuid.low != 0) {
        if (asset.FindEntityByGuid(m_PrefabRootGuid)) {
            asset.RootGuid = m_PrefabRootGuid;
        } else {
            std::cerr << "[PrefabEditor] Cached root GUID " << m_PrefabRootGuid.ToString()
                      << " was not found in rebuilt prefab entities. Using rebuilt root GUID "
                      << asset.RootGuid.ToString() << " instead.\n";
            m_PrefabRootGuid = asset.RootGuid;
        }
        if (asset.Raw.is_object()) {
            asset.Raw["rootGuid"] = asset.RootGuid;
        }
    } else if (asset.Raw.is_object()) {
        asset.Raw["rootGuid"] = asset.RootGuid;
    }
    // Prefer keeping same path; ensure extension is .prefab
    std::filesystem::path p(m_PrefabPath);
    if (p.extension() != ".prefab") p.replace_extension(".prefab");
    m_PrefabPath = p.string();

    RefreshDiagnostics(&asset);
    if (!m_DiagnosticErrors.empty()) {
        SetStatusMessage("Validation failed. Prefab was not saved.", StatusLevel::Error);
        return false;
    }

    if (!PrefabIO::SavePrefab(m_PrefabPath, asset)) {
        SetStatusMessage("Failed to save prefab.", StatusLevel::Error);
        std::cerr << "[PrefabEditor] Save failed for: " << m_PrefabPath << std::endl;
        return false;
    }

    m_IsDirty = false;
    RebuildBaseline();
    // Ensure prefab is registered and has a .meta
    try {
        std::string name = p.filename().string();
        std::error_code ec;
        std::filesystem::path rel = std::filesystem::relative(p, Project::GetProjectDirectory(), ec);
        std::string vpath = (ec ? p.string() : rel.string());
        std::replace(vpath.begin(), vpath.end(), '\\', '/');
        size_t pos = vpath.find("assets/"); if (pos != std::string::npos) vpath = vpath.substr(pos);
        std::filesystem::path metaPath = p; metaPath += ".meta";
        AssetMetadata meta; bool hasMeta = false;
        if (std::filesystem::exists(metaPath)) {
            std::ifstream in(metaPath.string()); if (in) { nlohmann::json j; in >> j; in.close(); meta = j.get<AssetMetadata>(); hasMeta = true; }
        }
        // Always align .meta GUID with authoring prefab GUID to keep instance linkage stable
        meta.guid = asset.Guid;
        meta.type = "prefab";
        meta.sourcePath = m_PrefabPath;
        meta.processedPath = m_PrefabPath;
        meta.hash = AssetPipeline::Instance().ComputeFileHash(m_PrefabPath);
        meta.reference = AssetReference(meta.guid, 0, static_cast<int32_t>(AssetType::Prefab));
        nlohmann::json j = meta; std::ofstream outm(metaPath.string()); outm << j.dump(4); outm.close();
        AssetRegistry::Instance().SetMetadata(m_PrefabPath, meta);
        AssetLibrary::Instance().RegisterAsset(AssetReference(meta.guid, 0, (int)AssetType::Prefab), AssetType::Prefab, vpath, name);
        AssetLibrary::Instance().RegisterPathAlias(meta.guid, m_PrefabPath);
    } catch(...) {}
    BinaryAssetCache::Instance().Invalidate(m_PrefabPath);
    if (!BinaryAssetCache::Instance().EnsureBinary(m_PrefabPath)) {
        std::cerr << "[PrefabEditor] Failed to rebuild prefab binary for: " << m_PrefabPath << std::endl;
    }
    runtime::RuntimePrefabInstantiator::InvalidateCache(m_PrefabPath);
    runtime::RuntimePrefabInstantiator::InvalidateCache(BinaryAssetCache::Instance().GetBinaryPath(m_PrefabPath));
    AssetPipeline::Instance().HotSwapPrefabInScene(m_PrefabPath);

    m_Scene.ClearDirty();
    m_LastDeltaSceneRevision = m_Scene.GetDirtyRevision();
    if (!m_DiagnosticWarnings.empty()) {
        SetStatusMessage("Prefab saved with warnings.", StatusLevel::Warning);
    } else {
        SetStatusMessage("Prefab saved.", StatusLevel::Info);
    }

    return true;
}

void PrefabEditorPanel::OnImGuiRender()
{
    if (!m_IsOpen) return;

    // Visible title plus hidden unique ID to avoid collisions when opening same-named prefabs
    std::string prefabFileName = fs::path(m_PrefabPath).filename().string();
    std::string displayName = "Prefab Editor - " + prefabFileName;
    if (m_IsDirty) displayName += "*";
    std::string windowTitle = displayName + std::string("###PrefabEditor|") + m_PrefabPath;
    if (m_FocusNextFrame) { ImGui::SetNextWindowFocus(); m_FocusNextFrame = false; }
    // Dock into the main dockspace on first open so it appears as its own tab
    if (!m_Docked && m_UILayer) {
        ImGui::SetNextWindowDockID(m_UILayer->GetMainDockspaceID(), ImGuiCond_Appearing);
        m_Docked = true;
    }

    // Apply a subtle prefab-mode tint to the window
    ImGui::PushStyleColor(ImGuiCol_TitleBg, kPrefabHeaderBg);
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(kPrefabHeaderBg.x + 0.05f, kPrefabHeaderBg.y + 0.05f, kPrefabHeaderBg.z + 0.05f, 1.0f));

    bool windowOpen = true;
    const bool windowVisible = ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_MenuBar);
    ImGui::PopStyleColor(2);

    // Track whether this window should drive the shared hierarchy/inspector (focus only to avoid flicker)
    m_IsFocusedOrHovered = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    bool requestClose = !windowOpen;
    if (windowVisible) {
        if (!m_InPlayMode) {
            const uint64_t sceneRevision = m_Scene.GetDirtyRevision();
            if (m_DeltasStale || sceneRevision != m_LastDeltaSceneRevision) {
                if (m_DeltasStale || ++m_DeltaFrameCounter >= 6) {
                    ComputeDeltas();
                }
            } else {
                m_DeltaFrameCounter = 0;
            }
        } else {
            m_DeltaFrameCounter = 0;
        }

        const size_t totalChanges =
            m_AddedDescriptions.size() +
            m_ModifiedDescriptions.size() +
            m_RemovedDescriptions.size();
        size_t modelDependencyCount = m_DependentModelPaths.size();
        modelDependencyCount = std::max(modelDependencyCount, m_DependentModelAssetGuids.size());
        if (modelDependencyCount == 0) {
            modelDependencyCount = m_DependentModelGuids.size();
        }

        auto statusColor = [&]() -> ImVec4 {
            switch (m_StatusLevel) {
            case StatusLevel::Warning: return ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
            case StatusLevel::Error: return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
            case StatusLevel::Info:
            default:
                return ImVec4(0.55f, 0.85f, 1.0f, 1.0f);
            }
        };
        auto statusLabel = [&]() -> const char* {
            switch (m_StatusLevel) {
            case StatusLevel::Warning: return "Warning";
            case StatusLevel::Error: return "Error";
            case StatusLevel::Info:
            default:
                return "Ready";
            }
        };

        // Menu bar with File options
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                const bool canSave = (m_PrefabRoot != INVALID_ENTITY_ID) && !m_InPlayMode;
                if (ImGui::MenuItem("Save", "Ctrl+S", false, canSave)) {
                    ComputeDeltas();
                    SavePrefab();
                }
                if (ImGui::MenuItem("Close")) {
                    requestClose = true;
                }
                ImGui::EndMenu();
            }

            if (m_IsDirty) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kPrefabDirtyColor);
                ImGui::TextUnformatted("Unsaved changes");
                ImGui::PopStyleColor();
            }

            ImGui::EndMenuBar();
        }

        // Prefab Mode Header Banner
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            const float bannerHeight = 30.0f;
            const float availWidth = ImGui::GetContentRegionAvail().x;

            ImRect bannerRect(cursorPos, ImVec2(cursorPos.x + availWidth, cursorPos.y + bannerHeight));

            ImU32 leftColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.08f, 0.22f, 0.38f, 0.96f));
            ImU32 rightColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.16f, 0.38f, 0.56f, 0.96f));
            drawList->AddRectFilledMultiColor(bannerRect.Min, bannerRect.Max, leftColor, rightColor, rightColor, leftColor);
            drawList->AddRect(bannerRect.Min, bannerRect.Max, ImGui::ColorConvertFloat4ToU32(ImVec4(0.34f, 0.56f, 0.76f, 0.55f)), 3.0f);

            const float textY = bannerRect.Min.y + (bannerHeight - ImGui::GetFontSize()) * 0.5f;
            const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(kPrefabHeaderText);
            if (m_InPlayMode) {
                drawList->AddText(ImVec2(bannerRect.Min.x + 10.0f, textY),
                                  ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.72f, 0.3f, 1.0f)),
                                  "PREFAB EDITING (PLAY MODE - SAVE PAUSED)");
            } else {
                drawList->AddText(ImVec2(bannerRect.Min.x + 10.0f, textY), textColor, "PREFAB EDITING MODE");
            }

            const std::string nameLabel = prefabFileName.empty() ? std::string("<unnamed prefab>") : prefabFileName;
            const ImVec2 nameSize = ImGui::CalcTextSize(nameLabel.c_str());
            drawList->AddText(ImVec2(bannerRect.Max.x - nameSize.x - 10.0f, textY),
                              m_IsDirty ? ImGui::ColorConvertFloat4ToU32(kPrefabDirtyColor) : textColor,
                              nameLabel.c_str());

            ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, cursorPos.y + bannerHeight + 6.0f));
        }

        // Status / actions bar
        {
            const float statusHeight = ImGui::GetFrameHeightWithSpacing() * 2.25f;
            ImGui::BeginChild("PrefabStatusBar", ImVec2(0, statusHeight), false, ImGuiWindowFlags_NoScrollbar);

            const bool hasPrefabScene = (m_PrefabRoot != INVALID_ENTITY_ID);
            const bool canSave = hasPrefabScene && !m_InPlayMode;
            const bool canResetView = hasPrefabScene;

            if (!canSave) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Save")) {
                ComputeDeltas();
                SavePrefab();
            }
            if (!canSave) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::SmallButton("Changes")) {
                if (!m_InPlayMode) {
                    ComputeDeltas();
                }
                ImGui::OpenPopup("ChangesPopup");
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Diagnostics")) {
                ImGui::OpenPopup("PrefabDiagnosticsPopup");
            }

            ImGui::SameLine();
            if (!canResetView) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Reset View")) {
                InitializeCameraToBounds();
            }
            if (!canResetView) ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::SmallButton("Locate")) {
                if (m_UILayer) {
                    std::filesystem::path p(m_PrefabPath);
                    m_UILayer->GetProjectPanel().NavigateTo(p.parent_path().string());
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open prefab folder in Project panel");
            }

            std::string metrics =
                std::to_string(GetEditableEntityCount()) + " entities | " +
                std::to_string(totalChanges) + " changes | " +
                std::to_string(modelDependencyCount) + " model refs";
            const float metricsWidth = ImGui::CalcTextSize(metrics.c_str()).x;
            const float metricsX = ImGui::GetWindowContentRegionMax().x - metricsWidth;
            if (metricsX > ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x) {
                ImGui::SameLine();
                ImGui::SetCursorPosX(metricsX);
            } else {
                ImGui::SameLine();
            }
            ImGui::TextDisabled("%s", metrics.c_str());

            ImGui::Separator();
            ImGui::TextColored(statusColor(), "%s", statusLabel());
            ImGui::SameLine();
            ImGui::TextUnformatted(m_StatusText.empty() ? "Prefab editor ready." : m_StatusText.c_str());

            if (EntityData* selectedData = m_Scene.GetEntityData(m_SelectedEntity)) {
                ImGui::SameLine();
                ImGui::TextDisabled("Selected: %s", selectedData->Name.c_str());
            }

            if (!m_DiagnosticErrors.empty() || !m_DiagnosticWarnings.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("Validation: %d error(s), %d warning(s)",
                                    (int)m_DiagnosticErrors.size(),
                                    (int)m_DiagnosticWarnings.size());
            }

            if (m_InPlayMode) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1.0f), "Live delta tracking paused");
            }

            if (ImGui::BeginPopup("ChangesPopup")) {
                if (m_InPlayMode) {
                    ImGui::TextWrapped("Change tracking is paused during play mode. Save and authoritative delta recomputation resume when play mode ends.");
                } else if (totalChanges == 0) {
                    ImGui::TextUnformatted("No unsaved changes.");
                } else {
                    ImGui::Text("Changes since last save: %d", (int)totalChanges);
                    ImGui::Separator();
                    if (!m_AddedDescriptions.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.6f, 1.0f));
                        for (const auto& s : m_AddedDescriptions) ImGui::BulletText("%s", s.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (!m_ModifiedDescriptions.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.85f, 0.45f, 1.0f));
                        for (const auto& s : m_ModifiedDescriptions) ImGui::BulletText("%s", s.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (!m_RemovedDescriptions.empty()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
                        for (const auto& s : m_RemovedDescriptions) ImGui::BulletText("%s", s.c_str());
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopup("PrefabDiagnosticsPopup")) {
                if (m_DiagnosticErrors.empty() && m_DiagnosticWarnings.empty()) {
                    ImGui::TextUnformatted("Prefab validates cleanly.");
                } else {
                    if (!m_DiagnosticErrors.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Errors");
                        for (const auto& error : m_DiagnosticErrors) {
                            ImGui::BulletText("%s", error.c_str());
                        }
                    }
                    if (!m_DiagnosticWarnings.empty()) {
                        if (!m_DiagnosticErrors.empty()) {
                            ImGui::Separator();
                        }
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "Warnings");
                        for (const auto& warning : m_DiagnosticWarnings) {
                            ImGui::BulletText("%s", warning.c_str());
                        }
                    }
                }
                ImGui::EndPopup();
            }

            ImGui::EndChild();
        }

        ImGui::Separator();

        // VIEWPORT / EMPTY STATE
        float fullH = ImGui::GetContentRegionAvail().y;
        if (fullH < 1.0f) fullH = 1.0f;

        if (m_PrefabRoot == INVALID_ENTITY_ID) {
            ImGui::BeginChild("PrefabEmptyState", ImVec2(0, fullH), true);
            ImGui::Dummy(ImVec2(0.0f, fullH * 0.22f));
            ImGui::SetCursorPosX(std::max(12.0f, (ImGui::GetContentRegionAvail().x - 220.0f) * 0.5f));
            ImGui::TextColored(statusColor(), "Prefab Scene Unavailable");
            ImGui::Spacing();
            ImGui::TextWrapped("The prefab could not be loaded into the isolated editor scene. Review diagnostics, then retry loading or locate the source asset.");
            ImGui::Spacing();
            if (ImGui::Button("Retry Load")) {
                if (LoadPrefab(m_PrefabPath)) {
                    RebuildBaseline();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Locate Source") && m_UILayer) {
                std::filesystem::path p(m_PrefabPath);
                m_UILayer->GetProjectPanel().NavigateTo(p.parent_path().string());
            }
            ImGui::EndChild();
        } else {
            ImGui::BeginChild("PrefabViewport", ImVec2(0, fullH), false);
            ImVec2 avail = ImGui::GetContentRegionAvail();
            uint32_t rw = (uint32_t)std::max(1.0f, avail.x);
            uint32_t rh = (uint32_t)std::max(1.0f, avail.y);
            m_ViewportPanel.SetRenderTargetSize(rw, rh);
            if (Camera* cam = m_ViewportPanel.GetPanelCamera()) {
                cam->SetViewportSize((float)rw, (float)rh);
            }
            m_Scene.UpdateTransforms();
            ecs::ParticleEmitterSystem::Get().SyncEmittersOnly(m_Scene);
            bgfx::TextureHandle tex = Renderer::Get().RenderSceneToTexture(&m_Scene, rw, rh, m_ViewportPanel.GetPanelCamera(), m_ViewIdBase);
            m_ViewportPanel.OnImGuiRenderEmbedded(tex, "PrefabViewportImage");
            ImGui::EndChild();
        }
    }

    if (requestClose) {
        if (m_IsDirty) {
            m_PendingCloseConfirmation = true;
        } else {
            m_IsOpen = false;
        }
    }

    if (m_PendingCloseConfirmation) {
        ImGui::OpenPopup("Discard Prefab Changes?");
        m_PendingCloseConfirmation = false;
    }

    if (ImGui::BeginPopupModal("Discard Prefab Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("This prefab has unsaved changes. Close the panel and discard them, or save before closing?");
        ImGui::Spacing();

        if (ImGui::Button("Save and Close", ImVec2(140.0f, 0.0f))) {
            ComputeDeltas();
            if (SavePrefab()) {
                m_IsOpen = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(100.0f, 0.0f))) {
            m_IsOpen = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}

// Initialize camera to focus on prefab AABB bounds
void PrefabEditorPanel::InitializeCameraToBounds() {
    if (m_PrefabRoot == -1) return;
    
    Camera* cam = m_ViewportPanel.GetPanelCamera();
    if (!cam) return;
    
    // Update transforms to ensure world-space bounds are accurate
    m_Scene.UpdateTransforms();
    
    // Compute world-space AABB by traversing all entities with mesh components
    glm::vec3 bbMin(std::numeric_limits<float>::max());
    glm::vec3 bbMax(std::numeric_limits<float>::lowest());
    bool hasAnyBounds = false;
    
    std::function<void(EntityID)> visit = [&](EntityID id) {
        auto* d = m_Scene.GetEntityData(id);
        if (!d || !d->Visible || !d->Active) return;
        
        // Check for mesh component with valid bounds
        if (d->Mesh && d->Mesh->mesh) {
            const glm::vec3 lmin = d->Mesh->mesh->BoundsMin;
            const glm::vec3 lmax = d->Mesh->mesh->BoundsMax;
            
            // Validate bounds (some meshes may have invalid/empty bounds)
            if (lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z) {
                // Transform local AABB corners to world space
                const glm::mat4& M = d->Transform.WorldMatrix;
                const glm::vec3 corners[8] = {
                    {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z},
                    {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
                    {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z},
                    {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
                };
                for (const auto& c : corners) {
                    glm::vec3 w = glm::vec3(M * glm::vec4(c, 1.0f));
                    bbMin = glm::min(bbMin, w);
                    bbMax = glm::max(bbMax, w);
                }
                hasAnyBounds = true;
            }
        }
        
        // Recurse to children
        for (EntityID child : d->Children) {
            visit(child);
        }
    };
    
    visit(m_PrefabRoot);
    
    // Fallback: use collider bounds if no mesh bounds found
    if (!hasAnyBounds) {
        std::function<void(EntityID)> visitColliders = [&](EntityID id) {
            auto* d = m_Scene.GetEntityData(id);
            if (!d || !d->Visible || !d->Active) return;
            
            if (d->Collider) {
                const glm::mat4& M = d->Transform.WorldMatrix;
                glm::vec3 worldCenter = glm::vec3(M[3]);
                glm::vec3 scale = glm::abs(d->Transform.Scale);
                glm::vec3 extents(0.0f);
                
                switch (d->Collider->ShapeType) {
                    case ColliderShape::Box: {
                        extents = d->Collider->Size * 0.5f;
                        extents *= scale;
                        break;
                    }
                    case ColliderShape::Sphere: {
                        float r = d->Collider->Radius * std::max(scale.x, std::max(scale.y, scale.z));
                        extents = glm::vec3(r);
                        break;
                    }
                    case ColliderShape::Capsule: {
                        float r = d->Collider->Radius * std::max(scale.x, scale.z);
                        float h = d->Collider->Height * 0.5f * scale.y;
                        extents = glm::vec3(r, h + r, r);
                        break;
                    }
                    default:
                        break;
                }
                
                if (glm::length(extents) > 0.0f) {
                    bbMin = glm::min(bbMin, worldCenter - extents);
                    bbMax = glm::max(bbMax, worldCenter + extents);
                    hasAnyBounds = true;
                }
            }
            
            for (EntityID child : d->Children) {
                visitColliders(child);
            }
        };
        visitColliders(m_PrefabRoot);
    }
    
    // If no mesh bounds found, use default 1x1 cube around center
    if (!hasAnyBounds) {
        auto* rootData = m_Scene.GetEntityData(m_PrefabRoot);
        glm::vec3 center = rootData ? glm::vec3(rootData->Transform.WorldMatrix[3]) : glm::vec3(0.0f);
        bbMin = center - glm::vec3(0.5f);
        bbMax = center + glm::vec3(0.5f);
        hasAnyBounds = true;
    }
    
    if (!hasAnyBounds) return;
    
    // Compute center and size
    glm::vec3 center = (bbMin + bbMax) * 0.5f;
    glm::vec3 extents = (bbMax - bbMin) * 0.5f;
    
    // Compute distance to frame the bounding box
    // Use vertical FOV to compute distance that frames the bounding sphere
    float radius = glm::length(extents);
    float fovDeg = cam->GetFieldOfView();
    float fovRad = glm::radians(fovDeg);
    
    // Distance to frame the bounding sphere with padding
    float distance = radius / std::tan(fovRad * 0.5f);
    distance *= 1.5f; // Add padding
    
    // Clamp to reasonable range
    distance = glm::clamp(distance, 1.0f, 1000.0f);
    
    // Position camera looking at center from a reasonable angle
    // Use default orbit camera angles (45 degrees pitch, 45 degrees yaw)
    float yaw = 45.0f;
    float pitch = 45.0f;
    
    glm::vec3 dir;
    dir.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    dir.y = sin(glm::radians(pitch));
    dir.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    dir = glm::normalize(dir);
    
    glm::vec3 camPos = center - dir * distance;
    
    // Update viewport panel camera state
    ViewportPanel::ViewportCameraState state;
    state.Target = center;
    state.Distance = distance;
    state.Yaw = yaw;
    state.Pitch = pitch;
    state.FieldOfView = fovDeg;
    state.NearClip = cam->GetNearClip();
    state.FarClip = cam->GetFarClip();
    
    m_ViewportPanel.ApplyCameraState(state);
}

// Ensure there is editor-only lighting without serializing it into the prefab
void PrefabEditorPanel::EnsureEditorLighting() {
    // Soften ambient to make untextured assets visible
    Environment& env = m_Scene.GetEnvironment();
    env.Ambient = Environment::AmbientMode::FlatColor;
    env.AmbientColor = glm::vec3(0.6f, 0.6f, 0.6f);
    env.AmbientIntensity = 1.0f;
    env.UseSkybox = false;

    // Add a single directional light if none exists
    bool hasAnyLight = false;
    for (const auto& e : m_Scene.GetEntities()) {
        if (auto* d = m_Scene.GetEntityData(e.GetID()); d && d->Light) { hasAnyLight = true; break; }
    }
    if (!hasAnyLight) {
        Entity light = m_Scene.CreateEntityExact("__EditorLight");
        m_EditorLight = light.GetID();
        if (auto* d = m_Scene.GetEntityData(m_EditorLight)) {
            d->Light = std::make_unique<LightComponent>(LightType::Directional, glm::vec3(1.0f), 1.0f);
            d->Transform.Position = glm::vec3(3.0f, 5.0f, 3.0f);
            d->Transform.Rotation = glm::vec3(-45.0f, 45.0f, 0.0f);
        }
    }
}

static std::string PackGuid(const ClaymoreGUID& g) {
    return g.ToString();
}

void PrefabEditorPanel::RebuildBaseline() {
    m_BaselineGuids.clear();
    m_BaselineNames.clear();
    m_BaselinePaths.clear();
    m_BaselineTransforms.clear();
    m_BaselineComponents.clear();
    m_AddedDescriptions.clear();
    m_RemovedDescriptions.clear();
    m_ModifiedDescriptions.clear();
    m_AddedGUIDs.clear();
    m_ModifiedGUIDs.clear();
    
    if (m_PrefabRoot == -1) return;
    
    // Capture current scene state as "saved baseline" for prefab delta detection
    std::function<void(EntityID, const std::string&)> dfs = [&](EntityID id, const std::string& path) {
        auto* d = m_Scene.GetEntityData(id);
        if (!d) return;
        std::string guidKey = PackGuid(d->EntityGuid);
        m_BaselineGuids.insert(guidKey);
        m_BaselineNames[guidKey] = d->Name;
        m_BaselinePaths[guidKey] = path;
        m_BaselineTransforms[guidKey] = { d->Transform.Position, d->Transform.Rotation, d->Transform.Scale };
        m_BaselineComponents[guidKey] = CaptureComponentSignature(m_Scene, id);
        
        for (EntityID c : d->Children) {
            auto* cd = m_Scene.GetEntityData(c);
            if (!cd) continue;
            std::string childPath = path.empty() ? cd->Name : (path + "/" + cd->Name);
            dfs(c, childPath);
        }
    };
    
    auto* rd = m_Scene.GetEntityData(m_PrefabRoot);
    std::string rootPath = rd ? rd->Name : std::string("<root>");
    dfs(m_PrefabRoot, rootPath);
    
    // Mark deltas as stale so they get recomputed
    m_DeltasStale = true;
    m_LastDeltaSceneRevision = m_Scene.GetDirtyRevision();
    m_DeltaFrameCounter = 0;
}

void PrefabEditorPanel::ComputeDeltas() {
    m_AddedDescriptions.clear();
    m_RemovedDescriptions.clear();
    m_ModifiedDescriptions.clear();
    m_AddedGUIDs.clear();
    m_ModifiedGUIDs.clear();
    m_DeltasStale = false;
    if (m_PrefabRoot == -1) return;
    
    // Compare against saved baseline
    std::unordered_set<std::string> cur;
    std::unordered_map<std::string, std::string> curNames;
    std::unordered_map<std::string, std::string> curPaths;
    std::unordered_map<std::string, BaselineTransform> curTransforms;
    std::unordered_map<std::string, nlohmann::json> curComponents;
    std::unordered_map<std::string, std::vector<std::string>> modificationReasons;
    auto noteChange = [&](const std::string& guid, const std::string& reason){
        if (guid.empty() || reason.empty()) return;
        auto& vec = modificationReasons[guid];
        if (std::find(vec.begin(), vec.end(), reason) == vec.end()) vec.push_back(reason);
    };
    std::function<void(EntityID, const std::string&)> dfs = [&](EntityID id, const std::string& path){
        auto* d = m_Scene.GetEntityData(id); if (!d) return;
        std::string key = PackGuid(d->EntityGuid);
        cur.insert(key);
        curNames[key] = d->Name;
        curPaths[key] = path;
        curTransforms[key] = { d->Transform.Position, d->Transform.Rotation, d->Transform.Scale };
        curComponents[key] = CaptureComponentSignature(m_Scene, id);
        for (EntityID c : d->Children) {
            auto* cd = m_Scene.GetEntityData(c); if (!cd) continue;
            std::string childPath = path.empty() ? cd->Name : (path + "/" + cd->Name);
            dfs(c, childPath);
        }
    };
    auto* rd = m_Scene.GetEntityData(m_PrefabRoot);
    std::string rootPath = rd ? rd->Name : std::string("<root>");
    dfs(m_PrefabRoot, rootPath);

    // Additions
    for (const auto& g : cur) {
        if (m_BaselineGuids.find(g) == m_BaselineGuids.end()) {
            const std::string& path = curPaths[g];
            m_AddedDescriptions.push_back("+ " + path);
            m_AddedGUIDs.insert(g);
        }
    }
    // Deletions
    for (const auto& g : m_BaselineGuids) {
        if (cur.find(g) == cur.end()) {
            const std::string& path = m_BaselinePaths.at(g);
            m_RemovedDescriptions.push_back("- " + path);
        }
    }

    // Modifications: detect transform changes for nodes present in both
    for (const auto& g : cur) {
        if (m_BaselineGuids.find(g) != m_BaselineGuids.end()) {
            auto itB = m_BaselineTransforms.find(g);
            auto itC = curTransforms.find(g);
            if (itB != m_BaselineTransforms.end() && itC != curTransforms.end()) {
                auto almostEqual = [](const glm::vec3& a, const glm::vec3& b){ return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(1e-4f))); };
                if (!almostEqual(itB->second.pos, itC->second.pos) || !almostEqual(itB->second.rot, itC->second.rot) || !almostEqual(itB->second.scale, itC->second.scale)) {
                    noteChange(g, "transform");
                }
            }
        }
    }
    for (const auto& g : cur) {
        if (m_BaselineGuids.find(g) == m_BaselineGuids.end()) continue;
        auto itBase = m_BaselineComponents.find(g);
        auto itCur = curComponents.find(g);
        nlohmann::json baseSig = (itBase != m_BaselineComponents.end()) ? itBase->second : nlohmann::json::object();
        nlohmann::json curSig = (itCur != curComponents.end()) ? itCur->second : nlohmann::json::object();
        
        std::unordered_set<std::string> allKeys;
        allKeys.reserve(kDeltaComponentKeys.size() + baseSig.size() + curSig.size());
        for (const auto& key : kDeltaComponentKeys) allKeys.insert(key);
        for (auto it = baseSig.begin(); it != baseSig.end(); ++it) allKeys.insert(it.key());
        for (auto it = curSig.begin(); it != curSig.end(); ++it) allKeys.insert(it.key());

        for (const auto& key : allKeys) {
            nlohmann::json baseVal = baseSig.value(key, nlohmann::json());
            nlohmann::json curVal = curSig.value(key, nlohmann::json());
            if (baseVal != curVal) {
                noteChange(g, key);
            }
        }
    }
    // Build modification descriptions with reasons, prioritizing current path fallback to baseline path
    for (const auto& kv : modificationReasons) {
        const std::string& guid = kv.first;
        m_ModifiedGUIDs.insert(guid);  // Track modified GUID for hierarchy coloring
        auto itPath = curPaths.find(guid);
        std::string path = (itPath != curPaths.end()) ? itPath->second : (m_BaselinePaths.count(guid) ? m_BaselinePaths.at(guid) : std::string("<unknown>"));
        std::string entry = "~ " + path;
        if (!kv.second.empty()) {
            entry += " (";
            for (size_t i = 0; i < kv.second.size(); ++i) {
                entry += kv.second[i];
                if (i + 1 < kv.second.size()) entry += ", ";
            }
            entry += ")";
        }
        m_ModifiedDescriptions.push_back(std::move(entry));
    }

    m_IsDirty = (!m_AddedDescriptions.empty() || !m_RemovedDescriptions.empty() || !m_ModifiedDescriptions.empty());
    m_LastDeltaSceneRevision = m_Scene.GetDirtyRevision();
    m_DeltaFrameCounter = 0;
}

void PrefabEditorPanel::OnAssetEvent(int event, const std::string& path, ClaymoreGUID guid) {
    AssetEvent assetEvent = static_cast<AssetEvent>(event);
    
    // Only handle reimport events for models
    if (assetEvent != AssetEvent::Reimported) return;
    
    // Check if this prefab depends on the changed model
    if (!DependsOnModel(path, guid)) return;
    
    std::cout << "[PrefabEditor] Model dependency changed: " << path << std::endl;
    
    // Refresh the prefab view
    RefreshFromModelChange(path, guid);
}

bool PrefabEditorPanel::DependsOnModel(const std::string& modelPath, ClaymoreGUID modelGuid) const {
    if ((modelGuid.high != 0 || modelGuid.low != 0) &&
        m_DependentModelAssetGuids.count(PackGuidStable(modelGuid)) > 0) {
        return true;
    }

    const std::string normalizedModelPath = NormalizeAssetPath(modelPath);
    if (!normalizedModelPath.empty() && m_DependentModelPaths.count(normalizedModelPath) > 0) {
        return true;
    }

    // Quick check using cached GUIDs
    uint64_t packedGuid = PackGuidStable(modelGuid);
    if (m_DependentModelGuids.count(packedGuid) > 0) {
        return true;
    }
    
    // Full scan: check if any entity in the prefab scene references this model
    for (const auto& e : m_Scene.GetEntities()) {
        const auto* d = const_cast<Scene&>(m_Scene).GetEntityData(e.GetID());
        if (!d) continue;
        
        if (d->Mesh && d->Mesh->meshReference.IsValid()) {
            if (d->Mesh->meshReference.guid == modelGuid) {
                return true;
            }
            
            // Also check by path
            std::string meshPath = AssetLibrary::Instance().GetPathForGUID(d->Mesh->meshReference.guid);
            if (!meshPath.empty()) {
                // Normalize paths for comparison
                std::string normModelPath = normalizedModelPath.empty() ? NormalizeAssetPath(modelPath) : normalizedModelPath;
                meshPath = NormalizeAssetPath(meshPath);
                
                // Check if this mesh belongs to the model
                const std::string modelStem = fs::path(normModelPath).stem().string();
                if (!modelStem.empty() && meshPath.find(modelStem) != std::string::npos) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

void PrefabEditorPanel::RefreshFromModelChange(const std::string& modelPath, ClaymoreGUID modelGuid) {
    if (m_PrefabRoot == INVALID_ENTITY_ID) return;

    SetStatusMessage("Refreshing from model update...", StatusLevel::Info);
    std::cout << "[PrefabEditor] Refreshing prefab from model change: " << modelPath
              << " (" << modelGuid.ToString() << ")\n";

    PrefabAsset currentState;
    if (!prefab_editor::BuildPrefabAssetFromScene(m_Scene, m_PrefabRoot, currentState)) {
        SetStatusMessage("Failed to capture current prefab state before refresh.", StatusLevel::Error);
        std::cerr << "[PrefabEditor] Failed to capture current state for refresh\n";
        return;
    }

    const auto savedAddedDescriptions = m_AddedDescriptions;
    const auto savedRemovedDescriptions = m_RemovedDescriptions;
    const auto savedModifiedDescriptions = m_ModifiedDescriptions;
    const auto savedAddedGuids = m_AddedGUIDs;
    const bool savedDirty = m_IsDirty;
    const auto savedDependentModelGuids = m_DependentModelGuids;
    const auto savedDependentModelAssetGuids = m_DependentModelAssetGuids;
    const auto savedDependentModelPaths = m_DependentModelPaths;

    ClaymoreGUID selectedAuthoredGuid{};
    prefab::PropertyOverride::ResolutionHints selectedHints;
    if (m_SelectedEntity != INVALID_ENTITY_ID && m_SelectedEntity != m_EditorLight) {
        if (EntityData* selectedData = m_Scene.GetEntityData(m_SelectedEntity)) {
            selectedAuthoredGuid = selectedData->EntityGuid;
            selectedHints = BuildResolutionHints(m_Scene, m_PrefabRoot, m_SelectedEntity);
            if (EntityData* rootData = m_Scene.GetEntityData(m_PrefabRoot);
                rootData && rootData->PrefabInstance) {
                auto remap = rootData->PrefabInstance->InstanceToPrefabGuid.find(selectedAuthoredGuid);
                if (remap != rootData->PrefabInstance->InstanceToPrefabGuid.end()) {
                    selectedAuthoredGuid = remap->second;
                }
            }
        }
    }

    PrefabAsset baselineAsset;
    if (!PrefabIO::LoadPrefabSource(m_PrefabPath, baselineAsset)) {
        SetStatusMessage("Failed to load prefab from disk for refresh.", StatusLevel::Error);
        std::cerr << "[PrefabEditor] Failed to load baseline prefab for refresh\n";
        return;
    }

    const auto userOverrides = prefab_editor::ComputeOverrides(baselineAsset, m_Scene, m_PrefabRoot);
    const auto addedEntities = prefab_editor::GetAddedEntities(baselineAsset, m_Scene, m_PrefabRoot);
    auto removedEntities = prefab_editor::GetRemovedEntities(baselineAsset, m_Scene, m_PrefabRoot);

    std::vector<prefab::PropertyOverride> componentOverrides;
    std::vector<prefab::PropertyOverride> parentOverrides;
    componentOverrides.reserve(userOverrides.size());
    parentOverrides.reserve(userOverrides.size());
    for (const auto& ov : userOverrides) {
        if (ov.ComponentKey == "parent") {
            parentOverrides.push_back(ov);
        } else {
            componentOverrides.push_back(ov);
        }
    }

    auto restoreCurrentScene = [&]() -> bool {
        ClearLoadedPrefabScene(true);
        m_DependentModelGuids = savedDependentModelGuids;
        m_DependentModelAssetGuids = savedDependentModelAssetGuids;
        m_DependentModelPaths = savedDependentModelPaths;
        m_PrefabAssetGuid = currentState.Guid;
        m_PrefabRootGuid = currentState.RootGuid;
        RefreshDiagnostics(&currentState);

        EntityID restoredRoot = InstantiatePrefabAsset(currentState, m_Scene, INVALID_ENTITY_ID, false, m_PrefabPath.c_str());
        if (restoredRoot == INVALID_ENTITY_ID || restoredRoot == 0) {
            return false;
        }

        m_PrefabRoot = restoredRoot;
        EnsureEditorLighting();
        m_Scene.MarkTransformDirty(m_PrefabRoot);
        m_Scene.ResolveScriptEntityReferencesFromMetadata();
        m_Scene.UpdateTransforms();
        InitializeCameraToBounds();

        RefreshLookup restoreLookup;
        if (EntityData* restoredRootData = m_Scene.GetEntityData(m_PrefabRoot);
            restoredRootData && restoredRootData->PrefabInstance) {
            restoreLookup.PrefabToInstanceGuid = &restoredRootData->PrefabInstance->PrefabToInstanceGuid;
        }
        BuildRefreshLookup(m_Scene, m_PrefabRoot, restoreLookup);
        EntityID restoredSelection = ResolveEntity(restoreLookup, selectedAuthoredGuid, selectedHints);
        m_SelectedEntity = (restoredSelection != INVALID_ENTITY_ID) ? restoredSelection : m_PrefabRoot;

        RebuildBaseline();
        m_AddedDescriptions = savedAddedDescriptions;
        m_RemovedDescriptions = savedRemovedDescriptions;
        m_ModifiedDescriptions = savedModifiedDescriptions;
        m_AddedGUIDs = savedAddedGuids;
        m_ModifiedGUIDs.clear();
        m_IsDirty = savedDirty || !savedAddedDescriptions.empty() || !savedRemovedDescriptions.empty() || !savedModifiedDescriptions.empty();
        m_DeltasStale = false;
        m_LastDeltaSceneRevision = m_Scene.GetDirtyRevision();
        m_DeltaFrameCounter = 0;
        return true;
    };

    if (!LoadPrefab(m_PrefabPath)) {
        if (restoreCurrentScene()) {
            SetStatusMessage("Model refresh failed. Restored the unsaved editor scene.", StatusLevel::Error);
        } else {
            SetStatusMessage("Model refresh failed and editor state could not be restored.", StatusLevel::Error);
        }
        return;
    }

    RebuildBaseline();
    if (m_PrefabRoot == INVALID_ENTITY_ID) {
        SetStatusMessage("Prefab refresh completed without a valid root entity.", StatusLevel::Error);
        return;
    }

    RefreshLookup lookup;
    if (EntityData* rootData = m_Scene.GetEntityData(m_PrefabRoot);
        rootData && rootData->PrefabInstance) {
        lookup.PrefabToInstanceGuid = &rootData->PrefabInstance->PrefabToInstanceGuid;
    }
    BuildRefreshLookup(m_Scene, m_PrefabRoot, lookup);

    std::sort(removedEntities.begin(), removedEntities.end(),
              [](const prefab::RemovedEntity& a, const prefab::RemovedEntity& b) {
                  return std::count(a.NodePath.begin(), a.NodePath.end(), '/') >
                         std::count(b.NodePath.begin(), b.NodePath.end(), '/');
              });

    int removedCount = 0;
    int addedCount = 0;
    int appliedOverrideCount = 0;
    int skippedCount = 0;

    for (const auto& removed : removedEntities) {
        EntityID targetId = ResolveEntity(lookup, removed.Guid, BuildResolutionHintsFromRemoved(removed));
        if (targetId == INVALID_ENTITY_ID || targetId == m_PrefabRoot) {
            ++skippedCount;
            continue;
        }
        if (!m_Scene.GetEntityData(targetId)) {
            ++skippedCount;
            continue;
        }
        m_Scene.RemoveEntity(targetId);
        ++removedCount;
    }

    BuildRefreshLookup(m_Scene, m_PrefabRoot, lookup);

    std::function<int(const prefab::AddedEntity&, EntityID)> instantiateAddedSubtree =
        [&](const prefab::AddedEntity& added, EntityID parentId) -> int {
            if (parentId == INVALID_ENTITY_ID) {
                return 0;
            }

            nlohmann::json entityJson = added.Components;
            entityJson.erase("id");
            entityJson.erase("parent");
            entityJson.erase("children");
            entityJson.erase("prefabInstance");
            entityJson.erase("prefabGuid");
            entityJson.erase("prefabSource");

            EntityID newId = Serializer::DeserializeEntity(entityJson, m_Scene);
            if (newId == INVALID_ENTITY_ID || newId == 0) {
                return 0;
            }

            m_Scene.SetParent(newId, parentId, false);
            if (lookup.PrefabToInstanceGuid) {
                RemapSubtreeAuthoringIdentity(m_Scene, newId, *lookup.PrefabToInstanceGuid);
            }

            int createdCount = 1;
            for (const auto& child : added.Children) {
                createdCount += instantiateAddedSubtree(child, newId);
            }
            return createdCount;
        };

    for (const auto& added : addedEntities) {
        EntityID parentId = ResolveEntity(lookup, added.ParentGuid, BuildResolutionHintsFromAddedParent(added));
        if (parentId == INVALID_ENTITY_ID) {
            ++skippedCount;
            continue;
        }

        const int createdCount = instantiateAddedSubtree(added, parentId);
        if (createdCount == 0) {
            ++skippedCount;
            continue;
        }
        addedCount += createdCount;
    }

    BuildRefreshLookup(m_Scene, m_PrefabRoot, lookup);

    std::vector<prefab::PropertyOverride> batchedComponentOverrides;
    batchedComponentOverrides.reserve(componentOverrides.size());

    for (const auto& ov : componentOverrides) {
        EntityID targetId = ResolveEntity(lookup, ov.TargetEntityGuid, ov.Hints);
        EntityData* entityData = m_Scene.GetEntityData(targetId);
        if (!entityData) {
            ++skippedCount;
            continue;
        }

        if (ov.ComponentKey == "name") {
            if (!ov.Value.is_string()) {
                ++skippedCount;
                continue;
            }
            entityData->Name = ov.Value.get<std::string>();
            ++appliedOverrideCount;
            continue;
        }

        if (ov.ComponentKey == "active") {
            if (!ov.Value.is_boolean()) {
                ++skippedCount;
                continue;
            }
            entityData->Active = ov.Value.get<bool>();
            ++appliedOverrideCount;
            continue;
        }

        if (ov.ComponentKey == "visible") {
            if (!ov.Value.is_boolean()) {
                ++skippedCount;
                continue;
            }
            entityData->Visible = ov.Value.get<bool>();
            ++appliedOverrideCount;
            continue;
        }

        if (ov.ComponentKey == "transform") {
            if (!ov.Value.is_object()) {
                ++skippedCount;
                continue;
            }
            Serializer::DeserializeTransform(ov.Value, entityData->Transform);
            entityData->Transform.TransformDirty = true;
            ++appliedOverrideCount;
            continue;
        }

        prefab::PropertyOverride remapped = ov;
        remapped.TargetEntityGuid = entityData->EntityGuid;
        batchedComponentOverrides.push_back(std::move(remapped));
    }

    if (!batchedComponentOverrides.empty()) {
        ApplyPrefabOverrides(m_PrefabRoot, m_Scene, batchedComponentOverrides);
        appliedOverrideCount += (int)batchedComponentOverrides.size();
    }

    for (const auto& ov : parentOverrides) {
        EntityID targetId = ResolveEntity(lookup, ov.TargetEntityGuid, ov.Hints);
        if (targetId == INVALID_ENTITY_ID) {
            ++skippedCount;
            continue;
        }

        ClaymoreGUID desiredParentGuid{};
        prefab::PropertyOverride::ResolutionHints parentHints;
        EntityID parentId = INVALID_ENTITY_ID;
        if (ov.Value.is_object()) {
            if (ov.Value.contains("guid")) {
                try { ov.Value.at("guid").get_to(desiredParentGuid); } catch (...) {}
            }
            if (ov.Value.contains("hints")) {
                parentHints = BuildResolutionHintsFromJson(ov.Value["hints"]);
            }
            parentId = ResolveEntity(lookup, desiredParentGuid, parentHints);
            if ((desiredParentGuid.high != 0 || desiredParentGuid.low != 0) && parentId == INVALID_ENTITY_ID) {
                ++skippedCount;
                continue;
            }
        } else if (!ov.Value.is_null()) {
            ++skippedCount;
            continue;
        }

        if (targetId == m_PrefabRoot) {
            continue;
        }
        if (parentId == targetId) {
            ++skippedCount;
            continue;
        }

        bool cycleDetected = false;
        for (EntityID cursor = parentId; cursor != INVALID_ENTITY_ID; ) {
            if (cursor == targetId) {
                cycleDetected = true;
                break;
            }
            EntityData* cursorData = m_Scene.GetEntityData(cursor);
            cursor = cursorData ? cursorData->Parent : INVALID_ENTITY_ID;
        }
        if (cycleDetected) {
            ++skippedCount;
            continue;
        }

        m_Scene.SetParent(targetId, parentId, false);
        ++appliedOverrideCount;
    }

    BuildRefreshLookup(m_Scene, m_PrefabRoot, lookup);
    EntityID refreshedSelection = ResolveEntity(lookup, selectedAuthoredGuid, selectedHints);
    m_SelectedEntity = (refreshedSelection != INVALID_ENTITY_ID) ? refreshedSelection : m_PrefabRoot;

    m_Scene.ResolveScriptEntityReferencesFromMetadata();
    m_Scene.MarkTransformDirty(m_PrefabRoot);
    m_Scene.UpdateTransforms();
    InitializeCameraToBounds();

    if (!m_InPlayMode) {
        ComputeDeltas();
    } else {
        m_IsDirty = !userOverrides.empty() || !addedEntities.empty() || !removedEntities.empty();
        MarkDeltasStale();
        m_LastDeltaSceneRevision = m_Scene.GetDirtyRevision();
        m_DeltaFrameCounter = 0;
    }

    if (skippedCount > 0) {
        SetStatusMessage("Model update applied with partial remapping. Review diagnostics and change summary.", StatusLevel::Warning);
    } else if (!userOverrides.empty() || !addedEntities.empty() || !removedEntities.empty()) {
        SetStatusMessage("Model update applied and unsaved edits were preserved.", StatusLevel::Info);
    } else {
        SetStatusMessage("Model update applied.", StatusLevel::Info);
    }

    std::cout << "[PrefabEditor] Refresh complete: removed=" << removedCount
              << ", added=" << addedCount
              << ", overrides=" << appliedOverrideCount
              << ", skipped=" << skippedCount << std::endl;
}
