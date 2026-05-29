#include "Canonicalizer.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include "core/serialization/Serializer.h"

using json = nlohmann::json;

namespace cm { namespace editor {

static void CanonicalizeFloats(json& j);
static void SortObjectKeys(json& j);
static void NormalizeGuids(json& j);

SerializedBlob SerializeSceneToBlob(Scene& scene) {
	json j = Serializer::SerializeScene(scene);
	SerializedBlob blob; blob.bytes = j.dump();
	return blob;
}

void DeserializeSceneFromBlob(const SerializedBlob& blob, Scene& outScene) {
	json j = json::parse(blob.bytes, nullptr, false);
	if (!j.is_discarded()) {
		Serializer::DeserializeScene(j, outScene);
	}
}

SerializedBlob Canonicalize(const SerializedBlob& in) {
	SerializedBlob out;
	json j = json::parse(in.bytes, nullptr, false);
	if (j.is_discarded()) { out.bytes = in.bytes; return out; }
	SortObjectKeys(j);
	CanonicalizeFloats(j);
	NormalizeGuids(j);
	out.bytes = j.dump();
	return out;
}

SerializedBlob ParseCanonical(const std::string& s) {
	SerializedBlob b; b.bytes = s; return b;
}

static std::string FormatFloat(double v) {
	if (v == 0.0) v = 0.0; // clamp -0 to 0
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%.6g", v);
	return std::string(buf);
}

static void CanonicalizeFloats(json& j) {
	if (j.is_object()) {
		for (auto& it : j.items()) CanonicalizeFloats(it.value());
	} else if (j.is_array()) {
		for (auto& v : j) CanonicalizeFloats(v);
	} else if (j.is_number_float()) {
		std::string s = FormatFloat(j.get<double>());
		j = std::stod(s);
	}
}

static void SortObjectKeys(json& j) {
	if (j.is_object()) {
		// nlohmann::json preserves insertion order; rebuild object in sorted key order
		std::vector<std::pair<std::string, json>> items;
		items.reserve(j.size());
		for (auto it = j.begin(); it != j.end(); ++it) items.emplace_back(it.key(), it.value());
		std::sort(items.begin(), items.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
		json sorted = json::object();
		for (auto& kv : items) { SortObjectKeys(kv.second); sorted[kv.first] = kv.second; }
		j = std::move(sorted);
	} else if (j.is_array()) {
		for (auto& v : j) SortObjectKeys(v);
	}
}

static void NormalizeGuids(json& j) {
	// Expect GUIDs encoded as hex strings in our engine
	if (j.is_object()) {
		for (auto& it : j.items()) NormalizeGuids(it.value());
	} else if (j.is_array()) {
		for (auto& v : j) NormalizeGuids(v);
	} else if (j.is_string()) {
		std::string s = j.get<std::string>();
		bool hex = !s.empty() && s.find_first_not_of("0123456789abcdefABCDEF-") == std::string::npos;
		if (hex) {
			for (char& c : s) c = (char)std::tolower((unsigned char)c);
			j = s;
		}
	}
}

}} // namespace cm::editor


