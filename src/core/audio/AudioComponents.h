// AudioComponents.h
// Claymore Engine Audio ECS Components
#pragma once

#include <glm/glm.hpp>
#include <string>
#include "core/assets/AssetReference.h"
#include "Audio.h"

//------------------------------------------------------------------------------
// Audio Source Component
// Attach to entities that emit sound (3D positional or 2D global)
//------------------------------------------------------------------------------
struct AudioSourceComponent {
    /// Reference to audio asset (.wav, .mp3, .ogg, .flac)
    AssetReference AudioClip;
    
    /// Direct path to audio file (used if AudioClip is not set)
    std::string AudioPath;

    //--------------------------------------------------------------------------
    // Playback Settings
    //--------------------------------------------------------------------------
    
    /// Volume multiplier (0.0 - 1.0+)
    float Volume = 1.0f;
    
    /// Pitch/playback speed (1.0 = normal)
    float Pitch = 1.0f;
    
    /// Whether the sound loops
    bool Loop = false;
    
    /// Start playing automatically when the entity is created
    bool PlayOnAwake = true;
    
    /// Mute the sound (still plays, but silent)
    bool Mute = false;

    //--------------------------------------------------------------------------
    // 3D Spatial Audio Settings
    //--------------------------------------------------------------------------
    
    /// Enable 3D spatialization (position-based audio)
    /// When false, plays as 2D audio (constant volume regardless of position)
    bool Spatial = true;
    
    /// Distance at which volume starts attenuating
    float MinDistance = 1.0f;
    
    /// Distance at which sound becomes inaudible
    float MaxDistance = 50.0f;
    
    /// Doppler effect multiplier (0 = disabled, 1 = normal)
    float DopplerFactor = 1.0f;
    
    /// Attenuation rolloff factor (higher = faster falloff)
    float Rolloff = 1.0f;

    //--------------------------------------------------------------------------
    // Runtime State (managed by audio system)
    //--------------------------------------------------------------------------
    
    /// Handle to the currently playing sound instance
    AudioHandle SoundHandle = INVALID_AUDIO_HANDLE;
    
    /// Whether the sound is currently playing
    bool IsPlaying = false;
    
    /// Whether the sound is currently paused
    bool IsPaused = false;
    
    /// Whether the component has been initialized
    bool Initialized = false;
    
    /// Previous world position (for velocity calculation)
    glm::vec3 LastPosition = glm::vec3(0.0f);

    //--------------------------------------------------------------------------
    // Control Methods
    //--------------------------------------------------------------------------
    
    /// Play the sound (uses AudioClip or AudioPath)
    void Play() {
        PlayRequested = true;
    }
    
    /// Stop the sound
    void Stop() {
        StopRequested = true;
    }
    
    /// Pause the sound
    void Pause() {
        PauseRequested = true;
    }
    
    /// Resume a paused sound
    void Resume() {
        ResumeRequested = true;
    }

    //--------------------------------------------------------------------------
    // Internal Request Flags (processed by audio system)
    //--------------------------------------------------------------------------
    bool PlayRequested = false;
    bool StopRequested = false;
    bool PauseRequested = false;
    bool ResumeRequested = false;

    /// Get the resolved audio path
    /// Note: If using AudioClip (AssetReference), caller must resolve via AssetLibrary
    /// This method returns AudioPath for direct file references
    std::string GetResolvedPath() const {
        // AudioClip GUID resolution happens in Scene::Update via AssetLibrary
        // For direct path usage, return AudioPath
        return AudioPath;
    }
    
    /// Check if this component uses an asset reference (vs direct path)
    bool UsesAssetReference() const {
        return AudioClip.IsValid();
    }
};

//------------------------------------------------------------------------------
// Audio Listener Component
// Attach to the entity that represents the "ears" (usually the camera)
// Only one AudioListener should be active at a time
//------------------------------------------------------------------------------
struct AudioListenerComponent {
    /// Whether this listener is active
    /// Only one listener should be active; highest priority wins
    bool Active = true;
    
    /// Priority for listener selection (higher = preferred)
    int Priority = 0;
    
    /// Volume multiplier for all audio heard by this listener
    float VolumeMultiplier = 1.0f;

    //--------------------------------------------------------------------------
    // Runtime State
    //--------------------------------------------------------------------------
    
    /// Previous position for velocity calculation
    glm::vec3 LastPosition = glm::vec3(0.0f);
    
    /// Calculated velocity
    glm::vec3 Velocity = glm::vec3(0.0f);
    
    /// Whether this listener was active last frame
    bool WasActive = false;
};

//------------------------------------------------------------------------------
// Audio Reverb Zone Component (Future Extension)
// Defines a volume where reverb/echo effects are applied
//------------------------------------------------------------------------------
struct AudioReverbZoneComponent {
    /// Shape of the reverb zone
    enum class Shape { Sphere, Box } ZoneShape = Shape::Sphere;
    
    /// Radius for sphere shape
    float Radius = 10.0f;
    
    /// Size for box shape
    glm::vec3 Size = glm::vec3(10.0f);
    
    /// Reverb preset
    enum class Preset { 
        None, 
        Room, 
        Hall, 
        Cave, 
        Arena, 
        Forest, 
        Underwater 
    } ReverbPreset = Preset::Room;
    
    /// Blend distance (transition zone)
    float BlendDistance = 2.0f;
    
    /// Custom reverb parameters
    float DecayTime = 1.5f;
    float Diffusion = 0.8f;
    float Density = 0.5f;
    float WetLevel = 0.5f;
};
