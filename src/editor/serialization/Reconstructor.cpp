#include "Reconstructor.h"
#include "core/serialization/Serializer.h"
#include <nlohmann/json.hpp>

namespace cm { namespace editor {

Scene ReconstructFromSerialized(const SerializedBlob& in, const ReconstructionOptions& opts) {
	(void)opts;
	Scene s;
	try {
		nlohmann::json j = nlohmann::json::parse(in.bytes);
		Serializer::DeserializeScene(j, s);
	} catch (...) {
		// Return empty scene on parse error
	}
	return s;
}

}} // namespace cm::editor


