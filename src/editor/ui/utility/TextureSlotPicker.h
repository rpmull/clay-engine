#pragma once

#include <functional>
#include <string>
#include <vector>
#include <filesystem>

namespace texturepicker {

// Clears any cached thumbnails held by the picker (releases bgfx handles).
void ClearCachedThumbnails();

// Indicates whether bgfx is still active for safe handle destruction.
// Call with false before bgfx::shutdown to prevent teardown crashes.
void SetBgfxActive(bool active);

// Invalidate a specific thumbnail cache entry by path.
// Call this when a texture file is modified to force reload on next display.
void InvalidateThumbnail(const std::string& path);

// Draws the popup that lists available texture assets with thumbnails.
// The popup identifier should match the one passed to ImGui::OpenPopup.
// When the user picks an entry, onSelectPath is invoked with the absolute path
// (falling back to the registered relative path if an absolute path cannot be resolved).
// currentPathHint is optional and is used to highlight the currently assigned texture.
void DrawTexturePickerPopup(const char* popupId,
                            const std::function<void(const std::string&)>& onSelectPath,
                            const std::string& currentPathHint = std::string());

// Overload that also scans additional directories for textures (e.g., engine asset folders).
// Textures found in additionalDirs are included alongside project assets.
void DrawTexturePickerPopup(const char* popupId,
                            const std::function<void(const std::string&)>& onSelectPath,
                            const std::string& currentPathHint,
                            const std::vector<std::filesystem::path>& additionalDirs);

} // namespace texturepicker



