#include "IAssetResolver.h"

// Global resolver instance
namespace {
    IAssetResolver* g_AssetResolver = nullptr;
}

namespace Assets {
    IAssetResolver* GetResolver() {
        return g_AssetResolver;
    }
    
    void SetResolver(IAssetResolver* resolver) {
        g_AssetResolver = resolver;
    }
}

