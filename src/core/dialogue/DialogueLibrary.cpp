#include "DialogueLibrary.h"
#include "core/vfs/FileSystem.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <thread>
#include <cctype>

namespace Dialogue {

static uint64_t Fnv1a64(const std::string& data, uint64_t seed) {
    uint64_t hash = 14695981039346656037ULL ^ seed;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static ClaymoreGUID DeterministicGuidFromPath(const std::string& path) {
    if (path.empty()) return ClaymoreGUID();

    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    uint64_t high = Fnv1a64(normalized, 0);
    uint64_t low = Fnv1a64(normalized, 0x9e3779b97f4a7c15ULL);
    return ClaymoreGUID(high, low);
}

//------------------------------------------------------------------------------
// DialogueLibrary implementation
//------------------------------------------------------------------------------

void DialogueLibrary::AddEntry(const DialogueEntry& entry) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Check for existing entry with same ID
    for (auto& e : m_Entries) {
        if (e.entryId == entry.entryId) {
            e = entry;
            NotifyChanged();
            return;
        }
    }
    
    m_Entries.push_back(entry);
    NotifyChanged();
}

void DialogueLibrary::RemoveEntry(const std::string& entryId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    for (auto it = m_Entries.begin(); it != m_Entries.end(); ++it) {
        if (it->entryId == entryId) {
            m_Entries.erase(it);
            NotifyChanged();
            return;
        }
    }
}

DialogueEntry* DialogueLibrary::GetEntry(const std::string& entryId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    for (auto& e : m_Entries) {
        if (e.entryId == entryId) return &e;
    }
    return nullptr;
}

const DialogueEntry* DialogueLibrary::GetEntry(const std::string& entryId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    for (const auto& e : m_Entries) {
        if (e.entryId == entryId) return &e;
    }
    return nullptr;
}

std::shared_ptr<Conversation> DialogueLibrary::GetConversation(
    const std::unordered_map<std::string, std::string>& state,
    QuestStatusGetter questStatusGetter) const 
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // First, try to find a conditional entry that matches
    for (const auto& entry : m_Entries) {
        if (entry.isDefault) continue;
        
        if (entry.condition.Evaluate(state, questStatusGetter)) {
            return entry.GetConversation();
        }
    }
    
    // Fall back to default (use internal unlocked version since we already hold the lock)
    return GetDefaultConversationUnlocked();
}

std::shared_ptr<Conversation> DialogueLibrary::GetDefaultConversation() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return GetDefaultConversationUnlocked();
}

std::shared_ptr<Conversation> DialogueLibrary::GetDefaultConversationUnlocked() const {
    // Internal helper - assumes mutex is already held by caller
    for (const auto& entry : m_Entries) {
        if (entry.isDefault) {
            return entry.GetConversation();
        }
    }
    
    // If no default, return first entry
    if (!m_Entries.empty()) {
        return m_Entries[0].GetConversation();
    }
    
    return nullptr;
}

void DialogueLibrary::NotifyChanged() {
    if (m_OnChanged) {
        m_OnChanged(*this);
    }
}

//------------------------------------------------------------------------------
// Serialization
//------------------------------------------------------------------------------

void to_json(nlohmann::json& j, const DialogueCondition& cond) {
    j = nlohmann::json{
        {"requiredQuestId", cond.requiredQuestId},
        {"requiredStepId", cond.requiredStepId},
        {"requiredStepStatus", cond.requiredStepStatus},
        {"requiredStateKey", cond.requiredStateKey},
        {"requiredStateValue", cond.requiredStateValue}
    };
}

void from_json(const nlohmann::json& j, DialogueCondition& cond) {
    if (j.contains("requiredQuestId")) j.at("requiredQuestId").get_to(cond.requiredQuestId);
    if (j.contains("requiredStepId")) j.at("requiredStepId").get_to(cond.requiredStepId);
    if (j.contains("requiredStepStatus")) j.at("requiredStepStatus").get_to(cond.requiredStepStatus);
    if (j.contains("requiredStateKey")) j.at("requiredStateKey").get_to(cond.requiredStateKey);
    if (j.contains("requiredStateValue")) j.at("requiredStateValue").get_to(cond.requiredStateValue);
}

void to_json(nlohmann::json& j, const DialogueEntry& entry) {
    j = nlohmann::json{
        {"entryId", entry.entryId},
        {"displayName", entry.displayName},
        {"rawText", entry.rawText},
        {"isDefault", entry.isDefault},
        {"condition", entry.condition}
    };
}

void from_json(const nlohmann::json& j, DialogueEntry& entry) {
    if (j.contains("entryId")) j.at("entryId").get_to(entry.entryId);
    if (j.contains("displayName")) j.at("displayName").get_to(entry.displayName);
    if (j.contains("rawText")) j.at("rawText").get_to(entry.rawText);
    if (j.contains("isDefault")) j.at("isDefault").get_to(entry.isDefault);
    if (j.contains("condition")) j.at("condition").get_to(entry.condition);
    entry.InvalidateCache();
}

bool DialogueLibrary::Serialize(nlohmann::json& j) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    // Caller should ensure a GUID exists; keep a guard to avoid writing empty IDs
    if (m_Guid == ClaymoreGUID()) {
        std::cerr << "[DialogueLibrary] Serialize called with empty GUID" << std::endl;
    }

    j["_type"] = "DialogueLibrary";
    j["_version"] = 1;
    j["guid"] = m_Guid.ToString();
    j["characterId"] = m_CharacterId;
    j["displayName"] = m_DisplayName;
    j["entries"] = m_Entries;
    
    return true;
}

bool DialogueLibrary::Deserialize(const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    try {
        if (j.contains("guid")) {
            std::string guidStr = j.at("guid").get<std::string>();
            m_Guid = ClaymoreGUID::FromString(guidStr);
        }
        
        if (j.contains("characterId")) j.at("characterId").get_to(m_CharacterId);
        if (j.contains("displayName")) j.at("displayName").get_to(m_DisplayName);
        
        if (j.contains("entries")) {
            m_Entries.clear();
            for (const auto& entryJson : j.at("entries")) {
                DialogueEntry entry;
                from_json(entryJson, entry);
                m_Entries.push_back(std::move(entry));
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[DialogueLibrary] Deserialize error: " << e.what() << std::endl;
        return false;
    }
}

bool DialogueLibrary::SaveToFile(const std::string& path) const {
    try {
        if (m_Guid == ClaymoreGUID()) {
            ClaymoreGUID fallback = DeterministicGuidFromPath(path);
            if (fallback != ClaymoreGUID()) {
                const_cast<DialogueLibrary*>(this)->SetGuid(fallback);
            }
        }

        nlohmann::json j;
        if (!Serialize(j)) return false;
        
        std::ofstream file(path);
        if (!file.is_open()) {
            std::cerr << "[DialogueLibrary] Failed to open file for writing: " << path << std::endl;
            return false;
        }
        
        file << j.dump(2);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[DialogueLibrary] Save error: " << e.what() << std::endl;
        return false;
    }
}

std::unique_ptr<DialogueLibrary> DialogueLibrary::LoadFromFile(const std::string& path) {
    try {
        std::string jsonText;
        
        // Use VFS for both runtime (PAK) and editor (disk) modes
        if (!FileSystem::Instance().ReadTextFile(path, jsonText)) {
            // Fallback to direct file access for absolute paths (editor mode)
            if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
                std::cerr << "[DialogueLibrary] Failed to read file from VFS: " << path << std::endl;
                return nullptr;
            }
            std::ifstream file(path);
            if (!file.is_open()) {
                std::cerr << "[DialogueLibrary] Failed to open file: " << path << std::endl;
                return nullptr;
            }
            std::stringstream ss;
            ss << file.rdbuf();
            jsonText = ss.str();
        }
        
        nlohmann::json j = nlohmann::json::parse(jsonText);
        
        auto library = std::make_unique<DialogueLibrary>();
        if (!library->Deserialize(j)) {
            return nullptr;
        }

        if (library->GetGuid() == ClaymoreGUID()) {
            ClaymoreGUID fallback = DeterministicGuidFromPath(path);
            if (fallback != ClaymoreGUID()) {
                library->SetGuid(fallback);
            }
        }
        
        return library;
    } catch (const std::exception& e) {
        std::cerr << "[DialogueLibrary] Load error: " << e.what() << std::endl;
        return nullptr;
    }
}

//------------------------------------------------------------------------------
// Async loading support
//------------------------------------------------------------------------------

void DialogueLibrary::LoadFromFileAsync(const std::string& path, LoadCallback callback) {
    // Launch on a detached thread (in production, use a thread pool)
    std::thread([path, callback = std::move(callback)]() {
        auto library = LoadFromFile(path);
        
        // Callback should be invoked on the main thread in production
        // For now, call directly (UI code should handle thread safety)
        if (callback) {
            callback(std::move(library));
        }
    }).detach();
}

std::future<std::unique_ptr<DialogueLibrary>> DialogueLibrary::LoadFromFileAsyncFuture(const std::string& path) {
    return std::async(std::launch::async, [path]() {
        return LoadFromFile(path);
    });
}

std::future<std::shared_ptr<Conversation>> DialogueLibrary::ParseTextAsync(const std::string& text) {
    return std::async(std::launch::async, [text]() {
        return DialogueParser::Parse(text);
    });
}

//------------------------------------------------------------------------------
// DialogueLibraryRef
//------------------------------------------------------------------------------

std::shared_ptr<DialogueLibrary> DialogueLibraryRef::Resolve() const {
    if (!IsValid()) return nullptr;
    return DialogueLibraryRegistry::Get().Find(guid);
}

void to_json(nlohmann::json& j, const DialogueLibraryRef& ref) {
    j = ref.guid.ToString();
}

void from_json(const nlohmann::json& j, DialogueLibraryRef& ref) {
    if (j.is_string()) {
        ref.guid = ClaymoreGUID::FromString(j.get<std::string>());
    }
}

//------------------------------------------------------------------------------
// DialogueLibraryRegistry
//------------------------------------------------------------------------------

DialogueLibraryRegistry& DialogueLibraryRegistry::Get() {
    static DialogueLibraryRegistry instance;
    return instance;
}

void DialogueLibraryRegistry::Register(const ClaymoreGUID& guid, std::shared_ptr<DialogueLibrary> library) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    m_ByGuid[guid] = library;
    
    if (!library->GetCharacterId().empty()) {
        m_ByCharacterId[library->GetCharacterId()] = guid;
    }
    
    std::cout << "[DialogueLibrary] Registered: " << guid.ToString() 
              << " (" << library->GetDisplayName() << ")" << std::endl;
}

void DialogueLibraryRegistry::Unregister(const ClaymoreGUID& guid) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_ByGuid.find(guid);
    if (it != m_ByGuid.end()) {
        // Remove character ID mapping
        const std::string& charId = it->second->GetCharacterId();
        if (!charId.empty()) {
            m_ByCharacterId.erase(charId);
        }
        m_ByGuid.erase(it);
    }
}

std::shared_ptr<DialogueLibrary> DialogueLibraryRegistry::Find(const ClaymoreGUID& guid) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_ByGuid.find(guid);
    return it != m_ByGuid.end() ? it->second : nullptr;
}

std::shared_ptr<DialogueLibrary> DialogueLibraryRegistry::FindByCharacterId(const std::string& characterId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    auto it = m_ByCharacterId.find(characterId);
    if (it == m_ByCharacterId.end()) return nullptr;
    
    auto libIt = m_ByGuid.find(it->second);
    return libIt != m_ByGuid.end() ? libIt->second : nullptr;
}

std::vector<std::shared_ptr<DialogueLibrary>> DialogueLibraryRegistry::GetAll() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    
    std::vector<std::shared_ptr<DialogueLibrary>> result;
    result.reserve(m_ByGuid.size());
    for (const auto& [guid, lib] : m_ByGuid) {
        result.push_back(lib);
    }
    return result;
}

void DialogueLibraryRegistry::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ByGuid.clear();
    m_ByCharacterId.clear();
}

} // namespace Dialogue

