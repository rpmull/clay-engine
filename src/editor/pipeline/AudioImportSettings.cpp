// AudioImportSettings.cpp
// Audio file info extraction using miniaudio

#include "AudioImportSettings.h"
#include <miniaudio.h>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

AudioFileInfo GetAudioFileInfo(const std::string& path) {
    AudioFileInfo info;
    
    if (!fs::exists(path)) {
        std::cerr << "[AudioImport] File not found: " << path << std::endl;
        return info;
    }
    
    // Get file size
    info.fileSizeBytes = fs::file_size(path);
    
    // Determine format from extension
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    if (ext == ".wav") info.format = "WAV";
    else if (ext == ".mp3") info.format = "MP3";
    else if (ext == ".ogg") info.format = "OGG";
    else if (ext == ".flac") info.format = "FLAC";
    else info.format = "Unknown";
    
    // Use miniaudio decoder to get file info
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    
    ma_result result = ma_decoder_init_file(path.c_str(), &config, &decoder);
    if (result != MA_SUCCESS) {
        std::cerr << "[AudioImport] Failed to decode file info: " << path 
                  << " (error: " << result << ")" << std::endl;
        return info;
    }
    
    // Extract info
    info.sampleRate = decoder.outputSampleRate;
    info.channels = decoder.outputChannels;
    
    // Get total frame count
    ma_uint64 frameCount;
    result = ma_decoder_get_length_in_pcm_frames(&decoder, &frameCount);
    if (result == MA_SUCCESS) {
        info.totalFrames = frameCount;
        info.durationSeconds = static_cast<float>(frameCount) / static_cast<float>(info.sampleRate);
    }
    
    ma_decoder_uninit(&decoder);
    
    info.valid = true;
    return info;
}
