#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include <glm/glm.hpp>
#include "core/ecs/Scene.h"

namespace snapshot_utils {

struct SnapshotViewFit {
    glm::vec3 viewDir{0.0f, 0.0f, 1.0f};
    glm::vec3 upDir{0.0f, 1.0f, 0.0f};
    float halfWidth = 0.5f;
    float halfHeight = 0.5f;
};

inline bool ComputeModelWorldBounds(Scene& scene, EntityID rootId, glm::vec3& outMin, glm::vec3& outMax) {
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    bool hasBounds = false;

    std::function<void(EntityID)> visit = [&](EntityID id) {
        auto* d = scene.GetEntityData(id);
        if (!d || !d->Visible || !d->Active) return;
        if (d->Mesh && d->Mesh->mesh) {
            const glm::vec3 lmin = d->Mesh->mesh->BoundsMin;
            const glm::vec3 lmax = d->Mesh->mesh->BoundsMax;
            if (lmax.x > lmin.x && lmax.y > lmin.y && lmax.z > lmin.z) {
                const glm::mat4& M = d->Transform.WorldMatrix;
                const glm::vec3 corners[8] = {
                    {lmin.x, lmin.y, lmin.z}, {lmax.x, lmin.y, lmin.z},
                    {lmin.x, lmax.y, lmin.z}, {lmax.x, lmax.y, lmin.z},
                    {lmin.x, lmin.y, lmax.z}, {lmax.x, lmin.y, lmax.z},
                    {lmin.x, lmax.y, lmax.z}, {lmax.x, lmax.y, lmax.z}
                };
                for (const auto& c : corners) {
                    glm::vec3 w = glm::vec3(M * glm::vec4(c, 1.0f));
                    outMin = glm::min(outMin, w);
                    outMax = glm::max(outMax, w);
                }
                hasBounds = true;
            }
        }
        for (EntityID child : d->Children) {
            visit(child);
        }
    };

    visit(rootId);
    if (!hasBounds) {
        outMin = glm::vec3(-0.5f);
        outMax = glm::vec3(0.5f);
    }
    return hasBounds;
}

inline SnapshotViewFit ComputeSnapshotViewFit(const glm::vec3& extents) {
    SnapshotViewFit fit;
    const float ex = std::max(extents.x, 0.0001f);
    const float ey = std::max(extents.y, 0.0001f);
    const float ez = std::max(extents.z, 0.0001f);

    int viewAxis = 0;
    if (ey <= ex && ey <= ez) viewAxis = 1;
    else if (ez <= ex && ez <= ey) viewAxis = 2;

    int axisA = (viewAxis + 1) % 3;
    int axisB = (viewAxis + 2) % 3;
    float sizeA = (axisA == 0) ? ex : (axisA == 1 ? ey : ez);
    float sizeB = (axisB == 0) ? ex : (axisB == 1 ? ey : ez);
    int upAxis = (sizeA >= sizeB) ? axisA : axisB;
    int rightAxis = (upAxis == axisA) ? axisB : axisA;

    fit.viewDir = (viewAxis == 0) ? glm::vec3(1.0f, 0.0f, 0.0f)
                 : (viewAxis == 1) ? glm::vec3(0.0f, 1.0f, 0.0f)
                                   : glm::vec3(0.0f, 0.0f, 1.0f);
    fit.upDir = (upAxis == 0) ? glm::vec3(1.0f, 0.0f, 0.0f)
               : (upAxis == 1) ? glm::vec3(0.0f, 1.0f, 0.0f)
                               : glm::vec3(0.0f, 0.0f, 1.0f);

    fit.halfWidth = (rightAxis == 0) ? ex : (rightAxis == 1 ? ey : ez);
    fit.halfHeight = (upAxis == 0) ? ex : (upAxis == 1 ? ey : ez);
    return fit;
}

inline void FlipImageVertical(uint8_t* pixels, int width, int height, int channels) {
    if (!pixels || width <= 0 || height <= 0 || channels <= 0) return;
    const int stride = width * channels;
    std::vector<uint8_t> row(static_cast<size_t>(stride));
    for (int y = 0; y < height / 2; ++y) {
        uint8_t* top = pixels + y * stride;
        uint8_t* bottom = pixels + (height - 1 - y) * stride;
        std::memcpy(row.data(), top, stride);
        std::memcpy(top, bottom, stride);
        std::memcpy(bottom, row.data(), stride);
    }
}

inline bool ApplySnapshotAlphaKey(uint8_t* pixels, int width, int height, uint8_t keyR, uint8_t keyG, uint8_t keyB, uint8_t threshold = 2) {
    if (!pixels || width <= 0 || height <= 0) return false;
    const int stride = width * 4;
    bool anyNonKey = false;
    for (int y = 0; y < height; ++y) {
        uint8_t* row = pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            uint8_t* px = row + x * 4;
            const uint8_t r = px[0];
            const uint8_t g = px[1];
            const uint8_t b = px[2];
            const bool keyMatch = (std::abs(int(r) - int(keyR)) <= threshold) &&
                                  (std::abs(int(g) - int(keyG)) <= threshold) &&
                                  (std::abs(int(b) - int(keyB)) <= threshold);
            if (keyMatch) {
                px[3] = 0;
            } else {
                px[3] = 255;
                anyNonKey = true;
            }
        }
    }
    return anyNonKey;
}

} // namespace snapshot_utils
