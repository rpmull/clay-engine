using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>Quest state enum (mirrors native QuestState)</summary>
    public enum QuestState
    {
        Inactive = 0,
        Active = 1,
        Completed = 2,
        Failed = 3
    }

    /// <summary>Quest stage state enum (mirrors native StageState)</summary>
    public enum QuestStageState
    {
        Inactive = 0,
        Active = 1,
        Completed = 2,
        Failed = 3
    }

    /// <summary>
    /// Reference to a quest (serializable ID reference)
    /// </summary>
    [Serializable]
    public struct QuestRef
    {
        public string questId;

        public bool IsValid => !string.IsNullOrEmpty(questId);

        public QuestRef(string id)
        {
            questId = id;
        }

        public QuestState State => QuestSystem.GetQuestState(questId);
        public bool IsActive => State == QuestState.Active;
        public bool IsCompleted => State == QuestState.Completed;
        public bool IsFailed => State == QuestState.Failed;
    }

    /// <summary>
    /// Main interface to the native quest system.
    /// Provides static methods for quest management and events for game integration.
    /// </summary>
    public static class QuestSystem
    {
        #region Events

        /// <summary>Fired when a quest starts</summary>
        public static event Action<string>? OnQuestStarted;
        
        /// <summary>Fired when a stage starts</summary>
        public static event Action<string, string>? OnStageStarted;
        
        /// <summary>Fired when a stage completes</summary>
        public static event Action<string, string>? OnStageCompleted;
        
        /// <summary>Fired when a quest completes</summary>
        public static event Action<string>? OnQuestCompleted;
        
        /// <summary>Fired when a quest fails</summary>
        public static event Action<string>? OnQuestFailed;
        
        /// <summary>Fired when objective progress changes (questId, stageId, objectiveId, current, target)</summary>
        public static event Action<string, string, string, int, int>? OnObjectiveProgress;

        #endregion

        #region Callback Instances (prevent GC collection)

        private static QuestInterop.QuestStartedCallback? _startedCb;
        private static QuestInterop.QuestStageStartedCallback? _stageStartedCb;
        private static QuestInterop.QuestStageCompletedCallback? _stageCompletedCb;
        private static QuestInterop.QuestCompletedCallback? _questCompletedCb;
        private static QuestInterop.QuestFailedCallback? _questFailedCb;
        private static QuestInterop.QuestObjectiveProgressCallback? _objectiveProgressCb;

        #endregion

        #region Initialization

        private static bool _initialized;

        /// <summary>Initialize callbacks. Called automatically on first use.</summary>
        public static void Initialize()
        {
            if (_initialized) return;
            if (!QuestInterop.IsInitialized)
            {
                Console.WriteLine("[QuestSystem] QuestInterop not initialized yet.");
                return;
            }

            _startedCb = (ptr) => OnQuestStarted?.Invoke(Marshal.PtrToStringAnsi(ptr) ?? string.Empty);
            _stageStartedCb = (q, s) => OnStageStarted?.Invoke(
                Marshal.PtrToStringAnsi(q) ?? string.Empty,
                Marshal.PtrToStringAnsi(s) ?? string.Empty);
            _stageCompletedCb = (q, s) => OnStageCompleted?.Invoke(
                Marshal.PtrToStringAnsi(q) ?? string.Empty,
                Marshal.PtrToStringAnsi(s) ?? string.Empty);
            _questCompletedCb = (ptr) => OnQuestCompleted?.Invoke(Marshal.PtrToStringAnsi(ptr) ?? string.Empty);
            _questFailedCb = (ptr) => OnQuestFailed?.Invoke(Marshal.PtrToStringAnsi(ptr) ?? string.Empty);
            _objectiveProgressCb = (q, s, o, current, target) => OnObjectiveProgress?.Invoke(
                Marshal.PtrToStringAnsi(q) ?? string.Empty,
                Marshal.PtrToStringAnsi(s) ?? string.Empty,
                Marshal.PtrToStringAnsi(o) ?? string.Empty,
                current, target);

            QuestInterop.SetOnQuestStarted?.Invoke(_startedCb);
            QuestInterop.SetOnStageStarted?.Invoke(_stageStartedCb);
            QuestInterop.SetOnStageCompleted?.Invoke(_stageCompletedCb);
            QuestInterop.SetOnQuestCompleted?.Invoke(_questCompletedCb);
            QuestInterop.SetOnQuestFailed?.Invoke(_questFailedCb);
            QuestInterop.SetOnObjectiveProgress?.Invoke(_objectiveProgressCb);

            _initialized = true;
            Console.WriteLine("[QuestSystem] Initialized - callbacks registered");
        }

        internal static void ResetRuntimeState()
        {
            OnQuestStarted = null;
            OnStageStarted = null;
            OnStageCompleted = null;
            OnQuestCompleted = null;
            OnQuestFailed = null;
            OnObjectiveProgress = null;
        }

        #endregion

        #region Database Management

        /// <summary>Load a quest database from file. Returns database GUID.</summary>
        public static string LoadDatabase(string path)
        {
            if (!QuestInterop.IsInitialized) return string.Empty;
            var guidPtr = QuestInterop.LoadDatabase?.Invoke(path) ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(guidPtr) ?? string.Empty;
        }

        /// <summary>Unload a quest database by GUID.</summary>
        public static void UnloadDatabase(string guid)
        {
            QuestInterop.UnloadDatabase?.Invoke(guid);
        }

        #endregion

        #region Core Operations

        /// <summary>Start a quest by ID.</summary>
        public static bool StartQuest(string questId)
        {
            Initialize();
            return QuestInterop.StartQuest?.Invoke(questId) ?? false;
        }

        /// <summary>Advance to a specific stage.</summary>
        public static bool AdvanceStage(string questId, string stageId)
        {
            Initialize();
            return QuestInterop.AdvanceStage?.Invoke(questId, stageId) ?? false;
        }

        /// <summary>Complete the specified stage.</summary>
        public static bool CompleteStage(string questId, string stageId)
        {
            Initialize();
            return QuestInterop.CompleteStage?.Invoke(questId, stageId) ?? false;
        }

        /// <summary>Fail a quest.</summary>
        public static bool FailQuest(string questId)
        {
            Initialize();
            return QuestInterop.FailQuest?.Invoke(questId) ?? false;
        }

        /// <summary>Abandon a quest (reset to inactive).</summary>
        public static bool AbandonQuest(string questId)
        {
            Initialize();
            return QuestInterop.AbandonQuest?.Invoke(questId) ?? false;
        }

        /// <summary>Update objective progress by delta amount.</summary>
        public static bool UpdateObjective(string questId, string stageId, string objectiveId, int delta)
        {
            Initialize();
            return QuestInterop.UpdateObjective?.Invoke(questId, stageId, objectiveId, delta) ?? false;
        }

        /// <summary>Set objective progress to absolute value.</summary>
        public static bool SetObjectiveProgress(string questId, string stageId, string objectiveId, int value)
        {
            Initialize();
            return QuestInterop.SetObjectiveProgress?.Invoke(questId, stageId, objectiveId, value) ?? false;
        }

        #endregion

        #region State Queries

        /// <summary>Get the state of a quest.</summary>
        public static QuestState GetQuestState(string questId)
        {
            var val = QuestInterop.GetQuestState?.Invoke(questId) ?? 0;
            return (QuestState)val;
        }

        /// <summary>Get the state of a stage.</summary>
        public static QuestStageState GetStageState(string questId, string stageId)
        {
            var val = QuestInterop.GetStageState?.Invoke(questId, stageId) ?? 0;
            return (QuestStageState)val;
        }

        /// <summary>Get the currently active stage ID for a quest.</summary>
        public static string GetActiveStageId(string questId)
        {
            var ptr = QuestInterop.GetActiveStageId?.Invoke(questId) ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
        }

        /// <summary>Get current progress for an objective.</summary>
        public static int GetObjectiveProgress(string questId, string stageId, string objectiveId)
        {
            return QuestInterop.GetObjectiveProgress?.Invoke(questId, stageId, objectiveId) ?? 0;
        }

        /// <summary>Get target count for an objective.</summary>
        public static int GetObjectiveTarget(string questId, string stageId, string objectiveId)
        {
            return QuestInterop.GetObjectiveTarget?.Invoke(questId, stageId, objectiveId) ?? 1;
        }

        /// <summary>Check if an objective is complete.</summary>
        public static bool IsObjectiveComplete(string questId, string stageId, string objectiveId)
        {
            return QuestInterop.IsObjectiveComplete?.Invoke(questId, stageId, objectiveId) ?? false;
        }

        /// <summary>Check if a quest is active.</summary>
        public static bool IsQuestActive(string questId) => GetQuestState(questId) == QuestState.Active;

        /// <summary>Check if a quest is completed.</summary>
        public static bool IsQuestCompleted(string questId) => GetQuestState(questId) == QuestState.Completed;

        /// <summary>Check if a quest has failed.</summary>
        public static bool IsQuestFailed(string questId) => GetQuestState(questId) == QuestState.Failed;

        /// <summary>Check if a stage is active.</summary>
        public static bool IsStageActive(string questId, string stageId) => GetStageState(questId, stageId) == QuestStageState.Active;

        #endregion

        #region Bulk Queries

        /// <summary>Get count of active quests.</summary>
        public static int GetActiveQuestCount()
        {
            return QuestInterop.GetActiveQuestCount?.Invoke() ?? 0;
        }

        /// <summary>Get list of active quest IDs.</summary>
        public static List<string> GetActiveQuestIds()
        {
            var ptr = QuestInterop.GetActiveQuestIds?.Invoke() ?? IntPtr.Zero;
            var str = Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
            return ParseCommaSeparated(str);
        }

        /// <summary>Get count of completed quests.</summary>
        public static int GetCompletedQuestCount()
        {
            return QuestInterop.GetCompletedQuestCount?.Invoke() ?? 0;
        }

        /// <summary>Get list of completed quest IDs.</summary>
        public static List<string> GetCompletedQuestIds()
        {
            var ptr = QuestInterop.GetCompletedQuestIds?.Invoke() ?? IntPtr.Zero;
            var str = Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
            return ParseCommaSeparated(str);
        }

        /// <summary>Get branch history for a quest.</summary>
        public static List<string> GetBranchHistory(string questId)
        {
            var ptr = QuestInterop.GetBranchHistory?.Invoke(questId) ?? IntPtr.Zero;
            var str = Marshal.PtrToStringAnsi(ptr) ?? string.Empty;
            return ParseCommaSeparated(str);
        }

        private static List<string> ParseCommaSeparated(string str)
        {
            var result = new List<string>();
            if (string.IsNullOrEmpty(str)) return result;
            foreach (var part in str.Split(','))
            {
                var trimmed = part.Trim();
                if (!string.IsNullOrEmpty(trimmed))
                    result.Add(trimmed);
            }
            return result;
        }

        #endregion

        #region Condition Checking

        /// <summary>Check if a quest can be started (prerequisites met, not already active/completed).</summary>
        public static bool CanStartQuest(string questId)
        {
            return QuestInterop.CanStartQuest?.Invoke(questId) ?? false;
        }

        /// <summary>Check if all conditions for a stage are met.</summary>
        public static bool AreStageConditionsMet(string questId, string stageId)
        {
            return QuestInterop.AreStageConditionsMet?.Invoke(questId, stageId) ?? false;
        }

        /// <summary>Check if a dialogue trigger is active for quest integration.</summary>
        public static bool IsDialogueTriggerActive(string questId, string dialogueLibraryGuid, string entryId = "")
        {
            return QuestInterop.IsDialogueTriggerActive?.Invoke(questId, dialogueLibraryGuid, entryId) ?? false;
        }

        #endregion

        #region Save/Load

        /// <summary>Serialize quest runtime state to JSON string.</summary>
        public static string SerializeState()
        {
            var ptr = QuestInterop.SerializeState?.Invoke() ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(ptr) ?? "{}";
        }

        /// <summary>Deserialize quest runtime state from JSON string.</summary>
        public static bool DeserializeState(string json)
        {
            return QuestInterop.DeserializeState?.Invoke(json) ?? false;
        }

        /// <summary>Reset all quest runtime state.</summary>
        public static void ResetAllState()
        {
            QuestInterop.ResetAllState?.Invoke();
        }

        #endregion

        #region Helper Methods for Common Patterns

        /// <summary>
        /// Try to start a quest if it can be started.
        /// Returns true if quest was started, false if prerequisites not met.
        /// </summary>
        public static bool TryStartQuest(string questId)
        {
            if (CanStartQuest(questId))
            {
                return StartQuest(questId);
            }
            return false;
        }

        /// <summary>
        /// Complete the currently active stage of a quest.
        /// </summary>
        public static bool CompleteCurrentStage(string questId)
        {
            var stageId = GetActiveStageId(questId);
            if (string.IsNullOrEmpty(stageId)) return false;
            return CompleteStage(questId, stageId);
        }

        /// <summary>
        /// Increment objective progress by 1 for the current active stage.
        /// Useful for kill/collect tracking.
        /// </summary>
        public static bool IncrementObjective(string questId, string objectiveId)
        {
            var stageId = GetActiveStageId(questId);
            if (string.IsNullOrEmpty(stageId)) return false;
            return UpdateObjective(questId, stageId, objectiveId, 1);
        }

        /// <summary>
        /// Check if the player is on a specific stage of a quest.
        /// Useful for dialogue conditions like "Have you gotten my sword yet?"
        /// </summary>
        public static bool IsOnStage(string questId, string stageId)
        {
            return IsQuestActive(questId) && GetActiveStageId(questId) == stageId;
        }

        /// <summary>
        /// Check if a quest was completed with a specific branch.
        /// Useful for checking past choices.
        /// </summary>
        public static bool CompletedWithBranch(string questId, string branchId)
        {
            if (!IsQuestCompleted(questId)) return false;
            return GetBranchHistory(questId).Contains(branchId);
        }

        #endregion
    }
}
