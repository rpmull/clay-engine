#include "Project.h"
#include "editor/ProjectGenerator.h"
#include "editor/pipeline/BinaryAssetCache.h"
#include "core/particles/SpriteLoader.h"
#include "core/rendering/MaterialCache.h"
#include "core/rendering/TextureLoader.h"
#include "core/vfs/FileSystem.h"
#include "core/physics/PhysicsLayerManager.h"
#include <fstream>
#include <iostream>
#include "editor/application.h"
#include "editor/pipeline/AssetWatcher.h"
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {
EditorColorScheme ParseEditorColorScheme(const std::string& value) {
    if (value == "solarized") return EditorColorScheme::Solarized;
    if (value == "dark") return EditorColorScheme::Dark;
    if (value == "light") return EditorColorScheme::Light;
    if (value == "classic") return EditorColorScheme::Classic;
    if (value == "medieval") return EditorColorScheme::Medieval;
    if (value == "custom") return EditorColorScheme::Custom;
    return EditorColorScheme::Claymore;
}

const char* ToEditorColorSchemeString(EditorColorScheme value) {
    switch (value) {
        case EditorColorScheme::Solarized: return "solarized";
        case EditorColorScheme::Dark: return "dark";
        case EditorColorScheme::Light: return "light";
        case EditorColorScheme::Classic: return "classic";
        case EditorColorScheme::Medieval: return "medieval";
        case EditorColorScheme::Custom: return "custom";
        case EditorColorScheme::Claymore:
        default: return "claymore";
    }
}

ColorRGBA ParseColor(const json& parent, const char* key, const ColorRGBA& fallback) {
    if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() < 3) {
        return fallback;
    }
    ColorRGBA value = fallback;
    const auto& arr = parent[key];
    value.r = arr[0].get<float>();
    value.g = arr[1].get<float>();
    value.b = arr[2].get<float>();
    value.a = arr.size() > 3 ? arr[3].get<float>() : fallback.a;
    return value;
}

json ToColorJson(const ColorRGBA& value) {
    return json::array({value.r, value.g, value.b, value.a});
}

std::string TrimWhitespace(std::string value) {
    const auto isWhitespace = [](unsigned char ch) { return std::isspace(ch) != 0; };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
        return !isWhitespace(static_cast<unsigned char>(ch));
    }));

    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
        return !isWhitespace(static_cast<unsigned char>(ch));
    }).base(), value.end());

    return value;
}

bool NormalizeProjectName(const std::string& value, std::string& normalized, std::string& error) {
    normalized = TrimWhitespace(value);
    if (normalized.empty()) {
        error = "Project name cannot be empty.";
        return false;
    }

    if (normalized == "." || normalized == "..") {
        error = "Project name cannot be '.' or '..'.";
        return false;
    }

    constexpr const char* kInvalidCharacters = "<>:\"/\\|?*";
    if (normalized.find_first_of(kInvalidCharacters) != std::string::npos) {
        error = "Project name contains characters that are not valid in file names.";
        return false;
    }

    return true;
}

bool IsValidResolutionPreset(const ViewportResolutionPreset& preset) {
    return !preset.label.empty() && preset.width > 0 && preset.height > 0;
}

void MoveGeneratedFileIfPresent(const std::filesystem::path& sourcePath,
                                const std::filesystem::path& targetPath,
                                const char* label) {
    if (sourcePath.empty() || sourcePath == targetPath) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(sourcePath, ec)) {
        return;
    }

    ec.clear();
    if (std::filesystem::exists(targetPath, ec)) {
        ec.clear();
        std::filesystem::remove(sourcePath, ec);
        if (ec) {
            std::cerr << "[Project] Failed to remove stale " << label << ": " << sourcePath
                      << " (" << ec.message() << ")" << std::endl;
        }
        return;
    }

    ec.clear();
    std::filesystem::rename(sourcePath, targetPath, ec);
    if (ec) {
        std::cerr << "[Project] Failed to rename " << label << " from " << sourcePath
                  << " to " << targetPath << " (" << ec.message() << ")" << std::endl;
    }
}
}

bool Project::Load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "[Project] File does not exist: " << path << std::endl;
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Project] Failed to open project file: " << path << std::endl;
        return false;
    }

    json j;
    try {
        file >> j;
    }
    catch (const json::parse_error& e) {
        std::cerr << "[Project] JSON parse error: " << e.what() << std::endl;
        return false;
    }

    s_ProjectFile = path;
    s_ProjectDir = path.parent_path();
    s_ProjectName = j.value("name", "UnnamedProject");

    // assetDirectory is relative to .clayproj location
    std::string relAssetPath = j.value("assetDirectory", "assets");
    s_AssetDir = s_ProjectDir / relAssetPath;

    // Keep filesystem-relative asset resolution aligned with the active project,
    // even when switching projects after the editor has already started.
    FileSystem::Instance().SetProjectRoot(s_ProjectDir);
    BinaryAssetCache::Instance().Initialize(s_ProjectDir);
    TextureLoader::ResetPathCaches();
    InvalidateAllTextureCaches();
    particles::ClearSpriteCache();

    // Modules
    s_Modules.clear();
    if (j.contains("modules") && j["modules"].is_array()) {
        for (const auto& m : j["modules"]) {
            ProjectModuleRef r;
            r.id = m.value("id", std::string());
            r.dll = m.value("dll", std::string());
            r.enabled = m.value("enabled", false);
            if (!r.id.empty()) s_Modules.push_back(std::move(r));
        }
    }

    // Optional: tags and layers (project-defined)
    s_Tags.clear();
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const auto& t : j["tags"]) s_Tags.push_back(t.get<std::string>());
    }
    if (s_Tags.empty()) s_Tags.push_back("Untagged");

    s_Layers.clear();
    if (j.contains("layers") && j["layers"].is_array()) {
        for (const auto& t : j["layers"]) s_Layers.push_back(t.get<std::string>());
    }
    if (s_Layers.empty()) s_Layers.push_back("Default");

    // Physics layers (project-defined; index = position). Apply to the global
    // PhysicsLayerManager so name->index resolution matches what was authored.
    // Absent (older projects) -> reset to the built-in defaults.
    {
        std::vector<std::string> physicsLayers;
        if (j.contains("physicsLayers") && j["physicsLayers"].is_array()) {
            for (const auto& t : j["physicsLayers"]) {
                if (t.is_string()) physicsLayers.push_back(t.get<std::string>());
            }
        }
        PhysicsLayers::PhysicsLayerManager::Get().SetLayers(physicsLayers);
    }
    
    // Game cursor settings
    s_CursorSettings = GameCursorSettings{}; // Reset to defaults
    if (j.contains("gameCursor") && j["gameCursor"].is_object()) {
        auto& cursor = j["gameCursor"];
        s_CursorSettings.texturePath = cursor.value("texture", std::string());
        s_CursorSettings.baseScale = cursor.value("scale", 1.0f);
        s_CursorSettings.hotspotX = cursor.value("hotspotX", 0);
        s_CursorSettings.hotspotY = cursor.value("hotspotY", 0);
        s_CursorSettings.useDPIScaling = cursor.value("useDPIScaling", true);
        s_CursorSettings.previewInEditor = cursor.value("previewInEditor", false);
    }
    
    // Default font path for TextRenderer (optional)
    s_DefaultFontPath = j.value("defaultFont", std::string());
    s_ManagedScriptDebuggingEnabled = j.value("managedScriptDebugging", false);
    s_EditorColorScheme = ParseEditorColorScheme(j.value("editorColorScheme", std::string("claymore")));
    s_EditorCustomPalette = EditorCustomPalette{};
    if (j.contains("editorCustomPalette") && j["editorCustomPalette"].is_object()) {
        const auto& p = j["editorCustomPalette"];
        s_EditorCustomPalette.accent = ParseColor(p, "accent", s_EditorCustomPalette.accent);
        s_EditorCustomPalette.accentSoft = ParseColor(p, "accentSoft", s_EditorCustomPalette.accentSoft);
        s_EditorCustomPalette.accentMuted = ParseColor(p, "accentMuted", s_EditorCustomPalette.accentMuted);
        s_EditorCustomPalette.background00 = ParseColor(p, "background00", s_EditorCustomPalette.background00);
        s_EditorCustomPalette.background0 = ParseColor(p, "background0", s_EditorCustomPalette.background0);
        s_EditorCustomPalette.background1 = ParseColor(p, "background1", s_EditorCustomPalette.background1);
        s_EditorCustomPalette.background2 = ParseColor(p, "background2", s_EditorCustomPalette.background2);
        s_EditorCustomPalette.background3 = ParseColor(p, "background3", s_EditorCustomPalette.background3);
        s_EditorCustomPalette.text = ParseColor(p, "text", s_EditorCustomPalette.text);
        s_EditorCustomPalette.textDim = ParseColor(p, "textDim", s_EditorCustomPalette.textDim);
        s_EditorCustomPalette.warning = ParseColor(p, "warning", s_EditorCustomPalette.warning);
    }
    s_ViewportResolutionPresets.clear();
    if (j.contains("viewportResolutionPresets") && j["viewportResolutionPresets"].is_array()) {
        for (const auto& item : j["viewportResolutionPresets"]) {
            if (!item.is_object()) {
                continue;
            }
            ViewportResolutionPreset preset;
            preset.label = item.value("label", std::string());
            preset.width = item.value("width", 0u);
            preset.height = item.value("height", 0u);
            if (IsValidResolutionPreset(preset)) {
                s_ViewportResolutionPresets.push_back(std::move(preset));
            }
        }
    }

    std::cout << "[Project] Loaded: " << s_ProjectName << std::endl;
    std::cout << "[Project] Root: " << s_ProjectDir << std::endl;
    std::cout << "[Project] Assets: " << s_AssetDir << std::endl;
    std::cout << "[Project] Modules: " << s_Modules.size() << std::endl;
    
    // Automatically convert any absolute module paths to relative paths
    ConvertModulePathsToRelative();
    if (!ProjectGenerator::EnsureManagedScriptProject(s_ProjectDir, s_ProjectName, s_Modules)) {
        std::cerr << "[Project] Warning: failed to refresh managed script project metadata." << std::endl;
    }

	// If application not initialized yet, skip notifying systems; main will set up later
	if (Application::HasInstance() && Application::Get().GetAssetWatcher()) {
		Application::Get().GetAssetWatcher()->SetRootPath(s_AssetDir.string());
	}

    return true;
}

bool Project::Save() {
    if (s_ProjectDir.empty()) {
        std::cerr << "[Project] No project directory set. Cannot save." << std::endl;
        return false;
    }

    std::string normalizedProjectName;
    std::string validationError;
    const std::string requestedProjectName = s_ProjectName.empty()
        ? s_ProjectDir.filename().string()
        : s_ProjectName;
    if (!NormalizeProjectName(requestedProjectName, normalizedProjectName, validationError)) {
        std::cerr << "[Project] " << validationError << std::endl;
        return false;
    }
    s_ProjectName = normalizedProjectName;

    const std::filesystem::path previousProjectFile = s_ProjectFile;
    const std::string previousProjectStem = previousProjectFile.empty()
        ? std::string()
        : previousProjectFile.stem().string();
    const std::filesystem::path desiredProjectFile = s_ProjectDir / (s_ProjectName + ".clayproj");

    std::error_code ec;
    if (!previousProjectFile.empty() && previousProjectFile != desiredProjectFile) {
        if (std::filesystem::exists(desiredProjectFile, ec)) {
            std::cerr << "[Project] Cannot rename project file because the target already exists: "
                      << desiredProjectFile << std::endl;
            return false;
        }
    }

    json j;
    j["name"] = s_ProjectName;
    j["version"] = 1;
    j["assetDirectory"] = std::filesystem::relative(s_AssetDir, s_ProjectDir).string();

    // Modules
    j["modules"] = json::array();
    for (const auto& m : s_Modules) {
        json jm;
        jm["id"] = m.id;
        
        // Ensure DLL path is relative to project directory
        std::filesystem::path dllPath(m.dll);
        if (dllPath.is_absolute()) {
            // Convert absolute path to relative
            std::error_code ec;
            auto relativePath = std::filesystem::relative(dllPath, s_ProjectDir, ec);
            jm["dll"] = ec ? m.dll : relativePath.string(); // Fallback to original if relative() fails
        } else {
            jm["dll"] = m.dll; // Already relative
        }
        
        jm["enabled"] = m.enabled;
        j["modules"].push_back(std::move(jm));
    }

    // Persist tags and layers if present
    if (!s_Tags.empty()) j["tags"] = s_Tags; else j["tags"] = json::array({"Untagged"});
    if (!s_Layers.empty()) j["layers"] = s_Layers; else j["layers"] = json::array({"Default"});
    // Physics layers (the full ordered list, defaults + user-added). Index matters.
    j["physicsLayers"] = PhysicsLayers::PhysicsLayerManager::Get().GetAllLayers();
    
    // Game cursor settings
    if (!s_CursorSettings.texturePath.empty()) {
        json cursor;
        cursor["texture"] = s_CursorSettings.texturePath;
        cursor["scale"] = s_CursorSettings.baseScale;
        cursor["hotspotX"] = s_CursorSettings.hotspotX;
        cursor["hotspotY"] = s_CursorSettings.hotspotY;
        cursor["useDPIScaling"] = s_CursorSettings.useDPIScaling;
        cursor["previewInEditor"] = s_CursorSettings.previewInEditor;
        j["gameCursor"] = cursor;
    }
    
    if (!s_DefaultFontPath.empty()) {
        j["defaultFont"] = s_DefaultFontPath;
    }
    j["managedScriptDebugging"] = s_ManagedScriptDebuggingEnabled;
    j["editorColorScheme"] = ToEditorColorSchemeString(s_EditorColorScheme);
    json palette;
    palette["accent"] = ToColorJson(s_EditorCustomPalette.accent);
    palette["accentSoft"] = ToColorJson(s_EditorCustomPalette.accentSoft);
    palette["accentMuted"] = ToColorJson(s_EditorCustomPalette.accentMuted);
    palette["background00"] = ToColorJson(s_EditorCustomPalette.background00);
    palette["background0"] = ToColorJson(s_EditorCustomPalette.background0);
    palette["background1"] = ToColorJson(s_EditorCustomPalette.background1);
    palette["background2"] = ToColorJson(s_EditorCustomPalette.background2);
    palette["background3"] = ToColorJson(s_EditorCustomPalette.background3);
    palette["text"] = ToColorJson(s_EditorCustomPalette.text);
    palette["textDim"] = ToColorJson(s_EditorCustomPalette.textDim);
    palette["warning"] = ToColorJson(s_EditorCustomPalette.warning);
    j["editorCustomPalette"] = std::move(palette);
    if (!s_ViewportResolutionPresets.empty()) {
        j["viewportResolutionPresets"] = json::array();
        for (const auto& preset : s_ViewportResolutionPresets) {
            if (!IsValidResolutionPreset(preset)) {
                continue;
            }
            json entry;
            entry["label"] = preset.label;
            entry["width"] = preset.width;
            entry["height"] = preset.height;
            j["viewportResolutionPresets"].push_back(std::move(entry));
        }
    }

    std::ofstream out(desiredProjectFile);
    if (!out) {
        std::cerr << "[Project] Failed to save project to: " << desiredProjectFile << std::endl;
        return false;
    }

    out << j.dump(4);
    out.close();
    if (!out) {
        std::cerr << "[Project] Failed while writing project file: " << desiredProjectFile << std::endl;
        return false;
    }

    s_ProjectFile = desiredProjectFile;
    if (!previousProjectFile.empty() && previousProjectFile != desiredProjectFile) {
        ec.clear();
        std::filesystem::remove(previousProjectFile, ec);
        if (ec) {
            std::cerr << "[Project] Warning: failed to remove old project file " << previousProjectFile
                      << " (" << ec.message() << ")" << std::endl;
        }
    } else if (previousProjectFile.empty()) {
        std::cout << "[Project] No project file loaded. Creating default: " << s_ProjectFile << std::endl;
    }

    if (!previousProjectStem.empty() && previousProjectStem != s_ProjectName) {
        MoveGeneratedFileIfPresent(s_ProjectDir / (previousProjectStem + ".csproj"),
                                   s_ProjectDir / (s_ProjectName + ".csproj"),
                                   "managed script project");
        MoveGeneratedFileIfPresent(s_ProjectDir / (previousProjectStem + ".sln"),
                                   s_ProjectDir / (s_ProjectName + ".sln"),
                                   "solution");
    }

    std::cout << "[Project] Saved: " << s_ProjectFile << std::endl;
    if (!ProjectGenerator::EnsureManagedScriptProject(s_ProjectDir, s_ProjectName, s_Modules)) {
        std::cerr << "[Project] Warning: failed to refresh managed script project metadata." << std::endl;
    }

    return true;
}

bool Project::RenameProject(const std::string& name) {
    std::string normalizedProjectName;
    std::string validationError;
    if (!NormalizeProjectName(name, normalizedProjectName, validationError)) {
        std::cerr << "[Project] " << validationError << std::endl;
        return false;
    }

    if (normalizedProjectName == s_ProjectName) {
        return true;
    }

    const std::string previousProjectName = s_ProjectName;
    s_ProjectName = normalizedProjectName;
    if (Save()) {
        return true;
    }

    s_ProjectName = previousProjectName;
    return false;
}

// Accessors
const std::filesystem::path& Project::GetProjectDirectory() {
    return s_ProjectDir;
}

const std::filesystem::path& Project::GetAssetDirectory() {
    return s_AssetDir;
}

const std::string& Project::GetProjectName() {
    return s_ProjectName;
}

const std::filesystem::path& Project::GetProjectFile() {
    return s_ProjectFile;
}

void Project::SetModuleEnabled(const std::string& id, bool enabled) {
    for (auto& m : s_Modules) {
        if (m.id == id) { m.enabled = enabled; return; }
    }
    // Not found: create a disabled record by default path
    ProjectModuleRef r; r.id = id; r.dll = std::string(); r.enabled = enabled; s_Modules.push_back(std::move(r));
}

void Project::SetOrAddModule(const ProjectModuleRef& m) {
    for (auto& e : s_Modules) {
        if (e.id == m.id) { e = m; return; }
    }
    s_Modules.push_back(m);
}

void Project::ConvertModulePathsToRelative() {
    bool changed = false;
    for (auto& module : s_Modules) {
        std::filesystem::path dllPath(module.dll);
        if (dllPath.is_absolute()) {
            std::error_code ec;
            auto relativePath = std::filesystem::relative(dllPath, s_ProjectDir, ec);
            if (!ec) {
                std::cout << "[Project] Converting module path from absolute to relative:\n";
                std::cout << "  Old: " << module.dll << "\n";
                std::cout << "  New: " << relativePath.string() << "\n";
                module.dll = relativePath.string();
                changed = true;
            }
        }
    }
    if (changed) {
        std::cout << "[Project] Module paths converted to relative. Saving project file.\n";
        Save();
    } else {
        std::cout << "[Project] All module paths are already relative.\n";
    }
}
