// PhysicsDebug.h
#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <mutex>

namespace PhysicsDebug {

// Debug line with color and lifetime
struct DebugLine {
    glm::vec3 from;
    glm::vec3 to;
    uint32_t color;     // ABGR format for bgfx
    float lifetime;     // Seconds remaining, <= 0 means single frame
    bool hit;           // Whether this was a hit (for visual distinction)
};

// Colors (ABGR format)
constexpr uint32_t kColorRayMiss   = 0xFF0000FF;  // Red - ray missed
constexpr uint32_t kColorRayHit    = 0xFF00FF00;  // Green - ray hit
constexpr uint32_t kColorHitPoint  = 0xFFFFFF00;  // Cyan - hit point marker
constexpr uint32_t kColorHitNormal = 0xFFFF00FF;  // Magenta - hit normal

// Enable/disable debug visualization
void SetEnabled(bool enabled);
bool IsEnabled();

// Add a debug line (will be drawn for one frame or until lifetime expires)
void AddLine(const glm::vec3& from, const glm::vec3& to, uint32_t color, float lifetime = 0.0f);

// Add raycast visualization (automatically chooses hit/miss color)
void AddRaycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, 
                bool hit, const glm::vec3& hitPoint = glm::vec3(0), const glm::vec3& hitNormal = glm::vec3(0));

// Add linecast visualization (point-to-point)
void AddLinecast(const glm::vec3& from, const glm::vec3& to,
                 bool hit, const glm::vec3& hitPoint = glm::vec3(0), const glm::vec3& hitNormal = glm::vec3(0));

// Get all pending lines for rendering (called by Renderer)
const std::vector<DebugLine>& GetLines();

// Clear single-frame lines and decrement lifetimes (called each frame after rendering)
void Tick(float deltaTime);

// Clear all debug lines immediately
void Clear();

} // namespace PhysicsDebug

