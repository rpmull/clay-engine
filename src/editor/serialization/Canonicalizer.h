#pragma once
#include <string>
#include "DiffTypes.h"
#include "core/ecs/Scene.h"

namespace cm { namespace editor {

// Canonical string IO
SerializedBlob SerializeSceneToBlob(Scene& scene); // forward-declared elsewhere; provide wrapper in cpp
SerializedBlob Canonicalize(const SerializedBlob& in);
SerializedBlob ParseCanonical(const std::string& s);
void DeserializeSceneFromBlob(const SerializedBlob& blob, Scene& outScene);

}} // namespace cm::editor


