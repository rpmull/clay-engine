#include "ui/utility/DialogueLibraryAssetListCache.h"

#include "ui/utility/ProjectAssetIndex.h"

#include "editor/pipeline/AssetLibrary.h"

#include <mutex>

namespace ui {
namespace {

std::vector<DialogueLibraryAssetOption> g_dialogueOptions;
std::mutex g_dialogueMutex;

} // namespace

const std::vector<DialogueLibraryAssetOption>& GetDialogueLibraryAssetOptions()
{
    std::lock_guard<std::mutex> lock(g_dialogueMutex);
    const auto& entries = GetProjectAssetEntries(MakeExtensionQuery({ ".dlglib" }));
    if (g_dialogueOptions.size() != entries.size()) {
        g_dialogueOptions.clear();
        g_dialogueOptions.reserve(entries.size());
        for (const ProjectAssetEntry& entry : entries) {
            const ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(entry.absolutePath);
            g_dialogueOptions.push_back({ entry.name, entry.absolutePath, guid, guid.ToString() });
        }
        return g_dialogueOptions;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(entries[i].absolutePath);
        const std::string guidString = guid.ToString();
        if (g_dialogueOptions[i].name != entries[i].name ||
            g_dialogueOptions[i].path != entries[i].absolutePath ||
            g_dialogueOptions[i].guid != guid ||
            g_dialogueOptions[i].guidString != guidString) {
            g_dialogueOptions.clear();
            g_dialogueOptions.reserve(entries.size());
            for (const ProjectAssetEntry& entry : entries) {
                const ClaymoreGUID refreshedGuid = AssetLibrary::Instance().GetGUIDForPath(entry.absolutePath);
                g_dialogueOptions.push_back({ entry.name, entry.absolutePath, refreshedGuid, refreshedGuid.ToString() });
            }
            break;
        }
    }

    return g_dialogueOptions;
}

void InvalidateDialogueLibraryAssetOptions()
{
    std::lock_guard<std::mutex> lock(g_dialogueMutex);
    g_dialogueOptions.clear();
    InvalidateProjectAssetIndex();
}

} // namespace ui
