#pragma once

#pragma once

#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <nlohmann/json_fwd.hpp>

#include "core/ecs/Entity.h"

struct Model;
struct PreparedModel;
struct SkeletonComponent;
class Scene;

namespace DebugModelDump
{
bool ShouldDump(const std::string& sourcePath, const std::string& fallbackPath);

std::string ResolveDumpKey(const std::string& sourcePath, const std::string& fallbackPath);

void WriteDump(const std::string& key,
               const char* stage,
               const nlohmann::json& payload);

void DumpPreparedModel(const Model& model,
                       const PreparedModel& prepared,
                       const char* stage);

void DumpSkinningPose(const SkeletonComponent& skeleton,
                      const std::vector<glm::mat4>& boneWorld,
                      const std::vector<glm::mat4>& palette);

} // namespace DebugModelDump

