#include "QuestDatabase.h"
#include "core/vfs/FileSystem.h"
#include <fstream>
#include <iostream>
#include <sstream>

namespace Quest {

//------------------------------------------------------------------------------
// JSON helpers
//------------------------------------------------------------------------------
std::string StageStateToString(StageState state) {
    switch (state) {
    case StageState::Inactive: return "Inactive";
    case StageState::Active: return "Active";
    case StageState::Completed: return "Completed";
    case StageState::Failed: return "Failed";
    default: return "Inactive";
    }
}

StageState StageStateFromString(const std::string& state) {
    if (state == "Active") return StageState::Active;
    if (state == "Completed") return StageState::Completed;
    if (state == "Failed") return StageState::Failed;
    return StageState::Inactive;
}

std::string ObjectiveTypeToString(ObjectiveType type) {
    switch (type) {
    case ObjectiveType::Kill: return "Kill";
    case ObjectiveType::Collect: return "Collect";
    case ObjectiveType::Talk: return "Talk";
    case ObjectiveType::ReachLocation: return "ReachLocation";
    case ObjectiveType::Escort: return "Escort";
    case ObjectiveType::Defend: return "Defend";
    case ObjectiveType::Use: return "Use";
    case ObjectiveType::Custom: return "Custom";
    default: return "None";
    }
}

ObjectiveType ObjectiveTypeFromString(const std::string& type) {
    if (type == "Kill") return ObjectiveType::Kill;
    if (type == "Collect") return ObjectiveType::Collect;
    if (type == "Talk") return ObjectiveType::Talk;
    if (type == "ReachLocation") return ObjectiveType::ReachLocation;
    if (type == "Escort") return ObjectiveType::Escort;
    if (type == "Defend") return ObjectiveType::Defend;
    if (type == "Use") return ObjectiveType::Use;
    if (type == "Custom") return ObjectiveType::Custom;
    return ObjectiveType::None;
}

std::string GenerateQuestId() {
    return "Q_" + ClaymoreGUID::Generate().ToString();
}

std::string GenerateStageId() {
    return "QS_" + ClaymoreGUID::Generate().ToString();
}

std::string GenerateObjectiveId() {
    return "QO_" + ClaymoreGUID::Generate().ToString();
}

std::string GenerateBranchId() {
    return "QB_" + ClaymoreGUID::Generate().ToString();
}

std::string GenerateRewardId() {
    return "QR_" + ClaymoreGUID::Generate().ToString();
}

// LocationData serialization
void to_json(nlohmann::json& j, const LocationData& loc) {
    j = nlohmann::json{
        {"x", loc.x},
        {"y", loc.y},
        {"z", loc.z},
        {"radius", loc.radius},
        {"locationName", loc.locationName},
        {"sceneId", loc.sceneId}
    };
}

void from_json(const nlohmann::json& j, LocationData& loc) {
    if (j.contains("x")) j.at("x").get_to(loc.x);
    if (j.contains("y")) j.at("y").get_to(loc.y);
    if (j.contains("z")) j.at("z").get_to(loc.z);
    if (j.contains("radius")) j.at("radius").get_to(loc.radius);
    if (j.contains("locationName")) j.at("locationName").get_to(loc.locationName);
    if (j.contains("sceneId")) j.at("sceneId").get_to(loc.sceneId);
}

// QuestReward serialization
void to_json(nlohmann::json& j, const QuestReward& reward) {
    j = nlohmann::json{
        {"rewardId", reward.rewardId},
        {"resourceGuid", reward.resourceGuid},
        {"rewardType", reward.rewardType},
        {"amount", reward.amount},
        {"optional", reward.optional}
    };
}

void from_json(const nlohmann::json& j, QuestReward& reward) {
    if (j.contains("rewardId")) j.at("rewardId").get_to(reward.rewardId);
    if (j.contains("resourceGuid")) j.at("resourceGuid").get_to(reward.resourceGuid);
    if (j.contains("rewardType")) j.at("rewardType").get_to(reward.rewardType);
    if (j.contains("amount")) j.at("amount").get_to(reward.amount);
    if (j.contains("optional")) j.at("optional").get_to(reward.optional);
}

// DialogueTrigger serialization
void to_json(nlohmann::json& j, const DialogueTrigger& trigger) {
    j = nlohmann::json{
        {"dialogueLibraryGuid", trigger.dialogueLibraryGuid},
        {"entryId", trigger.entryId},
        {"triggerCondition", trigger.triggerCondition},
        {"speakerResourceGuid", trigger.speakerResourceGuid}
    };
}

void from_json(const nlohmann::json& j, DialogueTrigger& trigger) {
    if (j.contains("dialogueLibraryGuid")) j.at("dialogueLibraryGuid").get_to(trigger.dialogueLibraryGuid);
    if (j.contains("entryId")) j.at("entryId").get_to(trigger.entryId);
    if (j.contains("triggerCondition")) j.at("triggerCondition").get_to(trigger.triggerCondition);
    if (j.contains("speakerResourceGuid")) j.at("speakerResourceGuid").get_to(trigger.speakerResourceGuid);
}

// QuestGraphNode serialization
void to_json(nlohmann::json& j, const QuestGraphNode& node) {
    j = nlohmann::json{
        {"nodeId", node.nodeId},
        {"x", node.x},
        {"y", node.y},
        {"nodeType", node.nodeType}
    };
}

void from_json(const nlohmann::json& j, QuestGraphNode& node) {
    if (j.contains("nodeId")) j.at("nodeId").get_to(node.nodeId);
    if (j.contains("x")) j.at("x").get_to(node.x);
    if (j.contains("y")) j.at("y").get_to(node.y);
    if (j.contains("nodeType")) j.at("nodeType").get_to(node.nodeType);
}

void to_json(nlohmann::json& j, const Objective& obj) {
    j = nlohmann::json{
        {"objectiveId", obj.objectiveId},
        {"type", ObjectiveTypeToString(obj.type)},
        {"displayText", obj.displayText},
        {"displayTextComplete", obj.displayTextComplete},
        {"targetCount", obj.targetCount},
        {"targetResourceGuid", obj.targetResourceGuid},
        {"dialogueLibraryGuid", obj.dialogueLibraryGuid},
        {"dialogueEntryId", obj.dialogueEntryId},
        {"location", obj.location},
        {"optional", obj.optional},
        {"hidden", obj.hidden},
        {"priority", obj.priority},
        {"hintText", obj.hintText}
    };
}

void from_json(const nlohmann::json& j, Objective& obj) {
    if (j.contains("objectiveId")) j.at("objectiveId").get_to(obj.objectiveId);
    if (j.contains("type")) obj.type = ObjectiveTypeFromString(j.value("type", "None"));
    if (j.contains("displayText")) j.at("displayText").get_to(obj.displayText);
    if (j.contains("displayTextComplete")) j.at("displayTextComplete").get_to(obj.displayTextComplete);
    if (j.contains("targetCount")) j.at("targetCount").get_to(obj.targetCount);
    if (j.contains("targetResourceGuid")) j.at("targetResourceGuid").get_to(obj.targetResourceGuid);
    if (j.contains("dialogueLibraryGuid")) j.at("dialogueLibraryGuid").get_to(obj.dialogueLibraryGuid);
    if (j.contains("dialogueEntryId")) j.at("dialogueEntryId").get_to(obj.dialogueEntryId);
    if (j.contains("location")) j.at("location").get_to(obj.location);
    if (j.contains("optional")) j.at("optional").get_to(obj.optional);
    if (j.contains("hidden")) j.at("hidden").get_to(obj.hidden);
    if (j.contains("priority")) j.at("priority").get_to(obj.priority);
    if (j.contains("hintText")) j.at("hintText").get_to(obj.hintText);
}

void to_json(nlohmann::json& j, const StageCondition& cond) {
    j = nlohmann::json{
        {"requiredQuestId", cond.requiredQuestId},
        {"requiredStageId", cond.requiredStageId},
        {"requiredState", cond.requiredState},
        {"requiredGameState", cond.requiredGameState},
        {"requiredItemGuid", cond.requiredItemGuid},
        {"requiredItemCount", cond.requiredItemCount},
        {"invertCondition", cond.invertCondition}
    };
}

void from_json(const nlohmann::json& j, StageCondition& cond) {
    if (j.contains("requiredQuestId")) j.at("requiredQuestId").get_to(cond.requiredQuestId);
    if (j.contains("requiredStageId")) j.at("requiredStageId").get_to(cond.requiredStageId);
    if (j.contains("requiredState")) j.at("requiredState").get_to(cond.requiredState);
    if (j.contains("requiredGameState")) j.at("requiredGameState").get_to(cond.requiredGameState);
    if (j.contains("requiredItemGuid")) j.at("requiredItemGuid").get_to(cond.requiredItemGuid);
    if (j.contains("requiredItemCount")) j.at("requiredItemCount").get_to(cond.requiredItemCount);
    if (j.contains("invertCondition")) j.at("invertCondition").get_to(cond.invertCondition);
}

void to_json(nlohmann::json& j, const StageOutcome& outcome) {
    j = nlohmann::json{
        {"nextStageId", outcome.nextStageId},
        {"branchId", outcome.branchId},
        {"unlockQuestIds", outcome.unlockQuestIds},
        {"failQuestIds", outcome.failQuestIds},
        {"rewards", outcome.rewards},
        {"completeQuest", outcome.completeQuest},
        {"failQuest", outcome.failQuest},
        {"gameStateKey", outcome.gameStateKey},
        {"gameStateValue", outcome.gameStateValue}
    };
}

void from_json(const nlohmann::json& j, StageOutcome& outcome) {
    if (j.contains("nextStageId")) j.at("nextStageId").get_to(outcome.nextStageId);
    if (j.contains("branchId")) j.at("branchId").get_to(outcome.branchId);
    if (j.contains("unlockQuestIds")) j.at("unlockQuestIds").get_to(outcome.unlockQuestIds);
    if (j.contains("failQuestIds")) j.at("failQuestIds").get_to(outcome.failQuestIds);
    if (j.contains("rewards")) j.at("rewards").get_to(outcome.rewards);
    if (j.contains("completeQuest")) j.at("completeQuest").get_to(outcome.completeQuest);
    if (j.contains("failQuest")) j.at("failQuest").get_to(outcome.failQuest);
    if (j.contains("gameStateKey")) j.at("gameStateKey").get_to(outcome.gameStateKey);
    if (j.contains("gameStateValue")) j.at("gameStateValue").get_to(outcome.gameStateValue);
}

void to_json(nlohmann::json& j, const QuestStage& stage) {
    j = nlohmann::json{
        {"stageId", stage.stageId},
        {"name", stage.name},
        {"description", stage.description},
        {"journalEntry", stage.journalEntry},
        {"initialState", StageStateToString(stage.initialState)},
        {"objectives", stage.objectives},
        {"conditions", stage.conditions},
        {"outcomes", stage.outcomes},
        {"dialogueTriggers", stage.dialogueTriggers},
        {"timeoutSeconds", stage.timeoutSeconds},
        {"onTimeoutStageId", stage.onTimeoutStageId},
        {"onTimeoutFail", stage.onTimeoutFail}
    };
}

void from_json(const nlohmann::json& j, QuestStage& stage) {
    if (j.contains("stageId")) j.at("stageId").get_to(stage.stageId);
    if (j.contains("name")) j.at("name").get_to(stage.name);
    if (j.contains("description")) j.at("description").get_to(stage.description);
    if (j.contains("journalEntry")) j.at("journalEntry").get_to(stage.journalEntry);
    if (j.contains("initialState")) stage.initialState = StageStateFromString(j.value("initialState", "Inactive"));
    if (j.contains("objectives")) j.at("objectives").get_to(stage.objectives);
    if (j.contains("conditions")) j.at("conditions").get_to(stage.conditions);
    if (j.contains("outcomes")) j.at("outcomes").get_to(stage.outcomes);
    if (j.contains("dialogueTriggers")) j.at("dialogueTriggers").get_to(stage.dialogueTriggers);
    if (j.contains("timeoutSeconds")) j.at("timeoutSeconds").get_to(stage.timeoutSeconds);
    if (j.contains("onTimeoutStageId")) j.at("onTimeoutStageId").get_to(stage.onTimeoutStageId);
    if (j.contains("onTimeoutFail")) j.at("onTimeoutFail").get_to(stage.onTimeoutFail);
}

void to_json(nlohmann::json& j, const QuestBranch& branch) {
    j = nlohmann::json{
        {"branchId", branch.branchId},
        {"name", branch.name},
        {"description", branch.description},
        {"consequenceDescription", branch.consequenceDescription}
    };
}

void from_json(const nlohmann::json& j, QuestBranch& branch) {
    if (j.contains("branchId")) j.at("branchId").get_to(branch.branchId);
    if (j.contains("name")) j.at("name").get_to(branch.name);
    if (j.contains("description")) j.at("description").get_to(branch.description);
    if (j.contains("consequenceDescription")) j.at("consequenceDescription").get_to(branch.consequenceDescription);
}

void to_json(nlohmann::json& j, const QuestFlags& flags) {
    j = nlohmann::json{
        {"repeatable", flags.repeatable},
        {"failsOnTimeout", flags.failsOnTimeout},
        {"hiddenUntilStarted", flags.hiddenUntilStarted},
        {"trackable", flags.trackable},
        {"mainQuest", flags.mainQuest},
        {"priority", flags.priority}
    };
}

void from_json(const nlohmann::json& j, QuestFlags& flags) {
    if (j.contains("repeatable")) j.at("repeatable").get_to(flags.repeatable);
    if (j.contains("failsOnTimeout")) j.at("failsOnTimeout").get_to(flags.failsOnTimeout);
    if (j.contains("hiddenUntilStarted")) j.at("hiddenUntilStarted").get_to(flags.hiddenUntilStarted);
    if (j.contains("trackable")) j.at("trackable").get_to(flags.trackable);
    if (j.contains("mainQuest")) j.at("mainQuest").get_to(flags.mainQuest);
    if (j.contains("priority")) j.at("priority").get_to(flags.priority);
}

void to_json(nlohmann::json& j, const QuestRecord& record) {
    j = nlohmann::json{
        {"questId", record.questId},
        {"displayName", record.displayName},
        {"description", record.description},
        {"category", record.category},
        {"questGiver", record.questGiver},
        {"recommendedLevel", record.recommendedLevel},
        {"flags", record.flags},
        {"stages", record.stages},
        {"branches", record.branches},
        {"completionRewards", record.completionRewards},
        {"globalDialogueTriggers", record.globalDialogueTriggers},
        {"editorGraphNodes", record.editorGraphNodes},
        {"prerequisiteQuestIds", record.prerequisiteQuestIds}
    };
}

void from_json(const nlohmann::json& j, QuestRecord& record) {
    if (j.contains("questId")) j.at("questId").get_to(record.questId);
    if (j.contains("displayName")) j.at("displayName").get_to(record.displayName);
    if (j.contains("description")) j.at("description").get_to(record.description);
    if (j.contains("category")) j.at("category").get_to(record.category);
    if (j.contains("questGiver")) j.at("questGiver").get_to(record.questGiver);
    if (j.contains("recommendedLevel")) j.at("recommendedLevel").get_to(record.recommendedLevel);
    if (j.contains("flags")) j.at("flags").get_to(record.flags);
    if (j.contains("stages")) j.at("stages").get_to(record.stages);
    if (j.contains("branches")) j.at("branches").get_to(record.branches);
    if (j.contains("completionRewards")) j.at("completionRewards").get_to(record.completionRewards);
    if (j.contains("globalDialogueTriggers")) j.at("globalDialogueTriggers").get_to(record.globalDialogueTriggers);
    if (j.contains("editorGraphNodes")) j.at("editorGraphNodes").get_to(record.editorGraphNodes);
    if (j.contains("prerequisiteQuestIds")) j.at("prerequisiteQuestIds").get_to(record.prerequisiteQuestIds);
}

//------------------------------------------------------------------------------
// QuestDatabase
//------------------------------------------------------------------------------

QuestRecord* QuestDatabase::FindQuest(const std::string& questId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (auto& q : m_Quests) {
        if (q.questId == questId) return &q;
    }
    return nullptr;
}

const QuestRecord* QuestDatabase::FindQuest(const std::string& questId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    for (const auto& q : m_Quests) {
        if (q.questId == questId) return &q;
    }
    return nullptr;
}

bool QuestDatabase::Serialize(nlohmann::json& j) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    j["_type"] = "QuestDatabase";
    j["guid"] = m_Guid.ToString();
    j["quests"] = m_Quests;
    return true;
}

bool QuestDatabase::Deserialize(const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (j.contains("guid")) {
        m_Guid = ClaymoreGUID::FromString(j.value("guid", ""));
    }
    if (j.contains("quests")) {
        m_Quests = j.at("quests").get<std::vector<QuestRecord>>();
    }
    EnsureIdentifiers();
    return true;
}

bool QuestDatabase::SaveToFile(const std::string& path) const {
    nlohmann::json j;
    if (!Serialize(j)) return false;
    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << "[QuestDatabase] Failed to open for save: " << path << std::endl;
        return false;
    }
    out << j.dump(2);
    return true;
}

std::shared_ptr<QuestDatabase> QuestDatabase::LoadFromFile(const std::string& path) {
    std::string text;
    if (!FileSystem::Instance().ReadTextFile(path, text)) {
        if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
            std::cerr << "[QuestDatabase] Failed to read file from VFS: " << path << std::endl;
            return nullptr;
        }
        std::ifstream in(path);
        if (!in.is_open()) {
            std::cerr << "[QuestDatabase] Failed to open: " << path << std::endl;
            return nullptr;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    nlohmann::json j = nlohmann::json::parse(text);
    auto db = std::make_shared<QuestDatabase>();
    if (!db->Deserialize(j)) return nullptr;
    if (db->GetGuid() == ClaymoreGUID()) {
        db->SetGuid(ClaymoreGUID::Generate());
    }
    return db;
}

void QuestDatabase::EnsureIdentifiers() {
    for (auto& q : m_Quests) {
        if (q.questId.empty()) q.questId = GenerateQuestId();
        for (auto& s : q.stages) {
            if (s.stageId.empty()) s.stageId = GenerateStageId();
            for (auto& o : s.objectives) {
                if (o.objectiveId.empty()) o.objectiveId = GenerateObjectiveId();
            }
            for (auto& outcome : s.outcomes) {
                for (auto& reward : outcome.rewards) {
                    if (reward.rewardId.empty()) reward.rewardId = GenerateRewardId();
                }
            }
        }
        for (auto& b : q.branches) {
            if (b.branchId.empty()) b.branchId = GenerateBranchId();
        }
        for (auto& reward : q.completionRewards) {
            if (reward.rewardId.empty()) reward.rewardId = GenerateRewardId();
        }
    }
}

//------------------------------------------------------------------------------
// Registry
//------------------------------------------------------------------------------
QuestDatabaseRegistry& QuestDatabaseRegistry::Get() {
    static QuestDatabaseRegistry instance;
    return instance;
}

void QuestDatabaseRegistry::Register(const ClaymoreGUID& guid, std::shared_ptr<QuestDatabase> db) {
    if (!db) return;
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ByGuid[guid] = std::move(db);
}

void QuestDatabaseRegistry::Unregister(const ClaymoreGUID& guid) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ByGuid.erase(guid);
}

std::shared_ptr<QuestDatabase> QuestDatabaseRegistry::Find(const ClaymoreGUID& guid) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_ByGuid.find(guid);
    if (it != m_ByGuid.end()) return it->second;
    return nullptr;
}

void QuestDatabaseRegistry::Clear() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_ByGuid.clear();
}

} // namespace Quest

