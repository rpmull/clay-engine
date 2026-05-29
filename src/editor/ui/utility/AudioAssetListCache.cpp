#include "ui/utility/AudioAssetListCache.h"
#include "ui/utility/ProjectAssetIndex.h"

#include "editor/pipeline/AssetLibrary.h"

#include <mutex>

namespace ui {
namespace {

std::vector<AudioAssetOption> g_audioOptions;
std::mutex g_audioMutex;

} // namespace

const std::vector<AudioAssetOption>& GetAudioAssetOptions()
{
    std::lock_guard<std::mutex> lock(g_audioMutex);
    const auto& entries = GetProjectAssetEntries(MakeExtensionQuery({ ".wav", ".mp3", ".ogg", ".flac" }));
    if (g_audioOptions.size() != entries.size()) {
        g_audioOptions.clear();
        g_audioOptions.reserve(entries.size());
        for (const ProjectAssetEntry& entry : entries) {
            const ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(entry.absolutePath);
            g_audioOptions.push_back({ entry.name, entry.absolutePath, guid, guid.ToString() });
        }
        return g_audioOptions;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(entries[i].absolutePath);
        const std::string guidString = guid.ToString();
        if (g_audioOptions[i].name != entries[i].name ||
            g_audioOptions[i].path != entries[i].absolutePath ||
            g_audioOptions[i].guid != guid ||
            g_audioOptions[i].guidString != guidString) {
            g_audioOptions.clear();
            g_audioOptions.reserve(entries.size());
            for (const ProjectAssetEntry& entry : entries) {
                const ClaymoreGUID refreshedGuid = AssetLibrary::Instance().GetGUIDForPath(entry.absolutePath);
                g_audioOptions.push_back({ entry.name, entry.absolutePath, refreshedGuid, refreshedGuid.ToString() });
            }
            break;
        }
    }
    return g_audioOptions;
}

void InvalidateAudioAssetOptions()
{
    std::lock_guard<std::mutex> lock(g_audioMutex);
    g_audioOptions.clear();
    InvalidateProjectAssetIndex();
}

} // namespace ui

