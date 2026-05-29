#pragma once
#include <bgfx/bgfx.h>
#include <memory>
#include <unordered_map>
#include <string>
#include <cstdint>
#include "Mesh.h"

// Forward declare the primitive type enum to avoid circular dependencies
// Must match the definition in AssetReference.h
enum class PrimitiveMeshType : uint8_t {
    Cube = 0,
    Sphere = 1,
    Plane = 2,
    Capsule = 3,
    Unknown = 0xFF
};

class StandardMeshManager {
public:
    static StandardMeshManager& Instance();

    // Accessors for standard meshes
    std::shared_ptr<Mesh> GetCubeMesh();
    std::shared_ptr<Mesh> GetPlaneMesh();
    std::shared_ptr<Mesh> GetSphereMesh();
    std::shared_ptr<Mesh> GetCapsuleMesh();
    
    // Get primitive mesh by type enum
    std::shared_ptr<Mesh> GetPrimitiveMesh(PrimitiveMeshType type);
    
    // Register primitive meshes with AssetLibrary
    void RegisterPrimitiveMeshes();
    // Explicitly release primitive GPU buffers while bgfx is still active.
    void Shutdown();

private:
    StandardMeshManager() = default;
    ~StandardMeshManager();

    void CreateCubeMesh();
    void CreatePlaneMesh();
    void CreateSphereMesh();
    void CreateCapsuleMesh();

    std::unique_ptr<Mesh> m_CubeMesh;
    std::unique_ptr<Mesh> m_PlaneMesh;
    std::unique_ptr<Mesh> m_SphereMesh;
    std::unique_ptr<Mesh> m_CapsuleMesh;
    bool m_Shutdown = false;
};
