#include "QuestSystem.h"
#include <algorithm>
#include <iostream>

namespace Quest {

static QuestSystem g_GlobalQuestSystem;

QuestSystem& GetGlobalQuestSystem() {
    return g_GlobalQuestSystem;
}

std::string QuestStateToString(QuestState state) {
    switch (state) {
    case QuestState::Active: return "Active";
    case QuestState::Completed: return "Completed";
    case QuestState::Failed: return "Failed";
    default: return "Inactive";
    }
}

QuestState QuestStateFromString(const std::string& str) {
    if (str == "Active") return QuestState::Active;
    if (str == "Completed") return QuestState::Completed;
    if (str == "Failed") return QuestState::Failed;
    return QuestState::Inactive;
}

//------------------------------------------------------------------------------
// Helper methods
//------------------------------------------------------------------------------
StageRuntimeState* QuestSystem::GetStageRuntime(const std::string& questId, const std::string& stageId) {
    auto itQuest = m_Runtime.find(questId);
    if (itQuest == m_Runtime.end()) return nullptr;
    auto itStage = itQuest->second.stages.find(stageId);
    if (itStage == itQuest->second.stages.end()) return nullptr;
    return &itStage->second;
}

const StageRuntimeState* QuestSystem::GetStageRuntime(const std::string& questId, const std::string& stageId) const {
    auto itQuest = m_Runtime.find(questId);
    if (itQuest == m_Runtime.end()) return nullptr;
    auto itStage = itQuest->second.stages.find(stageId);
    if (itStage == itQuest->second.stages.end()) return nullptr;
    return &itStage->second;
}

const QuestStage* QuestSystem::FindStage(const QuestRecord& quest, const std::string& stageId) const {
    for (const auto& s : quest.stages) {
        if (s.stageId == stageId) return &s;
    }
    return nullptr;
}

const Objective* QuestSystem::FindObjective(const QuestStage& stage, const std::string& objectiveId) const {
    for (const auto& o : stage.objectives) {
        if (o.objectiveId == objectiveId) return &o;
    }
    return nullptr;
}

bool QuestSystem::ActivateStageInternal(const QuestRecord& quest, QuestRuntimeState& runtime, const std::string& stageId) {
    const QuestStage* stageDef = FindStage(quest, stageId);
    if (!stageDef) return false;

    auto& stageRuntime = runtime.stages[stageId];
    stageRuntime.state = StageState::Active;
    stageRuntime.objectiveProgress.clear();
    stageRuntime.elapsedTime = 0.0f;

    for (const auto& obj : stageDef->objectives) {
        ObjectiveRuntimeState objState;
        objState.currentCount = 0;
        objState.revealed = !obj.hidden;
        stageRuntime.objectiveProgress[obj.objectiveId] = objState;
    }

    runtime.activeStageId = stageId;

    if (m_Callbacks.onStageStarted) {
        m_Callbacks.onStageStarted(quest.questId, stageId);
    }

    return true;
}

void QuestSystem::ProcessOutcome(const QuestRecord& quest, QuestRuntimeState& runtime, const StageOutcome& outcome) {
    // Record branch if specified
    if (!outcome.branchId.empty()) {
        runtime.branchHistory.push_back(outcome.branchId);
        if (m_Callbacks.onBranchTaken) {
            m_Callbacks.onBranchTaken(quest.questId, outcome.branchId);
        }
    }

    // Set game state if specified
    // Note: This would integrate with a game state system
    // For now we just note it happened

    // Grant rewards
    if (!outcome.rewards.empty()) {
        GrantRewards(outcome.rewards);
    }
}

void QuestSystem::GrantRewards(const std::vector<QuestReward>& rewards) {
    if (m_Callbacks.onRewardsGranted && !rewards.empty()) {
        m_Callbacks.onRewardsGranted(rewards);
    }
}

//------------------------------------------------------------------------------
// Condition checking
//------------------------------------------------------------------------------
bool QuestSystem::CheckCondition(const StageCondition& condition) const {
    bool result = true;

    // Check required quest state
    if (!condition.requiredQuestId.empty()) {
        QuestState qState = GetQuestState(condition.requiredQuestId);
        StageState expectedState = StageStateFromString(condition.requiredState);
        
        if (!condition.requiredStageId.empty()) {
            StageState sState = GetStageState(condition.requiredQuestId, condition.requiredStageId);
            result = result && (sState == expectedState);
        } else {
            // Check quest-level state
            result = result && (static_cast<int>(qState) == static_cast<int>(expectedState));
        }
    }

    // Check game state
    if (!condition.requiredGameState.empty() && m_GameStateCallback) {
        auto eqPos = condition.requiredGameState.find('=');
        if (eqPos != std::string::npos) {
            std::string key = condition.requiredGameState.substr(0, eqPos);
            std::string expectedValue = condition.requiredGameState.substr(eqPos + 1);
            std::string actualValue = m_GameStateCallback(key);
            result = result && (actualValue == expectedValue);
        }
    }

    // Check item requirement
    if (!condition.requiredItemGuid.empty() && m_HasItemCallback) {
        result = result && m_HasItemCallback(condition.requiredItemGuid, condition.requiredItemCount);
    }

    // Apply inversion if needed
    return condition.invertCondition ? !result : result;
}

bool QuestSystem::AreStageConditionsMet(const std::string& questId, const std::string& stageId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return false;

    const QuestStage* stage = FindStage(*quest, stageId);
    if (!stage) return false;

    for (const auto& cond : stage->conditions) {
        if (!CheckCondition(cond)) return false;
    }
    return true;
}

bool QuestSystem::CanStartQuest(const std::string& questId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return false;

    // Check if already active or completed (and not repeatable)
    auto it = m_Runtime.find(questId);
    if (it != m_Runtime.end()) {
        if (it->second.state == QuestState::Active) return false;
        if (it->second.state == QuestState::Completed && !quest->flags.repeatable) return false;
    }

    // Check prerequisites
    for (const auto& prereqId : quest->prerequisiteQuestIds) {
        QuestState prereqState = GetQuestState(prereqId);
        if (prereqState != QuestState::Completed) return false;
    }

    return true;
}

//------------------------------------------------------------------------------
// Core quest operations
//------------------------------------------------------------------------------
bool QuestSystem::StartQuest(const std::string& questId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) {
        std::cerr << "[QuestSystem] Quest not found: " << questId << std::endl;
        return false;
    }

    auto& runtime = m_Runtime[questId];
    if (runtime.state == QuestState::Completed && !quest->flags.repeatable) return false;

    runtime.state = QuestState::Active;
    runtime.stages.clear();
    runtime.branchHistory.clear();
    runtime.activeStageId.clear();
    runtime.startTime = 0.0f;

    // Activate first stage
    if (!quest->stages.empty()) {
        ActivateStageInternal(*quest, runtime, quest->stages.front().stageId);
    }

    if (m_Callbacks.onQuestStarted) m_Callbacks.onQuestStarted(questId);
    return true;
}

bool QuestSystem::AdvanceStage(const std::string& questId, const std::string& stageId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return false;

    auto& runtime = m_Runtime[questId];
    runtime.state = QuestState::Active;
    return ActivateStageInternal(*quest, runtime, stageId);
}

bool QuestSystem::CompleteStage(const std::string& questId, const std::string& stageId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return false;

    auto& runtime = m_Runtime[questId];
    auto& stageRuntime = runtime.stages[stageId];
    stageRuntime.state = StageState::Completed;

    const QuestStage* stageDef = FindStage(*quest, stageId);
    if (!stageDef) return false;

    if (m_Callbacks.onStageCompleted) {
        m_Callbacks.onStageCompleted(questId, stageId);
    }

    // Process outcomes
    for (const auto& outcome : stageDef->outcomes) {
        ProcessOutcome(*quest, runtime, outcome);

        if (outcome.failQuest) {
            runtime.activeStageId.clear();
            runtime.state = QuestState::Failed;
            if (m_Callbacks.onQuestFailed) m_Callbacks.onQuestFailed(questId);
            return true;
        }

        if (outcome.completeQuest) {
            runtime.activeStageId.clear();
            runtime.state = QuestState::Completed;
            GrantRewards(quest->completionRewards);
            if (m_Callbacks.onQuestCompleted) m_Callbacks.onQuestCompleted(questId);
            return true;
        }

        if (!outcome.nextStageId.empty()) {
            ActivateStageInternal(*quest, runtime, outcome.nextStageId);
            break; // Only process first outcome with next stage
        }
    }

    return true;
}

bool QuestSystem::FailQuest(const std::string& questId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto itQuest = m_Runtime.find(questId);
    if (itQuest == m_Runtime.end()) return false;

    itQuest->second.state = QuestState::Failed;
    itQuest->second.activeStageId.clear();
    if (m_Callbacks.onQuestFailed) m_Callbacks.onQuestFailed(questId);
    return true;
}

bool QuestSystem::AbandonQuest(const std::string& questId) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Runtime.find(questId);
    if (it == m_Runtime.end()) return false;

    it->second.state = QuestState::Inactive;
    it->second.activeStageId.clear();
    it->second.stages.clear();
    return true;
}

bool QuestSystem::UpdateObjectiveProgress(const std::string& questId, const std::string& stageId, const std::string& objectiveId, int delta) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return false;

    const QuestStage* stageDef = FindStage(*quest, stageId);
    if (!stageDef) return false;

    auto* stageRuntime = GetStageRuntime(questId, stageId);
    if (!stageRuntime) {
        auto& runtime = m_Runtime[questId];
        ActivateStageInternal(*quest, runtime, stageId);
        stageRuntime = GetStageRuntime(questId, stageId);
    }
    if (!stageRuntime) return false;
    stageRuntime->state = StageState::Active;

    auto itObj = stageRuntime->objectiveProgress.find(objectiveId);
    if (itObj == stageRuntime->objectiveProgress.end()) {
        stageRuntime->objectiveProgress[objectiveId] = ObjectiveRuntimeState{};
        itObj = stageRuntime->objectiveProgress.find(objectiveId);
    }
    itObj->second.currentCount += delta;
    itObj->second.revealed = true; // Reveal on progress

    const Objective* objDef = FindObjective(*stageDef, objectiveId);
    int targetCount = objDef ? std::max(1, objDef->targetCount) : 1;

    if (m_Callbacks.onObjectiveProgress) {
        m_Callbacks.onObjectiveProgress(questId, stageId, objectiveId, itObj->second.currentCount, targetCount);
    }

    // Check completion (all non-optional objectives reached targetCount)
    bool allComplete = true;
    for (const auto& obj : stageDef->objectives) {
        if (obj.optional) continue;
        int target = std::max(1, obj.targetCount);
        auto progressIt = stageRuntime->objectiveProgress.find(obj.objectiveId);
        int current = (progressIt != stageRuntime->objectiveProgress.end()) ? progressIt->second.currentCount : 0;
        if (current < target) {
            allComplete = false;
            break;
        }
    }

    if (allComplete) {
        // Need to release lock before calling CompleteStage
        m_Mutex.unlock();
        bool result = CompleteStage(questId, stageId);
        m_Mutex.lock();
        return result;
    }
    return true;
}

bool QuestSystem::SetObjectiveProgress(const std::string& questId, const std::string& stageId, const std::string& objectiveId, int value) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    auto* stageRuntime = GetStageRuntime(questId, stageId);
    if (!stageRuntime) return false;

    auto itObj = stageRuntime->objectiveProgress.find(objectiveId);
    if (itObj == stageRuntime->objectiveProgress.end()) {
        stageRuntime->objectiveProgress[objectiveId] = ObjectiveRuntimeState{};
        itObj = stageRuntime->objectiveProgress.find(objectiveId);
    }

    int delta = value - itObj->second.currentCount;
    itObj->second.currentCount = value;
    itObj->second.revealed = true;

    if (delta != 0) {
        const QuestRecord* quest = m_Database->FindQuest(questId);
        if (quest) {
            const QuestStage* stageDef = FindStage(*quest, stageId);
            if (stageDef) {
                const Objective* objDef = FindObjective(*stageDef, objectiveId);
                int targetCount = objDef ? std::max(1, objDef->targetCount) : 1;
                if (m_Callbacks.onObjectiveProgress) {
                    m_Callbacks.onObjectiveProgress(questId, stageId, objectiveId, value, targetCount);
                }
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------------
// State queries
//------------------------------------------------------------------------------
QuestState QuestSystem::GetQuestState(const std::string& questId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Runtime.find(questId);
    if (it == m_Runtime.end()) return QuestState::Inactive;
    return it->second.state;
}

StageState QuestSystem::GetStageState(const std::string& questId, const std::string& stageId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto* stage = GetStageRuntime(questId, stageId);
    if (!stage) return StageState::Inactive;
    return stage->state;
}

std::string QuestSystem::GetActiveStageId(const std::string& questId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Runtime.find(questId);
    if (it == m_Runtime.end()) return "";
    return it->second.activeStageId;
}

int QuestSystem::GetObjectiveProgress(const std::string& questId, const std::string& stageId, const std::string& objectiveId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto* stage = GetStageRuntime(questId, stageId);
    if (!stage) return 0;
    auto it = stage->objectiveProgress.find(objectiveId);
    if (it == stage->objectiveProgress.end()) return 0;
    return it->second.currentCount;
}

int QuestSystem::GetObjectiveTarget(const std::string& questId, const std::string& stageId, const std::string& objectiveId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return 1;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return 1;

    const QuestStage* stageDef = FindStage(*quest, stageId);
    if (!stageDef) return 1;

    const Objective* objDef = FindObjective(*stageDef, objectiveId);
    return objDef ? std::max(1, objDef->targetCount) : 1;
}

bool QuestSystem::IsObjectiveComplete(const std::string& questId, const std::string& stageId, const std::string& objectiveId) const {
    int progress = GetObjectiveProgress(questId, stageId, objectiveId);
    int target = GetObjectiveTarget(questId, stageId, objectiveId);
    return progress >= target;
}

std::vector<std::string> QuestSystem::GetBranchHistory(const std::string& questId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Runtime.find(questId);
    if (it == m_Runtime.end()) return {};
    return it->second.branchHistory;
}

std::vector<std::string> QuestSystem::GetActiveQuestIds() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<std::string> result;
    for (const auto& [id, state] : m_Runtime) {
        if (state.state == QuestState::Active) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<std::string> QuestSystem::GetCompletedQuestIds() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<std::string> result;
    for (const auto& [id, state] : m_Runtime) {
        if (state.state == QuestState::Completed) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<std::string> QuestSystem::GetFailedQuestIds() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<std::string> result;
    for (const auto& [id, state] : m_Runtime) {
        if (state.state == QuestState::Failed) {
            result.push_back(id);
        }
    }
    return result;
}

std::vector<std::string> QuestSystem::GetAllTrackedQuestIds() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<std::string> result;
    for (const auto& [id, state] : m_Runtime) {
        if (state.state != QuestState::Inactive) {
            result.push_back(id);
        }
    }
    return result;
}

//------------------------------------------------------------------------------
// Dialogue integration
//------------------------------------------------------------------------------
bool QuestSystem::IsDialogueTriggerActive(const std::string& questId, const std::string& dialogueLibraryGuid, const std::string& entryId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return false;

    auto it = m_Runtime.find(questId);
    if (it == m_Runtime.end() || it->second.state != QuestState::Active) return false;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return false;

    // Check global dialogue triggers
    for (const auto& trigger : quest->globalDialogueTriggers) {
        if (trigger.dialogueLibraryGuid == dialogueLibraryGuid &&
            (trigger.entryId.empty() || trigger.entryId == entryId)) {
            return true;
        }
    }

    // Check active stage dialogue triggers
    const std::string& activeStage = it->second.activeStageId;
    if (!activeStage.empty()) {
        const QuestStage* stage = FindStage(*quest, activeStage);
        if (stage) {
            for (const auto& trigger : stage->dialogueTriggers) {
                if (trigger.dialogueLibraryGuid == dialogueLibraryGuid &&
                    (trigger.entryId.empty() || trigger.entryId == entryId)) {
                    // Check trigger condition
                    if (trigger.triggerCondition == "on_active" ||
                        trigger.triggerCondition.empty()) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

std::vector<DialogueTrigger> QuestSystem::GetActiveDialogueTriggers(const std::string& questId) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<DialogueTrigger> result;

    if (!m_Database) return result;

    auto it = m_Runtime.find(questId);
    if (it == m_Runtime.end() || it->second.state != QuestState::Active) return result;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return result;

    // Add global triggers
    for (const auto& trigger : quest->globalDialogueTriggers) {
        result.push_back(trigger);
    }

    // Add active stage triggers
    const std::string& activeStage = it->second.activeStageId;
    if (!activeStage.empty()) {
        const QuestStage* stage = FindStage(*quest, activeStage);
        if (stage) {
            for (const auto& trigger : stage->dialogueTriggers) {
                if (trigger.triggerCondition == "on_active" || trigger.triggerCondition.empty()) {
                    result.push_back(trigger);
                }
            }
        }
    }

    return result;
}

std::vector<DialogueTrigger> QuestSystem::GetActiveDialogueTriggersForNpc(const std::string& npcResourceGuid) const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    std::vector<DialogueTrigger> result;

    if (!m_Database) return result;

    // Check all active quests for dialogue triggers with this NPC
    for (const auto& [questId, runtime] : m_Runtime) {
        if (runtime.state != QuestState::Active) continue;

        const QuestRecord* quest = m_Database->FindQuest(questId);
        if (!quest) continue;

        // Check global triggers
        for (const auto& trigger : quest->globalDialogueTriggers) {
            if (trigger.speakerResourceGuid == npcResourceGuid) {
                result.push_back(trigger);
            }
        }

        // Check active stage triggers
        if (!runtime.activeStageId.empty()) {
            const QuestStage* stage = FindStage(*quest, runtime.activeStageId);
            if (stage) {
                for (const auto& trigger : stage->dialogueTriggers) {
                    if (trigger.speakerResourceGuid == npcResourceGuid &&
                        (trigger.triggerCondition == "on_active" || trigger.triggerCondition.empty())) {
                        result.push_back(trigger);
                    }
                }
            }
        }
    }

    return result;
}

//------------------------------------------------------------------------------
// Time-based updates
//------------------------------------------------------------------------------
void QuestSystem::Update(float deltaTime) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Database) return;

    for (auto& [questId, runtime] : m_Runtime) {
        if (runtime.state != QuestState::Active) continue;
        CheckStageTimeout(questId, runtime, deltaTime);
    }
}

void QuestSystem::CheckStageTimeout(const std::string& questId, QuestRuntimeState& runtime, float deltaTime) {
    if (runtime.activeStageId.empty()) return;

    const QuestRecord* quest = m_Database->FindQuest(questId);
    if (!quest) return;

    const QuestStage* stage = FindStage(*quest, runtime.activeStageId);
    if (!stage || stage->timeoutSeconds <= 0.0f) return;

    auto& stageRuntime = runtime.stages[runtime.activeStageId];
    stageRuntime.elapsedTime += deltaTime;

    if (stageRuntime.elapsedTime >= stage->timeoutSeconds) {
        if (stage->onTimeoutFail) {
            runtime.state = QuestState::Failed;
            runtime.activeStageId.clear();
            if (m_Callbacks.onQuestFailed) m_Callbacks.onQuestFailed(questId);
        } else if (!stage->onTimeoutStageId.empty()) {
            ActivateStageInternal(*quest, runtime, stage->onTimeoutStageId);
        }
    }
}

//------------------------------------------------------------------------------
// Save/Load
//------------------------------------------------------------------------------
bool QuestSystem::SerializeState(nlohmann::json& j) const {
    std::lock_guard<std::mutex> lock(m_Mutex);

    j["_type"] = "QuestSystemState";
    j["version"] = 1;

    nlohmann::json questsJson = nlohmann::json::array();
    for (const auto& [questId, runtime] : m_Runtime) {
        nlohmann::json qj;
        qj["questId"] = questId;
        qj["state"] = QuestStateToString(runtime.state);
        qj["activeStageId"] = runtime.activeStageId;
        qj["branchHistory"] = runtime.branchHistory;
        qj["startTime"] = runtime.startTime;

        nlohmann::json stagesJson = nlohmann::json::array();
        for (const auto& [stageId, stageRuntime] : runtime.stages) {
            nlohmann::json sj;
            sj["stageId"] = stageId;
            sj["state"] = StageStateToString(stageRuntime.state);
            sj["elapsedTime"] = stageRuntime.elapsedTime;

            nlohmann::json objsJson = nlohmann::json::array();
            for (const auto& [objId, objState] : stageRuntime.objectiveProgress) {
                nlohmann::json oj;
                oj["objectiveId"] = objId;
                oj["currentCount"] = objState.currentCount;
                oj["revealed"] = objState.revealed;
                objsJson.push_back(oj);
            }
            sj["objectives"] = objsJson;
            stagesJson.push_back(sj);
        }
        qj["stages"] = stagesJson;
        questsJson.push_back(qj);
    }
    j["quests"] = questsJson;

    return true;
}

bool QuestSystem::DeserializeState(const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (!j.contains("quests") || !j["quests"].is_array()) return false;

    m_Runtime.clear();

    for (const auto& qj : j["quests"]) {
        std::string questId = qj.value("questId", "");
        if (questId.empty()) continue;

        QuestRuntimeState runtime;
        runtime.state = QuestStateFromString(qj.value("state", "Inactive"));
        runtime.activeStageId = qj.value("activeStageId", "");
        runtime.startTime = qj.value("startTime", 0.0f);

        if (qj.contains("branchHistory") && qj["branchHistory"].is_array()) {
            for (const auto& b : qj["branchHistory"]) {
                runtime.branchHistory.push_back(b.get<std::string>());
            }
        }

        if (qj.contains("stages") && qj["stages"].is_array()) {
            for (const auto& sj : qj["stages"]) {
                std::string stageId = sj.value("stageId", "");
                if (stageId.empty()) continue;

                StageRuntimeState stageRuntime;
                stageRuntime.state = StageStateFromString(sj.value("state", "Inactive"));
                stageRuntime.elapsedTime = sj.value("elapsedTime", 0.0f);

                if (sj.contains("objectives") && sj["objectives"].is_array()) {
                    for (const auto& oj : sj["objectives"]) {
                        std::string objId = oj.value("objectiveId", "");
                        if (objId.empty()) continue;

                        ObjectiveRuntimeState objState;
                        objState.currentCount = oj.value("currentCount", 0);
                        objState.revealed = oj.value("revealed", true);
                        stageRuntime.objectiveProgress[objId] = objState;
                    }
                }

                runtime.stages[stageId] = stageRuntime;
            }
        }

        m_Runtime[questId] = runtime;
    }

    return true;
}

void QuestSystem::ResetAllState() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Runtime.clear();
}

//------------------------------------------------------------------------------
// Debug/Editor
//------------------------------------------------------------------------------
void QuestSystem::ForceSetQuestState(const std::string& questId, QuestState state) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Runtime[questId].state = state;
}

void QuestSystem::ForceSetStageState(const std::string& questId, const std::string& stageId, StageState state) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Runtime[questId].stages[stageId].state = state;
}

} // namespace Quest
