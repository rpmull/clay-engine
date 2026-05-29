using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    public unsafe delegate void DialogueInteropInitDelegate(IntPtr* functionPointers, int count);

    /// <summary>
    /// Managed-side bindings for native dialogue system functions.
    /// </summary>
    public static unsafe class DialogueInterop
    {
        // Delegate types for native functions
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr LoadLibraryFn([MarshalAs(UnmanagedType.LPStr)] string filePath);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void UnloadLibraryFn([MarshalAs(UnmanagedType.LPStr)] string guidStr);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetLibraryNameFn([MarshalAs(UnmanagedType.LPStr)] string guidStr);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetLibraryCharacterIdFn([MarshalAs(UnmanagedType.LPStr)] string guidStr);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public delegate bool StartFromLibraryFn([MarshalAs(UnmanagedType.LPStr)] string libraryGuidStr, int speakerEntityId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public delegate bool StartFromTextFn([MarshalAs(UnmanagedType.LPStr)] string dialogueText, int speakerEntityId);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void VoidFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SelectChoiceFn(int choiceIndex);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        [return: MarshalAs(UnmanagedType.I1)]
        public delegate bool BoolFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate int IntFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr StringFn();
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetChoiceTextFn(int index);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetStateFn([MarshalAs(UnmanagedType.LPStr)] string key, [MarshalAs(UnmanagedType.LPStr)] string value);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate IntPtr GetStateValueFn([MarshalAs(UnmanagedType.LPStr)] string key);
        
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetFloatFn(float value);
        
        // Callback registration delegates
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueStartedCallback(IntPtr title);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueEndedCallback();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueLineCallback(IntPtr speaker, IntPtr text, int choiceCount);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueEmoteCallback(IntPtr emoteName);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueStateChangeCallback(IntPtr key, IntPtr value);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueEmotionCallback(IntPtr emotionName);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueTypingCompleteCallback();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueCharTypedCallback(byte c);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueGiveItemCallback(IntPtr itemId, int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueTakeItemCallback(IntPtr itemId, int count);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueStartQuestCallback(IntPtr questId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueCompleteStepCallback(IntPtr questId, IntPtr stepId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialoguePlayAnimCallback(IntPtr animName);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialoguePlaySoundCallback(IntPtr soundPath);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueWaitCallback(float seconds);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueCameraCallback(IntPtr action, IntPtr target);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void DialogueSetEventCallback(IntPtr eventName, IntPtr eventState);

        // Callback setter delegate types
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnStartedFn(DialogueStartedCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnEndedFn(DialogueEndedCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnLineFn(DialogueLineCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnEmoteFn(DialogueEmoteCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnStateChangeFn(DialogueStateChangeCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnEmotionFn(DialogueEmotionCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnTypingCompleteFn(DialogueTypingCompleteCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnCharTypedFn(DialogueCharTypedCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnGiveItemFn(DialogueGiveItemCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnTakeItemFn(DialogueTakeItemCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnStartQuestFn(DialogueStartQuestCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnCompleteStepFn(DialogueCompleteStepCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnPlayAnimFn(DialoguePlayAnimCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnPlaySoundFn(DialoguePlaySoundCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnWaitFn(DialogueWaitCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnCameraFn(DialogueCameraCallback cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        public delegate void SetOnSetEventFn(DialogueSetEventCallback cb);

        // Bound function pointers (set during initialization)
        public static LoadLibraryFn? LoadLibrary;
        public static UnloadLibraryFn? UnloadLibrary;
        public static GetLibraryNameFn? GetLibraryName;
        public static GetLibraryCharacterIdFn? GetLibraryCharacterId;
        public static StartFromLibraryFn? StartFromLibrary;
        public static StartFromTextFn? StartFromText;
        public static VoidFn? Advance;
        public static SelectChoiceFn? SelectChoice;
        public static VoidFn? ForceEnd;
        public static VoidFn? Pause;
        public static VoidFn? Resume;
        public static BoolFn? IsActive;
        public static BoolFn? IsTyping;
        public static BoolFn? IsWaitingForChoice;
        public static IntFn? GetState;
        public static StringFn? GetDisplayedText;
        public static StringFn? GetCurrentSpeaker;
        public static IntFn? GetSpeakerEntityId;
        public static IntFn? GetChoiceCount;
        public static GetChoiceTextFn? GetChoiceText;
        public static StringFn? GetConversationTitle;
        public static SetStateFn? SetState;
        public static GetStateValueFn? GetStateValue;
        public static VoidFn? ClearState;
        public static SetFloatFn? SetTextSpeed;
        public static SetFloatFn? SetMinDisplayTime;
        public static SetFloatFn? SetEndCooldown;
        
        // Callback setters
        public static SetOnStartedFn? SetOnStarted;
        public static SetOnEndedFn? SetOnEnded;
        public static SetOnLineFn? SetOnLine;
        public static SetOnEmoteFn? SetOnEmote;
        public static SetOnStateChangeFn? SetOnStateChange;
        public static SetOnEmotionFn? SetOnEmotion;
        public static SetOnTypingCompleteFn? SetOnTypingComplete;
        public static SetOnCharTypedFn? SetOnCharTyped;
        public static SetOnGiveItemFn? SetOnGiveItem;
        public static SetOnTakeItemFn? SetOnTakeItem;
        public static SetOnStartQuestFn? SetOnStartQuest;
        public static SetOnCompleteStepFn? SetOnCompleteStep;
        public static SetOnPlayAnimFn? SetOnPlayAnim;
        public static SetOnPlaySoundFn? SetOnPlaySound;
        public static SetOnWaitFn? SetOnWait;
        public static SetOnCameraFn? SetOnCamera;
        public static SetOnSetEventFn? SetOnSetEvent;

        public static bool IsInitialized { get; private set; } = false;

        public static void InitializeInteropExport(IntPtr* ptrs, int count) => InitializeInterop(ptrs, count);

        public static void InitializeInterop(IntPtr* ptrs, int count)
        {
            if (ptrs == null || count <= 0)
            {
                Console.WriteLine("[DialogueInterop] Invalid init args.");
                return;
            }

            try
            {
                int i = 0;
                // Library management (4)
                if (count > i) LoadLibrary = Marshal.GetDelegateForFunctionPointer<LoadLibraryFn>(ptrs[i++]);
                if (count > i) UnloadLibrary = Marshal.GetDelegateForFunctionPointer<UnloadLibraryFn>(ptrs[i++]);
                if (count > i) GetLibraryName = Marshal.GetDelegateForFunctionPointer<GetLibraryNameFn>(ptrs[i++]);
                if (count > i) GetLibraryCharacterId = Marshal.GetDelegateForFunctionPointer<GetLibraryCharacterIdFn>(ptrs[i++]);
                
                // Playback (7)
                if (count > i) StartFromLibrary = Marshal.GetDelegateForFunctionPointer<StartFromLibraryFn>(ptrs[i++]);
                if (count > i) StartFromText = Marshal.GetDelegateForFunctionPointer<StartFromTextFn>(ptrs[i++]);
                if (count > i) Advance = Marshal.GetDelegateForFunctionPointer<VoidFn>(ptrs[i++]);
                if (count > i) SelectChoice = Marshal.GetDelegateForFunctionPointer<SelectChoiceFn>(ptrs[i++]);
                if (count > i) ForceEnd = Marshal.GetDelegateForFunctionPointer<VoidFn>(ptrs[i++]);
                if (count > i) Pause = Marshal.GetDelegateForFunctionPointer<VoidFn>(ptrs[i++]);
                if (count > i) Resume = Marshal.GetDelegateForFunctionPointer<VoidFn>(ptrs[i++]);
                
                // State queries (10)
                if (count > i) IsActive = Marshal.GetDelegateForFunctionPointer<BoolFn>(ptrs[i++]);
                if (count > i) IsTyping = Marshal.GetDelegateForFunctionPointer<BoolFn>(ptrs[i++]);
                if (count > i) IsWaitingForChoice = Marshal.GetDelegateForFunctionPointer<BoolFn>(ptrs[i++]);
                if (count > i) GetState = Marshal.GetDelegateForFunctionPointer<IntFn>(ptrs[i++]);
                if (count > i) GetDisplayedText = Marshal.GetDelegateForFunctionPointer<StringFn>(ptrs[i++]);
                if (count > i) GetCurrentSpeaker = Marshal.GetDelegateForFunctionPointer<StringFn>(ptrs[i++]);
                if (count > i) GetSpeakerEntityId = Marshal.GetDelegateForFunctionPointer<IntFn>(ptrs[i++]);
                if (count > i) GetChoiceCount = Marshal.GetDelegateForFunctionPointer<IntFn>(ptrs[i++]);
                if (count > i) GetChoiceText = Marshal.GetDelegateForFunctionPointer<GetChoiceTextFn>(ptrs[i++]);
                if (count > i) GetConversationTitle = Marshal.GetDelegateForFunctionPointer<StringFn>(ptrs[i++]);
                
                // Game state (3)
                if (count > i) SetState = Marshal.GetDelegateForFunctionPointer<SetStateFn>(ptrs[i++]);
                if (count > i) GetStateValue = Marshal.GetDelegateForFunctionPointer<GetStateValueFn>(ptrs[i++]);
                if (count > i) ClearState = Marshal.GetDelegateForFunctionPointer<VoidFn>(ptrs[i++]);
                
                // Config (3)
                if (count > i) SetTextSpeed = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) SetMinDisplayTime = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                if (count > i) SetEndCooldown = Marshal.GetDelegateForFunctionPointer<SetFloatFn>(ptrs[i++]);
                
                // Callback setters (17)
                if (count > i) SetOnStarted = Marshal.GetDelegateForFunctionPointer<SetOnStartedFn>(ptrs[i++]);
                if (count > i) SetOnEnded = Marshal.GetDelegateForFunctionPointer<SetOnEndedFn>(ptrs[i++]);
                if (count > i) SetOnLine = Marshal.GetDelegateForFunctionPointer<SetOnLineFn>(ptrs[i++]);
                if (count > i) SetOnEmote = Marshal.GetDelegateForFunctionPointer<SetOnEmoteFn>(ptrs[i++]);
                if (count > i) SetOnStateChange = Marshal.GetDelegateForFunctionPointer<SetOnStateChangeFn>(ptrs[i++]);
                if (count > i) SetOnEmotion = Marshal.GetDelegateForFunctionPointer<SetOnEmotionFn>(ptrs[i++]);
                if (count > i) SetOnTypingComplete = Marshal.GetDelegateForFunctionPointer<SetOnTypingCompleteFn>(ptrs[i++]);
                if (count > i) SetOnCharTyped = Marshal.GetDelegateForFunctionPointer<SetOnCharTypedFn>(ptrs[i++]);
                if (count > i) SetOnGiveItem = Marshal.GetDelegateForFunctionPointer<SetOnGiveItemFn>(ptrs[i++]);
                if (count > i) SetOnTakeItem = Marshal.GetDelegateForFunctionPointer<SetOnTakeItemFn>(ptrs[i++]);
                if (count > i) SetOnStartQuest = Marshal.GetDelegateForFunctionPointer<SetOnStartQuestFn>(ptrs[i++]);
                if (count > i) SetOnCompleteStep = Marshal.GetDelegateForFunctionPointer<SetOnCompleteStepFn>(ptrs[i++]);
                if (count > i) SetOnPlayAnim = Marshal.GetDelegateForFunctionPointer<SetOnPlayAnimFn>(ptrs[i++]);
                if (count > i) SetOnPlaySound = Marshal.GetDelegateForFunctionPointer<SetOnPlaySoundFn>(ptrs[i++]);
                if (count > i) SetOnWait = Marshal.GetDelegateForFunctionPointer<SetOnWaitFn>(ptrs[i++]);
                if (count > i) SetOnCamera = Marshal.GetDelegateForFunctionPointer<SetOnCameraFn>(ptrs[i++]);
                if (count > i) SetOnSetEvent = Marshal.GetDelegateForFunctionPointer<SetOnSetEventFn>(ptrs[i++]);

                IsInitialized = true;
                Console.WriteLine($"[Managed] DialogueInterop delegates initialized ({i} functions).");
                
                // Auto-initialize DialogueSystem to wire up event callbacks
                DialogueSystem.Initialize();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[DialogueInterop] Initialization failed: {ex}");
            }
        }
    }
}

