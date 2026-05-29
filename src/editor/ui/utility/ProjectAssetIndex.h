#pragma once

#include <initializer_list>
#include <string>
#include <vector>

namespace ui {

struct ProjectAssetEntry {
    std::string name;
    std::string absolutePath;
    std::string projectRelativePath;
    std::string normalizedAbsolutePath;
    std::string normalizedProjectRelativePath;
    std::string extensionLower;
};

struct ProjectAssetQuery {
    std::vector<std::string> extensions;
    std::vector<std::string> suffixes;
};

ProjectAssetQuery MakeExtensionQuery(std::initializer_list<const char*> extensions);
ProjectAssetQuery MakeSuffixQuery(std::initializer_list<const char*> suffixes);

const std::vector<ProjectAssetEntry>& GetProjectAssetEntries(const ProjectAssetQuery& query);
void InvalidateProjectAssetIndex();

} // namespace ui
