#pragma once

#include "core/assets/AssetReference.h"

#include <string>
#include <vector>

namespace ui {

struct AudioAssetOption {
    std::string name;
    std::string path;
    ClaymoreGUID guid;
    std::string guidString;
};

// Returns a cached list of all audio files (.wav, .mp3, .ogg, .flac) under the project's asset directory.
// The cache automatically rebuilds when the asset root changes or after file system updates.
const std::vector<AudioAssetOption>& GetAudioAssetOptions();

// Explicitly clears the cached list so the next call forces a rescan.
void InvalidateAudioAssetOptions();

} // namespace ui

