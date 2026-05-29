#pragma once

#include "QuestDatabase.h"
#include <functional>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>

namespace Quest {

enum class QuestState {
    Inactive = 0,
    Active = 1,
    Completed = 2,
    Failed = 3
};

// Extended callbacks for full quest lifecycle
struct QuestCallbacks {
    std::function<void(const std::string&)> onQuestStarted;
    std::function<void(const std::string&, const std::string&)> onStageStarted;
    std::function<void(const std::string&, const std::string&)> onStageCompleted;
    std::function<void(const std::string&)> onQuestCompleted;
    std::function<void(const std::string&)> onQuestFailed;
    std::function<void(const std::string&, const std::string&, const std::string&, int, int)> onObjectiveProgress; // quest, stage, obj, current, target
    std::function<void(const std::string&, const std::string&)> onBranchTaken; // quest, branch
    std::function<void(const std::vector<QuestReward>&)> onRewardsGranted;
};

// Callback signature for checking if player has an item (for condition checking)
using HasItemCallback = std::function<bool(const std::string& itemGuid, int count)>;
// Callback signature for getting game state values
using GetGameStateCallback = std::function<std::string(const std::string& key)>;

struct ObjectiveRuntimeState {
    int currentCount = 0;
    bool revealed = false;  // For hidden objectives
};

struct StageRuntimeState {
    StageState state = StageState::Inactive;
    std::unordered_map<std::string, ObjectiveRuntimeState> objectiveProgress;
    float elapsedTime = 0.0f;  // For timeout tracking
};

struct QuestRuntimeState {
    QuestState state = QuestState::Inactive;
    std::unordered_map<std::string, StageRuntimeState> stages;
    std::vector<std::string> branchHistory;
    std::string activeStageId;
    float startTime = 0.0f;
};

class QuestSystem {
public:
    QuestSystem() = default;
    ~QuestSystem() = default;

    // Database management
    void SetDatabase(std::shared_ptr<QuestDatabase> db) { m_Database = std::move(db); }
    std::shared_ptr<QuestDatabase> GetDatabase() const { return m_Database; }

    // Callback registration
    void SetCallbacks(QuestCallbacks cb) { m_Callbacks = std::move(cb); }
    const QuestCallbacks& GetCallbacks() const { return m_Callbacks; }
    void SetHasItemCallback(HasItemCallback cb) { m_HasItemCallback = std::move(cb); }
    void SetGameStateCallback(GetGameStateCallback cb) { m_GameStateCallback = std::move(cb); }

    // Core quest operations
    bool StartQuest(const std::string& questId);
    bool AdvanceStage(const std::string& questId, const std::string& stageId);
    bool CompleteStage(const std::string& questId, const std::string& stageId);
    bool FailQuest(const std::string& questId);
    bool AbandonQuest(const std::string& questId);
    bool UpdateObjectiveProgress(const std::string& questId, const std::string& stageId, const std::string& objectiveId, int delta);
    bool SetObjectiveProgress(const std::string& questId, const std::string& stageId, const std::string& objectiveId, int value);

    // State queries
    QuestState GetQuestState(const std::string& questId) const;
    StageState GetStageState(const std::string& questId, const std::string& stageId) const;
    std::string GetActiveStageId(const std::string& questId) const;
    int GetObjectiveProgress(const std::string& questId, const std::string& stageId, const std::string& objectiveId) const;
    int GetObjectiveTarget(const std::string& questId, const std::string& stageId, const std::string& objectiveId) const;
    bool IsObjectiveComplete(const std::string& questId, const std::string& stageId, const std::string& objectiveId) const;
    std::vector<std::string> GetBranchHistory(const std::string& questId) const;

    // Bulk queries for UI
    std::vector<std::string> GetActiveQuestIds() const;
    std::vector<std::string> GetCompletedQuestIds() const;
    std::vector<std::string> GetFailedQuestIds() const;
    std::vector<std::string> GetAllTrackedQuestIds() const;

    // Condition checking
    bool CanStartQuest(const std::string& questId) const;
    bool AreStageConditionsMet(const std::string& questId, const std::string& stageId) const;
    bool CheckCondition(const StageCondition& condition) const;

    // Dialogue integration - check if a dialogue should be available
    bool IsDialogueTriggerActive(const std::string& questId, const std::string& dialogueLibraryGuid, const std::string& entryId) const;
    std::vector<DialogueTrigger> GetActiveDialogueTriggers(const std::string& questId) const;
    std::vector<DialogueTrigger> GetActiveDialogueTriggersForNpc(const std::string& npcResourceGuid) const;

    // Time-based updates (call from game loop)
    void Update(float deltaTime);

    // Save/Load state
    bool SerializeState(nlohmann::json& j) const;
    bool DeserializeState(const nlohmann::json& j);
    void ResetAllState();

    // Debug/Editor
    const std::unordered_map<std::string, QuestRuntimeState>& GetAllRuntimeState() const { return m_Runtime; }
    void ForceSetQuestState(const std::string& questId, QuestState state);
    void ForceSetStageState(const std::string& questId, const std::string& stageId, StageState state);

private:
    StageRuntimeState* GetStageRuntime(const std::string& questId, const std::string& stageId);
    const StageRuntimeState* GetStageRuntime(const std::string& questId, const std::string& stageId) const;
    bool ActivateStageInternal(const QuestRecord& quest, QuestRuntimeState& runtime, const std::string& stageId);
    void ProcessOutcome(const QuestRecord& quest, QuestRuntimeState& runtime, const StageOutcome& outcome);
    void GrantRewards(const std::vector<QuestReward>& rewards);
    const QuestStage* FindStage(const QuestRecord& quest, const std::string& stageId) const;
    const Objective* FindObjective(const QuestStage& stage, const std::string& objectiveId) const;
    void CheckStageTimeout(const std::string& questId, QuestRuntimeState& runtime, float deltaTime);

    mutable std::mutex m_Mutex;
    std::shared_ptr<QuestDatabase> m_Database;
    std::unordered_map<std::string, QuestRuntimeState> m_Runtime;
    QuestCallbacks m_Callbacks;
    HasItemCallback m_HasItemCallback;
    GetGameStateCallback m_GameStateCallback;
};

QuestSystem& GetGlobalQuestSystem();

// Utility functions for dialogue/quest integration
std::string QuestStateToString(QuestState state);
QuestState QuestStateFromString(const std::string& str);

} // namespace Quest

