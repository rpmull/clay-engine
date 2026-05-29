#include "PrefabPrewarm.h"
#include "core/assets/BinaryFormats.h"
#include "core/assets/IAssetResolver.h"
#include "core/ecs/EntityData.h"
#include "core/ecs/Scene.h"
#include "core/managed/ScriptReflection.h"
#include "core/managed/ScriptComponent.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/RuntimeShaderGraphMaterial.h"
#include "core/serialization/EntityBinaryLoader.h"
#include "core/serialization/MaterialCache.h"
#include "core/rendering/ShaderManager.h"
#include "core/ecs/UIComponents.h"
#include "core/vfs/FileSystem.h"
#include "core/vfs/VirtualFS.h"
#include "core/animation/AnimatorControllerIO.h"
#include "core/animation/AnimatorControllerOverride.h"
#include "core/animation/AnimatorControllerOverrideIO.h"
#include "core/animation/AnimationAssetCache.h"
#include "core/particles/SpriteLoader.h"
#include "core/dialogue/DialogueLibrary.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <unordered_set>

namespace prefab {
namespace {

struct ReadContext {
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
    size_t offset = 0;
    size_t stringTableOffset = 0;
    std::vector<std::string> strings;
    
    bool Read(void* dst, size_t count) {
        if (offset + count > dataSize) return false;
        std::memcpy(dst, data + offset, count);
        offset += count;
        return true;
    }
    
    template<typename T>
    bool Read(T& value) { return Read(&value, sizeof(T)); }
    
    std::string ReadString(uint32_t index) {
        if (!strings.empty() && index < strings.size()) {
            return strings[index];
        }
        size_t pos = stringTableOffset;
        uint32_t currentIdx = 0;
        while (currentIdx < index && pos < dataSize) {
            while (pos < dataSize && data[pos] != 0) ++pos;
            ++pos;
            ++currentIdx;
        }
        if (pos >= dataSize) return "";
        size_t end = pos;
        while (end < dataSize && data[end] != 0) ++end;
        return std::string(reinterpret_cast<const char*>(data + pos), end - pos);
    }

    void BuildStringTable(size_t endOffset) {
        strings.clear();
        if (stringTableOffset >= dataSize) return;
        size_t end = std::min(endOffset, dataSize);
        if (stringTableOffset >= end) return;
        size_t pos = stringTableOffset;
        while (pos < end) {
            size_t start = pos;
            while (pos < end && data[pos] != 0) {
                ++pos;
            }
            strings.emplace_back(reinterpret_cast<const char*>(data + start), pos - start);
            if (pos < end) {
                ++pos;
            }
        }
    }
};

struct EntityHeader {
    uint64_t guidHigh;
    uint64_t guidLow;
    uint64_t parentGuidHigh;
    uint64_t parentGuidLow;
    uint32_t entityId;
    uint32_t nameIndex;
    uint8_t  flags;
    uint8_t  padding[3];
    int32_t  layer;
    uint32_t tagIndex;
    uint64_t modelGuidHigh;
    uint64_t modelGuidLow;
    uint32_t componentCount;
    uint32_t componentOffset;
};

struct EntityHeaderV3 {
    uint64_t guidHigh;
    uint64_t guidLow;
    uint64_t parentGuidHigh;
    uint64_t parentGuidLow;
    uint32_t entityId;
    uint32_t nameIndex;
    uint32_t componentCount;
    uint32_t componentOffset;
};

struct EntityHeaderV2 {
    uint64_t guidHigh;
    uint64_t guidLow;
    uint64_t parentGuidHigh;
    uint64_t parentGuidLow;
    uint32_t nameIndex;
    uint32_t componentCount;
    uint32_t componentOffset;
};

struct MaterialPrewarmJob {
    std::string path;
    bool skinned = false;
};

struct ShaderProgramJob {
    std::string vs;
    std::string fs;
};

class PrefabPrewarmManager {
public:
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    void Clear();
    void QueueScenePrefabs(Scene& scene);
    void QueuePrefabGuid(const ClaymoreGUID& guid);
    void Update(double budgetMs);

private:
    bool m_Enabled = true;
    std::mutex m_Mutex;
    std::deque<ClaymoreGUID> m_PrefabQueue;
    std::unordered_set<ClaymoreGUID, ClaymoreGUIDHasher> m_PrefabQueued;
    std::deque<ClaymoreGUID> m_DialogueQueue;
    std::unordered_set<ClaymoreGUID, ClaymoreGUIDHasher> m_DialogueQueued;
    std::deque<std::string> m_TextureQueue;
    std::unordered_set<std::string> m_TextureQueued;
    std::deque<MaterialPrewarmJob> m_MaterialQueue;
    std::unordered_set<std::string> m_MaterialQueued;
    std::deque<ShaderProgramJob> m_ShaderQueue;
    std::unordered_set<std::string> m_ShaderQueued;
    std::deque<std::string> m_AnimControllerQueue;
    std::unordered_set<std::string> m_AnimControllerQueued;
    std::deque<std::string> m_AnimOverrideQueue;
    std::unordered_set<std::string> m_AnimOverrideQueued;
    std::deque<std::string> m_AnimAssetQueue;
    std::unordered_set<std::string> m_AnimAssetQueued;
    std::deque<std::string> m_ParticleSpriteQueue;
    std::unordered_set<std::string> m_ParticleSpriteQueued;
    
    void CollectPrefabDependencies(const std::vector<uint8_t>& data);
    void CollectDependenciesFromEntity(const EntityData& data);
    void QueueDialogueLibrary(const ClaymoreGUID& guid);
    void QueueTexture(const std::string& path);
    void QueueMaterial(const std::string& path, bool skinned);
    void QueueShaderProgram(const std::string& vs, const std::string& fs);
    void QueueAnimationController(const std::string& path);
    void QueueAnimationControllerOverride(const std::string& path);
    void QueueAnimationAsset(const std::string& path);
    void QueueParticleSprite(const std::string& path);
    static std::string ResolveGuidToMeshPath(const ClaymoreGUID& guid);
};

PrefabPrewarmManager& GetManager() {
    static PrefabPrewarmManager instance;
    return instance;
}

void PrefabPrewarmManager::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_PrefabQueue.clear();
    m_PrefabQueued.clear();
    m_DialogueQueue.clear();
    m_DialogueQueued.clear();
    m_TextureQueue.clear();
    m_TextureQueued.clear();
    m_MaterialQueue.clear();
    m_MaterialQueued.clear();
    m_ShaderQueue.clear();
    m_ShaderQueued.clear();
    m_AnimControllerQueue.clear();
    m_AnimControllerQueued.clear();
    m_AnimOverrideQueue.clear();
    m_AnimOverrideQueued.clear();
    m_AnimAssetQueue.clear();
    m_AnimAssetQueued.clear();
    m_ParticleSpriteQueue.clear();
    m_ParticleSpriteQueued.clear();
}

void PrefabPrewarmManager::QueueScenePrefabs(Scene& scene) {
    if (!m_Enabled) return;
    if (!Assets::GetResolver()) return;
    
    const auto& entities = scene.GetEntities();
    for (const auto& entity : entities) {
        EntityData* data = scene.GetEntityData(entity.GetID());
        if (!data) continue;
        
        for (const auto& script : data->Scripts) {
            if (!ScriptReflection::HasProperties(script.ClassName)) continue;
            const auto& props = ScriptReflection::GetScriptProperties(script.ClassName);
            
            for (const auto& prop : props) {
                if (prop.type == PropertyType::Prefab) {
                    auto it = script.Values.find(prop.name);
                    if (it != script.Values.end() && std::holds_alternative<std::string>(it->second)) {
                        std::string guidStr = std::get<std::string>(it->second);
                        if (!guidStr.empty()) {
                            QueuePrefabGuid(ClaymoreGUID::FromString(guidStr));
                        }
                    } else if (std::holds_alternative<std::string>(prop.currentValue)) {
                        std::string guidStr = std::get<std::string>(prop.currentValue);
                        if (!guidStr.empty()) {
                            QueuePrefabGuid(ClaymoreGUID::FromString(guidStr));
                        }
                    }
                } else if (prop.type == PropertyType::DialogueLibrary) {
                    auto it = script.Values.find(prop.name);
                    if (it != script.Values.end() && std::holds_alternative<std::string>(it->second)) {
                        std::string guidStr = std::get<std::string>(it->second);
                        if (!guidStr.empty()) {
                            QueueDialogueLibrary(ClaymoreGUID::FromString(guidStr));
                        }
                    } else if (std::holds_alternative<std::string>(prop.currentValue)) {
                        std::string guidStr = std::get<std::string>(prop.currentValue);
                        if (!guidStr.empty()) {
                            QueueDialogueLibrary(ClaymoreGUID::FromString(guidStr));
                        }
                    }
                } else if (prop.type == PropertyType::AnimatorController) {
                    auto it = script.Values.find(prop.name);
                    if (it != script.Values.end() && std::holds_alternative<std::string>(it->second)) {
                        std::string path = std::get<std::string>(it->second);
                        if (!path.empty()) {
                            QueueAnimationController(path);
                        }
                    } else if (std::holds_alternative<std::string>(prop.currentValue)) {
                        std::string path = std::get<std::string>(prop.currentValue);
                        if (!path.empty()) {
                            QueueAnimationController(path);
                        }
                    }
                } else if (prop.type == PropertyType::AnimatorControllerOverride) {
                    auto it = script.Values.find(prop.name);
                    if (it != script.Values.end() && std::holds_alternative<std::string>(it->second)) {
                        std::string path = std::get<std::string>(it->second);
                        if (!path.empty()) {
                            QueueAnimationControllerOverride(path);
                        }
                    } else if (std::holds_alternative<std::string>(prop.currentValue)) {
                        std::string path = std::get<std::string>(prop.currentValue);
                        if (!path.empty()) {
                            QueueAnimationControllerOverride(path);
                        }
                    }
                } else if (prop.type == PropertyType::List &&
                           (prop.listElementType == PropertyType::Prefab ||
                            prop.listElementType == PropertyType::DialogueLibrary ||
                            prop.listElementType == PropertyType::AnimatorController ||
                            prop.listElementType == PropertyType::AnimatorControllerOverride)) {
                    auto it = script.Values.find(prop.name);
                    if (it == script.Values.end()) continue;
                    if (!std::holds_alternative<std::shared_ptr<ListPropertyValue>>(it->second)) continue;
                    auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(it->second);
                    if (!listPtr) continue;
                    if (listPtr->elementType != prop.listElementType) continue;
                    
                    for (const auto& element : listPtr->elements) {
                        if (std::holds_alternative<std::string>(element)) {
                            std::string value = std::get<std::string>(element);
                            if (value.empty()) continue;
                            switch (prop.listElementType) {
                                case PropertyType::Prefab:
                                    QueuePrefabGuid(ClaymoreGUID::FromString(value));
                                    break;
                                case PropertyType::DialogueLibrary:
                                    QueueDialogueLibrary(ClaymoreGUID::FromString(value));
                                    break;
                                case PropertyType::AnimatorController:
                                    QueueAnimationController(value);
                                    break;
                                case PropertyType::AnimatorControllerOverride:
                                    QueueAnimationControllerOverride(value);
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void PrefabPrewarmManager::QueuePrefabGuid(const ClaymoreGUID& guid) {
    if (!m_Enabled) return;
    if (guid.high == 0 && guid.low == 0) return;
    if (!Assets::GetResolver()) return;
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_PrefabQueued.insert(guid).second) {
        m_PrefabQueue.push_back(guid);
    }
}

void PrefabPrewarmManager::QueueTexture(const std::string& path) {
    if (path.empty()) return;
    std::string resolved = TextureLoader::ResolveTexturePath(path);
    
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_TextureQueued.insert(resolved).second) {
        m_TextureQueue.push_back(resolved);
    }
}

void PrefabPrewarmManager::QueueMaterial(const std::string& path, bool skinned) {
    if (path.empty()) return;
    std::string resolved = Assets::ResolvePath(path);
    if (resolved.empty()) resolved = path;
    
    std::filesystem::path p(resolved);
    if (p.extension() == ".mat") {
        p.replace_extension(".matbin");
        resolved = p.string();
    }
    
    std::string key = resolved + (skinned ? ":skinned" : ":static");
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_MaterialQueued.insert(key).second) {
        m_MaterialQueue.push_back({resolved, skinned});
    }
}

void PrefabPrewarmManager::QueueShaderProgram(const std::string& vs, const std::string& fs) {
    if (vs.empty() || fs.empty()) return;
    std::string key = vs + "+" + fs;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_ShaderQueued.insert(key).second) {
        m_ShaderQueue.push_back({vs, fs});
    }
}

void PrefabPrewarmManager::QueueDialogueLibrary(const ClaymoreGUID& guid) {
    if (guid.high == 0 && guid.low == 0) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_DialogueQueued.insert(guid).second) {
        m_DialogueQueue.push_back(guid);
    }
}

void PrefabPrewarmManager::QueueAnimationController(const std::string& path) {
    if (path.empty()) return;
    std::string resolved = IVirtualFS::NormalizePath(path);
    if (resolved.empty()) resolved = path;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_AnimControllerQueued.insert(resolved).second) {
        m_AnimControllerQueue.push_back(resolved);
    }
}

void PrefabPrewarmManager::QueueAnimationControllerOverride(const std::string& path) {
    if (path.empty()) return;
    std::string resolved = IVirtualFS::NormalizePath(path);
    if (resolved.empty()) resolved = path;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_AnimOverrideQueued.insert(resolved).second) {
        m_AnimOverrideQueue.push_back(resolved);
    }
}

void PrefabPrewarmManager::QueueAnimationAsset(const std::string& path) {
    if (path.empty()) return;
    std::string resolved = IVirtualFS::NormalizePath(path);
    if (resolved.empty()) resolved = path;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_AnimAssetQueued.insert(resolved).second) {
        m_AnimAssetQueue.push_back(resolved);
    }
}

void PrefabPrewarmManager::QueueParticleSprite(const std::string& path) {
    if (path.empty()) return;
    std::string key = IVirtualFS::NormalizePathLowercase(path);
    std::string resolved = IVirtualFS::NormalizePath(path);
    if (resolved.empty()) resolved = path;
    if (key.empty()) key = resolved;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_ParticleSpriteQueued.insert(key).second) {
        m_ParticleSpriteQueue.push_back(resolved);
    }
}

std::string PrefabPrewarmManager::ResolveGuidToMeshPath(const ClaymoreGUID& guid) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (resolver) {
        std::string path = resolver->GetPathForGUID(guid);
        if (!path.empty()) {
            std::filesystem::path p(path);
            std::string ext = p.extension().string();
            if (ext != ".meshbin") {
                p.replace_extension(".meshbin");
            }
            return p.string();
        }
    }
    return "";
}

void PrefabPrewarmManager::CollectDependenciesFromEntity(const EntityData& data) {
    IAssetResolver* resolver = Assets::GetResolver();
    if (data.Mesh) {
        const auto& mc = *data.Mesh;
        bool needsSkinned = data.Skinning != nullptr;
        
        if (!mc.MaterialAssetPaths.empty()) {
            for (const auto& matPath : mc.MaterialAssetPaths) {
                QueueMaterial(matPath, needsSkinned);
            }
        }
        
        for (const auto& inl : mc.InlineMaterials) {
            QueueTexture(inl.albedoPath);
            QueueTexture(inl.normalPath);
            QueueTexture(inl.metallicRoughnessPath);
            QueueTexture(inl.aoPath);
            QueueTexture(inl.emissionPath);
            
            if (inl.materialType == binary::InlineMaterialType::PSX) {
                QueueShaderProgram(needsSkinned ? "vs_psx_skinned" : "vs_psx", "fs_psx");
            } else {
                QueueShaderProgram(needsSkinned ? "vs_pbr_skinned" : "vs_pbr",
                                   needsSkinned ? "fs_pbr_skinned" : "fs_pbr");
            }
        }
        
        for (const auto& sg : mc.ShaderGraphMaterials) {
            QueueShaderProgram(sg.compiledVSName, sg.compiledFSName);
            for (const auto& p : sg.parameters) {
                if (p.type == static_cast<uint32_t>(cm::RuntimeShaderValueType::Texture2D)) {
                    QueueTexture(p.texturePath);
                }
            }
        }
        
        for (const auto& slotTexPaths : mc.SlotPropertyBlockTexturePaths) {
            for (const auto& kv : slotTexPaths) {
                QueueTexture(kv.second);
            }
        }
        for (const auto& kv : mc.PropertyBlockTexturePaths) {
            QueueTexture(kv.second);
        }
    }

    if (data.AnimationPlayer) {
        const auto& ap = *data.AnimationPlayer;
        QueueAnimationController(ap.ControllerPath);
        QueueAnimationAsset(ap.SingleClipPath);
    }
    
    if (resolver) {
        if (data.Panel) {
            QueueTexture(resolver->GetPathForGUID(data.Panel->Texture.guid));
        }
        if (data.Slider) {
            QueueTexture(resolver->GetPathForGUID(data.Slider->HandleTexture.guid));
            QueueTexture(resolver->GetPathForGUID(data.Slider->FillTexture.guid));
        }
        if (data.ProgressBar) {
            QueueTexture(resolver->GetPathForGUID(data.ProgressBar->FillTexture.guid));
        }
        if (data.Toggle) {
            QueueTexture(resolver->GetPathForGUID(data.Toggle->CheckmarkTexture.guid));
        }
        if (data.ScrollView) {
            QueueTexture(resolver->GetPathForGUID(data.ScrollView->ScrollbarTrackTexture.guid));
            QueueTexture(resolver->GetPathForGUID(data.ScrollView->ScrollbarThumbTexture.guid));
        }
        if (data.Dropdown) {
            QueueTexture(resolver->GetPathForGUID(data.Dropdown->ArrowTexture.guid));
        }
    }

    if (!data.Scripts.empty()) {
        auto queuePrefabFromValue = [&](const PropertyValue& value) {
            if (std::holds_alternative<std::string>(value)) {
                const std::string& guidStr = std::get<std::string>(value);
                if (!guidStr.empty()) {
                    QueuePrefabGuid(ClaymoreGUID::FromString(guidStr));
                }
            }
        };
        auto queueDialogueFromValue = [&](const PropertyValue& value) {
            if (std::holds_alternative<std::string>(value)) {
                const std::string& guidStr = std::get<std::string>(value);
                if (!guidStr.empty()) {
                    QueueDialogueLibrary(ClaymoreGUID::FromString(guidStr));
                }
            }
        };
        auto queueAnimControllerFromValue = [&](const PropertyValue& value) {
            if (std::holds_alternative<std::string>(value)) {
                const std::string& path = std::get<std::string>(value);
                if (!path.empty()) {
                    QueueAnimationController(path);
                }
            }
        };
        auto handleList = [&](const PropertyValue& value, PropertyType elementType) {
            if (!std::holds_alternative<std::shared_ptr<ListPropertyValue>>(value)) return;
            auto listPtr = std::get<std::shared_ptr<ListPropertyValue>>(value);
            if (!listPtr) return;
            for (const auto& element : listPtr->elements) {
                switch (elementType) {
                    case PropertyType::Prefab:
                        queuePrefabFromValue(element);
                        break;
                    case PropertyType::DialogueLibrary:
                        queueDialogueFromValue(element);
                        break;
                    case PropertyType::AnimatorController:
                        queueAnimControllerFromValue(element);
                        break;
                    case PropertyType::AnimatorControllerOverride:
                        if (std::holds_alternative<std::string>(element)) {
                            QueueAnimationControllerOverride(std::get<std::string>(element));
                        }
                        break;
                    default:
                        break;
                }
            }
        };
        
        for (const auto& script : data.Scripts) {
            if (!ScriptReflection::HasProperties(script.ClassName)) continue;
            const auto& props = ScriptReflection::GetScriptProperties(script.ClassName);
            for (const auto& prop : props) {
                auto it = script.Values.find(prop.name);
                const PropertyValue* value = (it != script.Values.end()) ? &it->second : nullptr;
                
                if (prop.type == PropertyType::Prefab) {
                    if (value) {
                        queuePrefabFromValue(*value);
                    } else {
                        queuePrefabFromValue(prop.currentValue);
                    }
                } else if (prop.type == PropertyType::DialogueLibrary) {
                    if (value) {
                        queueDialogueFromValue(*value);
                    } else {
                        queueDialogueFromValue(prop.currentValue);
                    }
                } else if (prop.type == PropertyType::AnimatorController) {
                    if (value) {
                        queueAnimControllerFromValue(*value);
                    } else {
                        queueAnimControllerFromValue(prop.currentValue);
                    }
                } else if (prop.type == PropertyType::AnimatorControllerOverride) {
                    auto queueAnimOverrideFromValue = [&](const PropertyValue& propValue) {
                        if (std::holds_alternative<std::string>(propValue)) {
                            QueueAnimationControllerOverride(std::get<std::string>(propValue));
                        }
                    };
                    if (value) {
                        queueAnimOverrideFromValue(*value);
                    } else {
                        queueAnimOverrideFromValue(prop.currentValue);
                    }
                } else if (prop.type == PropertyType::List) {
                    PropertyType elemType = prop.listElementType;
                    if (value) {
                        handleList(*value, elemType);
                    } else {
                        handleList(prop.currentValue, elemType);
                    }
                }
            }
        }
    }
    if (data.Emitter) {
        QueueTexture(data.Emitter->SpritePath);
        QueueParticleSprite(data.Emitter->SpritePath);
    }
}

void PrefabPrewarmManager::CollectPrefabDependencies(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(binary::PrefabBinaryHeader)) return;
    
    binary::PrefabBinaryHeader header{};
    std::memcpy(&header, data.data(), sizeof(binary::PrefabBinaryHeader));
    
    if (header.base.magic != binary::PREFAB_MAGIC) return;
    if ((header.base.flags & 1) != 0) {
        // Legacy JSON blob format - skip for now
        return;
    }
    
    ReadContext ctx;
    ctx.data = data.data();
    ctx.dataSize = data.size();
    ctx.stringTableOffset = header.stringTableOffset;
    
    size_t stringTableEnd = data.size();
    if (header.modelDeltaTableOffset > 0 && header.modelDeltaTableOffset < stringTableEnd) {
        stringTableEnd = header.modelDeltaTableOffset;
    }
    ctx.BuildStringTable(stringTableEnd);
    
    bool isV4 = (header.base.version >= 4);
    bool isV3 = (header.base.version >= 3);
    
    size_t offset = sizeof(binary::PrefabBinaryHeader) + 16 + 4;
    
    std::vector<EntityHeader> entityHeaders(header.entityCount);
    for (uint32_t i = 0; i < header.entityCount; ++i) {
        ctx.offset = offset;
        if (isV4) {
            ctx.Read(entityHeaders[i]);
        } else if (isV3) {
            EntityHeaderV3 v3Header;
            ctx.Read(v3Header);
            entityHeaders[i].guidHigh = v3Header.guidHigh;
            entityHeaders[i].guidLow = v3Header.guidLow;
            entityHeaders[i].parentGuidHigh = v3Header.parentGuidHigh;
            entityHeaders[i].parentGuidLow = v3Header.parentGuidLow;
            entityHeaders[i].entityId = v3Header.entityId;
            entityHeaders[i].nameIndex = v3Header.nameIndex;
            entityHeaders[i].flags = 0x03;
            entityHeaders[i].layer = 0;
            entityHeaders[i].tagIndex = 0;
            entityHeaders[i].modelGuidHigh = 0;
            entityHeaders[i].modelGuidLow = 0;
            entityHeaders[i].componentCount = v3Header.componentCount;
            entityHeaders[i].componentOffset = v3Header.componentOffset;
        } else {
            EntityHeaderV2 v2Header;
            ctx.Read(v2Header);
            entityHeaders[i].guidHigh = v2Header.guidHigh;
            entityHeaders[i].guidLow = v2Header.guidLow;
            entityHeaders[i].parentGuidHigh = v2Header.parentGuidHigh;
            entityHeaders[i].parentGuidLow = v2Header.parentGuidLow;
            entityHeaders[i].entityId = static_cast<uint32_t>(i);
            entityHeaders[i].nameIndex = v2Header.nameIndex;
            entityHeaders[i].flags = 0x03;
            entityHeaders[i].layer = 0;
            entityHeaders[i].tagIndex = 0;
            entityHeaders[i].modelGuidHigh = 0;
            entityHeaders[i].modelGuidLow = 0;
            entityHeaders[i].componentCount = v2Header.componentCount;
            entityHeaders[i].componentOffset = v2Header.componentOffset;
        }
        offset = ctx.offset;
    }
    
    for (const auto& eh : entityHeaders) {
        ctx.offset = eh.componentOffset;
        auto entityData = std::make_unique<EntityData>();
        
        for (uint32_t c = 0; c < eh.componentCount; ++c) {
            binary::ComponentEntry entry;
            ctx.Read(entry);
            
            size_t nextCompOffset = ctx.offset + entry.dataSize;
            
            bool shouldParse = false;
            switch (entry.typeId) {
                case binary::ComponentTypeId::Mesh:
                case binary::ComponentTypeId::Skinning:
                case binary::ComponentTypeId::Panel:
                case binary::ComponentTypeId::Button:
                case binary::ComponentTypeId::Slider:
                case binary::ComponentTypeId::ProgressBar:
                case binary::ComponentTypeId::Toggle:
                case binary::ComponentTypeId::ScrollView:
                case binary::ComponentTypeId::Dropdown:
                case binary::ComponentTypeId::ParticleEmitter:
                case binary::ComponentTypeId::AnimationPlayer:
                case binary::ComponentTypeId::Script:
                    shouldParse = true;
                    break;
                default:
                    break;
            }
            
            if (shouldParse) {
                binary::ComponentLoadContext compCtx;
                compCtx.data = ctx.data + ctx.offset;
                compCtx.size = entry.dataSize;
                compCtx.pos = 0;
                compCtx.version = header.base.version;
                compCtx.readString = [&ctx](uint32_t idx) { return ctx.ReadString(idx); };
                compCtx.prewarm = true;
                
                binary::LoadComponentBinary(compCtx, entityData.get(), entry.typeId, entry.dataSize);
            }
            
            ctx.offset = nextCompOffset;
        }
        
        CollectDependenciesFromEntity(*entityData);
    }
}

void PrefabPrewarmManager::Update(double budgetMs) {
    if (!m_Enabled) return;
    if (!Assets::GetResolver()) return;
    if (Assets::GetLoadMode() == AssetLoadMode::Editor) return;
    
    auto start = std::chrono::high_resolution_clock::now();
    auto budget = std::chrono::duration<double, std::milli>(budgetMs);
    
    auto withinBudget = [&]() {
        auto now = std::chrono::high_resolution_clock::now();
        return (now - start) < budget;
    };
    
    while (withinBudget()) {
        ClaymoreGUID prefabGuid{};
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_PrefabQueue.empty()) {
                prefabGuid = m_PrefabQueue.front();
                m_PrefabQueue.pop_front();
            }
        }
        if (prefabGuid.high != 0 || prefabGuid.low != 0) {
            std::string sourcePath = Assets::GetResolver()->GetPathForGUID(prefabGuid);
            if (!sourcePath.empty()) {
                std::string loadPath = sourcePath;
                if (Assets::ShouldLoadBinary()) {
                    std::string binPath = Assets::GetBinaryPath(sourcePath);
                    if (!binPath.empty()) {
                        loadPath = binPath;
                    } else if (!Assets::AllowSourceFallback()) {
                        std::cerr << "[PrefabPrewarm] Missing binary prefab for GUID: " 
                                  << prefabGuid.ToString() << std::endl;
                        continue;
                    }
                }
                
                std::vector<uint8_t> bytes;
                if (FileSystem::Instance().ReadFile(loadPath, bytes)) {
                    CollectPrefabDependencies(bytes);
                }
            }
            continue;
        }
        
        MaterialPrewarmJob matJob;
        bool hasMatJob = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_MaterialQueue.empty()) {
                matJob = m_MaterialQueue.front();
                m_MaterialQueue.pop_front();
                hasMatJob = true;
            }
        }
        if (hasMatJob) {
            RuntimeMaterialCache::GetOrLoad(matJob.path, matJob.skinned);
            continue;
        }
        
        ShaderProgramJob shaderJob;
        bool hasShaderJob = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_ShaderQueue.empty()) {
                shaderJob = m_ShaderQueue.front();
                m_ShaderQueue.pop_front();
                hasShaderJob = true;
            }
        }
        if (hasShaderJob) {
            ShaderManager::Instance().LoadProgram(shaderJob.vs, shaderJob.fs);
            continue;
        }
        
        std::string controllerPath;
        bool hasControllerJob = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_AnimControllerQueue.empty()) {
                controllerPath = m_AnimControllerQueue.front();
                m_AnimControllerQueue.pop_front();
                hasControllerJob = true;
            }
        }
        if (hasControllerJob) {
            auto controller = cm::animation::LoadAnimatorControllerFromFile(controllerPath);
            if (controller) {
                auto queueState = [&](const cm::animation::AnimatorState& state) {
                    QueueAnimationAsset(state.AnimationAssetPath);
                    QueueAnimationAsset(state.ClipPath);
                    for (const auto& entry : state.Blend1DEntries) {
                        QueueAnimationAsset(entry.AssetPath);
                        QueueAnimationAsset(entry.ClipPath);
                    }
                    for (const auto& entry : state.Blend2DEntries) {
                        QueueAnimationAsset(entry.AssetPath);
                        QueueAnimationAsset(entry.ClipPath);
                    }
                };
                if (!controller->Layers.empty()) {
                    for (const auto& layer : controller->Layers) {
                        for (const auto& state : layer.States) {
                            queueState(state);
                        }
                    }
                } else {
                    for (const auto& state : controller->States) {
                        queueState(state);
                    }
                }
            }
            continue;
        }

        std::string overridePath;
        bool hasOverrideJob = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_AnimOverrideQueue.empty()) {
                overridePath = m_AnimOverrideQueue.front();
                m_AnimOverrideQueue.pop_front();
                hasOverrideJob = true;
            }
        }
        if (hasOverrideJob) {
            auto overrideAsset = cm::animation::LoadAnimatorControllerOverrideFromFile(overridePath);
            if (overrideAsset) {
                QueueAnimationController(overrideAsset->BaseControllerPath);
                for (const auto& entry : overrideAsset->Entries) {
                    QueueAnimationAsset(entry.OverridePath);
                }
            }
            continue;
        }
        
        std::string animAssetPath;
        bool hasAnimJob = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_AnimAssetQueue.empty()) {
                animAssetPath = m_AnimAssetQueue.front();
                m_AnimAssetQueue.pop_front();
                hasAnimJob = true;
            }
        }
        if (hasAnimJob) {
            cm::animation::LoadAnimationAssetCached(animAssetPath, true);
            continue;
        }
        
        ClaymoreGUID dialogueGuid{};
        bool hasDialogueJob = false;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_DialogueQueue.empty()) {
                dialogueGuid = m_DialogueQueue.front();
                m_DialogueQueue.pop_front();
                hasDialogueJob = true;
            }
        }
        if (hasDialogueJob) {
            if (dialogueGuid.high != 0 || dialogueGuid.low != 0) {
                if (!Dialogue::DialogueLibraryRegistry::Get().Find(dialogueGuid)) {
                    std::string path = Assets::GetResolver()->GetPathForGUID(dialogueGuid);
                    if (!path.empty()) {
                        auto loaded = Dialogue::DialogueLibrary::LoadFromFile(path);
                        if (loaded) {
                            loaded->SetGuid(dialogueGuid);
                            auto shared = std::shared_ptr<Dialogue::DialogueLibrary>(std::move(loaded));
                            Dialogue::DialogueLibraryRegistry::Get().Register(dialogueGuid, shared);
                        }
                    }
                }
            }
            continue;
        }
        
        std::string spritePath;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_ParticleSpriteQueue.empty()) {
                spritePath = m_ParticleSpriteQueue.front();
                m_ParticleSpriteQueue.pop_front();
            }
        }
        if (!spritePath.empty()) {
            particles::AcquireSprite(spritePath);
            continue;
        }
        
        std::string texturePath;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_TextureQueue.empty()) {
                texturePath = m_TextureQueue.front();
                m_TextureQueue.pop_front();
            }
        }
        if (!texturePath.empty()) {
            TextureSpecifier spec;
            spec.Path = texturePath;
            AcquireTextureHandle(spec, TextureColorSpace::Linear);
            continue;
        }
        
        break;
    }
}

} // namespace

void QueueScenePrefabs(Scene& scene) {
    GetManager().QueueScenePrefabs(scene);
}

void QueuePrefabGuid(const ClaymoreGUID& guid) {
    GetManager().QueuePrefabGuid(guid);
}

void Update(double budgetMs) {
    GetManager().Update(budgetMs);
}

void SetEnabled(bool enabled) {
    GetManager().SetEnabled(enabled);
}

void Clear() {
    GetManager().Clear();
}

} // namespace prefab

