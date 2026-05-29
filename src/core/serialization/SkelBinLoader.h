#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <glm/glm.hpp>

// Forward declaration
struct SkeletonComponent;

namespace cm {

// Runtime skeleton binary loader
// Loads .skelbin files via VFS for runtime model instantiation
namespace SkelBinLoader {

static constexpr uint32_t SKEL_BIN_MAGIC = 'B' | ('L'<<8) | ('E'<<16) | ('S'<<24); // 'SELB'
static constexpr uint32_t SKEL_BIN_VERSION = 2;

// Load skeleton from .skelbin file into a SkeletonComponent
// Returns nullptr on failure
std::unique_ptr<SkeletonComponent> Load(const std::string& skelBinPath);

} // namespace SkelBinLoader

} // namespace cm


