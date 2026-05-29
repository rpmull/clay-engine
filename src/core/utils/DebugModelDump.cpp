#include "core/utils/DebugModelDump.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include "core/ecs/AnimationComponents.h"
#include "editor/import/ModelLoader.h"
#include "editor/import/ModelPreprocessor.h"

using json = nlohmann::json;

namespace
{
struct DebugConfig
{
    bool Initialized = false;
    bool Enabled = false;
    bool MatchAll = false;
    std::vector<std::string> Targets;
    std::filesystem::path OutputDir;
};

std::string ToLower(std::string value)
{
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string NormalizeKey(const std::string& pathOrName)
{
    if (pathOrName.empty())
    {
        return {};
    }
    std::filesystem::path p(pathOrName);
    std::string stem = p.filename().string();
    if (stem.empty())
    {
        stem = p.string();
    }
    return ToLower(stem);
}

std::string SanitizeFilename(const std::string& input)
{
    std::string result = input;
    for (char& c : result)
    {
        const bool allowed = std::isalnum(static_cast<unsigned char>(c)) ||
            c == '_' || c == '-' || c == '.';
        if (!allowed)
        {
            c = '_';
        }
    }
    return result;
}

void EnsureConfig(DebugConfig& config)
{
    if (config.Initialized)
    {
        return;
    }
    config.Initialized = true;
    const char* env = std::getenv("CLAYMORE_DEBUG_MODEL_DUMPS");
    if (!env || !env[0])
    {
        return;
    }
    config.Enabled = true;
    std::string tokens = env;
    if (tokens == "*")
    {
        config.MatchAll = true;
    }
    else
    {
        std::stringstream ss(tokens);
        std::string item;
        while (std::getline(ss, item, ';'))
        {
            if (!item.empty())
            {
                config.Targets.push_back(NormalizeKey(item));
            }
        }
    }
    const char* dirEnv = std::getenv("CLAYMORE_DEBUG_MODEL_DUMP_DIR");
    if (dirEnv && dirEnv[0])
    {
        config.OutputDir = dirEnv;
    }
    else
    {
        config.OutputDir = std::filesystem::current_path() / "debug" / "model_dumps";
    }
    std::error_code ec;
    std::filesystem::create_directories(config.OutputDir, ec);
}

DebugConfig& GetConfig()
{
    static DebugConfig cfg;
    EnsureConfig(cfg);
    return cfg;
}

bool MatchesTarget(const DebugConfig& cfg, const std::string& normalizedKey)
{
    if (!cfg.Enabled)
    {
        return false;
    }
    if (cfg.MatchAll || cfg.Targets.empty())
    {
        return true;
    }
    if (normalizedKey.empty())
    {
        return false;
    }
    return std::any_of(cfg.Targets.begin(), cfg.Targets.end(),
        [&](const std::string& candidate)
        {
            return normalizedKey == candidate;
        });
}

json SerializeMatrix(const glm::mat4& m)
{
    json arr = json::array();
    const float* ptr = glm::value_ptr(m);
    for (int i = 0; i < 16; ++i)
    {
        arr.push_back(ptr[i]);
    }
    return arr;
}

json SerializeMatrixArray(const std::vector<glm::mat4>& mats)
{
    json outer = json::array();
    for (const auto& mat : mats)
    {
        outer.push_back(SerializeMatrix(mat));
    }
    return outer;
}

std::filesystem::path BuildOutputPath(const std::string& baseName,
                                      const std::string& stage)
{
    DebugConfig& cfg = GetConfig();
    std::string sanitized = SanitizeFilename(baseName + "." + stage + ".json");
    return cfg.OutputDir / sanitized;
}

void WriteJson(const std::filesystem::path& path, const json& payload)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        return;
    }
    file << payload.dump(2);
}

std::string ChooseSourceLabel(const std::string& preferred,
                              const std::string& fallback)
{
    if (!preferred.empty())
    {
        return preferred;
    }
    return fallback;
}

} // namespace

namespace DebugModelDump
{

bool ShouldDump(const std::string& sourcePath, const std::string& fallbackPath)
{
    DebugConfig& cfg = GetConfig();
    if (!cfg.Enabled)
    {
        return false;
    }
    const std::string key = NormalizeKey(ChooseSourceLabel(sourcePath, fallbackPath));
    return MatchesTarget(cfg, key);
}

std::string ResolveDumpKey(const std::string& sourcePath, const std::string& fallbackPath)
{
    return NormalizeKey(ChooseSourceLabel(sourcePath, fallbackPath));
}

void WriteDump(const std::string& key,
               const char* stage,
               const nlohmann::json& payload)
{
    if (key.empty())
    {
        return;
    }
    std::string effectiveStage = stage ? stage : "dump";
    WriteJson(BuildOutputPath(key, effectiveStage), payload);
}

void DumpPreparedModel(const Model& model,
                       const PreparedModel& prepared,
                       const char* stage)
{
    if (!ShouldDump(model.SourcePath, model.ModelName))
    {
        return;
    }

    json j;
    j["stage"] = stage ? stage : "prepared";
    j["sourcePath"] = model.SourcePath;
    j["modelName"] = model.ModelName;
    j["rootLocal"] = SerializeMatrix(prepared.RootLocal);

    json meshes = json::array();
    for (const auto& mesh : prepared.Meshes)
    {
        json m;
        m["name"] = mesh.NodeName;
        m["skinned"] = mesh.Skinned;
        m["transform"] = SerializeMatrix(mesh.LocalTransform);
        m["sourceIndices"] = mesh.SourceMeshIndices;
        m["materialCount"] = mesh.Materials.size();
        meshes.push_back(std::move(m));
    }
    j["meshes"] = std::move(meshes);

    json proxies = json::array();
    for (const auto& proxy : prepared.Proxies)
    {
        json p;
        p["name"] = proxy.NodeName;
        p["meshIndex"] = proxy.MeshEntryIndex;
        p["skinned"] = proxy.Skinned;
        p["transform"] = SerializeMatrix(proxy.LocalTransform);
        p["slots"] = proxy.SubmeshSlots;
        proxies.push_back(std::move(p));
    }
    j["proxies"] = std::move(proxies);

    json skeleton;
    skeleton["hasSkeleton"] = prepared.Skeleton.HasSkeleton;
    skeleton["boneNames"] = prepared.Skeleton.BoneNames;
    skeleton["boneParents"] = prepared.Skeleton.BoneParents;
    skeleton["inverseBindPoses"] = SerializeMatrixArray(prepared.Skeleton.InverseBindPoses);
    j["skeleton"] = std::move(skeleton);

    const std::string key = ResolveDumpKey(model.SourcePath, model.ModelName);
    WriteDump(key, j["stage"].get<std::string>().c_str(), j);
}

void DumpSkinningPose(const SkeletonComponent& skeleton,
                      const std::vector<glm::mat4>& boneWorld,
                      const std::vector<glm::mat4>& palette)
{
    if (skeleton.DebugSourcePath.empty())
    {
        return;
    }
    json j;
    j["stage"] = skeleton.DebugStageHint.empty() ? "skinning" : skeleton.DebugStageHint;
    j["sourcePath"] = skeleton.DebugSourcePath;

    json worldArr = json::array();
    for (size_t i = 0; i < boneWorld.size(); ++i)
    {
        json entry;
        entry["boneIndex"] = i;
        if (i < skeleton.BoneNames.size())
        {
            entry["boneName"] = skeleton.BoneNames[i];
        }
        entry["world"] = SerializeMatrix(boneWorld[i]);
        if (i < palette.size())
        {
            entry["palette"] = SerializeMatrix(palette[i]);
        }
        worldArr.push_back(std::move(entry));
    }
    j["bones"] = std::move(worldArr);

    const std::string key = skeleton.DebugSourcePath;
    WriteDump(key, j["stage"].get<std::string>().c_str(), j);
}

} // namespace DebugModelDump

