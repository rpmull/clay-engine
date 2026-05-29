// Audio.cpp
// Claymore Engine Audio System - miniaudio implementation with background threading

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "Audio.h"
#include "core/vfs/FileSystem.h"

#include <iostream>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <queue>
#include <condition_variable>
#include <algorithm>

//------------------------------------------------------------------------------
// Audio Thread - Background loading and processing
//------------------------------------------------------------------------------
class AudioThread {
public:
    struct LoadRequest {
        AudioHandle handle;
        std::string path;
        float volume;
        bool loop;
        bool is3D;
        glm::vec3 position;
        float minDistance;
        float maxDistance;
        AudioLoadPriority priority;
        AudioLoadCallback callback;
        bool isPreload;  // Just preload, don't play
        bool isStreaming; // Use streaming instead of full decode
    };

    struct CompletionEvent {
        AudioHandle handle;
        bool success;
        AudioLoadCallback callback;
    };

    AudioThread() : m_Running(false) {}
    
    void Start() {
        m_Running = true;
        m_Thread = std::thread(&AudioThread::ThreadFunc, this);
        std::cout << "[Audio] Background thread started." << std::endl;
    }
    
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_Running = false;
        }
        m_Condition.notify_all();
        if (m_Thread.joinable()) {
            m_Thread.join();
        }
        std::cout << "[Audio] Background thread stopped." << std::endl;
    }
    
    void EnqueueLoad(LoadRequest&& request) {
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_LoadQueue.push(std::move(request));
        }
        m_Condition.notify_one();
    }
    
    void ProcessCompletions(ma_engine* engine) {
        std::vector<CompletionEvent> completions;
        {
            std::lock_guard<std::mutex> lock(m_CompletionMutex);
            std::swap(completions, m_Completions);
        }
        
        for (auto& evt : completions) {
            if (evt.callback) {
                evt.callback(evt.handle, evt.success);
            }
        }
    }
    
    uint32_t GetPendingCount() const {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        return static_cast<uint32_t>(m_LoadQueue.size());
    }

private:
    void ThreadFunc();
    
    std::thread m_Thread;
    std::atomic<bool> m_Running;
    
    mutable std::mutex m_QueueMutex;
    std::condition_variable m_Condition;
    std::queue<LoadRequest> m_LoadQueue;
    
    std::mutex m_CompletionMutex;
    std::vector<CompletionEvent> m_Completions;
};

//------------------------------------------------------------------------------
// Static member initialization
//------------------------------------------------------------------------------
ma_engine* Audio::s_Engine = nullptr;
bool Audio::s_Initialized = false;
float Audio::s_MasterVolume = 1.0f;
AudioHandle Audio::s_NextHandle = 1;
AudioThread* Audio::s_AudioThread = nullptr;

//------------------------------------------------------------------------------
// Internal sound instance tracking
//------------------------------------------------------------------------------
struct SoundInstance {
    ma_sound sound;
    bool is3D = false;
    bool isActive = false;
    bool isStreaming = false;
    float volume = 1.0f;  // Store original volume for master volume changes
};

static std::unordered_map<AudioHandle, std::unique_ptr<SoundInstance>> s_ActiveSounds;
static std::unordered_map<std::string, std::vector<uint8_t>> s_PreloadedData;
static std::mutex s_AudioMutex;

// Listener state
static glm::vec3 s_ListenerPosition = glm::vec3(0.0f);
static glm::vec3 s_ListenerForward = glm::vec3(0.0f, 0.0f, -1.0f);
static glm::vec3 s_ListenerUp = glm::vec3(0.0f, 1.0f, 0.0f);

//------------------------------------------------------------------------------
// Audio Thread Implementation
//------------------------------------------------------------------------------
void AudioThread::ThreadFunc() {
    while (m_Running) {
        LoadRequest request;
        
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_Condition.wait(lock, [this] { 
                return !m_LoadQueue.empty() || !m_Running; 
            });
            
            if (!m_Running && m_LoadQueue.empty()) break;
            if (m_LoadQueue.empty()) continue;
            
            request = std::move(m_LoadQueue.front());
            m_LoadQueue.pop();
        }
        
        // Load the audio file (this is the slow part we want off main thread)
        bool success = false;
        auto instance = std::make_unique<SoundInstance>();
        instance->is3D = request.is3D;
        instance->volume = request.volume;
        instance->isStreaming = request.isStreaming;
        
        ma_uint32 flags = request.isStreaming ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        if (!request.is3D) {
            flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        }
        
        ma_result result = ma_sound_init_from_file(
            Audio::s_Engine, 
            request.path.c_str(), 
            flags, 
            nullptr, 
            nullptr, 
            &instance->sound
        );
        
        if (result == MA_SUCCESS) {
            // Configure sound
            float effectiveVolume = request.volume * Audio::s_MasterVolume;
            ma_sound_set_volume(&instance->sound, effectiveVolume);
            ma_sound_set_looping(&instance->sound, request.loop ? MA_TRUE : MA_FALSE);
            
            if (request.is3D) {
                ma_sound_set_spatialization_enabled(&instance->sound, MA_TRUE);
                ma_sound_set_position(&instance->sound, request.position.x, request.position.y, request.position.z);
                ma_sound_set_min_distance(&instance->sound, request.minDistance);
                ma_sound_set_max_distance(&instance->sound, request.maxDistance);
                ma_sound_set_attenuation_model(&instance->sound, ma_attenuation_model_inverse);
                ma_sound_set_rolloff(&instance->sound, 1.0f);
            }
            
            // Start playback unless this is just a preload
            if (!request.isPreload) {
                result = ma_sound_start(&instance->sound);
            }
            
            if (result == MA_SUCCESS || request.isPreload) {
                instance->isActive = true;
                
                // Add to active sounds
                {
                    std::lock_guard<std::mutex> lock(s_AudioMutex);
                    s_ActiveSounds[request.handle] = std::move(instance);
                }
                
                success = true;
            } else {
                ma_sound_uninit(&instance->sound);
                std::cerr << "[Audio] Failed to start sound: " << request.path << std::endl;
            }
        } else {
            std::cerr << "[Audio] Failed to load sound: " << request.path 
                      << " (error: " << result << ")" << std::endl;
        }
        
        // Queue completion callback for main thread
        if (request.callback) {
            std::lock_guard<std::mutex> lock(m_CompletionMutex);
            m_Completions.push_back({ request.handle, success, request.callback });
        }
    }
}

//------------------------------------------------------------------------------
// Initialization / Shutdown
//------------------------------------------------------------------------------

void Audio::Init() {
    if (s_Initialized) {
        std::cout << "[Audio] Already initialized." << std::endl;
        return;
    }

    std::cout << "[Audio] Initializing miniaudio engine..." << std::endl;

    s_Engine = new ma_engine();
    
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;
    engineConfig.sampleRate = 48000;
    engineConfig.listenerCount = 1;

    ma_result result = ma_engine_init(&engineConfig, s_Engine);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to initialize audio engine. Error: " << result << std::endl;
        delete s_Engine;
        s_Engine = nullptr;
        return;
    }

    // Start background loading thread
    s_AudioThread = new AudioThread();
    s_AudioThread->Start();

    s_Initialized = true;
    s_MasterVolume = 1.0f;
    s_NextHandle = 1;

    std::cout << "[Audio] Initialized successfully (with background thread)." << std::endl;
}

void Audio::Shutdown() {
    if (!s_Initialized) return;

    std::cout << "[Audio] Shutting down..." << std::endl;

    // Stop background thread first
    if (s_AudioThread) {
        s_AudioThread->Stop();
        delete s_AudioThread;
        s_AudioThread = nullptr;
    }

    // Stop and cleanup all active sounds
    {
        std::lock_guard<std::mutex> lock(s_AudioMutex);
        for (auto& [handle, instance] : s_ActiveSounds) {
            if (instance && instance->isActive) {
                ma_sound_uninit(&instance->sound);
            }
        }
        s_ActiveSounds.clear();
        s_PreloadedData.clear();
    }

    if (s_Engine) {
        ma_engine_uninit(s_Engine);
        delete s_Engine;
        s_Engine = nullptr;
    }

    s_Initialized = false;
    std::cout << "[Audio] Shutdown complete." << std::endl;
}

void Audio::Update(float deltaTime) {
    if (!s_Initialized) return;

    // Process async load completions (callbacks on main thread)
    if (s_AudioThread) {
        s_AudioThread->ProcessCompletions(s_Engine);
    }

    // Cleanup finished sounds
    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    std::vector<AudioHandle> toRemove;
    for (auto& [handle, instance] : s_ActiveSounds) {
        if (instance && instance->isActive) {
            if (ma_sound_at_end(&instance->sound) && !ma_sound_is_looping(&instance->sound)) {
                ma_sound_uninit(&instance->sound);
                instance->isActive = false;
                toRemove.push_back(handle);
            }
        }
    }

    for (AudioHandle h : toRemove) {
        s_ActiveSounds.erase(h);
    }
}

bool Audio::IsInitialized() {
    return s_Initialized;
}

uint32_t Audio::GetActiveSoundCount() {
    std::lock_guard<std::mutex> lock(s_AudioMutex);
    return static_cast<uint32_t>(s_ActiveSounds.size());
}

uint32_t Audio::GetPendingLoadCount() {
    return s_AudioThread ? s_AudioThread->GetPendingCount() : 0;
}

//------------------------------------------------------------------------------
// Synchronous Playback API
//------------------------------------------------------------------------------

AudioHandle Audio::Play(const std::string& path, float volume, bool loop) {
    if (!s_Initialized || !s_Engine) {
        std::cerr << "[Audio] Cannot play - not initialized." << std::endl;
        return INVALID_AUDIO_HANDLE;
    }

    std::lock_guard<std::mutex> lock(s_AudioMutex);

    auto instance = std::make_unique<SoundInstance>();
    instance->is3D = false;
    instance->volume = volume;

    ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result result = ma_sound_init_from_file(s_Engine, path.c_str(), flags, nullptr, nullptr, &instance->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to load sound: " << path << " (error: " << result << ")" << std::endl;
        return INVALID_AUDIO_HANDLE;
    }

    ma_sound_set_volume(&instance->sound, volume * s_MasterVolume);
    ma_sound_set_looping(&instance->sound, loop ? MA_TRUE : MA_FALSE);

    result = ma_sound_start(&instance->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to start sound: " << path << std::endl;
        ma_sound_uninit(&instance->sound);
        return INVALID_AUDIO_HANDLE;
    }

    instance->isActive = true;
    AudioHandle handle = s_NextHandle++;
    s_ActiveSounds[handle] = std::move(instance);

    return handle;
}

AudioHandle Audio::Play3D(const std::string& path, 
                          const glm::vec3& position, 
                          float volume, 
                          bool loop,
                          float minDistance,
                          float maxDistance) {
    if (!s_Initialized || !s_Engine) {
        std::cerr << "[Audio] Cannot play 3D - not initialized." << std::endl;
        return INVALID_AUDIO_HANDLE;
    }

    std::lock_guard<std::mutex> lock(s_AudioMutex);

    auto instance = std::make_unique<SoundInstance>();
    instance->is3D = true;
    instance->volume = volume;

    ma_uint32 flags = MA_SOUND_FLAG_DECODE;

    ma_result result = ma_sound_init_from_file(s_Engine, path.c_str(), flags, nullptr, nullptr, &instance->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to load 3D sound: " << path << " (error: " << result << ")" << std::endl;
        return INVALID_AUDIO_HANDLE;
    }

    ma_sound_set_volume(&instance->sound, volume * s_MasterVolume);
    ma_sound_set_looping(&instance->sound, loop ? MA_TRUE : MA_FALSE);
    ma_sound_set_spatialization_enabled(&instance->sound, MA_TRUE);
    ma_sound_set_position(&instance->sound, position.x, position.y, position.z);
    ma_sound_set_min_distance(&instance->sound, minDistance);
    ma_sound_set_max_distance(&instance->sound, maxDistance);
    ma_sound_set_attenuation_model(&instance->sound, ma_attenuation_model_inverse);
    ma_sound_set_rolloff(&instance->sound, 1.0f);

    result = ma_sound_start(&instance->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to start 3D sound: " << path << std::endl;
        ma_sound_uninit(&instance->sound);
        return INVALID_AUDIO_HANDLE;
    }

    instance->isActive = true;
    AudioHandle handle = s_NextHandle++;
    s_ActiveSounds[handle] = std::move(instance);

    return handle;
}

//------------------------------------------------------------------------------
// Asynchronous Playback API
//------------------------------------------------------------------------------

AudioHandle Audio::PlayAsync(const std::string& path, 
                              float volume, 
                              bool loop,
                              AudioLoadCallback callback,
                              AudioLoadPriority priority) {
    if (!s_Initialized || !s_AudioThread) {
        if (callback) callback(INVALID_AUDIO_HANDLE, false);
        return INVALID_AUDIO_HANDLE;
    }

    AudioHandle handle = s_NextHandle++;
    
    AudioThread::LoadRequest request;
    request.handle = handle;
    request.path = path;
    request.volume = volume;
    request.loop = loop;
    request.is3D = false;
    request.priority = priority;
    request.callback = callback;
    request.isPreload = false;
    request.isStreaming = false;
    
    s_AudioThread->EnqueueLoad(std::move(request));
    
    return handle;
}

AudioHandle Audio::Play3DAsync(const std::string& path,
                                const glm::vec3& position,
                                float volume,
                                bool loop,
                                float minDistance,
                                float maxDistance,
                                AudioLoadCallback callback,
                                AudioLoadPriority priority) {
    if (!s_Initialized || !s_AudioThread) {
        if (callback) callback(INVALID_AUDIO_HANDLE, false);
        return INVALID_AUDIO_HANDLE;
    }

    AudioHandle handle = s_NextHandle++;
    
    AudioThread::LoadRequest request;
    request.handle = handle;
    request.path = path;
    request.volume = volume;
    request.loop = loop;
    request.is3D = true;
    request.position = position;
    request.minDistance = minDistance;
    request.maxDistance = maxDistance;
    request.priority = priority;
    request.callback = callback;
    request.isPreload = false;
    request.isStreaming = false;
    
    s_AudioThread->EnqueueLoad(std::move(request));
    
    return handle;
}

void Audio::PreloadAsync(const std::string& path, 
                          AudioLoadCallback callback,
                          AudioLoadPriority priority) {
    if (!s_Initialized || !s_AudioThread) {
        if (callback) callback(INVALID_AUDIO_HANDLE, false);
        return;
    }

    AudioHandle handle = s_NextHandle++;
    
    AudioThread::LoadRequest request;
    request.handle = handle;
    request.path = path;
    request.volume = 1.0f;
    request.loop = false;
    request.is3D = false;
    request.priority = priority;
    request.callback = callback;
    request.isPreload = true;
    request.isStreaming = false;
    
    s_AudioThread->EnqueueLoad(std::move(request));
}

//------------------------------------------------------------------------------
// Streaming API
//------------------------------------------------------------------------------

AudioHandle Audio::PlayStreaming(const std::string& path, float volume, bool loop) {
    if (!s_Initialized || !s_Engine) {
        std::cerr << "[Audio] Cannot play streaming - not initialized." << std::endl;
        return INVALID_AUDIO_HANDLE;
    }

    std::lock_guard<std::mutex> lock(s_AudioMutex);

    auto instance = std::make_unique<SoundInstance>();
    instance->is3D = false;
    instance->volume = volume;
    instance->isStreaming = true;

    // Use streaming flag - doesn't load entire file into memory
    ma_uint32 flags = MA_SOUND_FLAG_STREAM | MA_SOUND_FLAG_NO_SPATIALIZATION;

    ma_result result = ma_sound_init_from_file(s_Engine, path.c_str(), flags, nullptr, nullptr, &instance->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to load streaming sound: " << path << " (error: " << result << ")" << std::endl;
        return INVALID_AUDIO_HANDLE;
    }

    ma_sound_set_volume(&instance->sound, volume * s_MasterVolume);
    ma_sound_set_looping(&instance->sound, loop ? MA_TRUE : MA_FALSE);

    result = ma_sound_start(&instance->sound);
    if (result != MA_SUCCESS) {
        std::cerr << "[Audio] Failed to start streaming sound: " << path << std::endl;
        ma_sound_uninit(&instance->sound);
        return INVALID_AUDIO_HANDLE;
    }

    instance->isActive = true;
    AudioHandle handle = s_NextHandle++;
    s_ActiveSounds[handle] = std::move(instance);

    return handle;
}

//------------------------------------------------------------------------------
// Sound Instance Control
//------------------------------------------------------------------------------

void Audio::Stop(AudioHandle handle) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive) {
        ma_sound_stop(&it->second->sound);
        ma_sound_uninit(&it->second->sound);
        it->second->isActive = false;
        s_ActiveSounds.erase(it);
    }
}

void Audio::StopAll() {
    if (!s_Initialized) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    for (auto& [handle, instance] : s_ActiveSounds) {
        if (instance && instance->isActive) {
            ma_sound_stop(&instance->sound);
            ma_sound_uninit(&instance->sound);
            instance->isActive = false;
        }
    }
    s_ActiveSounds.clear();
}

void Audio::Pause(AudioHandle handle) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive) {
        ma_sound_stop(&it->second->sound);
    }
}

void Audio::Resume(AudioHandle handle) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive) {
        ma_sound_start(&it->second->sound);
    }
}

bool Audio::IsPlaying(AudioHandle handle) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return false;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive) {
        return ma_sound_is_playing(&it->second->sound) == MA_TRUE;
    }
    return false;
}

void Audio::SetVolume(AudioHandle handle, float volume) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive) {
        it->second->volume = volume;
        ma_sound_set_volume(&it->second->sound, volume * s_MasterVolume);
    }
}

void Audio::SetPitch(AudioHandle handle, float pitch) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive) {
        ma_sound_set_pitch(&it->second->sound, pitch);
    }
}

void Audio::SetPosition(AudioHandle handle, const glm::vec3& position) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive && it->second->is3D) {
        ma_sound_set_position(&it->second->sound, position.x, position.y, position.z);
    }
}

void Audio::SetVelocity(AudioHandle handle, const glm::vec3& velocity) {
    if (!s_Initialized || handle == INVALID_AUDIO_HANDLE) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    auto it = s_ActiveSounds.find(handle);
    if (it != s_ActiveSounds.end() && it->second && it->second->isActive && it->second->is3D) {
        ma_sound_set_velocity(&it->second->sound, velocity.x, velocity.y, velocity.z);
    }
}

//------------------------------------------------------------------------------
// Listener
//------------------------------------------------------------------------------

void Audio::SetListenerTransform(const glm::vec3& position, 
                                  const glm::vec3& forward, 
                                  const glm::vec3& up) {
    if (!s_Initialized || !s_Engine) return;

    s_ListenerPosition = position;
    s_ListenerForward = forward;
    s_ListenerUp = up;

    ma_engine_listener_set_position(s_Engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(s_Engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(s_Engine, 0, up.x, up.y, up.z);
}

void Audio::SetListenerVelocity(const glm::vec3& velocity) {
    if (!s_Initialized || !s_Engine) return;

    ma_engine_listener_set_velocity(s_Engine, 0, velocity.x, velocity.y, velocity.z);
}

//------------------------------------------------------------------------------
// Global Controls
//------------------------------------------------------------------------------

void Audio::SetMasterVolume(float volume) {
    s_MasterVolume = glm::clamp(volume, 0.0f, 2.0f);
    
    if (s_Initialized && s_Engine) {
        ma_engine_set_volume(s_Engine, s_MasterVolume);
        
        // Update all active sounds with new master volume
        std::lock_guard<std::mutex> lock(s_AudioMutex);
        for (auto& [handle, instance] : s_ActiveSounds) {
            if (instance && instance->isActive) {
                ma_sound_set_volume(&instance->sound, instance->volume * s_MasterVolume);
            }
        }
    }
}

float Audio::GetMasterVolume() {
    return s_MasterVolume;
}

void Audio::PauseAll() {
    if (!s_Initialized) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    for (auto& [handle, instance] : s_ActiveSounds) {
        if (instance && instance->isActive) {
            ma_sound_stop(&instance->sound);
        }
    }
}

void Audio::ResumeAll() {
    if (!s_Initialized) return;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    for (auto& [handle, instance] : s_ActiveSounds) {
        if (instance && instance->isActive) {
            ma_sound_start(&instance->sound);
        }
    }
}

//------------------------------------------------------------------------------
// Resource Management
//------------------------------------------------------------------------------

bool Audio::Preload(const std::string& path) {
    if (!s_Initialized) return false;

    std::lock_guard<std::mutex> lock(s_AudioMutex);
    
    // Check if already preloaded
    if (s_PreloadedData.find(path) != s_PreloadedData.end()) {
        return true;
    }

    // Read file data
    std::vector<uint8_t> data;
    if (FileSystem::Instance().ReadFile(path, data) && !data.empty()) {
        s_PreloadedData[path] = std::move(data);
        return true;
    }

    // Try direct file read (editor-only fallback)
    if (!FileSystem::Instance().IsDiskFallbackAllowed()) {
        std::cerr << "[Audio] Failed to preload from VFS: " << path << std::endl;
        return false;
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        std::cerr << "[Audio] Failed to preload: " << path << std::endl;
        return false;
    }

    fseek(file, 0, SEEK_END);
    size_t size = ftell(file);
    fseek(file, 0, SEEK_SET);

    data.resize(size);
    fread(data.data(), 1, size, file);
    fclose(file);

    s_PreloadedData[path] = std::move(data);
    return true;
}

void Audio::Unload(const std::string& path) {
    std::lock_guard<std::mutex> lock(s_AudioMutex);
    s_PreloadedData.erase(path);
}

void Audio::UnloadAll() {
    std::lock_guard<std::mutex> lock(s_AudioMutex);
    s_PreloadedData.clear();
}
