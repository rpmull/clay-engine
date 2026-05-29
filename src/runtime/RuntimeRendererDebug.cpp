#include "core/rendering/Renderer.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtx/norm.hpp>

#include <cmath>
#include <vector>

namespace {
constexpr uint16_t kRuntimeDebugOverlayViewId = 3;
}

void Renderer::DrawRing(const glm::vec3& center, const glm::vec3& normal, float radius, uint32_t abgrColor)
{
    if (!bgfx::isValid(m_DebugLineProgram)) return;
    glm::vec3 n = glm::length2(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 reference = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 tangent = glm::normalize(glm::cross(reference, n));
    glm::vec3 bitangent = glm::normalize(glm::cross(n, tangent));
    const float hoverOffset = 0.1f;
    const glm::vec3 ringCenter = center + n * hoverOffset;

    const int segments = 64;
    std::vector<PosColorVertex> verts;
    verts.reserve(segments * 2);
    for (int i = 0; i < segments; ++i)
    {
        float a0 = (float)i / segments * glm::two_pi<float>();
        float a1 = (float)(i + 1) / segments * glm::two_pi<float>();
        glm::vec3 p0 = ringCenter + (tangent * std::cos(a0) + bitangent * std::sin(a0)) * radius;
        glm::vec3 p1 = ringCenter + (tangent * std::cos(a1) + bitangent * std::sin(a1)) * radius;
        verts.push_back({ p0.x, p0.y, p0.z, abgrColor });
        verts.push_back({ p1.x, p1.y, p1.z, abgrColor });
    }

    if (verts.empty()) return;
    PosColorVertex::Init();
    const bgfx::Memory* mem = bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(PosColorVertex)));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, PosColorVertex::layout);
    float identity[16]; bx::mtxIdentity(identity);
    bgfx::setTransform(identity);
    bgfx::setVertexBuffer(0, vbh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_PT_LINES | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_LINEAA);
    glm::vec4 ringColor(0.95f, 0.95f, 1.0f, 0.85f);
    ApplyDebugLineColor(ringColor);
    bgfx::submit(kRuntimeDebugOverlayViewId, m_DebugLineProgram);
    ApplyDefaultDebugLineColor();
    bgfx::destroy(vbh);
}

void Renderer::DrawFilledCircle(const glm::vec3& center, const glm::vec3& normal, float radius, uint32_t abgrColor)
{
    if (!bgfx::isValid(m_DebugLineProgram)) return;
    glm::vec3 n = glm::length2(normal) > 1e-4f ? glm::normalize(normal) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 reference = std::abs(n.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 tangent = glm::normalize(glm::cross(reference, n));
    glm::vec3 bitangent = glm::normalize(glm::cross(n, tangent));
    const float hoverOffset = 0.05f;
    const glm::vec3 circleCenter = center + n * hoverOffset;

    const int segments = 32;
    std::vector<PosColorVertex> verts;
    verts.reserve(segments * 3);
    for (int i = 0; i < segments; ++i) {
        float a0 = (float)i / segments * glm::two_pi<float>();
        float a1 = (float)(i + 1) / segments * glm::two_pi<float>();
        glm::vec3 p0 = circleCenter + (tangent * std::cos(a0) + bitangent * std::sin(a0)) * radius;
        glm::vec3 p1 = circleCenter + (tangent * std::cos(a1) + bitangent * std::sin(a1)) * radius;
        verts.push_back({ circleCenter.x, circleCenter.y, circleCenter.z, abgrColor });
        verts.push_back({ p0.x, p0.y, p0.z, abgrColor });
        verts.push_back({ p1.x, p1.y, p1.z, abgrColor });
    }

    if (verts.empty()) return;
    PosColorVertex::Init();
    const bgfx::Memory* mem = bgfx::copy(verts.data(), uint32_t(verts.size() * sizeof(PosColorVertex)));
    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(mem, PosColorVertex::layout);
    float identity[16]; bx::mtxIdentity(identity);
    bgfx::setTransform(identity);
    bgfx::setVertexBuffer(0, vbh);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_ALPHA);
    float r = ((abgrColor) & 0xFF) / 255.0f;
    float g = ((abgrColor >> 8) & 0xFF) / 255.0f;
    float b = ((abgrColor >> 16) & 0xFF) / 255.0f;
    float a = ((abgrColor >> 24) & 0xFF) / 255.0f;
    ApplyDebugLineColor(glm::vec4(r, g, b, a));
    bgfx::submit(kRuntimeDebugOverlayViewId, m_DebugLineProgram);
    ApplyDefaultDebugLineColor();
    bgfx::destroy(vbh);
}
