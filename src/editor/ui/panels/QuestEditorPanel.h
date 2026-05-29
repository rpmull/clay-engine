#pragma once

#include "EditorPanel.h"
#include "core/quest/QuestDatabase.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

class QuestEditorPanel : public EditorPanel {
public:
    QuestEditorPanel() = default;
    ~QuestEditorPanel() = default;

    void OnImGuiRender();
    void SetDatabase(std::shared_ptr<Quest::QuestDatabase> db);
    std::shared_ptr<Quest::QuestDatabase> GetDatabase() const { return m_Database; }
    void Open() { m_Open = true; }
    void SetOpen(bool open) { m_Open = open; }
    bool IsOpen() const { return m_Open; }
    bool IsWindowFocusedOrHovered() const { return m_WindowFocusedOrHovered; }

    // File operations
    bool NewDatabase();
    bool LoadDatabase(const std::string& path);
    bool SaveDatabase(const std::string& path = "");
    void OpenDatabaseDialog();
    bool SaveDatabaseDialog();
    bool SaveCurrent() { return SaveDatabase(); }
    bool SaveCurrentAsDialog() { return SaveDatabaseDialog(); }

private:
    // Main tabs
    void DrawMenuBar();
    void DrawToolbar();
    void DrawDesignerTab();
    void DrawGraphTab();
    void DrawRuntimeTab();

    // Designer sub-panels
    void DrawQuestList();
    void DrawQuestInspector();
    void DrawStageList();
    void DrawStageInspector();
    void DrawObjectiveEditor(Quest::Objective& objective, int index);
    void DrawConditionEditor(Quest::StageCondition& condition, int index);
    void DrawOutcomeEditor(Quest::StageOutcome& outcome, int index);
    void DrawRewardEditor(Quest::QuestReward& reward, int index);
    void DrawDialogueTriggerEditor(Quest::DialogueTrigger& trigger, int index);

    // Quest operations
    void AddNewQuest();
    void DuplicateQuest(int index);
    void DeleteQuest(int index);
    void MoveQuestUp(int index);
    void MoveQuestDown(int index);

    // Stage operations
    void AddNewStage();
    void DuplicateStage(int index);
    void DeleteStage(int index);
    void MoveStageUp(int index);
    void MoveStageDown(int index);

    // Graph editor helpers
    void DrawGraphNodes();
    void DrawGraphConnections();
    void HandleGraphInteraction();
    void AutoLayoutGraph();
    void UpdateGraphNodePositions();

    // UI Helpers
    void DrawResourceGuidPicker(const char* label, std::string& guid, const char* resourceType);
    void DrawObjectiveTypeCombo(const char* label, Quest::ObjectiveType& type);
    void DrawStageStateCombo(const char* label, Quest::StageState& state);
    bool DrawInputText(const char* label, std::string& str);
    bool DrawInputTextMultiline(const char* label, std::string& str, float height = 80.0f);

    void MarkModified();
    std::string GetWindowTitle() const;

    // State
    bool m_Open = false;
    bool m_Modified = false;
    bool m_WindowFocusedOrHovered = false;
    std::string m_OpenPath;
    std::shared_ptr<Quest::QuestDatabase> m_Database;

    // Selection state
    int m_SelectedQuest = -1;
    int m_SelectedStage = -1;
    int m_SelectedObjective = -1;

    // Graph editor state
    struct GraphNode {
        std::string id;
        float x = 0, y = 0;
        float width = 150, height = 60;
        bool selected = false;
        bool dragging = false;
    };
    std::vector<GraphNode> m_GraphNodes;
    float m_GraphScrollX = 0.0f;
    float m_GraphScrollY = 0.0f;
    float m_GraphZoom = 1.0f;
    bool m_GraphNeedsLayout = true;
    std::string m_DraggedNodeId;

    // Layout constants
    float m_QuestListWidth = 280.0f;
    float m_StageListWidth = 260.0f;
    float m_SplitterSize = 4.0f;
};

