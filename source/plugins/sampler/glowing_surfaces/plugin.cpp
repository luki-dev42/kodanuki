#include "plugins/sampler/glowing_surfaces/plugin.h"

#include "core/render.h"
#include "plugins/sampler/glowing_surfaces/glowing_surfaces.h"
#include "plugins/transport/paths/paths.h"

#include <string>
#include <utility>
#include <vector>

namespace kodanuki {

constexpr int glowGridDepth = 6;

void glowGridPrepared(Renderer& renderer) {
	const Tracing& tracing                  = tracingOf(renderer.drawing.transported.get());
	const GlowCells gathered                = gatheredGlowGrid(tracing.geometry, tracing.looks, glowGridDepth);
	glowCellsState(renderer.history.states) = tracing.sampling.glowBalls ? withGlowBalls(tracing.geometry, tracing.looks, gathered) : gathered;
}

std::vector<std::string> glowsNeeded() {
	return { "geometry/tree" };
}

Hooks glowGridPreparedHooks() {
	Hooks hooks        = noHooks();
	hooks.prepareFrame = glowGridPrepared;
	hooks.needed       = glowsNeeded;
	return hooks;
}

Plugin samplerGlowingSurfacesPlugin() {
	return Plugin {
		.name   = "sampler/glowing_surfaces",
		.point  = Point::Owned,
		.effect = Effect::Exact,
		.hooks  = glowGridPreparedHooks(),
		.reads  = joinedReads(Reads::Shapes, Reads::Looks),
		.after  = {},
	};
}

} // namespace kodanuki
