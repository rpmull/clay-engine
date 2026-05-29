#include "FuzzMutator.h"
#include <random>

namespace cm { namespace editor {

int FuzzScene(Scene& s, const FuzzOptions& opts) {
	std::mt19937_64 rng(opts.seed ? opts.seed : std::random_device{}());
	std::uniform_real_distribution<float> jitter(-0.01f, 0.01f);
	int failures = 0;
	for (int p = 0; p < opts.passes; ++p) {
		for (auto& e : s.GetEntities()) {
			if (auto* d = s.GetEntityData(e.GetID())) {
				// Transform is a value on EntityData
				d->Transform.Position.x += jitter(rng);
				d->Transform.Position.y += jitter(rng);
				d->Transform.Position.z += jitter(rng);
			}
		}
	}
	return failures;
}

}} // namespace cm::editor


