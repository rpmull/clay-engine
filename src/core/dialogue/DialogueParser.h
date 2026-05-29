#pragma once

#include "DialogueTypes.h"
#include <string>
#include <memory>
#include <functional>
#include <regex>

namespace Dialogue {

//------------------------------------------------------------------------------
// Custom Command Parser Callback
//------------------------------------------------------------------------------
using CustomCommandParser = std::function<std::shared_ptr<DialogueCommand>(const std::string& line)>;

//------------------------------------------------------------------------------
// Dialogue Parser
// Parses .dlg script files into Conversation structures
//------------------------------------------------------------------------------
class DialogueParser {
public:
    // Parse dialogue text into a Conversation
    static std::shared_ptr<Conversation> Parse(const std::string& input, const std::string& defaultSpeaker = "");
    
    // Parse from file path
    static std::shared_ptr<Conversation> ParseFromFile(const std::string& filePath, const std::string& defaultSpeaker = "");
    
    // Format a Conversation back to dialogue script text
    static std::string Format(const Conversation& conversation);
    
    // Register custom command parser
    static void RegisterCustomParser(const std::string& pattern, CustomCommandParser parser);
    
    // Clear all custom parsers
    static void ClearCustomParsers();
    
private:
    struct ParserState {
        std::shared_ptr<Conversation> conversation;
        LineCommand* lastDialogue = nullptr;
        DialogueChoice* currentChoice = nullptr;
        std::string defaultSpeaker;
        int currentIndex = 0;
    };
    
    // Parsing helpers
    static std::shared_ptr<DialogueCommand> ParseLine(const std::string& rawLine, const std::string& line, ParserState& state, const std::vector<std::string>& allLines);
    static std::shared_ptr<LineCommand> ParseDialogueLine(const std::string& line, const std::string& defaultSpeaker, std::shared_ptr<EmoteCommand>& outEmote);
    static std::vector<std::shared_ptr<DialogueCommand>> ParseNestedBranch(const std::vector<std::string>& lines, const std::string& speakerName);
    static std::shared_ptr<DialogueCommand> TryParseCustomCommand(const std::string& line);
    static std::shared_ptr<DialogueCommand> TryParseBuiltinCommand(const std::string& line);
    
    // Formatting helpers
    static void FormatCommand(const DialogueCommand& cmd, std::vector<std::string>& lines, int indentLevel);
    
    // Custom parser registry
    static std::vector<std::pair<std::regex, CustomCommandParser>> s_CustomParsers;
};

//------------------------------------------------------------------------------
// Condition Evaluator
// Evaluates condition strings against game state
//------------------------------------------------------------------------------
class ConditionEvaluator {
public:
    using StateGetter = std::function<std::string(const std::string&)>;
    using QuestStatusGetter = std::function<std::string(const std::string&, const std::string&)>;

    // Evaluate a condition string
    // Supports: key, !key, key==value, key!=value, key>=value, key<=value, key>value, key<value
    // Also supports quest-prefixed conditions:
    //   quest.QUEST_ID.state       -> returns "not_started", "active", "completed", "failed"
    //   quest.QUEST_ID.stage       -> returns active stage ID
    //   quest.QUEST_ID.active      -> evaluates to true if quest state is "active"
    //   quest.QUEST_ID.completed   -> evaluates to true if quest state is "completed"
    static bool Evaluate(const std::string& condition, StateGetter stateGetter, 
                        QuestStatusGetter questGetter = nullptr);

    // Parse and evaluate compound conditions (AND, OR)
    static bool EvaluateCompound(const std::string& condition, StateGetter stateGetter,
                                QuestStatusGetter questGetter = nullptr);

private:
    static bool EvaluateSingle(const std::string& condition, StateGetter stateGetter,
                              QuestStatusGetter questGetter = nullptr);
    
    // Parse quest-prefixed condition (returns empty if not a quest condition)
    static std::string GetQuestValue(const std::string& key, QuestStatusGetter questGetter);
};

//------------------------------------------------------------------------------
// Variable Substitution
// Replaces {varName} in text with state values
//------------------------------------------------------------------------------
class VariableSubstitution {
public:
    using StateGetter = std::function<std::string(const std::string&)>;
    
    static std::string Substitute(const std::string& text, StateGetter stateGetter);
};

} // namespace Dialogue

