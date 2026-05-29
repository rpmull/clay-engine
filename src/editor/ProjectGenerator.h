#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ProjectModuleRef;

namespace ProjectGenerator {
    bool CreateBlankProjectInFolder(const std::filesystem::path& targetFolder);
    bool CreateNewProject(const std::string& name, const std::filesystem::path& targetDir);
    bool EnsureManagedScriptProject(const std::filesystem::path& projectRoot, const std::string& projectName);
    bool EnsureManagedScriptProject(const std::filesystem::path& projectRoot,
                                    const std::string& projectName,
                                    const std::vector<ProjectModuleRef>& modules);

} // namespace ProjectGenerator
