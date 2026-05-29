#include "TextureSlotPicker.h"

#include <imgui.h>
#include <bgfx/bgfx.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "editor/Project.h"
#include "editor/pipeline/AssetLibrary.h"
#include "core/rendering/TextureLoader.h"
#include "core/assets/AssetReference.h"

namespace texturepicker {
namespace {

namespace fs = std::filesystem;

struct ThumbnailEntry {
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    fs::file_time_type timestamp{};
};

struct TextureOption {
    std::string name;
    std::string relPath;
    std::string absPath;
    std::string normRel;
    std::string normAbs;
    std::string normName;
    ClaymoreGUID guid; // For deduplication
};

static std::unordered_map<std::string, ThumbnailEntry> s_ThumbCache;
static std::unordered_map<ImGuiID, std::string> s_SearchBuffers;
static bool s_BgfxActive = true;

static std::string NormalizePathKey(const std::string& path) {
    std::string key = path;
    std::replace(key.begin(), key.end(), '\\', '/');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return key;
}

static std::string ResolveAbsolutePath(const std::string& raw) {
    if (raw.empty()) return raw;
    fs::path path(raw);
    if (path.is_absolute()) {
        return path.lexically_normal().string();
    }
    fs::path project = Project::GetProjectDirectory();
    if (!project.empty()) {
        return (project / path).lexically_normal().string();
    }
    return path.lexically_normal().string();
}

static bool IsTextureExtension(const std::string& ext) {
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return lower == ".png" || lower == ".jpg" || lower == ".jpeg" || lower == ".tga" || lower == ".bmp" || lower == ".hdr";
}

static std::string TruncateTextureLabel(const std::string& text, float wrapWidth, int maxLines) {
    if (text.empty()) return text;
    if (maxLines < 1) maxLines = 1;
    const float maxHeight = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(maxLines);
    ImVec2 fullSize = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size(), false, wrapWidth);
    if (fullSize.y <= maxHeight + 0.1f) {
        return text;
    }

    const std::string ellipsis = "...";
    int low = 0;
    int high = static_cast<int>(text.size());
    int best = 0;
    while (low <= high) {
        int mid = (low + high) / 2;
        std::string candidate = text.substr(0, mid) + ellipsis;
        ImVec2 size = ImGui::CalcTextSize(candidate.c_str(), candidate.c_str() + candidate.size(), false, wrapWidth);
        if (size.y <= maxHeight + 0.1f) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    std::string clipped = text.substr(0, best);
    while (!clipped.empty() && std::isspace(static_cast<unsigned char>(clipped.back()))) {
        clipped.pop_back();
    }
    clipped += ellipsis;
    return clipped;
}

static std::vector<TextureOption> CollectTextureOptions(const std::vector<fs::path>& additionalDirs = {}) {
    auto assets = AssetLibrary::Instance().GetAllAssets();
    std::vector<TextureOption> options;
    options.reserve(assets.size());
    
    // Track seen GUIDs and normalized paths to prevent duplicates
    std::unordered_set<uint64_t> seenGuidHashes;
    std::unordered_set<std::string> seenNormPaths;
    
    // Helper to compute a hash for deduplication by GUID
    auto guidHash = [](const ClaymoreGUID& g) -> uint64_t {
        return g.high ^ g.low;
    };
    
    // Collect from project assets
    for (const auto& tup : assets) {
        const std::string& path = std::get<0>(tup);
        const ClaymoreGUID& guid = std::get<1>(tup);
        AssetType type = std::get<2>(tup);
        if (type != AssetType::Texture) continue;
        
        // Skip if we've already seen this GUID
        uint64_t gh = guidHash(guid);
        if (seenGuidHashes.count(gh)) continue;
        seenGuidHashes.insert(gh);
        
        TextureOption opt;
        opt.guid = guid;
        opt.relPath = path;
        opt.absPath = ResolveAbsolutePath(path);
        opt.name = fs::path(path).stem().string();
        if (opt.name.empty()) opt.name = fs::path(path).filename().string();
        opt.normRel = NormalizePathKey(opt.relPath);
        opt.normAbs = NormalizePathKey(opt.absPath);
        opt.normName = NormalizePathKey(opt.name);
        
        // Also track normalized path to prevent path-based duplicates
        seenNormPaths.insert(opt.normAbs);
        
        options.emplace_back(std::move(opt));
    }
    
    // Collect from additional directories (e.g., engine assets)
    for (const auto& dir : additionalDirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) continue;
        
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            if (!IsTextureExtension(ext)) continue;
            
            TextureOption opt;
            opt.absPath = entry.path().lexically_normal().string();
            opt.relPath = entry.path().filename().string(); // Use filename as relative for engine assets
            opt.name = entry.path().stem().string();
            if (opt.name.empty()) opt.name = entry.path().filename().string();
            opt.normRel = NormalizePathKey(opt.relPath);
            opt.normAbs = NormalizePathKey(opt.absPath);
            opt.normName = NormalizePathKey(opt.name);
            
            // Skip duplicates by normalized absolute path
            if (seenNormPaths.count(opt.normAbs)) continue;
            seenNormPaths.insert(opt.normAbs);
            
            options.emplace_back(std::move(opt));
        }
    }
    
    std::sort(options.begin(), options.end(), [](const TextureOption& a, const TextureOption& b) {
        return a.name < b.name;
    });
    return options;
}

static ImTextureID FetchThumbnail(const std::string& absPath) {
    if (absPath.empty()) return 0;
    std::error_code ec;
    if (!fs::exists(absPath, ec)) {
        auto key = NormalizePathKey(absPath);
        auto it = s_ThumbCache.find(key);
        if (it != s_ThumbCache.end()) {
            if (bgfx::isValid(it->second.handle)) {
                bgfx::destroy(it->second.handle);
            }
            s_ThumbCache.erase(it);
        }
        return 0;
    }
    fs::file_time_type stamp = fs::last_write_time(absPath, ec);
    auto key = NormalizePathKey(absPath);
    auto it = s_ThumbCache.find(key);
    if (it != s_ThumbCache.end()) {
        bool stale = !bgfx::isValid(it->second.handle);
        if (!stale && stamp != fs::file_time_type{} && it->second.timestamp != stamp) {
            stale = true;
        }
        if (!stale) {
            return TextureLoader::ToImGuiTextureID(it->second.handle);
        }
        if (bgfx::isValid(it->second.handle)) {
            bgfx::destroy(it->second.handle);
        }
        s_ThumbCache.erase(it);
    }
    bgfx::TextureHandle handle = TextureLoader::Load2D(absPath);
    if (!bgfx::isValid(handle)) {
        return 0;
    }
    ThumbnailEntry entry;
    entry.handle = handle;
    entry.timestamp = stamp;
    s_ThumbCache[key] = entry;
    return TextureLoader::ToImGuiTextureID(handle);
}

static bool PathMatchesCurrent(const TextureOption& opt, const std::string& normalizedCurrent) {
    if (normalizedCurrent.empty()) return false;
    return normalizedCurrent == opt.normAbs || normalizedCurrent == opt.normRel;
}

static void DrawSearchField(ImGuiID searchId, std::string& buffer) {
    (void)searchId;
    char temp[128];
    std::strncpy(temp, buffer.c_str(), sizeof(temp));
    temp[sizeof(temp) - 1] = '\0';
    if (ImGui::InputTextWithHint("##texture_picker_search", "Search textures...", temp, sizeof(temp))) {
        buffer = temp;
    }
}

static void DrawTextureOptionGrid(const std::vector<const TextureOption*>& filtered,
                                  const std::string& normalizedCurrent,
                                  bool useAbsoluteTooltip,
                                  const std::function<void(const std::string&)>& onSelectPath) {
    constexpr float kThumbSize = 64.0f;
    constexpr float kTileMinWidth = 112.0f;
    constexpr float kTileSpacingX = 12.0f;
    constexpr float kTileSpacingY = 12.0f;

    const float availableWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const int columns = std::max(1, static_cast<int>(std::floor((availableWidth + kTileSpacingX) / (kTileMinWidth + kTileSpacingX))));
    const float tileWidth = std::max(kTileMinWidth, (availableWidth - kTileSpacingX * (columns - 1)) / columns);
    const float labelHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kTileSpacingX, kTileSpacingY));
    if (ImGui::BeginTable("##TexturePickerGrid", columns, ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX)) {
        for (const TextureOption* opt : filtered) {
            ImGui::TableNextColumn();

            if (opt->guid.high != 0 || opt->guid.low != 0) {
                ImGui::PushID(static_cast<int>(opt->guid.high ^ opt->guid.low));
            } else {
                ImGui::PushID(opt->normAbs.c_str());
            }

            ImGui::BeginGroup();
            const float groupStartX = ImGui::GetCursorPosX();
            const float previewIndent = std::max(0.0f, (tileWidth - kThumbSize) * 0.5f);
            if (previewIndent > 0.0f) {
                ImGui::SetCursorPosX(groupStartX + previewIndent);
            }

            bool picked = false;
            ImTextureID thumb = FetchThumbnail(opt->absPath);
            const ImVec2 previewSize(kThumbSize, kThumbSize);
            const std::string& resolvedPath = !opt->absPath.empty() ? opt->absPath : opt->relPath;
            if (thumb) {
                if (ImGui::ImageButton("##thumb", thumb, previewSize)) {
                    picked = true;
                }
            } else {
                if (ImGui::Button("No Preview", previewSize)) {
                    picked = true;
                }
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", useAbsoluteTooltip ? opt->absPath.c_str() : opt->relPath.c_str());
            }

            const bool isCurrent = PathMatchesCurrent(*opt, normalizedCurrent);
            ImGui::SetCursorPosX(groupStartX);
            const ImVec2 labelStart = ImGui::GetCursorPos();
            const std::string clippedName = TruncateTextureLabel(opt->name, tileWidth - 4.0f, 2);
            ImGui::PushTextWrapPos(labelStart.x + tileWidth - 4.0f);
            if (isCurrent) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.78f, 0.38f, 1.0f));
            }
            ImGui::TextUnformatted(clippedName.c_str());
            if (isCurrent) {
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();

            const float usedLabelHeight = ImGui::GetCursorPosY() - labelStart.y;
            const float remainingHeight = std::max(0.0f, labelHeight - usedLabelHeight);
            ImGui::Dummy(ImVec2(tileWidth, remainingHeight));
            ImGui::EndGroup();

            if (picked && onSelectPath) {
                onSelectPath(resolvedPath);
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

} // namespace

void SetBgfxActive(bool active) {
    s_BgfxActive = active;
}

void ClearCachedThumbnails() {
    if (s_BgfxActive) {
        for (auto& kv : s_ThumbCache) {
            if (bgfx::isValid(kv.second.handle)) {
                bgfx::destroy(kv.second.handle);
            }
        }
    }
    s_ThumbCache.clear();
    s_SearchBuffers.clear();
}

void InvalidateThumbnail(const std::string& path) {
    if (path.empty()) return;
    
    // Try normalized path
    std::string key = NormalizePathKey(path);
    auto it = s_ThumbCache.find(key);
    if (it != s_ThumbCache.end()) {
        if (bgfx::isValid(it->second.handle)) {
            bgfx::destroy(it->second.handle);
        }
        s_ThumbCache.erase(it);
    }
    
    // Also try absolute resolved path
    std::string absPath = ResolveAbsolutePath(path);
    if (absPath != path) {
        key = NormalizePathKey(absPath);
        it = s_ThumbCache.find(key);
        if (it != s_ThumbCache.end()) {
            if (bgfx::isValid(it->second.handle)) {
                bgfx::destroy(it->second.handle);
            }
            s_ThumbCache.erase(it);
        }
    }
}

void DrawTexturePickerPopup(const char* popupId,
                            const std::function<void(const std::string&)>& onSelectPath,
                            const std::string& currentPathHint) {
    if (!popupId) return;
    ImGui::SetNextWindowSize(ImVec2(500.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup(popupId)) {
        return;
    }

    ImGuiID popupKey = ImGui::GetID(popupId);
    std::string& searchBuffer = s_SearchBuffers[popupKey];
    DrawSearchField(popupKey, searchBuffer);
    ImGui::Separator();

    std::string normalizedCurrent = NormalizePathKey(currentPathHint);

    auto options = CollectTextureOptions();
    if (options.empty()) {
        ImGui::TextDisabled("No texture assets registered.");
        ImGui::EndPopup();
        return;
    }

    std::string searchLower = NormalizePathKey(searchBuffer);
    std::vector<const TextureOption*> filtered;
    filtered.reserve(options.size());
    for (const auto& opt : options) {
        if (searchLower.empty()) {
            filtered.push_back(&opt);
            continue;
        }
        if (opt.normRel.find(searchLower) != std::string::npos ||
            opt.normAbs.find(searchLower) != std::string::npos ||
            opt.normName.find(searchLower) != std::string::npos) {
            filtered.push_back(&opt);
        }
    }

    if (filtered.empty()) {
        ImGui::TextDisabled("No textures matching '%s'.", searchBuffer.c_str());
        ImGui::EndPopup();
        return;
    }

    DrawTextureOptionGrid(filtered, normalizedCurrent, false, onSelectPath);

    ImGui::EndPopup();
}

void DrawTexturePickerPopup(const char* popupId,
                            const std::function<void(const std::string&)>& onSelectPath,
                            const std::string& currentPathHint,
                            const std::vector<std::filesystem::path>& additionalDirs) {
    if (!popupId) return;
    ImGui::SetNextWindowSize(ImVec2(500.0f, 420.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup(popupId)) {
        return;
    }

    ImGuiID popupKey = ImGui::GetID(popupId);
    std::string& searchBuffer = s_SearchBuffers[popupKey];
    DrawSearchField(popupKey, searchBuffer);
    ImGui::Separator();

    std::string normalizedCurrent = NormalizePathKey(currentPathHint);

    auto options = CollectTextureOptions(additionalDirs);
    if (options.empty()) {
        ImGui::TextDisabled("No texture assets found.");
        ImGui::EndPopup();
        return;
    }

    std::string searchLower = NormalizePathKey(searchBuffer);
    std::vector<const TextureOption*> filtered;
    filtered.reserve(options.size());
    for (const auto& opt : options) {
        if (searchLower.empty()) {
            filtered.push_back(&opt);
            continue;
        }
        if (opt.normRel.find(searchLower) != std::string::npos ||
            opt.normAbs.find(searchLower) != std::string::npos ||
            opt.normName.find(searchLower) != std::string::npos) {
            filtered.push_back(&opt);
        }
    }

    if (filtered.empty()) {
        ImGui::TextDisabled("No textures matching '%s'.", searchBuffer.c_str());
        ImGui::EndPopup();
        return;
    }

    DrawTextureOptionGrid(filtered, normalizedCurrent, true, onSelectPath);

    ImGui::EndPopup();
}

} // namespace texturepicker

