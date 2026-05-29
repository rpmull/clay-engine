#pragma once
#include <glm/glm.hpp>
#include "core/ecs/Scene.h"

class Camera;

class NavLinkPainter
{
public:
    // Call every frame from editor when in edit mode
    static void Update(Scene& scene, EntityID selectedEntity, bool playMode, bool viewportHovered, bool allowSelectionChanges, Camera* viewportCamera);
    static bool IsPaintModeEnabled();
    static void SetPaintModeEnabled(bool enabled);
    static bool TogglePaintMode();
};
