#include "plugins/geometry/tree/plugin.h"

#include "core/block.h"
#include "core/render.h"
#include "core/step.h"
#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/lazy.h"
#include "plugins/geometry/tree/walk.h"
#include "plugins/geometry/tree/world.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace kodanuki {

using TreeWorld = std::optional<BuiltGeometry>;

static_assert(std::is_trivially_copyable_v<PackedNode>, "a block's element is plain data");
static_assert(std::is_trivially_copyable_v<SurfaceLeaf>, "a block's element is plain data");
static_assert(std::is_trivially_copyable_v<SolidLeaf>, "a block's element is plain data");
static_assert(std::is_trivially_copyable_v<ProgramList>, "a block's element is plain data");
static_assert(std::is_trivially_copyable_v<ProgramReference>, "a block's element is plain data");
static_assert(std::is_trivially_copyable_v<ProgramCut>, "a block's element is plain data");

TreeWorld* treeWorld(const Renderer& renderer) {
	return static_cast<TreeWorld*>(stateOf(renderer, "geometry/tree"));
}

TreeWorld& openedTreeWorld(Renderer& renderer) {
	if (!treeWorld(renderer)) {
		keptState(renderer, "geometry/tree", std::make_shared<TreeWorld>());
	}
	return *treeWorld(renderer);
}

bool forgivingTiles(const Renderer& renderer) {
	return std::any_of(renderer.sinks.begin(), renderer.sinks.end(), [](const Plugin& sink) {
		return sink.hooks.hostRenderer != nullptr;
	});
}

void remadeWorld(TreeWorld& world, const SceneFile& file, const Choices& choices, bool forgiving) {
	const std::uint64_t opened    = world->tree.growth->opened;
	const std::uint64_t reset     = world->tree.growth->reset;
	const std::uint64_t forgotten = world->tree.growth->forgotten;
	const std::uint64_t rooted    = world->tree.growth->rooted;
	Choices again                 = choices;
	again.least                   = world->tree.growth->detail.least;
	world.reset();
	world.emplace(builtGeometry(file, again, forgiving));
	world->tree.growth->opened    = opened;
	world->tree.growth->reset     = reset;
	world->tree.growth->forgotten = forgotten;
	world->tree.growth->rooted    = rooted;
}

Geometry treeBuilt(Renderer& renderer) {
	TreeWorld& world       = openedTreeWorld(renderer);
	const Choices& choices = renderer.options.choices;
	if (!world) {
		world.emplace(builtGeometry(renderer.file, choices, forgivingTiles(renderer)));
		return treeGeometry(renderer.file.scene, world->tree);
	}
	if (!keptGeometry(*world, renderer.file, renderer.frame.camera.eye)) {
		remadeWorld(world, renderer.file, choices, forgivingTiles(renderer));
	}
	return treeGeometry(renderer.file.scene, world->tree);
}

void treePrepared(Renderer& renderer) {
	TreeWorld& world = openedTreeWorld(renderer);
	if (!world) {
		return;
	}
	Shot shot   = renderer.file.shot;
	shot.camera = renderer.frame.camera;
	if (!renderer.options.choices.framed && renderer.world) {
		growRegion(world->tree, reachedHull(heldBoxes(world->tree.growth->boxes.boxes, renderer.file.scene), shot));
		renderer.world->geometry.region = world->tree.region;
	}
	moveReach(world->tree, shotReach(shot));
	prepareTree(world->tree, shot, changedBy(renderer.step.change, joinedChange(Change::Cut, Change::Moved)));
}

std::vector<Counter> treePublished(const Renderer& renderer) {
	const TreeWorld* world = treeWorld(renderer);
	const Tree* tree       = world && *world ? &(*world)->tree : nullptr;
	return {
		Counter { .name = "bytes.tree", .value = tree ? double(heldBytes(*tree)) : 0 },
		Counter { .name = "nodes.opened", .value = tree ? double(tree->growth->opened) : 0 },
		Counter { .name = "nodes.reset", .value = tree ? double(tree->growth->reset) : 0 },
		Counter { .name = "nodes.forgotten", .value = tree ? double(tree->growth->forgotten) : 0 },
		Counter { .name = "tree.rooted", .value = tree ? double(tree->growth->rooted) : 0 },
		Counter { .name = "tiles.evaluated", .value = tree ? double(tree->growth->tiles.evaluated) : 0 },
		Counter { .name = "tiles.seconds", .value = tree ? tree->growth->tiles.seconds : 0 },
		Counter { .name = "tiles.held", .value = tree ? double(tree->growth->tiles.numbers.size()) : 0 },
		Counter { .name = "bytes.tiles", .value = tree ? double(tree->growth->tiles.bytes) : 0 },
	};
}

std::vector<Block> treeBlocksOf(const Tree& tree) {
	return {
		Block { .name = "geometry/tree.nodes", .data = tree.nodes.data(), .count = tree.nodes.size(), .size = sizeof(PackedNode) },
		Block { .name = "geometry/tree.surfaces", .data = tree.surfaces.data(), .count = tree.surfaces.size(), .size = sizeof(SurfaceLeaf) },
		Block { .name = "geometry/tree.solids", .data = tree.solids.data(), .count = tree.solids.size(), .size = sizeof(SolidLeaf) },
		Block { .name = "geometry/tree.runs", .data = tree.runs.data(), .count = tree.runs.size(), .size = sizeof(std::uint32_t) },
		Block { .name = "geometry/tree.lists", .data = tree.lists.data(), .count = tree.lists.size(), .size = sizeof(ProgramList) },
		Block { .name = "geometry/tree.references", .data = tree.references.data(), .count = tree.references.size(), .size = sizeof(ProgramReference) },
		Block { .name = "geometry/tree.cuts", .data = tree.cuts.data(), .count = tree.cuts.size(), .size = sizeof(ProgramCut) },
		Block { .name = "geometry/tree.cutSteps", .data = tree.cutSteps.data(), .count = tree.cutSteps.size(), .size = sizeof(std::uint16_t) },
	};
}

std::vector<Block> treeBlocks(const Renderer& renderer) {
	const TreeWorld* world = treeWorld(renderer);
	if (!world || !*world) {
		return {};
	}
	return treeBlocksOf((*world)->tree);
}

Plugin geometryTreePlugin() {
	Hooks hooks        = noHooks();
	hooks.buildWorld   = treeBuilt;
	hooks.prepareFrame = treePrepared;
	hooks.published    = treePublished;
	hooks.blocks       = treeBlocks;
	return Plugin {
		.name   = "geometry/tree",
		.point  = Point::Geometry,
		.effect = Effect::Exact,
		.hooks  = hooks,
		.reads  = everyRead(),
		.after  = {},
	};
}

} // namespace kodanuki
