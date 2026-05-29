#include "DialogueInterop.h"
#include "../../core/dialogue/Dialogue.h"
#include "../../core/quest/QuestSystem.h"
#include "core/assets/IAssetResolver.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#include "editor/Project.h"
#endif
#include <iostream>
#include <string>
#include <filesystem>

namespace DialogueInterop {

//------------------------------------------------------------------------------
// Re-entrant safe string buffer for interop returns
// Uses rotating thread-local buffers to handle nested calls safely
//------------------------------------------------------------------------------
namespace {
    static constexpr int kNumStringBuffers = 8;
    
    std::string& GetRotatingStringBuffer() {
        thread_local std::string s_Buffers[kNumStringBuffers];
        thread_local int s_CurrentBuffer = 0;
        s_CurrentBuffer = (s_CurrentBuffer + 1) % kNumStringBuffers;
        return s_Buffers[s_CurrentBuffer];
    }
}

// Managed callbacks
static DialogueStartedCallback s_OnStarted = nullptr;
static DialogueEndedCallback s_OnEnded = nullptr;
static DialogueLineCallback s_OnLine = nullptr;
static DialogueEmoteCallback s_OnEmote = nullptr;
static DialogueStateChangeCallback s_OnStateChange = nullptr;
static DialogueEmotionCallback s_OnEmotion = nullptr;
static DialogueTypingCompleteCallback s_OnTypingComplete = nullptr;
static DialogueCharTypedCallback s_OnCharTyped = nullptr;
static DialogueGiveItemCallback s_OnGiveItem = nullptr;
static DialogueTakeItemCallback s_OnTakeItem = nullptr;
static DialogueStartQuestCallback s_OnStartQuest = nullptr;
static DialogueCompleteStepCallback s_OnCompleteStep = nullptr;
static DialoguePlayAnimCallback s_OnPlayAnim = nullptr;
static DialoguePlaySoundCallback s_OnPlaySound = nullptr;
static DialogueWaitCallback s_OnWait = nullptr;
static DialogueCameraCallback s_OnCamera = nullptr;
static DialogueSetEventCallback s_OnSetEvent = nullptr;

//------------------------------------------------------------------------------
// Helper to get global manager
//------------------------------------------------------------------------------
static Dialogue::DialogueManager& GetManager() {
    return Dialogue::GetGlobalDialogueManager();
}


//------------------------------------------------------------------------------
// Initialize/Shutdown
//------------------------------------------------------------------------------
void Initialize() {
    std::cout << "[DialogueInterop] Initializing..." << std::endl;
    
    // Set up callbacks from native to managed
    auto& callbacks = GetManager().GetCallbacks();

    // Provide quest status getter for dialogue conditions
    // Supports: (questId, "state") -> quest state
    //           (questId, "stage") -> active stage ID
    //           (questId, stageId) -> stage state
    GetManager().SetQuestStatusGetter([](const std::string& questId, const std::string& property) -> std::string {
        auto& system = Quest::GetGlobalQuestSystem();
        
        if (property == "state") {
            // Return quest-level state
            const auto state = system.GetQuestState(questId);
            switch (state) {
            case Quest::QuestState::Active: return "active";
            case Quest::QuestState::Completed: return "completed";
            case Quest::QuestState::Failed: return "failed";
            default: return "not_started";
            }
        } else if (property == "stage") {
            // Return active stage ID
            return system.GetActiveStageId(questId);
        } else {
            // Assume property is a stage ID - return stage state
            const auto state = system.GetStageState(questId, property);
            switch (state) {
            case Quest::StageState::Completed: return "completed";
            case Quest::StageState::Active: return "active";
            case Quest::StageState::Failed: return "failed";
            default: return "inactive";
            }
        }
    });

    
    callbacks.onStarted = [](const Dialogue::Conversation& conv) {
        if (s_OnStarted) s_OnStarted(conv.title.c_str());
    };
    
    callbacks.onEnded = []() {
        if (s_OnEnded) s_OnEnded();
    };
    
    callbacks.onLine = [](const Dialogue::DialogueLineEvent& event) {
        if (s_OnLine) {
            s_OnLine(event.speaker.c_str(), event.text.c_str(), (int)event.choices.size());
        }
    };
    
    callbacks.onEmote = [](const Dialogue::DialogueEmoteEvent& event) {
        if (s_OnEmote) s_OnEmote(event.emoteName.c_str());
    };
    
    callbacks.onStateChange = [](const Dialogue::DialogueStateEvent& event) {
        if (s_OnStateChange) s_OnStateChange(event.stateName.c_str(), event.stateValue.c_str());
    };
    
    callbacks.onEmotion = [](const Dialogue::DialogueEmotionEvent& event) {
        std::cout << "[DialogueInterop] onEmotion callback received: " << event.emotionName << std::endl;
        if (s_OnEmotion) {
            std::cout << "[DialogueInterop] s_OnEmotion is set, calling managed..." << std::endl;
            s_OnEmotion(event.emotionName.c_str());
        } else {
            std::cout << "[DialogueInterop] WARNING: s_OnEmotion is NULL!" << std::endl;
        }
    };
    
    callbacks.onTypingComplete = []() {
        if (s_OnTypingComplete) s_OnTypingComplete();
    };
    
    callbacks.onCharacterTyped = [](char c) {
        if (s_OnCharTyped) s_OnCharTyped(c);
    };
    
    // Game command callbacks
    callbacks.onGiveItem = [](const std::string& itemId, int count) {
        if (s_OnGiveItem) s_OnGiveItem(itemId.c_str(), count);
    };
    
    callbacks.onTakeItem = [](const std::string& itemId, int count) {
        if (s_OnTakeItem) s_OnTakeItem(itemId.c_str(), count);
    };
    
    callbacks.onStartQuest = [](const std::string& questId) {
        Quest::GetGlobalQuestSystem().StartQuest(questId);
        if (s_OnStartQuest) s_OnStartQuest(questId.c_str());
    };
    
    callbacks.onCompleteStep = [](const std::string& questId, const std::string& stepId) {
        Quest::GetGlobalQuestSystem().CompleteStage(questId, stepId);
        if (s_OnCompleteStep) s_OnCompleteStep(questId.c_str(), stepId.c_str());
    };
    
    callbacks.onPlayAnim = [](const std::string& animName) {
        if (s_OnPlayAnim) s_OnPlayAnim(animName.c_str());
    };
    
    callbacks.onPlaySound = [](const std::string& soundPath) {
        if (s_OnPlaySound) s_OnPlaySound(soundPath.c_str());
    };
    
    callbacks.onWait = [](float seconds) {
        if (s_OnWait) s_OnWait(seconds);
    };
    
    callbacks.onCamera = [](const std::string& action, const std::string& target) {
        if (s_OnCamera) s_OnCamera(action.c_str(), target.c_str());
    };
    
    callbacks.onSetEvent = [](const std::string& eventName, const std::string& eventState) {
        if (s_OnSetEvent) s_OnSetEvent(eventName.c_str(), eventState.c_str());
    };

    std::cout << "[DialogueInterop] Initialized" << std::endl;
}

void Shutdown() {
    // Clear callbacks
    s_OnStarted = nullptr;
    s_OnEnded = nullptr;
    s_OnLine = nullptr;
    s_OnEmote = nullptr;
    s_OnStateChange = nullptr;
    s_OnEmotion = nullptr;
    s_OnTypingComplete = nullptr;
    s_OnCharTyped = nullptr;
    s_OnGiveItem = nullptr;
    s_OnTakeItem = nullptr;
    s_OnStartQuest = nullptr;
    s_OnCompleteStep = nullptr;
    s_OnPlayAnim = nullptr;
    s_OnPlaySound = nullptr;
    s_OnWait = nullptr;
    s_OnCamera = nullptr;
    
    Dialogue::DialogueLibraryRegistry::Get().Clear();
}

void RegisterInteropFunctions() {
    // This would register function pointers with the managed runtime
    // Implementation depends on your interop setup
}

//------------------------------------------------------------------------------
// Library Management
//------------------------------------------------------------------------------
const char* Dialogue_LoadLibrary(const char* filePath) {
    if (!filePath) return "";
    
    auto library = Dialogue::DialogueLibrary::LoadFromFile(filePath);
    if (!library) {
        std::cerr << "[DialogueInterop] Failed to load library: " << filePath << std::endl;
        return "";
    }
    
    // Generate GUID if not set
    auto guid = library->GetGuid();
    if (guid.high == 0 && guid.low == 0) {
        library->SetGuid(ClaymoreGUID::Generate());
        guid = library->GetGuid();
    }
    
    auto sharedLib = std::shared_ptr<Dialogue::DialogueLibrary>(std::move(library));
    Dialogue::DialogueLibraryRegistry::Get().Register(guid, sharedLib);
    
    auto& buf = GetRotatingStringBuffer();
    buf = guid.ToString();
    return buf.c_str();
}

void Dialogue_UnloadLibrary(const char* guidStr) {
    if (!guidStr) return;
    auto guid = ClaymoreGUID::FromString(guidStr);
    Dialogue::DialogueLibraryRegistry::Get().Unregister(guid);
}

const char* Dialogue_GetLibraryName(const char* guidStr) {
    if (!guidStr) return "";
    auto guid = ClaymoreGUID::FromString(guidStr);
    auto lib = Dialogue::DialogueLibraryRegistry::Get().Find(guid);
    if (!lib) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = lib->GetDisplayName();
    return buf.c_str();
}

const char* Dialogue_GetLibraryCharacterId(const char* guidStr) {
    if (!guidStr) return "";
    auto guid = ClaymoreGUID::FromString(guidStr);
    auto lib = Dialogue::DialogueLibraryRegistry::Get().Find(guid);
    if (!lib) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = lib->GetCharacterId();
    return buf.c_str();
}

//------------------------------------------------------------------------------
// Dialogue Playback
//------------------------------------------------------------------------------
bool Dialogue_StartFromLibrary(const char* libraryGuidStr, int speakerEntityId) {
    try {
        if (!libraryGuidStr) return false;
        
        std::cout << "[DialogueInterop] StartFromLibrary: " << libraryGuidStr << std::endl;
        
        auto guid = ClaymoreGUID::FromString(libraryGuidStr);
        auto lib = Dialogue::DialogueLibraryRegistry::Get().Find(guid);
        
        // On-demand loading: if not in registry, try to load from asset path
        if (!lib) {
            // Use asset resolver interface (works for both editor and runtime)
            std::string vpath;
            IAssetResolver* resolver = Assets::GetResolver();
            if (resolver) {
                vpath = resolver->GetPathForGUID(guid);
            }
#ifndef CLAYMORE_RUNTIME
            // Editor fallback: also try AssetLibrary
            if (vpath.empty()) {
                vpath = AssetLibrary::Instance().GetPathForGUID(guid);
            }
#endif
            if (!vpath.empty()) {
                std::cout << "[DialogueInterop] Loading library on-demand: " << vpath << std::endl;
                
                // LoadFromFile uses VFS which handles both runtime (PAK) and editor modes
                // For editor mode with absolute paths, also try project-relative resolution
                auto loaded = Dialogue::DialogueLibrary::LoadFromFile(vpath);
                
#ifndef CLAYMORE_RUNTIME
                // In editor, if VFS didn't find it, try absolute path
                if (!loaded) {
                    std::filesystem::path absPath = Project::GetProjectDirectory() / vpath;
                    std::cout << "[DialogueInterop] Trying absolute path: " << absPath << std::endl;
                    loaded = Dialogue::DialogueLibrary::LoadFromFile(absPath.string());
                }
#endif
                
                if (loaded) {
                    std::cout << "[DialogueInterop] Library loaded, entries: " << loaded->GetEntries().size() << std::endl;
                    loaded->SetGuid(guid);
                    lib = std::shared_ptr<Dialogue::DialogueLibrary>(std::move(loaded));
                    Dialogue::DialogueLibraryRegistry::Get().Register(guid, lib);
                }
            }
        }
        
        if (!lib) {
            std::cerr << "[DialogueInterop] Library not found: " << libraryGuidStr << std::endl;
            return false;
        }
        
        std::cout << "[DialogueInterop] Starting dialogue from library..." << std::endl;
        bool result = GetManager().StartDialogueFromLibrary(lib, speakerEntityId);
        std::cout << "[DialogueInterop] StartDialogueFromLibrary returned: " << (result ? "true" : "false") << std::endl;
        return result;
    } catch (const std::exception& e) {
        std::cerr << "[DialogueInterop] Exception in StartFromLibrary: " << e.what() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "[DialogueInterop] Unknown exception in StartFromLibrary" << std::endl;
        return false;
    }
}

bool Dialogue_StartFromText(const char* dialogueText, int speakerEntityId) {
    if (!dialogueText) return false;
    return GetManager().StartDialogueFromText(dialogueText, speakerEntityId);
}

void Dialogue_Advance() {
    std::cout << "[DialogueInterop] Dialogue_Advance() called" << std::endl;
    std::cout.flush();
    GetManager().Advance();
    std::cout << "[DialogueInterop] Dialogue_Advance() returned" << std::endl;
    std::cout.flush();
}

void Dialogue_SelectChoice(int choiceIndex) {
    GetManager().SelectChoice(choiceIndex);
}

void Dialogue_ForceEnd() {
    GetManager().ForceEnd();
}

void Dialogue_Pause() {
    GetManager().Pause();
}

void Dialogue_Resume() {
    GetManager().Resume();
}

//------------------------------------------------------------------------------
// State Queries
//------------------------------------------------------------------------------
bool Dialogue_IsActive() {
    return GetManager().IsActive();
}

bool Dialogue_IsTyping() {
    return GetManager().IsTyping();
}

bool Dialogue_IsWaitingForChoice() {
    return GetManager().IsWaitingForChoice();
}

int Dialogue_GetState() {
    return static_cast<int>(GetManager().GetState());
}

const char* Dialogue_GetDisplayedText() {
    auto& buf = GetRotatingStringBuffer();
    buf = GetManager().GetDisplayedText();
    return buf.c_str();
}

const char* Dialogue_GetCurrentSpeaker() {
    auto* line = GetManager().GetCurrentLine();
    if (!line) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = line->speaker;
    return buf.c_str();
}

int Dialogue_GetSpeakerEntityId() {
    return GetManager().GetSpeakerEntityId();
}

int Dialogue_GetChoiceCount() {
    auto choices = GetManager().GetAvailableChoices();
    return static_cast<int>(choices.size());
}

const char* Dialogue_GetChoiceText(int index) {
    auto choices = GetManager().GetAvailableChoices();
    if (index < 0 || index >= (int)choices.size()) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = choices[index]->text;
    return buf.c_str();
}

const char* Dialogue_GetConversationTitle() {
    auto* conv = GetManager().GetCurrentConversation();
    if (!conv) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = conv->title;
    return buf.c_str();
}

//------------------------------------------------------------------------------
// Game State Management
//------------------------------------------------------------------------------
void Dialogue_SetState(const char* key, const char* value) {
    if (!key) return;
    GetManager().SetState(key, value ? value : "");
}

const char* Dialogue_GetStateValue(const char* key) {
    if (!key) return "";
    auto& buf = GetRotatingStringBuffer();
    buf = GetManager().GetState(key);
    return buf.c_str();
}

void Dialogue_ClearState() {
    GetManager().ClearState();
}

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------
void Dialogue_SetTextSpeed(float charsPerSecond) {
    auto config = GetManager().GetConfig();
    config.textSpeed = charsPerSecond;
    GetManager().SetConfig(config);
}

void Dialogue_SetMinDisplayTime(float seconds) {
    auto config = GetManager().GetConfig();
    config.minDisplayTime = seconds;
    GetManager().SetConfig(config);
}

void Dialogue_SetEndCooldown(float seconds) {
    auto config = GetManager().GetConfig();
    config.endCooldown = seconds;
    GetManager().SetConfig(config);
}

//------------------------------------------------------------------------------
// Callback Registration
//------------------------------------------------------------------------------
void Dialogue_SetOnStarted(DialogueStartedCallback cb) { s_OnStarted = cb; }
void Dialogue_SetOnEnded(DialogueEndedCallback cb) { s_OnEnded = cb; }
void Dialogue_SetOnLine(DialogueLineCallback cb) { s_OnLine = cb; }
void Dialogue_SetOnEmote(DialogueEmoteCallback cb) { s_OnEmote = cb; }
void Dialogue_SetOnStateChange(DialogueStateChangeCallback cb) { s_OnStateChange = cb; }
void Dialogue_SetOnEmotion(DialogueEmotionCallback cb) { s_OnEmotion = cb; }
void Dialogue_SetOnTypingComplete(DialogueTypingCompleteCallback cb) { s_OnTypingComplete = cb; }
void Dialogue_SetOnCharTyped(DialogueCharTypedCallback cb) { s_OnCharTyped = cb; }
void Dialogue_SetOnGiveItem(DialogueGiveItemCallback cb) { s_OnGiveItem = cb; }
void Dialogue_SetOnTakeItem(DialogueTakeItemCallback cb) { s_OnTakeItem = cb; }
void Dialogue_SetOnStartQuest(DialogueStartQuestCallback cb) { s_OnStartQuest = cb; }
void Dialogue_SetOnCompleteStep(DialogueCompleteStepCallback cb) { s_OnCompleteStep = cb; }
void Dialogue_SetOnPlayAnim(DialoguePlayAnimCallback cb) { s_OnPlayAnim = cb; }
void Dialogue_SetOnPlaySound(DialoguePlaySoundCallback cb) { s_OnPlaySound = cb; }
void Dialogue_SetOnWait(DialogueWaitCallback cb) { s_OnWait = cb; }
void Dialogue_SetOnCamera(DialogueCameraCallback cb) { s_OnCamera = cb; }
void Dialogue_SetOnSetEvent(DialogueSetEventCallback cb) { s_OnSetEvent = cb; }

} // namespace DialogueInterop

//------------------------------------------------------------------------------
// Function pointer getters for managed interop bootstrap
//------------------------------------------------------------------------------
extern "C" {
    // Library management (4)
    void* Get_Dialogue_LoadLibrary_Ptr() { return (void*)&DialogueInterop::Dialogue_LoadLibrary; }
    void* Get_Dialogue_UnloadLibrary_Ptr() { return (void*)&DialogueInterop::Dialogue_UnloadLibrary; }
    void* Get_Dialogue_GetLibraryName_Ptr() { return (void*)&DialogueInterop::Dialogue_GetLibraryName; }
    void* Get_Dialogue_GetLibraryCharacterId_Ptr() { return (void*)&DialogueInterop::Dialogue_GetLibraryCharacterId; }
    
    // Playback (7)
    void* Get_Dialogue_StartFromLibrary_Ptr() { return (void*)&DialogueInterop::Dialogue_StartFromLibrary; }
    void* Get_Dialogue_StartFromText_Ptr() { return (void*)&DialogueInterop::Dialogue_StartFromText; }
    void* Get_Dialogue_Advance_Ptr() { return (void*)&DialogueInterop::Dialogue_Advance; }
    void* Get_Dialogue_SelectChoice_Ptr() { return (void*)&DialogueInterop::Dialogue_SelectChoice; }
    void* Get_Dialogue_ForceEnd_Ptr() { return (void*)&DialogueInterop::Dialogue_ForceEnd; }
    void* Get_Dialogue_Pause_Ptr() { return (void*)&DialogueInterop::Dialogue_Pause; }
    void* Get_Dialogue_Resume_Ptr() { return (void*)&DialogueInterop::Dialogue_Resume; }
    
    // State queries (10)
    void* Get_Dialogue_IsActive_Ptr() { return (void*)&DialogueInterop::Dialogue_IsActive; }
    void* Get_Dialogue_IsTyping_Ptr() { return (void*)&DialogueInterop::Dialogue_IsTyping; }
    void* Get_Dialogue_IsWaitingForChoice_Ptr() { return (void*)&DialogueInterop::Dialogue_IsWaitingForChoice; }
    void* Get_Dialogue_GetState_Ptr() { return (void*)&DialogueInterop::Dialogue_GetState; }
    void* Get_Dialogue_GetDisplayedText_Ptr() { return (void*)&DialogueInterop::Dialogue_GetDisplayedText; }
    void* Get_Dialogue_GetCurrentSpeaker_Ptr() { return (void*)&DialogueInterop::Dialogue_GetCurrentSpeaker; }
    void* Get_Dialogue_GetSpeakerEntityId_Ptr() { return (void*)&DialogueInterop::Dialogue_GetSpeakerEntityId; }
    void* Get_Dialogue_GetChoiceCount_Ptr() { return (void*)&DialogueInterop::Dialogue_GetChoiceCount; }
    void* Get_Dialogue_GetChoiceText_Ptr() { return (void*)&DialogueInterop::Dialogue_GetChoiceText; }
    void* Get_Dialogue_GetConversationTitle_Ptr() { return (void*)&DialogueInterop::Dialogue_GetConversationTitle; }
    
    // Game state (3)
    void* Get_Dialogue_SetState_Ptr() { return (void*)&DialogueInterop::Dialogue_SetState; }
    void* Get_Dialogue_GetStateValue_Ptr() { return (void*)&DialogueInterop::Dialogue_GetStateValue; }
    void* Get_Dialogue_ClearState_Ptr() { return (void*)&DialogueInterop::Dialogue_ClearState; }
    
    // Config (3)
    void* Get_Dialogue_SetTextSpeed_Ptr() { return (void*)&DialogueInterop::Dialogue_SetTextSpeed; }
    void* Get_Dialogue_SetMinDisplayTime_Ptr() { return (void*)&DialogueInterop::Dialogue_SetMinDisplayTime; }
    void* Get_Dialogue_SetEndCooldown_Ptr() { return (void*)&DialogueInterop::Dialogue_SetEndCooldown; }
    
    // Callback setters (16)
    void* Get_Dialogue_SetOnStarted_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnStarted; }
    void* Get_Dialogue_SetOnEnded_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnEnded; }
    void* Get_Dialogue_SetOnLine_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnLine; }
    void* Get_Dialogue_SetOnEmote_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnEmote; }
    void* Get_Dialogue_SetOnStateChange_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnStateChange; }
    void* Get_Dialogue_SetOnEmotion_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnEmotion; }
    void* Get_Dialogue_SetOnTypingComplete_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnTypingComplete; }
    void* Get_Dialogue_SetOnCharTyped_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnCharTyped; }
    void* Get_Dialogue_SetOnGiveItem_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnGiveItem; }
    void* Get_Dialogue_SetOnTakeItem_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnTakeItem; }
    void* Get_Dialogue_SetOnStartQuest_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnStartQuest; }
    void* Get_Dialogue_SetOnCompleteStep_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnCompleteStep; }
    void* Get_Dialogue_SetOnPlayAnim_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnPlayAnim; }
    void* Get_Dialogue_SetOnPlaySound_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnPlaySound; }
    void* Get_Dialogue_SetOnWait_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnWait; }
    void* Get_Dialogue_SetOnCamera_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnCamera; }
    void* Get_Dialogue_SetOnSetEvent_Ptr() { return (void*)&DialogueInterop::Dialogue_SetOnSetEvent; }
}

