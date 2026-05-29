#pragma once

#include <cstdint>

namespace QuestInterop {

void Initialize();
void Shutdown();

extern "C" {
    // Database management
    const char* Quest_LoadDatabase(const char* filePath);
    void Quest_UnloadDatabase(const char* guidStr);
    
    // Core operations
    bool Quest_StartQuest(const char* questId);
    bool Quest_AdvanceStage(const char* questId, const char* stageId);
    bool Quest_CompleteStage(const char* questId, const char* stageId);
    bool Quest_FailQuest(const char* questId);
    bool Quest_AbandonQuest(const char* questId);
    bool Quest_UpdateObjectiveProgress(const char* questId, const char* stageId, const char* objectiveId, int delta);
    bool Quest_SetObjectiveProgress(const char* questId, const char* stageId, const char* objectiveId, int value);
    
    // State queries
    int Quest_GetQuestState(const char* questId);
    int Quest_GetStageState(const char* questId, const char* stageId);
    const char* Quest_GetActiveStageId(const char* questId);
    int Quest_GetObjectiveProgress(const char* questId, const char* stageId, const char* objectiveId);
    int Quest_GetObjectiveTarget(const char* questId, const char* stageId, const char* objectiveId);
    bool Quest_IsObjectiveComplete(const char* questId, const char* stageId, const char* objectiveId);
    
    // Bulk queries (returns count, fills buffer with comma-separated IDs)
    int Quest_GetActiveQuestCount();
    const char* Quest_GetActiveQuestIds();
    int Quest_GetCompletedQuestCount();
    const char* Quest_GetCompletedQuestIds();
    
    // Branch history
    int Quest_GetBranchHistoryCount(const char* questId);
    const char* Quest_GetBranchHistory(const char* questId);
    
    // Condition checking
    bool Quest_CanStartQuest(const char* questId);
    bool Quest_AreStageConditionsMet(const char* questId, const char* stageId);
    
    // Dialogue integration
    bool Quest_IsDialogueTriggerActive(const char* questId, const char* dialogueLibraryGuid, const char* entryId);
    
    // Save/Load
    const char* Quest_SerializeState();
    bool Quest_DeserializeState(const char* jsonStr);
    void Quest_ResetAllState();
}

// Callback signatures
typedef void (*QuestStartedCallback)(const char* questId);
typedef void (*QuestStageStartedCallback)(const char* questId, const char* stageId);
typedef void (*QuestStageCompletedCallback)(const char* questId, const char* stageId);
typedef void (*QuestCompletedCallback)(const char* questId);
typedef void (*QuestFailedCallback)(const char* questId);
typedef void (*QuestObjectiveProgressCallback)(const char* questId, const char* stageId, const char* objectiveId, int current, int target);

extern "C" {
    void Quest_SetOnQuestStarted(QuestStartedCallback cb);
    void Quest_SetOnStageStarted(QuestStageStartedCallback cb);
    void Quest_SetOnStageCompleted(QuestStageCompletedCallback cb);
    void Quest_SetOnQuestCompleted(QuestCompletedCallback cb);
    void Quest_SetOnQuestFailed(QuestFailedCallback cb);
    void Quest_SetOnObjectiveProgress(QuestObjectiveProgressCallback cb);
}

} // namespace QuestInterop

// Getter exports for managed bootstrap
extern "C" {
    // Core functions
    void* Get_Quest_LoadDatabase_Ptr();
    void* Get_Quest_UnloadDatabase_Ptr();
    void* Get_Quest_StartQuest_Ptr();
    void* Get_Quest_AdvanceStage_Ptr();
    void* Get_Quest_CompleteStage_Ptr();
    void* Get_Quest_FailQuest_Ptr();
    void* Get_Quest_AbandonQuest_Ptr();
    void* Get_Quest_UpdateObjectiveProgress_Ptr();
    void* Get_Quest_SetObjectiveProgress_Ptr();
    
    // State queries
    void* Get_Quest_GetQuestState_Ptr();
    void* Get_Quest_GetStageState_Ptr();
    void* Get_Quest_GetActiveStageId_Ptr();
    void* Get_Quest_GetObjectiveProgress_Ptr();
    void* Get_Quest_GetObjectiveTarget_Ptr();
    void* Get_Quest_IsObjectiveComplete_Ptr();
    
    // Bulk queries
    void* Get_Quest_GetActiveQuestCount_Ptr();
    void* Get_Quest_GetActiveQuestIds_Ptr();
    void* Get_Quest_GetCompletedQuestCount_Ptr();
    void* Get_Quest_GetCompletedQuestIds_Ptr();
    void* Get_Quest_GetBranchHistoryCount_Ptr();
    void* Get_Quest_GetBranchHistory_Ptr();
    
    // Conditions
    void* Get_Quest_CanStartQuest_Ptr();
    void* Get_Quest_AreStageConditionsMet_Ptr();
    void* Get_Quest_IsDialogueTriggerActive_Ptr();
    
    // Save/Load
    void* Get_Quest_SerializeState_Ptr();
    void* Get_Quest_DeserializeState_Ptr();
    void* Get_Quest_ResetAllState_Ptr();
    
    // Callbacks
    void* Get_Quest_SetOnQuestStarted_Ptr();
    void* Get_Quest_SetOnStageStarted_Ptr();
    void* Get_Quest_SetOnStageCompleted_Ptr();
    void* Get_Quest_SetOnQuestCompleted_Ptr();
    void* Get_Quest_SetOnQuestFailed_Ptr();
    void* Get_Quest_SetOnObjectiveProgress_Ptr();
}

