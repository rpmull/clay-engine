#pragma once
#include <string>
#include "core/assets/AssetMetadata.h"

class AssetMetadataIO {
public:
    static bool Load(const std::string& metaPath, AssetMetadata& outMeta);
    static void Save(const std::string& metaPath, const AssetMetadata& meta);
};
