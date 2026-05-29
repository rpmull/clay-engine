#include "SplineUtils.h"
#include <algorithm>
#include <cmath>

namespace cm {
namespace spline {

std::vector<glm::vec3> SampleSpline(
    const std::vector<SplinePathPoint>& controlPoints,
    int subdivision,
    bool closed)
{
    std::vector<glm::vec3> result;
    if (controlPoints.empty()) return result;
    if (controlPoints.size() == 1) {
        result.push_back(controlPoints[0].Position);
        return result;
    }

    const int sub = std::max(1, subdivision);
    const size_t n = controlPoints.size();

    if (n < 4 && !closed) {
        // Linear interpolation
        for (size_t i = 0; i < n - 1; ++i) {
            const auto& a = controlPoints[i].Position;
            const auto& b = controlPoints[i + 1].Position;
            for (int j = 0; j <= sub; ++j) {
                float t = static_cast<float>(j) / static_cast<float>(sub);
                result.push_back(glm::mix(a, b, t));
            }
        }
        result.push_back(controlPoints.back().Position);
        return result;
    }

    auto getPoint = [&](int idx) -> glm::vec3 {
        if (closed) {
            int i = idx % static_cast<int>(n);
            if (i < 0) i += static_cast<int>(n);
            return controlPoints[static_cast<size_t>(i)].Position;
        }
        idx = std::clamp(idx, 0, static_cast<int>(n) - 1);
        return controlPoints[static_cast<size_t>(idx)].Position;
    };

    const size_t segCount = closed ? n : n - 1;
    for (size_t seg = 0; seg < segCount; ++seg) {
        int i0 = closed ? (static_cast<int>(seg) - 1 + static_cast<int>(n)) % static_cast<int>(n)
                        : (seg == 0 ? 0 : static_cast<int>(seg) - 1);
        int i1 = static_cast<int>(seg);
        int i2 = (seg + 1) % static_cast<int>(n);
        int i3 = (seg + 2) % static_cast<int>(n);
        if (!closed) {
            i2 = std::min(i2, static_cast<int>(n) - 1);
            i3 = std::min(i3, static_cast<int>(n) - 1);
        }

        glm::vec3 p0 = getPoint(i0);
        glm::vec3 p1 = getPoint(i1);
        glm::vec3 p2 = getPoint(i2);
        glm::vec3 p3 = getPoint(i3);

        for (int j = 0; j <= sub; ++j) {
            if (j == sub && seg < segCount - 1) continue;
            float t = static_cast<float>(j) / static_cast<float>(sub);
            result.push_back(CatmullRom(p0, p1, p2, p3, t));
        }
    }
    return result;
}

bool GetPointAtNormalized(
    const std::vector<SplinePathPoint>& controlPoints,
    int subdivision,
    bool closed,
    float t,
    glm::vec3& outLocalPosition)
{
    std::vector<glm::vec3> sampled = SampleSpline(controlPoints, subdivision, closed);
    if (sampled.empty()) return false;

    t = std::clamp(t, 0.0f, 1.0f);
    if (sampled.size() == 1) {
        outLocalPosition = sampled[0];
        return true;
    }

    // Build cumulative arc lengths
    std::vector<float> lengths;
    lengths.reserve(sampled.size());
    lengths.push_back(0.0f);
    float total = 0.0f;
    for (size_t i = 1; i < sampled.size(); ++i) {
        total += glm::length(sampled[i] - sampled[i - 1]);
        lengths.push_back(total);
    }

    if (total < 1e-9f) {
        outLocalPosition = sampled[0];
        return true;
    }

    float targetDist = t * total;
    size_t seg = 0;
    for (size_t i = 0; i + 1 < lengths.size(); ++i) {
        if (targetDist <= lengths[i + 1]) {
            seg = i;
            break;
        }
        seg = i;
    }
    if (seg >= sampled.size() - 1) {
        outLocalPosition = sampled.back();
        return true;
    }

    float segStart = lengths[seg];
    float segEnd = lengths[seg + 1];
    float segLen = segEnd - segStart;
    float u = (segLen > 1e-9f) ? (targetDist - segStart) / segLen : 0.0f;
    u = std::clamp(u, 0.0f, 1.0f);
    outLocalPosition = glm::mix(sampled[seg], sampled[seg + 1], u);
    return true;
}

NearestResult GetNearestPoint(
    const std::vector<glm::vec3>& sampledPoints,
    const glm::vec3& worldQueryPos,
    const glm::mat4& entityWorldMatrix)
{
    NearestResult best;
    best.Distance = std::numeric_limits<float>::max();

    if (sampledPoints.empty()) return best;

    glm::mat4 invWorld = glm::inverse(entityWorldMatrix);
    glm::vec3 queryLocal = glm::vec3(invWorld * glm::vec4(worldQueryPos, 1.0f));

    for (size_t i = 0; i < sampledPoints.size(); ++i) {
        float d = glm::length(sampledPoints[i] - queryLocal);
        if (d < best.Distance) {
            best.Distance = d;
            best.LocalPosition = sampledPoints[i];
            best.WorldPosition = glm::vec3(entityWorldMatrix * glm::vec4(sampledPoints[i], 1.0f));
            best.SegmentIndex = i;
        }
    }
    return best;
}

} // namespace spline
} // namespace cm
