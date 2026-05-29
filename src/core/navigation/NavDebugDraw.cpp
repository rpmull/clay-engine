#include "core/navigation/NavDebugDraw.h"
#include "core/navigation/NavMesh.h"
#include "core/rendering/Renderer.h"
#include "core/rendering/ShaderManager.h"
#include "core/rendering/VertexTypes.h"
#include <unordered_map>
#include <cmath>
#include <cfloat>

using namespace nav;

static std::atomic<uint32_t> sMask{ (uint32_t)NavDrawMask::None };
static std::atomic<float> sDebugOffset{ 0.15f }; // Default offset above terrain for visibility
static std::atomic<float> sDrawDistance{ 100.0f }; // Max distance for debug rendering

void nav::debug::SetMask(NavDrawMask mask) { sMask.store((uint32_t)mask); }
NavDrawMask nav::debug::GetMask() { return (NavDrawMask)sMask.load(); }

void nav::debug::SetOffset(float offset) { sDebugOffset.store(offset); }
float nav::debug::GetOffset() { return sDebugOffset.load(); }

void nav::debug::SetDrawDistance(float distance) { sDrawDistance.store(distance); }
float nav::debug::GetDrawDistance() { return sDrawDistance.load(); }

void nav::debug::InvalidateCache(const NavMeshRuntime* /*rt*/) {
    // No longer using static caches - we build dynamic buffers per frame
    // based on camera position for distance culling
}

// Helper to pack RGBA to ABGR as used by PosColorVertex
static uint32_t PackABGR(float r, float g, float b, float a) {
    uint8_t R = (uint8_t)(glm::clamp(r, 0.0f, 1.0f) * 255.0f);
    uint8_t G = (uint8_t)(glm::clamp(g, 0.0f, 1.0f) * 255.0f);
    uint8_t B = (uint8_t)(glm::clamp(b, 0.0f, 1.0f) * 255.0f);
    uint8_t A = (uint8_t)(glm::clamp(a, 0.0f, 1.0f) * 255.0f);
    return (uint32_t(A) << 24) | (uint32_t(B) << 16) | (uint32_t(G) << 8) | uint32_t(R);
}

// Check if AABB is within distance of camera (XZ plane distance for terrain)
static bool BoundsNearCamera(const Bounds& b, const glm::vec3& cam, float maxDist) {
    // Find closest point on AABB to camera
    glm::vec3 closest = glm::clamp(cam, b.min, b.max);
    // Use XZ distance for terrain-like navmeshes
    float dx = closest.x - cam.x;
    float dz = closest.z - cam.z;
    return (dx * dx + dz * dz) <= maxDist * maxDist;
}

void nav::debug::DrawRuntime(const NavMeshRuntime& rt, const glm::vec3& cameraPos, uint16_t viewId)
{
    const uint32_t mask = sMask.load();
    if (!(mask & (uint32_t)NavDrawMask::TriMesh) && !(mask & (uint32_t)NavDrawMask::Polys)) return;
    if (rt.m_Vertices.empty() || rt.m_Polys.empty()) return;

    // Lazy-load shader
    static bgfx::ProgramHandle sColorProgram = BGFX_INVALID_HANDLE;
    if (!bgfx::isValid(sColorProgram)) {
        PosColorVertex::Init();
        sColorProgram = ShaderManager::Instance().LoadProgram("vs_navcolor", "fs_navcolor");
    }
    if (!bgfx::isValid(sColorProgram)) return;

    const float baseOffset = sDebugOffset.load();
    const float drawDist = sDrawDistance.load();
    const float drawDistSq = drawDist * drawDist;
    
    // Collect visible triangles using BVH traversal
    std::vector<uint32_t> visiblePolys;
    visiblePolys.reserve(std::min(rt.m_Polys.size(), size_t(50000))); // Cap for performance
    
    if (!rt.m_BVH.empty() && drawDist > 0) {
        // BVH-accelerated culling
        std::vector<uint32_t> stack;
        stack.reserve(64);
        stack.push_back(0);
        
        while (!stack.empty() && visiblePolys.size() < 50000) {
            uint32_t nodeIdx = stack.back();
            stack.pop_back();
            
            if (nodeIdx >= rt.m_BVH.size()) continue;
            const auto& node = rt.m_BVH[nodeIdx];
            
            // Check if node bounds are within draw distance
            if (!BoundsNearCamera(node.b, cameraPos, drawDist)) continue;
            
            if (node.count > 0) {
                // Leaf node - add triangles
                for (uint32_t i = node.start; i < node.start + node.count && visiblePolys.size() < 50000; ++i) {
                    if (i < rt.m_BVHIndices.size()) {
                        uint32_t polyIdx = rt.m_BVHIndices[i];
                        if (polyIdx < rt.m_Polys.size()) {
                            // Additional per-triangle distance check
                            const auto& p = rt.m_Polys[polyIdx];
                            if (p.i0 >= rt.m_Vertices.size() || p.i1 >= rt.m_Vertices.size() || p.i2 >= rt.m_Vertices.size()) {
                                continue;
                            }
                            glm::vec3 center = (rt.m_Vertices[p.i0] + rt.m_Vertices[p.i1] + rt.m_Vertices[p.i2]) / 3.0f;
                            float dx = center.x - cameraPos.x;
                            float dz = center.z - cameraPos.z;
                            if (dx * dx + dz * dz <= drawDistSq) {
                                visiblePolys.push_back(polyIdx);
                            }
                        }
                    }
                }
            } else {
                // Internal node - push children
                if (node.left != UINT32_MAX) stack.push_back(node.left);
                if (node.right != UINT32_MAX) stack.push_back(node.right);
            }
        }
    } else {
        // No BVH or unlimited distance - brute force with distance check
        for (uint32_t i = 0; i < rt.m_Polys.size() && visiblePolys.size() < 50000; ++i) {
            if (drawDist <= 0) {
                visiblePolys.push_back(i);
            } else {
                const auto& p = rt.m_Polys[i];
                if (p.i0 >= rt.m_Vertices.size() || p.i1 >= rt.m_Vertices.size() || p.i2 >= rt.m_Vertices.size()) {
                    continue;
                }
                glm::vec3 center = (rt.m_Vertices[p.i0] + rt.m_Vertices[p.i1] + rt.m_Vertices[p.i2]) / 3.0f;
                float dx = center.x - cameraPos.x;
                float dz = center.z - cameraPos.z;
                if (dx * dx + dz * dz <= drawDistSq) {
                    visiblePolys.push_back(i);
                }
            }
        }
    }
    
    if (visiblePolys.empty()) return;
    
    // Build vertices for visible triangles
    std::vector<PosColorVertex> polyVerts;
    std::vector<uint32_t> polyIndices;
    const bool useCostColors = (mask & (uint32_t)NavDrawMask::Costs) != 0;
    const uint32_t polyColor = PackABGR(0.1f, 0.5f, 0.9f, 0.4f);

    float costMin = FLT_MAX;
    float costMax = 0.0f;
    if (useCostColors) {
        for (uint32_t polyIdx : visiblePolys) {
            costMin = std::min(costMin, rt.m_Polys[polyIdx].cost);
            costMax = std::max(costMax, rt.m_Polys[polyIdx].cost);
        }
        if (costMax <= costMin) { costMax = costMin + 0.01f; }
    }

    auto costColor = [&](float cost) {
        float t = (cost - costMin) / (costMax - costMin);
        t = glm::clamp(t, 0.0f, 1.0f);
        // Green (low) -> Red (high)
        return PackABGR(1.0f * t, 1.0f * (1.0f - t), 0.1f, 0.45f);
    };

    if (useCostColors) {
        polyVerts.reserve(visiblePolys.size() * 3);
        polyIndices.reserve(visiblePolys.size() * 3);
        uint32_t idx = 0;
        for (uint32_t polyIdx : visiblePolys) {
            const auto& p = rt.m_Polys[polyIdx];
            if (p.i0 >= rt.m_Vertices.size() || p.i1 >= rt.m_Vertices.size() || p.i2 >= rt.m_Vertices.size()) {
                continue;
            }
            uint32_t color = costColor(p.cost);
            const glm::vec3& v0 = rt.m_Vertices[p.i0];
            const glm::vec3& v1 = rt.m_Vertices[p.i1];
            const glm::vec3& v2 = rt.m_Vertices[p.i2];
            polyVerts.push_back({ v0.x, v0.y + baseOffset, v0.z, color });
            polyVerts.push_back({ v1.x, v1.y + baseOffset, v1.z, color });
            polyVerts.push_back({ v2.x, v2.y + baseOffset, v2.z, color });
            polyIndices.push_back(idx++);
            polyIndices.push_back(idx++);
            polyIndices.push_back(idx++);
        }
    } else {
        std::unordered_map<uint32_t, uint32_t> vertexRemap; // original index -> new index
        auto getOrAddVertex = [&](uint32_t origIdx) -> uint32_t {
            if (origIdx >= rt.m_Vertices.size()) return UINT32_MAX;
            auto it = vertexRemap.find(origIdx);
            if (it != vertexRemap.end()) return it->second;
            
            uint32_t newIdx = (uint32_t)polyVerts.size();
            vertexRemap[origIdx] = newIdx;
            
            const glm::vec3& v = rt.m_Vertices[origIdx];
            // Simple upward offset for visibility
            PosColorVertex pv;
            pv.x = v.x;
            pv.y = v.y + baseOffset;
            pv.z = v.z;
            pv.abgr = polyColor;
            polyVerts.push_back(pv);
            return newIdx;
        };
        
        for (uint32_t polyIdx : visiblePolys) {
            const auto& p = rt.m_Polys[polyIdx];
            uint32_t i0 = getOrAddVertex(p.i0);
            uint32_t i1 = getOrAddVertex(p.i1);
            uint32_t i2 = getOrAddVertex(p.i2);
            if (i0 == UINT32_MAX || i1 == UINT32_MAX || i2 == UINT32_MAX) continue;
            polyIndices.push_back(i0);
            polyIndices.push_back(i1);
            polyIndices.push_back(i2);
        }
    }
    
    float identity[16];
    bx::mtxIdentity(identity);

    // Draw filled polys
    if ((mask & (uint32_t)NavDrawMask::Polys) && !polyVerts.empty()) {
        const bgfx::Memory* vmem = bgfx::copy(polyVerts.data(), (uint32_t)(polyVerts.size() * sizeof(PosColorVertex)));
        const bgfx::Memory* imem = bgfx::copy(polyIndices.data(), (uint32_t)(polyIndices.size() * sizeof(uint32_t)));
        bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, PosColorVertex::layout);
        bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(imem, BGFX_BUFFER_INDEX32);
        
        bgfx::setTransform(identity);
        bgfx::setVertexBuffer(0, vbh);
        bgfx::setIndexBuffer(ibh);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(viewId, sColorProgram);
        
        bgfx::destroy(vbh);
        bgfx::destroy(ibh);
    }

    // Draw wireframe
    if (mask & (uint32_t)NavDrawMask::TriMesh) {
        const uint32_t lineColor = PackABGR(0.3f, 0.8f, 1.0f, 1.0f);
        const float wireOffset = baseOffset + 0.02f;
        
        std::vector<PosColorVertex> lines;
        lines.reserve(visiblePolys.size() * 6);
        
        for (uint32_t polyIdx : visiblePolys) {
            const auto& p = rt.m_Polys[polyIdx];
            if (p.i0 >= rt.m_Vertices.size() || p.i1 >= rt.m_Vertices.size() || p.i2 >= rt.m_Vertices.size()) {
                continue;
            }
            glm::vec3 a = rt.m_Vertices[p.i0]; a.y += wireOffset;
            glm::vec3 b = rt.m_Vertices[p.i1]; b.y += wireOffset;
            glm::vec3 c = rt.m_Vertices[p.i2]; c.y += wireOffset;
            
            lines.push_back({ a.x, a.y, a.z, lineColor });
            lines.push_back({ b.x, b.y, b.z, lineColor });
            lines.push_back({ b.x, b.y, b.z, lineColor });
            lines.push_back({ c.x, c.y, c.z, lineColor });
            lines.push_back({ c.x, c.y, c.z, lineColor });
            lines.push_back({ a.x, a.y, a.z, lineColor });
        }
        
        if (!lines.empty()) {
            const bgfx::Memory* vmem = bgfx::copy(lines.data(), (uint32_t)(lines.size() * sizeof(PosColorVertex)));
            bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vmem, PosColorVertex::layout);
            
            bgfx::setTransform(identity);
            bgfx::setVertexBuffer(0, vbh);
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES);
            bgfx::submit(viewId, sColorProgram);
            
            bgfx::destroy(vbh);
        }
    }
}

void nav::debug::DrawPath(const NavPath& path, uint16_t viewId)
{
    if (!(sMask.load() & (uint32_t)NavDrawMask::Path)) return;
    for (size_t i = 1; i < path.points.size(); ++i) {
        glm::vec3 a = path.points[i-1]; glm::vec3 b = path.points[i];
        Renderer::Get().DrawDebugRay(a, b - a, 1.0f);
    }
}

void nav::debug::DrawAgent(const NavAgentComponent& agent, const glm::vec3& pos, const glm::vec3& vel, uint16_t viewId)
{
    if (!(sMask.load() & (uint32_t)NavDrawMask::Agents)) return;
    // Draw velocity vector
    Renderer::Get().DrawDebugRay(pos, vel, 1.0f);
}

void nav::debug::DrawPortals(const std::vector<std::pair<glm::vec3, glm::vec3>>& portals, uint16_t viewId)
{
    if (!(sMask.load() & (uint32_t)NavDrawMask::Links)) return;
    const float offset = sDebugOffset.load();
    for (const auto& p : portals) {
        glm::vec3 a = p.first;
        glm::vec3 b = p.second;
        a.y += offset;
        b.y += offset;
        Renderer::Get().DrawDebugRay(a, b - a, 1.0f);
    }
}


