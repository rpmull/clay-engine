// PhysicsDebug.cpp
#include "PhysicsDebug.h"
#include <algorithm>

namespace PhysicsDebug {

static bool s_Enabled = false;
static std::vector<DebugLine> s_Lines;
static std::mutex s_Mutex;

void SetEnabled(bool enabled) {
    s_Enabled = enabled;
    if (!enabled) {
        Clear();
    }
}

bool IsEnabled() {
    return s_Enabled;
}

void AddLine(const glm::vec3& from, const glm::vec3& to, uint32_t color, float lifetime) {
    if (!s_Enabled) return;
    
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Lines.push_back({ from, to, color, lifetime, false });
}

void AddRaycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                bool hit, const glm::vec3& hitPoint, const glm::vec3& hitNormal) {
    if (!s_Enabled) return;
    
    std::lock_guard<std::mutex> lock(s_Mutex);
    
    // Normalize direction for consistent visualization
    glm::vec3 dir = glm::length(direction) > 1e-6f ? glm::normalize(direction) : direction;
    glm::vec3 fullEndPoint = origin + dir * maxDistance;
    
    if (hit) {
        // HIT: Draw green line from origin to hit point
        s_Lines.push_back({ origin, hitPoint, kColorRayHit, 0.0f, true });
        
        // Draw remainder as dim red (where ray would have continued)
        float remainderDist = glm::length(fullEndPoint - hitPoint);
        if (remainderDist > 0.01f) {
            s_Lines.push_back({ hitPoint, fullEndPoint, 0x400000FF, 0.0f, false }); // Dim red
        }
        
        // Draw hit point marker (larger cross for visibility)
        float markerSize = 0.15f;
        s_Lines.push_back({ hitPoint - glm::vec3(markerSize, 0, 0), hitPoint + glm::vec3(markerSize, 0, 0), kColorHitPoint, 0.0f, true });
        s_Lines.push_back({ hitPoint - glm::vec3(0, markerSize, 0), hitPoint + glm::vec3(0, markerSize, 0), kColorHitPoint, 0.0f, true });
        s_Lines.push_back({ hitPoint - glm::vec3(0, 0, markerSize), hitPoint + glm::vec3(0, 0, markerSize), kColorHitPoint, 0.0f, true });
        
        // Draw hit normal (magenta arrow)
        if (glm::length(hitNormal) > 0.001f) {
            s_Lines.push_back({ hitPoint, hitPoint + hitNormal * 0.5f, kColorHitNormal, 0.0f, true });
        }
        
        // Draw origin marker (small yellow cross)
        float originMarker = 0.08f;
        uint32_t kColorOrigin = 0xFF00FFFF; // Yellow
        s_Lines.push_back({ origin - glm::vec3(originMarker, 0, 0), origin + glm::vec3(originMarker, 0, 0), kColorOrigin, 0.0f, true });
        s_Lines.push_back({ origin - glm::vec3(0, originMarker, 0), origin + glm::vec3(0, originMarker, 0), kColorOrigin, 0.0f, true });
        s_Lines.push_back({ origin - glm::vec3(0, 0, originMarker), origin + glm::vec3(0, 0, originMarker), kColorOrigin, 0.0f, true });
    } else {
        // MISS: Draw full red line from origin to end
        s_Lines.push_back({ origin, fullEndPoint, kColorRayMiss, 0.0f, false });
    }
}

void AddLinecast(const glm::vec3& from, const glm::vec3& to,
                 bool hit, const glm::vec3& hitPoint, const glm::vec3& hitNormal) {
    if (!s_Enabled) return;
    
    std::lock_guard<std::mutex> lock(s_Mutex);
    
    if (hit) {
        // HIT: Draw green line from origin to hit point
        s_Lines.push_back({ from, hitPoint, kColorRayHit, 0.0f, true });
        
        // Draw remainder as dim red (where ray would have continued)
        float remainderDist = glm::length(to - hitPoint);
        if (remainderDist > 0.01f) {
            s_Lines.push_back({ hitPoint, to, 0x400000FF, 0.0f, false }); // Dim red
        }
        
        // Draw hit point marker (larger cross for visibility)
        float markerSize = 0.15f;
        s_Lines.push_back({ hitPoint - glm::vec3(markerSize, 0, 0), hitPoint + glm::vec3(markerSize, 0, 0), kColorHitPoint, 0.0f, true });
        s_Lines.push_back({ hitPoint - glm::vec3(0, markerSize, 0), hitPoint + glm::vec3(0, markerSize, 0), kColorHitPoint, 0.0f, true });
        s_Lines.push_back({ hitPoint - glm::vec3(0, 0, markerSize), hitPoint + glm::vec3(0, 0, markerSize), kColorHitPoint, 0.0f, true });
        
        // Draw hit normal (magenta arrow)
        if (glm::length(hitNormal) > 0.001f) {
            s_Lines.push_back({ hitPoint, hitPoint + hitNormal * 0.5f, kColorHitNormal, 0.0f, true });
        }
        
        // Draw origin marker (small yellow cross to show where cast started)
        float originMarker = 0.08f;
        uint32_t kColorOrigin = 0xFF00FFFF; // Yellow
        s_Lines.push_back({ from - glm::vec3(originMarker, 0, 0), from + glm::vec3(originMarker, 0, 0), kColorOrigin, 0.0f, true });
        s_Lines.push_back({ from - glm::vec3(0, originMarker, 0), from + glm::vec3(0, originMarker, 0), kColorOrigin, 0.0f, true });
        s_Lines.push_back({ from - glm::vec3(0, 0, originMarker), from + glm::vec3(0, 0, originMarker), kColorOrigin, 0.0f, true });
    } else {
        // MISS: Draw full red line from origin to target
        s_Lines.push_back({ from, to, kColorRayMiss, 0.0f, false });
    }
}

const std::vector<DebugLine>& GetLines() {
    return s_Lines;
}

void Tick(float deltaTime) {
    std::lock_guard<std::mutex> lock(s_Mutex);
    
    // Remove expired lines
    s_Lines.erase(
        std::remove_if(s_Lines.begin(), s_Lines.end(), [deltaTime](DebugLine& line) {
            if (line.lifetime <= 0.0f) {
                return true; // Single frame line, remove
            }
            line.lifetime -= deltaTime;
            return line.lifetime <= 0.0f;
        }),
        s_Lines.end()
    );
}

void Clear() {
    std::lock_guard<std::mutex> lock(s_Mutex);
    s_Lines.clear();
}

} // namespace PhysicsDebug

