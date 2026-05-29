#include "QuestInterop.h"
#include "../../core/quest/QuestSystem.h"
#include <nlohmann/json.hpp>
#include <string>
#include <sstream>

namespace QuestInterop {

// ============================================================================
// Re-entrant safe string buffer for interop returns
// Uses rotating thread-local buffers to handle nested calls safely
// ============================================================================
namespace {
    static constexpr int kNumStringBuffers = 8;
    
    std::string& GetRotatingStringBuffer() {
        thread_local std::string s_Buffers[kNumStringBuffers];
        thread_local int s_CurrentBuffer = 0;
        s_CurrentBuffer = (s_CurrentBuffer + 1) % kNumStringBuffers;
        return s_Buffers[s_CurrentBuffer];
    }
}

// Callback storage
static QuestStartedCallback s_OnQuestStarted = nullptr;
static QuestStageStartedCallback s_OnStageStarted = nullptr;
static QuestStageCompletedCallback s_OnStageCompleted = nullptr;
static QuestCompletedCallback s_OnQuestCompleted = nullptr;
static QuestFailedCallback s_OnQuestFailed = nullptr;
static QuestObjectiveProgressCallback s_OnObjectiveProgress = nullptr;

static Quest::QuestSystem& GetSystem() {
    return Quest::GetGlobalQuestSystem();
}

void Initialize() {
    Quest::QuestCallbacks cb{};
    cb.onQuestStarted = [](const std::string& questId) {
        if (s_OnQuestStarted) s_OnQuestStarted(questId.c_str());
    };
    cb.onStageStarted = [](const std::string& questId, const std::string& stageId) {
        if (s_OnStageStarted) s_OnStageStarted(questId.c_str(), stageId.c_str());
    };
    cb.onStageCompleted = [](const std::string& questId, const std::string& stageId) {
        if (s_OnStageCompleted) s_OnStageCompleted(questId.c_str(), stageId.c_str());
    };
    cb.onQuestCompleted = [](const std::string& questId) {
        if (s_OnQuestCompleted) s_OnQuestCompleted(questId.c_str());
    };
    cb.onQuestFailed = [](const std::string& questId) {
        if (s_OnQuestFailed) s_OnQuestFailed(questId.c_str());
    };
    cb.onObjectiveProgress = [](const std::string& questId, const std::string& stageId, 
                                const std::string& objectiveId, int current, int target) {
        if (s_OnObjectiveProgress) {
            s_OnObjectiveProgress(questId.c_str(), stageId.c_str(), objectiveId.c_str(), current, target);
        }
    };
    GetSystem().SetCallbacks(cb);
}

void Shutdown() {
    s_OnQuestStarted = nullptr;
    s_OnStageStarted = nullptr;
    s_OnStageCompleted = nullptr;
    s_OnQuestCompleted = nullptr;
    s_OnQuestFailed = nullptr;
    s_OnObjectiveProgress = nullptr;
}

//------------------------------------------------------------------------------
// Database Management
//------------------------------------------------------------------------------
const char* Quest_LoadDatabase(const char* filePath) {
    if (!filePath) return "";
    auto db = Quest::QuestDatabase::LoadFromFile(filePath);
    if (!db) return "";
    auto guid = db->GetGuid();
    if (guid == ClaymoreGUID()) {
        guid = ClaymoreGUID::Generate();
        db->SetGuid(guid);
    }
    Quest::QuestDatabaseRegistry::Get().Register(guid, db);
    GetSystem().SetDatabase(db);
    auto& buf = GetRotatingStringBuffer();
    buf = guid.ToString();
    return buf.c_str();
}

void Quest_UnloadDatabase(const char* guidStr) {
    if (!guidStr) return;
    auto guid = ClaymoreGUID::FromString(guidStr);
    Quest::QuestDatabaseRegistry::Get().Unregister(guid);
}

//------------------------------------------------------------------------------
// Core Operations
//------------------------------------------------------------------------------
bool Quest_StartQuest(const char* questId) {
    Initialize();
    return GetSystem().StartQuest(questId ? questId : "");
}

bool Quest_AdvanceStage(const char* questId, const char* stageId) {
    Initialize();
    return GetSystem().AdvanceStage(questId ? questId : "", stageId ? stageId : "");
}

bool Quest_CompleteStage(const char* questId, const char* stageId) {
    Initialize();
    return GetSystem().CompleteStage(questId ? questId : "", stageId ? stageId : "");
}

bool Quest_FailQuest(const char* questId) {
    Initialize();
    return GetSystem().FailQuest(questId ? questId : "");
}

bool Quest_AbandonQuest(const char* questId) {
    Initialize();
    return GetSystem().AbandonQuest(questId ? questId : "");
}

bool Quest_UpdateObjectiveProgress(const char* questId, const char* stageId, const char* objectiveId, int delta) {
    Initialize();
    return GetSystem().UpdateObjectiveProgress(
        questId ? questId : "",
        stageId ? stageId : "",
        objectiveId ? objectiveId : "",
        delta);
}

bool Quest_SetObjectiveProgress(const char* questId, const char* stageId, const char* objectiveId, int value) {
    Initialize();
    return GetSystem().SetObjectiveProgress(
        questId ? questId : "",
        stageId ? stageId : "",
        objectiveId ? objectiveId : "",
        value);
}

//------------------------------------------------------------------------------
// State Queries
//------------------------------------------------------------------------------
int Quest_GetQuestState(const char* questId) {
    return static_cast<int>(GetSystem().GetQuestState(questId ? questId : ""));
}

int Quest_GetStageState(const char* questId, const char* stageId) {
    return static_cast<int>(GetSystem().GetStageState(questId ? questId : "", stageId ? stageId : ""));
}

const char* Quest_GetActiveStageId(const char* questId) {
    auto& buf = GetRotatingStringBuffer();
    buf = GetSystem().GetActiveStageId(questId ? questId : "");
    return buf.c_str();
}

int Quest_GetObjectiveProgress(const char* questId, const char* stageId, const char* objectiveId) {
    return GetSystem().GetObjectiveProgress(
        questId ? questId : "",
        stageId ? stageId : "",
        objectiveId ? objectiveId : "");
}

int Quest_GetObjectiveTarget(const char* questId, const char* stageId, const char* objectiveId) {
    return GetSystem().GetObjectiveTarget(
        questId ? questId : "",
        stageId ? stageId : "",
        objectiveId ? objectiveId : "");
}

bool Quest_IsObjectiveComplete(const char* questId, const char* stageId, const char* objectiveId) {
    return GetSystem().IsObjectiveComplete(
        questId ? questId : "",
        stageId ? stageId : "",
        objectiveId ? objectiveId : "");
}

//------------------------------------------------------------------------------
// Bulk Queries
//------------------------------------------------------------------------------
int Quest_GetActiveQuestCount() {
    return static_cast<int>(GetSystem().GetActiveQuestIds().size());
}

const char* Quest_GetActiveQuestIds() {
    auto ids = GetSystem().GetActiveQuestIds();
    std::ostringstream oss;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) oss << ",";
        oss << ids[i];
    }
    auto& buf = GetRotatingStringBuffer();
    buf = oss.str();
    return buf.c_str();
}

int Quest_GetCompletedQuestCount() {
    return static_cast<int>(GetSystem().GetCompletedQuestIds().size());
}

const char* Quest_GetCompletedQuestIds() {
    auto ids = GetSystem().GetCompletedQuestIds();
    std::ostringstream oss;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) oss << ",";
        oss << ids[i];
    }
    auto& buf = GetRotatingStringBuffer();
    buf = oss.str();
    return buf.c_str();
}

int Quest_GetBranchHistoryCount(const char* questId) {
    return static_cast<int>(GetSystem().GetBranchHistory(questId ? questId : "").size());
}

const char* Quest_GetBranchHistory(const char* questId) {
    auto branches = GetSystem().GetBranchHistory(questId ? questId : "");
    std::ostringstream oss;
    for (size_t i = 0; i < branches.size(); ++i) {
        if (i > 0) oss << ",";
        oss << branches[i];
    }
    auto& buf = GetRotatingStringBuffer();
    buf = oss.str();
    return buf.c_str();
}

//------------------------------------------------------------------------------
// Condition Checking
//------------------------------------------------------------------------------
bool Quest_CanStartQuest(const char* questId) {
    return GetSystem().CanStartQuest(questId ? questId : "");
}

bool Quest_AreStageConditionsMet(const char* questId, const char* stageId) {
    return GetSystem().AreStageConditionsMet(questId ? questId : "", stageId ? stageId : "");
}

bool Quest_IsDialogueTriggerActive(const char* questId, const char* dialogueLibraryGuid, const char* entryId) {
    return GetSystem().IsDialogueTriggerActive(
        questId ? questId : "",
        dialogueLibraryGuid ? dialogueLibraryGuid : "",
        entryId ? entryId : "");
}

//------------------------------------------------------------------------------
// Save/Load
//------------------------------------------------------------------------------
const char* Quest_SerializeState() {
    nlohmann::json j;
    GetSystem().SerializeState(j);
    auto& buf = GetRotatingStringBuffer();
    buf = j.dump();
    return buf.c_str();
}

bool Quest_DeserializeState(const char* jsonStr) {
    if (!jsonStr) return false;
    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        return GetSystem().DeserializeState(j);
    } catch (...) {
        return false;
    }
}

void Quest_ResetAllState() {
    GetSystem().ResetAllState();
}

//------------------------------------------------------------------------------
// Callbacks
//------------------------------------------------------------------------------
void Quest_SetOnQuestStarted(QuestStartedCallback cb) { s_OnQuestStarted = cb; }
void Quest_SetOnStageStarted(QuestStageStartedCallback cb) { s_OnStageStarted = cb; }
void Quest_SetOnStageCompleted(QuestStageCompletedCallback cb) { s_OnStageCompleted = cb; }
void Quest_SetOnQuestCompleted(QuestCompletedCallback cb) { s_OnQuestCompleted = cb; }
void Quest_SetOnQuestFailed(QuestFailedCallback cb) { s_OnQuestFailed = cb; }
void Quest_SetOnObjectiveProgress(QuestObjectiveProgressCallback cb) { s_OnObjectiveProgress = cb; }

} // namespace QuestInterop

//------------------------------------------------------------------------------
// Function Pointer Getters for Managed Bootstrap
//------------------------------------------------------------------------------
extern "C" {
    // Core functions
    void* Get_Quest_LoadDatabase_Ptr() { return (void*)&QuestInterop::Quest_LoadDatabase; }
    void* Get_Quest_UnloadDatabase_Ptr() { return (void*)&QuestInterop::Quest_UnloadDatabase; }
    void* Get_Quest_StartQuest_Ptr() { return (void*)&QuestInterop::Quest_StartQuest; }
    void* Get_Quest_AdvanceStage_Ptr() { return (void*)&QuestInterop::Quest_AdvanceStage; }
    void* Get_Quest_CompleteStage_Ptr() { return (void*)&QuestInterop::Quest_CompleteStage; }
    void* Get_Quest_FailQuest_Ptr() { return (void*)&QuestInterop::Quest_FailQuest; }
    void* Get_Quest_AbandonQuest_Ptr() { return (void*)&QuestInterop::Quest_AbandonQuest; }
    void* Get_Quest_UpdateObjectiveProgress_Ptr() { return (void*)&QuestInterop::Quest_UpdateObjectiveProgress; }
    void* Get_Quest_SetObjectiveProgress_Ptr() { return (void*)&QuestInterop::Quest_SetObjectiveProgress; }
    
    // State queries
    void* Get_Quest_GetQuestState_Ptr() { return (void*)&QuestInterop::Quest_GetQuestState; }
    void* Get_Quest_GetStageState_Ptr() { return (void*)&QuestInterop::Quest_GetStageState; }
    void* Get_Quest_GetActiveStageId_Ptr() { return (void*)&QuestInterop::Quest_GetActiveStageId; }
    void* Get_Quest_GetObjectiveProgress_Ptr() { return (void*)&QuestInterop::Quest_GetObjectiveProgress; }
    void* Get_Quest_GetObjectiveTarget_Ptr() { return (void*)&QuestInterop::Quest_GetObjectiveTarget; }
    void* Get_Quest_IsObjectiveComplete_Ptr() { return (void*)&QuestInterop::Quest_IsObjectiveComplete; }
    
    // Bulk queries
    void* Get_Quest_GetActiveQuestCount_Ptr() { return (void*)&QuestInterop::Quest_GetActiveQuestCount; }
    void* Get_Quest_GetActiveQuestIds_Ptr() { return (void*)&QuestInterop::Quest_GetActiveQuestIds; }
    void* Get_Quest_GetCompletedQuestCount_Ptr() { return (void*)&QuestInterop::Quest_GetCompletedQuestCount; }
    void* Get_Quest_GetCompletedQuestIds_Ptr() { return (void*)&QuestInterop::Quest_GetCompletedQuestIds; }
    void* Get_Quest_GetBranchHistoryCount_Ptr() { return (void*)&QuestInterop::Quest_GetBranchHistoryCount; }
    void* Get_Quest_GetBranchHistory_Ptr() { return (void*)&QuestInterop::Quest_GetBranchHistory; }
    
    // Conditions
    void* Get_Quest_CanStartQuest_Ptr() { return (void*)&QuestInterop::Quest_CanStartQuest; }
    void* Get_Quest_AreStageConditionsMet_Ptr() { return (void*)&QuestInterop::Quest_AreStageConditionsMet; }
    void* Get_Quest_IsDialogueTriggerActive_Ptr() { return (void*)&QuestInterop::Quest_IsDialogueTriggerActive; }
    
    // Save/Load
    void* Get_Quest_SerializeState_Ptr() { return (void*)&QuestInterop::Quest_SerializeState; }
    void* Get_Quest_DeserializeState_Ptr() { return (void*)&QuestInterop::Quest_DeserializeState; }
    void* Get_Quest_ResetAllState_Ptr() { return (void*)&QuestInterop::Quest_ResetAllState; }
    
    // Callbacks
    void* Get_Quest_SetOnQuestStarted_Ptr() { return (void*)&QuestInterop::Quest_SetOnQuestStarted; }
    void* Get_Quest_SetOnStageStarted_Ptr() { return (void*)&QuestInterop::Quest_SetOnStageStarted; }
    void* Get_Quest_SetOnStageCompleted_Ptr() { return (void*)&QuestInterop::Quest_SetOnStageCompleted; }
    void* Get_Quest_SetOnQuestCompleted_Ptr() { return (void*)&QuestInterop::Quest_SetOnQuestCompleted; }
    void* Get_Quest_SetOnQuestFailed_Ptr() { return (void*)&QuestInterop::Quest_SetOnQuestFailed; }
    void* Get_Quest_SetOnObjectiveProgress_Ptr() { return (void*)&QuestInterop::Quest_SetOnObjectiveProgress; }
}
