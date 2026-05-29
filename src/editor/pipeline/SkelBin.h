#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

namespace skelbin {

static constexpr uint32_t SKEL_BIN_MAGIC = 'B' | ('L'<<8) | ('E'<<16) | ('S'<<24); // 'SELB'
// v2 adds bone name string table
static constexpr uint32_t SKEL_BIN_VERSION = 2;

struct PackedSkeleton {
    std::vector<glm::mat4> inverseBindPoses;
    std::vector<int> boneParents;         // -1 for root
    std::vector<std::string> boneNames;   // debug/editor
};

bool WriteSkelBin(const PackedSkeleton& s, const std::string& filePath);
bool ReadSkelBin(const std::string& filePath, PackedSkeleton& out);

}


