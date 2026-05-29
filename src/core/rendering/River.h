#pragma once

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <glm/glm.hpp>

struct RiverComponent;
struct Mesh;
class Scene;

class River
{
public:
    // Binary asset serialization for river meshes
    static bool SaveRiverMeshAsset(RiverComponent& river, 
                                    const std::vector<glm::vec3>& vertices,
                                    const std::vector<glm::vec3>& normals, 
                                    const std::vector<glm::vec2>& uvs,
                                    const std::vector<uint32_t>& indices);
    
    static bool LoadRiverMeshAsset(const std::string& assetPath, 
                                    std::shared_ptr<Mesh>& outMesh);
    
    // Regenerate mesh from path points (for editor when asset not available)
    static std::shared_ptr<Mesh> GenerateMeshFromPath(
        const RiverComponent& river,
        float terrainMaxHeight,
        const std::function<float(float, float)>& sampleTerrainHeight);
    
    // Restore river meshes after scene load - loads mesh assets and assigns to entities
    static void RestoreRiverMeshes(Scene& scene);
};

