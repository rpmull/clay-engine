// Audio.h
// Claymore Engine Audio System - miniaudio backend
// 
// Threading Model:
// - miniaudio runs its own audio mixing thread internally
// - File loading/decoding happens on a dedicated background thread
// - Commands are queued and processed thread-safely
// - Main thread only needs to call Update() once per frame
#pragma once

#include <glm/glm.hpp>
#include <string>
#include <cstdint>
#include <functional>
#include <vector>

// Forward declarations (miniaudio internals are opaque)
struct ma_engine;
struct ma_sound;

/// Audio handle representing an active sound instance
using AudioHandle = uint32_t;
constexpr AudioHandle INVALID_AUDIO_HANDLE = 0;

/// Audio loading priority
enum class AudioLoadPriority {
    Low = 0,      // Background music, ambient
    Normal = 1,   // Standard sound effects
    High = 2      // UI sounds, critical feedback
};

/// Callback for async audio operations
using AudioLoadCallback = std::function<void(AudioHandle handle, bool success)>;

/// Global audio system singleton
/// Provides 2D and 3D spatial audio playback using miniaudio
/// 
/// Threading: The audio system uses a dedicated background thread for file loading
/// and decoding. All public methods are thread-safe and can be called from any thread.
class Audio {
public:
    static Audio& Get() {
        static Audio instance;
        return instance;
    }

    /// Initialize the audio engine and background thread. Call once at startup.
    static void Init();
    
    /// Shutdown the audio engine and background thread. Call once at exit.
    static void Shutdown();
    
    /// Update audio system (process commands, cleanup finished sounds)
    /// Call once per frame from the main thread.
    static void Update(float deltaTime);

    /// Check if audio system is initialized
    static bool IsInitialized();
    
    /// Get number of currently active sounds
    static uint32_t GetActiveSoundCount();
    
    /// Get number of pending async loads
    static uint32_t GetPendingLoadCount();

    //--------------------------------------------------------------------------
    // Synchronous Playback API (blocks until loaded)
    //--------------------------------------------------------------------------
    
    /// Play a 2D sound (not spatialized)
    /// @param path Asset path to audio file (.wav, .mp3, .ogg, .flac)
    /// @param volume Volume multiplier (0.0 - 1.0+)
    /// @param loop Whether to loop the sound
    /// @return Handle to the sound instance, or INVALID_AUDIO_HANDLE on failure
    static AudioHandle Play(const std::string& path, float volume = 1.0f, bool loop = false);
    
    /// Play a 3D spatialized sound
    /// @param path Asset path to audio file
    /// @param position World position of the sound
    /// @param volume Volume multiplier
    /// @param loop Whether to loop
    /// @param minDistance Distance at which volume starts attenuating
    /// @param maxDistance Distance at which sound becomes inaudible
    /// @return Handle to the sound instance
    static AudioHandle Play3D(const std::string& path, 
                              const glm::vec3& position, 
                              float volume = 1.0f, 
                              bool loop = false,
                              float minDistance = 1.0f,
                              float maxDistance = 50.0f);

    //--------------------------------------------------------------------------
    // Asynchronous Playback API (non-blocking, uses background thread)
    //--------------------------------------------------------------------------
    
    /// Play a 2D sound asynchronously (loads on background thread)
    /// @param path Asset path to audio file
    /// @param volume Volume multiplier
    /// @param loop Whether to loop
    /// @param callback Optional callback when loading completes (called on main thread)
    /// @param priority Loading priority (higher = loaded first)
    /// @return Pending handle (becomes valid after loading completes)
    static AudioHandle PlayAsync(const std::string& path, 
                                  float volume = 1.0f, 
                                  bool loop = false,
                                  AudioLoadCallback callback = nullptr,
                                  AudioLoadPriority priority = AudioLoadPriority::Normal);
    
    /// Play a 3D sound asynchronously
    static AudioHandle Play3DAsync(const std::string& path,
                                    const glm::vec3& position,
                                    float volume = 1.0f,
                                    bool loop = false,
                                    float minDistance = 1.0f,
                                    float maxDistance = 50.0f,
                                    AudioLoadCallback callback = nullptr,
                                    AudioLoadPriority priority = AudioLoadPriority::Normal);
    
    /// Preload audio file asynchronously (for later instant playback)
    static void PreloadAsync(const std::string& path, 
                              AudioLoadCallback callback = nullptr,
                              AudioLoadPriority priority = AudioLoadPriority::Low);
    
    /// Stop a playing sound
    static void Stop(AudioHandle handle);
    
    /// Stop all currently playing sounds
    static void StopAll();
    
    /// Pause a playing sound
    static void Pause(AudioHandle handle);
    
    /// Resume a paused sound
    static void Resume(AudioHandle handle);
    
    /// Check if a sound is currently playing
    static bool IsPlaying(AudioHandle handle);

    //--------------------------------------------------------------------------
    // Sound Instance Control
    //--------------------------------------------------------------------------
    
    /// Set volume of a playing sound
    static void SetVolume(AudioHandle handle, float volume);
    
    /// Set pitch/playback speed of a playing sound (1.0 = normal)
    static void SetPitch(AudioHandle handle, float pitch);
    
    /// Set 3D position of a spatialized sound
    static void SetPosition(AudioHandle handle, const glm::vec3& position);
    
    /// Set 3D velocity for doppler effect
    static void SetVelocity(AudioHandle handle, const glm::vec3& velocity);

    //--------------------------------------------------------------------------
    // Listener (Camera/Player)
    //--------------------------------------------------------------------------
    
    /// Set the audio listener's position and orientation
    /// Typically called with the active camera's transform
    static void SetListenerTransform(const glm::vec3& position, 
                                     const glm::vec3& forward, 
                                     const glm::vec3& up);
    
    /// Set listener velocity for doppler calculations
    static void SetListenerVelocity(const glm::vec3& velocity);

    //--------------------------------------------------------------------------
    // Global Controls
    //--------------------------------------------------------------------------
    
    /// Set master volume (affects all sounds)
    static void SetMasterVolume(float volume);
    
    /// Get current master volume
    static float GetMasterVolume();
    
    /// Pause all audio playback
    static void PauseAll();
    
    /// Resume all audio playback
    static void ResumeAll();

    //--------------------------------------------------------------------------
    // Resource Management
    //--------------------------------------------------------------------------
    
    /// Preload an audio file into memory for faster playback
    static bool Preload(const std::string& path);
    
    /// Unload a preloaded audio file
    static void Unload(const std::string& path);
    
    /// Clear all preloaded audio files
    static void UnloadAll();

    //--------------------------------------------------------------------------
    // Streaming API (for large files like music)
    //--------------------------------------------------------------------------
    
    /// Play a sound with streaming (doesn't load entire file into memory)
    /// Best for music and long ambient sounds
    static AudioHandle PlayStreaming(const std::string& path, float volume = 1.0f, bool loop = true);

private:
    Audio() = default;
    ~Audio() = default;
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Allow AudioThread to access private members for async loading
    friend class AudioThread;

    // Internal methods
    static void AudioThreadFunc();
    static void ProcessLoadQueue();
    static void ProcessCompletionCallbacks();

    static ma_engine* s_Engine;
    static bool s_Initialized;
    static float s_MasterVolume;
    static AudioHandle s_NextHandle;
    
    // Threading state (defined in Audio.cpp)
    static class AudioThread* s_AudioThread;
};
