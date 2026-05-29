// AudioImportSettings.h
// Audio asset import configuration
#pragma once

#include <string>
#include <nlohmann/json.hpp>

/// Audio sample rate options for import
enum class AudioSampleRate {
    Auto = 0,      // Keep original sample rate
    Rate22050 = 22050,
    Rate44100 = 44100,
    Rate48000 = 48000,
    Rate96000 = 96000
};

/// Audio channel configuration
enum class AudioChannels {
    Auto = 0,      // Keep original channels
    Mono = 1,
    Stereo = 2
};

/// Audio loading strategy
enum class AudioLoadType {
    DecompressOnLoad,    // Decode entire file into memory at load time (low latency, high memory)
    CompressedInMemory,  // Keep compressed in memory, decode on play (medium latency, lower memory)
    Streaming            // Stream from disk (high latency, lowest memory) - best for music
};

/// Audio import settings - stored in .meta sidecar files
struct AudioImportSettings {
    // Format conversion
    AudioSampleRate sampleRate = AudioSampleRate::Auto;
    AudioChannels channels = AudioChannels::Auto;
    
    // Loading behavior
    AudioLoadType loadType = AudioLoadType::DecompressOnLoad;
    
    // 3D audio settings
    bool defaultSpatial = false;        // Default spatialization when played
    float defaultMinDistance = 1.0f;
    float defaultMaxDistance = 50.0f;
    float defaultRolloff = 1.0f;
    
    // Normalization
    bool normalize = false;             // Normalize audio levels on import
    float normalizeTargetDb = -3.0f;    // Target dB for normalization
    
    // Quality
    bool preloadOnStartup = false;      // Preload into memory at game start
    int priority = 128;                 // Loading priority (0-255, higher = more important)
    
    // Preview
    float previewVolume = 1.0f;         // Volume for editor preview
};

// JSON serialization
inline void to_json(nlohmann::json& j, const AudioImportSettings& s) {
    j = nlohmann::json{
        {"sampleRate", static_cast<int>(s.sampleRate)},
        {"channels", static_cast<int>(s.channels)},
        {"loadType", static_cast<int>(s.loadType)},
        {"defaultSpatial", s.defaultSpatial},
        {"defaultMinDistance", s.defaultMinDistance},
        {"defaultMaxDistance", s.defaultMaxDistance},
        {"defaultRolloff", s.defaultRolloff},
        {"normalize", s.normalize},
        {"normalizeTargetDb", s.normalizeTargetDb},
        {"preloadOnStartup", s.preloadOnStartup},
        {"priority", s.priority},
        {"previewVolume", s.previewVolume}
    };
}

inline void from_json(const nlohmann::json& j, AudioImportSettings& s) {
    if (j.contains("sampleRate")) s.sampleRate = static_cast<AudioSampleRate>(j["sampleRate"].get<int>());
    if (j.contains("channels")) s.channels = static_cast<AudioChannels>(j["channels"].get<int>());
    if (j.contains("loadType")) s.loadType = static_cast<AudioLoadType>(j["loadType"].get<int>());
    if (j.contains("defaultSpatial")) s.defaultSpatial = j["defaultSpatial"].get<bool>();
    if (j.contains("defaultMinDistance")) s.defaultMinDistance = j["defaultMinDistance"].get<float>();
    if (j.contains("defaultMaxDistance")) s.defaultMaxDistance = j["defaultMaxDistance"].get<float>();
    if (j.contains("defaultRolloff")) s.defaultRolloff = j["defaultRolloff"].get<float>();
    if (j.contains("normalize")) s.normalize = j["normalize"].get<bool>();
    if (j.contains("normalizeTargetDb")) s.normalizeTargetDb = j["normalizeTargetDb"].get<float>();
    if (j.contains("preloadOnStartup")) s.preloadOnStartup = j["preloadOnStartup"].get<bool>();
    if (j.contains("priority")) s.priority = j["priority"].get<int>();
    if (j.contains("previewVolume")) s.previewVolume = j["previewVolume"].get<float>();
}

/// Helper to get audio file info
struct AudioFileInfo {
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint64_t totalFrames = 0;
    float durationSeconds = 0.0f;
    uint64_t fileSizeBytes = 0;
    std::string format;  // "WAV", "MP3", "OGG", "FLAC"
    
    bool valid = false;
};

/// Get info about an audio file without fully loading it
AudioFileInfo GetAudioFileInfo(const std::string& path);
