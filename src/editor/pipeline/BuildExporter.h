#pragma once
#include <string>
#include <vector>
#include <functional>

// BuildExporter: collects required files (scenes, prefabs, textures, models, shaders, scripts)
// and writes a single .pak file alongside a stripped runtime.

class BuildExporter {
public:
    struct Options {
        std::string outputDirectory; // where to place MyGame.exe and MyGame.pak
        std::vector<std::string> entryScenes; // absolute or project-relative scene paths to include
        bool validateBeforeExport = true; // run pre-flight checks
        bool compressPak = true; // compress assets in pak file
        bool pakOnly = false; // build only the pak/manifest (no runtime copy)
        bool allowPartialBinaryBuilds = false; // preview-only: skip non-critical assets whose runtime binaries fail to build
    };

    // Validation result for pre-flight checks
    struct ValidationResult {
        bool success = true;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
        
        void AddError(const std::string& msg) { errors.push_back(msg); success = false; }
        void AddWarning(const std::string& msg) { warnings.push_back(msg); }
        bool HasWarnings() const { return !warnings.empty(); }
    };

    // Build progress callback
    using ProgressCallback = std::function<void(float progress, const std::string& status)>;

    // High-level: export current project as standalone
    static bool ExportProject(const Options& opts, ProgressCallback progress = nullptr);
    
    // Pre-flight validation
    static ValidationResult ValidateBuild(const Options& opts);
    
    static void AddIfExists(const std::string& path,
        std::vector<std::string>& outFiles);
};


