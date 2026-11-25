#include "plugins/geometry/tree/tiles.h"
#include "plugins/geometry/tree/boxes.h"
#include "plugins/geometry/tree/lazy.h"
#include "sdf/compare.h"
#include "shared/parallel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

namespace kodanuki {

struct RanTile {
	TileContent content;
	std::vector<std::uint64_t> keys;
	std::vector<Box> boxes;
};

struct TileRange {
	std::int64_t lowest[3];
	std::int64_t highest[3];
};

struct KeyedObject {
	std::uint64_t key;
	std::uint32_t object;
};

std::string leastText(double least) {
	char text[32];
	std::snprintf(text, sizeof(text), "%g", least);
	return text;
}

std::string narrowStep(const TileSource& source) {
	for (const TileBlock& block : source.scene.tiles) {
		if (std::min({ block.step[0], block.step[1], block.step[2] }) >= leastTileSteps * source.least) {
			continue;
		}
		const Location& location = tileBlockLocation(block);
		return *location.file + ":" + std::to_string(location.line) + ":" + std::to_string(location.column) + ": a tile's step is at least 16 times the least tolerance, " + leastText(source.least) + " m";
	}
	return {};
}

bool refusedSteps(const TileSource& source) {
	const std::string error = narrowStep(source);
	if (error.empty()) {
		return false;
	}
	std::fprintf(stderr, "%s\n", error.c_str());
	if (!source.forgiving) {
		std::exit(1);
	}
	return true;
}

std::array<std::int64_t, 6> blockBounds(const TileBlock& block) {
	return { block.from[0], block.from[1], block.from[2], block.to[0], block.to[1], block.to[2] };
}

HeldTiles noTiles(const TileSource& source) {
	std::vector<std::array<double, 3>> steps;
	std::vector<std::array<std::int64_t, 6>> bounds;
	for (const TileBlock& block : source.scene.tiles) {
		steps.push_back({ block.step[0], block.step[1], block.step[2] });
		bounds.push_back(blockBounds(block));
	}
	return HeldTiles {
		.tiles       = {},
		.numbers     = {},
		.freeNumbers = {},
		.freeSlots   = {},
		.readNames   = {},
		.leaves      = emptyStore(),
		.steps       = steps,
		.bounds      = bounds,
		.held        = {},
		.forgiving   = source.forgiving,
		.refused     = refusedSteps(source),
		.told        = {},
		.evaluated   = 0,
		.bytes       = 0,
	};
}

bool sameTileSteps(const HeldTiles& tiles, const Scene& scene) {
	if (tiles.steps.size() != scene.tiles.size()) {
		return false;
	}
	for (std::size_t block = 0; block < scene.tiles.size(); ++block) {
		const double* step = scene.tiles[block].step;
		if (tiles.steps[block] != std::array<double, 3> { step[0], step[1], step[2] } || tiles.bounds[block] != blockBounds(scene.tiles[block])) {
			return false;
		}
	}
	return true;
}

Box boundedBox(const TileBlock& block) {
	Box box = {};
	for (int axis = 0; axis < 3; ++axis) {
		box.axes[axis].lower = block.step[axis] * double(block.from[axis]);
		box.axes[axis].upper = block.step[axis] * double(block.to[axis] + 1);
	}
	return box;
}

bool blockAboveTiles(const TileBlock& block, const Box& box) {
	const Box bounded = boxOverlap(box, boundedBox(block));
	for (int axis = 0; axis < 3; ++axis) {
		if (bounded.axes[axis].upper - bounded.axes[axis].lower > tileReach * block.step[axis]) {
			return true;
		}
	}
	return false;
}

bool cellAboveTiles(const Scene& scene, const Box& box) {
	return std::any_of(scene.tiles.begin(), scene.tiles.end(), [&](const TileBlock& block) {
		return blockAboveTiles(block, box);
	});
}

TileRange metRange(const TileBlock& block, const Box& box) {
	TileRange range = {};
	for (int axis = 0; axis < 3; ++axis) {
		const double lowest  = std::ceil(box.axes[axis].lower / block.step[axis]) - 1;
		const double highest = std::floor(box.axes[axis].upper / block.step[axis]);
		range.lowest[axis]   = std::int64_t(std::clamp(lowest, double(block.from[axis]), double(block.to[axis]) + 1));
		range.highest[axis]  = std::int64_t(std::clamp(highest, double(block.from[axis]) - 1, double(block.to[axis])));
	}
	return range;
}

bool emptyRange(const TileRange& range) {
	for (int axis = 0; axis < 3; ++axis) {
		if (range.lowest[axis] > range.highest[axis]) {
			return true;
		}
	}
	return false;
}

bool cellBesideTiles(const Scene& scene, const Box& box) {
	return std::all_of(scene.tiles.begin(), scene.tiles.end(), [&](const TileBlock& block) {
		return emptyRange(metRange(block, box));
	});
}

std::array<std::int64_t, 4> placeKey(const TilePlace& place) {
	return { std::int64_t(place.block), place.index[0], place.index[1], place.index[2] };
}

Box tileBox(const TileBlock& block, const TilePlace& place) {
	Box box = {};
	for (int axis = 0; axis < 3; ++axis) {
		box.axes[axis].lower = block.step[axis] * double(place.index[axis]);
		box.axes[axis].upper = block.step[axis] * double(place.index[axis] + 1);
	}
	return box;
}

void appendBlockTiles(std::vector<TilePlace>& places, std::uint32_t block, const TileRange& range) {
	for (std::int64_t k = range.lowest[2]; k <= range.highest[2]; ++k) {
		for (std::int64_t j = range.lowest[1]; j <= range.highest[1]; ++j) {
			for (std::int64_t i = range.lowest[0]; i <= range.highest[0]; ++i) {
				const TilePlace place = {
					.block = block,
					.index = { i, j, k },
				};

				places.push_back(place);
			}
		}
	}
}

std::vector<TilePlace> tilesMeeting(const Scene& scene, const Box& box) {
	std::vector<TilePlace> places;
	for (std::uint32_t block = 0; block < scene.tiles.size(); ++block) {
		appendBlockTiles(places, block, metRange(scene.tiles[block], box));
	}
	return places;
}

void toldError(Growth& growth, const std::string& error) {
	const std::lock_guard<std::mutex> hold(growth.lock);
	if (!growth.tiles.forgiving) {
		std::fprintf(stderr, "%s\n", error.c_str());
		std::fflush(stderr);
		std::_Exit(1);
	}
	if (growth.tiles.told.insert(error).second) {
		std::fprintf(stderr, "%s\n", error.c_str());
	}
}

std::vector<Box> tileObjectBoxes(const Scene& scene, const Box& tile) {
	std::vector<Box> boxes;
	for (const Box& box : objectBoxes(scene, 1).boxes) {
		boxes.push_back(boxOverlap(box, tile));
	}
	return boxes;
}

RanTile ranTile(Growth& growth, const TilePlace& place) {
	TileContent content = evaluatedTile(growth.scene, place);
	if (!content.error.empty()) {
		toldError(growth, content.error);
	}
	const std::vector<std::uint64_t> keys = objectKeys(content.scene);
	const std::vector<Box> boxes          = tileObjectBoxes(content.scene, tileBox(growth.scene.tiles[place.block], place));
	return RanTile {
		.content = std::move(content),
		.keys    = keys,
		.boxes   = boxes,
	};
}

Span noTileLeaves() {
	return Span {
		.first = 0,
		.count = 0,
	};
}

ObjectSlot freeObjectSlot() {
	return ObjectSlot {
		.key     = 0,
		.object  = noObject,
		.leaves  = noLeaf,
		.tile    = noTile,
		.program = nullptr,
	};
}

std::uint32_t takenSlot(Growth& growth) {
	while (!growth.tiles.freeSlots.empty()) {
		const std::uint32_t slot = growth.tiles.freeSlots.back();
		growth.tiles.freeSlots.pop_back();
		if (growth.slots[slot].object == noObject) {
			return slot;
		}
	}
	if (growth.slots.size() == growth.slots.capacity() || growth.slots.size() >= growth.boxes.boxes.capacity()) {
		refusedGrowth("object slots");
	}
	growth.slots.push_back(freeObjectSlot());
	growth.boxes.boxes.resize(growth.slots.size(), nothingBox());
	return std::uint32_t(growth.slots.size() - 1);
}

void freedSlot(Growth& growth, std::uint32_t slot) {
	growth.slots[slot]       = freeObjectSlot();
	growth.boxes.boxes[slot] = nothingBox();
	growth.tiles.freeSlots.push_back(slot);
}

std::vector<TileLeaf> tileLeafRow(const Scene& scene) {
	std::vector<TileLeaf> row;
	for (std::uint32_t leaf = 0; leaf < scene.leaves.size(); ++leaf) {
		const TileLeaf held = {
			.scene = &scene,
			.leaf  = leaf,
		};

		row.push_back(held);
	}
	return row;
}

void renumberedLeaves(std::vector<Program>& programs, std::uint32_t first) {
	for (Program& program : programs) {
		for (Step& step : program.steps) {
			step.leaf = step.leaf == noLeaf ? noLeaf : first + step.leaf;
		}
	}
}

std::size_t programBytes(const std::vector<Program>& programs) {
	std::size_t bytes = programs.capacity() * sizeof(Program);
	for (const Program& program : programs) {
		bytes += program.steps.capacity() * sizeof(Step) + program.name.capacity();
	}
	return bytes;
}

std::size_t sceneBytes(const Scene& scene) {
	const std::size_t shapes = scene.leaves.capacity() * (sizeof(Leaf) + sizeof(TileLeaf)) + scene.nodes.capacity() * sizeof(Node) + scene.arguments.capacity() * sizeof(double);
	return shapes + scene.objects.capacity() * sizeof(std::uint32_t) + programBytes(scene.programs) + programBytes(scene.objectPrograms);
}

std::size_t shapeBytes(const HeldTile& tile) {
	const std::size_t objects = tile.keys.capacity() * sizeof(std::uint64_t) + tile.boxes.capacity() * sizeof(Box) + tile.slots.capacity() * sizeof(std::uint32_t);
	return sceneBytes(tile.content.scene) + objects;
}

std::uint64_t bindingKey(const TileBlock& block, const std::string& name) {
	const auto found = block.bindings.find(name);
	return found == block.bindings.end() ? noBinding : found->second;
}

std::uint64_t readKey(const TileBlock& block, const std::vector<std::string>& names) {
	std::uint64_t key = noBinding;
	for (const std::string& name : names) {
		key = mixedKey(key, bindingKey(block, name));
	}
	return key;
}

void readBindings(HeldTiles& tiles, HeldTile& tile, const TileBlock& block) {
	const std::vector<std::string> names(tile.content.reads.begin(), tile.content.reads.end());
	tile.source  = block.source;
	tile.names   = &*tiles.readNames.insert(names).first;
	tile.readKey = readKey(block, names);
	block.reads->names.insert(names.begin(), names.end());
	tile.content.reads = {};
}

void placedTile(Growth& growth, std::uint32_t number, RanTile ran, const std::vector<std::uint32_t>& slots) {
	HeldTile& tile = *growth.tiles.tiles[number];
	tile.content   = std::move(ran.content);
	tile.keys      = ran.keys;
	tile.boxes     = ran.boxes;
	tile.leaves    = storedRow(growth.scene.tileLeaves->leaves, growth.tiles.leaves, tileLeafRow(tile.content.scene), "tile leaves");
	renumberedLeaves(tile.content.scene.objectPrograms, tileLeafBase + tile.leaves.first);
	for (Program& program : tile.content.scene.objectPrograms) {
		program = reachedProgram(program, growth.reach);
	}
	tile.slots = slots;
	for (std::uint32_t object = 0; object < tile.slots.size(); ++object) {
		tile.slots[object]     = tile.slots[object] == noObject ? takenSlot(growth) : tile.slots[object];
		const Program& program = tile.content.scene.objectPrograms[object];

		const ObjectSlot held = {
			.key     = tile.keys[object],
			.object  = object,
			.leaves  = leastLeaf(program),
			.tile    = number,
			.program = &program,
		};

		growth.slots[tile.slots[object]]       = held;
		growth.boxes.boxes[tile.slots[object]] = tile.boxes[object];
	}
	readBindings(growth.tiles, tile, growth.scene.tiles[tile.place.block]);
	tile.bytes = shapeBytes(tile);
	growth.tiles.bytes += tile.bytes;
	growth.tiles.evaluated += 1;
	tile.state = TileState::Held;
}

std::uint32_t takenNumber(HeldTiles& tiles, const TilePlace& place) {
	const bool fresh           = tiles.freeNumbers.empty();
	const std::uint32_t number = fresh ? std::uint32_t(tiles.tiles.size()) : tiles.freeNumbers.back();
	if (fresh) {
		tiles.tiles.emplace_back();
	}
	if (!fresh) {
		tiles.freeNumbers.pop_back();
	}

	const HeldTile running = {
		.place   = place,
		.state   = TileState::Running,
		.content = {},
		.keys    = {},
		.boxes   = {},
		.slots   = {},
		.leaves  = noTileLeaves(),
		.source  = 0,
		.names   = nullptr,
		.readKey = noBinding,
		.bytes   = 0,
		.marked  = false,
		.named   = false,
	};

	tiles.tiles[number]            = std::make_unique<HeldTile>(running);
	tiles.numbers[placeKey(place)] = number;
	tiles.bytes += tileRecordBytes;
	return number;
}

std::uint32_t claimedTile(Growth& growth, std::unique_lock<std::mutex>& hold, const TilePlace& place) {
	const auto found = growth.tiles.numbers.find(placeKey(place));
	if (found == growth.tiles.numbers.end()) {
		return takenNumber(growth.tiles, place);
	}
	HeldTile& tile = *growth.tiles.tiles[found->second];
	growth.tiles.held.wait(hold, [&]() {
		return tile.state != TileState::Running;
	});
	if (tile.state == TileState::Held) {
		return noTile;
	}
	tile.state = TileState::Running;
	return found->second;
}

const HeldTile& heldTile(Growth& growth, const TilePlace& place) {
	std::unique_lock<std::mutex> hold(growth.lock);
	const std::uint32_t number = claimedTile(growth, hold, place);
	if (number == noTile) {
		return *growth.tiles.tiles[growth.tiles.numbers.at(placeKey(place))];
	}
	const HeldTile& tile = *growth.tiles.tiles[number];
	hold.unlock();
	RanTile ran                            = ranTile(growth, place);
	const std::vector<std::uint32_t> slots = std::vector<std::uint32_t>(ran.keys.size(), noObject);
	hold.lock();
	placedTile(growth, number, std::move(ran), slots);
	growth.tiles.held.notify_all();
	return tile;
}

std::vector<std::uint32_t> ranTiles(Tree& tree, const Box& box) {
	Growth& growth = *tree.growth;
	std::vector<std::uint32_t> slots;
	for (const TilePlace& place : tilesMeeting(growth.scene, box)) {
		if (!reachMeets(growth.reach, tileBox(growth.scene.tiles[place.block], place))) {
			continue;
		}
		const HeldTile& tile = heldTile(growth, place);
		for (std::size_t object = 0; object < tile.slots.size(); ++object) {
			if (boxesMeet(tile.boxes[object], box)) {
				slots.push_back(tile.slots[object]);
			}
		}
	}
	std::sort(slots.begin(), slots.end());
	return slots;
}

bool newTilesReached(const Tree& tree, const Box& box, const Reach& old) {
	const Growth& growth = *tree.growth;
	for (const TilePlace& place : tilesMeeting(growth.scene, box)) {
		const Box tile = tileBox(growth.scene.tiles[place.block], place);
		if (reachMeets(growth.reach, tile) && !reachMeets(old, tile)) {
			return true;
		}
	}
	return false;
}

bool staleTile(const HeldTile& tile, const TileBlock& block, std::map<const std::vector<std::string>*, std::uint64_t>& keys) {
	if (tile.source != block.source) {
		return true;
	}
	const auto found        = keys.find(tile.names);
	const std::uint64_t key = found == keys.end() ? readKey(block, *tile.names) : found->second;
	keys[tile.names]        = key;
	return key != tile.readKey;
}

std::vector<KeyedObject> keyedTileObjects(const std::vector<std::uint64_t>& keys) {
	std::vector<KeyedObject> keyed;
	for (std::uint32_t object = 0; object < keys.size(); ++object) {
		const KeyedObject held = {
			.key    = keys[object],
			.object = object,
		};

		keyed.push_back(held);
	}
	std::stable_sort(keyed.begin(), keyed.end(), [](const KeyedObject& a, const KeyedObject& b) {
		return a.key < b.key;
	});
	return keyed;
}

std::vector<std::uint32_t> keptTileSlots(const HeldTile& tile, const std::vector<std::uint64_t>& keys) {
	const std::vector<KeyedObject> held   = keyedTileObjects(tile.keys);
	const std::vector<KeyedObject> listed = keyedTileObjects(keys);
	std::vector<std::uint32_t> kept(keys.size(), noObject);
	std::size_t next = 0;
	for (const KeyedObject& object : held) {
		while (next < listed.size() && listed[next].key < object.key) {
			next += 1;
		}
		if (next < listed.size() && listed[next].key == object.key) {
			kept[listed[next].object] = tile.slots[object.object];
			next += 1;
		}
	}
	return kept;
}

void releasedTileShapes(Growth& growth, HeldTile& tile) {
	freedRow(growth.scene.tileLeaves->leaves, growth.tiles.leaves, tile.leaves);
	growth.tiles.bytes -= tile.bytes;
	tile.leaves = noTileLeaves();
	tile.bytes  = 0;
}

std::vector<std::uint32_t> refollowedTile(Growth& growth, std::uint32_t number, RanTile ran) {
	HeldTile& tile                        = *growth.tiles.tiles[number];
	const std::vector<std::uint32_t> kept = keptTileSlots(tile, ran.keys);
	std::vector<std::uint32_t> changed;
	for (const std::uint32_t slot : tile.slots) {
		if (std::find(kept.begin(), kept.end(), slot) == kept.end()) {
			freedSlot(growth, slot);
			changed.push_back(slot);
		}
	}
	releasedTileShapes(growth, tile);
	placedTile(growth, number, std::move(ran), kept);
	for (std::size_t object = 0; object < kept.size(); ++object) {
		if (kept[object] == noObject) {
			changed.push_back(tile.slots[object]);
		}
	}
	return changed;
}

std::vector<std::uint32_t> staleTiles(const Tree& tree) {
	const Growth& growth = *tree.growth;
	std::vector<std::uint32_t> stale;
	std::vector<std::map<const std::vector<std::string>*, std::uint64_t>> keys(growth.scene.tiles.size());
	for (std::uint32_t number = 0; number < growth.tiles.tiles.size(); ++number) {
		const HeldTile* tile = growth.tiles.tiles[number].get();
		if (tile && staleTile(*tile, growth.scene.tiles[tile->place.block], keys[tile->place.block])) {
			stale.push_back(number);
		}
	}
	return stale;
}

std::vector<std::uint32_t> followedTiles(Tree& tree, const std::vector<std::uint32_t>& stale) {
	Growth& growth = *tree.growth;
	std::vector<RanTile> ran(stale.size());
	inParallel(growth.workers, stale.size(), [&](std::size_t k) {
		ran[k] = ranTile(growth, growth.tiles.tiles[stale[k]]->place);
	});

	std::vector<std::uint32_t> changed;
	for (std::size_t k = 0; k < stale.size(); ++k) {
		const std::vector<std::uint32_t> moved = refollowedTile(growth, stale[k], std::move(ran[k]));
		changed.insert(changed.end(), moved.begin(), moved.end());
	}
	std::sort(changed.begin(), changed.end());
	changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
	return changed;
}

void markListedTiles(Tree& tree, const Span& list) {
	Growth& growth = *tree.growth;
	for (std::uint32_t k = 0; k < list.count; ++k) {
		const std::uint32_t tile = growth.slots[growth.objects[list.first + k]].tile;
		if (tile != noTile) {
			growth.tiles.tiles[tile]->named = true;
		}
	}
}

void markRanTiles(Tree& tree, const Box& box) {
	Growth& growth = *tree.growth;
	for (const TilePlace& place : tilesMeeting(growth.scene, box)) {
		const auto found = growth.tiles.numbers.find(placeKey(place));
		if (found != growth.tiles.numbers.end()) {
			growth.tiles.tiles[found->second]->marked = true;
		}
	}
}

void forgetTile(Growth& growth, std::uint32_t number) {
	HeldTile& tile = *growth.tiles.tiles[number];
	for (const std::uint32_t slot : tile.slots) {
		freedSlot(growth, slot);
	}
	releasedTileShapes(growth, tile);
	growth.tiles.numbers.erase(placeKey(tile.place));
	growth.tiles.tiles[number].reset();
	growth.tiles.freeNumbers.push_back(number);
	growth.tiles.bytes -= tileRecordBytes;
}

void droppedTile(Growth& growth, HeldTile& tile) {
	for (const std::uint32_t slot : tile.slots) {
		freedSlot(growth, slot);
	}
	releasedTileShapes(growth, tile);
	tile.content = TileContent();
	tile.keys.clear();
	tile.boxes.clear();
	tile.slots.clear();
	tile.state = TileState::Dropped;
}

void sweptTiles(Tree& tree) {
	Growth& growth = *tree.growth;
	for (std::uint32_t number = 0; number < growth.tiles.tiles.size(); ++number) {
		HeldTile* tile = growth.tiles.tiles[number].get();
		if (!tile) {
			continue;
		}
		if (!tile->marked && !tile->named) {
			forgetTile(growth, number);
			continue;
		}
		if (!tile->named && tile->state == TileState::Held) {
			droppedTile(growth, *tile);
		}
		tile->marked = false;
		tile->named  = false;
	}
}

}
