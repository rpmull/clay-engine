#include "QuestEditorPanel.h"
#include "core/quest/QuestSystem.h"
#include "editor/pipeline/AssetLibrary.h"
#include "ui/utility/DialogueLibraryAssetListCache.h"
#include <imgui.h>
#include <imgui_clay_inspector.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#undef LoadLibrary
#endif

//------------------------------------------------------------------------------
// File Operations
//------------------------------------------------------------------------------
void QuestEditorPanel::SetDatabase(std::shared_ptr<Quest::QuestDatabase> db) {
    m_Database = std::move(db);
    m_SelectedQuest = -1;
    m_SelectedStage = -1;
    m_SelectedObjective = -1;
    m_Modified = false;
    m_GraphNeedsLayout = true;
}

bool QuestEditorPanel::NewDatabase() {
    m_Database = std::make_shared<Quest::QuestDatabase>();
    m_Database->SetGuid(ClaymoreGUID::Generate());
    m_OpenPath.clear();
    m_SelectedQuest = -1;
    m_SelectedStage = -1;
    m_Modified = false;
    m_GraphNeedsLayout = true;
    return true;
}

bool QuestEditorPanel::LoadDatabase(const std::string& path) {
    auto db = Quest::QuestDatabase::LoadFromFile(path);
    if (!db) {
        std::cerr << "[QuestEditor] Failed to load: " << path << std::endl;
        return false;
    }
    m_Database = std::move(db);
    m_OpenPath = path;
    m_SelectedQuest = -1;
    m_SelectedStage = -1;
    m_Modified = false;
    m_GraphNeedsLayout = true;
    std::cout << "[QuestEditor] Loaded: " << path << std::endl;
    return true;
}

bool QuestEditorPanel::SaveDatabase(const std::string& path) {
    if (!m_Database) return false;
    std::string savePath = path.empty() ? m_OpenPath : path;
    if (savePath.empty()) {
        return SaveDatabaseDialog();
    }
    
    m_Database->EnsureIdentifiers();
    if (m_Database->SaveToFile(savePath)) {
        m_OpenPath = savePath;
        m_Modified = false;
        std::cout << "[QuestEditor] Saved: " << savePath << std::endl;
        return true;
    }
    return false;
}

void QuestEditorPanel::OpenDatabaseDialog() {
#ifdef _WIN32
    OPENFILENAMEA ofn = {};
    char filename[MAX_PATH] = "";
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Quest Database (*.quest.json)\0*.quest.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "quest.json";
    if (GetOpenFileNameA(&ofn)) {
        LoadDatabase(filename);
    }
#endif
}

bool QuestEditorPanel::SaveDatabaseDialog() {
#ifdef _WIN32
    OPENFILENAMEA ofn = {};
    char filename[MAX_PATH] = "";
    if (!m_OpenPath.empty()) {
        strncpy(filename, m_OpenPath.c_str(), MAX_PATH - 1);
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Quest Database (*.quest.json)\0*.quest.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "quest.json";
    if (GetSaveFileNameA(&ofn)) {
        return SaveDatabase(filename);
    }
#endif
    return false;
}

//------------------------------------------------------------------------------
// Main Render
//------------------------------------------------------------------------------
void QuestEditorPanel::OnImGuiRender() {
    if (!m_Open) {
        m_WindowFocusedOrHovered = false;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(1400, 800), ImGuiCond_FirstUseEver);
    std::string title = GetWindowTitle();
    
    if (!ImGui::Begin(title.c_str(), &m_Open, ImGuiWindowFlags_MenuBar)) {
        m_WindowFocusedOrHovered = false;
        ImGui::End();
        return;
    }
    m_WindowFocusedOrHovered =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows);

    DrawMenuBar();
    DrawToolbar();

    if (ImGui::BeginTabBar("QuestEditorTabs")) {
        if (ImGui::BeginTabItem("Designer")) {
            DrawDesignerTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Graph")) {
            DrawGraphTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Runtime")) {
            DrawRuntimeTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void QuestEditorPanel::DrawMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Database", "Ctrl+N")) NewDatabase();
            if (ImGui::MenuItem("Open...", "Ctrl+O")) OpenDatabaseDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, m_Database != nullptr)) SaveDatabase();
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_Database != nullptr)) SaveDatabaseDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Close")) m_Open = false;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Add Quest", nullptr, false, m_Database != nullptr)) AddNewQuest();
            if (ImGui::MenuItem("Duplicate Quest", nullptr, false, m_SelectedQuest >= 0)) DuplicateQuest(m_SelectedQuest);
            if (ImGui::MenuItem("Delete Quest", "Delete", false, m_SelectedQuest >= 0)) DeleteQuest(m_SelectedQuest);
            ImGui::Separator();
            if (ImGui::MenuItem("Add Stage", nullptr, false, m_SelectedQuest >= 0)) AddNewStage();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Auto-Layout Graph")) AutoLayoutGraph();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void QuestEditorPanel::DrawToolbar() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    if (ImGui::Button("New")) NewDatabase();
    ImGui::SameLine();
    if (ImGui::Button("Open")) OpenDatabaseDialog();
    ImGui::SameLine();
    if (ImGui::Button("Save")) SaveDatabase();
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
    
    ImGui::BeginDisabled(m_Database == nullptr);
    if (ImGui::Button("+ Quest")) AddNewQuest();
    ImGui::EndDisabled();
    
    ImGui::SameLine();
    ImGui::BeginDisabled(m_SelectedQuest < 0);
    if (ImGui::Button("+ Stage")) AddNewStage();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    if (m_Database) {
        ImGui::Text("Quests: %zu", m_Database->GetQuests().size());
        ImGui::SameLine();
        if (m_SelectedQuest >= 0 && m_SelectedQuest < static_cast<int>(m_Database->GetQuests().size())) {
            ImGui::Text("| Stages: %zu", m_Database->GetQuests()[m_SelectedQuest].stages.size());
        }
    }

    ImGui::PopStyleVar();
    ImGui::Separator();
}

//------------------------------------------------------------------------------
// Designer Tab
//------------------------------------------------------------------------------
void QuestEditorPanel::DrawDesignerTab() {
    if (!m_Database) {
        ImGui::TextDisabled("No quest database loaded. Create a new database or open an existing one.");
        return;
    }

    const float availHeight = ImGui::GetContentRegionAvail().y;
    const float availWidth = ImGui::GetContentRegionAvail().x;

    constexpr float minQuestList = 200.0f;
    constexpr float minStageList = 200.0f;
    constexpr float minInspector = 260.0f;

    const float maxQuestList = std::max(minQuestList, availWidth - m_SplitterSize * 2.0f - minStageList - minInspector);
    m_QuestListWidth = std::clamp(m_QuestListWidth, minQuestList, maxQuestList);
    const float maxStageList = std::max(minStageList, availWidth - m_SplitterSize * 2.0f - m_QuestListWidth - minInspector);
    m_StageListWidth = std::clamp(m_StageListWidth, minStageList, maxStageList);

    // Left panel - Quest List
    ImGui::BeginChild("QuestListPanel", ImVec2(m_QuestListWidth, availHeight), true);
    DrawQuestList();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::ClaySplitterConfig splitter1;
    splitter1.Vertical = true;
    splitter1.Thickness = m_SplitterSize;
    splitter1.MinPrimary = minQuestList;
    splitter1.MinSecondary = minStageList + minInspector;
    splitter1.HoverCursor = ImGuiMouseCursor_ResizeEW;
    ImGui::ClaySplitter("QuestEditor_Splitter1", &m_QuestListWidth, availWidth, availHeight, splitter1);

    ImGui::SameLine();

    // Middle panel - Stage List and Quest Inspector
    float middleWidth = m_StageListWidth;
    ImGui::BeginChild("StageListPanel", ImVec2(middleWidth, availHeight), true);
    if (m_SelectedQuest >= 0) {
        DrawStageList();
    } else {
        ImGui::TextDisabled("Select a quest to view stages.");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::ClaySplitterConfig splitter2;
    splitter2.Vertical = true;
    splitter2.Thickness = m_SplitterSize;
    splitter2.MinPrimary = minStageList;
    splitter2.MinSecondary = minInspector;
    splitter2.HoverCursor = ImGuiMouseCursor_ResizeEW;
    const float secondRegionWidth = availWidth - m_QuestListWidth - m_SplitterSize;
    ImGui::ClaySplitter("QuestEditor_Splitter2", &m_StageListWidth, secondRegionWidth, availHeight, splitter2);

    ImGui::SameLine();

    // Right panel - Inspector
    ImGui::BeginChild("InspectorPanel", ImVec2(0, availHeight), true);
    if (m_SelectedStage >= 0) {
        DrawStageInspector();
    } else if (m_SelectedQuest >= 0) {
        DrawQuestInspector();
    } else {
        ImGui::TextDisabled("Select a quest or stage to edit properties.");
    }
    ImGui::EndChild();
}

void QuestEditorPanel::DrawQuestList() {
    ImGui::Text("Quests");
    ImGui::Separator();

    auto& quests = m_Database->GetQuests();

    for (int i = 0; i < static_cast<int>(quests.size()); ++i) {
        auto& quest = quests[i];
        ImGui::PushID(i);
        
        bool selected = (m_SelectedQuest == i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        // Quest icon and type indicator
        const char* icon = quest.flags.mainQuest ? "[M]" : "[S]";
        std::string label = std::string(icon) + " " + quest.displayName;
        
        ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", label.c_str());
        
        if (ImGui::IsItemClicked()) {
            m_SelectedQuest = i;
            m_SelectedStage = -1;
            m_SelectedObjective = -1;
        }

        // Show quest ID on hover
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("ID: %s", quest.questId.c_str());
            ImGui::Text("Category: %s", quest.category.c_str());
            ImGui::Text("Stages: %zu", quest.stages.size());
            ImGui::EndTooltip();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Duplicate")) {
                DuplicateQuest(i);
            }
            if (ImGui::MenuItem("Move Up", nullptr, false, i > 0)) {
                MoveQuestUp(i);
            }
            if (ImGui::MenuItem("Move Down", nullptr, false, i < static_cast<int>(quests.size()) - 1)) {
                MoveQuestDown(i);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                DeleteQuest(i);
            }
            ImGui::EndPopup();
        }

        // Show quest ID below name
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu stages)", quest.stages.size());
        
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("+ Add Quest", ImVec2(-1, 0))) {
        AddNewQuest();
    }
}

void QuestEditorPanel::DrawStageList() {
    if (m_SelectedQuest < 0 || m_SelectedQuest >= static_cast<int>(m_Database->GetQuests().size())) return;
    
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];

    ImGui::Text("Stages - %s", quest.displayName.c_str());
    ImGui::Separator();

        for (int i = 0; i < static_cast<int>(quest.stages.size()); ++i) {
        auto& stage = quest.stages[i];
        ImGui::PushID(i);

            bool selected = (m_SelectedStage == i);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selected) flags |= ImGuiTreeNodeFlags_Selected;

        std::string stateIcon;
        switch (stage.initialState) {
            case Quest::StageState::Active: stateIcon = "[A]"; break;
            case Quest::StageState::Completed: stateIcon = "[C]"; break;
            case Quest::StageState::Failed: stateIcon = "[F]"; break;
            default: stateIcon = "[ ]"; break;
        }

        std::string label = stateIcon + " " + stage.name;
        ImGui::TreeNodeEx((void*)(intptr_t)i, flags, "%s", label.c_str());

        if (ImGui::IsItemClicked()) {
                m_SelectedStage = i;
            m_SelectedObjective = -1;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("ID: %s", stage.stageId.c_str());
            ImGui::Text("Objectives: %zu", stage.objectives.size());
            ImGui::Text("Outcomes: %zu", stage.outcomes.size());
            ImGui::EndTooltip();
        }

        // Context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Duplicate")) {
                DuplicateStage(i);
            }
            if (ImGui::MenuItem("Move Up", nullptr, false, i > 0)) {
                MoveStageUp(i);
            }
            if (ImGui::MenuItem("Move Down", nullptr, false, i < static_cast<int>(quest.stages.size()) - 1)) {
                MoveStageDown(i);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                DeleteStage(i);
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

        ImGui::Separator();
    if (ImGui::Button("+ Add Stage", ImVec2(-1, 0))) {
        AddNewStage();
    }
}

void QuestEditorPanel::DrawQuestInspector() {
    if (m_SelectedQuest < 0 || m_SelectedQuest >= static_cast<int>(m_Database->GetQuests().size())) return;
    
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];

    ImGui::Text("Quest Properties");
    ImGui::Separator();

    ImGui::TextDisabled("ID: %s", quest.questId.c_str());

    if (DrawInputText("Display Name", quest.displayName)) MarkModified();
    if (DrawInputTextMultiline("Description", quest.description)) MarkModified();
    if (DrawInputText("Category", quest.category)) MarkModified();

    ImGui::Separator();
    ImGui::Text("Quest Giver & Requirements");
    
    DrawResourceGuidPicker("Quest Giver NPC", quest.questGiver, "npc");
    
    if (ImGui::InputInt("Recommended Level", &quest.recommendedLevel)) MarkModified();

    ImGui::Separator();
    ImGui::Text("Flags");
    
    if (ImGui::Checkbox("Main Quest", &quest.flags.mainQuest)) MarkModified();
    ImGui::SameLine();
    if (ImGui::Checkbox("Repeatable", &quest.flags.repeatable)) MarkModified();
    
    if (ImGui::Checkbox("Trackable", &quest.flags.trackable)) MarkModified();
    ImGui::SameLine();
    if (ImGui::Checkbox("Hidden Until Started", &quest.flags.hiddenUntilStarted)) MarkModified();

    if (ImGui::InputInt("Priority", &quest.flags.priority)) MarkModified();

    // Prerequisites
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Prerequisites", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < quest.prerequisiteQuestIds.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (DrawInputText("##prereq", quest.prerequisiteQuestIds[i])) MarkModified();
            ImGui::SameLine();
            if (ImGui::Button("X")) {
                quest.prerequisiteQuestIds.erase(quest.prerequisiteQuestIds.begin() + i);
                MarkModified();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add Prerequisite")) {
            quest.prerequisiteQuestIds.push_back("");
            MarkModified();
        }
    }

    // Completion Rewards
    if (ImGui::CollapsingHeader("Completion Rewards")) {
        for (size_t i = 0; i < quest.completionRewards.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            DrawRewardEditor(quest.completionRewards[i], static_cast<int>(i));
            ImGui::SameLine();
            if (ImGui::Button("X##reward")) {
                quest.completionRewards.erase(quest.completionRewards.begin() + i);
                MarkModified();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        if (ImGui::Button("+ Add Reward")) {
            Quest::QuestReward reward;
            reward.rewardId = Quest::GenerateRewardId();
            quest.completionRewards.push_back(reward);
            MarkModified();
        }
    }

    // Branches summary
    if (ImGui::CollapsingHeader("Branches")) {
        for (size_t i = 0; i < quest.branches.size(); ++i) {
            auto& branch = quest.branches[i];
            ImGui::PushID(static_cast<int>(i));
            
            if (DrawInputText("Branch Name", branch.name)) MarkModified();
            if (DrawInputText("Description", branch.description)) MarkModified();
            if (DrawInputText("Consequence", branch.consequenceDescription)) MarkModified();
            ImGui::TextDisabled("ID: %s", branch.branchId.c_str());
            
            if (ImGui::Button("Delete Branch")) {
                quest.branches.erase(quest.branches.begin() + i);
                MarkModified();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        if (ImGui::Button("+ Add Branch")) {
            Quest::QuestBranch branch;
            branch.branchId = Quest::GenerateBranchId();
            branch.name = "New Branch";
            quest.branches.push_back(branch);
            MarkModified();
        }
    }
}

void QuestEditorPanel::DrawStageInspector() {
    if (m_SelectedQuest < 0 || m_SelectedQuest >= static_cast<int>(m_Database->GetQuests().size())) return;
    if (m_SelectedStage < 0) return;

    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    if (m_SelectedStage >= static_cast<int>(quest.stages.size())) return;

    auto& stage = quest.stages[m_SelectedStage];

    ImGui::Text("Stage Properties");
    ImGui::Separator();

    ImGui::TextDisabled("ID: %s", stage.stageId.c_str());

    if (DrawInputText("Stage Name", stage.name)) MarkModified();
    if (DrawInputTextMultiline("Description", stage.description, 60.0f)) MarkModified();
    if (DrawInputTextMultiline("Journal Entry", stage.journalEntry, 80.0f)) MarkModified();

    DrawStageStateCombo("Initial State", stage.initialState);

    // Timeout settings
    ImGui::Separator();
    ImGui::Text("Timeout");
    if (ImGui::InputFloat("Timeout (seconds)", &stage.timeoutSeconds, 1.0f, 10.0f, "%.1f")) MarkModified();
    if (stage.timeoutSeconds > 0) {
        if (DrawInputText("On Timeout -> Stage", stage.onTimeoutStageId)) MarkModified();
        if (ImGui::Checkbox("On Timeout -> Fail Quest", &stage.onTimeoutFail)) MarkModified();
    }

    // Objectives
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Objectives", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < stage.objectives.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            std::string header = stage.objectives[i].displayText.empty() 
                ? ("Objective " + std::to_string(i + 1))
                : stage.objectives[i].displayText;
            
            if (ImGui::TreeNode(header.c_str())) {
                DrawObjectiveEditor(stage.objectives[i], static_cast<int>(i));
                if (ImGui::Button("Delete Objective")) {
                    stage.objectives.erase(stage.objectives.begin() + i);
                    MarkModified();
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add Objective")) {
            Quest::Objective obj;
            obj.objectiveId = Quest::GenerateObjectiveId();
            obj.type = Quest::ObjectiveType::Custom;
            obj.displayText = "New Objective";
            obj.targetCount = 1;
            stage.objectives.push_back(obj);
            MarkModified();
        }
    }

    // Conditions
    if (ImGui::CollapsingHeader("Conditions")) {
        for (size_t i = 0; i < stage.conditions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(("Condition " + std::to_string(i + 1)).c_str())) {
                DrawConditionEditor(stage.conditions[i], static_cast<int>(i));
                if (ImGui::Button("Delete Condition")) {
                    stage.conditions.erase(stage.conditions.begin() + i);
                    MarkModified();
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add Condition")) {
            Quest::StageCondition cond;
            stage.conditions.push_back(cond);
            MarkModified();
        }
    }

    // Outcomes
    if (ImGui::CollapsingHeader("Outcomes", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (size_t i = 0; i < stage.outcomes.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            std::string outcomeLabel = "Outcome " + std::to_string(i + 1);
            if (stage.outcomes[i].completeQuest) outcomeLabel += " [COMPLETE]";
            if (stage.outcomes[i].failQuest) outcomeLabel += " [FAIL]";
            if (!stage.outcomes[i].nextStageId.empty()) outcomeLabel += " -> " + stage.outcomes[i].nextStageId;

            if (ImGui::TreeNode(outcomeLabel.c_str())) {
                DrawOutcomeEditor(stage.outcomes[i], static_cast<int>(i));
                if (ImGui::Button("Delete Outcome")) {
                    stage.outcomes.erase(stage.outcomes.begin() + i);
                    MarkModified();
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add Outcome")) {
            Quest::StageOutcome outcome;
            stage.outcomes.push_back(outcome);
            MarkModified();
        }
    }

    // Dialogue Triggers
    if (ImGui::CollapsingHeader("Dialogue Triggers")) {
        for (size_t i = 0; i < stage.dialogueTriggers.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::TreeNode(("Dialogue " + std::to_string(i + 1)).c_str())) {
                DrawDialogueTriggerEditor(stage.dialogueTriggers[i], static_cast<int>(i));
                if (ImGui::Button("Delete Trigger")) {
                    stage.dialogueTriggers.erase(stage.dialogueTriggers.begin() + i);
                    MarkModified();
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (ImGui::Button("+ Add Dialogue Trigger")) {
            Quest::DialogueTrigger trigger;
            trigger.triggerCondition = "on_active";
            stage.dialogueTriggers.push_back(trigger);
            MarkModified();
        }
    }
}

//------------------------------------------------------------------------------
// Sub-editors
//------------------------------------------------------------------------------
void QuestEditorPanel::DrawObjectiveEditor(Quest::Objective& obj, int index) {
    ImGui::TextDisabled("ID: %s", obj.objectiveId.c_str());
    
    DrawObjectiveTypeCombo("Type", obj.type);
    if (DrawInputText("Display Text", obj.displayText)) MarkModified();
    if (DrawInputText("Complete Text", obj.displayTextComplete)) MarkModified();
    if (ImGui::InputInt("Target Count", &obj.targetCount)) MarkModified();

    DrawResourceGuidPicker("Target Resource", obj.targetResourceGuid, "any");

    if (obj.type == Quest::ObjectiveType::Talk) {
        DrawResourceGuidPicker("Dialogue Library", obj.dialogueLibraryGuid, "dialogue");
        if (DrawInputText("Dialogue Entry ID", obj.dialogueEntryId)) MarkModified();
    }

    if (obj.type == Quest::ObjectiveType::ReachLocation) {
        ImGui::Text("Location:");
        if (ImGui::InputFloat3("Position", &obj.location.x)) MarkModified();
        if (ImGui::InputFloat("Radius", &obj.location.radius)) MarkModified();
        if (DrawInputText("Location Name", obj.location.locationName)) MarkModified();
        if (DrawInputText("Scene ID", obj.location.sceneId)) MarkModified();
    }

    if (ImGui::Checkbox("Optional", &obj.optional)) MarkModified();
    ImGui::SameLine();
    if (ImGui::Checkbox("Hidden", &obj.hidden)) MarkModified();

    if (ImGui::InputInt("Priority", &obj.priority)) MarkModified();
    if (DrawInputText("Hint Text", obj.hintText)) MarkModified();
}

void QuestEditorPanel::DrawConditionEditor(Quest::StageCondition& cond, int index) {
    if (DrawInputText("Required Quest ID", cond.requiredQuestId)) MarkModified();
    if (DrawInputText("Required Stage ID", cond.requiredStageId)) MarkModified();
    
    const char* states[] = { "Inactive", "Active", "Completed", "Failed" };
    int currentState = 0;
    if (cond.requiredState == "Active") currentState = 1;
    else if (cond.requiredState == "Completed") currentState = 2;
    else if (cond.requiredState == "Failed") currentState = 3;
    
    if (ImGui::Combo("Required State", &currentState, states, 4)) {
        cond.requiredState = states[currentState];
        MarkModified();
    }

    if (DrawInputText("Required Game State (key=value)", cond.requiredGameState)) MarkModified();
    DrawResourceGuidPicker("Required Item", cond.requiredItemGuid, "item");
    if (!cond.requiredItemGuid.empty()) {
        if (ImGui::InputInt("Required Item Count", &cond.requiredItemCount)) MarkModified();
    }
    if (ImGui::Checkbox("Invert Condition (NOT)", &cond.invertCondition)) MarkModified();
}

void QuestEditorPanel::DrawOutcomeEditor(Quest::StageOutcome& outcome, int index) {
    if (DrawInputText("Next Stage ID", outcome.nextStageId)) MarkModified();
    if (DrawInputText("Branch ID", outcome.branchId)) MarkModified();
    
    if (ImGui::Checkbox("Complete Quest", &outcome.completeQuest)) MarkModified();
    ImGui::SameLine();
    if (ImGui::Checkbox("Fail Quest", &outcome.failQuest)) MarkModified();

    if (DrawInputText("Set Game State Key", outcome.gameStateKey)) MarkModified();
    if (!outcome.gameStateKey.empty()) {
        if (DrawInputText("Set Game State Value", outcome.gameStateValue)) MarkModified();
    }

    // Unlock quests
    ImGui::Text("Unlock Quests:");
    for (size_t i = 0; i < outcome.unlockQuestIds.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (DrawInputText("##unlock", outcome.unlockQuestIds[i])) MarkModified();
        ImGui::SameLine();
        if (ImGui::Button("X##unlock")) {
            outcome.unlockQuestIds.erase(outcome.unlockQuestIds.begin() + i);
            MarkModified();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("+ Unlock Quest")) {
        outcome.unlockQuestIds.push_back("");
        MarkModified();
    }

    // Rewards
    if (!outcome.rewards.empty() || ImGui::TreeNode("Stage Rewards")) {
        for (size_t i = 0; i < outcome.rewards.size(); ++i) {
            ImGui::PushID(static_cast<int>(i) + 1000);
            DrawRewardEditor(outcome.rewards[i], static_cast<int>(i));
            ImGui::SameLine();
            if (ImGui::Button("X")) {
                outcome.rewards.erase(outcome.rewards.begin() + i);
                MarkModified();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("+ Add Reward")) {
            Quest::QuestReward reward;
            reward.rewardId = Quest::GenerateRewardId();
            outcome.rewards.push_back(reward);
            MarkModified();
        }
        if (!outcome.rewards.empty()) {
            ImGui::TreePop();
        }
    }
}

void QuestEditorPanel::DrawRewardEditor(Quest::QuestReward& reward, int index) {
    const char* types[] = { "item", "currency", "xp", "reputation" };
    int currentType = 0;
    for (int i = 0; i < 4; ++i) {
        if (reward.rewardType == types[i]) { currentType = i; break; }
    }
    if (ImGui::Combo("Reward Type", &currentType, types, 4)) {
        reward.rewardType = types[currentType];
        MarkModified();
    }
    
    DrawResourceGuidPicker("Resource", reward.resourceGuid, reward.rewardType.c_str());
    if (ImGui::InputInt("Amount", &reward.amount)) MarkModified();
    if (ImGui::Checkbox("Optional Choice", &reward.optional)) MarkModified();
}

void QuestEditorPanel::DrawDialogueTriggerEditor(Quest::DialogueTrigger& trigger, int index) {
    DrawResourceGuidPicker("Dialogue Library", trigger.dialogueLibraryGuid, "dialogue");
    if (DrawInputText("Entry ID", trigger.entryId)) MarkModified();
    
    const char* conditions[] = { "on_start", "on_active", "on_complete", "on_fail" };
    int current = 1;
    for (int i = 0; i < 4; ++i) {
        if (trigger.triggerCondition == conditions[i]) { current = i; break; }
    }
    if (ImGui::Combo("Trigger Condition", &current, conditions, 4)) {
        trigger.triggerCondition = conditions[current];
        MarkModified();
    }

    DrawResourceGuidPicker("Speaker NPC", trigger.speakerResourceGuid, "npc");
}

//------------------------------------------------------------------------------
// Graph Tab
//------------------------------------------------------------------------------
void QuestEditorPanel::DrawGraphTab() {
    if (!m_Database) {
        ImGui::TextDisabled("No quest database loaded.");
        return;
    }

    if (m_GraphNeedsLayout) {
        UpdateGraphNodePositions();
        AutoLayoutGraph();
        m_GraphNeedsLayout = false;
    }

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    // Draw background
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(40, 40, 45, 255));

    // Draw grid
    float gridSize = 50.0f * m_GraphZoom;
    for (float x = fmod(m_GraphScrollX, gridSize); x < canvasSize.x; x += gridSize) {
        drawList->AddLine(ImVec2(canvasPos.x + x, canvasPos.y), ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y), IM_COL32(60, 60, 65, 255));
    }
    for (float y = fmod(m_GraphScrollY, gridSize); y < canvasSize.y; y += gridSize) {
        drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + y), ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y), IM_COL32(60, 60, 65, 255));
    }

    ImGui::InvisibleButton("canvas", canvasSize);
    
    // Handle panning
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        m_GraphScrollX += ImGui::GetIO().MouseDelta.x;
        m_GraphScrollY += ImGui::GetIO().MouseDelta.y;
    }

    // Handle zoom
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0) {
            m_GraphZoom = std::clamp(m_GraphZoom + wheel * 0.1f, 0.5f, 2.0f);
        }
    }

    DrawGraphConnections();
    DrawGraphNodes();

    // Instructions
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x + 10, canvasPos.y + 10));
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Right-drag: Pan | Scroll: Zoom | Select quest to highlight");
}

void QuestEditorPanel::DrawGraphNodes() {
    if (m_SelectedQuest < 0 || m_SelectedQuest >= static_cast<int>(m_Database->GetQuests().size())) return;
    
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    float nodeWidth = 150 * m_GraphZoom;
    float nodeHeight = 60 * m_GraphZoom;
    float startX = canvasPos.x + m_GraphScrollX + 50;
    float startY = canvasPos.y + m_GraphScrollY + 50;

    for (size_t i = 0; i < quest.stages.size(); ++i) {
        auto& stage = quest.stages[i];
        
        float x = startX + (i % 4) * (nodeWidth + 50);
        float y = startY + (i / 4) * (nodeHeight + 80);

        // Find graph node position if exists
        for (auto& node : quest.editorGraphNodes) {
            if (node.nodeId == stage.stageId) {
                x = canvasPos.x + m_GraphScrollX + node.x * m_GraphZoom;
                y = canvasPos.y + m_GraphScrollY + node.y * m_GraphZoom;
                break;
            }
        }

        ImVec2 nodeMin(x, y);
        ImVec2 nodeMax(x + nodeWidth, y + nodeHeight);

        // Node color based on state
        ImU32 nodeColor = IM_COL32(70, 70, 80, 255);
        if (static_cast<int>(i) == m_SelectedStage) {
            nodeColor = IM_COL32(100, 150, 200, 255);
        }
        if (stage.initialState == Quest::StageState::Active) {
            nodeColor = IM_COL32(100, 180, 100, 255);
        }

        // Draw node
        drawList->AddRectFilled(nodeMin, nodeMax, nodeColor, 8.0f);
        drawList->AddRect(nodeMin, nodeMax, IM_COL32(150, 150, 160, 255), 8.0f, 0, 2.0f);

        // Draw text
        ImVec2 textPos(x + 8, y + 8);
        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), stage.name.c_str());
        
        std::string objText = std::to_string(stage.objectives.size()) + " objectives";
        drawList->AddText(ImVec2(x + 8, y + 28), IM_COL32(180, 180, 180, 255), objText.c_str());

        // Handle click
        ImGui::SetCursorScreenPos(nodeMin);
        ImGui::InvisibleButton(stage.stageId.c_str(), ImVec2(nodeWidth, nodeHeight));
        if (ImGui::IsItemClicked()) {
            m_SelectedStage = static_cast<int>(i);
        }
    }
}

void QuestEditorPanel::DrawGraphConnections() {
    if (m_SelectedQuest < 0 || m_SelectedQuest >= static_cast<int>(m_Database->GetQuests().size())) return;
    
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();

    float nodeWidth = 150 * m_GraphZoom;
    float nodeHeight = 60 * m_GraphZoom;

    // Build stage position map
    std::unordered_map<std::string, ImVec2> stagePositions;
    float startX = canvasPos.x + m_GraphScrollX + 50;
    float startY = canvasPos.y + m_GraphScrollY + 50;

    for (size_t i = 0; i < quest.stages.size(); ++i) {
        auto& stage = quest.stages[i];
        float x = startX + (i % 4) * (nodeWidth + 50);
        float y = startY + (i / 4) * (nodeHeight + 80);

        for (auto& node : quest.editorGraphNodes) {
            if (node.nodeId == stage.stageId) {
                x = canvasPos.x + m_GraphScrollX + node.x * m_GraphZoom;
                y = canvasPos.y + m_GraphScrollY + node.y * m_GraphZoom;
                break;
            }
        }
        stagePositions[stage.stageId] = ImVec2(x + nodeWidth / 2, y + nodeHeight);
    }

    // Draw connections
    for (auto& stage : quest.stages) {
        auto fromIt = stagePositions.find(stage.stageId);
        if (fromIt == stagePositions.end()) continue;

        for (auto& outcome : stage.outcomes) {
            if (outcome.nextStageId.empty()) continue;
            auto toIt = stagePositions.find(outcome.nextStageId);
            if (toIt == stagePositions.end()) continue;

            ImVec2 p1 = fromIt->second;
            ImVec2 p2(toIt->second.x, toIt->second.y - nodeHeight);

            ImU32 color = IM_COL32(150, 150, 200, 200);
            if (outcome.completeQuest) color = IM_COL32(100, 200, 100, 200);
            if (outcome.failQuest) color = IM_COL32(200, 100, 100, 200);

            drawList->AddBezierCubic(p1, ImVec2(p1.x, p1.y + 30), ImVec2(p2.x, p2.y - 30), p2, color, 2.0f);
            
            // Arrow head
            ImVec2 dir = ImVec2(p2.x - (p2.x + p1.x) / 2, p2.y - (p2.y + p1.y) / 2);
            float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
            if (len > 0) {
                dir.x /= len; dir.y /= len;
                ImVec2 arrowP1(p2.x - dir.x * 10 - dir.y * 5, p2.y - dir.y * 10 + dir.x * 5);
                ImVec2 arrowP2(p2.x - dir.x * 10 + dir.y * 5, p2.y - dir.y * 10 - dir.x * 5);
                drawList->AddTriangleFilled(p2, arrowP1, arrowP2, color);
            }
        }
    }
}

void QuestEditorPanel::AutoLayoutGraph() {
    if (m_SelectedQuest < 0 || !m_Database) return;
    
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    quest.editorGraphNodes.clear();

    float x = 100, y = 100;
    float xSpacing = 200, ySpacing = 120;

    for (size_t i = 0; i < quest.stages.size(); ++i) {
        Quest::QuestGraphNode node;
        node.nodeId = quest.stages[i].stageId;
        node.x = x + (i % 4) * xSpacing;
        node.y = y + (i / 4) * ySpacing;
        node.nodeType = "stage";
        quest.editorGraphNodes.push_back(node);
    }

    MarkModified();
}

void QuestEditorPanel::UpdateGraphNodePositions() {
    // Sync graph nodes with current stages
}

void QuestEditorPanel::HandleGraphInteraction() {
    // Future: drag nodes, create connections
}

//------------------------------------------------------------------------------
// Runtime Tab
//------------------------------------------------------------------------------
void QuestEditorPanel::DrawRuntimeTab() {
    auto& system = Quest::GetGlobalQuestSystem();
    
    ImGui::Text("Quest System Runtime State");
    ImGui::Separator();

    auto activeQuests = system.GetActiveQuestIds();
    auto completedQuests = system.GetCompletedQuestIds();
    auto failedQuests = system.GetFailedQuestIds();

    ImGui::Text("Active: %zu | Completed: %zu | Failed: %zu", 
        activeQuests.size(), completedQuests.size(), failedQuests.size());

    ImGui::Separator();

    if (ImGui::CollapsingHeader("Active Quests", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const auto& questId : activeQuests) {
            ImGui::BulletText("%s", questId.c_str());
            std::string activeStage = system.GetActiveStageId(questId);
            if (!activeStage.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("Active Stage: %s", activeStage.c_str());
                ImGui::Unindent();
            }
        }
        if (activeQuests.empty()) {
            ImGui::TextDisabled("No active quests");
        }
    }

    if (ImGui::CollapsingHeader("Completed Quests")) {
        for (const auto& questId : completedQuests) {
            ImGui::BulletText("%s", questId.c_str());
            auto branches = system.GetBranchHistory(questId);
            if (!branches.empty()) {
                ImGui::Indent();
                ImGui::TextDisabled("Branches: ");
                ImGui::SameLine();
                for (size_t i = 0; i < branches.size(); ++i) {
                    if (i > 0) ImGui::SameLine();
                    ImGui::Text("%s", branches[i].c_str());
                }
                ImGui::Unindent();
            }
        }
        if (completedQuests.empty()) {
            ImGui::TextDisabled("No completed quests");
        }
    }

    if (ImGui::CollapsingHeader("Failed Quests")) {
        for (const auto& questId : failedQuests) {
            ImGui::BulletText("%s", questId.c_str());
        }
        if (failedQuests.empty()) {
            ImGui::TextDisabled("No failed quests");
        }
    }

    ImGui::Separator();
    ImGui::Text("Debug Controls");
    
    static char testQuestId[128] = "";
    ImGui::InputText("Quest ID", testQuestId, sizeof(testQuestId));
    
    ImGui::SameLine();
    if (ImGui::Button("Start")) {
        system.StartQuest(testQuestId);
    }
    ImGui::SameLine();
    if (ImGui::Button("Complete")) {
        std::string activeStage = system.GetActiveStageId(testQuestId);
        if (!activeStage.empty()) {
            system.CompleteStage(testQuestId, activeStage);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Fail")) {
        system.FailQuest(testQuestId);
    }

    if (ImGui::Button("Reset All State")) {
        system.ResetAllState();
    }
}

//------------------------------------------------------------------------------
// Quest/Stage Operations
//------------------------------------------------------------------------------
void QuestEditorPanel::AddNewQuest() {
    if (!m_Database) return;

    Quest::QuestRecord quest;
    quest.questId = Quest::GenerateQuestId();
    quest.displayName = "New Quest";
    quest.description = "Quest description...";
    quest.category = "Side";

    // Add initial stage
    Quest::QuestStage stage;
    stage.stageId = Quest::GenerateStageId();
    stage.name = "Stage 1";
    stage.description = "Stage description...";
    quest.stages.push_back(stage);

    m_Database->GetQuests().push_back(quest);
    m_SelectedQuest = static_cast<int>(m_Database->GetQuests().size()) - 1;
    m_SelectedStage = 0;
    m_GraphNeedsLayout = true;
    MarkModified();
}

void QuestEditorPanel::DuplicateQuest(int index) {
    if (!m_Database || index < 0) return;
    auto& quests = m_Database->GetQuests();
    if (index >= static_cast<int>(quests.size())) return;

    Quest::QuestRecord copy = quests[index];
    copy.questId = Quest::GenerateQuestId();
    copy.displayName += " (Copy)";
    
    // Regenerate all IDs
    for (auto& stage : copy.stages) {
        stage.stageId = Quest::GenerateStageId();
        for (auto& obj : stage.objectives) {
            obj.objectiveId = Quest::GenerateObjectiveId();
        }
    }
    for (auto& branch : copy.branches) {
        branch.branchId = Quest::GenerateBranchId();
    }

    quests.push_back(copy);
    m_SelectedQuest = static_cast<int>(quests.size()) - 1;
    m_GraphNeedsLayout = true;
    MarkModified();
}

void QuestEditorPanel::DeleteQuest(int index) {
    if (!m_Database || index < 0) return;
    auto& quests = m_Database->GetQuests();
    if (index >= static_cast<int>(quests.size())) return;

    quests.erase(quests.begin() + index);
    if (m_SelectedQuest >= static_cast<int>(quests.size())) {
        m_SelectedQuest = static_cast<int>(quests.size()) - 1;
    }
    m_SelectedStage = -1;
    m_GraphNeedsLayout = true;
    MarkModified();
}

void QuestEditorPanel::MoveQuestUp(int index) {
    if (!m_Database || index <= 0) return;
    auto& quests = m_Database->GetQuests();
    std::swap(quests[index], quests[index - 1]);
    m_SelectedQuest = index - 1;
    MarkModified();
}

void QuestEditorPanel::MoveQuestDown(int index) {
    if (!m_Database || index < 0) return;
    auto& quests = m_Database->GetQuests();
    if (index >= static_cast<int>(quests.size()) - 1) return;
    std::swap(quests[index], quests[index + 1]);
    m_SelectedQuest = index + 1;
    MarkModified();
}

void QuestEditorPanel::AddNewStage() {
    if (!m_Database || m_SelectedQuest < 0) return;
    auto& quests = m_Database->GetQuests();
    if (m_SelectedQuest >= static_cast<int>(quests.size())) return;

    Quest::QuestStage stage;
    stage.stageId = Quest::GenerateStageId();
    stage.name = "New Stage";
    stage.description = "Stage description...";

    quests[m_SelectedQuest].stages.push_back(stage);
    m_SelectedStage = static_cast<int>(quests[m_SelectedQuest].stages.size()) - 1;
    m_GraphNeedsLayout = true;
    MarkModified();
}

void QuestEditorPanel::DuplicateStage(int index) {
    if (!m_Database || m_SelectedQuest < 0 || index < 0) return;
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    if (index >= static_cast<int>(quest.stages.size())) return;

    Quest::QuestStage copy = quest.stages[index];
    copy.stageId = Quest::GenerateStageId();
    copy.name += " (Copy)";
    for (auto& obj : copy.objectives) {
        obj.objectiveId = Quest::GenerateObjectiveId();
    }

    quest.stages.push_back(copy);
    m_SelectedStage = static_cast<int>(quest.stages.size()) - 1;
    m_GraphNeedsLayout = true;
    MarkModified();
}

void QuestEditorPanel::DeleteStage(int index) {
    if (!m_Database || m_SelectedQuest < 0 || index < 0) return;
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    if (index >= static_cast<int>(quest.stages.size())) return;

    quest.stages.erase(quest.stages.begin() + index);
    if (m_SelectedStage >= static_cast<int>(quest.stages.size())) {
        m_SelectedStage = static_cast<int>(quest.stages.size()) - 1;
    }
    m_GraphNeedsLayout = true;
    MarkModified();
}

void QuestEditorPanel::MoveStageUp(int index) {
    if (!m_Database || m_SelectedQuest < 0 || index <= 0) return;
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    std::swap(quest.stages[index], quest.stages[index - 1]);
    m_SelectedStage = index - 1;
    MarkModified();
}

void QuestEditorPanel::MoveStageDown(int index) {
    if (!m_Database || m_SelectedQuest < 0 || index < 0) return;
    auto& quest = m_Database->GetQuests()[m_SelectedQuest];
    if (index >= static_cast<int>(quest.stages.size()) - 1) return;
    std::swap(quest.stages[index], quest.stages[index + 1]);
    m_SelectedStage = index + 1;
    MarkModified();
}

//------------------------------------------------------------------------------
// UI Helpers
//------------------------------------------------------------------------------
void QuestEditorPanel::DrawResourceGuidPicker(const char* label, std::string& guid, const char* resourceType) {
    std::string typeFilter = resourceType ? resourceType : "any";
    std::vector<std::tuple<std::string, std::string, std::string>> filteredAssets; // name, path, guid
    filteredAssets.push_back(std::make_tuple("(None)", "", "")); // Allow clearing
    
    std::string currentDisplayName = guid.empty() ? "(None)" : guid;
    int currentIndex = 0;

    if (typeFilter == "dialogue") {
        const auto& dialogueAssets = ui::GetDialogueLibraryAssetOptions();
        for (const auto& option : dialogueAssets) {
            filteredAssets.push_back(std::make_tuple(option.name, option.path, option.guidString));
            if (option.guidString == guid || option.path == guid || option.name == guid) {
                currentIndex = static_cast<int>(filteredAssets.size()) - 1;
                currentDisplayName = option.name;
            }
        }
    } else {
        // Get all assets from the library
        auto allAssets = AssetLibrary::Instance().GetAllAssets();

        for (const auto& [path, assetGuid, assetType] : allAssets) {
            // Extract filename from path for display
            std::filesystem::path p(path);
            std::string name = p.stem().string();
            std::string ext = p.extension().string();
            std::string guidStr = assetGuid.ToString();

            // Filter by type using file extension or folder path
            bool matchesType = (typeFilter == "any");
            if (!matchesType) {
                if (typeFilter == "npc" && (path.find("/npcs/") != std::string::npos || path.find("\\npcs\\") != std::string::npos)) matchesType = true;
                else if (typeFilter == "item" && (path.find("/items/") != std::string::npos || path.find("\\items\\") != std::string::npos)) matchesType = true;
                else if (typeFilter == "enemy" && (path.find("/enemies/") != std::string::npos || path.find("\\enemies\\") != std::string::npos)) matchesType = true;
                else if (typeFilter == "prefab" && (ext == ".prefab" || ext == ".fbx")) matchesType = true;
                else if (typeFilter == "currency" || typeFilter == "xp") matchesType = true; // Special types, allow any
                else if (assetType == AssetType::Scriptable) matchesType = true; // Scriptable objects are always good
            }

            if (matchesType) {
                // Format: "DisplayName (folder/filename)"
                std::string displayName = name;
                std::string folder = p.parent_path().filename().string();
                if (!folder.empty() && folder != ".") {
                    displayName += " (" + folder + "/" + p.filename().string() + ")";
                }

                filteredAssets.push_back(std::make_tuple(displayName, path, guidStr));

                // Check if this matches the current guid
                if (guidStr == guid || path == guid || name == guid) {
                    currentIndex = (int)filteredAssets.size() - 1;
                    currentDisplayName = displayName;
                }
            }
        }
    }
    
    // Draw combo box
    ImGui::PushID(label);
    
    float comboWidth = ImGui::GetContentRegionAvail().x - 80.0f;
    ImGui::SetNextItemWidth(comboWidth);
    
    if (ImGui::BeginCombo("##combo", currentDisplayName.c_str())) {
        for (int i = 0; i < (int)filteredAssets.size(); ++i) {
            const auto& [name, path, assetGuid] = filteredAssets[i];
            bool isSelected = (i == currentIndex);
            
            if (ImGui::Selectable(name.c_str(), isSelected)) {
                guid = assetGuid;
                MarkModified();
            }
            
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
            
            // Tooltip showing full GUID
            if (ImGui::IsItemHovered() && !assetGuid.empty()) {
                ImGui::SetTooltip("GUID: %s\nPath: %s", assetGuid.c_str(), path.c_str());
            }
        }
        ImGui::EndCombo();
    }
    
    // Small "..." button for manual GUID entry
    ImGui::SameLine();
    if (ImGui::Button("...", ImVec2(30, 0))) {
        ImGui::OpenPopup("ManualGuidEntry");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Enter GUID manually");
    }
    
    // Manual entry popup
    if (ImGui::BeginPopup("ManualGuidEntry")) {
        ImGui::Text("Manual GUID Entry:");
        char buffer[256];
        strncpy(buffer, guid.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        ImGui::SetNextItemWidth(300);
        if (ImGui::InputText("##manual", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
            guid = buffer;
            MarkModified();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("OK")) {
            guid = buffer;
            MarkModified();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    
    ImGui::SameLine();
    ImGui::TextDisabled("%s", label);
    
    ImGui::PopID();
}

void QuestEditorPanel::DrawObjectiveTypeCombo(const char* label, Quest::ObjectiveType& type) {
    const char* types[] = { "None", "Kill", "Collect", "Talk", "ReachLocation", "Escort", "Defend", "Use", "Custom" };
    int current = static_cast<int>(type);
    if (current > 7) current = 8; // Custom is 99 in enum, but 8 in combo
    
    if (ImGui::Combo(label, &current, types, 9)) {
        if (current == 8) type = Quest::ObjectiveType::Custom;
        else type = static_cast<Quest::ObjectiveType>(current);
        MarkModified();
    }
}

void QuestEditorPanel::DrawStageStateCombo(const char* label, Quest::StageState& state) {
    const char* states[] = { "Inactive", "Active", "Completed", "Failed" };
    int current = static_cast<int>(state);
    
    if (ImGui::Combo(label, &current, states, 4)) {
        state = static_cast<Quest::StageState>(current);
        MarkModified();
    }
}

bool QuestEditorPanel::DrawInputText(const char* label, std::string& str) {
    char buffer[512];
    strncpy(buffer, str.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    if (ImGui::InputText(label, buffer, sizeof(buffer))) {
        str = buffer;
        return true;
    }
    return false;
}

bool QuestEditorPanel::DrawInputTextMultiline(const char* label, std::string& str, float height) {
    char buffer[4096];
    strncpy(buffer, str.c_str(), sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    if (ImGui::InputTextMultiline(label, buffer, sizeof(buffer), ImVec2(-1, height))) {
        str = buffer;
        return true;
    }
    return false;
}

void QuestEditorPanel::MarkModified() {
    m_Modified = true;
}

std::string QuestEditorPanel::GetWindowTitle() const {
    std::string title = "Quest Editor";
    if (m_Database) {
        if (!m_OpenPath.empty()) {
            title += " - " + std::filesystem::path(m_OpenPath).filename().string();
        } else {
            title += " - Untitled";
        }
    }
    if (m_Modified) {
        title += " *";
    }
    return title;
}
