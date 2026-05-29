#pragma once

#include "DialogueTypes.h"
#include "DialogueLibrary.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <string>

namespace Dialogue {

//------------------------------------------------------------------------------
// DialogueConfig - Configuration for dialogue playback
//------------------------------------------------------------------------------
struct DialogueConfig {
    float textSpeed = 50.0f;           // Characters per second (0 = instant)
    float minDisplayTime = 0.5f;       // Minimum time before skip allowed
    float endCooldown = 0.5f;          // Cooldown after dialogue ends
    float shortPauseDuration = 0.15f;  // Pause for comma/semicolon
    float longPauseDuration = 0.3f;    // Pause for period/!/? 
};

//------------------------------------------------------------------------------
// Dialogue Event Callbacks
//------------------------------------------------------------------------------
struct DialogueCallbacks {
    std::function<void(const Conversation&)> onStarted;
    std::function<void()> onEnded;
    std::function<void(const DialogueLineEvent&)> onLine;
    std::function<void(const DialogueEmoteEvent&)> onEmote;
    std::function<void(const DialogueStateEvent&)> onStateChange;
    std::function<void(const DialogueEmotionEvent&)> onEmotion;
    std::function<void()> onTypingComplete;
    std::function<void(char)> onCharacterTyped;
    
    // Game integration callbacks
    std::function<void(const std::string&, int)> onGiveItem;
    std::function<void(const std::string&, int)> onTakeItem;
    std::function<void(const std::string&)> onStartQuest;
    std::function<void(const std::string&, const std::string&)> onCompleteStep;
    std::function<void(const std::string&)> onPlayAnim;
    std::function<void(const std::string&)> onPlaySound;
    std::function<void(float)> onWait;
    std::function<void(const std::string&, const std::string&)> onCamera;
    std::function<void(const std::string&, const std::string&)> onSetEvent;  // (eventName, state)
};

//------------------------------------------------------------------------------
// DialogueManager - Runtime dialogue execution
//------------------------------------------------------------------------------
class DialogueManager {
public:
    DialogueManager();
    ~DialogueManager();
    
    // Configuration
    void SetConfig(const DialogueConfig& config) { m_Config = config; }
    const DialogueConfig& GetConfig() const { return m_Config; }
    
    // Callbacks
    void SetCallbacks(const DialogueCallbacks& callbacks) { m_Callbacks = callbacks; }
    DialogueCallbacks& GetCallbacks() { return m_Callbacks; }
    
    // State management
    void SetState(const std::string& key, const std::string& value);
    std::string GetState(const std::string& key) const;
    void ClearState();
    
    // External state provider - called first before checking internal state map
    // Return empty string to fall through to internal state
    using StateProvider = std::function<std::string(const std::string&)>;
    void SetStateProvider(StateProvider provider) { m_StateProvider = std::move(provider); }
    
    // Quest status getter (for condition evaluation)
    using QuestStatusGetter = std::function<std::string(const std::string&, const std::string&)>;
    void SetQuestStatusGetter(QuestStatusGetter getter) { m_QuestStatusGetter = std::move(getter); }
    
    // Start dialogue
    bool StartDialogue(std::shared_ptr<Conversation> conversation, int speakerEntityId = -1);
    bool StartDialogueFromLibrary(const DialogueLibraryRef& libraryRef, int speakerEntityId = -1);
    bool StartDialogueFromLibrary(std::shared_ptr<DialogueLibrary> library, int speakerEntityId = -1);
    bool StartDialogueFromText(const std::string& dialogueText, int speakerEntityId = -1);
    
    // Control
    void Advance();
    void SelectChoice(int choiceIndex);
    void SkipTyping();
    void ForceEnd();
    void Pause();
    void Resume();
    
    // Update (call once per frame)
    void Update(float dt);
    
    // State queries
    DialogueState GetState() const { return m_State; }
    bool IsActive() const { return m_State != DialogueState::Inactive; }
    bool IsTyping() const { return m_State == DialogueState::DisplayingLine && !m_TypewriterComplete; }
    bool IsPaused() const { return m_State == DialogueState::Paused; }
    bool IsWaitingForChoice() const { return m_State == DialogueState::WaitingForChoice; }
    
    // Current dialogue info
    const std::string& GetDisplayedText() const { return m_DisplayedText; }
    const LineCommand* GetCurrentLine() const { return m_CurrentLine; }
    const Conversation* GetCurrentConversation() const { return m_CurrentConversation.get(); }
    int GetSpeakerEntityId() const { return m_SpeakerEntityId; }
    
    // Get available choices (filtered by conditions)
    std::vector<const DialogueChoice*> GetAvailableChoices() const;
    
private:
    // Command processing
    void ProcessNextCommand();
    void DisplayLine(const LineCommand& line);
    void HandleEmote(const EmoteCommand& cmd);
    void HandleCondition(const ConditionCommand& cmd);
    void HandleSetState(const SetStateCommand& cmd);
    void HandleSetEmotion(const SetEmotionCommand& cmd);
    bool HandleGoto(const GotoCommand& cmd);
    void HandleGameCommand(const DialogueCommand& cmd);
    void EndDialogue();
    
    // Typewriter
    void UpdateTypewriter(float dt);
    void CompleteTyping();
    
    // State
    DialogueState m_State = DialogueState::Inactive;
    DialogueConfig m_Config;
    DialogueCallbacks m_Callbacks;
    
    // Current conversation
    std::shared_ptr<Conversation> m_CurrentConversation;
    size_t m_CurrentCommandIndex = 0;
    const LineCommand* m_CurrentLine = nullptr;
    int m_SpeakerEntityId = -1;
    
    // Typewriter state
    std::string m_FullText;
    std::string m_DisplayedText;
    size_t m_TypeIndex = 0;
    float m_TypeTimer = 0.0f;
    bool m_TypewriterComplete = false;
    
    // Timing
    float m_DisplayTimer = 0.0f;
    float m_CooldownTimer = 0.0f;
    float m_WaitTimer = 0.0f;
    
    // Game state
    std::unordered_map<std::string, std::string> m_GameState;
    StateProvider m_StateProvider;
    QuestStatusGetter m_QuestStatusGetter;
    
    // Condition skip tracking
    bool m_SkippingToEndCondition = false;
    
    // Inline condition tracking for "else" support
    // Tracks whether ANY line in the current conditional sequence was shown
    // Reset when we hit a line without conditions (starts a new sequence)
    bool m_ConditionalSequenceMatched = false;
    bool m_InConditionalSequence = false;
    
    // Helper to evaluate inline conditions on a line
    bool EvaluateLineConditions(const LineCommand& line);
};

//------------------------------------------------------------------------------
// Global dialogue manager instance (optional singleton)
//------------------------------------------------------------------------------
DialogueManager& GetGlobalDialogueManager();

} // namespace Dialogue

