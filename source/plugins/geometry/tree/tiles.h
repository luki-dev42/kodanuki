#pragma once

#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/cut.h"
#include "plugins/geometry/tree/store.h"
#include "plugins/geometry/tree/tree.h"
#include "sdf/scene.h"
#include "syntax/evaluator.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace kodanuki {

/**
 * Where a tile the tree holds is in its life.
 */
enum class TileState : std::uint8_t {
	/**
	 * A thread runs it; others that need it wait.
	 */
	Running,
	/**
	 * Its objects hold slots and its leaves are in the scene's tile
	 * leaves.
	 */
	Held,
	/**
	 * Its shapes were let go, since no node listed them: only what it
	 * was run from is kept, so that a node that ran it learns of a change.
	 * A node that needs it runs it again.
	 */
	Dropped,
};

/**
 * One tile a growing tree has run and holds.
 */
struct HeldTile {
	/**
	 * The block and the tile.
	 */
	TilePlace place;
	/**
	 * Where it is in its life.
	 */
	TileState state;
	/**
	 * Its shapes. Once it is held, its object programs name its leaves
	 * from tileLeafBase plus leaves.first on (sceneLeaf()) and are cut to
	 * the reach.
	 */
	TileContent content;
	/**
	 * Each object's key (objectKey()), taken before the programs were
	 * renumbered.
	 */
	std::vector<std::uint64_t> keys;
	/**
	 * Each object's box, held to the tile's own box: a tile's shapes are
	 * trusted to stay inside it.
	 */
	std::vector<Box> boxes;
	/**
	 * The slot each object holds (ObjectSlot), matching the objects.
	 */
	std::vector<std::uint32_t> slots;
	/**
	 * Where its leaves stand in the scene's tile leaves (TileLeaves).
	 */
	Span leaves;
	/**
	 * The block's source the tile was run from (TileBlock::source).
	 */
	std::uint64_t source;
	/**
	 * The top-level names the tile read, in order, one list shared by
	 * every tile that read the same names (HeldTiles::readNames); null
	 * while it first runs.
	 */
	const std::vector<std::string>* names;
	/**
	 * A key of the key of each of those bindings (TileBlock::bindings),
	 * noBinding for a name the block did not bind.
	 */
	std::uint64_t readKey;
	/**
	 * How many bytes its shapes hold, with its objects' keys, boxes and
	 * slots.
	 */
	std::size_t bytes;
	/**
	 * Whether a node that ran it stands, while the tree looks for tiles
	 * to forget.
	 */
	bool marked;
	/**
	 * Whether a node lists one of its objects, likewise.
	 */
	bool named;
};

/**
 * The tiles a growing tree holds, and what it needs to run more.
 *
 * A tile is run the first time a node whose cell meets it is built
 * narrow enough (cellAboveTiles()), on the thread that builds the node,
 * outside the growth lock; its objects then take slots and its leaves a
 * span of the scene's tile leaves under the lock. It is held while a
 * node lists one of its objects; while only nodes that ran it stand, it
 * keeps what it was run from and drops its shapes, and with the last of
 * them it is forgotten (sweptTiles()).
 */
struct HeldTiles {
	/**
	 * The tiles, by their number; null where a number is free.
	 */
	std::vector<std::unique_ptr<HeldTile>> tiles;
	/**
	 * Each tile's number, by its block and index.
	 */
	std::map<std::array<std::int64_t, 4>, std::uint32_t> numbers;
	/**
	 * The free numbers.
	 */
	std::vector<std::uint32_t> freeNumbers;
	/**
	 * Slots that freed tile objects gave back, which a tile takes first;
	 * one the scene's objects took since is passed over.
	 */
	std::vector<std::uint32_t> freeSlots;
	/**
	 * Each list of top-level names some tile read (HeldTile::names).
	 */
	std::set<std::vector<std::string>> readNames;
	/**
	 * The eye a tile sees (`eye(step)`): the frame's camera's, set before
	 * the frame's tiles run.
	 */
	math::Vector eye;
	/**
	 * Each step some tile read the eye at.
	 */
	std::set<double> eyeSteps;
	/**
	 * The free spans of the scene's tile leaves.
	 */
	SpanStore leaves;
	/**
	 * The step of each tiles block of the scene the tree was made for.
	 */
	std::vector<std::array<double, 3>> steps;
	/**
	 * The bounds of each tiles block of that scene: its from, then its to
	 * (TileBlock::from, TileBlock::to).
	 */
	std::vector<std::array<std::int64_t, 6>> bounds;
	/**
	 * Told when a tile is held, under the growth lock.
	 */
	std::condition_variable held;
	/**
	 * Whether a tile's error is printed and the tile left empty, as the
	 * viewer wants, rather than ending the program.
	 */
	bool forgiving;
	/**
	 * Whether the tree runs no tile: a block's step is too narrow for its
	 * least tolerance (noTiles()).
	 */
	bool refused;
	/**
	 * The errors printed, each printed once.
	 */
	std::set<std::string> told;
	/**
	 * How many times a tile was run since the tree was made.
	 */
	std::uint64_t evaluated;
	/**
	 * How many seconds the tiles' runs took since the tree was made, each
	 * run's wall time summed over the threads that ran them.
	 */
	double seconds;
	/**
	 * How many bytes the tiles hold: each held tile's shapes
	 * (HeldTile::bytes) and every tile's record (tileRecordBytes).
	 */
	std::size_t bytes;
};

/**
 * The key a held tile keeps for a name its block does not bind
 * (HeldTile::read).
 */
constexpr std::uint64_t noBinding = 0;

/**
 * How many bytes a tile the tree knows holds beyond its shapes: its
 * record and its place in HeldTiles::tiles and HeldTiles::numbers.
 */
constexpr std::size_t tileRecordBytes = sizeof(HeldTile) + 64;

/**
 * How many steps of a tile wide a cell may be along each axis, at most,
 * for its node to run the tiles it meets.
 */
constexpr double tileReach = 4;

/**
 * How many least tolerances a tile's step must be along every axis, at
 * least: a node at the deepest level, four least tolerances wide, then
 * meets no more than two tiles along an axis.
 */
constexpr double leastTileSteps = 16;

/**
 * Where a tree's tiles come from and what happens on an error.
 */
struct TileSource {
	/**
	 * The scene.
	 */
	const Scene& scene;
	/**
	 * The tree's least tolerance.
	 */
	double least;
	/**
	 * The eye the tiles see until a frame moves the reach: the open
	 * reach's (openReach()).
	 */
	math::Vector eye;
	/**
	 * Whether an error is printed once and the tiles left empty, as the
	 * viewer wants, rather than ending the program.
	 */
	bool forgiving;
};

/**
 * Return the tiles of a tree made for a scene: none held, the scene's
 * blocks' steps kept.
 *
 * A block whose step is narrower than leastTileSteps least tolerances
 * along some axis is an error, `file:line:column: a tile's step is at
 * least 16 times the least tolerance, <least> m`: it ends the program,
 * or, where the tree forgives, is printed and the tree runs no tile.
 *
 * @param source The scene, the least tolerance and whether errors are
 *        forgiven.
 */
HeldTiles noTiles(const TileSource& source);

/**
 * Return whether a scene's tiles blocks have the steps and bounds a
 * tree's tiles were made for: where not, the tree is made again.
 *
 * @param tiles The tree's tiles.
 * @param scene The scene.
 */
bool sameTileSteps(const HeldTiles& tiles, const Scene& scene);

/**
 * Return whether a cell is too wide to run the tiles it meets: where it
 * meets the box a block's bounds hold (all space, for a block without
 * bounds), that part is wider than tileReach steps of the block along
 * some axis. Its node then lists no tile objects and branches into every
 * slot that no object fills.
 *
 * @param scene The scene.
 * @param box The cell.
 */
bool cellAboveTiles(const Scene& scene, const Box& box);

/**
 * Return whether a cell meets no tile of any block within the block's
 * bounds (TileBlock::from, TileBlock::to): its node, and every node
 * below it, needs no tile and lists the scene's objects alone.
 *
 * @param scene The scene.
 * @param box The cell.
 */
bool cellBesideTiles(const Scene& scene, const Box& box);

/**
 * Have every tile a cell meets within its block's bounds whose box the
 * reach (Growth::reach) meets held, running those not held yet or
 * waiting for those another thread runs, and return the slots of their objects whose box
 * meets the cell, ascending. A tile's object programs are cut to the
 * reach (reachedProgram()) as it is placed.
 *
 * A tile whose block makes an error ends the program where the tiles
 * are not forgiving; else the error is printed once and the tile holds
 * nothing.
 *
 * @param tree The growing tree.
 * @param box The cell.
 */
std::vector<std::uint32_t> ranTiles(Tree& tree, const Box& box);

/**
 * Whether a cell meets a tile whose box the reach (Growth::reach) meets
 * now but the old reach did not: a node that ran the cell's tiles under
 * the old reach did not run it.
 *
 * @param tree The growing tree.
 * @param box The cell.
 * @param old The reach before it moved.
 */
bool newTilesReached(const Tree& tree, const Box& box, const Reach& old);

/**
 * Return whether a tile that read the eye sees it elsewhere at the eye
 * given: at some step a tile read it at (HeldTiles::eyeSteps), the eye
 * snapped to that step (snappedEye()) differs from the tiles' eye
 * snapped to it.
 *
 * @param tiles The tree's tiles.
 * @param eye The frame's camera's eye.
 */
bool eyeMoved(const HeldTiles& tiles, const math::Vector& eye);

/**
 * Return the numbers of the tiles a tree knows that a scene whose tiles
 * blocks were replaced, or the eye they see (HeldTiles::eye), makes
 * anew: those whose block's source, a binding they read, or the eye
 * snapped to a step they read it at differs from what they were run
 * from.
 *
 * @param tree The growing tree, whose scene is the new one.
 */
std::vector<std::uint32_t> staleTiles(const Tree& tree);

/**
 * Run some stale tiles (staleTiles()) again, between frames, and return
 * the slots that changed, ascending.
 *
 * They run on the tree's threads, and each one's objects are matched to
 * the ones it had by key: one of a key it had keeps its slot, one it had
 * no more frees its slot and a new one takes a slot; the freed and the
 * taken slots are the changed ones. Each tile is then held, its leaves
 * naming its new shapes, until the tiles are swept (sweptTiles()).
 *
 * @param tree The growing tree, whose scene is the new one.
 * @param stale The tiles, by their number.
 */
std::vector<std::uint32_t> followedTiles(Tree& tree, const std::vector<std::uint32_t>& stale);

/**
 * Mark the tiles whose objects a node's object list names, as named.
 *
 * @param tree The growing tree.
 * @param list The list, in Growth::objects.
 */
void markListedTiles(Tree& tree, const Span& list);

/**
 * Mark the held tiles a cell meets, for a node that ran them.
 *
 * @param tree The growing tree.
 * @param box The node's cell.
 */
void markRanTiles(Tree& tree, const Box& box);

/**
 * Forget every held tile that is not marked or named, freeing its slots
 * and its leaves, drop the shapes of every one that is marked but not
 * named, and clear the marks.
 *
 * @param tree The growing tree.
 */
void sweptTiles(Tree& tree);

}
