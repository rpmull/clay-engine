#include "EditorSceneUndoStack.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/ecs/EntityData.h"
#include "core/serialization/Serializer.h"

namespace
{
using json = nlohmann::json;

constexpr float kVecEpsilon = 1.0e-5f;
constexpr float kQuatEpsilon = 1.0e-5f;

bool AreGuidsEqual(const ClaymoreGUID& lhs, const ClaymoreGUID& rhs)
{
    return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool NearlyEqualVec3(const glm::vec3& lhs, const glm::vec3& rhs, float epsilon = kVecEpsilon)
{
    const glm::vec3 delta = glm::abs(lhs - rhs);
    return delta.x <= epsilon && delta.y <= epsilon && delta.z <= epsilon;
}

bool NearlyEqualQuat(const glm::quat& lhs, const glm::quat& rhs, float epsilon = kQuatEpsilon)
{
    const glm::quat normalizedLhs = glm::normalize(lhs);
    const glm::quat normalizedRhs = glm::normalize(rhs);
    return std::abs(std::abs(glm::dot(normalizedLhs, normalizedRhs)) - 1.0f) <= epsilon;
}
}

EditorSceneUndoStack& EditorSceneUndoStack::Get()
{
    static EditorSceneUndoStack s_Instance;
    return s_Instance;
}

void EditorSceneUndoStack::Bind(Scene* scene, EntityID* selectedEntity)
{
    if (scene == m_Scene && selectedEntity == m_SelectedEntity) {
        return;
    }

    m_Scene = scene;
    m_SelectedEntity = selectedEntity;
    ResetToScene(scene, selectedEntity);
}

void EditorSceneUndoStack::ResetToScene(Scene* scene, EntityID* selectedEntity)
{
    if (scene) {
        m_Scene = scene;
    }
    if (selectedEntity) {
        m_SelectedEntity = selectedEntity;
    }

    m_Undo.clear();
    m_Redo.clear();
    m_ScopedActionActive = false;
    m_DeferredCommitPending = false;
    m_AutoCaptureSuppressed = false;
    m_ScopedActionLabel.clear();
    m_DeferredCommitLabel.clear();
    m_HasCurrent = false;
    m_CurrentSceneMatchesSnapshot = false;
    m_ActiveTransformAction = {};
    m_LastObservedRevision = (m_Scene != nullptr) ? m_Scene->GetDirtyRevision() : 0;

    if (m_Scene) {
        EnsureCurrentSnapshot();
    }
}

void EditorSceneUndoStack::BeginScopedAction(Scene* scene, const std::string& label)
{
    if (!scene || scene != m_Scene || m_Restoring || m_ActiveTransformAction.Active) {
        return;
    }

    if (!SyncCurrentSnapshotToScene()) {
        return;
    }
    if (m_ScopedActionActive) {
        return;
    }

    m_ScopedActionActive = true;
    m_ScopedActionLabel = label;
    m_DeferredCommitPending = false;
    m_DeferredCommitLabel.clear();
}

void EditorSceneUndoStack::EndScopedAction(Scene* scene)
{
    if (!scene || scene != m_Scene || !m_ScopedActionActive || m_Restoring) {
        return;
    }

    CommitSnapshot(m_ScopedActionLabel.empty() ? std::string("Scene Edit") : m_ScopedActionLabel);
    m_ScopedActionActive = false;
    m_ScopedActionLabel.clear();
}

bool EditorSceneUndoStack::BeginEntityTransformAction(Scene* scene, EntityID entity, const std::string& label)
{
    if (!scene || scene != m_Scene || m_Restoring || entity == INVALID_ENTITY_ID || m_ScopedActionActive) {
        return false;
    }

    if (!EnsureCurrentSnapshot()) {
        return false;
    }

    EntityData* data = m_Scene->GetEntityData(entity);
    if (!data) {
        return false;
    }

    if (m_ActiveTransformAction.Active) {
        return AreGuidsEqual(m_ActiveTransformAction.EntityGuid, data->EntityGuid);
    }

    m_ActiveTransformAction.Active = true;
    m_ActiveTransformAction.EntityGuid = data->EntityGuid;
    m_ActiveTransformAction.Before = CaptureLocalTransformState(entity);
    m_ActiveTransformAction.SelectedGuidBefore = CaptureSelectedGuid();
    m_ActiveTransformAction.SceneDirtyBefore = m_Scene->IsDirty();
    m_ActiveTransformAction.Label = label.empty() ? std::string("Transform Entity") : label;
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    return true;
}

bool EditorSceneUndoStack::EndEntityTransformAction(Scene* scene)
{
    if (!scene || scene != m_Scene || !m_ActiveTransformAction.Active || m_Restoring) {
        return false;
    }

    const ActiveTransformAction activeAction = m_ActiveTransformAction;
    m_ActiveTransformAction = {};

    const EntityID entity = m_Scene->FindEntityByGUID(activeAction.EntityGuid);
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    if (entity == INVALID_ENTITY_ID) {
        m_CurrentSceneMatchesSnapshot = false;
        return false;
    }

    TransformActionState actionState;
    actionState.EntityGuid = activeAction.EntityGuid;
    actionState.Before = activeAction.Before;
    actionState.After = CaptureLocalTransformState(entity);
    actionState.SelectedGuidBefore = activeAction.SelectedGuidBefore;
    actionState.SelectedGuidAfter = CaptureSelectedGuid();
    actionState.SceneDirtyBefore = activeAction.SceneDirtyBefore;
    actionState.SceneDirtyAfter = m_Scene->IsDirty();

    const bool hasMeaningfulChange =
        !TransformStatesEqual(actionState.Before, actionState.After) ||
        !AreGuidsEqual(actionState.SelectedGuidBefore, actionState.SelectedGuidAfter) ||
        actionState.SceneDirtyBefore != actionState.SceneDirtyAfter;

    if (!hasMeaningfulChange) {
        return false;
    }

    HistoryEntry entry;
    entry.Kind = HistoryEntryKind::EntityTransform;
    entry.TransformState = std::move(actionState);
    entry.Label = activeAction.Label.empty() ? std::string("Transform Entity") : activeAction.Label;
    m_Undo.push_back(std::move(entry));
    m_Redo.clear();
    m_CurrentSceneMatchesSnapshot = false;
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    TrimHistory();
    return true;
}

void EditorSceneUndoStack::RequestDeferredCommit(Scene* scene, const std::string& label)
{
    if (!scene || scene != m_Scene || m_Restoring || m_ActiveTransformAction.Active) {
        return;
    }

    if (!SyncCurrentSnapshotToScene()) {
        return;
    }
    m_DeferredCommitPending = true;
    m_DeferredCommitLabel = label;
}

void EditorSceneUndoStack::CommitSceneState(Scene* scene, const std::string& label)
{
    if (!scene || scene != m_Scene || m_Restoring || m_ActiveTransformAction.Active) {
        return;
    }

    if (!SyncCurrentSnapshotToScene()) {
        return;
    }
    CommitSnapshot(label);
}

void EditorSceneUndoStack::SetAutoCaptureSuppressed(Scene* scene, bool suppressed)
{
    if (!scene || scene != m_Scene) {
        return;
    }

    m_AutoCaptureSuppressed = suppressed;
}

void EditorSceneUndoStack::Update()
{
    if (!m_Scene || m_Restoring) {
        return;
    }

    if (!EnsureCurrentSnapshot()) {
        return;
    }

    if (m_AutoCaptureSuppressed || m_ScopedActionActive || m_ActiveTransformAction.Active) {
        return;
    }

    const std::uint64_t currentRevision = m_Scene->GetDirtyRevision();
    if (!m_CurrentSceneMatchesSnapshot) {
        m_LastObservedRevision = currentRevision;
        return;
    }

    if (m_DeferredCommitPending) {
        if (m_Scene->HasPendingEntityCreations() || m_Scene->HasPendingEntityRemovals()) {
            return;
        }

        CommitSnapshot(m_DeferredCommitLabel.empty() ? std::string("Scene Edit") : m_DeferredCommitLabel);
        m_DeferredCommitPending = false;
        m_DeferredCommitLabel.clear();
        return;
    }

    if (currentRevision != m_LastObservedRevision) {
        CommitSnapshot("Scene Edit");
    } else if (m_HasCurrent) {
        m_Current.SelectedGuid = CaptureSelectedGuid();
        m_Current.SceneDirty = m_Scene->IsDirty();
    }
}

bool EditorSceneUndoStack::Undo()
{
    if (!m_Scene || m_Restoring) {
        return false;
    }

    if (m_ScopedActionActive) {
        EndScopedAction(m_Scene);
    }
    if (m_ActiveTransformAction.Active) {
        EndEntityTransformAction(m_Scene);
    }

    if (!CanUndo()) {
        return false;
    }

    if (!EnsureCurrentSnapshot()) {
        return false;
    }

    m_DeferredCommitPending = false;
    m_DeferredCommitLabel.clear();

    HistoryEntry entry = std::move(m_Undo.back());
    m_Undo.pop_back();

    switch (entry.Kind) {
    case HistoryEntryKind::Snapshot: {
        if (!SyncCurrentSnapshotToScene()) {
            m_Undo.push_back(std::move(entry));
            return false;
        }

        HistoryEntry redoEntry;
        redoEntry.Kind = HistoryEntryKind::Snapshot;
        redoEntry.SnapshotState = m_Current;
        redoEntry.Label = entry.Label;
        if (!RestoreSnapshot(entry.SnapshotState)) {
            m_Undo.push_back(std::move(entry));
            return false;
        }

        m_Redo.push_back(std::move(redoEntry));
        m_Current = std::move(entry.SnapshotState);
        m_CurrentSceneMatchesSnapshot = true;
        break;
    }
    case HistoryEntryKind::EntityTransform:
        if (!ApplyTransformAction(entry.TransformState, false)) {
            m_Undo.push_back(std::move(entry));
            return false;
        }

        m_Redo.push_back(std::move(entry));
        m_CurrentSceneMatchesSnapshot = false;
        break;
    }
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    TrimHistory();
    return true;
}

bool EditorSceneUndoStack::Redo()
{
    if (!m_Scene || m_Restoring) {
        return false;
    }

    if (m_ScopedActionActive) {
        EndScopedAction(m_Scene);
    }
    if (m_ActiveTransformAction.Active) {
        EndEntityTransformAction(m_Scene);
    }

    if (!CanRedo()) {
        return false;
    }

    if (!EnsureCurrentSnapshot()) {
        return false;
    }

    m_DeferredCommitPending = false;
    m_DeferredCommitLabel.clear();

    HistoryEntry entry = std::move(m_Redo.back());
    m_Redo.pop_back();

    switch (entry.Kind) {
    case HistoryEntryKind::Snapshot: {
        if (!SyncCurrentSnapshotToScene()) {
            m_Redo.push_back(std::move(entry));
            return false;
        }

        HistoryEntry undoEntry;
        undoEntry.Kind = HistoryEntryKind::Snapshot;
        undoEntry.SnapshotState = m_Current;
        undoEntry.Label = entry.Label;
        if (!RestoreSnapshot(entry.SnapshotState)) {
            m_Redo.push_back(std::move(entry));
            return false;
        }

        m_Undo.push_back(std::move(undoEntry));
        m_Current = std::move(entry.SnapshotState);
        m_CurrentSceneMatchesSnapshot = true;
        break;
    }
    case HistoryEntryKind::EntityTransform:
        if (!ApplyTransformAction(entry.TransformState, true)) {
            m_Redo.push_back(std::move(entry));
            return false;
        }

        m_Undo.push_back(std::move(entry));
        m_CurrentSceneMatchesSnapshot = false;
        break;
    }
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    TrimHistory();
    return true;
}

std::string EditorSceneUndoStack::GetUndoLabel() const
{
    return CanUndo() ? m_Undo.back().Label : std::string();
}

std::string EditorSceneUndoStack::GetRedoLabel() const
{
    return CanRedo() ? m_Redo.back().Label : std::string();
}

bool EditorSceneUndoStack::EnsureCurrentSnapshot()
{
    if (!m_Scene) {
        return false;
    }

    if (!m_HasCurrent) {
        m_Current = CaptureSnapshot();
        if (m_Current.Bytes.empty()) {
            m_Current = {};
            return false;
        }
        m_HasCurrent = true;
        m_CurrentSceneMatchesSnapshot = true;
        m_LastObservedRevision = m_Scene->GetDirtyRevision();
    }
    return true;
}

bool EditorSceneUndoStack::SyncCurrentSnapshotToScene()
{
    if (!EnsureCurrentSnapshot()) {
        return false;
    }

    if (m_CurrentSceneMatchesSnapshot) {
        return true;
    }

    Snapshot snapshot = CaptureSnapshot();
    if (snapshot.Bytes.empty()) {
        return false;
    }

    m_Current = std::move(snapshot);
    m_CurrentSceneMatchesSnapshot = true;
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    return true;
}

EditorSceneUndoStack::Snapshot EditorSceneUndoStack::CaptureSnapshot() const
{
    Snapshot snapshot;
    if (!m_Scene) {
        return snapshot;
    }

    try {
        json sceneJson = Serializer::SerializeScene(*m_Scene);
        snapshot.Bytes = json::to_msgpack(sceneJson);
        snapshot.SelectedGuid = CaptureSelectedGuid();
        snapshot.SceneDirty = m_Scene->IsDirty();
    } catch (const std::exception& e) {
        std::cerr << "[EditorUndo] Failed to capture snapshot: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[EditorUndo] Failed to capture snapshot." << std::endl;
    }

    return snapshot;
}

bool EditorSceneUndoStack::RestoreSnapshot(const Snapshot& snapshot)
{
    if (!m_Scene) {
        return false;
    }

    try {
        m_Restoring = true;
        json sceneJson = json::from_msgpack(snapshot.Bytes);
        if (!Serializer::DeserializeScene(sceneJson, *m_Scene)) {
            m_Restoring = false;
            return false;
        }

        if (!snapshot.SceneDirty) {
            m_Scene->ClearDirty();
        } else if (!m_Scene->IsDirty()) {
            m_Scene->MarkDirty();
        }

        RestoreSelection(snapshot.SelectedGuid);
        m_LastObservedRevision = m_Scene->GetDirtyRevision();
        m_CurrentSceneMatchesSnapshot = true;
        m_Restoring = false;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[EditorUndo] Failed to restore snapshot: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[EditorUndo] Failed to restore snapshot." << std::endl;
    }

    m_Restoring = false;
    return false;
}

void EditorSceneUndoStack::CommitSnapshot(const std::string& label)
{
    if (!m_Scene || m_Restoring) {
        return;
    }

    Snapshot next = CaptureSnapshot();
    if (next.Bytes.empty()) {
        return;
    }
    if (SnapshotsEqual(m_Current, next)) {
        m_Current.SelectedGuid = next.SelectedGuid;
        m_Current.SceneDirty = next.SceneDirty;
        m_CurrentSceneMatchesSnapshot = true;
        m_LastObservedRevision = m_Scene->GetDirtyRevision();
        return;
    }

    HistoryEntry undoEntry;
    undoEntry.Kind = HistoryEntryKind::Snapshot;
    undoEntry.SnapshotState = std::move(m_Current);
    undoEntry.Label = label.empty() ? std::string("Scene Edit") : label;
    m_Undo.push_back(std::move(undoEntry));
    m_Current = std::move(next);
    m_CurrentSceneMatchesSnapshot = true;
    m_Redo.clear();
    m_LastObservedRevision = m_Scene->GetDirtyRevision();
    TrimHistory();
}

void EditorSceneUndoStack::TrimHistory()
{
    if (m_Undo.size() > m_MaxHistoryEntries) {
        m_Undo.erase(m_Undo.begin(), m_Undo.begin() + static_cast<std::ptrdiff_t>(m_Undo.size() - m_MaxHistoryEntries));
    }
    if (m_Redo.size() > m_MaxHistoryEntries) {
        m_Redo.erase(m_Redo.begin(), m_Redo.begin() + static_cast<std::ptrdiff_t>(m_Redo.size() - m_MaxHistoryEntries));
    }
}

ClaymoreGUID EditorSceneUndoStack::CaptureSelectedGuid() const
{
    if (!m_Scene || !m_SelectedEntity || *m_SelectedEntity == INVALID_ENTITY_ID) {
        return {};
    }

    if (EntityData* data = m_Scene->GetEntityData(*m_SelectedEntity)) {
        return data->EntityGuid;
    }
    return {};
}

void EditorSceneUndoStack::RestoreSelection(const ClaymoreGUID& selectedGuid)
{
    if (!m_SelectedEntity) {
        return;
    }

    *m_SelectedEntity = INVALID_ENTITY_ID;
    if (IsZeroGuid(selectedGuid) || !m_Scene) {
        return;
    }

    *m_SelectedEntity = m_Scene->FindEntityByGUID(selectedGuid);
}

bool EditorSceneUndoStack::ApplyTransformAction(const TransformActionState& action, bool useAfterState)
{
    if (!m_Scene) {
        return false;
    }

    const EntityID entity = m_Scene->FindEntityByGUID(action.EntityGuid);
    if (entity == INVALID_ENTITY_ID) {
        return false;
    }

    EntityData* data = m_Scene->GetEntityData(entity);
    if (!data) {
        return false;
    }

    const LocalTransformState& transformState = useAfterState ? action.After : action.Before;
    const ClaymoreGUID& selectedGuid = useAfterState ? action.SelectedGuidAfter : action.SelectedGuidBefore;
    const bool sceneDirty = useAfterState ? action.SceneDirtyAfter : action.SceneDirtyBefore;

    data->Transform.Position = transformState.Position;
    data->Transform.Rotation = transformState.Rotation;
    data->Transform.RotationQ = transformState.RotationQ;
    data->Transform.Scale = transformState.Scale;
    data->Transform.UseQuatRotation = transformState.UseQuatRotation;
    m_Scene->MarkTransformDirty(entity);

    if (sceneDirty) {
        m_Scene->MarkDirty();
    } else {
        m_Scene->ClearDirty();
    }

    RestoreSelection(selectedGuid);
    return true;
}

EditorSceneUndoStack::LocalTransformState EditorSceneUndoStack::CaptureLocalTransformState(EntityID entity) const
{
    LocalTransformState state;
    if (!m_Scene) {
        return state;
    }

    if (EntityData* data = m_Scene->GetEntityData(entity)) {
        state.Position = data->Transform.Position;
        state.Rotation = data->Transform.Rotation;
        state.RotationQ = data->Transform.RotationQ;
        state.Scale = data->Transform.Scale;
        state.UseQuatRotation = data->Transform.UseQuatRotation;
    }
    return state;
}

bool EditorSceneUndoStack::IsZeroGuid(const ClaymoreGUID& guid)
{
    return guid.high == 0 && guid.low == 0;
}

bool EditorSceneUndoStack::SnapshotsEqual(const Snapshot& lhs, const Snapshot& rhs)
{
    return lhs.Bytes == rhs.Bytes;
}

bool EditorSceneUndoStack::TransformStatesEqual(const LocalTransformState& lhs, const LocalTransformState& rhs)
{
    return lhs.UseQuatRotation == rhs.UseQuatRotation &&
           NearlyEqualVec3(lhs.Position, rhs.Position) &&
           NearlyEqualVec3(lhs.Rotation, rhs.Rotation) &&
           NearlyEqualQuat(lhs.RotationQ, rhs.RotationQ) &&
           NearlyEqualVec3(lhs.Scale, rhs.Scale);
}
