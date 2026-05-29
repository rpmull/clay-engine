#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/ecs/Entity.h"
#include "core/ecs/Scene.h"

class EditorSceneUndoStack
{
public:
    static EditorSceneUndoStack& Get();

    void Bind(Scene* scene, EntityID* selectedEntity);
    void ResetToScene(Scene* scene = nullptr, EntityID* selectedEntity = nullptr);

    bool IsBoundTo(const Scene* scene) const { return scene && scene == m_Scene; }

    void BeginScopedAction(Scene* scene, const std::string& label);
    void EndScopedAction(Scene* scene);
    bool BeginEntityTransformAction(Scene* scene, EntityID entity, const std::string& label);
    bool EndEntityTransformAction(Scene* scene);
    void RequestDeferredCommit(Scene* scene, const std::string& label);
    void CommitSceneState(Scene* scene, const std::string& label);
    void SetAutoCaptureSuppressed(Scene* scene, bool suppressed);
    void Update();

    bool CanUndo() const { return !m_Undo.empty(); }
    bool CanRedo() const { return !m_Redo.empty(); }
    bool Undo();
    bool Redo();

    std::string GetUndoLabel() const;
    std::string GetRedoLabel() const;

private:
    struct LocalTransformState
    {
        glm::vec3 Position{ 0.0f };
        glm::vec3 Rotation{ 0.0f };
        glm::quat RotationQ{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::vec3 Scale{ 1.0f };
        bool UseQuatRotation = true;
    };

    struct Snapshot
    {
        std::vector<std::uint8_t> Bytes;
        ClaymoreGUID SelectedGuid{};
        bool SceneDirty = false;
    };

    struct TransformActionState
    {
        ClaymoreGUID EntityGuid{};
        LocalTransformState Before{};
        LocalTransformState After{};
        ClaymoreGUID SelectedGuidBefore{};
        ClaymoreGUID SelectedGuidAfter{};
        bool SceneDirtyBefore = false;
        bool SceneDirtyAfter = false;
    };

    enum class HistoryEntryKind
    {
        Snapshot,
        EntityTransform
    };

    struct HistoryEntry
    {
        HistoryEntryKind Kind = HistoryEntryKind::Snapshot;
        Snapshot SnapshotState;
        TransformActionState TransformState;
        std::string Label;
    };

    struct ActiveTransformAction
    {
        bool Active = false;
        ClaymoreGUID EntityGuid{};
        LocalTransformState Before{};
        ClaymoreGUID SelectedGuidBefore{};
        bool SceneDirtyBefore = false;
        std::string Label;
    };

    EditorSceneUndoStack() = default;

    bool EnsureCurrentSnapshot();
    bool SyncCurrentSnapshotToScene();
    Snapshot CaptureSnapshot() const;
    bool RestoreSnapshot(const Snapshot& snapshot);
    void CommitSnapshot(const std::string& label);
    void TrimHistory();
    ClaymoreGUID CaptureSelectedGuid() const;
    void RestoreSelection(const ClaymoreGUID& selectedGuid);
    bool ApplyTransformAction(const TransformActionState& action, bool useAfterState);
    LocalTransformState CaptureLocalTransformState(EntityID entity) const;
    static bool IsZeroGuid(const ClaymoreGUID& guid);
    static bool SnapshotsEqual(const Snapshot& lhs, const Snapshot& rhs);
    static bool TransformStatesEqual(const LocalTransformState& lhs, const LocalTransformState& rhs);

private:
    Scene* m_Scene = nullptr;
    EntityID* m_SelectedEntity = nullptr;

    Snapshot m_Current{};
    bool m_HasCurrent = false;
    bool m_CurrentSceneMatchesSnapshot = false;
    std::vector<HistoryEntry> m_Undo;
    std::vector<HistoryEntry> m_Redo;

    bool m_ScopedActionActive = false;
    bool m_DeferredCommitPending = false;
    bool m_AutoCaptureSuppressed = false;
    bool m_Restoring = false;
    std::string m_ScopedActionLabel;
    std::string m_DeferredCommitLabel;
    std::uint64_t m_LastObservedRevision = 0;
    std::size_t m_MaxHistoryEntries = 32;
    ActiveTransformAction m_ActiveTransformAction{};
};
