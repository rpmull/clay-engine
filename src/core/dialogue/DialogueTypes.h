#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <variant>
#include <functional>
#include "../assets/AssetReference.h"

namespace Dialogue {

//------------------------------------------------------------------------------
// Forward declarations
//------------------------------------------------------------------------------
struct DialogueCommand;
struct Conversation;
class DialogueLibrary;

//------------------------------------------------------------------------------
// Command Types Enum
//------------------------------------------------------------------------------
enum class CommandType : uint32_t {
    Unknown = 0,
    Line,           // Dialogue line with speaker and text
    Choice,         // Player choice option
    Emote,          // Animation/emote cue
    Condition,      // Conditional block
    SetState,       // Set game state variable
    SetEmotion,     // Set character emotion
    Label,          // Jump target label
    Goto,           // Jump to label
    End,            // End conversation
    
    // Extended commands
    GiveItem,
    TakeItem,
    StartQuest,
    CompleteStep,
    PlayAnim,
    PlaySound,
    Wait,
    Camera,
    SetEvent,       // Set event state (NotStarted/Started/Finished)
    Custom,         // User-defined command
};

//------------------------------------------------------------------------------
// Choice - Player dialogue choice
//------------------------------------------------------------------------------
struct DialogueChoice {
    std::string text;
    std::string condition;  // Optional condition for showing this choice
    std::vector<std::shared_ptr<DialogueCommand>> response;
    
    // Quest status getter: (questId, "state"|"stage"|stageId) -> status string
    using QuestStatusGetter = std::function<std::string(const std::string&, const std::string&)>;
    
    // Evaluate condition - supports quest-prefixed conditions like:
    //   quest.Q_ForestWolves.state==active
    //   quest.Q_ForestWolves.stage==QS_ForestHunt
    //   quest.Q_ForestWolves.active (shorthand for state==active)
    bool EvaluateCondition(const std::unordered_map<std::string, std::string>& state,
                          QuestStatusGetter questGetter = nullptr) const;
};

//------------------------------------------------------------------------------
// Base Command Structure
//------------------------------------------------------------------------------
struct DialogueCommand {
    CommandType type = CommandType::Unknown;
    
    virtual ~DialogueCommand() = default;
    virtual std::string ToString() const { return "Unknown"; }
};

//------------------------------------------------------------------------------
// Dialogue Line Command
//------------------------------------------------------------------------------
struct LineCommand : DialogueCommand {
    std::string speaker;
    std::string text;
    std::vector<std::string> inlineConditions;
    std::vector<std::string> tags;
    std::vector<DialogueChoice> choices;
    
    LineCommand() { type = CommandType::Line; }
    std::string ToString() const override;
};

//------------------------------------------------------------------------------
// Emote Command (animation/camera cue)
//------------------------------------------------------------------------------
struct EmoteCommand : DialogueCommand {
    std::string emote;
    
    EmoteCommand() { type = CommandType::Emote; }
    std::string ToString() const override { return "Emote: " + emote; }
};

//------------------------------------------------------------------------------
// Condition Command
//------------------------------------------------------------------------------
struct ConditionCommand : DialogueCommand {
    std::string condition;
    
    ConditionCommand() { type = CommandType::Condition; }
    std::string ToString() const override { return "Condition: " + condition; }
};

//------------------------------------------------------------------------------
// SetState Command
//------------------------------------------------------------------------------
struct SetStateCommand : DialogueCommand {
    std::string stateName;
    std::string stateValue;
    
    SetStateCommand() { type = CommandType::SetState; }
    std::string ToString() const override { return "SetState: " + stateName + " = " + stateValue; }
};

//------------------------------------------------------------------------------
// SetEmotion Command
//------------------------------------------------------------------------------
struct SetEmotionCommand : DialogueCommand {
    std::string emotionName;
    
    SetEmotionCommand() { type = CommandType::SetEmotion; }
    std::string ToString() const override { return "SetEmotion: " + emotionName; }
};

//------------------------------------------------------------------------------
// Label Command (jump target)
//------------------------------------------------------------------------------
struct LabelCommand : DialogueCommand {
    std::string labelName;
    
    LabelCommand() { type = CommandType::Label; }
    std::string ToString() const override { return "Label: " + labelName; }
};

//------------------------------------------------------------------------------
// Goto Command
//------------------------------------------------------------------------------
struct GotoCommand : DialogueCommand {
    std::string targetLabel;
    
    GotoCommand() { type = CommandType::Goto; }
    std::string ToString() const override { return "Goto: " + targetLabel; }
};

//------------------------------------------------------------------------------
// End Conversation Command
//------------------------------------------------------------------------------
struct EndCommand : DialogueCommand {
    EndCommand() { type = CommandType::End; }
    std::string ToString() const override { return "End"; }
};

//------------------------------------------------------------------------------
// Extended Commands
//------------------------------------------------------------------------------
struct GiveItemCommand : DialogueCommand {
    std::string itemId;
    int count = 1;
    
    GiveItemCommand() { type = CommandType::GiveItem; }
    std::string ToString() const override { return "GiveItem: " + itemId + " x" + std::to_string(count); }
};

struct TakeItemCommand : DialogueCommand {
    std::string itemId;
    int count = 1;
    
    TakeItemCommand() { type = CommandType::TakeItem; }
    std::string ToString() const override { return "TakeItem: " + itemId + " x" + std::to_string(count); }
};

struct StartQuestCommand : DialogueCommand {
    std::string questId;
    
    StartQuestCommand() { type = CommandType::StartQuest; }
    std::string ToString() const override { return "StartQuest: " + questId; }
};

struct CompleteStepCommand : DialogueCommand {
    std::string questId;
    std::string stepId;
    
    CompleteStepCommand() { type = CommandType::CompleteStep; }
    std::string ToString() const override { return "CompleteStep: " + questId + "." + stepId; }
};

struct PlayAnimCommand : DialogueCommand {
    std::string animationName;
    bool waitForComplete = false;
    
    PlayAnimCommand() { type = CommandType::PlayAnim; }
    std::string ToString() const override { return "PlayAnim: " + animationName; }
};

struct PlaySoundCommand : DialogueCommand {
    std::string soundPath;
    
    PlaySoundCommand() { type = CommandType::PlaySound; }
    std::string ToString() const override { return "PlaySound: " + soundPath; }
};

struct WaitCommand : DialogueCommand {
    float seconds = 0.0f;
    
    WaitCommand() { type = CommandType::Wait; }
    std::string ToString() const override { return "Wait: " + std::to_string(seconds) + "s"; }
};

struct CameraCommand : DialogueCommand {
    std::string action;
    std::string target;
    std::unordered_map<std::string, std::string> parameters;
    
    CameraCommand() { type = CommandType::Camera; }
    std::string ToString() const override { return "Camera: " + action + " -> " + target; }
};

struct SetEventCommand : DialogueCommand {
    std::string eventName;
    std::string eventState;  // "NotStarted", "Started", "Finished"
    
    SetEventCommand() { type = CommandType::SetEvent; }
    std::string ToString() const override { return "SetEvent: " + eventName + " -> " + eventState; }
};

struct CustomCommand : DialogueCommand {
    std::string commandName;
    std::vector<std::string> arguments;
    
    CustomCommand() { type = CommandType::Custom; }
    std::string ToString() const override { return "Custom: " + commandName; }
};

//------------------------------------------------------------------------------
// Conversation - A complete dialogue tree
//------------------------------------------------------------------------------
struct Conversation {
    std::string title;
    std::string id;  // Unique identifier within library
    std::vector<std::shared_ptr<DialogueCommand>> commands;
    
    // Built label index for fast goto resolution
    std::unordered_map<std::string, size_t> labelIndex;
    
    void BuildLabelIndex();
    std::string ToString() const;
    
    // Deep copy the conversation (including all commands and choices)
    // Used to avoid modifying cached library conversations at runtime
    std::shared_ptr<Conversation> DeepCopy() const;
};

//------------------------------------------------------------------------------
// Dialogue Condition (for library entry selection)
//------------------------------------------------------------------------------
struct DialogueCondition {
    std::string requiredQuestId;
    std::string requiredStepId;
    std::string requiredStepStatus;
    std::string requiredStateKey;
    std::string requiredStateValue;
    
    bool Evaluate(const std::unordered_map<std::string, std::string>& state,
                  const std::function<std::string(const std::string&, const std::string&)>& questStatusFn) const;
};

//------------------------------------------------------------------------------
// Dialogue Entry - Single entry in a library
//------------------------------------------------------------------------------
struct DialogueEntry {
    std::string entryId;
    std::string displayName;
    std::string rawText;           // Raw dialogue script text
    bool isDefault = false;
    DialogueCondition condition;
    
    // Cached parsed conversation (parsed on demand)
    mutable std::shared_ptr<Conversation> cachedConversation;
    mutable bool parsed = false;
    
    std::shared_ptr<Conversation> GetConversation() const;
    void InvalidateCache() { parsed = false; cachedConversation.reset(); }
};

//------------------------------------------------------------------------------
// Dialogue Events for runtime callbacks
//------------------------------------------------------------------------------
struct DialogueLineEvent {
    std::string speaker;
    std::string text;
    std::vector<const DialogueChoice*> choices;
    bool hasChoices() const { return !choices.empty(); }
};

struct DialogueEmoteEvent {
    std::string emoteName;
};

struct DialogueStateEvent {
    std::string stateName;
    std::string stateValue;
};

struct DialogueEmotionEvent {
    std::string emotionName;
};

//------------------------------------------------------------------------------
// Dialogue Runtime State
//------------------------------------------------------------------------------
enum class DialogueState {
    Inactive,
    DisplayingLine,
    WaitingForInput,
    WaitingForChoice,
    ProcessingCommand,
    Paused,
    Ending
};

} // namespace Dialogue

