#pragma once

#include "core/ecs/Components.h"
#include <glm/glm.hpp>
#include <vector>

namespace cm {
namespace spline {

// Sample a Catmull-Rom spline from control points. Returns sampled positions in local space.
// subdivision: number of segments between each pair of control points
// closed: if true, connect last to first
std::vector<glm::vec3> SampleSpline(
    const std::vector<SplinePathPoint>& controlPoints,
    int subdivision,
    bool closed);

// Get point at normalized position along spline (arc-length parameterization).
// t in [0,1]: 0 = start, 1 = end, 0.5 = midpoint by distance.
// Returns position in local space. Returns false if spline is empty.
bool GetPointAtNormalized(
    const std::vector<SplinePathPoint>& controlPoints,
    int subdivision,
    bool closed,
    float t,
    glm::vec3& outLocalPosition);

// Get nearest point on sampled spline to a given world position.
// entityWorldMatrix: transform of the spline entity (local -> world)
// Returns: (nearest local position, distance, segment index)
struct NearestResult {
    glm::vec3 LocalPosition{0.0f};
    glm::vec3 WorldPosition{0.0f};
    float Distance = 0.0f;
    size_t SegmentIndex = 0;
};
NearestResult GetNearestPoint(
    const std::vector<glm::vec3>& sampledPoints,
    const glm::vec3& worldQueryPos,
    const glm::mat4& entityWorldMatrix);

// Catmull-Rom interpolation (shared with RiverCutterPanel logic)
inline glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1,
                           const glm::vec3& p2, const glm::vec3& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

} // namespace spline
} // namespace cm
