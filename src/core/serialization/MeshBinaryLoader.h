#pragma once
#include <string>
#include <memory>
#include <vector>
#include <cstdint>

struct Mesh;
struct BlendShapeComponent;

// Runtime-safe mesh binary loader
// Loads meshes from .meshbin files via VFS (no editor dependencies)
namespace MeshBinaryLoader {

// Magic and version must match editor's meshbin format
static constexpr uint32_t MESH_BIN_MAGIC = 'B' | ('H'<<8) | ('S'<<16) | ('M'<<24);
static constexpr uint32_t MESH_BIN_VERSION = 9;

// Load a specific submesh from a meshbin file
// Uses VFS/FileSystem to read from PAK or disk
// Returns nullptr on failure
std::shared_ptr<Mesh> LoadMesh(const std::string& meshBinPath, uint32_t submeshIndex, bool* outSkinned = nullptr);

// Load blend shapes for a submesh
std::unique_ptr<BlendShapeComponent> LoadBlendShapes(const std::string& meshBinPath, uint32_t submeshIndex);

// Get submesh count from meshbin
uint32_t GetSubmeshCount(const std::string& meshBinPath);

// Check if meshbin file exists and is valid
bool IsValidMeshBin(const std::string& meshBinPath);

} // namespace MeshBinaryLoader

