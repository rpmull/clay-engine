#include "DialogueManager.h"
#include "DialogueParser.h"
#include <iostream>
#include <algorithm>

namespace Dialogue {

//------------------------------------------------------------------------------
// Global instance
//------------------------------------------------------------------------------
DialogueManager& GetGlobalDialogueManager() {
    static DialogueManager instance;
    return instance;
}

//------------------------------------------------------------------------------
// Constructor/Destructor
//------------------------------------------------------------------------------
DialogueManager::DialogueManager() = default;
DialogueManager::~DialogueManager() = default;

//------------------------------------------------------------------------------
// State management
//------------------------------------------------------------------------------
void DialogueManager::SetState(const std::string& key, const std::string& value) {
    m_GameState[key] = value;
}

std::string DialogueManager::GetState(const std::string& key) const {
    // First, check external state provider (for game-specific data)
    if (m_StateProvider) {
        std::string value = m_StateProvider(key);
        if (!value.empty()) {
            return value;
        }
    }
    
    // Fall back to internal state map
    auto it = m_GameState.find(key);
    if (it != m_GameState.end()) {
        return it->second;
    }
    
    // For event_ prefixed keys, return "NotStarted" if not found
    if (key.rfind("event_", 0) == 0) {
        return "NotStarted";
    }
    
    return "";
}

void DialogueManager::ClearState() {
    m_GameState.clear();
}

//------------------------------------------------------------------------------
// Start dialogue
//------------------------------------------------------------------------------
bool DialogueManager::StartDialogue(std::shared_ptr<Conversation> conversation, int speakerEntityId) {
    if (!conversation) {
        std::cerr << "[DialogueManager] Cannot start null conversation" << std::endl;
        return false;
    }
    
    if (m_State != DialogueState::Inactive) {
        std::cerr << "[DialogueManager] Dialogue already active" << std::endl;
        return false;
    }
    
    if (m_CooldownTimer > 0) {
        std::cerr << "[DialogueManager] Still in cooldown" << std::endl;
        return false;
    }
    
    // Deep copy the conversation to avoid modifying the cached library version
    // This ensures each playthrough starts fresh, even if choices modify the command list
    m_CurrentConversation = conversation->DeepCopy();
    m_CurrentCommandIndex = 0;
    m_SpeakerEntityId = speakerEntityId;
    m_DisplayTimer = 0;
    m_SkippingToEndCondition = false;
    m_ConditionalSequenceMatched = false;  // Reset for else condition tracking
    m_InConditionalSequence = false;
    
    // Label index is already built by DeepCopy
    
    m_State = DialogueState::ProcessingCommand;
    
    std::cout << "[DialogueManager] Firing onStarted callback..." << std::endl;
    if (m_Callbacks.onStarted) {
        try {
            m_Callbacks.onStarted(*m_CurrentConversation);
        } catch (const std::exception& e) {
            std::cerr << "[DialogueManager] onStarted callback exception: " << e.what() << std::endl;
        }
    }
    
    std::cout << "[DialogueManager] Started: " << m_CurrentConversation->title << std::endl;
    
    std::cout << "[DialogueManager] Processing first command..." << std::endl;
    ProcessNextCommand();
    std::cout << "[DialogueManager] First command processed" << std::endl;
    return true;
}

bool DialogueManager::StartDialogueFromLibrary(const DialogueLibraryRef& libraryRef, int speakerEntityId) {
    auto library = libraryRef.Resolve();
    if (!library) {
        std::cerr << "[DialogueManager] Failed to resolve library ref" << std::endl;
        return false;
    }
    return StartDialogueFromLibrary(library, speakerEntityId);
}

bool DialogueManager::StartDialogueFromLibrary(std::shared_ptr<DialogueLibrary> library, int speakerEntityId) {
    if (!library) {
        std::cerr << "[DialogueManager] Null library" << std::endl;
        return false;
    }
    
    std::cout << "[DialogueManager] Getting conversation from library..." << std::endl;
    std::cout << "[DialogueManager] Library has " << library->GetEntries().size() << " entries" << std::endl;
    
    auto conversation = library->GetConversation(m_GameState, m_QuestStatusGetter);
    if (!conversation) {
        std::cerr << "[DialogueManager] No available conversation in library" << std::endl;
        return false;
    }
    
    std::cout << "[DialogueManager] Got conversation: " << conversation->title << " with " << conversation->commands.size() << " commands" << std::endl;
    
    return StartDialogue(conversation, speakerEntityId);
}

bool DialogueManager::StartDialogueFromText(const std::string& dialogueText, int speakerEntityId) {
    auto conversation = DialogueParser::Parse(dialogueText);
    if (!conversation) {
        std::cerr << "[DialogueManager] Failed to parse dialogue text" << std::endl;
        return false;
    }
    return StartDialogue(conversation, speakerEntityId);
}

//------------------------------------------------------------------------------
// Control methods
//------------------------------------------------------------------------------
void DialogueManager::Advance() {
    std::cout << "[DialogueManager] Advance() ENTRY, state=" << (int)m_State 
              << " typeComplete=" << m_TypewriterComplete << std::endl;
    std::cout.flush(); // Force flush to see output before potential crash
    
    if (m_State == DialogueState::Inactive) {
        std::cout << "[DialogueManager] Advance(): Inactive, returning" << std::endl;
        return;
    }
    
    // If still typing, complete instantly if past min time
    if (m_State == DialogueState::DisplayingLine && !m_TypewriterComplete) {
        std::cout << "[DialogueManager] Advance(): DisplayingLine, displayTimer=" << m_DisplayTimer 
                  << " minTime=" << m_Config.minDisplayTime << std::endl;
        if (m_DisplayTimer >= m_Config.minDisplayTime) {
            CompleteTyping();
        }
        return;
    }
    
    // If waiting for input (no choices), advance
    if (m_State == DialogueState::WaitingForInput) {
        std::cout << "[DialogueManager] Advance(): WaitingForInput, advancing to next command" << std::endl;
        m_CurrentCommandIndex++;
        m_State = DialogueState::ProcessingCommand;
        ProcessNextCommand();
    } else {
        std::cout << "[DialogueManager] Advance(): State not WaitingForInput (" << (int)m_State << "), no action" << std::endl;
        std::cout.flush();
    }
    std::cout << "[DialogueManager] Advance() EXIT" << std::endl;
    std::cout.flush();
}

void DialogueManager::SelectChoice(int choiceIndex) {
    std::cout << "[DialogueManager] SelectChoice(" << choiceIndex << ") called, state=" << (int)m_State << std::endl;
    
    if (m_State != DialogueState::WaitingForChoice || !m_CurrentLine) {
        std::cout << "[DialogueManager] SelectChoice: Wrong state or no current line" << std::endl;
        return;
    }
    
    auto available = GetAvailableChoices();
    std::cout << "[DialogueManager] SelectChoice: " << available.size() << " choices available" << std::endl;
    
    if (choiceIndex < 0 || choiceIndex >= (int)available.size()) {
        std::cerr << "[DialogueManager] Invalid choice index: " << choiceIndex << std::endl;
        return;
    }
    
    const DialogueChoice* choice = available[choiceIndex];
    std::cout << "[DialogueManager] SelectChoice: Selected choice has " << choice->response.size() << " response commands" << std::endl;
    
    // IMPORTANT: Copy the response commands BEFORE erasing, because erasing
    // invalidates the choice pointer (it points into the line being erased)
    std::vector<std::shared_ptr<DialogueCommand>> responseCommands;
    responseCommands.reserve(choice->response.size());
    for (const auto& cmd : choice->response) {
        responseCommands.push_back(cmd);
    }
    
    // Insert choice response commands after current position
    if (!responseCommands.empty()) {
        // Remove the current line command (invalidates m_CurrentLine and choice!)
        m_CurrentConversation->commands.erase(
            m_CurrentConversation->commands.begin() + m_CurrentCommandIndex);
        m_CurrentLine = nullptr; // Mark as invalid
        
        // Insert response commands (using our safe copy)
        m_CurrentConversation->commands.insert(
            m_CurrentConversation->commands.begin() + m_CurrentCommandIndex,
            responseCommands.begin(),
            responseCommands.end());
        
        // Rebuild label index
        m_CurrentConversation->BuildLabelIndex();
    } else {
        m_CurrentCommandIndex++;
    }
    
    m_State = DialogueState::ProcessingCommand;
    ProcessNextCommand();
}

void DialogueManager::SkipTyping() {
    if (m_State == DialogueState::DisplayingLine && !m_TypewriterComplete) {
        CompleteTyping();
    }
}

void DialogueManager::ForceEnd() {
    if (m_State != DialogueState::Inactive) {
        EndDialogue();
    }
}

void DialogueManager::Pause() {
    if (m_State != DialogueState::Inactive && m_State != DialogueState::Paused) {
        m_State = DialogueState::Paused;
    }
}

void DialogueManager::Resume() {
    if (m_State == DialogueState::Paused) {
        m_State = DialogueState::ProcessingCommand;
        ProcessNextCommand();
    }
}

//------------------------------------------------------------------------------
// Update
//------------------------------------------------------------------------------
void DialogueManager::Update(float dt) {
    // Handle cooldown when inactive
    if (m_State == DialogueState::Inactive) {
        if (m_CooldownTimer > 0) {
            m_CooldownTimer -= dt;
        }
        return;
    }
    
    // Handle wait timer
    if (m_WaitTimer > 0) {
        m_WaitTimer -= dt;
        if (m_WaitTimer <= 0) {
            m_CurrentCommandIndex++;
            ProcessNextCommand();
        }
        return;
    }
    
    // Update display timer
    m_DisplayTimer += dt;
    
    // Handle typewriter effect
    if (m_State == DialogueState::DisplayingLine && !m_TypewriterComplete) {
        UpdateTypewriter(dt);
    }
}

//------------------------------------------------------------------------------
// Get available choices
//------------------------------------------------------------------------------
std::vector<const DialogueChoice*> DialogueManager::GetAvailableChoices() const {
    std::vector<const DialogueChoice*> result;

    if (!m_CurrentLine) return result;

    for (const auto& choice : m_CurrentLine->choices) {
        if (choice.EvaluateCondition(m_GameState, m_QuestStatusGetter)) {
            result.push_back(&choice);
        }
    }

    return result;
}

//------------------------------------------------------------------------------
// Command processing
//------------------------------------------------------------------------------
void DialogueManager::ProcessNextCommand() {
    std::cout << "[DialogueManager] ProcessNextCommand() called, cmdIndex=" << m_CurrentCommandIndex << std::endl;
    
    int loopCount = 0;
    while (m_CurrentConversation && m_CurrentCommandIndex < m_CurrentConversation->commands.size()) {
        loopCount++;
        if (loopCount > 100) {
            std::cerr << "[DialogueManager] WARNING: ProcessNextCommand loop count exceeded 100, possible infinite loop!" << std::endl;
            break;
        }
        
        auto& cmd = m_CurrentConversation->commands[m_CurrentCommandIndex];
        std::cout << "[DialogueManager] Processing command type=" << (int)cmd->type 
                  << " at index=" << m_CurrentCommandIndex << std::endl;
        
        // Skip if we're skipping due to failed condition
        if (m_SkippingToEndCondition) {
            // Stop skipping at next dialogue line or end
            if (cmd->type == CommandType::Line || cmd->type == CommandType::End) {
                m_SkippingToEndCondition = false;
            } else {
                m_CurrentCommandIndex++;
                continue;
            }
        }
        
        switch (cmd->type) {
            case CommandType::Line: {
                const auto& lineCmd = static_cast<const LineCommand&>(*cmd);
                
                // Check inline conditions on the line (e.g., NPC: "Text" <condition>)
                if (!lineCmd.inlineConditions.empty()) {
                    bool passed = EvaluateLineConditions(lineCmd);
                    
                    if (!passed) {
                        // Skip this line, try next command
                        m_CurrentCommandIndex++;
                        continue;
                    }
                } else {
                    // Line without conditions - reset the conditional sequence
                    m_InConditionalSequence = false;
                    m_ConditionalSequenceMatched = false;
                }
                
                DisplayLine(lineCmd);
                return; // Wait for player input
            }
                
            case CommandType::Emote:
                HandleEmote(static_cast<const EmoteCommand&>(*cmd));
                break;
                
            case CommandType::Condition:
                HandleCondition(static_cast<const ConditionCommand&>(*cmd));
                break;
                
            case CommandType::SetState:
                HandleSetState(static_cast<const SetStateCommand&>(*cmd));
                break;
                
            case CommandType::SetEmotion:
                std::cout << "[DialogueManager] Processing SetEmotion command" << std::endl;
                HandleSetEmotion(static_cast<const SetEmotionCommand&>(*cmd));
                break;
                
            case CommandType::Label:
                // Labels are markers, skip
                break;
                
            case CommandType::Goto:
                if (HandleGoto(static_cast<const GotoCommand&>(*cmd))) {
                    continue; // Don't increment, we jumped
                }
                break;
                
            case CommandType::End:
                EndDialogue();
                return;
                
            case CommandType::Wait: {
                auto& waitCmd = static_cast<const WaitCommand&>(*cmd);
                m_WaitTimer = waitCmd.seconds;
                if (m_Callbacks.onWait) {
                    m_Callbacks.onWait(waitCmd.seconds);
                }
                return; // Will resume after wait
            }
                
            case CommandType::GiveItem:
            case CommandType::TakeItem:
            case CommandType::StartQuest:
            case CommandType::CompleteStep:
            case CommandType::PlayAnim:
            case CommandType::PlaySound:
            case CommandType::Camera:
            case CommandType::SetEvent:
            case CommandType::Custom:
                HandleGameCommand(*cmd);
                break;
                
            default:
                std::cerr << "[DialogueManager] Unknown command type: " << (int)cmd->type << std::endl;
                break;
        }
        
        m_CurrentCommandIndex++;
    }
    
    // Reached end of commands
    EndDialogue();
}

void DialogueManager::DisplayLine(const LineCommand& line) {
    std::cout << "[DialogueManager] DisplayLine: '" << line.speaker << ": " << line.text.substr(0, 50) << "...' "
              << "choices=" << line.choices.size() << std::endl;
    
    m_CurrentLine = &line;
    m_DisplayTimer = 0;
    m_State = DialogueState::DisplayingLine;
    
    // Apply variable substitution
    auto stateGetter = [this](const std::string& key) { return GetState(key); };
    m_FullText = VariableSubstitution::Substitute(line.text, stateGetter);
    
    m_DisplayedText = "";
    m_TypeIndex = 0;
    m_TypeTimer = 0;
    m_TypewriterComplete = (m_Config.textSpeed <= 0);
    
    if (m_TypewriterComplete) {
        m_DisplayedText = m_FullText;
    }
    
    // Fire event
    if (m_Callbacks.onLine) {
        DialogueLineEvent event;
        event.speaker = line.speaker;
        event.text = m_FullText;
        for (const auto& choice : line.choices) {
            if (choice.EvaluateCondition(m_GameState, m_QuestStatusGetter)) {
                event.choices.push_back(&choice);
            }
        }
        m_Callbacks.onLine(event);
    }
    
    // Transition to appropriate waiting state if instant display
    if (m_TypewriterComplete) {
        auto avail = GetAvailableChoices();
        std::cout << "[DialogueManager] DisplayLine instant: lineChoices=" << line.choices.size() 
                  << " availChoices=" << avail.size() << std::endl;
        if (!line.choices.empty() && !avail.empty()) {
            m_State = DialogueState::WaitingForChoice;
            std::cout << "[DialogueManager] DisplayLine -> WaitingForChoice" << std::endl;
        } else {
            m_State = DialogueState::WaitingForInput;
            std::cout << "[DialogueManager] DisplayLine -> WaitingForInput" << std::endl;
        }
    } else {
        std::cout << "[DialogueManager] DisplayLine: typewriter active, staying in DisplayingLine" << std::endl;
    }
}

void DialogueManager::HandleEmote(const EmoteCommand& cmd) {
    if (m_Callbacks.onEmote) {
        m_Callbacks.onEmote({cmd.emote});
    }
}

void DialogueManager::HandleCondition(const ConditionCommand& cmd) {
    auto stateGetter = [this](const std::string& key) { return GetState(key); };
    bool passed = ConditionEvaluator::Evaluate(cmd.condition, stateGetter, m_QuestStatusGetter);

    if (!passed) {
        // Skip subsequent commands until next line or end
        m_SkippingToEndCondition = true;
    }
}

void DialogueManager::HandleSetState(const SetStateCommand& cmd) {
    SetState(cmd.stateName, cmd.stateValue);
    
    if (m_Callbacks.onStateChange) {
        m_Callbacks.onStateChange({cmd.stateName, cmd.stateValue});
    }
}

void DialogueManager::HandleSetEmotion(const SetEmotionCommand& cmd) {
    std::cout << "[DialogueManager] HandleSetEmotion called with: " << cmd.emotionName << std::endl;
    if (m_Callbacks.onEmotion) {
        std::cout << "[DialogueManager] Firing onEmotion callback..." << std::endl;
        m_Callbacks.onEmotion({cmd.emotionName});
        std::cout << "[DialogueManager] onEmotion callback completed" << std::endl;
    } else {
        std::cout << "[DialogueManager] WARNING: onEmotion callback is null!" << std::endl;
    }
}

bool DialogueManager::HandleGoto(const GotoCommand& cmd) {
    auto it = m_CurrentConversation->labelIndex.find(cmd.targetLabel);
    if (it != m_CurrentConversation->labelIndex.end()) {
        m_CurrentCommandIndex = it->second;
        return true;
    } else {
        std::cerr << "[DialogueManager] Goto target not found: " << cmd.targetLabel << std::endl;
        return false;
    }
}

void DialogueManager::HandleGameCommand(const DialogueCommand& cmd) {
    switch (cmd.type) {
        case CommandType::GiveItem: {
            auto& c = static_cast<const GiveItemCommand&>(cmd);
            if (m_Callbacks.onGiveItem) m_Callbacks.onGiveItem(c.itemId, c.count);
            break;
        }
        case CommandType::TakeItem: {
            auto& c = static_cast<const TakeItemCommand&>(cmd);
            if (m_Callbacks.onTakeItem) m_Callbacks.onTakeItem(c.itemId, c.count);
            break;
        }
        case CommandType::StartQuest: {
            auto& c = static_cast<const StartQuestCommand&>(cmd);
            if (m_Callbacks.onStartQuest) m_Callbacks.onStartQuest(c.questId);
            break;
        }
        case CommandType::CompleteStep: {
            auto& c = static_cast<const CompleteStepCommand&>(cmd);
            if (m_Callbacks.onCompleteStep) m_Callbacks.onCompleteStep(c.questId, c.stepId);
            break;
        }
        case CommandType::PlayAnim: {
            auto& c = static_cast<const PlayAnimCommand&>(cmd);
            if (m_Callbacks.onPlayAnim) m_Callbacks.onPlayAnim(c.animationName);
            break;
        }
        case CommandType::PlaySound: {
            auto& c = static_cast<const PlaySoundCommand&>(cmd);
            if (m_Callbacks.onPlaySound) m_Callbacks.onPlaySound(c.soundPath);
            break;
        }
        case CommandType::Camera: {
            auto& c = static_cast<const CameraCommand&>(cmd);
            if (m_Callbacks.onCamera) m_Callbacks.onCamera(c.action, c.target);
            break;
        }
        case CommandType::SetEvent: {
            auto& c = static_cast<const SetEventCommand&>(cmd);
            // Store event state with event_ prefix for easy querying
            SetState("event_" + c.eventName, c.eventState);
            if (m_Callbacks.onSetEvent) m_Callbacks.onSetEvent(c.eventName, c.eventState);
            break;
        }
        default:
            break;
    }
}

void DialogueManager::EndDialogue() {
    m_State = DialogueState::Ending;
    
    if (m_Callbacks.onEnded) {
        m_Callbacks.onEnded();
    }
    
    m_CurrentConversation.reset();
    m_CurrentLine = nullptr;
    m_CurrentCommandIndex = 0;
    m_SpeakerEntityId = -1;
    m_SkippingToEndCondition = false;
    
    m_CooldownTimer = m_Config.endCooldown;
    m_State = DialogueState::Inactive;
    
    std::cout << "[DialogueManager] Dialogue ended" << std::endl;
}

//------------------------------------------------------------------------------
// Typewriter
//------------------------------------------------------------------------------
void DialogueManager::UpdateTypewriter(float dt) {
    if (m_TypewriterComplete || m_Config.textSpeed <= 0) return;
    
    m_TypeTimer += dt;
    float charDelay = 1.0f / m_Config.textSpeed;
    
    // Safety: prevent infinite loop if charDelay is too small
    int maxIterations = 10000;
    int iterations = 0;
    
    while (m_TypeTimer >= charDelay && m_TypeIndex < m_FullText.size()) {
        if (++iterations > maxIterations) {
            std::cerr << "[DialogueManager] WARNING: Typewriter loop exceeded max iterations!" << std::endl;
            break;
        }
        
        char c = m_FullText[m_TypeIndex];
        m_DisplayedText += c;
        m_TypeIndex++;
        m_TypeTimer -= charDelay;
        
        if (m_Callbacks.onCharacterTyped) {
            m_Callbacks.onCharacterTyped(c);
        }
        
        // Punctuation pauses
        if (c == ',' || c == ';') {
            m_TypeTimer -= m_Config.shortPauseDuration;
        } else if (c == '.' || c == '!' || c == '?') {
            // Only pause at end of sentence
            if (m_TypeIndex >= m_FullText.size() || std::isspace(m_FullText[m_TypeIndex])) {
                m_TypeTimer -= m_Config.longPauseDuration;
            }
        }
    }
    
    if (m_TypeIndex >= m_FullText.size()) {
        CompleteTyping();
    }
}

void DialogueManager::CompleteTyping() {
    m_DisplayedText = m_FullText;
    m_TypewriterComplete = true;
    
    if (m_Callbacks.onTypingComplete) {
        m_Callbacks.onTypingComplete();
    }
    
    // Transition to appropriate state
    bool hasChoices = m_CurrentLine && !m_CurrentLine->choices.empty();
    auto availChoices = GetAvailableChoices();
    std::cout << "[DialogueManager] CompleteTyping: hasChoices=" << hasChoices 
              << " availableChoices=" << availChoices.size() << std::endl;
    
    if (hasChoices && !availChoices.empty()) {
        m_State = DialogueState::WaitingForChoice;
        std::cout << "[DialogueManager] State -> WaitingForChoice" << std::endl;
    } else {
        m_State = DialogueState::WaitingForInput;
        std::cout << "[DialogueManager] State -> WaitingForInput" << std::endl;
    }
}

//------------------------------------------------------------------------------
// Evaluate inline conditions on a dialogue line
//------------------------------------------------------------------------------
bool DialogueManager::EvaluateLineConditions(const LineCommand& line) {
    if (line.inlineConditions.empty()) {
        return true; // No conditions = always show
    }
    
    auto stateGetter = [this](const std::string& key) { return GetState(key); };
    
    // Check for "else" first - it has special handling
    bool hasElse = false;
    for (const auto& condition : line.inlineConditions) {
        if (condition == "else") {
            hasElse = true;
            break;
        }
    }
    
    // Handle "else" - show only if we're in a conditional sequence and nothing matched yet
    if (hasElse) {
        if (m_InConditionalSequence && m_ConditionalSequenceMatched) {
            // Something already matched in this sequence, skip this else
            return false;
        }
        
        // Evaluate any other conditions on this line (else can combine with other conditions)
        for (const auto& condition : line.inlineConditions) {
            if (condition == "else") continue; // Skip the else itself
            
            bool passed = ConditionEvaluator::Evaluate(condition, stateGetter, m_QuestStatusGetter);
            if (!passed) {
                return false;
            }
        }
        
        // Else matches - mark the sequence as matched
        m_ConditionalSequenceMatched = true;
        return true;
    }
    
    // Regular conditional line (not else)
    // Start or continue a conditional sequence
    m_InConditionalSequence = true;
    
    // Evaluate all conditions (AND logic)
    for (const auto& condition : line.inlineConditions) {
        bool passed = ConditionEvaluator::Evaluate(condition, stateGetter, m_QuestStatusGetter);
        if (!passed) {
            return false; // Condition failed, skip this line
        }
    }
    
    // All conditions passed - mark the sequence as matched so subsequent else won't trigger
    m_ConditionalSequenceMatched = true;
    return true;
}

} // namespace Dialogue

