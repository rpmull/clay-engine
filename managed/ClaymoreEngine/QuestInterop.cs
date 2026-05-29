using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void QuestInteropInitDelegate(IntPtr* functionPointers, int count);

    public static unsafe class QuestInterop
    {
        // Database management
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate IntPtr LoadDatabaseFn([MarshalAs(UnmanagedType.LPStr)] string path);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void UnloadDatabaseFn([MarshalAs(UnmanagedType.LPStr)] string guidStr);
        
        // Core operations
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool StartQuestFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool AdvanceStageFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool CompleteStageFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool FailQuestFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool AbandonQuestFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool UpdateObjectiveFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId, [MarshalAs(UnmanagedType.LPStr)] string objectiveId, int delta);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool SetObjectiveProgressFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId, [MarshalAs(UnmanagedType.LPStr)] string objectiveId, int value);
        
        // State queries
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetQuestStateFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetStageStateFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate IntPtr GetActiveStageIdFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetObjectiveProgressFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId, [MarshalAs(UnmanagedType.LPStr)] string objectiveId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetObjectiveTargetFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId, [MarshalAs(UnmanagedType.LPStr)] string objectiveId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool IsObjectiveCompleteFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId, [MarshalAs(UnmanagedType.LPStr)] string objectiveId);
        
        // Bulk queries
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetActiveQuestCountFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate IntPtr GetActiveQuestIdsFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetCompletedQuestCountFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate IntPtr GetCompletedQuestIdsFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate int GetBranchHistoryCountFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate IntPtr GetBranchHistoryFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        // Conditions
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool CanStartQuestFn([MarshalAs(UnmanagedType.LPStr)] string questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool AreStageConditionsMetFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string stageId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool IsDialogueTriggerActiveFn([MarshalAs(UnmanagedType.LPStr)] string questId, [MarshalAs(UnmanagedType.LPStr)] string dialogueLibraryGuid, [MarshalAs(UnmanagedType.LPStr)] string entryId);
        
        // Save/Load
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate IntPtr SerializeStateFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)] 
        public delegate bool DeserializeStateFn([MarshalAs(UnmanagedType.LPStr)] string jsonStr);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void ResetAllStateFn();

        // Callbacks
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void QuestStartedCallback(IntPtr questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void QuestStageStartedCallback(IntPtr questId, IntPtr stageId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void QuestStageCompletedCallback(IntPtr questId, IntPtr stageId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void QuestCompletedCallback(IntPtr questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void QuestFailedCallback(IntPtr questId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void QuestObjectiveProgressCallback(IntPtr questId, IntPtr stageId, IntPtr objectiveId, int current, int target);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void SetOnQuestStartedFn(QuestStartedCallback cb);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void SetOnStageStartedFn(QuestStageStartedCallback cb);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void SetOnStageCompletedFn(QuestStageCompletedCallback cb);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void SetOnQuestCompletedFn(QuestCompletedCallback cb);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void SetOnQuestFailedFn(QuestFailedCallback cb);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] 
        public delegate void SetOnObjectiveProgressFn(QuestObjectiveProgressCallback cb);

        // Function pointers
        public static LoadDatabaseFn? LoadDatabase;
        public static UnloadDatabaseFn? UnloadDatabase;
        public static StartQuestFn? StartQuest;
        public static AdvanceStageFn? AdvanceStage;
        public static CompleteStageFn? CompleteStage;
        public static FailQuestFn? FailQuest;
        public static AbandonQuestFn? AbandonQuest;
        public static UpdateObjectiveFn? UpdateObjective;
        public static SetObjectiveProgressFn? SetObjectiveProgress;
        public static GetQuestStateFn? GetQuestState;
        public static GetStageStateFn? GetStageState;
        public static GetActiveStageIdFn? GetActiveStageId;
        public static GetObjectiveProgressFn? GetObjectiveProgress;
        public static GetObjectiveTargetFn? GetObjectiveTarget;
        public static IsObjectiveCompleteFn? IsObjectiveComplete;
        public static GetActiveQuestCountFn? GetActiveQuestCount;
        public static GetActiveQuestIdsFn? GetActiveQuestIds;
        public static GetCompletedQuestCountFn? GetCompletedQuestCount;
        public static GetCompletedQuestIdsFn? GetCompletedQuestIds;
        public static GetBranchHistoryCountFn? GetBranchHistoryCount;
        public static GetBranchHistoryFn? GetBranchHistory;
        public static CanStartQuestFn? CanStartQuest;
        public static AreStageConditionsMetFn? AreStageConditionsMet;
        public static IsDialogueTriggerActiveFn? IsDialogueTriggerActive;
        public static SerializeStateFn? SerializeState;
        public static DeserializeStateFn? DeserializeState;
        public static ResetAllStateFn? ResetAllState;

        public static SetOnQuestStartedFn? SetOnQuestStarted;
        public static SetOnStageStartedFn? SetOnStageStarted;
        public static SetOnStageCompletedFn? SetOnStageCompleted;
        public static SetOnQuestCompletedFn? SetOnQuestCompleted;
        public static SetOnQuestFailedFn? SetOnQuestFailed;
        public static SetOnObjectiveProgressFn? SetOnObjectiveProgress;

        public static bool IsInitialized { get; private set; }

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count <= 0) return;
            int i = 0;
            
            // Core functions (order must match native side)
            if (count > i) LoadDatabase = Marshal.GetDelegateForFunctionPointer<LoadDatabaseFn>(ptrs[i++]);
            if (count > i) UnloadDatabase = Marshal.GetDelegateForFunctionPointer<UnloadDatabaseFn>(ptrs[i++]);
            if (count > i) StartQuest = Marshal.GetDelegateForFunctionPointer<StartQuestFn>(ptrs[i++]);
            if (count > i) AdvanceStage = Marshal.GetDelegateForFunctionPointer<AdvanceStageFn>(ptrs[i++]);
            if (count > i) CompleteStage = Marshal.GetDelegateForFunctionPointer<CompleteStageFn>(ptrs[i++]);
            if (count > i) FailQuest = Marshal.GetDelegateForFunctionPointer<FailQuestFn>(ptrs[i++]);
            if (count > i) AbandonQuest = Marshal.GetDelegateForFunctionPointer<AbandonQuestFn>(ptrs[i++]);
            if (count > i) UpdateObjective = Marshal.GetDelegateForFunctionPointer<UpdateObjectiveFn>(ptrs[i++]);
            if (count > i) SetObjectiveProgress = Marshal.GetDelegateForFunctionPointer<SetObjectiveProgressFn>(ptrs[i++]);
            
            // State queries
            if (count > i) GetQuestState = Marshal.GetDelegateForFunctionPointer<GetQuestStateFn>(ptrs[i++]);
            if (count > i) GetStageState = Marshal.GetDelegateForFunctionPointer<GetStageStateFn>(ptrs[i++]);
            if (count > i) GetActiveStageId = Marshal.GetDelegateForFunctionPointer<GetActiveStageIdFn>(ptrs[i++]);
            if (count > i) GetObjectiveProgress = Marshal.GetDelegateForFunctionPointer<GetObjectiveProgressFn>(ptrs[i++]);
            if (count > i) GetObjectiveTarget = Marshal.GetDelegateForFunctionPointer<GetObjectiveTargetFn>(ptrs[i++]);
            if (count > i) IsObjectiveComplete = Marshal.GetDelegateForFunctionPointer<IsObjectiveCompleteFn>(ptrs[i++]);
            
            // Bulk queries
            if (count > i) GetActiveQuestCount = Marshal.GetDelegateForFunctionPointer<GetActiveQuestCountFn>(ptrs[i++]);
            if (count > i) GetActiveQuestIds = Marshal.GetDelegateForFunctionPointer<GetActiveQuestIdsFn>(ptrs[i++]);
            if (count > i) GetCompletedQuestCount = Marshal.GetDelegateForFunctionPointer<GetCompletedQuestCountFn>(ptrs[i++]);
            if (count > i) GetCompletedQuestIds = Marshal.GetDelegateForFunctionPointer<GetCompletedQuestIdsFn>(ptrs[i++]);
            if (count > i) GetBranchHistoryCount = Marshal.GetDelegateForFunctionPointer<GetBranchHistoryCountFn>(ptrs[i++]);
            if (count > i) GetBranchHistory = Marshal.GetDelegateForFunctionPointer<GetBranchHistoryFn>(ptrs[i++]);
            
            // Conditions
            if (count > i) CanStartQuest = Marshal.GetDelegateForFunctionPointer<CanStartQuestFn>(ptrs[i++]);
            if (count > i) AreStageConditionsMet = Marshal.GetDelegateForFunctionPointer<AreStageConditionsMetFn>(ptrs[i++]);
            if (count > i) IsDialogueTriggerActive = Marshal.GetDelegateForFunctionPointer<IsDialogueTriggerActiveFn>(ptrs[i++]);
            
            // Save/Load
            if (count > i) SerializeState = Marshal.GetDelegateForFunctionPointer<SerializeStateFn>(ptrs[i++]);
            if (count > i) DeserializeState = Marshal.GetDelegateForFunctionPointer<DeserializeStateFn>(ptrs[i++]);
            if (count > i) ResetAllState = Marshal.GetDelegateForFunctionPointer<ResetAllStateFn>(ptrs[i++]);
            
            // Callbacks
            if (count > i) SetOnQuestStarted = Marshal.GetDelegateForFunctionPointer<SetOnQuestStartedFn>(ptrs[i++]);
            if (count > i) SetOnStageStarted = Marshal.GetDelegateForFunctionPointer<SetOnStageStartedFn>(ptrs[i++]);
            if (count > i) SetOnStageCompleted = Marshal.GetDelegateForFunctionPointer<SetOnStageCompletedFn>(ptrs[i++]);
            if (count > i) SetOnQuestCompleted = Marshal.GetDelegateForFunctionPointer<SetOnQuestCompletedFn>(ptrs[i++]);
            if (count > i) SetOnQuestFailed = Marshal.GetDelegateForFunctionPointer<SetOnQuestFailedFn>(ptrs[i++]);
            if (count > i) SetOnObjectiveProgress = Marshal.GetDelegateForFunctionPointer<SetOnObjectiveProgressFn>(ptrs[i++]);
            
            IsInitialized = true;
        }
    }
}
