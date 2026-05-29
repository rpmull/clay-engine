#pragma once

#include "DialogueTypes.h"
#include "DialogueParser.h"
#include "../assets/AssetReference.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <future>
#include <atomic>

namespace Dialogue {

//------------------------------------------------------------------------------
// DialogueLibrary - Asset containing multiple dialogue entries for a character
//------------------------------------------------------------------------------
class DialogueLibrary {
public:
    DialogueLibrary() = default;
    ~DialogueLibrary() = default;
    
    // Asset identification
    const ClaymoreGUID& GetGuid() const { return m_Guid; }
    void SetGuid(const ClaymoreGUID& guid) { m_Guid = guid; }
    
    const std::string& GetCharacterId() const { return m_CharacterId; }
    void SetCharacterId(const std::string& id) { m_CharacterId = id; }
    
    const std::string& GetDisplayName() const { return m_DisplayName; }
    void SetDisplayName(const std::string& name) { m_DisplayName = name; }
    
    // Entry management
    void AddEntry(const DialogueEntry& entry);
    void RemoveEntry(const std::string& entryId);
    DialogueEntry* GetEntry(const std::string& entryId);
    const DialogueEntry* GetEntry(const std::string& entryId) const;
    const std::vector<DialogueEntry>& GetEntries() const { return m_Entries; }
    std::vector<DialogueEntry>& GetEntries() { return m_Entries; }
    
    // Get the appropriate conversation based on conditions
    using QuestStatusGetter = std::function<std::string(const std::string&, const std::string&)>;
    std::shared_ptr<Conversation> GetConversation(
        const std::unordered_map<std::string, std::string>& state,
        QuestStatusGetter questStatusGetter = nullptr) const;
    
    // Get default conversation
    std::shared_ptr<Conversation> GetDefaultConversation() const;
    
    // Serialization
    bool Serialize(nlohmann::json& j) const;
    bool Deserialize(const nlohmann::json& j);
    
    // File I/O (synchronous)
    bool SaveToFile(const std::string& path) const;
    static std::unique_ptr<DialogueLibrary> LoadFromFile(const std::string& path);
    
    // Async loading support
    using LoadCallback = std::function<void(std::unique_ptr<DialogueLibrary>)>;
    static void LoadFromFileAsync(const std::string& path, LoadCallback callback);
    
    // Load from file on a thread pool (returns future)
    static std::future<std::unique_ptr<DialogueLibrary>> LoadFromFileAsyncFuture(const std::string& path);
    
    // Parse dialogue text on a background thread
    static std::future<std::shared_ptr<Conversation>> ParseTextAsync(const std::string& text);
    
    // Change notification
    void NotifyChanged();
    using ChangeCallback = std::function<void(DialogueLibrary&)>;
    void SetOnChanged(ChangeCallback cb) { m_OnChanged = std::move(cb); }
    
private:
    // Internal helper - assumes mutex is already held
    std::shared_ptr<Conversation> GetDefaultConversationUnlocked() const;
    
    ClaymoreGUID m_Guid{};
    std::string m_CharacterId;
    std::string m_DisplayName;
    std::vector<DialogueEntry> m_Entries;
    mutable std::mutex m_Mutex;
    ChangeCallback m_OnChanged;
};

//------------------------------------------------------------------------------
// DialogueLibraryRegistry - Global registry of loaded dialogue libraries
//------------------------------------------------------------------------------
class DialogueLibraryRegistry {
public:
    static DialogueLibraryRegistry& Get();
    
    // Register/unregister libraries
    void Register(const ClaymoreGUID& guid, std::shared_ptr<DialogueLibrary> library);
    void Unregister(const ClaymoreGUID& guid);
    
    // Lookup
    std::shared_ptr<DialogueLibrary> Find(const ClaymoreGUID& guid) const;
    std::shared_ptr<DialogueLibrary> FindByCharacterId(const std::string& characterId) const;
    
    // Get all libraries
    std::vector<std::shared_ptr<DialogueLibrary>> GetAll() const;
    
    // Clear all
    void Clear();
    
private:
    DialogueLibraryRegistry() = default;
    
    mutable std::mutex m_Mutex;
    std::unordered_map<ClaymoreGUID, std::shared_ptr<DialogueLibrary>> m_ByGuid;
    std::unordered_map<std::string, ClaymoreGUID> m_ByCharacterId;
};

//------------------------------------------------------------------------------
// DialogueLibraryRef - Serializable reference to a dialogue library
//------------------------------------------------------------------------------
struct DialogueLibraryRef {
    ClaymoreGUID guid{};
    
    bool IsValid() const { return guid.high != 0 || guid.low != 0; }
    std::shared_ptr<DialogueLibrary> Resolve() const;
    
    // JSON serialization
    friend void to_json(nlohmann::json& j, const DialogueLibraryRef& ref);
    friend void from_json(const nlohmann::json& j, DialogueLibraryRef& ref);
};

} // namespace Dialogue

// JSON serialization for DialogueEntry
namespace Dialogue {
    void to_json(nlohmann::json& j, const DialogueEntry& entry);
    void from_json(const nlohmann::json& j, DialogueEntry& entry);
    
    void to_json(nlohmann::json& j, const DialogueCondition& cond);
    void from_json(const nlohmann::json& j, DialogueCondition& cond);
}

