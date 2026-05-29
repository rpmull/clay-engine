#pragma once
#include <string>
#include "ParticleSystem.h"

namespace particles
{
    // Acquire a sprite handle for the given image path. This uses a ref-counted cache
    // to prevent atlas exhaustion when sprites are reassigned frequently.
    ps::EmitterSpriteHandle AcquireSprite(const std::string& path, bool flipY = true);
    
    // Release a previously acquired sprite handle. When the last reference is released,
    // the sprite remains cached to avoid atlas fragmentation (cleared on ClearSpriteCache).
    void ReleaseSprite(ps::EmitterSpriteHandle handle);

    // Invalidate cached sprite entries for a specific source path so they reload on next use.
    // Live emitters will notice the stale handle and reacquire from SpritePath.
    void InvalidateSpriteCache(const std::string& path);
    
    // Backward-compatible helper (now ref-counted). Prefer AcquireSprite/ReleaseSprite.
    ps::EmitterSpriteHandle LoadSprite(const std::string& path, bool flipY = true);
    
    // Clears the sprite cache and destroys sprites in the atlas.
    // Call this when shutting down or when sprites need to be reloaded.
    void ClearSpriteCache();
}
