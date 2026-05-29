#pragma once
#include <string>
#include <filesystem>
#include <nlohmann\json.hpp>
#include <vector>

struct ProjectModuleRef {
    std::string id;
    std::string dll;
    bool enabled = false;
};

// Game cursor settings for runtime builds
struct GameCursorSettings {
    std::string texturePath;          // Relative path to cursor texture (PNG)
    float baseScale = 1.0f;           // Base scale factor (1.0 = native DPI size)
    int hotspotX = 0;                 // Cursor hotspot X (pixels in texture space)
    int hotspotY = 0;                 // Cursor hotspot Y (pixels in texture space)
    bool useDPIScaling = true;        // Whether to scale cursor with DPI
    bool previewInEditor = false;     // Whether to show cursor in editor play mode
};

struct ColorRGBA {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct EditorCustomPalette {
    ColorRGBA accent{0.36f, 0.47f, 0.60f, 1.00f};
    ColorRGBA accentSoft{0.44f, 0.53f, 0.65f, 1.00f};
    ColorRGBA accentMuted{0.29f, 0.31f, 0.34f, 1.00f};
    ColorRGBA background00{0.15f, 0.15f, 0.16f, 1.00f};
    ColorRGBA background0{0.18f, 0.18f, 0.19f, 1.00f};
    ColorRGBA background1{0.21f, 0.215f, 0.22f, 1.00f};
    ColorRGBA background2{0.24f, 0.245f, 0.252f, 1.00f};
    ColorRGBA background3{0.30f, 0.305f, 0.315f, 1.00f};
    ColorRGBA text{0.90f, 0.90f, 0.91f, 1.00f};
    ColorRGBA textDim{0.61f, 0.63f, 0.66f, 1.00f};
    ColorRGBA warning{0.90f, 0.60f, 0.29f, 1.00f};
};

enum class EditorColorScheme {
    Claymore = 0,
    Solarized,
    Dark,
    Light,
    Classic,
    Medieval,
    Custom
};

class Project {
public:


    static bool Load(const std::filesystem::path& path);
    static bool Save(); // Optional, if you want to update the .clayproj
    static bool RenameProject(const std::string& name);

    static const std::filesystem::path& GetProjectDirectory();
    static const std::filesystem::path& GetAssetDirectory();
    static const std::string& GetProjectName();
    static const std::filesystem::path& GetProjectFile();

	static void SetProjectDirectory(const std::filesystem::path& path) {
		s_ProjectDir = path;
		s_AssetDir = path / "assets"; // Default asset directory
	}

    // Modules used by this project
    static const std::vector<ProjectModuleRef>& GetModules() { return s_Modules; }
    static void SetModuleEnabled(const std::string& id, bool enabled);
    static void SetOrAddModule(const ProjectModuleRef& m);
    
    // Utility to convert all module paths to relative paths (for migration)
    static void ConvertModulePathsToRelative();

    // Project-defined tags and layer names (used in Inspector dropdowns)
    static const std::vector<std::string>& GetTags() { return s_Tags; }
    static const std::vector<std::string>& GetLayerNames() { return s_Layers; }
    static void SetTags(std::vector<std::string> tags) { s_Tags = std::move(tags); }
    static void SetLayerNames(std::vector<std::string> layers) { s_Layers = std::move(layers); }
    
    // Game cursor settings (for runtime builds)
    static const GameCursorSettings& GetCursorSettings() { return s_CursorSettings; }
    static void SetCursorSettings(const GameCursorSettings& settings) { s_CursorSettings = settings; }
    
    // Default font path for TextRenderer when no override is set
    static const std::string& GetDefaultFontPath() { return s_DefaultFontPath; }
    static void SetDefaultFontPath(const std::string& path) { s_DefaultFontPath = path; }

    // Editor-only managed script debugging support
    static bool GetManagedScriptDebuggingEnabled() { return s_ManagedScriptDebuggingEnabled; }
    static void SetManagedScriptDebuggingEnabled(bool enabled) { s_ManagedScriptDebuggingEnabled = enabled; }

    // Per-project editor color scheme
    static EditorColorScheme GetEditorColorScheme() { return s_EditorColorScheme; }
    static void SetEditorColorScheme(EditorColorScheme scheme) { s_EditorColorScheme = scheme; }
    static const EditorCustomPalette& GetEditorCustomPalette() { return s_EditorCustomPalette; }
    static void SetEditorCustomPalette(const EditorCustomPalette& palette) { s_EditorCustomPalette = palette; }

private:
    static inline std::string s_ProjectName;
    static inline std::filesystem::path s_ProjectFile;
    static inline std::filesystem::path s_ProjectDir;
    static inline std::filesystem::path s_AssetDir;
    static inline std::vector<ProjectModuleRef> s_Modules;
    static inline std::vector<std::string> s_Tags;   // e.g., {"Untagged", "Player", ...}
    static inline std::vector<std::string> s_Layers; // e.g., {"Default", "TransparentFX", ...}
    static inline GameCursorSettings s_CursorSettings;
    static inline std::string s_DefaultFontPath;
    static inline bool s_ManagedScriptDebuggingEnabled = false;
    static inline EditorColorScheme s_EditorColorScheme = EditorColorScheme::Claymore;
    static inline EditorCustomPalette s_EditorCustomPalette{};
};
