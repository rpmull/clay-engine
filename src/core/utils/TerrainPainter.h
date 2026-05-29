#pragma once
#include <glm/glm.hpp>
#include "core/ecs/Scene.h"
#include "core/input/Input.h"

class Camera;

class TerrainPainter
{
public:
    // Call every frame from editor when in edit mode
    static void Update(Scene& scene, EntityID selectedEntity, bool playMode, bool viewportHovered, bool allowSelectionChanges, Camera* viewportCamera);
    static bool IsPainting();
    static void SetBrushModeEnabled(bool enabled);
    static bool IsBrushModeEnabled();
    static bool ToggleBrushMode();
};