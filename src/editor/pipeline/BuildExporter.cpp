#include "BuildExporter.h"
#include "BuildDependencyGraph.h"
#include "PakArchive.h"
#include "BinaryAssetCache.h"
#include "core/assets/AssetMetadata.h"
#include "AssetRegistry.h"
#include "AssetPipeline.h"
#include <serialization/Serializer.h>
#include "editor/Project.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include "editor/pipeline/AssetLibrary.h"
#include "core/resources/ResourceManifest.h"
#include "core/assets/AssetReference.h"
#include "editor/pipeline/RuntimeModelManifestWriter.h"
#include "core/vfs/VirtualFS.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

// Forward declare helper to check if shaders are compiled
static bool CheckShadersCompiled();

static bool CopyDirectoryRecursive(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (!fs::exists(src)) return false;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); ++it) {
        const auto& entry = *it;
        fs::path rel = fs::relative(entry.path(), src, ec);
        if (ec) rel = entry.path().filename();
        fs::path outPath = dst / rel;
        if (entry.is_directory()) {
            fs::create_directories(outPath, ec);
        } else if (entry.is_regular_file()) {
            fs::create_directories(outPath.parent_path(), ec);
            std::error_code cec; fs::copy_file(entry.path(), outPath, fs::copy_options::overwrite_existing, cec);
        }
    }
    return true;
}

static bool HasBundledDotnetRuntime(const fs::path& dir) {
    return fs::exists(dir / "dotnet" / "host" / "fxr") &&
           fs::exists(dir / "dotnet" / "shared" / "Microsoft.NETCore.App");
}

static bool HasLegacySelfContainedComponentLayout(const fs::path& dir) {
    return fs::exists(dir / "hostfxr.dll") ||
           fs::exists(dir / "hostpolicy.dll") ||
           fs::exists(dir / "coreclr.dll");
}

static std::string MakeVirtualPath(const fs::path& absPath) {
    // Normalize to a virtual path if possible
    std::string s = IVirtualFS::NormalizePath(absPath.string());
    std::string vpath = VFS::StripToKnownPrefix(s);
    if (!vpath.empty()) {
        return vpath;
    }

    // Otherwise try to make it relative to the project directory
    fs::path proj = Project::GetProjectDirectory();
    std::error_code ec;
    fs::path rel = fs::relative(absPath, proj, ec);
    if (!ec) {
        std::string out = IVirtualFS::NormalizePath(rel.string());
        // Only use if it doesn't go up directories
        if (out.find("../") == std::string::npos) {
            std::string relVpath = VFS::StripToKnownPrefix(out);
            return relVpath.empty() ? out : relVpath;
        }
    }
    
    // Fallback: just return the filename
    return absPath.filename().string();
}

void BuildExporter::AddIfExists(const std::string& path, std::vector<std::string>& outFiles) {
    if (path.empty()) return;
    if (fs::exists(path)) outFiles.push_back(path);
}

static const std::vector<std::string> kExts = {
    ".fbx", ".obj", ".gltf", ".glb",
    ".png", ".jpg", ".jpeg", ".tga",
    ".anim", ".animc", ".avatar", ".controller", ".animctrl", ".animoverride", ".ngraph",
    ".wav", ".mp3", ".ogg",
    ".ttf", ".otf",
    ".cs", ".dll",
    ".mat", ".json", ".prefab", ".ngraph", ".scene",
    ".dlglib", ".dlg",  // Dialogue library and script files
    // Binary asset caches (model import cache + animation controllers)
    // Model .meta paths are normalized to runtime mesh/skeleton binaries at pack time.
    ".terrainbin", ".navbin", ".navpack", ".meshbin", ".skelbin", ".animbin", ".actrlbin", ".modelrt", ".wrapbin"
};

static bool LooksLikeAssetPath(const std::string& s) {
    fs::path p(s);
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return std::find(kExts.begin(), kExts.end(), ext) != kExts.end();
}

static std::string NormalizeSlashes(std::string value) {
    for (char& c : value) {
        if (c == '\\') c = '/';
    }
    return value;
}

static fs::path ResolveProjectPath(const std::string& rawPath) {
    fs::path path(rawPath);
    if (path.is_absolute()) {
        return path.lexically_normal();
    }

    const fs::path& projectDir = Project::GetProjectDirectory();
    if (!projectDir.empty()) {
        return (projectDir / path).lexically_normal();
    }

    return path.lexically_normal();
}

static std::string ToLowerExt(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

static bool IsModelSourceExtension(const std::string& ext) {
    return ext == ".fbx" || ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

static bool IsModelArtifactExtension(const std::string& ext) {
    return ext == ".meshbin" || ext == ".skelbin";
}

static bool IsRuntimeBinaryExtension(const std::string& ext) {
    return ext == ".meshbin" || ext == ".skelbin" || ext == ".modelrt" ||
           ext == ".sceneb" || ext == ".prefabb" || ext == ".matbin" ||
           ext == ".animbin" || ext == ".actrlbin" || ext == ".terrainbin" ||
           ext == ".navbin" || ext == ".navpack" || ext == ".wrapbin";
}

static bool TryReadJsonFile(const fs::path& path, json& outJson) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    try {
        file >> outJson;
        return outJson.is_object();
    } catch (...) {
        return false;
    }
}

static fs::path ResolveMetaRelativePath(const fs::path& metaPath, const std::string& rawPath) {
    fs::path candidate(rawPath);
    if (candidate.is_absolute()) {
        return candidate.lexically_normal();
    }

    fs::path projectResolved = ResolveProjectPath(rawPath);
    if (fs::exists(projectResolved)) {
        return projectResolved.lexically_normal();
    }

    return (metaPath.parent_path() / candidate).lexically_normal();
}

static std::string ResolveModelSourcePathFromMeta(const fs::path& metaPath) {
    json metaJson;
    if (TryReadJsonFile(metaPath, metaJson)) {
        const char* sourceKeys[] = { "source", "sourcePath", "processedPath" };
        for (const char* key : sourceKeys) {
            if (!metaJson.contains(key) || !metaJson[key].is_string()) {
                continue;
            }

            fs::path candidate = ResolveMetaRelativePath(metaPath, metaJson[key].get<std::string>());
            std::string candidateExt = ToLowerExt(candidate);
            if (IsModelSourceExtension(candidateExt) && fs::exists(candidate)) {
                return candidate.string();
            }
        }
    }

    for (const char* candidateExt : { ".fbx", ".gltf", ".glb", ".obj" }) {
        fs::path candidate = metaPath;
        candidate.replace_extension(candidateExt);
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }

    return {};
}

static std::string ResolveModelSourcePath(const std::string& rawPath) {
    if (rawPath.empty()) {
        return {};
    }

    fs::path resolved = ResolveProjectPath(rawPath);
    std::string ext = ToLowerExt(resolved);
    if (IsModelSourceExtension(ext)) {
        return resolved.string();
    }

    if (ext == ".meta") {
        return ResolveModelSourcePathFromMeta(resolved);
    }

    if (!IsModelArtifactExtension(ext)) {
        return {};
    }

    fs::path metaPath = resolved;
    metaPath.replace_extension(".meta");
    return ResolveModelSourcePathFromMeta(metaPath);
}

static bool IsModelMetaPath(const fs::path& path) {
    return ToLowerExt(path) == ".meta" && !ResolveModelSourcePathFromMeta(path).empty();
}

static bool ResolveModelBinaryPaths(const std::string& rawPath,
                                    fs::path& outMeshbin,
                                    fs::path& outSkelbin,
                                    std::string* outSourcePath = nullptr) {
    outMeshbin.clear();
    outSkelbin.clear();
    if (outSourcePath) {
        outSourcePath->clear();
    }

    const fs::path resolved = ResolveProjectPath(rawPath);
    const std::string ext = ToLowerExt(resolved);

    if (ext == ".meshbin") {
        outMeshbin = resolved;
        outSkelbin = resolved;
        outSkelbin.replace_extension(".skelbin");
        if (outSourcePath) {
            *outSourcePath = ResolveModelSourcePath(rawPath);
        }
        return true;
    }

    if (ext == ".skelbin") {
        outSkelbin = resolved;
        outMeshbin = resolved;
        outMeshbin.replace_extension(".meshbin");
        if (outSourcePath) {
            *outSourcePath = ResolveModelSourcePath(rawPath);
        }
        return true;
    }

    std::string sourcePath = ResolveModelSourcePath(rawPath);
    if (sourcePath.empty() && IsModelSourceExtension(ext)) {
        sourcePath = resolved.string();
    }
    if (sourcePath.empty()) {
        return false;
    }

    BuiltModelPaths builtPaths;
    if (!HasModelCache(sourcePath, builtPaths)) {
        EnsureModelCache(sourcePath, builtPaths);
    }

    outMeshbin = builtPaths.meshPath.empty() ? fs::path(sourcePath).replace_extension(".meshbin")
                                             : fs::path(builtPaths.meshPath);
    outSkelbin = builtPaths.skelPath.empty() ? fs::path(sourcePath).replace_extension(".skelbin")
                                             : fs::path(builtPaths.skelPath);
    if (outSourcePath) {
        *outSourcePath = sourcePath;
    }
    return true;
}

static std::string MakeRuntimeAssetMapPath(const std::string& assetPath) {
    if (assetPath.empty()) {
        return {};
    }

    fs::path logicalPath = ResolveProjectPath(assetPath);
    std::string logicalExt = ToLowerExt(logicalPath);
    if (IsModelSourceExtension(logicalExt) ||
        IsModelArtifactExtension(logicalExt) ||
        IsModelMetaPath(logicalPath)) {
        std::string sourcePath = ResolveModelSourcePath(assetPath);
        fs::path basePath = sourcePath.empty() ? logicalPath : fs::path(sourcePath);
        std::string vpath = MakeVirtualPath(basePath);
        return fs::path(vpath).replace_extension(".meshbin").string();
    }

    std::string vpath = MakeVirtualPath(logicalPath);
    auto assetType = BinaryAssetCache::GetAssetType(logicalPath.string());
    if (assetType != BinaryAssetCache::AssetType::Unknown) {
        return fs::path(vpath).replace_extension(BinaryAssetCache::GetBinaryExtension(assetType)).string();
    }

    return vpath;
}

static std::string MakeRuntimeVirtualPath(const fs::path& actualDiskPath, const fs::path& logicalPath) {
    fs::path basePath = logicalPath.empty() ? actualDiskPath : logicalPath;
    std::string logicalExt = ToLowerExt(basePath);
    if (IsModelArtifactExtension(logicalExt) || IsModelMetaPath(basePath)) {
        std::string sourcePath = ResolveModelSourcePath(basePath.string());
        if (!sourcePath.empty()) {
            basePath = fs::path(sourcePath);
            logicalExt = ToLowerExt(basePath);
        }
    }

    std::string vpath = MakeVirtualPath(basePath);
    const std::string actualExt = ToLowerExt(actualDiskPath);
    if (IsRuntimeBinaryExtension(actualExt)) {
        return fs::path(vpath).replace_extension(actualExt).string();
    }

    auto assetType = BinaryAssetCache::GetAssetType(basePath.string());
    if (assetType != BinaryAssetCache::AssetType::Unknown) {
        return fs::path(vpath).replace_extension(BinaryAssetCache::GetBinaryExtension(assetType)).string();
    }

    return vpath;
}

static ClaymoreGUID ResolveAssetGuidForPath(const std::string& path) {
    if (path.empty()) {
        return {};
    }

    const std::string normalized = NormalizeSlashes(path);
    ClaymoreGUID guid = AssetLibrary::Instance().GetGUIDForPath(normalized);
    if (guid != ClaymoreGUID()) {
        return guid;
    }

    fs::path resolved = ResolveProjectPath(path);
    guid = AssetLibrary::Instance().GetGUIDForPath(NormalizeSlashes(resolved.string()));
    if (guid != ClaymoreGUID()) {
        return guid;
    }

    if (const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(normalized)) {
        return meta->guid;
    }
    if (const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(resolved.string())) {
        return meta->guid;
    }

    std::string vpath = MakeVirtualPath(resolved);
    if (!vpath.empty()) {
        fs::path resourcePath(vpath);
        std::string normalizedVpath = NormalizeSlashes(resourcePath.string());
        if (normalizedVpath.find("resources/") == 0) {
            return ResourceManifest::DeterministicGuidFromPath(normalizedVpath);
        }
    }

    return {};
}

// Get binary version of a source asset if available (for runtime export)
static std::string GetBinaryVersionIfExists(const std::string& sourcePath,
                                            std::vector<std::string>* outExtraFiles,
                                            std::string* outError) {
    fs::path p(sourcePath);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto type = BinaryAssetCache::GetAssetType(sourcePath);
    if (type != BinaryAssetCache::AssetType::Unknown) {
        if (!BinaryAssetCache::Instance().EnsureBinary(sourcePath)) {
            if (outError) {
                *outError = "Failed to build binary for asset: " + sourcePath;
            }
            return {};
        }
        std::string binaryPath = BinaryAssetCache::Instance().GetBinaryPath(sourcePath);
        if (!binaryPath.empty() && fs::exists(binaryPath)) {
            return binaryPath;
        }
        if (outError) {
            *outError = "Failed to build binary for asset: " + sourcePath;
        }
        return {};
    }

    // Model source/.meta/.meshbin/.skelbin -> meshbin + skelbin
    if (IsModelSourceExtension(ext) || IsModelArtifactExtension(ext) || IsModelMetaPath(p)) {
        fs::path meshbin;
        fs::path skelbin;
        if (!ResolveModelBinaryPaths(sourcePath, meshbin, skelbin)) {
            if (outError) {
                *outError = "Failed to resolve model binaries for asset: " + sourcePath;
            }
            return {};
        }
        if (!fs::exists(meshbin)) {
            if (outError) {
                *outError = "Missing meshbin for model asset: " + sourcePath;
            }
            return {};
        }
        if (!fs::exists(skelbin)) {
            if (outError) {
                *outError = "Missing skelbin for model asset: " + sourcePath;
            }
            return {};
        }
        if (outExtraFiles) {
            outExtraFiles->push_back(skelbin.string());
        }
        return meshbin.string();
    }

    return sourcePath; // No binary version, use source
}

// Get the virtual path for a binary file (maps binary back to source-style path)
static std::string MakeVirtualPathForBinary(const fs::path& binaryPath, const fs::path& sourcePath) {
    return MakeRuntimeVirtualPath(binaryPath, sourcePath);
}

// ---------------------------------------------------------------------------
// Pre-flight validation
// ---------------------------------------------------------------------------
BuildExporter::ValidationResult BuildExporter::ValidateBuild(const Options& opts) {
    ValidationResult result;
    
    // 1. Check entry scene exists
    if (opts.entryScenes.empty()) {
        result.AddError("No entry scene specified.");
    } else {
        for (const auto& scene : opts.entryScenes) {
            if (!fs::exists(scene)) {
                result.AddError("Entry scene not found: " + scene);
            }
        }
    }
    
    // 2. Check output directory is writable
    if (!opts.outputDirectory.empty()) {
        try {
            fs::create_directories(opts.outputDirectory);
            fs::path testFile = fs::path(opts.outputDirectory) / ".write_test";
            std::ofstream test(testFile);
            if (!test.is_open()) {
                result.AddError("Cannot write to output directory: " + opts.outputDirectory);
            } else {
                test.close();
                fs::remove(testFile);
            }
        } catch (const std::exception& e) {
            result.AddError(std::string("Output directory error: ") + e.what());
        }
    }
    
    // 3. Check scripts are compiled
    if (AssetPipeline::Instance().HasAnyScripts()) {
        if (!AssetPipeline::Instance().AreScriptsCompiled()) {
            result.AddError("C# scripts are not compiled. Please compile scripts before exporting.");
        }
        
        // Check GameScripts.dll exists
        fs::path gameScriptsDll = Project::GetProjectDirectory() / ".library" / "GameScripts.dll";
        if (!fs::exists(gameScriptsDll)) {
            result.AddError("GameScripts.dll not found. Build scripts first.");
        }
    }
    
    // 4. Check shaders are compiled
    fs::path exeDir = fs::current_path();
    fs::path compiledDir = exeDir / "shaders" / "compiled" / "windows";
    if (!fs::exists(compiledDir)) {
        result.AddError("Compiled shaders directory not found: " + compiledDir.string());
    } else {
        // Check for essential shader binaries
        std::vector<std::string> essentialShaders = {
            "vs_pbr.bin", "fs_pbr.bin",
            "vs_depth.bin", "fs_depth.bin"
        };
        for (const auto& shader : essentialShaders) {
            if (!fs::exists(compiledDir / shader)) {
                result.AddWarning("Essential shader missing: " + shader);
            }
        }
    }
    
    if (!opts.pakOnly) {
        // 5. Check runtime executable exists (GameRuntime.exe - NOT the editor!)
        bool hasRuntimeExe = fs::exists(exeDir / "GameRuntime.exe") || 
                             fs::exists(exeDir / "runtime" / "GameRuntime.exe") ||
                             fs::exists(exeDir / ".." / "runtime" / "GameRuntime.exe");
        if (!hasRuntimeExe) {
            result.AddError("GameRuntime.exe not found. Build the ClaymoreRuntime target first (cmake --build . --target ClaymoreRuntime)");
        }
        if (!fs::exists(exeDir / "nethost.dll")) {
            result.AddError("Required runtime binary missing: nethost.dll");
        }
        
        // 6. Check ClaymoreEngine.dll exists
        if (!fs::exists(exeDir / "ClaymoreEngine.dll")) {
            result.AddError("ClaymoreEngine.dll not found next to executable.");
        }
        
        // 7. Check runtimeconfig.json exists
        if (!fs::exists(exeDir / "ClaymoreEngine.runtimeconfig.json")) {
            result.AddWarning("ClaymoreEngine.runtimeconfig.json not found - .NET may fail to initialize.");
        }
    }
    
    // 8. Validate scene files can be parsed
    for (const auto& scenePath : opts.entryScenes) {
        if (!fs::exists(scenePath)) continue;
        try {
            std::ifstream in(scenePath);
            if (in.is_open()) {
                json j;
                in >> j;
                // Basic structure check
                if (!j.contains("entities") && !j.is_array()) {
                    result.AddWarning("Scene file may be malformed: " + scenePath);
                }
            }
        } catch (const std::exception& e) {
            result.AddError("Failed to parse scene file: " + scenePath + " - " + e.what());
        }
    }
    
    // 9. Check project directory is set
    if (Project::GetProjectDirectory().empty()) {
        result.AddError("No project directory set.");
    }
    
    return result;
}

static bool CheckShadersCompiled() {
    fs::path exeDir = fs::current_path();
    fs::path compiledDir = exeDir / "shaders" / "compiled" / "windows";
    return fs::exists(compiledDir) && !fs::is_empty(compiledDir);
}

bool BuildExporter::ExportProject(const Options& opts, ProgressCallback progress) {
    auto reportProgress = [&progress](float p, const std::string& msg) {
        std::cout << "[BuildExporter] " << msg << std::endl;
        if (progress) progress(p, msg);
    };
    
    reportProgress(0.0f, "Starting build export...");
    
    // Run validation if enabled
    if (opts.validateBeforeExport) {
        reportProgress(0.05f, "Validating build configuration...");
        ValidationResult validation = ValidateBuild(opts);
        
        // Log warnings
        for (const auto& warning : validation.warnings) {
            std::cerr << "[BuildExporter] WARNING: " << warning << std::endl;
        }
        
        // Log and abort on errors
        if (!validation.success) {
            for (const auto& error : validation.errors) {
                std::cerr << "[BuildExporter] ERROR: " << error << std::endl;
            }
            std::cerr << "[BuildExporter] Build validation failed with " << validation.errors.size() << " error(s)." << std::endl;
            return false;
        }
        
        if (validation.HasWarnings()) {
            std::cout << "[BuildExporter] Validation passed with " << validation.warnings.size() << " warning(s)." << std::endl;
        } else {
            std::cout << "[BuildExporter] Validation passed." << std::endl;
        }
    }
    
    if (opts.entryScenes.empty()) {
        std::cerr << "[BuildExporter] ERROR: No entry scene specified. Aborting export." << std::endl;
        return false;
    }

    std::unordered_set<std::string> entryScenePaths;
    entryScenePaths.reserve(opts.entryScenes.size());
    for (const auto& scenePath : opts.entryScenes) {
        entryScenePaths.insert(NormalizeSlashes(ResolveProjectPath(scenePath).string()));
    }

    std::unordered_set<std::string> skippedAssetPaths;
    std::vector<std::string> skippedAssets;
    std::unordered_set<std::string> skippedPakVirtualPaths;
    auto noteSkippedAsset = [&](const std::string& sourcePath) {
        std::string normalized = NormalizeSlashes(ResolveProjectPath(sourcePath).string());
        if (skippedAssetPaths.insert(normalized).second) {
            skippedAssets.push_back(sourcePath);
        }
    };
    auto noteSkippedPakPath = [&](const std::string& vpath) {
        if (!vpath.empty()) {
            skippedPakVirtualPaths.insert(vpath);
        }
    };
    auto skipOrAbortAsset = [&](const std::string& sourcePath,
                                const std::string& message,
                                const std::vector<std::string>* extraVirtualPaths = nullptr) -> bool {
        const std::string normalized = NormalizeSlashes(ResolveProjectPath(sourcePath).string());
        const bool critical = entryScenePaths.find(normalized) != entryScenePaths.end();
        if (!opts.allowPartialBinaryBuilds || critical) {
            std::cerr << "[BuildExporter] ERROR: " << message << std::endl;
            return false;
        }

        std::cerr << "[BuildExporter] WARNING: " << message
                  << " (skipping for runtime preview)" << std::endl;
        noteSkippedAsset(sourcePath);
        noteSkippedPakPath(MakeRuntimeAssetMapPath(sourcePath));
        if (extraVirtualPaths) {
            for (const auto& vpath : *extraVirtualPaths) {
                noteSkippedPakPath(vpath);
            }
        }
        return true;
    };
    
    reportProgress(0.1f, "Collecting scene dependencies...");
    
    std::unordered_set<std::string> uniqueFiles;
    BuildDependencyGraph dependencyGraph;
    for (const auto& scenePath : opts.entryScenes) {
        dependencyGraph.AddEntryScene(scenePath);
    }
    dependencyGraph.AddProjectResources();

    std::vector<std::string> files = dependencyGraph.FlattenExistingPaths();
    std::cout << "[BuildExporter] Dependency graph collected "
              << dependencyGraph.GetNodes().size() << " nodes and "
              << dependencyGraph.GetEdges().size() << " edges" << std::endl;
    std::cout << "[BuildExporter] Binary builds are scoped to the collected dependency graph." << std::endl;

    // Build GUID->path map from AssetLibrary (which has all registered assets)
    json assetMap = json::array();
    std::unordered_set<std::string> addedGuids;
    {
        // First, get all assets from AssetLibrary - returns vector of (path, guid, type) tuples
        auto allAssets = AssetLibrary::Instance().GetAllAssets();
        for (const auto& [path, guid, type] : allAssets) {
            if (guid == ClaymoreGUID()) continue;
            if (!path.empty()) {
                std::string guidStr = guid.ToString();
                if (addedGuids.find(guidStr) == addedGuids.end()) {
                    std::string vpath = MakeRuntimeAssetMapPath(path);
                    if (vpath.empty()) {
                        continue;
                    }

                    json rec;
                    rec["guid"] = guidStr;
                    rec["path"] = vpath;
                    assetMap.push_back(rec);
                    addedGuids.insert(guidStr);
                }
            }
        }
        
        // Also iterate the assets folder for registry metadata
        fs::path proj = Project::GetProjectDirectory();
        fs::path assets = proj / "assets";
        if (fs::exists(assets)) {
            for (auto& e : fs::recursive_directory_iterator(assets)) {
                if (!e.is_regular_file()) continue;
                std::string p = e.path().string();
                const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(p);
                if (meta && meta->guid != ClaymoreGUID()) {
                    std::string guidStr = meta->guid.ToString();
                    if (addedGuids.find(guidStr) == addedGuids.end()) {
                        std::string vpath = MakeRuntimeAssetMapPath(p);
                        if (vpath.empty()) {
                            continue;
                        }

                        json rec;
                        rec["guid"] = guidStr;
                        rec["path"] = vpath;
                        assetMap.push_back(rec);
                        addedGuids.insert(guidStr);
                    }
                }
            }
        }

        // Also include resources/ folder contents in asset map (VFS-accessible at runtime)
        fs::path resources = proj / "resources";
        if (fs::exists(resources)) {
            for (auto& e : fs::recursive_directory_iterator(resources)) {
                if (!e.is_regular_file()) continue;
                std::string p = e.path().string();
                std::string vpath = MakeRuntimeAssetMapPath(p);
                if (vpath.empty()) continue;

                ClaymoreGUID guid{};
                const AssetMetadata* meta = AssetRegistry::Instance().GetMetadata(p);
                if (meta && meta->guid != ClaymoreGUID()) {
                    guid = meta->guid;
                } else {
                    guid = ResourceManifest::DeterministicGuidFromPath(vpath);
                }

                if (guid == ClaymoreGUID()) continue;
                std::string guidStr = guid.ToString();
                if (addedGuids.find(guidStr) != addedGuids.end()) continue;

                json rec;
                rec["guid"] = guidStr;
                rec["path"] = vpath;
                assetMap.push_back(rec);
                addedGuids.insert(guidStr);
            }
        }
        
        std::cout << "[BuildExporter] Built asset map with " << assetMap.size() << " entries" << std::endl;
    }

    // Always include compiled shader bins only (not sources) for runtime
    fs::path exeDir = fs::current_path();
    fs::path compiledDir = exeDir / "shaders" / "compiled" / "windows";
    if (fs::exists(compiledDir)) {
        for (auto& e : fs::recursive_directory_iterator(compiledDir)) {
            if (e.is_regular_file() && e.path().extension() == ".bin") files.push_back(e.path().string());
        }
    }
    // Also include directly any pre-existing .bin in shaders/ for safety
    fs::path flatDir = exeDir / "shaders";
    if (fs::exists(flatDir)) {
        for (auto& e : fs::directory_iterator(flatDir)) {
            if (e.is_regular_file() && e.path().extension() == ".bin") files.push_back(e.path().string());
        }
    }

    // Minimal runtime asset set required by renderer/text
    {
        const char* kRuntimeAssets[] = {
            "assets/debug/white.png",
            "assets/debug/metallic_roughness.png",
            "assets/debug/normal.png",
            "assets/fonts/Roboto-Regular.ttf"
        };
        fs::path proj = Project::GetProjectDirectory();
        
        // Build a list of fallback search paths
        std::vector<fs::path> searchPaths;
        
        // 1. Project directory
        if (!proj.empty()) {
            searchPaths.push_back(proj);
        }
        
        // 2. Parent of project (repo root in typical layout)
        if (!proj.empty() && !proj.parent_path().empty()) {
            searchPaths.push_back(proj.parent_path());
        }
        
        // 3. Engine source directory (for development builds)
        // Try to find cmeng/assets relative to various locations
        fs::path exeDir = fs::current_path();
        std::vector<fs::path> engineSearchDirs = {
            exeDir / ".." / ".." / "..",           // build/x64-Debug -> repo
            exeDir / ".." / "..",                   // build/Debug -> repo
            exeDir / "..",                          // build -> repo
            exeDir,                                 // current dir
            fs::weakly_canonical(exeDir / "../../.."),
        };
        for (const auto& d : engineSearchDirs) {
            if (fs::exists(d / "assets")) {
                searchPaths.push_back(d);
            }
        }
        
        for (const char* rel : kRuntimeAssets) {
            bool added = false;
            for (const auto& base : searchPaths) {
                fs::path absPath = base / rel;
                if (fs::exists(absPath)) {
                    AddIfExists(absPath.string(), files);
                    added = true;
                    std::cout << "[BuildExporter] Found runtime asset: " << rel << " at " << absPath << std::endl;
                    break;
                }
            }
            if (!added) {
                std::cerr << "[BuildExporter] WARNING: Required runtime asset not found: " << rel << std::endl;
                std::cerr << "[BuildExporter] Searched in:" << std::endl;
                for (const auto& base : searchPaths) {
                    std::cerr << "  - " << (base / rel) << std::endl;
                }
                // Note: Non-fatal warning - build continues without this asset
            }
        }
    }

    // Always include terrain binaries from assets/terrain folder AND .bin/terrain folder
    {
        fs::path proj = Project::GetProjectDirectory();
        
        // Check assets/terrain/ (legacy location)
        fs::path terrainDir = proj / "assets" / "terrain";
        if (fs::exists(terrainDir)) {
            for (auto& e : fs::directory_iterator(terrainDir)) {
                if (e.is_regular_file() && e.path().extension() == ".terrainbin") {
                    files.push_back(e.path().string());
                    std::cout << "[BuildExporter] Including terrain: " << e.path().filename() << std::endl;
                }
            }
        }
        
        // Check .bin/terrain/ (current location for generated terrain binaries)
        fs::path binTerrainDir = proj / ".bin" / "terrain";
        if (fs::exists(binTerrainDir)) {
            for (auto& e : fs::directory_iterator(binTerrainDir)) {
                if (e.is_regular_file() && e.path().extension() == ".terrainbin") {
                    files.push_back(e.path().string());
                    std::cout << "[BuildExporter] Including terrain binary: " << e.path().filename() << std::endl;
                }
            }
        }
    }
    
    // Always include navmesh binaries from .bin/nav/ folder
    {
        fs::path proj = Project::GetProjectDirectory();
        
        // Check .bin/nav/ (location for generated navmesh binaries)
        fs::path binNavDir = proj / ".bin" / "nav";
        if (fs::exists(binNavDir)) {
            for (auto& e : fs::directory_iterator(binNavDir)) {
                if (e.is_regular_file() &&
                    (e.path().extension() == ".navbin" || e.path().extension() == ".navpack")) {
                    files.push_back(e.path().string());
                    std::cout << "[BuildExporter] Including navmesh binary: " << e.path().filename() << std::endl;
                }
            }
        }
        
        // Check legacy assets/Nav/ folder
        fs::path legacyNavDir = proj / "assets" / "Nav";
        if (fs::exists(legacyNavDir)) {
            for (auto& e : fs::directory_iterator(legacyNavDir)) {
                if (e.is_regular_file() &&
                    (e.path().extension() == ".navbin" || e.path().extension() == ".navpack")) {
                    files.push_back(e.path().string());
                    std::cout << "[BuildExporter] Including legacy navmesh: " << e.path().filename() << std::endl;
                }
            }
        }
    }

    // Always include world graph data from .bin/world/ folder
    {
        fs::path proj = Project::GetProjectDirectory();
        fs::path worldDir = proj / ".bin" / "world";
        if (fs::exists(worldDir)) {
            for (auto& e : fs::directory_iterator(worldDir)) {
                if (e.is_regular_file() && e.path().filename() == "worldgraph.json") {
                    files.push_back(e.path().string());
                    std::cout << "[BuildExporter] Including world graph: " << e.path().filename() << std::endl;
                }
            }
        }
    }

    // Always include armor wrap binaries from .bin/wraps/ folder
    {
        fs::path proj = Project::GetProjectDirectory();
        fs::path wrapDir = proj / ".bin" / "wraps";
        if (fs::exists(wrapDir)) {
            for (auto& e : fs::recursive_directory_iterator(wrapDir)) {
                if (e.is_regular_file() && e.path().extension() == ".wrapbin") {
                    files.push_back(e.path().string());
                    std::cout << "[BuildExporter] Including wrap bin: " << e.path().filename() << std::endl;
                }
            }
        }
    }
    
    // Compile model registry from the actual dependency graph inputs that will be packaged.
    reportProgress(0.2f, "Compiling model registry...");
    {
        fs::path proj = Project::GetProjectDirectory();
        fs::path binDir = proj / ".bin";
        fs::create_directories(binDir);
        
        std::vector<cm::RuntimeModelManifest> allManifests;
        std::unordered_set<std::string> neededModelSources;
        for (const std::string& file : files) {
            std::string modelSource = ResolveModelSourcePath(file);
            if (!modelSource.empty()) {
                neededModelSources.insert(NormalizeSlashes(modelSource));
                continue;
            }

            fs::path resolved = ResolveProjectPath(file);
            if (IsModelSourceExtension(ToLowerExt(resolved))) {
                neededModelSources.insert(NormalizeSlashes(resolved.string()));
            }
        }

        std::cout << "[BuildExporter] Found " << neededModelSources.size()
                  << " models needed for runtime" << std::endl;

        for (const std::string& modelSourcePath : neededModelSources) {
            BuiltModelPaths builtPaths;
            if (!EnsureModelCache(modelSourcePath, builtPaths)) {
                if (!skipOrAbortAsset(modelSourcePath,
                                      "Failed to build runtime model cache for " + modelSourcePath)) {
                    return false;
                }
                continue;
            }

            cm::RuntimeModelManifest manifest;
            if (!cm::RuntimeModelManifestWriter::CompileFromMeta(builtPaths.metaPath,
                    manifest,
                    [](const std::string& path) -> ClaymoreGUID {
                        return ResolveAssetGuidForPath(path);
                    })) {
                if (!skipOrAbortAsset(modelSourcePath,
                                      "Failed to compile runtime model manifest for " + builtPaths.metaPath)) {
                    return false;
                }
                continue;
            }

            if (manifest.modelGuid.high == 0 && manifest.modelGuid.low == 0) {
                if (!skipOrAbortAsset(modelSourcePath,
                                      "Runtime model manifest missing model GUID for " + builtPaths.metaPath)) {
                    return false;
                }
                continue;
            }

            allManifests.push_back(std::move(manifest));
            std::cout << "[BuildExporter] Including model: "
                      << fs::path(modelSourcePath).stem() << std::endl;
        }
        
        // Write centralized registry
        if (!allManifests.empty()) {
            fs::path registryPath = binDir / "model_registry.bin";
            if (cm::RuntimeModelManifestWriter::WriteRegistryToFile(allManifests, registryPath.string())) {
                files.push_back(registryPath.string());
                std::cout << "[BuildExporter] Compiled model registry with " << allManifests.size() << " models" << std::endl;
            }
        } else {
            std::cout << "[BuildExporter] No model manifests needed for runtime" << std::endl;
        }
    }

    reportProgress(0.25f, "Collecting shaders and runtime assets...");
    
    // Dedup
    std::vector<std::string> dedup;
    for (auto& f : files) {
        if (uniqueFiles.insert(f).second) dedup.push_back(f);
    }

    reportProgress(0.3f, "Building pak archive (" + std::to_string(dedup.size()) + " files)...");
    
    // Build .pak
    PakArchive pak;
    // Prepare manifest with entry scene virtual path (if any)
    // Use binary version of entry scene for runtime
    std::string entrySceneVPath;
    if (!opts.entryScenes.empty()) {
        const std::string& first = opts.entryScenes.front();
        fs::path scenePath(first);
        // Entry scene should use binary version in manifest
        auto type = BinaryAssetCache::GetAssetType(first);
        if (type == BinaryAssetCache::AssetType::Scene) {
            std::string vpath = MakeVirtualPath(scenePath);
            entrySceneVPath = fs::path(vpath).replace_extension(".sceneb").string();
        } else {
            entrySceneVPath = MakeVirtualPath(scenePath);
        }
    }
    
    std::unordered_set<std::string> addedPakPaths;
    auto addPakFile = [&](const std::string& diskPath, const std::string& vpath) -> bool {
        if (!addedPakPaths.insert(vpath).second) {
            return true;
        }
        std::ifstream in(diskPath, std::ios::binary);
        if (!in.is_open()) {
            std::cerr << "[BuildExporter] Warning: Could not open file: " << diskPath << std::endl;
            return false;
        }
        in.seekg(0, std::ios::end);
        size_t size = static_cast<size_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        std::vector<uint8_t> data(size);
        if (size != 0) in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

        pak.AddFile(vpath, data);
        std::cout << "[BuildExporter] Added to pak: " << vpath << " (" << size << " bytes)" << std::endl;
        return true;
    };

    for (const auto& f : dedup) {
        fs::path sourcePath(f);
        
        // Check if this asset has a binary version we should use instead
        std::vector<std::string> extraFiles;
        std::string error;
        std::string actualFile = GetBinaryVersionIfExists(f, &extraFiles, &error);
        if (actualFile.empty()) {
            std::vector<std::string> skippedExtraVirtualPaths;
            skippedExtraVirtualPaths.reserve(extraFiles.size());
            for (const auto& extra : extraFiles) {
                skippedExtraVirtualPaths.push_back(MakeRuntimeVirtualPath(fs::path(extra), sourcePath));
            }
            if (!skipOrAbortAsset(f, error, &skippedExtraVirtualPaths)) {
                return false;
            }
            continue;
        }
        std::string vpath;
        
        if (actualFile != f) {
            // Using binary version - create appropriate virtual path
            vpath = MakeVirtualPathForBinary(fs::path(actualFile), sourcePath);
            std::cout << "[BuildExporter] Using binary: " << sourcePath.filename() << " -> " << fs::path(vpath).filename() << std::endl;
        } else {
            vpath = MakeVirtualPath(sourcePath);
        }
        
        if (!addPakFile(actualFile, vpath)) {
            std::vector<std::string> skippedVirtualPaths = { vpath };
            for (const auto& extra : extraFiles) {
                skippedVirtualPaths.push_back(MakeRuntimeVirtualPath(fs::path(extra), sourcePath));
            }
            if (!skipOrAbortAsset(f, "Failed to add asset to pak: " + actualFile, &skippedVirtualPaths)) {
                return false;
            }
            continue;
        }

        // Add any extra binaries (e.g., skelbin for models)
        for (const auto& extra : extraFiles) {
            std::string extraVPath = MakeRuntimeVirtualPath(fs::path(extra), sourcePath);
            if (!addPakFile(extra, extraVPath)) {
                std::vector<std::string> skippedVirtualPaths = { vpath, extraVPath };
                if (!skipOrAbortAsset(f, "Failed to add asset dependency to pak: " + extra, &skippedVirtualPaths)) {
                    return false;
                }
                break;
            }
        }
    }

    // Validate assetMap entries against packed files
    json filteredAssetMap = json::array();
    std::vector<std::string> missingAssetMapEntries;
    auto isBinaryVirtualPath = [](const std::string& vpath) -> bool {
        fs::path p(vpath);
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".meshbin" || ext == ".skelbin" || ext == ".modelrt" ||
               ext == ".sceneb" || ext == ".prefabb" || ext == ".matbin" ||
               ext == ".animbin" || ext == ".actrlbin" || ext == ".terrainbin" ||
               ext == ".navbin" || ext == ".navpack" ||
               ext == ".wrapbin";
    };
    for (const auto& rec : assetMap) {
        std::string vpath = rec.value("path", "");
        if (vpath.empty()) continue;
        if (addedPakPaths.find(vpath) != addedPakPaths.end()) {
            filteredAssetMap.push_back(rec);
        } else if (skippedPakVirtualPaths.find(vpath) != skippedPakVirtualPaths.end()) {
            continue;
        } else if (isBinaryVirtualPath(vpath)) {
            missingAssetMapEntries.push_back(vpath);
        }
    }
    if (!missingAssetMapEntries.empty()) {
        std::cerr << "[BuildExporter] "
                  << (opts.allowPartialBinaryBuilds ? "WARNING" : "ERROR")
                  << ": Missing binary assets referenced by assetMap:" << std::endl;
        for (const auto& v : missingAssetMapEntries) {
            std::cerr << "[BuildExporter]   - " << v << std::endl;
            noteSkippedPakPath(v);
        }
        if (!opts.allowPartialBinaryBuilds) {
            return false;
        }
    }

    if (!skippedAssets.empty()) {
        std::cerr << "[BuildExporter] WARNING: Skipped " << skippedAssets.size()
                  << " non-critical asset(s) while preparing the runtime preview." << std::endl;
        for (const auto& asset : skippedAssets) {
            std::cerr << "[BuildExporter]   - " << asset << std::endl;
        }
    }

    // Add simple manifest
    {
        json manifest;
        manifest["entryScene"] = entrySceneVPath;
        if (!filteredAssetMap.empty()) manifest["assetMap"] = filteredAssetMap;
        
        // Include enabled modules configuration for runtime loading
        const auto& mods = Project::GetModules();
        if (!mods.empty()) {
            json modulesArray = json::array();
            for (const auto& m : mods) {
                if (!m.enabled || m.dll.empty()) continue;
                json modEntry;
                modEntry["id"] = m.id;
                // Store relative path from output directory (modules are copied relative)
                fs::path src = fs::path(m.dll);
                if (!src.is_absolute()) src = Project::GetProjectDirectory() / src;
                std::error_code ec;
                fs::path relToProj = fs::relative(src, Project::GetProjectDirectory(), ec);
                modEntry["dll"] = ec ? src.filename().string() : relToProj.string();
                modulesArray.push_back(modEntry);
            }
            if (!modulesArray.empty()) {
                manifest["modules"] = modulesArray;
            }
        }
        
        // Include game cursor settings
        const auto& cursorSettings = Project::GetCursorSettings();
        if (!cursorSettings.texturePath.empty()) {
            json cursor;
            cursor["texture"] = MakeVirtualPath(fs::path(cursorSettings.texturePath));
            cursor["scale"] = cursorSettings.baseScale;
            cursor["hotspotX"] = cursorSettings.hotspotX;
            cursor["hotspotY"] = cursorSettings.hotspotY;
            cursor["useDPIScaling"] = cursorSettings.useDPIScaling;
            manifest["gameCursor"] = cursor;
            
            // Ensure cursor texture is in the dedup list so it gets included in the PAK
            fs::path cursorFullPath = Project::GetAssetDirectory() / cursorSettings.texturePath;
            if (fs::exists(cursorFullPath)) {
                std::string cursorAbsPath = cursorFullPath.string();
                if (uniqueFiles.insert(cursorAbsPath).second) {
                    dedup.push_back(cursorAbsPath);
                    std::cout << "[BuildExporter] Including cursor texture: " << cursorAbsPath << std::endl;
                }
            }
        }
        
        // Include project default font (for TextRenderer)
        const auto& defaultFont = Project::GetDefaultFontPath();
        if (!defaultFont.empty()) {
            manifest["defaultFont"] = MakeVirtualPath(fs::path(defaultFont));
            
            // Ensure default font is included in the PAK
            fs::path fontFullPath = fs::path(defaultFont);
            if (!fontFullPath.is_absolute()) {
                std::string fontStr = fontFullPath.generic_string();
                if (fontStr.rfind("assets/", 0) == 0) {
                    fontFullPath = Project::GetProjectDirectory() / fontFullPath;
                } else {
                    fontFullPath = Project::GetAssetDirectory() / fontFullPath;
                }
            }
            if (fs::exists(fontFullPath)) {
                std::string fontAbsPath = fontFullPath.string();
                if (uniqueFiles.insert(fontAbsPath).second) {
                    dedup.push_back(fontAbsPath);
                    std::cout << "[BuildExporter] Including default font: " << fontAbsPath << std::endl;
                }
            }
        }
        
        std::string text = manifest.dump(0);
        std::vector<uint8_t> bytes(text.begin(), text.end());
        pak.AddFile("game_manifest.json", bytes);
    }
    
    // Add resource manifest for runtime Resources API
    {
        auto allResources = ResourceManifest::Get().GetAllResources();
        if (!allResources.empty()) {
            json resourceManifest;
            resourceManifest["version"] = 1;
            resourceManifest["resources"] = json::array();
            
            for (const auto* res : allResources) {
                json entry;
                entry["guid"] = res->guid.ToString();
                entry["name"] = res->name;
                entry["typeName"] = res->typeName;
                entry["path"] = MakeVirtualPath(fs::path(res->path));
                
                json deps = json::array();
                for (const auto& dep : res->dependencies) {
                    deps.push_back(dep.ToString());
                }
                entry["dependencies"] = deps;
                
                resourceManifest["resources"].push_back(entry);
            }
            
            std::string text = resourceManifest.dump(2);
            std::vector<uint8_t> bytes(text.begin(), text.end());
            pak.AddFile("resource_manifest.json", bytes);
            
            std::cout << "[BuildExporter] Added resource manifest with " << allResources.size() << " entries" << std::endl;
        }
    }

    reportProgress(0.5f, "Saving pak archive...");
    
    fs::create_directories(opts.outputDirectory);
    // Resolve project name with sensible fallback
    std::string projName = Project::GetProjectName();
    if (projName.empty()) {
        fs::path projDir = Project::GetProjectDirectory();
        if (!projDir.empty()) projName = projDir.filename().string();
        if (projName.empty()) projName = "Game";
    }
    fs::path pakOut = fs::path(opts.outputDirectory) / (projName + ".pak");
    if (!pak.SaveToFile(pakOut.string(), opts.compressPak)) {
        std::cerr << "[BuildExporter] ERROR: Failed to save pak file." << std::endl;
        return false;
    }
    
    if (opts.compressPak) {
        std::cout << "[BuildExporter] Pak saved with compression enabled." << std::endl;
    }

    if (opts.pakOnly) {
        reportProgress(1.0f, "Pak build complete.");
        return true;
    }
    
    reportProgress(0.6f, "Copying runtime binaries...");

    // Copy runtime executable and rename to project name
    // IMPORTANT: We must use the dedicated ClaymoreRuntime executable, NOT the editor!
    fs::path srcExe;
    
    // Look for GameRuntime.exe in various locations
    // The CMake output is typically at ${CMAKE_BINARY_DIR}/runtime/GameRuntime.exe
    std::vector<fs::path> runtimeCandidates = {
        exeDir / "runtime" / "GameRuntime.exe",           // CMake: same build dir/runtime
        exeDir / ".." / "runtime" / "GameRuntime.exe",    // CMake: sibling runtime folder  
        exeDir / "GameRuntime.exe",                       // Next to editor
    };
    
    std::cout << "[BuildExporter] Searching for runtime executable..." << std::endl;
    for (const auto& candidate : runtimeCandidates) {
        std::cout << "[BuildExporter]   Checking: " << candidate << std::endl;
        if (fs::exists(candidate)) {
            srcExe = candidate;
            std::cout << "[BuildExporter]   Found!" << std::endl;
            break;
        }
    }
    
    if (srcExe.empty() || !fs::exists(srcExe)) {
        std::cerr << "[BuildExporter] ERROR: GameRuntime.exe not found!" << std::endl;
        std::cerr << "[BuildExporter] You must build the ClaymoreRuntime target before exporting." << std::endl;
        std::cerr << "[BuildExporter] In Visual Studio: Right-click ClaymoreRuntime -> Build" << std::endl;
        std::cerr << "[BuildExporter] Or from command line: cmake --build . --target ClaymoreRuntime" << std::endl;
        std::cerr << "[BuildExporter] Searched locations:" << std::endl;
        for (const auto& candidate : runtimeCandidates) {
            std::cerr << "[BuildExporter]   - " << candidate << std::endl;
        }
        return false;
    }
    
    // Verify this is NOT the editor executable
    if (srcExe.filename() == "Claymore.exe") {
        std::cerr << "[BuildExporter] ERROR: Cannot use Claymore.exe (editor) as game runtime!" << std::endl;
        std::cerr << "[BuildExporter] Build the ClaymoreRuntime target to create GameRuntime.exe" << std::endl;
        return false;
    }
    
    fs::path dstExe = fs::path(opts.outputDirectory) / (projName + ".exe");
    try {
        fs::copy_file(srcExe, dstExe, fs::copy_options::overwrite_existing);
        std::cout << "[BuildExporter] Created game executable: " << (projName + ".exe") 
                  << " (from " << srcExe.filename() << ")" << std::endl;
    } catch(const std::exception& e) {
        std::cerr << "[BuildExporter] Failed to copy executable: " << e.what() << std::endl;
        return false;
    }
    
    // nethost.dll must be next to exe for LoadLibrary to find it
    fs::path nethostSrc = srcExe.parent_path() / "nethost.dll";
    if (fs::exists(nethostSrc)) {
        try {
            fs::copy_file(nethostSrc, fs::path(opts.outputDirectory) / "nethost.dll", fs::copy_options::overwrite_existing);
            std::cout << "[BuildExporter] Copied nethost.dll" << std::endl;
        } catch(...) {}
    }
    
    // Copy GameScripts.dll (per-project)
    fs::path gameScriptsSrc = Project::GetProjectDirectory() / ".library" / "GameScripts.dll";
    if (fs::exists(gameScriptsSrc)) {
        try {
            fs::copy_file(gameScriptsSrc, fs::path(opts.outputDirectory) / "GameScripts.dll", fs::copy_options::overwrite_existing);
            std::cout << "[BuildExporter] Copied GameScripts.dll" << std::endl;
        } catch(const std::exception& e) {
            std::cerr << "[BuildExporter] Warning: Failed to copy GameScripts.dll: " << e.what() << std::endl;
        }
    }
    

    reportProgress(0.7f, "Copying project modules...");
    
    // Copy enabled project module DLLs into the build output (preserve a 'modules' subfolder when used)
    try {
        const auto& mods = Project::GetModules();
        if (!mods.empty()) {
            for (const auto& m : mods) {
                if (!m.enabled) continue;
                if (m.dll.empty()) continue;
                fs::path src = fs::path(m.dll);
                if (!src.is_absolute()) src = Project::GetProjectDirectory() / src;
                if (!fs::exists(src)) {
                    std::cerr << "[BuildExporter] Warning: Module DLL not found: " << src << std::endl;
                    continue;
                }

                // If the module path is under a 'modules' folder in the project, mirror that in output; otherwise copy next to exe
                fs::path relToProj;
                std::error_code ec;
                relToProj = fs::relative(src, Project::GetProjectDirectory(), ec);
                fs::path dst = fs::path(opts.outputDirectory) / (ec ? src.filename() : relToProj);
                if (!ec) {
                    // Ensure the subdirectories (e.g., 'modules') exist
                    fs::create_directories(dst.parent_path());
                } else {
                    // Fallback: ensure output directory exists
                    fs::create_directories(fs::path(opts.outputDirectory));
                }

                try {
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
                    std::cout << "[BuildExporter] Copied module DLL: " << dst << std::endl;
                } catch(const std::exception& e) {
                    std::cerr << "[BuildExporter] Failed to copy module DLL '" << src << "' -> '" << dst << "': " << e.what() << std::endl;
                }
            }
        }
    } catch(const std::exception& e) {
        std::cerr << "[BuildExporter] Error while copying module DLLs: " << e.what() << std::endl;
    }

    reportProgress(0.8f, "Copying managed runtime...");
    
    // Helper to check if a file should be skipped (dev-only files and legacy root runtime binaries)
    auto shouldSkipFile = [](const fs::path& p) -> bool {
        std::string ext = p.extension().string();
        std::string name = p.filename().string();
        
        // Skip debug symbols in release builds
        if (ext == ".pdb") return true;
        
        // Skip development dependencies (assimp, etc.)
        if (name.find("assimp") != std::string::npos) return true;
        
        // Skip xml documentation files
        if (ext == ".xml" && fs::exists(p.parent_path() / (p.stem().string() + ".dll"))) return true;
        
        // Never copy legacy self-contained app runtime binaries to the output root.
        if (name == "hostfxr.dll" || name == "hostpolicy.dll" ||
            name == "coreclr.dll" || name == "clrjit.dll" ||
            name == "clrgc.dll" || name == "clretwrc.dll" ||
            name == "mscordbi.dll" || name == "createdump.exe" ||
            name.rfind("mscordaccore", 0) == 0) {
            return true;
        }
        
        return false;
    };

    auto removeLegacyRootRuntimeFiles = [](const fs::path& dir) {
        const std::vector<std::string> runtimeFiles = {
            "hostfxr.dll",
            "hostpolicy.dll",
            "coreclr.dll",
            "clrjit.dll",
            "clrgc.dll",
            "clretwrc.dll",
            "mscordbi.dll",
            "createdump.exe"
        };

        for (const auto& name : runtimeFiles) {
            std::error_code ec;
            fs::remove(dir / name, ec);
        }

        for (auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name.rfind("mscordaccore", 0) == 0) {
                std::error_code ec;
                fs::remove(entry.path(), ec);
            }
        }
    };
    
    // Copy managed engine runtime
    try {
        std::vector<fs::path> searchRoots;
        searchRoots.push_back(exeDir);
        
        fs::path repoRoot = fs::weakly_canonical(exeDir / "../../..");
        if (fs::exists(repoRoot)) {
            searchRoots.push_back(repoRoot);
        }
        
        fs::path projDir = Project::GetProjectDirectory();
        if (!projDir.empty()) {
            searchRoots.push_back(projDir.parent_path());
        }
        
        fs::path bundledManagedDir;
        fs::path frameworkManagedDir;
        fs::path legacyManagedDir;
        
        // Priority order: prefer a bundled private runtime layout, then framework-dependent output.
        std::vector<std::string> managedSubpaths = {
            // Publish outputs (highest priority when staged with a private dotnet runtime root)
            "managed/ClaymoreEngine/bin/Release/net10.0/win-x64/publish",
            "managed/ClaymoreEngine/bin/Debug/net10.0/win-x64/publish",
            "managed/ClaymoreEngine/bin/Release/net10.0-windows/win-x64/publish",
            // Regular build outputs (framework-dependent)
            "managed/ClaymoreEngine/bin/Release/net10.0",
            "managed/ClaymoreEngine/bin/Debug/net10.0",
            "managed/ClaymoreEngine/bin/Release/net10.0-windows",
            "managed/ClaymoreEngine/bin/Debug/net10.0-windows"
        };
        
        for (const auto& root : searchRoots) {
            for (const auto& subpath : managedSubpaths) {
                fs::path candidate = root / subpath;
                if (fs::exists(candidate) && fs::exists(candidate / "ClaymoreEngine.dll")) {
                    if (HasBundledDotnetRuntime(candidate)) {
                        bundledManagedDir = candidate;
                        break;
                    }

                    if (HasLegacySelfContainedComponentLayout(candidate)) {
                        if (legacyManagedDir.empty()) {
                            legacyManagedDir = candidate;
                        }
                        continue;
                    }

                    if (frameworkManagedDir.empty()) {
                        frameworkManagedDir = candidate;
                    }
                }
            }
            if (!bundledManagedDir.empty()) break;
        }

        const fs::path chosenManagedDir = !bundledManagedDir.empty()
            ? bundledManagedDir
            : frameworkManagedDir;
        const bool hasBundledRuntime = !chosenManagedDir.empty() && HasBundledDotnetRuntime(chosenManagedDir);
        
        if (!chosenManagedDir.empty()) {
            if (hasBundledRuntime) {
                std::cout << "[BuildExporter] Using bundled private .NET runtime from: " << chosenManagedDir << std::endl;
                std::cout << "[BuildExporter] Users will NOT need .NET installed." << std::endl;
            } else {
                std::cout << "[BuildExporter] Using FRAMEWORK-DEPENDENT runtime from: " << chosenManagedDir << std::endl;
                std::cout << "[BuildExporter] Users will need the .NET 10 Runtime installed." << std::endl;
            }
            
            removeLegacyRootRuntimeFiles(fs::path(opts.outputDirectory));

            // Copy all root-level managed files from the chosen directory.
            for (auto& entry : fs::directory_iterator(chosenManagedDir)) {
                if (entry.is_directory()) {
                    if (hasBundledRuntime && entry.path().filename() == "dotnet") {
                        fs::path dst = fs::path(opts.outputDirectory) / "dotnet";
                        if (CopyDirectoryRecursive(entry.path(), dst)) {
                            std::cout << "[BuildExporter] Copied bundled dotnet runtime root." << std::endl;
                        }
                    }
                    continue;
                }
                
                fs::path srcFile = entry.path();
                if (shouldSkipFile(srcFile)) {
                    continue;
                }
                
                // All managed files go in root (runtimeconfig must be next to dll)
                std::string filename = srcFile.filename().string();
                fs::path dst = fs::path(opts.outputDirectory) / filename;
                
                try {
                    fs::copy_file(srcFile, dst, fs::copy_options::overwrite_existing);
                    // Only log important files to reduce noise
                    if (filename.find("Claymore") != std::string::npos ||
                        filename == "GameScripts.dll") {
                        std::cout << "[BuildExporter] Copied: " << filename << std::endl;
                    }
                } catch(const std::exception& e) {
                    std::cerr << "[BuildExporter] Failed to copy " << filename << ": " << e.what() << std::endl;
                }
            }
        } else {
            // Fallback: copy essential files from exe directory
            std::vector<std::string> essentialFiles = {
                "ClaymoreEngine.dll",
                "ClaymoreEngine.runtimeconfig.json",
                "ClaymoreEngine.deps.json"
            };
            removeLegacyRootRuntimeFiles(fs::path(opts.outputDirectory));
            for (const auto& file : essentialFiles) {
                fs::path src = exeDir / file;
                if (fs::exists(src)) {
                    fs::path dst = fs::path(opts.outputDirectory) / file;
                    std::error_code ec;
                    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        std::cout << "[BuildExporter] Copied: " << file << std::endl;
                    }
                }
            }
            if (HasBundledDotnetRuntime(exeDir)) {
                CopyDirectoryRecursive(exeDir / "dotnet", fs::path(opts.outputDirectory) / "dotnet");
                std::cout << "[BuildExporter] Copied bundled dotnet runtime root from current build output." << std::endl;
            }
            if (!legacyManagedDir.empty()) {
                std::cerr << "[BuildExporter] Warning: Ignoring legacy self-contained managed publish at "
                          << legacyManagedDir << std::endl;
                std::cerr << "[BuildExporter] Component hosting requires a framework-dependent runtimeconfig plus optional dotnet\\ runtime root." << std::endl;
            }
            std::cerr << "[BuildExporter] Warning: Full managed directory not found. Using fallback." << std::endl;
            std::cerr << "[BuildExporter] Run 'cmake --build . --target publish_managed_selfcontained' to stage a bundled private runtime." << std::endl;
        }
    } catch(const std::exception& e) {
        std::cerr << "[BuildExporter] Failed copying managed runtime: " << e.what() << std::endl;
    }
    
    // Copy GameScripts.dll from project .library folder
    try {
        fs::path projDir = Project::GetProjectDirectory();
        fs::path gameScriptsSrc = projDir / ".library" / "GameScripts.dll";
        if (fs::exists(gameScriptsSrc)) {
            fs::path gameScriptsDst = fs::path(opts.outputDirectory) / "GameScripts.dll";
            fs::copy_file(gameScriptsSrc, gameScriptsDst, fs::copy_options::overwrite_existing);
            std::cout << "[BuildExporter] Copied: GameScripts.dll" << std::endl;
        } else {
            std::cerr << "[BuildExporter] Warning: GameScripts.dll not found at " << gameScriptsSrc << std::endl;
            std::cerr << "[BuildExporter] Scripts will not work in the exported build!" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[BuildExporter] Failed copying GameScripts.dll: " << e.what() << std::endl;
    }

    reportProgress(0.9f, "Finalizing build...");

    // Drop a marker file to force play-mode in exported builds
    try {
        std::ofstream marker(fs::path(opts.outputDirectory) / "game_mode_only.marker", std::ios::trunc);
        marker << "play_mode_only";
        marker.close();
    } catch(...) {}

    reportProgress(1.0f, "Export completed successfully!");
    std::cout << "[BuildExporter] Output directory: " << opts.outputDirectory << std::endl;
    std::cout << "[BuildExporter] Files included: " << dedup.size() << std::endl;

    return true;
}

