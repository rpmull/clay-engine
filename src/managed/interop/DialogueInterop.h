#pragma once

#include <cstdint>

//------------------------------------------------------------------------------
// Dialogue System Interop
// Exposes native dialogue functionality to managed C# code
//------------------------------------------------------------------------------

namespace DialogueInterop {

// Initialize dialogue interop (call during startup)
void Initialize();

// Shutdown (call during cleanup)
void Shutdown();

// Register all dialogue interop function pointers with managed runtime
void RegisterInteropFunctions();

//------------------------------------------------------------------------------
// Exported Functions (called from C#)
//------------------------------------------------------------------------------

// Library management
extern "C" {
    // Load a dialogue library from file path, returns GUID string (empty on failure)
    const char* Dialogue_LoadLibrary(const char* filePath);
    
    // Unload a library by GUID
    void Dialogue_UnloadLibrary(const char* guidStr);
    
    // Get library display name
    const char* Dialogue_GetLibraryName(const char* guidStr);
    
    // Get library character ID
    const char* Dialogue_GetLibraryCharacterId(const char* guidStr);
}

// Dialogue playback
extern "C" {
    // Start dialogue from library GUID
    bool Dialogue_StartFromLibrary(const char* libraryGuidStr, int speakerEntityId);
    
    // Start dialogue from raw text
    bool Dialogue_StartFromText(const char* dialogueText, int speakerEntityId);
    
    // Advance dialogue (continue/skip)
    void Dialogue_Advance();
    
    // Select a choice by index
    void Dialogue_SelectChoice(int choiceIndex);
    
    // Force end dialogue
    void Dialogue_ForceEnd();
    
    // Pause/Resume
    void Dialogue_Pause();
    void Dialogue_Resume();
}

// State queries
extern "C" {
    // Is dialogue currently active?
    bool Dialogue_IsActive();
    
    // Is currently typing (typewriter effect)?
    bool Dialogue_IsTyping();
    
    // Is waiting for choice selection?
    bool Dialogue_IsWaitingForChoice();
    
    // Get current dialogue state (0=Inactive, 1=DisplayingLine, 2=WaitingInput, 3=WaitingChoice, etc.)
    int Dialogue_GetState();
    
    // Get currently displayed text
    const char* Dialogue_GetDisplayedText();
    
    // Get current speaker name
    const char* Dialogue_GetCurrentSpeaker();
    
    // Get speaker entity ID (-1 if none)
    int Dialogue_GetSpeakerEntityId();
    
    // Get number of available choices
    int Dialogue_GetChoiceCount();
    
    // Get choice text at index
    const char* Dialogue_GetChoiceText(int index);
    
    // Get conversation title
    const char* Dialogue_GetConversationTitle();
}

// Game state management
extern "C" {
    // Set a dialogue state variable
    void Dialogue_SetState(const char* key, const char* value);
    
    // Get a dialogue state variable
    const char* Dialogue_GetStateValue(const char* key);
    
    // Clear all state
    void Dialogue_ClearState();
}

// Configuration
extern "C" {
    void Dialogue_SetTextSpeed(float charsPerSecond);
    void Dialogue_SetMinDisplayTime(float seconds);
    void Dialogue_SetEndCooldown(float seconds);
}

//------------------------------------------------------------------------------
// Callback Registration (C# subscribes to these)
//------------------------------------------------------------------------------

// Callback function pointer types
typedef void (*DialogueStartedCallback)(const char* title);
typedef void (*DialogueEndedCallback)();
typedef void (*DialogueLineCallback)(const char* speaker, const char* text, int choiceCount);
typedef void (*DialogueEmoteCallback)(const char* emoteName);
typedef void (*DialogueStateChangeCallback)(const char* key, const char* value);
typedef void (*DialogueEmotionCallback)(const char* emotionName);
typedef void (*DialogueTypingCompleteCallback)();
typedef void (*DialogueCharTypedCallback)(char c);

// Game command callbacks
typedef void (*DialogueGiveItemCallback)(const char* itemId, int count);
typedef void (*DialogueTakeItemCallback)(const char* itemId, int count);
typedef void (*DialogueStartQuestCallback)(const char* questId);
typedef void (*DialogueCompleteStepCallback)(const char* questId, const char* stepId);
typedef void (*DialoguePlayAnimCallback)(const char* animName);
typedef void (*DialoguePlaySoundCallback)(const char* soundPath);
typedef void (*DialogueWaitCallback)(float seconds);
typedef void (*DialogueCameraCallback)(const char* action, const char* target);
typedef void (*DialogueSetEventCallback)(const char* eventName, const char* eventState);

extern "C" {
    void Dialogue_SetOnStarted(DialogueStartedCallback cb);
    void Dialogue_SetOnEnded(DialogueEndedCallback cb);
    void Dialogue_SetOnLine(DialogueLineCallback cb);
    void Dialogue_SetOnEmote(DialogueEmoteCallback cb);
    void Dialogue_SetOnStateChange(DialogueStateChangeCallback cb);
    void Dialogue_SetOnEmotion(DialogueEmotionCallback cb);
    void Dialogue_SetOnTypingComplete(DialogueTypingCompleteCallback cb);
    void Dialogue_SetOnCharTyped(DialogueCharTypedCallback cb);
    
    // Game command callbacks
    void Dialogue_SetOnGiveItem(DialogueGiveItemCallback cb);
    void Dialogue_SetOnTakeItem(DialogueTakeItemCallback cb);
    void Dialogue_SetOnStartQuest(DialogueStartQuestCallback cb);
    void Dialogue_SetOnCompleteStep(DialogueCompleteStepCallback cb);
    void Dialogue_SetOnPlayAnim(DialoguePlayAnimCallback cb);
    void Dialogue_SetOnPlaySound(DialoguePlaySoundCallback cb);
    void Dialogue_SetOnWait(DialogueWaitCallback cb);
    void Dialogue_SetOnCamera(DialogueCameraCallback cb);
    void Dialogue_SetOnSetEvent(DialogueSetEventCallback cb);
}

} // namespace DialogueInterop

//------------------------------------------------------------------------------
// Function pointer getters for managed interop bootstrap (extern "C")
//------------------------------------------------------------------------------
extern "C" {
    // Library management (4)
    void* Get_Dialogue_LoadLibrary_Ptr();
    void* Get_Dialogue_UnloadLibrary_Ptr();
    void* Get_Dialogue_GetLibraryName_Ptr();
    void* Get_Dialogue_GetLibraryCharacterId_Ptr();
    
    // Playback (7)
    void* Get_Dialogue_StartFromLibrary_Ptr();
    void* Get_Dialogue_StartFromText_Ptr();
    void* Get_Dialogue_Advance_Ptr();
    void* Get_Dialogue_SelectChoice_Ptr();
    void* Get_Dialogue_ForceEnd_Ptr();
    void* Get_Dialogue_Pause_Ptr();
    void* Get_Dialogue_Resume_Ptr();
    
    // State queries (10)
    void* Get_Dialogue_IsActive_Ptr();
    void* Get_Dialogue_IsTyping_Ptr();
    void* Get_Dialogue_IsWaitingForChoice_Ptr();
    void* Get_Dialogue_GetState_Ptr();
    void* Get_Dialogue_GetDisplayedText_Ptr();
    void* Get_Dialogue_GetCurrentSpeaker_Ptr();
    void* Get_Dialogue_GetSpeakerEntityId_Ptr();
    void* Get_Dialogue_GetChoiceCount_Ptr();
    void* Get_Dialogue_GetChoiceText_Ptr();
    void* Get_Dialogue_GetConversationTitle_Ptr();
    
    // Game state (3)
    void* Get_Dialogue_SetState_Ptr();
    void* Get_Dialogue_GetStateValue_Ptr();
    void* Get_Dialogue_ClearState_Ptr();
    
    // Config (3)
    void* Get_Dialogue_SetTextSpeed_Ptr();
    void* Get_Dialogue_SetMinDisplayTime_Ptr();
    void* Get_Dialogue_SetEndCooldown_Ptr();
    
    // Callback setters (17) - includes SetEvent
    void* Get_Dialogue_SetOnStarted_Ptr();
    void* Get_Dialogue_SetOnEnded_Ptr();
    void* Get_Dialogue_SetOnLine_Ptr();
    void* Get_Dialogue_SetOnEmote_Ptr();
    void* Get_Dialogue_SetOnStateChange_Ptr();
    void* Get_Dialogue_SetOnEmotion_Ptr();
    void* Get_Dialogue_SetOnTypingComplete_Ptr();
    void* Get_Dialogue_SetOnCharTyped_Ptr();
    void* Get_Dialogue_SetOnGiveItem_Ptr();
    void* Get_Dialogue_SetOnTakeItem_Ptr();
    void* Get_Dialogue_SetOnStartQuest_Ptr();
    void* Get_Dialogue_SetOnCompleteStep_Ptr();
    void* Get_Dialogue_SetOnPlayAnim_Ptr();
    void* Get_Dialogue_SetOnPlaySound_Ptr();
    void* Get_Dialogue_SetOnWait_Ptr();
    void* Get_Dialogue_SetOnCamera_Ptr();
    void* Get_Dialogue_SetOnSetEvent_Ptr();
}

