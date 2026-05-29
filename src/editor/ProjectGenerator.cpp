#include "ProjectGenerator.h"

#include "Project.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
constexpr const char* kManagedAssemblyName = "GameScripts";
constexpr const char* kManagedTargetFramework = "net10.0";
constexpr const char* kManagedRuntimeVersion = "10.0.5";
constexpr const char* kSdkStyleCSharpProjectTypeGuid = "9A19103F-16F7-4668-BE54-9A1E7A4F7556";

struct ReferenceEntry {
    std::string includeName;
    fs::path hintPath;
};

struct ManagedEngineReference {
    bool useProjectReference = false;
    fs::path path;
};

std::string EscapeXml(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

std::string SanitizeNamespace(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (char ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '_') {
            result += ch;
        } else if (!result.empty() && result.back() != '_') {
            result += '_';
        }
    }

    if (result.empty()) {
        return "GameScripts";
    }

    if (std::isdigit(static_cast<unsigned char>(result.front()))) {
        result.insert(result.begin(), '_');
    }

    return result;
}

fs::path MakeRelativeHintPath(const fs::path& projectRoot, const fs::path& targetPath) {
    std::error_code ec;
    fs::path relativePath = fs::relative(targetPath, projectRoot, ec);
    return ec ? targetPath : relativePath;
}

uint64_t HashString64(std::string_view value, uint64_t seed) {
    constexpr uint64_t kPrime = 1099511628211ull;

    uint64_t hash = seed;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= kPrime;
    }

    return hash;
}

std::string MakeStableGuid(const fs::path& projectRoot, std::string_view salt) {
    std::error_code ec;
    fs::path canonicalRoot = fs::weakly_canonical(projectRoot, ec);
    if (ec) {
        canonicalRoot = projectRoot;
    }

    const std::string key = canonicalRoot.generic_string() + "#" + std::string(salt);
    uint64_t high = HashString64(key, 14695981039346656037ull);
    uint64_t low = HashString64(key, 7809847782465536322ull);

    high = (high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    low = (low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    std::ostringstream raw;
    raw << std::uppercase << std::hex << std::setfill('0')
        << std::setw(16) << high
        << std::setw(16) << low;

    const std::string hex = raw.str();
    std::ostringstream guid;
    guid << hex.substr(0, 8) << '-'
         << hex.substr(8, 4) << '-'
         << hex.substr(12, 4) << '-'
         << hex.substr(16, 4) << '-'
         << hex.substr(20, 12);
    return guid.str();
}

bool CopyIfDifferent(const fs::path& sourcePath, const fs::path& targetPath) {
    std::error_code ec;
    if (!fs::exists(sourcePath, ec)) {
        return false;
    }

    ec.clear();
    bool needsCopy = !fs::exists(targetPath, ec);
    if (!needsCopy) {
        ec.clear();
        const auto sourceSize = fs::file_size(sourcePath, ec);
        if (ec) {
            needsCopy = true;
        }
        ec.clear();
        const auto targetSize = fs::file_size(targetPath, ec);
        if (!ec && sourceSize != targetSize) {
            needsCopy = true;
        }
    }
    if (!needsCopy) {
        ec.clear();
        const auto sourceTime = fs::last_write_time(sourcePath, ec);
        if (!ec) {
            ec.clear();
            const auto targetTime = fs::last_write_time(targetPath, ec);
            if (!ec && targetTime < sourceTime) {
                needsCopy = true;
            }
        }
    }

    if (!needsCopy) {
        return true;
    }

    fs::create_directories(targetPath.parent_path(), ec);
    if (ec) {
        std::cerr << "[ProjectGenerator] Failed to create directory for " << targetPath << ": "
                  << ec.message() << std::endl;
        return false;
    }

    fs::copy_file(sourcePath, targetPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[ProjectGenerator] Failed to copy " << sourcePath << " to " << targetPath
                  << ": " << ec.message() << std::endl;
        return false;
    }

    return true;
}

bool WriteTextFileIfChanged(const fs::path& path, const std::string& contents) {
    {
        std::ifstream existing(path, std::ios::binary);
        if (existing) {
            std::ostringstream buffer;
            buffer << existing.rdbuf();
            if (buffer.str() == contents) {
                return true;
            }
        }
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[ProjectGenerator] Failed to write file: " << path << std::endl;
        return false;
    }

    out << contents;
    return static_cast<bool>(out);
}

fs::path GetExecutableDirectory() {
    wchar_t exePath[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return fs::current_path();
    }
    return fs::path(exePath).parent_path();
}

void SyncManagedReferenceAssembly(const fs::path& projectRoot) {
    const fs::path executableDir = GetExecutableDirectory();
    const fs::path sourceDll = executableDir / "ClaymoreEngine.dll";
    const fs::path sourcePdb = executableDir / "ClaymoreEngine.pdb";
    const fs::path libraryDir = projectRoot / ".library";

    if (!CopyIfDifferent(sourceDll, libraryDir / "ClaymoreEngine.dll")) {
        std::cerr << "[ProjectGenerator] Warning: ClaymoreEngine.dll was not found next to the editor executable at "
                  << sourceDll << std::endl;
    }

    CopyIfDifferent(sourcePdb, libraryDir / "ClaymoreEngine.pdb");
}

fs::path FindManagedEngineProjectFrom(const fs::path& startDirectory) {
    if (startDirectory.empty()) {
        return {};
    }

    std::error_code ec;
    fs::path current = startDirectory;
    for (int depth = 0; depth < 12; ++depth) {
        const fs::path candidate = current / "managed" / "ClaymoreEngine" / "ClaymoreEngine.csproj";
        if (fs::exists(candidate, ec)) {
            return candidate;
        }

        const fs::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }

        current = parent;
    }

    return {};
}

ManagedEngineReference ResolveManagedEngineReference(const fs::path& projectRoot) {
    const fs::path executableDir = GetExecutableDirectory();
    fs::path engineProjectPath = FindManagedEngineProjectFrom(executableDir);
    if (engineProjectPath.empty()) {
        engineProjectPath = FindManagedEngineProjectFrom(fs::current_path());
    }

    if (!engineProjectPath.empty()) {
        return {true, MakeRelativeHintPath(projectRoot, engineProjectPath)};
    }

    return {false, fs::path(".library") / "ClaymoreEngine.dll"};
}

std::vector<ReferenceEntry> CollectReferences(const fs::path& projectRoot,
                                              const std::vector<ProjectModuleRef>& modules) {
    std::vector<ReferenceEntry> references;
    std::unordered_set<std::string> seenHintPaths;

    for (const auto& module : modules) {
        if (!module.enabled || module.dll.empty()) {
            continue;
        }

        fs::path dllPath(module.dll);
        if (!dllPath.is_absolute()) {
            dllPath = projectRoot / dllPath;
        }

        const fs::path relativePath = MakeRelativeHintPath(projectRoot, dllPath);
        const std::string hintPath = relativePath.generic_string();
        if (!seenHintPaths.insert(hintPath).second) {
            continue;
        }

        if (!fs::exists(dllPath)) {
            std::cerr << "[ProjectGenerator] Warning: module DLL reference is missing: " << dllPath << std::endl;
        }

        references.push_back({dllPath.stem().string(), relativePath});
    }

    return references;
}

std::string BuildManagedScriptProject(const fs::path& projectRoot,
                                      const std::string& projectName,
                                      const std::vector<ProjectModuleRef>& modules) {
    const ManagedEngineReference engineReference = ResolveManagedEngineReference(projectRoot);
    const std::vector<ReferenceEntry> references = CollectReferences(projectRoot, modules);

    std::ostringstream csproj;
    csproj << "<Project Sdk=\"Microsoft.NET.Sdk\">\n";
    csproj << "  <PropertyGroup>\n";
    csproj << "    <TargetFramework>" << kManagedTargetFramework << "</TargetFramework>\n";
    csproj << "    <RuntimeFrameworkVersion>" << kManagedRuntimeVersion << "</RuntimeFrameworkVersion>\n";
    csproj << "    <OutputType>Library</OutputType>\n";
    csproj << "    <AssemblyName>" << kManagedAssemblyName << "</AssemblyName>\n";
    csproj << "    <RootNamespace>" << EscapeXml(SanitizeNamespace(projectName)) << "</RootNamespace>\n";
    csproj << "    <PlatformTarget>x64</PlatformTarget>\n";
    csproj << "    <Nullable>enable</Nullable>\n";
    csproj << "    <ImplicitUsings>false</ImplicitUsings>\n";
    csproj << "    <EnablePreviewFeatures>true</EnablePreviewFeatures>\n";
    csproj << "    <LangVersion>latest</LangVersion>\n";
    csproj << "    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n";
    csproj << "    <AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>\n";
    csproj << "    <DefaultItemExcludes>$(DefaultItemExcludes);bin/**;obj/**;.library/**;.git/**;.vs/**</DefaultItemExcludes>\n";
    csproj << "    <OutputPath>.library/</OutputPath>\n";
    csproj << "  </PropertyGroup>\n";
    csproj << "  <PropertyGroup Condition=\"'$(Configuration)' == 'Debug'\">\n";
    csproj << "    <DebugSymbols>true</DebugSymbols>\n";
    csproj << "    <DebugType>portable</DebugType>\n";
    csproj << "    <Optimize>false</Optimize>\n";
    csproj << "  </PropertyGroup>\n";
    csproj << "  <PropertyGroup Condition=\"'$(Configuration)' == 'Release'\">\n";
    csproj << "    <Optimize>true</Optimize>\n";
    csproj << "  </PropertyGroup>\n";

    if (engineReference.useProjectReference) {
        csproj << "  <ItemGroup>\n";
        csproj << "    <ProjectReference Include=\"" << EscapeXml(engineReference.path.generic_string()) << "\">\n";
        csproj << "      <Name>ClaymoreEngine</Name>\n";
        csproj << "      <Private>false</Private>\n";
        csproj << "    </ProjectReference>\n";
        csproj << "  </ItemGroup>\n";
    } else {
        csproj << "  <ItemGroup>\n";
        csproj << "    <Reference Include=\"ClaymoreEngine\">\n";
        csproj << "      <HintPath>" << EscapeXml(engineReference.path.generic_string()) << "</HintPath>\n";
        csproj << "      <Private>false</Private>\n";
        csproj << "    </Reference>\n";
        csproj << "  </ItemGroup>\n";
    }

    if (!references.empty()) {
        csproj << "  <ItemGroup>\n";
        for (const auto& reference : references) {
            csproj << "    <Reference Include=\"" << EscapeXml(reference.includeName) << "\">\n";
            csproj << "      <HintPath>" << EscapeXml(reference.hintPath.generic_string()) << "</HintPath>\n";
            csproj << "      <Private>false</Private>\n";
            csproj << "    </Reference>\n";
        }
        csproj << "  </ItemGroup>\n";
    }

    csproj << "  <ItemGroup>\n";
    csproj << "    <None Remove=\"**/*.anim\" />\n";
    csproj << "    <None Remove=\"**/*.meta\" />\n";
    csproj << "    <None Remove=\"**/*.skelbin\" />\n";
    csproj << "    <None Remove=\"**/*.meshbin\" />\n";
    csproj << "    <None Remove=\"**/*.avatar\" />\n";
    csproj << "    <Content Remove=\"**/*.anim\" />\n";
    csproj << "    <Content Remove=\"**/*.meta\" />\n";
    csproj << "    <Content Remove=\"**/*.skelbin\" />\n";
    csproj << "    <Content Remove=\"**/*.meshbin\" />\n";
    csproj << "    <Content Remove=\"**/*.avatar\" />\n";
    csproj << "  </ItemGroup>\n";
    csproj << "</Project>\n";

    return csproj.str();
}

std::string BuildManagedSolution(const fs::path& projectRoot, const std::string& projectName) {
    const ManagedEngineReference engineReference = ResolveManagedEngineReference(projectRoot);
    const std::string gameProjectGuid = MakeStableGuid(projectRoot, "GameScriptsProject");
    const std::string solutionGuid = MakeStableGuid(projectRoot, "ManagedSolution");

    struct SolutionProjectEntry {
        std::string name;
        fs::path path;
        std::string guid;
    };

    std::vector<SolutionProjectEntry> projects = {
        {projectName, fs::path(projectName + ".csproj"), gameProjectGuid}
    };

    if (engineReference.useProjectReference) {
        projects.push_back({"ClaymoreEngine", engineReference.path, MakeStableGuid(projectRoot, "ClaymoreEngineProject")});
    }

    std::ostringstream solution;
    solution << "Microsoft Visual Studio Solution File, Format Version 12.00\r\n";
    solution << "# Visual Studio Version 18\r\n";
    solution << "VisualStudioVersion = 18.0.0.0\r\n";
    solution << "MinimumVisualStudioVersion = 10.0.40219.1\r\n";

    for (const auto& project : projects) {
        fs::path solutionProjectPath = project.path;
        solutionProjectPath.make_preferred();
        solution << "Project(\"{" << kSdkStyleCSharpProjectTypeGuid << "}\") = \""
                 << project.name << "\", \""
                 << solutionProjectPath.string() << "\", \"{"
                 << project.guid << "}\"\r\n";
        solution << "EndProject\r\n";
    }

    solution << "Global\r\n";
    solution << "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\r\n";
    solution << "\t\tDebug|Any CPU = Debug|Any CPU\r\n";
    solution << "\t\tRelease|Any CPU = Release|Any CPU\r\n";
    solution << "\tEndGlobalSection\r\n";
    solution << "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\r\n";
    for (const auto& project : projects) {
        solution << "\t\t{" << project.guid << "}.Debug|Any CPU.ActiveCfg = Debug|Any CPU\r\n";
        solution << "\t\t{" << project.guid << "}.Debug|Any CPU.Build.0 = Debug|Any CPU\r\n";
        solution << "\t\t{" << project.guid << "}.Release|Any CPU.ActiveCfg = Release|Any CPU\r\n";
        solution << "\t\t{" << project.guid << "}.Release|Any CPU.Build.0 = Release|Any CPU\r\n";
    }
    solution << "\tEndGlobalSection\r\n";
    solution << "\tGlobalSection(SolutionProperties) = preSolution\r\n";
    solution << "\t\tHideSolutionNode = FALSE\r\n";
    solution << "\tEndGlobalSection\r\n";
    solution << "\tGlobalSection(ExtensibilityGlobals) = postSolution\r\n";
    solution << "\t\tSolutionGuid = {" << solutionGuid << "}\r\n";
    solution << "\tEndGlobalSection\r\n";
    solution << "EndGlobal\r\n";

    return solution.str();
}
} // namespace

namespace ProjectGenerator {
bool CreateBlankProjectInFolder(const fs::path& targetFolder) {
    std::error_code ec;
    if (!fs::exists(targetFolder)) {
        if (!fs::create_directories(targetFolder, ec)) {
            std::cerr << "[ProjectGenerator] Failed to create target folder: " << targetFolder << std::endl;
            return false;
        }
    }

    fs::create_directories(targetFolder / "assets", ec);
    fs::create_directories(targetFolder / "scenes", ec);
    fs::create_directories(targetFolder / "scripts", ec);
    fs::create_directories(targetFolder / "shaders", ec);

    const std::string projectName = targetFolder.filename().string();
    json projectJson = {
        {"name", projectName},
        {"version", 1},
        {"assetDirectory", "assets"}
    };

    std::ofstream out(targetFolder / (projectName + ".clayproj"));
    if (!out) {
        std::cerr << "[ProjectGenerator] Failed to write .clayproj at: " << targetFolder << std::endl;
        return false;
    }

    out << projectJson.dump(4);
    out.close();

    if (!EnsureManagedScriptProject(targetFolder, projectName)) {
        std::cerr << "[ProjectGenerator] Warning: managed script project generation failed for "
                  << targetFolder << std::endl;
    }

    return true;
}

bool CreateNewProject(const std::string& name, const fs::path& targetDir) {
    const fs::path projectRoot = targetDir / name;
    if (fs::exists(projectRoot)) {
        std::cerr << "[ProjectGenerator] Folder already exists: " << projectRoot << std::endl;
        return false;
    }

    fs::create_directories(projectRoot / "assets/textures");
    fs::create_directories(projectRoot / "assets/models");
    fs::create_directories(projectRoot / "assets/materials");
    fs::create_directories(projectRoot / "scenes");
    fs::create_directories(projectRoot / "scripts");
    fs::create_directories(projectRoot / "shaders");

    json projectJson;
    projectJson["name"] = name;
    projectJson["version"] = 1;
    projectJson["assetDirectory"] = "assets";
    projectJson["startScene"] = "scenes/main.scene";
    projectJson["renderer"] = {
        {"api", "Direct3D11"},
        {"vSync", true}
    };

    std::ofstream out(projectRoot / (name + ".clayproj"));
    out << projectJson.dump(4);
    out.close();

    std::ofstream mainScene(projectRoot / "scenes/main.scene");
    mainScene << "{}";
    mainScene.close();

    if (!EnsureManagedScriptProject(projectRoot, name)) {
        std::cerr << "[ProjectGenerator] Warning: managed script project generation failed for "
                  << projectRoot << std::endl;
    }

    std::cout << "[ProjectGenerator] Created new project at " << projectRoot << std::endl;
    return true;
}

bool EnsureManagedScriptProject(const fs::path& projectRoot, const std::string& projectName) {
    static const std::vector<ProjectModuleRef> kNoModules;
    return EnsureManagedScriptProject(projectRoot, projectName, kNoModules);
}

bool EnsureManagedScriptProject(const fs::path& projectRoot,
                                const std::string& projectName,
                                const std::vector<ProjectModuleRef>& modules) {
    if (projectRoot.empty() || projectName.empty()) {
        std::cerr << "[ProjectGenerator] Cannot generate managed script project without a project root and name."
                  << std::endl;
        return false;
    }

    std::error_code ec;
    fs::create_directories(projectRoot / ".library", ec);
    if (ec) {
        std::cerr << "[ProjectGenerator] Failed to create .library for " << projectRoot
                  << ": " << ec.message() << std::endl;
        return false;
    }

    SyncManagedReferenceAssembly(projectRoot);

    const fs::path csprojPath = projectRoot / (projectName + ".csproj");
    const std::string csprojContents = BuildManagedScriptProject(projectRoot, projectName, modules);
    if (!WriteTextFileIfChanged(csprojPath, csprojContents)) {
        return false;
    }

    const fs::path solutionPath = projectRoot / (projectName + ".sln");
    const std::string solutionContents = BuildManagedSolution(projectRoot, projectName);
    if (!WriteTextFileIfChanged(solutionPath, solutionContents)) {
        return false;
    }

    std::cout << "[ProjectGenerator] Ensured managed script project: " << csprojPath << std::endl;
    std::cout << "[ProjectGenerator] Ensured managed solution: " << solutionPath << std::endl;
    return true;
}
} // namespace ProjectGenerator
