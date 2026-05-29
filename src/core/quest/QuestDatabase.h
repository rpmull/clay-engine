#pragma once

#include "QuestTypes.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

namespace Quest {

class QuestDatabase {
public:
    QuestDatabase() = default;
    ~QuestDatabase() = default;

    const ClaymoreGUID& GetGuid() const { return m_Guid; }
    void SetGuid(const ClaymoreGUID& guid) { m_Guid = guid; }

    const std::vector<QuestRecord>& GetQuests() const { return m_Quests; }
    std::vector<QuestRecord>& GetQuests() { return m_Quests; }

    QuestRecord* FindQuest(const std::string& questId);
    const QuestRecord* FindQuest(const std::string& questId) const;

    // Serialization
    bool Serialize(nlohmann::json& j) const;
    bool Deserialize(const nlohmann::json& j);

    bool SaveToFile(const std::string& path) const;
    static std::shared_ptr<QuestDatabase> LoadFromFile(const std::string& path);

    // Ensure every quest/stage/objective has an ID
    void EnsureIdentifiers();

private:
    ClaymoreGUID m_Guid{};
    std::vector<QuestRecord> m_Quests;
    mutable std::mutex m_Mutex;
};

// Registry for loaded quest databases (mirrors dialogue registry style)
class QuestDatabaseRegistry {
public:
    static QuestDatabaseRegistry& Get();

    void Register(const ClaymoreGUID& guid, std::shared_ptr<QuestDatabase> db);
    void Unregister(const ClaymoreGUID& guid);
    std::shared_ptr<QuestDatabase> Find(const ClaymoreGUID& guid) const;
    void Clear();

private:
    QuestDatabaseRegistry() = default;
    mutable std::mutex m_Mutex;
    std::unordered_map<ClaymoreGUID, std::shared_ptr<QuestDatabase>> m_ByGuid;
};

} // namespace Quest

