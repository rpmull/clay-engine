#include "ModelImportCache.h"
#include "ModelNodeIdentity.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <iostream>
#include <mutex>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <limits>
#include <cctype>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include "editor/import/ModelLoader.h"
#include "editor/import/ModelPreprocessor.h"
#include "core/animation/AvatarDefinition.h"
#include "editor/pipeline/AssetLibrary.h"
#include "editor/pipeline/MaterialSourceSerialization.h"
#include "editor/pipeline/EmbeddedTextureCache.h"
#include "editor/Project.h"
#include "core/vfs/FileSystem.h"
#include "SkelBin.h"
#include "MeshBin.h"

namespace {
    using json = nlohmann::json;
    std::mutex g_modelCacheMutex;
    constexpr int kCurrentModelMetaVersion = 9;

    struct PreservedMaterialEntry {
        std::vector<MaterialSource> Materials;
        std::vector<std::string> SlotNames;
    };

    static std::string ToForwardSlashes(std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
    }

    static std::string ToLowerCopy(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    static std::string NormalizeTexturePathForCompare(const std::string& path) {
        return ToLowerCopy(ToForwardSlashes(IVirtualFS::NormalizePath(path)));
    }

    static std::filesystem::path ResolveProjectTexturePath(const std::string& path) {
        if (path.empty()) return {};

        std::filesystem::path candidate(ToForwardSlashes(IVirtualFS::NormalizePath(path)));
        if (candidate.is_absolute()) {
            return candidate;
        }

        try {
            std::filesystem::path project = Project::GetProjectDirectory();
            if (!project.empty()) {
                return project / candidate;
            }
        } catch (...) {}

        return candidate;
    }

    static bool TexturePathExists(const std::string& path) {
        std::filesystem::path resolved = ResolveProjectTexturePath(path);
        return !resolved.empty() && std::filesystem::exists(resolved);
    }

    static std::filesystem::file_time_type TexturePathLastWriteTime(const std::string& path) {
        std::filesystem::path resolved = ResolveProjectTexturePath(path);
        if (resolved.empty()) {
            return std::filesystem::file_time_type::min();
        }

        std::error_code ec;
        std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(resolved, ec);
        return ec ? std::filesystem::file_time_type::min() : timestamp;
    }

    static bool HasNumericDuplicateTextureSuffix(const std::string& filename) {
        std::string stem = std::filesystem::path(filename).stem().string();
        size_t dotPos = stem.find_last_of('.');
        if (dotPos == std::string::npos || dotPos + 1 >= stem.size()) {
            return false;
        }

        std::string suffix = stem.substr(dotPos + 1);
        if (suffix.size() < 3 || suffix.size() > 4) {
            return false;
        }

        return std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        });
    }

    static bool IsManagedTexturePath(const std::string& path) {
        std::string normalized = NormalizeTexturePathForCompare(path);
        return normalized.rfind("assets/textures/shared/", 0) == 0 ||
               normalized.rfind("resources/textures/", 0) == 0;
    }

    static bool IsModelScopedTexturePath(const std::string& path, const std::string& modelName) {
        if (path.empty() || modelName.empty()) {
            return false;
        }

        std::string normalized = NormalizeTexturePathForCompare(path);
        std::string scopedPrefix = "assets/textures/" + ToLowerCopy(modelName) + "/";
        return normalized.find(scopedPrefix) != std::string::npos;
    }

    static bool IsWeakImportedTexturePath(const std::string& path, const std::string& modelName) {
        if (path.empty()) {
            return true;
        }

        std::string normalized = NormalizeTexturePathForCompare(path);
        if (normalized.rfind("embedded://", 0) == 0) {
            return true;
        }

        std::filesystem::path rawPath(path);
        if (rawPath.is_absolute() && !IsManagedTexturePath(path)) {
            return true;
        }

        std::string filename = ToLowerCopy(std::filesystem::path(normalized).filename().string());
        if (filename.empty()) {
            return true;
        }

        if (filename.rfind("emb_", 0) == 0 ||
            filename.rfind("embauto_", 0) == 0 ||
            filename.rfind("emb-auto_", 0) == 0) {
            return true;
        }

        if (HasNumericDuplicateTextureSuffix(filename)) {
            return true;
        }

        if (IsModelScopedTexturePath(path, modelName)) {
            return true;
        }

        return !TexturePathExists(path);
    }

    static void PreserveManagedTextureSpecifier(TextureSpecifier& imported,
                                                const TextureSpecifier& existing,
                                                const std::string& modelName) {
        if (existing.Path.empty()) {
            return;
        }
        if (!IsManagedTexturePath(existing.Path)) {
            return;
        }
        if (!TexturePathExists(existing.Path)) {
            return;
        }
        if (!IsWeakImportedTexturePath(imported.Path, modelName)) {
            return;
        }
        if (NormalizeTexturePathForCompare(existing.Path) == NormalizeTexturePathForCompare(imported.Path)) {
            return;
        }

        if (!imported.Path.empty() && TexturePathExists(imported.Path)) {
            std::filesystem::file_time_type importedTime = TexturePathLastWriteTime(imported.Path);
            std::filesystem::file_time_type existingTime = TexturePathLastWriteTime(existing.Path);
            if (importedTime > existingTime) {
                return;
            }
        }

        imported.Path = IVirtualFS::NormalizePath(existing.Path);
        imported.Embedded = EmbeddedTextureData{};
    }

    static int FindPreservedMaterialIndex(const PreservedMaterialEntry& entry,
                                          const std::vector<std::string>& currentSlotNames,
                                          size_t slotIndex,
                                          const std::string& materialName) {
        if (slotIndex < entry.Materials.size()) {
            return static_cast<int>(slotIndex);
        }

        if (slotIndex < currentSlotNames.size() && !entry.SlotNames.empty()) {
            std::string currentSlot = ToLowerCopy(currentSlotNames[slotIndex]);
            for (size_t i = 0; i < entry.SlotNames.size() && i < entry.Materials.size(); ++i) {
                if (ToLowerCopy(entry.SlotNames[i]) == currentSlot) {
                    return static_cast<int>(i);
                }
            }
        }

        if (!materialName.empty()) {
            std::string materialNameLower = ToLowerCopy(materialName);
            int matchIndex = -1;
            for (size_t i = 0; i < entry.Materials.size(); ++i) {
                if (ToLowerCopy(entry.Materials[i].Name) != materialNameLower) {
                    continue;
                }
                if (matchIndex >= 0) {
                    return -1;
                }
                matchIndex = static_cast<int>(i);
            }
            return matchIndex;
        }

        return -1;
    }

    static std::string ToVirtualAssetPath(const std::string& path) {
        if (path.empty()) return {};
        std::string normalized = ToForwardSlashes(path);
        std::filesystem::path candidate = normalized;
        try {
            std::filesystem::path project = Project::GetProjectDirectory();
            if (!project.empty()) {
                std::error_code ec;
                auto rel = std::filesystem::relative(candidate, project, ec);
                if (!ec) candidate = rel;
            }
        } catch (...) {}
        std::string result = ToForwardSlashes(candidate.string());
        size_t pos = result.find("assets/");
        if (pos != std::string::npos) {
            result = result.substr(pos);
        }
        return result;
    }

    static json EncodeTransform(const glm::mat4& m) {
        glm::vec3 t(0.0f), s(1.0f), skew(0.0f);
        glm::vec4 persp(0.0f);
        glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
        glm::decompose(m, s, r, t, skew, persp);
        r = glm::normalize(r);
        json arr = json::array();
        for (int c = 0; c < 4; ++c) {
            for (int rIdx = 0; rIdx < 4; ++rIdx) {
                arr.push_back(m[c][rIdx]);
            }
        }
        return json{
            {"t", { t.x, t.y, t.z }},
            {"s", { s.x, s.y, s.z }},
            {"r", { r.w, r.x, r.y, r.z }},
            {"m", arr}
        };
    }

    static bool WriteMeta(const std::string& sourceModelPath,
                          const BuiltModelPaths& out,
                          const Model& model,
                          const PreparedModel& prepared) {
        try {
            json j;
            
            // CRITICAL: Read existing meta file to preserve user-authored settings
            // that should persist across reimports (import settings, armor mode, etc.)
            json preservedImportSettings;
            bool hasImportSettings = false;
            bool preservedArmorMode = false;
            bool hasArmorMode = false;
            std::unordered_map<std::string, PreservedMaterialEntry> preservedMaterialEntries;
            ModelIdentityMap preservedIdentityMap;
            bool hasPreservedIdentityMap = false;
            try {
                std::ifstream existingMeta(out.metaPath);
                if (existingMeta.is_open()) {
                    json existing;
                    existingMeta >> existing;
                    existingMeta.close();
                    
                    // Preserve importSettings (material presets, scale, tangents, etc.)
                    if (existing.contains("importSettings")) {
                        preservedImportSettings = existing["importSettings"];
                        hasImportSettings = true;
                    }
                    
                    // Preserve armorMode
                    if (existing.contains("armorMode")) {
                        preservedArmorMode = existing["armorMode"].get<bool>();
                        hasArmorMode = true;
                    }

                    if (existing.contains("nodeIdentities") && existing["nodeIdentities"].is_object()) {
                        preservedIdentityMap = ModelIdentityMap::FromJson(existing["nodeIdentities"]);
                        preservedIdentityMap.BuildLookups();
                        hasPreservedIdentityMap = !preservedIdentityMap.Nodes.empty();
                    }

                    if (existing.contains("entries") && existing["entries"].is_array()) {
                        for (const auto& existingEntry : existing["entries"]) {
                            if (!existingEntry.is_object()) {
                                continue;
                            }

                            std::string entryName = existingEntry.value("name", std::string{});
                            if (entryName.empty()) {
                                continue;
                            }

                            PreservedMaterialEntry preservedEntry;
                            if (existingEntry.contains("materials") && existingEntry["materials"].is_array()) {
                                for (const auto& matNode : existingEntry["materials"]) {
                                    if (!matNode.is_object()) {
                                        continue;
                                    }
                                    preservedEntry.Materials.push_back(material_serialization::FromJson(matNode));
                                }
                            }
                            if (existingEntry.contains("slotNames") && existingEntry["slotNames"].is_array()) {
                                for (const auto& slotNode : existingEntry["slotNames"]) {
                                    if (slotNode.is_string()) {
                                        preservedEntry.SlotNames.push_back(slotNode.get<std::string>());
                                    }
                                }
                            }

                            if (!preservedEntry.Materials.empty()) {
                                preservedMaterialEntries[entryName] = std::move(preservedEntry);
                            }
                        }
                    }
                }
            } catch (...) {
                // Failed to read existing meta - that's OK, we'll create fresh
            }
            
            j["version"] = kCurrentModelMetaVersion;

            const std::string sourceVirtual = ToVirtualAssetPath(sourceModelPath);
            const std::string meshVirtual = ToVirtualAssetPath(out.meshPath);
            const std::string skelVirtual = ToVirtualAssetPath(out.skelPath);
            const std::string metaVirtual = ToVirtualAssetPath(out.metaPath);

            const std::string sourcePathForJson = sourceVirtual.empty() ? ToForwardSlashes(sourceModelPath) : sourceVirtual;
            const std::string meshPathForJson = meshVirtual.empty() ? ToForwardSlashes(out.meshPath) : meshVirtual;
            const std::string skelPathForJson = skelVirtual.empty() ? ToForwardSlashes(out.skelPath) : skelVirtual;

            j["source"] = sourcePathForJson;
            j["meshbin"] = meshPathForJson;
            j["skeleton"] = skelPathForJson;
            j["rootTransform"] = EncodeTransform(prepared.RootLocal);

            std::string modelNameHint = model.ModelName;
            if (modelNameHint.empty()) {
                modelNameHint = std::filesystem::path(sourceModelPath).stem().string();
            }

            // GUID / asset registration
            AssetLibrary& library = AssetLibrary::Instance();
            const std::string guidLookup = !metaVirtual.empty() ? metaVirtual : sourcePathForJson;
            ClaymoreGUID guid = library.GetGUIDForPath(guidLookup);
            if (guid.high == 0 && guid.low == 0) {
                guid = ClaymoreGUID::Generate();
                AssetReference ref(guid, 0, static_cast<int32_t>(AssetType::Mesh));
                std::string registerPath = metaVirtual.empty() ? ToForwardSlashes(out.metaPath) : metaVirtual;
                library.RegisterAsset(ref, AssetType::Mesh, registerPath, modelNameHint);
                library.RegisterPathAlias(guid, registerPath);
            }
            library.RegisterPathAlias(guid, sourcePathForJson);
            library.RegisterPathAlias(guid, meshPathForJson);
            library.RegisterPathAlias(guid, skelPathForJson);
            library.RegisterPathAlias(guid, ToForwardSlashes(sourceModelPath));
            library.RegisterPathAlias(guid, ToForwardSlashes(out.meshPath));
            library.RegisterPathAlias(guid, ToForwardSlashes(out.skelPath));
            if (!metaVirtual.empty()) library.RegisterPathAlias(guid, metaVirtual);
            j["guid"] = guid.ToString();

            auto matrixToFloats = [](const glm::mat4& m) -> std::array<float, 16> {
                std::array<float, 16> arr{};
                for (int c = 0; c < 4; ++c) {
                    for (int r = 0; r < 4; ++r) {
                        arr[c * 4 + r] = m[c][r];
                    }
                }
                return arr;
            };

            auto buildMeshIdentity = [&](const auto& meshEntry, size_t meshIndex) {
                ModelNodeIdentity identity;
                identity.NodePath = meshEntry.NodeName;
                identity.NormalizedPath = ModelNodeIdentity::NormalizePath(meshEntry.NodeName);
                identity.NodeName = meshEntry.NodeName;
                identity.NormalizedName = ModelNodeIdentity::NormalizeName(meshEntry.NodeName);
                identity.MeshFileId = meshEntry.SourceMeshIndices.empty()
                    ? static_cast<int>(meshIndex)
                    : meshEntry.SourceMeshIndices.front();
                identity.Skinned = meshEntry.Skinned;
                identity.Depth = 1;

                auto matFloats = matrixToFloats(meshEntry.LocalTransform);
                int vertexHint = meshEntry.MeshData ? static_cast<int>(meshEntry.MeshData->Vertices.size()) : 0;
                identity.ContentHash = ModelNodeIdentity::ComputeContentHash(
                    meshEntry.SourceMeshIndices,
                    matFloats.data(),
                    vertexHint);
                identity.DerivedGUID = ModelNodeIdentity::GenerateDerivedGUID(
                    identity.NodePath,
                    identity.ContentHash,
                    guid);
                return identity;
            };

            // Mesh entries
            j["entries"] = json::array();
            for (size_t i = 0; i < prepared.Meshes.size(); ++i) {
                const auto& entry = prepared.Meshes[i];
                json e;
                e["name"] = entry.NodeName;
                e["meshIndex"] = static_cast<int>(i);
                e["skinned"] = entry.Skinned;
                e["sources"] = entry.SourceMeshIndices;
                e["transform"] = EncodeTransform(entry.LocalTransform);
                if (!entry.Materials.empty()) {
                    json mats = json::array();
                    const PreservedMaterialEntry* preservedEntry = nullptr;
                    auto preservedIt = preservedMaterialEntries.find(entry.NodeName);
                    if (preservedIt != preservedMaterialEntries.end()) {
                        preservedEntry = &preservedIt->second;
                    } else if (hasPreservedIdentityMap) {
                        ModelNodeIdentity query = buildMeshIdentity(entry, i);
                        if (const ModelNodeIdentity* matchedNode = preservedIdentityMap.FindNode(query)) {
                            auto matchedIt = preservedMaterialEntries.find(matchedNode->NodePath);
                            if (matchedIt == preservedMaterialEntries.end()) {
                                matchedIt = preservedMaterialEntries.find(matchedNode->NodeName);
                            }
                            if (matchedIt != preservedMaterialEntries.end()) {
                                preservedEntry = &matchedIt->second;
                            }
                        }
                    }

                    for (size_t slotIndex = 0; slotIndex < entry.Materials.size(); ++slotIndex) {
                        const auto& mat = entry.Materials[slotIndex];
                        MaterialSource externalized = embedded_textures::ExternalizeMaterialSource(mat, modelNameHint);
                        if (preservedEntry) {
                            int preservedIndex = FindPreservedMaterialIndex(*preservedEntry,
                                                                           entry.MaterialSlotNames,
                                                                           slotIndex,
                                                                           externalized.Name);
                            if (preservedIndex >= 0 &&
                                static_cast<size_t>(preservedIndex) < preservedEntry->Materials.size()) {
                                const MaterialSource& preservedMaterial = preservedEntry->Materials[static_cast<size_t>(preservedIndex)];
                                PreserveManagedTextureSpecifier(externalized.Albedo, preservedMaterial.Albedo, modelNameHint);
                                PreserveManagedTextureSpecifier(externalized.MetallicRoughness, preservedMaterial.MetallicRoughness, modelNameHint);
                                PreserveManagedTextureSpecifier(externalized.Normal, preservedMaterial.Normal, modelNameHint);
                                PreserveManagedTextureSpecifier(externalized.AO, preservedMaterial.AO, modelNameHint);
                                PreserveManagedTextureSpecifier(externalized.Emission, preservedMaterial.Emission, modelNameHint);
                                PreserveManagedTextureSpecifier(externalized.Displacement, preservedMaterial.Displacement, modelNameHint);
                            }
                        }
                        mats.push_back(material_serialization::ToJson(externalized));
                    }
                    e["materials"] = std::move(mats);
                }
                if (!entry.MaterialSlotNames.empty()) {
                    e["slotNames"] = entry.MaterialSlotNames;
                }
                j["entries"].push_back(std::move(e));
            }

            // Proxy entries
            j["proxies"] = json::array();
            for (const auto& proxy : prepared.Proxies) {
                json p;
                p["name"] = proxy.NodeName;
                if (!proxy.DisplayName.empty()) {
                    p["displayName"] = proxy.DisplayName;
                }
                p["meshIndex"] = static_cast<int>(proxy.MeshEntryIndex);
                p["skinned"] = proxy.Skinned;
                p["originalIndex"] = proxy.OriginalMeshIndex;
                p["slots"] = proxy.SubmeshSlots;
                p["transform"] = EncodeTransform(proxy.LocalTransform);
                // Store quaternion separately for tooling that only needs rotation
                glm::vec3 t(0.0f), s(1.0f), skew(0.0f);
                glm::vec4 persp(0.0f);
                glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
                glm::decompose(proxy.LocalTransform, s, r, t, skew, persp);
                r = glm::normalize(r);
                p["rotationHint"] = { r.w, r.x, r.y, r.z };
                j["proxies"].push_back(std::move(p));
            }

            if (prepared.Skeleton.HasSkeleton) {
                json skelInfo;
                skelInfo["boneCount"] = prepared.Skeleton.BoneNames.size();
                skelInfo["boneNames"] = prepared.Skeleton.BoneNames;
                skelInfo["boneParents"] = prepared.Skeleton.BoneParents;
                j["skeletonInfo"] = std::move(skelInfo);
            }

            j["animations"] = json::array();

            // Generate stable node identities for all mesh nodes
            // This enables robust matching during hot reload even if hierarchy changes
            ModelIdentityMap identityMap;
            identityMap.SourcePath = sourcePathForJson;
            identityMap.ModelGUID = guid;

            // Build identity for each mesh entry
            for (size_t i = 0; i < prepared.Meshes.size(); ++i) {
                const auto& entry = prepared.Meshes[i];
                identityMap.Nodes.push_back(buildMeshIdentity(entry, i));
            }
            
            // Build identity for each proxy (these are the actual rendered nodes)
            for (size_t i = 0; i < prepared.Proxies.size(); ++i) {
                const auto& proxy = prepared.Proxies[i];
                ModelNodeIdentity identity;
                identity.NodePath = proxy.NodeName;
                identity.NormalizedPath = ModelNodeIdentity::NormalizePath(proxy.NodeName);
                identity.NodeName = proxy.NodeName;
                identity.NormalizedName = ModelNodeIdentity::NormalizeName(proxy.NodeName);
                identity.MeshFileId = proxy.OriginalMeshIndex;
                identity.Skinned = proxy.Skinned;
                identity.Depth = 1;
                
                // Compute content hash including submesh slots
                auto matFloats = matrixToFloats(proxy.LocalTransform);
                std::vector<int> slotInts(proxy.SubmeshSlots.begin(), proxy.SubmeshSlots.end());
                identity.ContentHash = ModelNodeIdentity::ComputeContentHash(
                    slotInts,
                    matFloats.data(),
                    static_cast<int>(proxy.MeshEntryIndex));
                
                identity.DerivedGUID = ModelNodeIdentity::GenerateDerivedGUID(
                    identity.NodePath,
                    identity.ContentHash,
                    guid);
                
                identityMap.Nodes.push_back(std::move(identity));
            }
            
            // Build skeleton bone identities
            if (prepared.Skeleton.HasSkeleton) {
                for (size_t b = 0; b < prepared.Skeleton.BoneNames.size(); ++b) {
                    ModelNodeIdentity identity;
                    identity.NodePath = prepared.Skeleton.BoneNames[b];
                    identity.NormalizedPath = ModelNodeIdentity::NormalizePath(identity.NodePath);
                    identity.NodeName = prepared.Skeleton.BoneNames[b];
                    identity.NormalizedName = ModelNodeIdentity::NormalizeName(identity.NodeName);
                    identity.MeshFileId = -1; // Not a mesh node
                    identity.Skinned = false;
                    
                    // Compute depth from parent chain
                    int depth = 0;
                    int parent = (b < prepared.Skeleton.BoneParents.size()) ? prepared.Skeleton.BoneParents[b] : -1;
                    while (parent >= 0 && depth < 100) {
                        depth++;
                        parent = (static_cast<size_t>(parent) < prepared.Skeleton.BoneParents.size()) 
                            ? prepared.Skeleton.BoneParents[parent] : -1;
                    }
                    identity.Depth = depth;
                    
                    // Content hash from inverse bind pose
                    std::vector<int> boneIdx = { static_cast<int>(b) };
                    const float* ibpData = (b < prepared.Skeleton.InverseBindPoses.size())
                        ? &prepared.Skeleton.InverseBindPoses[b][0][0] : nullptr;
                    identity.ContentHash = ModelNodeIdentity::ComputeContentHash(boneIdx, ibpData, 0);
                    
                    // Parent GUID
                    int parentIdx = (b < prepared.Skeleton.BoneParents.size()) ? prepared.Skeleton.BoneParents[b] : -1;
                    if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < identityMap.Nodes.size()) {
                        // Find parent in already-added nodes
                        for (const auto& node : identityMap.Nodes) {
                            if (node.NodeName == prepared.Skeleton.BoneNames[parentIdx]) {
                                identity.ParentGUID = node.DerivedGUID;
                                break;
                            }
                        }
                    }
                    
                    identity.DerivedGUID = ModelNodeIdentity::GenerateDerivedGUID(
                        identity.NodePath,
                        identity.ContentHash,
                        identity.ParentGUID);
                    
                    identityMap.Nodes.push_back(std::move(identity));
                }
            }
            
            identityMap.BuildLookups();
            j["nodeIdentities"] = identityMap.ToJson();
            
            // CRITICAL: Restore preserved user-authored settings
            if (hasImportSettings) {
                j["importSettings"] = preservedImportSettings;
                std::cout << "[ModelImportCache] Preserved importSettings across reimport" << std::endl;
            }
            if (hasArmorMode) {
                j["armorMode"] = preservedArmorMode;
                std::cout << "[ModelImportCache] Preserved armorMode=" << preservedArmorMode << " across reimport" << std::endl;
            }

            std::ofstream of(out.metaPath, std::ios::binary | std::ios::trunc);
            if (!of.is_open()) return false;
            of << j.dump(4);
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[ModelImportCache] Failed to write meta: " << e.what() << std::endl;
            return false;
        } catch (...) {
            std::cerr << "[ModelImportCache] Failed to write meta: unknown error" << std::endl;
            return false;
        }
    }

    struct HumanoidSliceSettings {
        float minWeightContribution = 0.05f;
        float torsoDominanceMargin = 0.25f;
        float secondaryPromotionThreshold = 0.18f;
    };

    enum class HumanoidSliceRegion : uint8_t {
        Torso = 0,
        Arms,
        Legs,
        Head,
        Count
    };

    static const char* RegionLabel(HumanoidSliceRegion region) {
        switch (region) {
            case HumanoidSliceRegion::Torso: return "Torso";
            case HumanoidSliceRegion::Arms:  return "Arms";
            case HumanoidSliceRegion::Legs:  return "Legs";
            case HumanoidSliceRegion::Head:  return "Head";
            default: return "Region";
        }
    }

    static HumanoidSliceRegion MapHumanoidBoneToRegion(cm::animation::HumanoidBone bone) {
        using cm::animation::HumanoidBone;
        switch (bone) {
            case HumanoidBone::Root:
            case HumanoidBone::Hips:
            case HumanoidBone::Spine:
            case HumanoidBone::Chest:
            case HumanoidBone::UpperChest:
                return HumanoidSliceRegion::Torso;
            case HumanoidBone::Neck:
            case HumanoidBone::Head:
            case HumanoidBone::LeftEye:
            case HumanoidBone::RightEye:
                return HumanoidSliceRegion::Head;
            case HumanoidBone::LeftShoulder:
            case HumanoidBone::LeftUpperArm:
            case HumanoidBone::LeftLowerArm:
            case HumanoidBone::LeftHand:
            case HumanoidBone::LeftUpperArmTwist:
            case HumanoidBone::LeftLowerArmTwist:
            case HumanoidBone::LeftThumbProx:
            case HumanoidBone::LeftThumbInter:
            case HumanoidBone::LeftThumbDist:
            case HumanoidBone::LeftIndexProx:
            case HumanoidBone::LeftIndexInter:
            case HumanoidBone::LeftIndexDist:
            case HumanoidBone::LeftMiddleProx:
            case HumanoidBone::LeftMiddleInter:
            case HumanoidBone::LeftMiddleDist:
            case HumanoidBone::LeftRingProx:
            case HumanoidBone::LeftRingInter:
            case HumanoidBone::LeftRingDist:
            case HumanoidBone::LeftLittleProx:
            case HumanoidBone::LeftLittleInter:
            case HumanoidBone::LeftLittleDist:
            case HumanoidBone::RightShoulder:
            case HumanoidBone::RightUpperArm:
            case HumanoidBone::RightLowerArm:
            case HumanoidBone::RightHand:
            case HumanoidBone::RightUpperArmTwist:
            case HumanoidBone::RightLowerArmTwist:
            case HumanoidBone::RightThumbProx:
            case HumanoidBone::RightThumbInter:
            case HumanoidBone::RightThumbDist:
            case HumanoidBone::RightIndexProx:
            case HumanoidBone::RightIndexInter:
            case HumanoidBone::RightIndexDist:
            case HumanoidBone::RightMiddleProx:
            case HumanoidBone::RightMiddleInter:
            case HumanoidBone::RightMiddleDist:
            case HumanoidBone::RightRingProx:
            case HumanoidBone::RightRingInter:
            case HumanoidBone::RightRingDist:
            case HumanoidBone::RightLittleProx:
            case HumanoidBone::RightLittleInter:
            case HumanoidBone::RightLittleDist:
                return HumanoidSliceRegion::Arms;
            case HumanoidBone::LeftUpperLeg:
            case HumanoidBone::LeftLowerLeg:
            case HumanoidBone::LeftFoot:
            case HumanoidBone::LeftToes:
            case HumanoidBone::LeftUpperLegTwist:
            case HumanoidBone::LeftLowerLegTwist:
            case HumanoidBone::RightUpperLeg:
            case HumanoidBone::RightLowerLeg:
            case HumanoidBone::RightFoot:
            case HumanoidBone::RightToes:
            case HumanoidBone::RightUpperLegTwist:
            case HumanoidBone::RightLowerLegTwist:
                return HumanoidSliceRegion::Legs;
            default:
                return HumanoidSliceRegion::Torso;
        }
    }

    struct VertexAssignment {
        HumanoidSliceRegion region = HumanoidSliceRegion::Torso;
        float score = 1.0f;
        bool valid = false;
    };

    struct PreparedEntryWithProxy {
        PreparedMeshEntry entry;
        bool hasProxy = false;
        PreparedProxyEntry proxy;
    };

    static std::vector<HumanoidSliceRegion> BuildBoneRegionLUT(const PreparedSkeleton& skeleton,
                                                               const cm::animation::AvatarDefinition& avatar) {
        std::vector<HumanoidSliceRegion> lut(skeleton.BoneNames.size(), HumanoidSliceRegion::Torso);
        const size_t mapCount = std::min<size_t>(avatar.Map.size(), cm::animation::HumanoidBoneCount);
        for (size_t i = 0; i < mapCount; ++i) {
            int boneIndex = avatar.Map[i].BoneIndex;
            if (boneIndex < 0 || boneIndex >= static_cast<int>(lut.size())) {
                continue;
            }
            lut[boneIndex] = MapHumanoidBoneToRegion(static_cast<cm::animation::HumanoidBone>(i));
        }
        return lut;
    }

    static std::vector<VertexAssignment> ClassifyVertices(const Mesh& mesh,
                                                          const std::vector<HumanoidSliceRegion>& boneRegions,
                                                          const HumanoidSliceSettings& settings) {
        std::vector<VertexAssignment> assignments(mesh.Vertices.size());
        for (size_t i = 0; i < mesh.Vertices.size(); ++i) {
            VertexAssignment assign;
            std::array<float, static_cast<size_t>(HumanoidSliceRegion::Count)> totals{};
            bool contributed = false;
            if (i < mesh.BoneIndices.size() && i < mesh.BoneWeights.size()) {
                const glm::ivec4 bi = mesh.BoneIndices[i];
                const glm::vec4 bw = mesh.BoneWeights[i];
                for (int c = 0; c < 4; ++c) {
                    float w = bw[c];
                    if (w < settings.minWeightContribution) {
                        continue;
                    }
                    int boneIndex = bi[c];
                    if (boneIndex < 0 || boneIndex >= static_cast<int>(boneRegions.size())) {
                        continue;
                    }
                    totals[static_cast<size_t>(boneRegions[boneIndex])] += w;
                    contributed = true;
                }
            }
            if (!contributed) {
                assign.region = HumanoidSliceRegion::Torso;
                assign.score = 1.0f;
                assign.valid = true;
                assignments[i] = assign;
                continue;
            }
            float bestWeight = -1.0f;
            float secondWeight = -1.0f;
            HumanoidSliceRegion bestRegion = HumanoidSliceRegion::Torso;
            HumanoidSliceRegion secondRegion = HumanoidSliceRegion::Torso;
            for (size_t r = 0; r < static_cast<size_t>(HumanoidSliceRegion::Count); ++r) {
                float weight = totals[r];
                if (weight > bestWeight) {
                    secondWeight = bestWeight;
                    secondRegion = bestRegion;
                    bestWeight = weight;
                    bestRegion = static_cast<HumanoidSliceRegion>(r);
                } else if (weight > secondWeight) {
                    secondWeight = weight;
                    secondRegion = static_cast<HumanoidSliceRegion>(r);
                }
            }
            if (bestRegion == HumanoidSliceRegion::Torso &&
                secondWeight > settings.secondaryPromotionThreshold &&
                (bestWeight - secondWeight) < settings.torsoDominanceMargin) {
                assign.region = secondRegion;
                assign.score = secondWeight;
            } else {
                assign.region = bestRegion;
                assign.score = std::max(bestWeight, settings.minWeightContribution);
            }
            assign.valid = true;
            assignments[i] = assign;
        }
        return assignments;
    }

    static HumanoidSliceRegion DetermineTriangleRegion(uint32_t i0,
                                                       uint32_t i1,
                                                       uint32_t i2,
                                                       const std::vector<VertexAssignment>& assignments) {
        std::array<float, static_cast<size_t>(HumanoidSliceRegion::Count)> totals{};
        const uint32_t verts[3] = { i0, i1, i2 };
        for (uint32_t idx : verts) {
            if (idx >= assignments.size()) {
                continue;
            }
            const VertexAssignment& va = assignments[idx];
            if (!va.valid) {
                continue;
            }
            totals[static_cast<size_t>(va.region)] += va.score;
        }
        float bestWeight = -1.0f;
        HumanoidSliceRegion region = HumanoidSliceRegion::Torso;
        for (size_t r = 0; r < totals.size(); ++r) {
            if (totals[r] > bestWeight) {
                bestWeight = totals[r];
                region = static_cast<HumanoidSliceRegion>(r);
            }
        }
        return region;
    }

    struct RegionMeshBuild {
        std::shared_ptr<Mesh> mesh;
        BlendShapeComponent blendShapes;
        std::vector<uint32_t> sourceVertexMap;
    };

    static RegionMeshBuild BuildRegionMesh(const PreparedMeshEntry& entry,
                                           HumanoidSliceRegion targetRegion,
                                           const std::vector<VertexAssignment>& assignments) {
        RegionMeshBuild result;
        if (!entry.MeshData) {
            return result;
        }
        auto src = entry.MeshData;
        if (src->Indices.empty()) {
            return result;
        }

        std::vector<Mesh::Submesh> submeshes = src->Submeshes;
        if (submeshes.empty()) {
            Mesh::Submesh fallback;
            fallback.indexStart = 0;
            fallback.indexCount = static_cast<uint32_t>(src->Indices.size());
            fallback.materialSlot = 0;
            submeshes.push_back(fallback);
        }

        auto remap = std::vector<uint32_t>(src->Vertices.size(), std::numeric_limits<uint32_t>::max());
        auto copied = std::vector<uint32_t>(src->Vertices.size(), 0);
        std::vector<uint32_t> sourceOfNewVertex;

        auto ensureVertex = [&](uint32_t srcIndex) -> uint32_t {
            if (srcIndex >= src->Vertices.size()) {
                return 0;
            }
            uint32_t& mapped = remap[srcIndex];
            if (mapped != std::numeric_limits<uint32_t>::max()) {
                return mapped;
            }
            if (!result.mesh) {
                result.mesh = std::make_shared<Mesh>();
                result.mesh->Dynamic = src->Dynamic;
                result.mesh->SkinnedLayout = src->SkinnedLayout;
            }
            mapped = static_cast<uint32_t>(result.mesh->Vertices.size());
            result.mesh->Vertices.push_back(src->Vertices[srcIndex]);
            if (!src->Normals.empty()) result.mesh->Normals.push_back(srcIndex < src->Normals.size() ? src->Normals[srcIndex] : glm::vec3(0));
            if (!src->UVs.empty()) result.mesh->UVs.push_back(srcIndex < src->UVs.size() ? src->UVs[srcIndex] : glm::vec2(0));
            if (!src->BoneIndices.empty()) result.mesh->BoneIndices.push_back(srcIndex < src->BoneIndices.size() ? src->BoneIndices[srcIndex] : glm::ivec4(0));
            if (!src->BoneWeights.empty()) result.mesh->BoneWeights.push_back(srcIndex < src->BoneWeights.size() ? src->BoneWeights[srcIndex] : glm::vec4(1,0,0,0));
            sourceOfNewVertex.push_back(srcIndex);
            copied[srcIndex] = 1;
            return mapped;
        };

        for (const auto& sm : submeshes) {
            uint32_t start = static_cast<uint32_t>(result.mesh ? result.mesh->Indices.size() : 0);
            for (uint32_t offset = 0; offset + 2 < sm.indexCount; offset += 3) {
                uint32_t idx0 = src->Indices[sm.indexStart + offset + 0];
                uint32_t idx1 = src->Indices[sm.indexStart + offset + 1];
                uint32_t idx2 = src->Indices[sm.indexStart + offset + 2];
                HumanoidSliceRegion triRegion = DetermineTriangleRegion(idx0, idx1, idx2, assignments);
                if (triRegion != targetRegion) {
                    continue;
                }
                uint32_t v0 = ensureVertex(idx0);
                uint32_t v1 = ensureVertex(idx1);
                uint32_t v2 = ensureVertex(idx2);
                if (!result.mesh) {
                    continue;
                }
                result.mesh->Indices.push_back(v0);
                result.mesh->Indices.push_back(v1);
                result.mesh->Indices.push_back(v2);
            }
            if (!result.mesh) {
                continue;
            }
            uint32_t written = static_cast<uint32_t>(result.mesh->Indices.size()) - start;
            if (written == 0) {
                continue;
            }
            Mesh::Submesh slicedSm;
            slicedSm.indexStart = start;
            slicedSm.indexCount = written;
            slicedSm.baseVertex = 0;
            slicedSm.materialSlot = sm.materialSlot;
            result.mesh->Submeshes.push_back(slicedSm);
        }

        if (!result.mesh || result.mesh->Indices.empty()) {
            result.mesh.reset();
            result.sourceVertexMap.clear();
            return result;
        }

        result.mesh->numVertices = static_cast<uint32_t>(result.mesh->Vertices.size());
        result.mesh->numIndices = static_cast<uint32_t>(result.mesh->Indices.size());
        result.mesh->ComputeBounds();
        result.sourceVertexMap = std::move(sourceOfNewVertex);

        if (!entry.BlendShapes.Shapes.empty()) {
            // Build reverse lookup: source vertex -> new vertex index
            std::unordered_map<uint32_t, uint32_t> srcToNew;
            for (size_t i = 0; i < result.sourceVertexMap.size(); ++i) {
                srcToNew[result.sourceVertexMap[i]] = static_cast<uint32_t>(i);
            }
            
            result.blendShapes.Shapes.reserve(entry.BlendShapes.Shapes.size());
            for (const auto& shape : entry.BlendShapes.Shapes) {
                BlendShape slicedShape;
                slicedShape.Name = shape.Name;
                slicedShape.Weight = shape.Weight;
                slicedShape.IsSparse = true; // Always produce sparse output
                
                if (shape.IsSparse) {
                    // Sparse input: remap indices
                    for (size_t i = 0; i < shape.SparseIndices.size(); ++i) {
                        uint32_t srcIndex = shape.SparseIndices[i];
                        auto it = srcToNew.find(srcIndex);
                        if (it == srcToNew.end()) continue; // vertex not in this slice
                        
                        slicedShape.SparseIndices.push_back(it->second);
                        slicedShape.SparseDeltaPos.push_back(shape.SparseDeltaPos[i]);
                        if (i < shape.SparseDeltaNorm.size()) {
                            slicedShape.SparseDeltaNorm.push_back(shape.SparseDeltaNorm[i]);
                        }
                    }
                } else {
                    // Dense input: convert to sparse during slice
                    if (shape.DeltaPos.size() != src->Vertices.size()) {
                        continue;
                    }
                    constexpr float kThreshold = 1e-6f;
                    for (size_t i = 0; i < result.sourceVertexMap.size(); ++i) {
                        uint32_t srcIndex = result.sourceVertexMap[i];
                        if (srcIndex >= shape.DeltaPos.size()) continue;
                        
                        const glm::vec3& dp = shape.DeltaPos[srcIndex];
                        const glm::vec3& dn = (srcIndex < shape.DeltaNormal.size()) 
                                              ? shape.DeltaNormal[srcIndex] : glm::vec3(0);
                        const float lenSq = glm::dot(dp, dp) + glm::dot(dn, dn);
                        if (lenSq > kThreshold * kThreshold) {
                            slicedShape.SparseIndices.push_back(static_cast<uint32_t>(i));
                            slicedShape.SparseDeltaPos.push_back(dp);
                            slicedShape.SparseDeltaNorm.push_back(dn);
                        }
                    }
                }
                
                // Shrink to fit
                slicedShape.SparseIndices.shrink_to_fit();
                slicedShape.SparseDeltaPos.shrink_to_fit();
                slicedShape.SparseDeltaNorm.shrink_to_fit();
                
                result.blendShapes.Shapes.push_back(std::move(slicedShape));
            }
        }

        return result;
    }

    static PreparedProxyEntry MakeProxyForEntry(const std::string& parentName,
                                                const std::string& displayName,
                                                const glm::mat4& localTransform,
                                                int originalSourceIndex,
                                                size_t submeshCount,
                                                bool skinned) {
        PreparedProxyEntry proxy;
        proxy.NodeName = parentName;
        proxy.DisplayName = displayName;
        proxy.LocalTransform = localTransform;
        proxy.MeshEntryIndex = 0;
        proxy.SubmeshSlots.clear();
        proxy.Skinned = skinned;
        proxy.OriginalMeshIndex = originalSourceIndex;
        for (uint32_t slot = 0; slot < submeshCount; ++slot) {
            proxy.SubmeshSlots.push_back(slot);
        }
        return proxy;
    }

    static std::vector<PreparedEntryWithProxy> SlicePreparedEntry(const PreparedMeshEntry& entry,
                                                                  const std::vector<HumanoidSliceRegion>& boneRegions,
                                                                  const HumanoidSliceSettings& settings) {
        std::vector<PreparedEntryWithProxy> outputs;
        if (!entry.Skinned || !entry.MeshData || entry.MeshData->BoneIndices.empty()) {
            PreparedEntryWithProxy copy;
            copy.entry = entry;
            copy.hasProxy = entry.Skinned;
            if (copy.hasProxy) {
                copy.proxy = MakeProxyForEntry(entry.NodeName,
                                               entry.NodeName,
                                               entry.LocalTransform,
                                               entry.SourceMeshIndices.empty() ? -1 : entry.SourceMeshIndices.front(),
                                               entry.MeshData ? entry.MeshData->Submeshes.size() : 0,
                                               entry.Skinned);
            }
            outputs.push_back(std::move(copy));
            return outputs;
        }

        auto assignments = ClassifyVertices(*entry.MeshData, boneRegions, settings);
        const std::string baseName = entry.NodeName.empty() ? std::string("Mesh") : entry.NodeName;

        const HumanoidSliceRegion regions[] = {
            HumanoidSliceRegion::Torso,
            HumanoidSliceRegion::Arms,
            HumanoidSliceRegion::Legs,
            HumanoidSliceRegion::Head
        };

        for (HumanoidSliceRegion region : regions) {
            RegionMeshBuild build = BuildRegionMesh(entry, region, assignments);
            if (!build.mesh) {
                continue;
            }
            PreparedEntryWithProxy payload;
            payload.entry = entry;
            payload.entry.NodeName = baseName + " (" + RegionLabel(region) + ")";
            payload.entry.MeshData = build.mesh;
            payload.entry.BlendShapes = std::move(build.blendShapes);
            payload.entry.Materials = entry.Materials;
            payload.entry.MaterialSlotNames = entry.MaterialSlotNames;
            payload.entry.SourceMeshIndices = entry.SourceMeshIndices;
            payload.hasProxy = true;
            payload.proxy = MakeProxyForEntry(baseName,
                                              payload.entry.NodeName,
                                              entry.LocalTransform,
                                              entry.SourceMeshIndices.empty() ? -1 : entry.SourceMeshIndices.front(),
                                              build.mesh->Submeshes.size(),
                                              entry.Skinned);
            outputs.push_back(std::move(payload));
        }

        if (outputs.empty()) {
            PreparedEntryWithProxy copy;
            copy.entry = entry;
            copy.hasProxy = true;
            copy.proxy = MakeProxyForEntry(baseName,
                                           baseName,
                                           entry.LocalTransform,
                                           entry.SourceMeshIndices.empty() ? -1 : entry.SourceMeshIndices.front(),
                                           entry.MeshData->Submeshes.size(),
                                           entry.Skinned);
            outputs.push_back(std::move(copy));
        }
        return outputs;
    }

    static PreparedModel BuildHumanoidSlicedPreparedModel(const Model& /*sourceModel*/,
                                                          const PreparedModel& base,
                                                          const cm::animation::AvatarDefinition& avatar,
                                                          const HumanoidSliceSettings& settings) {
        PreparedModel result;
        result.RootLocal = base.RootLocal;
        result.Skeleton = base.Skeleton;
        if (!base.Skeleton.HasSkeleton) {
            return base;
        }
        auto boneRegions = BuildBoneRegionLUT(base.Skeleton, avatar);
        bool slicedAny = false;
        for (const auto& meshEntry : base.Meshes) {
            auto payloads = SlicePreparedEntry(meshEntry, boneRegions, settings);
            for (auto& payload : payloads) {
                size_t meshIndex = result.Meshes.size();
                result.Meshes.push_back(payload.entry);
                if (payload.hasProxy) {
                    payload.proxy.MeshEntryIndex = static_cast<size_t>(meshIndex);
                    result.Proxies.push_back(payload.proxy);
                }
            }
            if (payloads.size() > 1) {
                slicedAny = true;
            }
        }
        if (!slicedAny) {
            return base;
        }
        return result;
    }
}

bool HasModelCache(const std::string& sourceModelPath, BuiltModelPaths& out) {
    namespace fs = std::filesystem;
    
    // Normalize path to virtual path format for pak lookup
    std::string normalizedPath = sourceModelPath;
    for (char& c : normalizedPath) if (c == '\\') c = '/';
    
    // Extract just the "assets/..." portion if it's an absolute path
    auto extractVirtualPath = [](const std::string& path) -> std::string {
        std::string p = path;
        for (char& c : p) if (c == '\\') c = '/';
        size_t pos = p.find("assets/");
        if (pos != std::string::npos) return p.substr(pos);
        return p;
    };
    
    std::string vpath = extractVirtualPath(normalizedPath);
    fs::path src(vpath);
    fs::path baseDir = src.parent_path();
    fs::path stem = src.stem();

    out.metaPath = (baseDir / (stem.string() + ".meta")).string();
    out.skelPath = (baseDir / (stem.string() + ".skelbin")).string();
    out.meshPath = (baseDir / (stem.string() + ".meshbin")).string();
    
    // Normalize output paths
    for (char& c : out.metaPath) if (c == '\\') c = '/';
    for (char& c : out.skelPath) if (c == '\\') c = '/';
    for (char& c : out.meshPath) if (c == '\\') c = '/';
    
    // Check if running from pak (standalone mode)
    bool pakMode = FileSystem::Instance().IsPakMounted();
    
    if (pakMode) {
        // In pak mode, just check if the cache files exist in the pak
        bool metaExists = FileSystem::Instance().Exists(out.metaPath);
        bool skelExists = FileSystem::Instance().Exists(out.skelPath);
        bool meshExists = FileSystem::Instance().Exists(out.meshPath);
        
        if (!metaExists || !skelExists || !meshExists) {
            return false;
        }
        
        // Validate meta version from pak
        std::string metaText;
        if (FileSystem::Instance().ReadTextFile(out.metaPath, metaText)) {
            try {
                json meta = json::parse(metaText);
                int version = meta.value("version", 0);
                if (version < kCurrentModelMetaVersion) return false;
            } catch (...) {
                return false;
            }
        } else {
            return false;
        }
        
        return true;
    }
    
    // Editor mode: use filesystem directly with timestamp checks
    fs::path diskSrc(sourceModelPath);
    if (!fs::exists(diskSrc)) return false;

    // Build absolute paths for editor mode
    fs::path diskBaseDir = diskSrc.parent_path();
    out.metaPath = (diskBaseDir / (stem.string() + ".meta")).string();
    out.skelPath = (diskBaseDir / (stem.string() + ".skelbin")).string();
    out.meshPath = (diskBaseDir / (stem.string() + ".meshbin")).string();

    auto upToDate = [&](const fs::path& p) {
        if (!fs::exists(p)) return false;
        auto tSrc = fs::last_write_time(diskSrc);
        auto tP = fs::last_write_time(p);
        return tP >= tSrc;
    };

    bool ok = upToDate(out.metaPath) && upToDate(out.skelPath) && upToDate(out.meshPath);
    if (ok) {
        try {
            std::ifstream in(out.metaPath);
            if (!in.is_open()) ok = false;
            else {
                json meta; in >> meta;
                int version = meta.value("version", 0);
                if (version < kCurrentModelMetaVersion) ok = false;
            }
        } catch (...) {
            ok = false;
        }
        if (ok && !meshbin::HasCurrentVersion(out.meshPath)) {
            ok = false;
        }
    }

    return ok;
}

bool EnsureModelCache(const std::string& sourceModelPath, BuiltModelPaths& out) {
    if (HasModelCache(sourceModelPath, out)) {
        return true;
    }
    return BuildModelCacheBlocking(sourceModelPath, out);
}

static bool BuildModelCacheInternal(const std::string& sourceModelPath,
                                    const cm::animation::AvatarDefinition* humanoidAvatar,
                                    BuiltModelPaths& out) {
    namespace fs = std::filesystem;
    try {
        std::lock_guard<std::mutex> lk(g_modelCacheMutex);
        fs::path src(sourceModelPath);
        if (!fs::exists(src)) return false;
        fs::path baseDir = src.parent_path();
        fs::path stem = src.stem();

        out.metaPath = (baseDir / (stem.string() + ".meta")).string();
        out.skelPath = (baseDir / (stem.string() + ".skelbin")).string();
        out.meshPath = (baseDir / (stem.string() + ".meshbin")).string();

        std::error_code ec;
        fs::create_directories(baseDir, ec);

        Model model = ModelLoader::LoadModel(sourceModelPath);
        PreparedModel prepared = BuildPreparedModel(model);
        if (humanoidAvatar) {
            HumanoidSliceSettings settings;
            prepared = BuildHumanoidSlicedPreparedModel(model, prepared, *humanoidAvatar, settings);
        }
        if (prepared.Meshes.empty() && prepared.Proxies.empty() && !prepared.Skeleton.HasSkeleton) {
            std::cerr << "[ModelImportCache] Import failed: " << sourceModelPath << "\n";
            return false;
        }

        skelbin::PackedSkeleton ps{};
        if (prepared.Skeleton.HasSkeleton) {
            ps.inverseBindPoses = prepared.Skeleton.InverseBindPoses;
            ps.boneParents = prepared.Skeleton.BoneParents;
            ps.boneNames = prepared.Skeleton.BoneNames;
        }
        if (!skelbin::WriteSkelBin(ps, out.skelPath)) return false;

        if (!meshbin::WriteMeshBinFromPrepared(prepared, out.meshPath)) return false;
        if (!WriteMeta(sourceModelPath, out, model, prepared)) return false;

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ModelImportCache] Build failed: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[ModelImportCache] Build failed: unknown error" << std::endl;
        return false;
    }
}

bool BuildModelCacheBlocking(const std::string& sourceModelPath, BuiltModelPaths& out) {
    return BuildModelCacheInternal(sourceModelPath, nullptr, out);
}

bool BuildHumanoidSlicedModelCacheBlocking(const std::string& sourceModelPath,
                                           const cm::animation::AvatarDefinition& avatar,
                                           BuiltModelPaths& out) {
    return BuildModelCacheInternal(sourceModelPath, &avatar, out);
}
