using System;
using System.Runtime.InteropServices;

namespace ClaymoreEngine
{
    /// <summary>
    /// Dialogue state enum (mirrors native DialogueState)
    /// </summary>
    public enum DialogueState
    {
        Inactive = 0,
        DisplayingLine = 1,
        WaitingForInput = 2,
        WaitingForChoice = 3,
        ProcessingCommand = 4,
        Paused = 5,
        Ending = 6
    }


    /// <summary>
    /// Reference to a DialogueLibrary asset (serializable GUID reference)
    /// </summary>
    [Serializable]
    public struct DialogueLibraryRef
    {
        [SerializeField]
        public string guid;

        public bool IsValid => !string.IsNullOrEmpty(guid);

        public DialogueLibraryRef(string guidStr)
        {
            guid = guidStr;
        }
    }

    /// <summary>
    /// Main interface to the native dialogue system.
    /// Provides static methods for dialogue playback and events for UI integration.
    /// Uses DialogueInterop function pointers (not DllImport) for embedded .NET runtime.
    /// </summary>
    public static class DialogueSystem
    {
        #region Events

        /// <summary>Fired when dialogue starts</summary>
        public static event Action<string> OnDialogueStarted;
        
        /// <summary>Fired when dialogue ends</summary>
        public static event Action OnDialogueEnded;
        
        /// <summary>Fired when a dialogue line is displayed (speaker, text, choiceCount)</summary>
        public static event Action<string, string, int> OnDialogueLine;
        
        /// <summary>Fired for emote/animation cues</summary>
        public static event Action<string> OnEmote;
        
        /// <summary>Fired when game state changes</summary>
        public static event Action<string, string> OnStateChange;
        
        /// <summary>Fired for emotion changes</summary>
        public static event Action<string> OnEmotion;
        
        /// <summary>Fired when typewriter completes</summary>
        public static event Action OnTypingComplete;
        
        /// <summary>Fired per character during typewriter</summary>
        public static event Action<char> OnCharacterTyped;

        // Game command events
        public static event Action<string, int> OnGiveItem;
        public static event Action<string, int> OnTakeItem;
        public static event Action<string> OnStartQuest;
        public static event Action<string, string> OnCompleteStep;
        public static event Action<string> OnPlayAnimation;
        public static event Action<string> OnPlaySound;
        public static event Action<float> OnWait;
        public static event Action<string, string> OnCamera;
        public static event Action<string, string> OnSetEvent;

        #endregion

        #region Callback Instances (prevent GC collection)

        private static DialogueInterop.DialogueStartedCallback _onStartedCb;
        private static DialogueInterop.DialogueEndedCallback _onEndedCb;
        private static DialogueInterop.DialogueLineCallback _onLineCb;
        private static DialogueInterop.DialogueEmoteCallback _onEmoteCb;
        private static DialogueInterop.DialogueStateChangeCallback _onStateChangeCb;
        private static DialogueInterop.DialogueEmotionCallback _onEmotionCb;
        private static DialogueInterop.DialogueTypingCompleteCallback _onTypingCompleteCb;
        private static DialogueInterop.DialogueCharTypedCallback _onCharTypedCb;
        private static DialogueInterop.DialogueGiveItemCallback _onGiveItemCb;
        private static DialogueInterop.DialogueTakeItemCallback _onTakeItemCb;
        private static DialogueInterop.DialogueStartQuestCallback _onStartQuestCb;
        private static DialogueInterop.DialogueCompleteStepCallback _onCompleteStepCb;
        private static DialogueInterop.DialoguePlayAnimCallback _onPlayAnimCb;
        private static DialogueInterop.DialoguePlaySoundCallback _onPlaySoundCb;
        private static DialogueInterop.DialogueWaitCallback _onWaitCb;
        private static DialogueInterop.DialogueCameraCallback _onCameraCb;
        private static DialogueInterop.DialogueSetEventCallback _onSetEventCb;

        #endregion

        #region Initialization

        private static bool _initialized = false;

        /// <summary>
        /// Initialize the dialogue system. Called automatically when DialogueInterop is initialized.
        /// </summary>
        public static void Initialize()
        {
            if (_initialized) return;
            
            // Check if DialogueInterop is ready
            if (!DialogueInterop.IsInitialized)
            {
                Console.WriteLine("[DialogueSystem] Warning: DialogueInterop not yet initialized");
                return;
            }
            
            // Set up native callbacks
            _onStartedCb = (ptr) => OnDialogueStarted?.Invoke(Marshal.PtrToStringAnsi(ptr) ?? "");
            _onEndedCb = () => OnDialogueEnded?.Invoke();
            _onLineCb = (speaker, text, count) => OnDialogueLine?.Invoke(
                Marshal.PtrToStringAnsi(speaker) ?? "",
                Marshal.PtrToStringAnsi(text) ?? "",
                count);
            _onEmoteCb = (ptr) => OnEmote?.Invoke(Marshal.PtrToStringAnsi(ptr) ?? "");
            _onStateChangeCb = (key, val) => {
                var keyStr = Marshal.PtrToStringAnsi(key) ?? "";
                var valStr = Marshal.PtrToStringAnsi(val) ?? "";
                // Track keys set from dialogue for save/load
                if (!string.IsNullOrEmpty(keyStr))
                    _trackedStateKeys.Add(keyStr);
                OnStateChange?.Invoke(keyStr, valStr);
            };
            _onEmotionCb = (ptr) => OnEmotion?.Invoke(Marshal.PtrToStringAnsi(ptr) ?? "");
            _onTypingCompleteCb = () => OnTypingComplete?.Invoke();
            _onCharTypedCb = (c) => OnCharacterTyped?.Invoke((char)c);
            _onGiveItemCb = (item, count) => OnGiveItem?.Invoke(Marshal.PtrToStringAnsi(item) ?? "", count);
            _onTakeItemCb = (item, count) => OnTakeItem?.Invoke(Marshal.PtrToStringAnsi(item) ?? "", count);
            _onStartQuestCb = (quest) => OnStartQuest?.Invoke(Marshal.PtrToStringAnsi(quest) ?? "");
            _onCompleteStepCb = (quest, step) => OnCompleteStep?.Invoke(
                Marshal.PtrToStringAnsi(quest) ?? "",
                Marshal.PtrToStringAnsi(step) ?? "");
            _onPlayAnimCb = (anim) => OnPlayAnimation?.Invoke(Marshal.PtrToStringAnsi(anim) ?? "");
            _onPlaySoundCb = (sound) => OnPlaySound?.Invoke(Marshal.PtrToStringAnsi(sound) ?? "");
            _onWaitCb = (sec) => OnWait?.Invoke(sec);
            _onCameraCb = (action, target) => OnCamera?.Invoke(
                Marshal.PtrToStringAnsi(action) ?? "",
                Marshal.PtrToStringAnsi(target) ?? "");
            _onSetEventCb = (eventName, eventState) => OnSetEvent?.Invoke(
                Marshal.PtrToStringAnsi(eventName) ?? "",
                Marshal.PtrToStringAnsi(eventState) ?? "");

            // Register callbacks with native via DialogueInterop
            DialogueInterop.SetOnStarted?.Invoke(_onStartedCb);
            DialogueInterop.SetOnEnded?.Invoke(_onEndedCb);
            DialogueInterop.SetOnLine?.Invoke(_onLineCb);
            DialogueInterop.SetOnEmote?.Invoke(_onEmoteCb);
            DialogueInterop.SetOnStateChange?.Invoke(_onStateChangeCb);
            DialogueInterop.SetOnEmotion?.Invoke(_onEmotionCb);
            DialogueInterop.SetOnTypingComplete?.Invoke(_onTypingCompleteCb);
            DialogueInterop.SetOnCharTyped?.Invoke(_onCharTypedCb);
            DialogueInterop.SetOnGiveItem?.Invoke(_onGiveItemCb);
            DialogueInterop.SetOnTakeItem?.Invoke(_onTakeItemCb);
            DialogueInterop.SetOnStartQuest?.Invoke(_onStartQuestCb);
            DialogueInterop.SetOnCompleteStep?.Invoke(_onCompleteStepCb);
            DialogueInterop.SetOnPlayAnim?.Invoke(_onPlayAnimCb);
            DialogueInterop.SetOnPlaySound?.Invoke(_onPlaySoundCb);
            DialogueInterop.SetOnWait?.Invoke(_onWaitCb);
            DialogueInterop.SetOnCamera?.Invoke(_onCameraCb);
            DialogueInterop.SetOnSetEvent?.Invoke(_onSetEventCb);

            _initialized = true;
            Console.WriteLine("[DialogueSystem] Initialized - callbacks registered with native");
        }

        internal static void ResetRuntimeState()
        {
            OnDialogueStarted = null;
            OnDialogueEnded = null;
            OnDialogueLine = null;
            OnEmote = null;
            OnStateChange = null;
            OnEmotion = null;
            OnTypingComplete = null;
            OnCharacterTyped = null;
            OnGiveItem = null;
            OnTakeItem = null;
            OnStartQuest = null;
            OnCompleteStep = null;
            OnPlayAnimation = null;
            OnPlaySound = null;
            OnWait = null;
            OnCamera = null;
            OnSetEvent = null;

            _trackedStateKeys.Clear();
            _stateProviders.Clear();
        }

        #endregion

        #region Public API

        /// <summary>Load a dialogue library from file.</summary>
        public static string LoadLibrary(string filePath)
        {
            Initialize();
            var ptr = DialogueInterop.LoadLibrary?.Invoke(filePath) ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(ptr) ?? "";
        }

        /// <summary>Unload a library by GUID.</summary>
        public static void UnloadLibrary(string guid) => DialogueInterop.UnloadLibrary?.Invoke(guid);

        /// <summary>Get library display name.</summary>
        public static string GetLibraryName(string guid)
        {
            var ptr = DialogueInterop.GetLibraryName?.Invoke(guid) ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(ptr) ?? "";
        }

        /// <summary>Start dialogue from a library reference.</summary>
        public static bool StartDialogue(DialogueLibraryRef libraryRef, int speakerEntityId = -1)
        {
            Initialize();
            SyncStateFromProviders(); // Sync game state before dialogue starts
            if (!libraryRef.IsValid) return false;
            return DialogueInterop.StartFromLibrary?.Invoke(libraryRef.guid, speakerEntityId) ?? false;
        }

        /// <summary>Start dialogue from a library GUID.</summary>
        public static bool StartDialogue(string libraryGuid, int speakerEntityId = -1)
        {
            Initialize();
            SyncStateFromProviders(); // Sync game state before dialogue starts
            return DialogueInterop.StartFromLibrary?.Invoke(libraryGuid, speakerEntityId) ?? false;
        }

        /// <summary>Start dialogue from raw script text.</summary>
        public static bool StartFromText(string dialogueText, int speakerEntityId = -1)
        {
            Initialize();
            SyncStateFromProviders(); // Sync game state before dialogue starts
            return DialogueInterop.StartFromText?.Invoke(dialogueText, speakerEntityId) ?? false;
        }

        /// <summary>Advance dialogue (continue or skip typewriter).</summary>
        public static void Advance() => DialogueInterop.Advance?.Invoke();

        /// <summary>Select a choice by index.</summary>
        public static void SelectChoice(int index) => DialogueInterop.SelectChoice?.Invoke(index);

        /// <summary>Force end the dialogue.</summary>
        public static void ForceEnd() => DialogueInterop.ForceEnd?.Invoke();

        /// <summary>Pause dialogue.</summary>
        public static void Pause() => DialogueInterop.Pause?.Invoke();

        /// <summary>Resume dialogue.</summary>
        public static void Resume() => DialogueInterop.Resume?.Invoke();

        /// <summary>Is dialogue active?</summary>
        public static bool IsActive => DialogueInterop.IsActive?.Invoke() ?? false;

        /// <summary>Is typewriter effect in progress?</summary>
        public static bool IsTyping => DialogueInterop.IsTyping?.Invoke() ?? false;

        /// <summary>Is waiting for player to select a choice?</summary>
        public static bool IsWaitingForChoice => DialogueInterop.IsWaitingForChoice?.Invoke() ?? false;

        /// <summary>Current dialogue state.</summary>
        public static DialogueState State => (DialogueState)(DialogueInterop.GetState?.Invoke() ?? 0);

        /// <summary>Currently displayed text (with typewriter progress).</summary>
        public static string DisplayedText
        {
            get
            {
                var ptr = DialogueInterop.GetDisplayedText?.Invoke() ?? IntPtr.Zero;
                return Marshal.PtrToStringAnsi(ptr) ?? "";
            }
        }

        /// <summary>Current speaker name.</summary>
        public static string CurrentSpeaker
        {
            get
            {
                var ptr = DialogueInterop.GetCurrentSpeaker?.Invoke() ?? IntPtr.Zero;
                return Marshal.PtrToStringAnsi(ptr) ?? "";
            }
        }

        /// <summary>Speaker entity ID (-1 if none).</summary>
        public static int SpeakerEntityId => DialogueInterop.GetSpeakerEntityId?.Invoke() ?? -1;

        /// <summary>Number of available choices.</summary>
        public static int ChoiceCount => DialogueInterop.GetChoiceCount?.Invoke() ?? 0;

        /// <summary>Get choice text at index.</summary>
        public static string GetChoiceText(int index)
        {
            var ptr = DialogueInterop.GetChoiceText?.Invoke(index) ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(ptr) ?? "";
        }

        /// <summary>Current conversation title.</summary>
        public static string ConversationTitle
        {
            get
            {
                var ptr = DialogueInterop.GetConversationTitle?.Invoke() ?? IntPtr.Zero;
                return Marshal.PtrToStringAnsi(ptr) ?? "";
            }
        }

        /// <summary>Get a game state variable.</summary>
        public static string GetGameState(string key)
        {
            var ptr = DialogueInterop.GetStateValue?.Invoke(key) ?? IntPtr.Zero;
            return Marshal.PtrToStringAnsi(ptr) ?? "";
        }

        /// <summary>Clear all game state. Use sparingly - this erases all persistent dialogue flags!</summary>
        public static void ClearGameState() => DialogueInterop.ClearState?.Invoke();
        
        /// <summary>
        /// Get all dialogue state as a dictionary for saving.
        /// Note: This only returns state set via SetGameState/\SetState, not provider state.
        /// </summary>
        public static System.Collections.Generic.Dictionary<string, string> GetAllStateForSave()
        {
            var result = new System.Collections.Generic.Dictionary<string, string>();
            // The native side doesn't expose iteration, so we track keys ourselves
            foreach (var key in _trackedStateKeys)
            {
                var value = GetGameState(key);
                if (!string.IsNullOrEmpty(value))
                    result[key] = value;
            }
            return result;
        }
        
        /// <summary>
        /// Restore dialogue state from a saved dictionary.
        /// </summary>
        public static void RestoreStateFromSave(System.Collections.Generic.Dictionary<string, string> savedState)
        {
            if (savedState == null) return;
            foreach (var kv in savedState)
            {
                SetGameState(kv.Key, kv.Value);
            }
        }
        
        private static readonly System.Collections.Generic.HashSet<string> _trackedStateKeys 
            = new System.Collections.Generic.HashSet<string>();
        
        /// <summary>Set a game state variable (with tracking for save/load).</summary>
        public static void SetGameState(string key, string value)
        {
            _trackedStateKeys.Add(key);
            DialogueInterop.SetState?.Invoke(key, value);
        }

        /// <summary>
        /// Register a state provider that will be queried for state values.
        /// Multiple providers can be registered; they're queried in order until one returns a non-null value.
        /// </summary>
        public static void RegisterStateProvider(IDialogueStateProvider provider)
        {
            if (provider != null && !_stateProviders.Contains(provider))
                _stateProviders.Add(provider);
        }
        
        /// <summary>Unregister a state provider.</summary>
        public static void UnregisterStateProvider(IDialogueStateProvider provider)
        {
            _stateProviders.Remove(provider);
        }
        
        /// <summary>
        /// Sync all state from registered providers to the dialogue system.
        /// Call this before starting dialogue or when game state changes significantly.
        /// </summary>
        public static void SyncStateFromProviders()
        {
            foreach (var provider in _stateProviders)
            {
                var keys = provider.GetStateKeys();
                if (keys != null)
                {
                    foreach (var key in keys)
                    {
                        var value = provider.GetStateValue(key);
                        if (value != null)
                            SetGameState(key, value);
                    }
                }
            }
        }
        
        private static readonly System.Collections.Generic.List<IDialogueStateProvider> _stateProviders 
            = new System.Collections.Generic.List<IDialogueStateProvider>();

        /// <summary>Set text speed (characters per second, 0 = instant).</summary>
        public static void SetTextSpeed(float charsPerSecond) => DialogueInterop.SetTextSpeed?.Invoke(charsPerSecond);

        /// <summary>Set minimum display time before skip allowed.</summary>
        public static void SetMinDisplayTime(float seconds) => DialogueInterop.SetMinDisplayTime?.Invoke(seconds);

        /// <summary>Set cooldown after dialogue ends.</summary>
        public static void SetEndCooldown(float seconds) => DialogueInterop.SetEndCooldown?.Invoke(seconds);

        #endregion
    }
    
    /// <summary>
    /// Interface for providing dialogue state values from game data.
    /// Implement this to connect game-specific data (player stats, inventory, etc.) to dialogue conditions.
    /// </summary>
    public interface IDialogueStateProvider
    {
        /// <summary>Get all state keys this provider supplies.</summary>
        System.Collections.Generic.IEnumerable<string> GetStateKeys();
        
        /// <summary>Get the value for a specific key. Return null if not handled.</summary>
        string GetStateValue(string key);
    }
    
    /// <summary>
    /// Base class for a player data provider that syncs common player attributes to dialogue state.
    /// Inherit from this and override the properties to connect to your actual player data.
    /// </summary>
    /// <example>
    /// public class MyPlayerStateProvider : PlayerDialogueStateProvider
    /// {
    ///     private MyPlayerData _player;
    ///     
    ///     public MyPlayerStateProvider(MyPlayerData player) { _player = player; }
    ///     
    ///     public override string PlayerRace => _player.Race.ToString().ToLower();
    ///     public override string PlayerClass => _player.Class.ToString().ToLower();
    ///     public override string PlayerFaction => _player.Faction?.Name ?? "";
    ///     public override int PlayerLevel => _player.Level;
    ///     public override int PlayerGold => _player.Inventory.Gold;
    ///     
    ///     protected override System.Collections.Generic.IEnumerable<System.Collections.Generic.KeyValuePair<string, string>> GetCustomStates()
    ///     {
    ///         yield return new("player_name", _player.Name);
    ///         yield return new("has_magic", _player.HasMagic ? "true" : "false");
    ///         foreach (var title in _player.Titles)
    ///             yield return new($"has_title_{title.ToLower()}", "true");
    ///     }
    /// }
    /// </example>
    public abstract class PlayerDialogueStateProvider : IDialogueStateProvider
    {
        // Override these in your subclass to provide actual values
        public virtual string PlayerRace => "";
        public virtual string PlayerClass => "";
        public virtual string PlayerFaction => "";
        public virtual string PlayerGender => "";
        public virtual int PlayerLevel => 1;
        public virtual int PlayerGold => 0;
        public virtual int PlayerHealth => 100;
        public virtual int PlayerMaxHealth => 100;
        
        /// <summary>Override to provide additional custom state key-value pairs.</summary>
        protected virtual System.Collections.Generic.IEnumerable<System.Collections.Generic.KeyValuePair<string, string>> GetCustomStates()
        {
            yield break;
        }
        
        public System.Collections.Generic.IEnumerable<string> GetStateKeys()
        {
            yield return "player_race";
            yield return "player_class";
            yield return "player_faction";
            yield return "player_gender";
            yield return "player_level";
            yield return "player_gold";
            yield return "player_health";
            yield return "player_max_health";
            
            foreach (var kv in GetCustomStates())
                yield return kv.Key;
        }
        
        public string GetStateValue(string key)
        {
            return key switch
            {
                "player_race" => PlayerRace,
                "player_class" => PlayerClass,
                "player_faction" => PlayerFaction,
                "player_gender" => PlayerGender,
                "player_level" => PlayerLevel.ToString(),
                "player_gold" => PlayerGold.ToString(),
                "player_health" => PlayerHealth.ToString(),
                "player_max_health" => PlayerMaxHealth.ToString(),
                _ => GetCustomStateValue(key)
            };
        }
        
        private string GetCustomStateValue(string key)
        {
            foreach (var kv in GetCustomStates())
            {
                if (kv.Key == key)
                    return kv.Value;
            }
            return null;
        }
    }
    
    /// <summary>
    /// Simple dictionary-based state provider for quick prototyping.
    /// </summary>
    public class DictionaryStateProvider : IDialogueStateProvider
    {
        private readonly System.Collections.Generic.Dictionary<string, string> _states 
            = new System.Collections.Generic.Dictionary<string, string>();
        
        public void Set(string key, string value) => _states[key] = value;
        public void Set(string key, bool value) => _states[key] = value ? "true" : "false";
        public void Set(string key, int value) => _states[key] = value.ToString();
        public void Remove(string key) => _states.Remove(key);
        public void Clear() => _states.Clear();
        
        public System.Collections.Generic.IEnumerable<string> GetStateKeys() => _states.Keys;
        public string GetStateValue(string key) => _states.TryGetValue(key, out var v) ? v : null;
    }
}

