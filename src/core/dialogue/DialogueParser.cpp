#include "DialogueParser.h"
#include "core/vfs/FileSystem.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace Dialogue {

//------------------------------------------------------------------------------
// Static members
//------------------------------------------------------------------------------
std::vector<std::pair<std::regex, CustomCommandParser>> DialogueParser::s_CustomParsers;

//------------------------------------------------------------------------------
// LineCommand ToString
//------------------------------------------------------------------------------
std::string LineCommand::ToString() const {
    std::string result = "Line: ";
    if (!speaker.empty()) result += "[" + speaker + "] ";
    result += text;
    if (!choices.empty()) result += " (" + std::to_string(choices.size()) + " choices)";
    return result;
}

//------------------------------------------------------------------------------
// Conversation methods
//------------------------------------------------------------------------------
void Conversation::BuildLabelIndex() {
    labelIndex.clear();
    for (size_t i = 0; i < commands.size(); i++) {
        if (commands[i]->type == CommandType::Label) {
            auto* label = static_cast<LabelCommand*>(commands[i].get());
            labelIndex[label->labelName] = i;
        }
    }
}

std::string Conversation::ToString() const {
    std::string result = "Conversation: " + title + "\n";
    for (const auto& cmd : commands) {
        result += "  " + cmd->ToString() + "\n";
    }
    return result;
}

// Helper to deep copy a single command
static std::shared_ptr<DialogueCommand> DeepCopyCommand(const std::shared_ptr<DialogueCommand>& cmd) {
    if (!cmd) return nullptr;
    
    switch (cmd->type) {
        case CommandType::Line: {
            auto* src = static_cast<const LineCommand*>(cmd.get());
            auto copy = std::make_shared<LineCommand>();
            copy->speaker = src->speaker;
            copy->text = src->text;
            copy->inlineConditions = src->inlineConditions;
            copy->tags = src->tags;
            // Deep copy choices including their response commands
            for (const auto& choice : src->choices) {
                DialogueChoice choiceCopy;
                choiceCopy.text = choice.text;
                choiceCopy.condition = choice.condition;
                for (const auto& respCmd : choice.response) {
                    choiceCopy.response.push_back(DeepCopyCommand(respCmd));
                }
                copy->choices.push_back(std::move(choiceCopy));
            }
            return copy;
        }
        case CommandType::Emote: {
            auto* src = static_cast<const EmoteCommand*>(cmd.get());
            auto copy = std::make_shared<EmoteCommand>();
            copy->emote = src->emote;
            return copy;
        }
        case CommandType::Condition: {
            auto* src = static_cast<const ConditionCommand*>(cmd.get());
            auto copy = std::make_shared<ConditionCommand>();
            copy->condition = src->condition;
            return copy;
        }
        case CommandType::SetState: {
            auto* src = static_cast<const SetStateCommand*>(cmd.get());
            auto copy = std::make_shared<SetStateCommand>();
            copy->stateName = src->stateName;
            copy->stateValue = src->stateValue;
            return copy;
        }
        case CommandType::SetEmotion: {
            auto* src = static_cast<const SetEmotionCommand*>(cmd.get());
            auto copy = std::make_shared<SetEmotionCommand>();
            copy->emotionName = src->emotionName;
            return copy;
        }
        case CommandType::Label: {
            auto* src = static_cast<const LabelCommand*>(cmd.get());
            auto copy = std::make_shared<LabelCommand>();
            copy->labelName = src->labelName;
            return copy;
        }
        case CommandType::Goto: {
            auto* src = static_cast<const GotoCommand*>(cmd.get());
            auto copy = std::make_shared<GotoCommand>();
            copy->targetLabel = src->targetLabel;
            return copy;
        }
        case CommandType::End: {
            auto copy = std::make_shared<EndCommand>();
            return copy;
        }
        case CommandType::GiveItem: {
            auto* src = static_cast<const GiveItemCommand*>(cmd.get());
            auto copy = std::make_shared<GiveItemCommand>();
            copy->itemId = src->itemId;
            copy->count = src->count;
            return copy;
        }
        case CommandType::TakeItem: {
            auto* src = static_cast<const TakeItemCommand*>(cmd.get());
            auto copy = std::make_shared<TakeItemCommand>();
            copy->itemId = src->itemId;
            copy->count = src->count;
            return copy;
        }
        case CommandType::StartQuest: {
            auto* src = static_cast<const StartQuestCommand*>(cmd.get());
            auto copy = std::make_shared<StartQuestCommand>();
            copy->questId = src->questId;
            return copy;
        }
        case CommandType::CompleteStep: {
            auto* src = static_cast<const CompleteStepCommand*>(cmd.get());
            auto copy = std::make_shared<CompleteStepCommand>();
            copy->questId = src->questId;
            copy->stepId = src->stepId;
            return copy;
        }
        case CommandType::PlayAnim: {
            auto* src = static_cast<const PlayAnimCommand*>(cmd.get());
            auto copy = std::make_shared<PlayAnimCommand>();
            copy->animationName = src->animationName;
            copy->waitForComplete = src->waitForComplete;
            return copy;
        }
        case CommandType::PlaySound: {
            auto* src = static_cast<const PlaySoundCommand*>(cmd.get());
            auto copy = std::make_shared<PlaySoundCommand>();
            copy->soundPath = src->soundPath;
            return copy;
        }
        case CommandType::Wait: {
            auto* src = static_cast<const WaitCommand*>(cmd.get());
            auto copy = std::make_shared<WaitCommand>();
            copy->seconds = src->seconds;
            return copy;
        }
        case CommandType::Camera: {
            auto* src = static_cast<const CameraCommand*>(cmd.get());
            auto copy = std::make_shared<CameraCommand>();
            copy->action = src->action;
            copy->target = src->target;
            copy->parameters = src->parameters;
            return copy;
        }
        case CommandType::Custom: {
            auto* src = static_cast<const CustomCommand*>(cmd.get());
            auto copy = std::make_shared<CustomCommand>();
            copy->commandName = src->commandName;
            copy->arguments = src->arguments;
            return copy;
        }
        default:
            // Unknown command type - create empty command
            auto copy = std::make_shared<DialogueCommand>();
            copy->type = cmd->type;
            return copy;
    }
}

std::shared_ptr<Conversation> Conversation::DeepCopy() const {
    auto copy = std::make_shared<Conversation>();
    copy->title = title;
    copy->id = id;
    
    // Deep copy all commands
    copy->commands.reserve(commands.size());
    for (const auto& cmd : commands) {
        copy->commands.push_back(DeepCopyCommand(cmd));
    }
    
    // Rebuild label index for the copy
    copy->BuildLabelIndex();
    
    return copy;
}

//------------------------------------------------------------------------------
// DialogueChoice condition evaluation
//------------------------------------------------------------------------------
bool DialogueChoice::EvaluateCondition(const std::unordered_map<std::string, std::string>& state,
                                       QuestStatusGetter questGetter) const {
    if (condition.empty()) return true;
    
    auto getter = [&state](const std::string& key) -> std::string {
        auto it = state.find(key);
        return it != state.end() ? it->second : "";
    };
    
    return ConditionEvaluator::Evaluate(condition, getter, questGetter);
}

//------------------------------------------------------------------------------
// DialogueCondition evaluation
//------------------------------------------------------------------------------
bool DialogueCondition::Evaluate(
    const std::unordered_map<std::string, std::string>& state,
    const std::function<std::string(const std::string&, const std::string&)>& questStatusFn) const 
{
    // Check state condition
    if (!requiredStateKey.empty()) {
        auto it = state.find(requiredStateKey);
        std::string currentValue = it != state.end() ? it->second : "";
        if (currentValue != requiredStateValue) return false;
    }
    
    // Check quest condition
    if (!requiredQuestId.empty() && !requiredStepId.empty() && questStatusFn) {
        std::string status = questStatusFn(requiredQuestId, requiredStepId);
        if (status != requiredStepStatus) return false;
    }
    
    return true;
}

//------------------------------------------------------------------------------
// DialogueEntry GetConversation
//------------------------------------------------------------------------------
std::shared_ptr<Conversation> DialogueEntry::GetConversation() const {
    if (!parsed) {
        cachedConversation = DialogueParser::Parse(rawText);
        parsed = true;
    }
    return cachedConversation;
}

//------------------------------------------------------------------------------
// DialogueParser - Main parse function
//------------------------------------------------------------------------------
std::shared_ptr<Conversation> DialogueParser::Parse(const std::string& input, const std::string& defaultSpeaker) {
    if (input.empty()) return nullptr;
    
    ParserState state;
    state.conversation = std::make_shared<Conversation>();
    state.defaultSpeaker = defaultSpeaker;
    
    // Split into lines
    std::vector<std::string> lines;
    std::istringstream stream(input);
    std::string line;
    while (std::getline(stream, line)) {
        // Normalize line endings
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    
    // Parse each line
    for (state.currentIndex = 0; state.currentIndex < (int)lines.size(); state.currentIndex++) {
        const std::string& rawLine = lines[state.currentIndex];
        std::string trimmed = rawLine;
        
        // Trim leading/trailing whitespace
        size_t start = trimmed.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        trimmed = trimmed.substr(start);
        size_t end = trimmed.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) trimmed = trimmed.substr(0, end + 1);
        
        if (trimmed.empty()) continue;
        
        // Skip comments
        if (trimmed[0] == '#' || (trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/')) {
            continue;
        }
        
        auto cmd = ParseLine(rawLine, trimmed, state, lines);
        // Commands are added to conversation inside ParseLine
    }
    
    // Build label index
    state.conversation->BuildLabelIndex();
    
    return state.conversation;
}

//------------------------------------------------------------------------------
// Parse single line
//------------------------------------------------------------------------------
std::shared_ptr<DialogueCommand> DialogueParser::ParseLine(
    const std::string& rawLine, 
    const std::string& line, 
    ParserState& state,
    const std::vector<std::string>& allLines) 
{
    bool isIndented = !rawLine.empty() && (rawLine[0] == ' ' || rawLine[0] == '\t');
    bool isDoubleIndented = rawLine.size() >= 2 && 
        ((rawLine[0] == '\t' && rawLine[1] == '\t') ||
         (rawLine.size() >= 8 && rawLine.substr(0, 8) == "        "));
    bool isChoice = isIndented && !line.empty() && line[0] == '-';
    
    // Conversation title: {Title}
    if (line[0] == '{' && line.back() == '}') {
        state.conversation->title = line.substr(1, line.size() - 2);
        return nullptr;
    }
    
    // Label: @labelName
    if (line[0] == '@') {
        auto cmd = std::make_shared<LabelCommand>();
        cmd->labelName = line.substr(1);
        state.conversation->commands.push_back(cmd);
        return cmd;
    }
    
    // Condition block: <condition>
    if (!isIndented && line[0] == '<' && line.back() == '>') {
        auto cmd = std::make_shared<ConditionCommand>();
        cmd->condition = line.substr(1, line.size() - 2);
        state.conversation->commands.push_back(cmd);
        return cmd;
    }
    
    // Emote: (emote)
    if (!isIndented && line[0] == '(' && line.back() == ')') {
        auto cmd = std::make_shared<EmoteCommand>();
        cmd->emote = line.substr(1, line.size() - 2);
        state.conversation->commands.push_back(cmd);
        return cmd;
    }
    
    // Try built-in commands (\SetState, \goto, etc.)
    if (line[0] == '\\') {
        auto cmd = TryParseBuiltinCommand(line);
        if (cmd) {
            state.conversation->commands.push_back(cmd);
            return cmd;
        }
    }
    
    // Try custom commands
    auto customCmd = TryParseCustomCommand(line);
    if (customCmd) {
        state.conversation->commands.push_back(customCmd);
        return customCmd;
    }
    
    // Choice: - Choice text
    if (isChoice && state.lastDialogue) {
        std::string choiceText = line.substr(1);
        // Trim leading space
        size_t textStart = choiceText.find_first_not_of(" \t");
        if (textStart != std::string::npos) {
            choiceText = choiceText.substr(textStart);
        }
        
        // Check for condition on choice: [condition] text
        std::string choiceCondition;
        if (choiceText[0] == '[') {
            size_t endBracket = choiceText.find(']');
            if (endBracket != std::string::npos) {
                choiceCondition = choiceText.substr(1, endBracket - 1);
                choiceText = choiceText.substr(endBracket + 1);
                // Trim leading space after condition
                size_t start = choiceText.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    choiceText = choiceText.substr(start);
                }
            }
        }
        
        DialogueChoice choice;
        choice.text = choiceText;
        choice.condition = choiceCondition;
        
        // Collect nested response lines (double-indented)
        std::vector<std::string> nestedLines;
        state.currentIndex++;
        while (state.currentIndex < (int)allLines.size()) {
            const std::string& nextRaw = allLines[state.currentIndex];
            // Check for double indentation
            bool isNested = nextRaw.size() >= 2 && 
                ((nextRaw[0] == '\t' && nextRaw[1] == '\t') ||
                 (nextRaw.size() >= 8 && nextRaw.substr(0, 8) == "        "));
            if (!isNested) break;
            
            // Remove one level of indentation
            std::string stripped = nextRaw;
            if (stripped[0] == '\t') stripped = stripped.substr(1);
            else if (stripped.size() >= 4) stripped = stripped.substr(4);
            
            nestedLines.push_back(stripped);
            state.currentIndex++;
        }
        state.currentIndex--; // Back up one since the loop will increment
        
        // Parse nested commands
        choice.response = ParseNestedBranch(nestedLines, state.defaultSpeaker);
        
        state.lastDialogue->choices.push_back(std::move(choice));
        return nullptr;
    }
    
    // Regular dialogue line
    std::shared_ptr<EmoteCommand> emote;
    auto dialogueLine = ParseDialogueLine(line, state.defaultSpeaker, emote);
    
    if (emote) {
        state.conversation->commands.push_back(emote);
    }
    
    state.conversation->commands.push_back(dialogueLine);
    state.lastDialogue = dialogueLine.get();
    
    return dialogueLine;
}

//------------------------------------------------------------------------------
// Parse dialogue line
//------------------------------------------------------------------------------
std::shared_ptr<LineCommand> DialogueParser::ParseDialogueLine(
    const std::string& line, 
    const std::string& defaultSpeaker,
    std::shared_ptr<EmoteCommand>& outEmote) 
{
    auto cmd = std::make_shared<LineCommand>();
    std::string text = line;
    outEmote = nullptr;
    
    // Check for leading emote: (emote) text
    if (text[0] == '(') {
        size_t closePos = text.find(')');
        if (closePos != std::string::npos) {
            outEmote = std::make_shared<EmoteCommand>();
            outEmote->emote = text.substr(1, closePos - 1);
            text = text.substr(closePos + 1);
            // Trim leading space
            size_t start = text.find_first_not_of(" \t");
            if (start != std::string::npos) text = text.substr(start);
        }
    }
    
    // Check for speaker: Speaker: text
    size_t colonPos = text.find(':');
    if (colonPos != std::string::npos && colonPos < text.size() - 1) {
        std::string potentialSpeaker = text.substr(0, colonPos);
        // Verify it's a valid speaker name (alphanumeric + underscore)
        bool validSpeaker = true;
        for (char c : potentialSpeaker) {
            if (!std::isalnum(c) && c != '_') {
                validSpeaker = false;
                break;
            }
        }
        if (validSpeaker && !potentialSpeaker.empty()) {
            cmd->speaker = potentialSpeaker;
            text = text.substr(colonPos + 1);
            // Trim leading space
            size_t start = text.find_first_not_of(" \t");
            if (start != std::string::npos) text = text.substr(start);
        }
    }
    
    if (cmd->speaker.empty()) {
        cmd->speaker = defaultSpeaker;
    }
    
    // Extract inline conditions: <condition>
    std::regex condRegex("<([^>]+)>");
    std::smatch match;
    std::string temp = text;
    while (std::regex_search(temp, match, condRegex)) {
        cmd->inlineConditions.push_back(match[1].str());
        temp = match.suffix().str();
    }
    text = std::regex_replace(text, condRegex, "");
    
    // Extract tags: [tag]
    std::regex tagRegex("\\[([^\\]]+)\\]");
    temp = text;
    while (std::regex_search(temp, match, tagRegex)) {
        cmd->tags.push_back(match[1].str());
        temp = match.suffix().str();
    }
    text = std::regex_replace(text, tagRegex, "");
    
    // Trim final text
    size_t start = text.find_first_not_of(" \t");
    size_t end = text.find_last_not_of(" \t");
    if (start != std::string::npos && end != std::string::npos) {
        text = text.substr(start, end - start + 1);
    }
    
    cmd->text = text;
    return cmd;
}

//------------------------------------------------------------------------------
// Parse nested branch (choice responses)
//------------------------------------------------------------------------------
std::vector<std::shared_ptr<DialogueCommand>> DialogueParser::ParseNestedBranch(
    const std::vector<std::string>& lines, 
    const std::string& speakerName) 
{
    std::vector<std::shared_ptr<DialogueCommand>> commands;
    
    for (const std::string& rawLine : lines) {
        std::string line = rawLine;
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        
        if (line.empty()) continue;
        
        // Emote
        if (line[0] == '(' && line.back() == ')') {
            auto cmd = std::make_shared<EmoteCommand>();
            cmd->emote = line.substr(1, line.size() - 2);
            commands.push_back(cmd);
            continue;
        }
        
        // Condition
        if (line[0] == '<' && line.back() == '>') {
            auto cmd = std::make_shared<ConditionCommand>();
            cmd->condition = line.substr(1, line.size() - 2);
            commands.push_back(cmd);
            continue;
        }
        
        // Built-in commands
        if (line[0] == '\\') {
            auto cmd = TryParseBuiltinCommand(line);
            if (cmd) {
                commands.push_back(cmd);
                continue;
            }
        }
        
        // Custom command
        auto customCmd = TryParseCustomCommand(line);
        if (customCmd) {
            commands.push_back(customCmd);
            continue;
        }
        
        // Dialogue line
        std::shared_ptr<EmoteCommand> emote;
        auto dialogueLine = ParseDialogueLine(line, speakerName, emote);
        if (emote) commands.push_back(emote);
        commands.push_back(dialogueLine);
    }
    
    return commands;
}

//------------------------------------------------------------------------------
// Try parse built-in command
//------------------------------------------------------------------------------
std::shared_ptr<DialogueCommand> DialogueParser::TryParseBuiltinCommand(const std::string& line) {
    std::smatch match;
    
    // \end
    if (line == "\\end" || line == "\\End") {
        return std::make_shared<EndCommand>();
    }
    
    // \goto(label)
    std::regex gotoRegex(R"(\\goto\(([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, gotoRegex)) {
        auto cmd = std::make_shared<GotoCommand>();
        cmd->targetLabel = match[1].str();
        return cmd;
    }
    
    // \SetState(name) or \SetState(name, value)
    std::regex setStateRegex(R"(\\SetState\(([^,)]+)(?:,\s*([^)]+))?\))", std::regex::icase);
    if (std::regex_match(line, match, setStateRegex)) {
        auto cmd = std::make_shared<SetStateCommand>();
        cmd->stateName = match[1].str();
        cmd->stateValue = match[2].matched ? match[2].str() : "true";
        return cmd;
    }
    
    // \SetEvent(name, state) - state can be Started or Finished (NotStarted is default)
    std::regex setEventRegex(R"(\\SetEvent\(([^,)]+),\s*([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, setEventRegex)) {
        auto cmd = std::make_shared<SetEventCommand>();
        cmd->eventName = match[1].str();
        cmd->eventState = match[2].str();
        return cmd;
    }
    
    // \SetEmotion(name)
    std::regex emotionRegex(R"(\\SetEmotion\(([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, emotionRegex)) {
        auto cmd = std::make_shared<SetEmotionCommand>();
        cmd->emotionName = match[1].str();
        std::cout << "[DialogueParser] Parsed SetEmotion command: " << cmd->emotionName << std::endl;
        return cmd;
    }
    
    // \GiveItem(id, count)
    std::regex giveItemRegex(R"(\\GiveItem\(([^,)]+)(?:,\s*(\d+))?\))", std::regex::icase);
    if (std::regex_match(line, match, giveItemRegex)) {
        auto cmd = std::make_shared<GiveItemCommand>();
        cmd->itemId = match[1].str();
        cmd->count = match[2].matched ? std::stoi(match[2].str()) : 1;
        return cmd;
    }
    
    // \TakeItem(id, count)
    std::regex takeItemRegex(R"(\\TakeItem\(([^,)]+)(?:,\s*(\d+))?\))", std::regex::icase);
    if (std::regex_match(line, match, takeItemRegex)) {
        auto cmd = std::make_shared<TakeItemCommand>();
        cmd->itemId = match[1].str();
        cmd->count = match[2].matched ? std::stoi(match[2].str()) : 1;
        return cmd;
    }
    
    // \StartQuest(id)
    std::regex startQuestRegex(R"(\\StartQuest\(([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, startQuestRegex)) {
        auto cmd = std::make_shared<StartQuestCommand>();
        cmd->questId = match[1].str();
        return cmd;
    }
    
    // \CompleteStep(questId, stepId)
    std::regex completeStepRegex(R"(\\CompleteStep\(([^,]+),\s*([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, completeStepRegex)) {
        auto cmd = std::make_shared<CompleteStepCommand>();
        cmd->questId = match[1].str();
        cmd->stepId = match[2].str();
        return cmd;
    }
    
    // \PlayAnim(name)
    std::regex animRegex(R"(\\PlayAnim\(([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, animRegex)) {
        auto cmd = std::make_shared<PlayAnimCommand>();
        cmd->animationName = match[1].str();
        return cmd;
    }
    
    // \PlaySound(path)
    std::regex soundRegex(R"(\\PlaySound\(([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, soundRegex)) {
        auto cmd = std::make_shared<PlaySoundCommand>();
        cmd->soundPath = match[1].str();
        return cmd;
    }
    
    // \Wait(seconds)
    std::regex waitRegex(R"(\\Wait\(([^)]+)\))", std::regex::icase);
    if (std::regex_match(line, match, waitRegex)) {
        auto cmd = std::make_shared<WaitCommand>();
        try { cmd->seconds = std::stof(match[1].str()); } catch (...) {}
        return cmd;
    }
    
    // \Camera(action, target)
    std::regex cameraRegex(R"(\\Camera\(([^,)]+)(?:,\s*([^)]+))?\))", std::regex::icase);
    if (std::regex_match(line, match, cameraRegex)) {
        auto cmd = std::make_shared<CameraCommand>();
        cmd->action = match[1].str();
        if (match[2].matched) cmd->target = match[2].str();
        return cmd;
    }
    
    return nullptr;
}

//------------------------------------------------------------------------------
// Try parse custom command
//------------------------------------------------------------------------------
std::shared_ptr<DialogueCommand> DialogueParser::TryParseCustomCommand(const std::string& line) {
    for (const auto& [pattern, parser] : s_CustomParsers) {
        if (std::regex_match(line, pattern)) {
            return parser(line);
        }
    }
    return nullptr;
}

//------------------------------------------------------------------------------
// Register custom parser
//------------------------------------------------------------------------------
void DialogueParser::RegisterCustomParser(const std::string& pattern, CustomCommandParser parser) {
    s_CustomParsers.emplace_back(std::regex(pattern, std::regex::icase), std::move(parser));
}

void DialogueParser::ClearCustomParsers() {
    s_CustomParsers.clear();
}

//------------------------------------------------------------------------------
// Parse from file
//------------------------------------------------------------------------------
std::shared_ptr<Conversation> DialogueParser::ParseFromFile(const std::string& filePath, const std::string& defaultSpeaker) {
    std::string text;
    if (FileSystem::Instance().ReadTextFile(filePath, text)) {
        return Parse(text, defaultSpeaker);
    }

    if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
        std::cerr << "[DialogueParser] Failed to read file from VFS: " << filePath << std::endl;
        return nullptr;
    }

    std::ifstream file(filePath);
    if (!file.is_open()) {
        std::cerr << "[DialogueParser] Failed to open file: " << filePath << std::endl;
        return nullptr;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return Parse(buffer.str(), defaultSpeaker);
}

//------------------------------------------------------------------------------
// Format conversation to script text
//------------------------------------------------------------------------------
std::string DialogueParser::Format(const Conversation& conversation) {
    std::vector<std::string> lines;
    lines.push_back("{" + conversation.title + "}");
    
    for (const auto& cmd : conversation.commands) {
        FormatCommand(*cmd, lines, 0);
    }
    
    std::string result;
    for (const auto& line : lines) {
        result += line + "\n";
    }
    return result;
}

void DialogueParser::FormatCommand(const DialogueCommand& cmd, std::vector<std::string>& lines, int indentLevel) {
    std::string indent(indentLevel * 4, ' ');
    
    switch (cmd.type) {
        case CommandType::Line: {
            const auto& line = static_cast<const LineCommand&>(cmd);
            std::string text;
            if (!line.speaker.empty()) text = line.speaker + ": ";
            text += line.text;
            for (const auto& cond : line.inlineConditions) text += " <" + cond + ">";
            for (const auto& tag : line.tags) text += " [" + tag + "]";
            lines.push_back(indent + text);
            
            for (const auto& choice : line.choices) {
                std::string choiceLine = indent + "    - ";
                if (!choice.condition.empty()) choiceLine += "[" + choice.condition + "] ";
                choiceLine += choice.text;
                lines.push_back(choiceLine);
                for (const auto& resp : choice.response) {
                    FormatCommand(*resp, lines, indentLevel + 2);
                }
            }
            break;
        }
        case CommandType::Emote:
            lines.push_back(indent + "(" + static_cast<const EmoteCommand&>(cmd).emote + ")");
            break;
        case CommandType::Condition:
            lines.push_back(indent + "<" + static_cast<const ConditionCommand&>(cmd).condition + ">");
            break;
        case CommandType::SetState: {
            const auto& sc = static_cast<const SetStateCommand&>(cmd);
            if (sc.stateValue == "true") lines.push_back(indent + "\\SetState(" + sc.stateName + ")");
            else lines.push_back(indent + "\\SetState(" + sc.stateName + ", " + sc.stateValue + ")");
            break;
        }
        case CommandType::SetEmotion:
            lines.push_back(indent + "\\SetEmotion(" + static_cast<const SetEmotionCommand&>(cmd).emotionName + ")");
            break;
        case CommandType::Label:
            lines.push_back(indent + "@" + static_cast<const LabelCommand&>(cmd).labelName);
            break;
        case CommandType::Goto:
            lines.push_back(indent + "\\goto(" + static_cast<const GotoCommand&>(cmd).targetLabel + ")");
            break;
        case CommandType::End:
            lines.push_back(indent + "\\end");
            break;
        case CommandType::GiveItem: {
            const auto& gc = static_cast<const GiveItemCommand&>(cmd);
            lines.push_back(indent + "\\GiveItem(" + gc.itemId + ", " + std::to_string(gc.count) + ")");
            break;
        }
        case CommandType::TakeItem: {
            const auto& tc = static_cast<const TakeItemCommand&>(cmd);
            lines.push_back(indent + "\\TakeItem(" + tc.itemId + ", " + std::to_string(tc.count) + ")");
            break;
        }
        case CommandType::StartQuest:
            lines.push_back(indent + "\\StartQuest(" + static_cast<const StartQuestCommand&>(cmd).questId + ")");
            break;
        case CommandType::CompleteStep: {
            const auto& cs = static_cast<const CompleteStepCommand&>(cmd);
            lines.push_back(indent + "\\CompleteStep(" + cs.questId + ", " + cs.stepId + ")");
            break;
        }
        case CommandType::PlayAnim:
            lines.push_back(indent + "\\PlayAnim(" + static_cast<const PlayAnimCommand&>(cmd).animationName + ")");
            break;
        case CommandType::PlaySound:
            lines.push_back(indent + "\\PlaySound(" + static_cast<const PlaySoundCommand&>(cmd).soundPath + ")");
            break;
        case CommandType::Wait:
            lines.push_back(indent + "\\Wait(" + std::to_string(static_cast<const WaitCommand&>(cmd).seconds) + ")");
            break;
        case CommandType::Camera: {
            const auto& cc = static_cast<const CameraCommand&>(cmd);
            if (cc.target.empty()) lines.push_back(indent + "\\Camera(" + cc.action + ")");
            else lines.push_back(indent + "\\Camera(" + cc.action + ", " + cc.target + ")");
            break;
        }
        case CommandType::SetEvent: {
            const auto& se = static_cast<const SetEventCommand&>(cmd);
            lines.push_back(indent + "\\SetEvent(" + se.eventName + ", " + se.eventState + ")");
            break;
        }
        default:
            lines.push_back(indent + "# Unknown: " + cmd.ToString());
            break;
    }
}

//------------------------------------------------------------------------------
// ConditionEvaluator
//------------------------------------------------------------------------------
bool ConditionEvaluator::Evaluate(const std::string& condition, StateGetter stateGetter,
                                  QuestStatusGetter questGetter) {
    return EvaluateCompound(condition, stateGetter, questGetter);
}

bool ConditionEvaluator::EvaluateCompound(const std::string& condition, StateGetter stateGetter,
                                          QuestStatusGetter questGetter) {
    // Check for AND (&&)
    size_t andPos = condition.find("&&");
    if (andPos != std::string::npos) {
        std::string left = condition.substr(0, andPos);
        std::string right = condition.substr(andPos + 2);
        return EvaluateCompound(left, stateGetter, questGetter) && 
               EvaluateCompound(right, stateGetter, questGetter);
    }
    
    // Check for OR (||)
    size_t orPos = condition.find("||");
    if (orPos != std::string::npos) {
        std::string left = condition.substr(0, orPos);
        std::string right = condition.substr(orPos + 2);
        return EvaluateCompound(left, stateGetter, questGetter) || 
               EvaluateCompound(right, stateGetter, questGetter);
    }
    
    return EvaluateSingle(condition, stateGetter, questGetter);
}

// Helper: Get value for quest-prefixed keys like "quest.Q_ForestWolves.state"
std::string ConditionEvaluator::GetQuestValue(const std::string& key, QuestStatusGetter questGetter) {
    if (!questGetter) return "";
    
    // Check for quest. prefix
    if (key.substr(0, 6) != "quest.") return "";
    
    // Parse quest.QUEST_ID.property
    size_t firstDot = 5;  // After "quest"
    size_t secondDot = key.find('.', firstDot + 1);
    
    if (secondDot == std::string::npos) return "";
    
    std::string questId = key.substr(firstDot + 1, secondDot - firstDot - 1);
    std::string property = key.substr(secondDot + 1);
    
    // Supported properties:
    //   state   -> "not_started", "active", "completed", "failed"
    //   stage   -> active stage ID
    //   active  -> "true" if quest state is active
    //   completed -> "true" if quest state is completed
    //   started -> "true" if quest has been started (active or completed)
    
    if (property == "state") {
        return questGetter(questId, "state");
    } else if (property == "stage") {
        return questGetter(questId, "stage");
    } else if (property == "active") {
        std::string state = questGetter(questId, "state");
        return (state == "active" || state == "1") ? "true" : "false";
    } else if (property == "completed") {
        std::string state = questGetter(questId, "state");
        return (state == "completed" || state == "2") ? "true" : "false";
    } else if (property == "failed") {
        std::string state = questGetter(questId, "state");
        return (state == "failed" || state == "3") ? "true" : "false";
    } else if (property == "started") {
        std::string state = questGetter(questId, "state");
        return (state != "not_started" && state != "0" && !state.empty()) ? "true" : "false";
    } else {
        // Assume it's a stage ID - check if we're on that stage
        std::string activeStage = questGetter(questId, "stage");
        return (activeStage == property) ? "true" : "false";
    }
}

bool ConditionEvaluator::EvaluateSingle(const std::string& condition, StateGetter stateGetter,
                                        QuestStatusGetter questGetter) {
    std::string cond = condition;
    
    // Trim
    size_t start = cond.find_first_not_of(" \t");
    size_t end = cond.find_last_not_of(" \t");
    if (start == std::string::npos) return true;
    cond = cond.substr(start, end - start + 1);
    
    // Check negation
    bool negate = cond[0] == '!';
    if (negate) cond = cond.substr(1);
    
    // Value getter that supports both state and quest prefixes
    auto getValue = [&](const std::string& key) -> std::string {
        // Check for quest-prefixed key
        if (key.substr(0, 6) == "quest.") {
            return GetQuestValue(key, questGetter);
        }
        return stateGetter(key);
    };
    
    // Comparison operators
    auto evalComparison = [&](const std::string& op, auto comparator) -> std::pair<bool, bool> {
        size_t pos = cond.find(op);
        if (pos == std::string::npos) return {false, false};
        
        std::string key = cond.substr(0, pos);
        std::string valStr = cond.substr(pos + op.size());
        
        // Trim
        size_t ks = key.find_first_not_of(" \t");
        size_t ke = key.find_last_not_of(" \t");
        if (ks != std::string::npos) key = key.substr(ks, ke - ks + 1);
        
        size_t vs = valStr.find_first_not_of(" \t");
        size_t ve = valStr.find_last_not_of(" \t");
        if (vs != std::string::npos) valStr = valStr.substr(vs, ve - vs + 1);
        
        std::string currentVal = getValue(key);
        
        // Try numeric comparison
        try {
            int current = std::stoi(currentVal);
            int threshold = std::stoi(valStr);
            return {true, comparator(current, threshold)};
        } catch (...) {
            // String comparison
            return {true, comparator(currentVal, valStr)};
        }
    };
    
    auto [found, result] = evalComparison(">=", [](auto a, auto b) { return a >= b; });
    if (found) return negate ? !result : result;
    
    std::tie(found, result) = evalComparison("<=", [](auto a, auto b) { return a <= b; });
    if (found) return negate ? !result : result;
    
    std::tie(found, result) = evalComparison("!=", [](auto a, auto b) { return a != b; });
    if (found) return negate ? !result : result;
    
    std::tie(found, result) = evalComparison("==", [](auto a, auto b) { return a == b; });
    if (found) return negate ? !result : result;
    
    std::tie(found, result) = evalComparison(">", [](auto a, auto b) { return a > b; });
    if (found) return negate ? !result : result;
    
    std::tie(found, result) = evalComparison("<", [](auto a, auto b) { return a < b; });
    if (found) return negate ? !result : result;
    
    // Simple boolean check
    std::string val = getValue(cond);
    bool truthy = !val.empty() && val != "false" && val != "0";
    return negate ? !truthy : truthy;
}

//------------------------------------------------------------------------------
// VariableSubstitution
//------------------------------------------------------------------------------
std::string VariableSubstitution::Substitute(const std::string& text, StateGetter stateGetter) {
    if (text.find('{') == std::string::npos) return text;
    
    std::string result;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            size_t end = text.find('}', i);
            if (end != std::string::npos) {
                std::string varName = text.substr(i + 1, end - i - 1);
                std::string value = stateGetter(varName);
                result += value.empty() ? text.substr(i, end - i + 1) : value;
                i = end + 1;
                continue;
            }
        }
        result += text[i];
        i++;
    }
    return result;
}

} // namespace Dialogue

