#include "StandardMeshManager.h"
#include "VertexTypes.h"
#include <glm/glm.hpp>
#include <vector>
#include <cstdio>
#include "MaterialManager.h"
#ifndef CLAYMORE_RUNTIME
#include "editor/pipeline/AssetLibrary.h"
#endif

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/constants.hpp>

StandardMeshManager& StandardMeshManager::Instance() {
    static StandardMeshManager instance;
    return instance;
}

StandardMeshManager::~StandardMeshManager() {
    // Never call into bgfx from static destruction at process exit.
    // Application/Runtime shutdown should call StandardMeshManager::Shutdown()
    // while bgfx is still alive.
    if (!m_Shutdown) {
        m_CubeMesh.reset();
        m_PlaneMesh.reset();
        m_SphereMesh.reset();
        m_CapsuleMesh.reset();
        m_Shutdown = true;
    }
}

void StandardMeshManager::Shutdown() {
    if (m_Shutdown) {
        return;
    }
    auto destroyMesh = [](std::unique_ptr<Mesh>& mesh) {
        if (!mesh) return;
        if (bgfx::isValid(mesh->vbh)) {
            bgfx::destroy(mesh->vbh);
            mesh->vbh = BGFX_INVALID_HANDLE;
        }
        if (bgfx::isValid(mesh->ibh)) {
            bgfx::destroy(mesh->ibh);
            mesh->ibh = BGFX_INVALID_HANDLE;
        }
        mesh.reset();
    };

    destroyMesh(m_CubeMesh);
    destroyMesh(m_PlaneMesh);
    destroyMesh(m_SphereMesh);
    destroyMesh(m_CapsuleMesh);
    m_Shutdown = true;
}

std::shared_ptr<Mesh> StandardMeshManager::GetCubeMesh() {  
   if (!m_CubeMesh) CreateCubeMesh();  
   return std::shared_ptr<Mesh>(m_CubeMesh.get(), [](Mesh*) {}); // Non-owning shared_ptr
}

std::shared_ptr<Mesh> StandardMeshManager::GetPlaneMesh() {
    if (!m_PlaneMesh) CreatePlaneMesh();
    return std::shared_ptr<Mesh>(m_PlaneMesh.get(), [](Mesh*) {}); // Non-owning shared_ptr
}

std::shared_ptr<Mesh> StandardMeshManager::GetSphereMesh() {
    if (!m_SphereMesh) CreateSphereMesh();
    return std::shared_ptr<Mesh>(m_SphereMesh.get(), [](Mesh*) {}); // Non-owning shared_ptr
}

std::shared_ptr<Mesh> StandardMeshManager::GetPrimitiveMesh(PrimitiveMeshType type) {
    switch (type) {
        case PrimitiveMeshType::Cube:    return GetCubeMesh();
        case PrimitiveMeshType::Sphere:  return GetSphereMesh();
        case PrimitiveMeshType::Plane:   return GetPlaneMesh();
        case PrimitiveMeshType::Capsule: return GetCapsuleMesh();
        default: return nullptr;
    }
}

void StandardMeshManager::CreateCubeMesh() {
    static PBRVertex cubeVertices[] = {
        // Front
        {-1,  1,  1,  0, 0, 1,  0, 0},
        { 1,  1,  1,  0, 0, 1,  1, 0},
        {-1, -1,  1,  0, 0, 1,  0, 1},
        { 1, -1,  1,  0, 0, 1,  1, 1},

        // Back
        {-1,  1, -1,  0, 0, -1, 0, 0},
        { 1,  1, -1,  0, 0, -1, 1, 0},
        {-1, -1, -1,  0, 0, -1, 0, 1},
        { 1, -1, -1,  0, 0, -1, 1, 1},

        // Left
        {-1,  1, -1, -1, 0, 0,  0, 0},
        {-1,  1,  1, -1, 0, 0,  1, 0},
        {-1, -1, -1, -1, 0, 0,  0, 1},
        {-1, -1,  1, -1, 0, 0,  1, 1},

        // Right
        { 1,  1,  1,  1, 0, 0,  0, 0},
        { 1,  1, -1,  1, 0, 0,  1, 0},
        { 1, -1,  1,  1, 0, 0,  0, 1},
        { 1, -1, -1,  1, 0, 0,  1, 1},

        // Top
        {-1,  1, -1,  0, 1, 0,  0, 0},
        { 1,  1, -1,  0, 1, 0,  1, 0},
        {-1,  1,  1,  0, 1, 0,  0, 1},
        { 1,  1,  1,  0, 1, 0,  1, 1},

        // Bottom
        {-1, -1,  1,  0, -1, 0, 0, 0},
        { 1, -1,  1,  0, -1, 0, 1, 0},
        {-1, -1, -1,  0, -1, 0, 0, 1},
        { 1, -1, -1,  0, -1, 0, 1, 1}
    };

    static const uint16_t cubeIndices[] = {
        // Reverse winding to enforce clockwise
        0, 2, 1, 1, 2, 3,      // Front
        4, 5, 6, 5, 7, 6,      // Back
        8,10, 9, 9,10,11,      // Left
       12,14,13,13,14,15,      // Right
       16,18,17,17,18,19,      // Top
       20,22,21,21,22,23       // Bottom
    };


    m_CubeMesh = std::make_unique<Mesh>();

    m_CubeMesh->vbh = bgfx::createVertexBuffer(bgfx::makeRef(cubeVertices, sizeof(cubeVertices)), PBRVertex::layout);
    m_CubeMesh->ibh = bgfx::createIndexBuffer(bgfx::makeRef(cubeIndices, sizeof(cubeIndices)));

    // CPU-side storage for picking
    m_CubeMesh->Vertices.reserve(8);
    for (auto& v : cubeVertices) {
        m_CubeMesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    }

    size_t indexCount = sizeof(cubeIndices) / sizeof(uint16_t);
    m_CubeMesh->Indices.assign(cubeIndices, cubeIndices + indexCount);
    m_CubeMesh->numIndices = (uint32_t)indexCount;

    m_CubeMesh->ComputeBounds();
    printf("[StandardMeshManager] Cube Mesh created (PBR).\n");
}


void StandardMeshManager::CreatePlaneMesh() {
    static PBRVertex planeVertices[] = {
        // Front face (facing +Z)
        {-1,  1,  0,  0, 0, 1,  0, 0},  // Top-left
        { 1,  1,  0,  0, 0, 1,  1, 0},  // Top-right
        {-1, -1,  0,  0, 0, 1,  0, 1},  // Bottom-left
        { 1, -1,  0,  0, 0, 1,  1, 1}   // Bottom-right
    };

    static const uint16_t planeIndices[] = {
        // Reverse winding to enforce clockwise
        0, 2, 1, 1, 2, 3
    };

    m_PlaneMesh = std::make_unique<Mesh>();

    m_PlaneMesh->vbh = bgfx::createVertexBuffer(bgfx::makeRef(planeVertices, sizeof(planeVertices)), PBRVertex::layout);
    m_PlaneMesh->ibh = bgfx::createIndexBuffer(bgfx::makeRef(planeIndices, sizeof(planeIndices)));

    // CPU-side storage for picking
    m_PlaneMesh->Vertices.reserve(4);
    for (auto& v : planeVertices) {
        m_PlaneMesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    }

    size_t indexCount = sizeof(planeIndices) / sizeof(uint16_t);
    m_PlaneMesh->Indices.assign(planeIndices, planeIndices + indexCount);
    m_PlaneMesh->numIndices = (uint32_t)indexCount;

    m_PlaneMesh->ComputeBounds();
    printf("[StandardMeshManager] Plane Mesh created (PBR).\n");
}

void StandardMeshManager::CreateSphereMesh() {
    const int segments = 32;  // Horizontal segments
    const int rings = 16;     // Vertical rings
    const float radius = 1.0f;
    
    std::vector<PBRVertex> sphereVertices;
    std::vector<uint16_t> sphereIndices;
    
    // Generate vertices
    for (int ring = 0; ring <= rings; ++ring) {
        float phi = (float)ring / rings * glm::pi<float>();
        float y = radius * cos(phi);
        float ringRadius = radius * sin(phi);
        
        for (int segment = 0; segment <= segments; ++segment) {
            float theta = (float)segment / segments * 2.0f * glm::pi<float>();
            float x = ringRadius * cos(theta);
            float z = ringRadius * sin(theta);
            
            // Position
            float px = x;
            float py = y;
            float pz = z;
            
            // Normal (normalized position for unit sphere)
            float nx = x / radius;
            float ny = y / radius;
            float nz = z / radius;
            
            // UV coordinates
            float u = (float)segment / segments;
            float v = (float)ring / rings;
            
            sphereVertices.push_back({px, py, pz, nx, ny, nz, u, v});
        }
    }
    
    // Generate indices (reversed for clockwise winding)
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            uint16_t current = (uint16_t)(ring * (segments + 1) + segment);
            uint16_t next = (uint16_t)(current + segments + 1);
            
            // First triangle (reversed)
            sphereIndices.push_back(current);
            sphereIndices.push_back((uint16_t)(current + 1));
            sphereIndices.push_back(next);
            
            // Second triangle (reversed)
            sphereIndices.push_back(next);
            sphereIndices.push_back((uint16_t)(current + 1));
            sphereIndices.push_back((uint16_t)(next + 1));
        }
    }
    
    m_SphereMesh = std::make_unique<Mesh>();
    
    const bgfx::Memory* sphereVB = bgfx::copy(sphereVertices.data(), (uint32_t)(sphereVertices.size() * sizeof(PBRVertex)));
    const bgfx::Memory* sphereIB = bgfx::copy(sphereIndices.data(), (uint32_t)(sphereIndices.size() * sizeof(uint16_t)));
    m_SphereMesh->vbh = bgfx::createVertexBuffer(sphereVB, PBRVertex::layout);
    m_SphereMesh->ibh = bgfx::createIndexBuffer(sphereIB);
    m_SphereMesh->numVertices = (uint32_t)sphereVertices.size();
    
    // CPU-side storage for picking
    m_SphereMesh->Vertices.reserve(sphereVertices.size());
    for (auto& v : sphereVertices) {
        m_SphereMesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    }
    
    m_SphereMesh->Indices.assign(sphereIndices.begin(), sphereIndices.end());
    m_SphereMesh->numIndices = (uint32_t)sphereIndices.size();
    
    m_SphereMesh->ComputeBounds();
    printf("[StandardMeshManager] Sphere Mesh created (PBR) - %zu vertices, %zu indices.\n", 
           sphereVertices.size(), sphereIndices.size());
}

void StandardMeshManager::RegisterPrimitiveMeshes() {
#ifndef CLAYMORE_RUNTIME
    using Primitive = AssetReference::PrimitiveType;
    const Primitive primitives[] = {
        Primitive::Cube,
        Primitive::Sphere,
        Primitive::Plane,
        Primitive::Capsule
    };

    for (Primitive primitive : primitives) {
        const char* name = AssetReference::PrimitiveTypeToString(primitive);
        if (!name || !*name) continue;
        AssetReference ref = AssetReference::CreatePrimitive(name);
        AssetLibrary::Instance().RegisterAsset(ref, AssetType::Mesh, "", name);
    }

    std::cout << "[StandardMeshManager] Registered primitive meshes with AssetLibrary" << std::endl;
#else
    // In runtime, primitives are accessed directly without AssetLibrary registration
#endif
}

std::shared_ptr<Mesh> StandardMeshManager::GetCapsuleMesh() {
    if (!m_CapsuleMesh) CreateCapsuleMesh();
    return std::shared_ptr<Mesh>(m_CapsuleMesh.get(), [](Mesh*) {});
}

void StandardMeshManager::CreateCapsuleMesh() {
    const int segments = 32;      // Around Y axis
    const int ringsCap = 16;      // Per hemisphere
    const float radius = 0.5f;    // Match ~2.0 overall height with halfHeight below
    const float halfHeight = 0.5f;

    std::vector<PBRVertex> vertices;
    std::vector<uint16_t> indices;

    auto pushTriangleCW = [&](uint16_t a, uint16_t b, uint16_t c) {
        // Ensure clockwise: a, c, b would be reverse if we built CCW; here we build directly in CW
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
    };

    // Cylinder (two rings: bottom and top)
    int baseIndexCylinder = (int)vertices.size();
    for (int yStep = 0; yStep <= 1; ++yStep) {
        float y = (yStep == 0) ? -halfHeight : +halfHeight;
        float v = yStep == 0 ? 0.0f : 0.5f;
        for (int s = 0; s <= segments; ++s) {
            float u = (float)s / segments;
            float theta = u * glm::two_pi<float>();
            float cx = radius * cosf(theta);
            float cz = radius * sinf(theta);
            float nx = cosf(theta);
            float nz = sinf(theta);
            vertices.push_back({cx, y, cz, nx, 0.0f, nz, u, v});
        }
    }
    // Cylinder indices (clockwise). Two strips between the two rings
    for (int s = 0; s < segments; ++s) {
        uint16_t i0 = (uint16_t)(baseIndexCylinder + s);
        uint16_t i1 = (uint16_t)(baseIndexCylinder + s + 1);
        uint16_t i2 = (uint16_t)(baseIndexCylinder + (segments + 1) + s);
        uint16_t i3 = (uint16_t)(baseIndexCylinder + (segments + 1) + s + 1);
        // Two triangles per quad; enforce clockwise for outward-facing normals
        pushTriangleCW(i0, i2, i1);
        pushTriangleCW(i2, i3, i1);
    }

    // Top hemisphere (from equator to top pole)
    int baseIndexTop = (int)vertices.size();
    for (int r = 0; r <= ringsCap; ++r) {
        float t = (float)r / ringsCap;               // 0..1
        float phi = t * (glm::half_pi<float>());     // 0..pi/2
        float yLocal = radius * cosf(phi);           // radius..0
        float ringR = radius * sinf(phi);            // 0..radius
        float y = halfHeight + yLocal;
        float v = 0.5f + 0.5f * (1.0f - t);          // 1 at pole, 0.5 at equator
        for (int s = 0; s <= segments; ++s) {
            float u = (float)s / segments;
            float theta = u * glm::two_pi<float>();
            float x = ringR * cosf(theta);
            float z = ringR * sinf(theta);
            // Normal from cap center
            float nx = (ringR > 0.0f || yLocal > 0.0f) ? cosf(theta) * sinf(phi) : 0.0f;
            float ny = (ringR > 0.0f || yLocal > 0.0f) ? cosf(phi) : 1.0f;
            float nz = (ringR > 0.0f || yLocal > 0.0f) ? sinf(theta) * sinf(phi) : 0.0f;
            // Normalize normal just in case
            glm::vec3 n = glm::normalize(glm::vec3(nx, ny, nz));
            vertices.push_back({x, y, z, n.x, n.y, n.z, u, v});
        }
    }
    for (int r = 0; r < ringsCap; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint16_t curr = (uint16_t)(baseIndexTop + r * (segments + 1) + s);
            uint16_t next = (uint16_t)(curr + segments + 1);
            // Clockwise - reversed from bottom hemisphere since top bulges upward (opposite direction)
            pushTriangleCW(curr, (uint16_t)(curr + 1), next);
            pushTriangleCW(next, (uint16_t)(curr + 1), (uint16_t)(next + 1));
        }
    }

    // Bottom hemisphere (from bottom pole to equator)
    int baseIndexBottom = (int)vertices.size();
    for (int r = 0; r <= ringsCap; ++r) {
        float t = (float)r / ringsCap;               // 0..1
        float phi = t * (glm::half_pi<float>());     // 0..pi/2
        float yLocal = radius * cosf(phi);           // radius..0
        float ringR = radius * sinf(phi);            // 0..radius
        float y = -halfHeight - yLocal;
        float v = 0.5f * (1.0f - (1.0f - t));        // 0 at pole, 0.5 at equator
        for (int s = 0; s <= segments; ++s) {
            float u = (float)s / segments;
            float theta = u * glm::two_pi<float>();
            float x = ringR * cosf(theta);
            float z = ringR * sinf(theta);
            // Normal from cap center
            float nx = (ringR > 0.0f || yLocal > 0.0f) ? cosf(theta) * sinf(phi) : 0.0f;
            float ny = (ringR > 0.0f || yLocal > 0.0f) ? -cosf(phi) : -1.0f;
            float nz = (ringR > 0.0f || yLocal > 0.0f) ? sinf(theta) * sinf(phi) : 0.0f;
            glm::vec3 n = glm::normalize(glm::vec3(nx, ny, nz));
            vertices.push_back({x, y, z, n.x, n.y, n.z, u, v});
        }
    }
    for (int r = 0; r < ringsCap; ++r) {
        for (int s = 0; s < segments; ++s) {
            uint16_t curr = (uint16_t)(baseIndexBottom + r * (segments + 1) + s);
            uint16_t next = (uint16_t)(curr + segments + 1);
            // Clockwise - match top hemisphere pattern for consistent outward-facing normals
            pushTriangleCW(curr, next, (uint16_t)(curr + 1));
            pushTriangleCW(next, (uint16_t)(next + 1), (uint16_t)(curr + 1));
        }
    }

    m_CapsuleMesh = std::make_unique<Mesh>();
    const bgfx::Memory* capsuleVB = bgfx::copy(vertices.data(), (uint32_t)(vertices.size() * sizeof(PBRVertex)));
    const bgfx::Memory* capsuleIB = bgfx::copy(indices.data(), (uint32_t)(indices.size() * sizeof(uint16_t)));
    m_CapsuleMesh->vbh = bgfx::createVertexBuffer(capsuleVB, PBRVertex::layout);
    m_CapsuleMesh->ibh = bgfx::createIndexBuffer(capsuleIB);
    m_CapsuleMesh->numVertices = (uint32_t)vertices.size();

    // CPU-side storage for picking
    m_CapsuleMesh->Vertices.reserve(vertices.size());
    for (auto& v : vertices) m_CapsuleMesh->Vertices.push_back(glm::vec3(v.x, v.y, v.z));
    m_CapsuleMesh->Indices.assign(indices.begin(), indices.end());
    m_CapsuleMesh->numIndices = (uint32_t)indices.size();

    m_CapsuleMesh->ComputeBounds();
    printf("[StandardMeshManager] Capsule Mesh created (PBR) - %zu vertices, %zu indices.\n",
        vertices.size(), indices.size());
}
