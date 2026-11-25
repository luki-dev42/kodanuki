#include "plugins/geometry/sphere_trace/plugin.h"

#include "core/render.h"
#include "plugins/geometry/sphere_trace/sphere_trace.h"

#include <memory>

namespace kodanuki {

SphereWorld* keptWorld(Renderer& renderer) {
	if (!stateOf(renderer, "geometry/sphere_trace")) {
		keptState(renderer, "geometry/sphere_trace", std::make_shared<SphereWorld>(openWorld(renderer.file.scene)));
	}
	return static_cast<SphereWorld*>(stateOf(renderer, "geometry/sphere_trace"));
}

Geometry sphereBuilt(Renderer& renderer) {
	const Choices& choices = renderer.options.choices;
	SphereWorld& world     = *keptWorld(renderer);
	world                  = shotWorld(renderer.file.scene, renderer.file.shot);
	return sphereGeometry(renderer.file.scene, choices.framed ? choices.region : reachRegion(renderer.file.shot), world);
}

Plugin geometrySphereTracePlugin() {
	Hooks hooks      = noHooks();
	hooks.buildWorld = sphereBuilt;
	return Plugin {
		.name   = "geometry/sphere_trace",
		.point  = Point::Geometry,
		.effect = Effect::Exact,
		.hooks  = hooks,
		.reads  = Reads::Nothing,
		.after  = {},
	};
}

} // namespace kodanuki
