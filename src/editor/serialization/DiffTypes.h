#pragma once
#include <string>
#include <variant>
#include <vector>

namespace cm { namespace editor {

struct SerializedBlob {
	std::string bytes;
};

struct ValueView {
	// Minimal placeholder for future non-owning views
	std::variant<std::monostate, bool, int64_t, double, std::string> scalar;
};

struct Diff {
	enum class Kind { Add, Remove, Change, TypeMismatch, OrderMismatch, UnexpectedAdd, UnexpectedRemove, UnexpectedChange };
	Kind kind = Kind::Change;
	std::string path; // JSONPointer-like path
	ValueView before;
	ValueView after;
	float delta = 0.0f;
};

}} // namespace cm::editor


