#include "editor/Project.h"

#include "core/vfs/FileSystem.h"

#include <filesystem>

bool Project::Load(const std::filesystem::path& path) {
    s_ProjectFile = path;
    s_ProjectDir = path.parent_path();
    s_AssetDir = s_ProjectDir / "assets";
    s_ProjectName = path.stem().string();
    return !s_ProjectDir.empty();
}

bool Project::Save() {
    return false;
}

bool Project::RenameProject(const std::string& name) {
    if (name.empty()) {
        return false;
    }

    s_ProjectName = name;
    return true;
}

const std::filesystem::path& Project::GetProjectDirectory() {
    if (s_ProjectDir.empty()) {
        const std::filesystem::path& projectRoot = FileSystem::Instance().GetProjectRoot();
        if (!projectRoot.empty()) {
            s_ProjectDir = projectRoot;
        } else {
            std::error_code ec;
            s_ProjectDir = std::filesystem::current_path(ec);
        }
    }

    if (s_AssetDir.empty() && !s_ProjectDir.empty()) {
        s_AssetDir = s_ProjectDir / "assets";
    }

    return s_ProjectDir;
}

const std::filesystem::path& Project::GetAssetDirectory() {
    if (s_AssetDir.empty()) {
        s_AssetDir = GetProjectDirectory() / "assets";
    }

    return s_AssetDir;
}

const std::string& Project::GetProjectName() {
    if (s_ProjectName.empty()) {
        const std::filesystem::path& projectDir = GetProjectDirectory();
        if (!projectDir.empty()) {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(projectDir, ec)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (entry.path().extension() == ".pak") {
                    s_ProjectName = entry.path().stem().string();
                    break;
                }
            }
            if (s_ProjectName.empty()) {
                s_ProjectName = projectDir.filename().string();
            }
        }
        if (s_ProjectName.empty()) {
            s_ProjectName = "ClaymoreGame";
        }
    }

    return s_ProjectName;
}

const std::filesystem::path& Project::GetProjectFile() {
    return s_ProjectFile;
}
