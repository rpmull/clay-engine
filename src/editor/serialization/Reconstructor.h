#pragma once
#include "DiffTypes.h"
#include "core/ecs/Scene.h"

namespace cm { namespace editor {

struct ReconstructionOptions {
	bool enforcePrefabOverrides = true;
	bool resolveExternalAssets = true;
	bool strictParenting = true;
};

Scene ReconstructFromSerialized(const SerializedBlob& in, const ReconstructionOptions& opts);

}} // namespace cm::editor


