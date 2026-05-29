#include "ui/utility/AnimationAssetListCache.h"
#include "ui/utility/ProjectAssetIndex.h"

#include <mutex>

namespace ui {
namespace {

std::vector<AnimationAssetOption> g_options;
std::mutex g_mutex;

} // namespace

const std::vector<AnimationAssetOption>& GetAnimationAssetOptions()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto& entries = GetProjectAssetEntries(MakeExtensionQuery({ ".anim" }));
    if (g_options.size() != entries.size()) {
        g_options.clear();
        g_options.reserve(entries.size());
        for (const ProjectAssetEntry& entry : entries) {
            g_options.push_back({ entry.name, entry.absolutePath });
        }
        return g_options;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        if (g_options[i].name != entries[i].name || g_options[i].path != entries[i].absolutePath) {
            g_options.clear();
            g_options.reserve(entries.size());
            for (const ProjectAssetEntry& entry : entries) {
                g_options.push_back({ entry.name, entry.absolutePath });
            }
            break;
        }
    }
    return g_options;
}

void InvalidateAnimationAssetOptions()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_options.clear();
    InvalidateProjectAssetIndex();
}

} // namespace ui


