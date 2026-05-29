#pragma once

#include <string>
#include "core/rendering/MaterialSource.h"

namespace embedded_textures
{
    // Ensure the texture spec references a persisted asset on disk when embedded data is present.
    // modelNameHint controls the folder under assets/textures/ where extracted files are stored.
    TextureSpecifier ExternalizeTexture(const TextureSpecifier& tex, const std::string& modelNameHint);

    // Apply ExternalizeTexture to every map within the material source.
    MaterialSource ExternalizeMaterialSource(const MaterialSource& src, const std::string& modelNameHint);
}


