#pragma once

#include "core/assets/AssetReference.h"

#include <string>
#include <vector>

namespace ui {

struct DialogueLibraryAssetOption {
    std::string name;
    std::string path;
    ClaymoreGUID guid;
    std::string guidString;
};

// Returns a cached list of all dialogue library files (.dlglib) under the project's asset directory.
const std::vector<DialogueLibraryAssetOption>& GetDialogueLibraryAssetOptions();

// Explicitly clears the cached list so the next call forces a rescan.
void InvalidateDialogueLibraryAssetOptions();

} // namespace ui
