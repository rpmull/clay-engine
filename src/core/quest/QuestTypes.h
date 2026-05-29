#pragma once

#include "../assets/AssetReference.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Quest {

enum class StageState {
    Inactive = 0,
    Active = 1,
    Completed = 2,
    Failed = 3
};

enum class ObjectiveType {
    None = 0,
    Kill = 1,
    Collect = 2,
    Talk = 3,
    ReachLocation = 4,
    Escort = 5,
    Defend = 6,
    Use = 7,
    Custom = 99
};

// Location data for ReachLocation objectives
struct LocationData {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 5.0f;  // Detection radius
    std::string locationName;  // Optional display name
    std::string sceneId;  // Scene/level the location is in
};

// Reward item structure
struct QuestReward {
    std::string rewardId;
    std::string resourceGuid;  // Item/currency GUID
    std::string rewardType;    // "item", "currency", "xp", "reputation"
    int amount = 1;
    bool optional = false;     // Player can choose this reward
};

// Dialogue trigger configuration
struct DialogueTrigger {
    std::string dialogueLibraryGuid;
    std::string entryId;           // Specific dialogue entry
    std::string triggerCondition;  // "on_start", "on_active", "on_complete", "on_fail"
    std::string speakerResourceGuid;  // NPC who speaks this line
};

struct Objective {
    std::string objectiveId;
    ObjectiveType type = ObjectiveType::None;
    std::string displayText;           // UI text like "Kill wolves (0/3)"
    std::string displayTextComplete;   // "Wolves eliminated"
    int targetCount = 1;
    std::string targetResourceGuid;    // Entity type GUID (wolf type, item type, etc.)
    std::string dialogueLibraryGuid;   // For Talk objectives
    std::string dialogueEntryId;       // Specific conversation to trigger
    LocationData location;             // For ReachLocation objectives
    bool optional = false;
    bool hidden = false;               // Don't show in quest log until revealed
    int priority = 0;                  // Display order (higher = first)
    std::string hintText;              // Hint shown in journal
};

struct StageCondition {
    std::string requiredQuestId;
    std::string requiredStageId;
    std::string requiredState;         // "Active", "Completed", "Failed"
    std::string requiredGameState;     // Custom game state key=value
    std::string requiredItemGuid;      // Player must have this item
    int requiredItemCount = 0;
    bool invertCondition = false;      // NOT this condition
};

struct StageOutcome {
    std::string nextStageId;
    std::string branchId;
    std::vector<std::string> unlockQuestIds;
    std::vector<std::string> failQuestIds;  // Quests that become unavailable
    std::vector<QuestReward> rewards;
    bool completeQuest = false;
    bool failQuest = false;
    std::string gameStateKey;          // Set game state on this outcome
    std::string gameStateValue;
};

struct QuestStage {
    std::string stageId;
    std::string name;
    std::string description;
    std::string journalEntry;          // Full journal text for this stage
    StageState initialState = StageState::Inactive;
    std::vector<Objective> objectives;
    std::vector<StageCondition> conditions;
    std::vector<StageOutcome> outcomes;
    std::vector<DialogueTrigger> dialogueTriggers;  // Dialogues available during this stage
    float timeoutSeconds = 0.0f;       // 0 = no timeout
    std::string onTimeoutStageId;      // Stage to go to on timeout
    bool onTimeoutFail = false;
};

struct QuestBranch {
    std::string branchId;
    std::string name;
    std::string description;
    std::string consequenceDescription;  // Long-term effect of this branch
};

struct QuestFlags {
    bool repeatable = false;
    bool failsOnTimeout = false;
    bool hiddenUntilStarted = false;
    bool trackable = true;              // Show in quest tracker
    bool mainQuest = false;             // Main story quest
    int priority = 0;                   // Sort order in quest log
};

// Graph node position for editor visualization
struct QuestGraphNode {
    std::string nodeId;                // stageId or questId
    float x = 0.0f;
    float y = 0.0f;
    std::string nodeType;              // "quest", "stage", "branch"
};

struct QuestRecord {
    std::string questId;
    std::string displayName;
    std::string description;
    std::string category;              // "Main", "Side", "Misc", "Faction"
    std::string questGiver;            // NPC resource GUID who gives quest
    int recommendedLevel = 0;
    QuestFlags flags;
    std::vector<QuestStage> stages;
    std::vector<QuestBranch> branches;
    std::vector<QuestReward> completionRewards;  // Rewards on final completion
    std::vector<DialogueTrigger> globalDialogueTriggers;  // Always available during quest
    std::vector<QuestGraphNode> editorGraphNodes;  // Editor layout data
    std::vector<std::string> prerequisiteQuestIds;  // Quests that must be complete
};

// JSON helpers - Location and Reward types
void to_json(nlohmann::json& j, const LocationData& loc);
void from_json(const nlohmann::json& j, LocationData& loc);
void to_json(nlohmann::json& j, const QuestReward& reward);
void from_json(const nlohmann::json& j, QuestReward& reward);
void to_json(nlohmann::json& j, const DialogueTrigger& trigger);
void from_json(const nlohmann::json& j, DialogueTrigger& trigger);

// JSON helpers - Core types
void to_json(nlohmann::json& j, const Objective& obj);
void from_json(const nlohmann::json& j, Objective& obj);
void to_json(nlohmann::json& j, const StageCondition& cond);
void from_json(const nlohmann::json& j, StageCondition& cond);
void to_json(nlohmann::json& j, const StageOutcome& outcome);
void from_json(const nlohmann::json& j, StageOutcome& outcome);
void to_json(nlohmann::json& j, const QuestStage& stage);
void from_json(const nlohmann::json& j, QuestStage& stage);
void to_json(nlohmann::json& j, const QuestBranch& branch);
void from_json(const nlohmann::json& j, QuestBranch& branch);
void to_json(nlohmann::json& j, const QuestFlags& flags);
void from_json(const nlohmann::json& j, QuestFlags& flags);
void to_json(nlohmann::json& j, const QuestGraphNode& node);
void from_json(const nlohmann::json& j, QuestGraphNode& node);
void to_json(nlohmann::json& j, const QuestRecord& record);
void from_json(const nlohmann::json& j, QuestRecord& record);

// String conversion helpers
std::string StageStateToString(StageState state);
StageState StageStateFromString(const std::string& state);
std::string ObjectiveTypeToString(ObjectiveType type);
ObjectiveType ObjectiveTypeFromString(const std::string& type);

// ID generation helpers
std::string GenerateQuestId();
std::string GenerateStageId();
std::string GenerateObjectiveId();
std::string GenerateBranchId();
std::string GenerateRewardId();

} // namespace Quest

