#pragma once

#include "core/geometry.h"
#include "plugins/geometry/tree/boxes.h"
#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/detail.h"
#include "plugins/geometry/tree/pass.h"
#include "plugins/geometry/tree/sharing.h"
#include "plugins/geometry/tree/store.h"
#include "plugins/geometry/tree/tiles.h"
#include "plugins/geometry/tree/tree.h"
#include "sdf/scene.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace kodanuki {

/**
 * Whether a node's object list holds the objects of the tiles its cell
 * meets (plugins/geometry/tree/tiles.h).
 */
enum class TileListing : std::uint8_t {
	/**
	 * It does not. Built, a node whose cell is too wide to run tiles
	 * (cellAboveTiles()) branches into every slot no object fills, and
	 * its children wait too; a narrower one runs the tiles its cell
	 * meets and lists their objects.
	 */
	Waiting,
	/**
	 * It does, from its parent's list; every node of a tree whose scene
	 * has no tiles, and every node whose cell meets no tile within the
	 * blocks' bounds (cellBesideTiles()).
	 */
	Listed,
	/**
	 * It does: it ran the tiles its cell meets when it was built, and
	 * holds them while it stands.
	 */
	Ran,
	/**
	 * It does not, and it was built so, as one waiting, while its cell
	 * lay beyond the reach (Growth::reach): it listed only the scene's
	 * objects, and its children wait. It is dropped once the reach meets
	 * its cell (moveReach()).
	 */
	Beyond,
};

/**
 * The index that names no node: the parent of the root, and the
 * forward of a node no finer one stands for.
 */
constexpr std::uint32_t noNode = 0xFFFFFFFFu;

/**
 * The spans a built node wrote into the tree's arrays, so that they are
 * given back when the node is dropped. The program lists its leaves
 * name are shared between nodes (SharedPrograms) and given back by
 * their uses.
 */
struct NodeSpans {
	/**
	 * Its branch children's nodes, in Tree::nodes, with their records.
	 */
	Span nodes;
	/**
	 * Its surface leaves, in Tree::surfaces.
	 */
	Span surfaces;
	/**
	 * Its solid leaves, in Tree::solids.
	 */
	Span solids;
	/**
	 * Its leaves' face runs, in Tree::runs.
	 */
	Span runs;
	/**
	 * Its branch children's object lists as they were first written, in
	 * Growth::objects. A child's list that a change made longer stands
	 * outside it.
	 */
	Span objects;
};

/**
 * What a growing tree keeps of one node beside the node itself, at the
 * node's index.
 *
 * A node's cell is not kept: it is the region halved along the slots
 * from the root down to the node (nodeBox()), which is to the bit the
 * box the node's cells were planned with.
 */
struct NodeRecord {
	/**
	 * The node whose slot names this one, noNode for the root.
	 */
	std::uint32_t parent;
	/**
	 * The finer node that stands for this one once its state is
	 * Forwarded, noNode before.
	 */
	std::uint32_t forward;
	/**
	 * The objects that may decide the surface in the node's cell, as
	 * ascending slots (Growth::slots), in Growth::objects.
	 * The node is built from their whole programs, and the list is kept
	 * once it is built. A node built in place of a loose one names the
	 * loose one's list.
	 */
	Span objects;
	/**
	 * How far the node's own cell was allowed to stray when the node was
	 * built, 0 while it is not: what a sharp ray allows where the node
	 * is Fine, the looser tolerance where it is Loose.
	 */
	float tolerance;
	/**
	 * The slot of the parent that names this node, from 0 to 63.
	 */
	std::uint8_t slot;
	/**
	 * How deep the node's cell stands below the region, always even.
	 */
	std::uint8_t depth;
	/**
	 * Whether its list holds the objects of the tiles its cell meets.
	 */
	TileListing tiles;
	/**
	 * What the node wrote into the tree's arrays, nothing while it is
	 * not built.
	 */
	NodeSpans owned;
};

/**
 * Everything a tree grows from, beside the tree's own arrays.
 *
 * A node is built outside the lock, from its record alone, and placed
 * under it: its leaves, runs and lists at the arrays' ends, then a
 * placeholder and a whole record for each of its branch children, then
 * the node itself, and only then its state. The arrays and the records
 * are reserved once and never move, so rays read what is published
 * while other nodes are placed.
 */
struct Growth {
	/**
	 * The scene's geometry, which outlives the tree. Its shapes may be
	 * replaced between frames; the tree then follows them
	 * (followScene()).
	 */
	const Scene& scene;
	/**
	 * How fine each cell must be. Its least tolerance is the one the
	 * tree was made with, whatever the region becomes.
	 */
	Detail detail;
	/**
	 * The Build pass members that run on each node.
	 */
	std::vector<PassMember> passes;
	/**
	 * The deepest a cell may stand below the region.
	 */
	int depthCap;
	/**
	 * How many threads walk the tree, which a node near the root may
	 * plan its cells on while they wait for it.
	 */
	int workers;
	/**
	 * Whether derived faces are tried.
	 */
	bool faces;
	/**
	 * One record for each node of Tree::nodes, at the same index.
	 */
	std::vector<NodeRecord> records;
	/**
	 * The object lists the records name, each a span of its own.
	 */
	std::vector<std::uint32_t> objects;
	/**
	 * The slots the tree names the scene's objects by: the object each
	 * holds as the tree was last told of the scene, with its key. They
	 * start as the scene's objects in order, and an object keeps its slot
	 * while the scene holds an object of its key (followScene()). The
	 * objects of held tiles take slots too, while rays run, under the
	 * lock (HeldTiles): for a scene with tiles the slots and the boxes are
	 * reserved once and never move.
	 */
	std::vector<ObjectSlot> slots;
	/**
	 * The scene's objects' programs cut to the reach (reachedProgram()),
	 * matching Scene::objectPrograms: the whole programs the slots of
	 * the scene's objects name. A held tile's objects are cut in place.
	 */
	std::vector<Program> reached;
	/**
	 * The slot of each of the scene's objects, matching
	 * Scene::objectPrograms.
	 */
	std::vector<std::uint32_t> objectSlots;
	/**
	 * The boxes of the objects as the tree was last told of them, one for
	 * each slot at the same index (ObjectBoxes::boxes), a box that holds
	 * no point for a free slot. A node is built from the listed objects
	 * whose box meets its cell, and a changed object is followed only
	 * into cells its box meets.
	 */
	ObjectBoxes boxes;
	/**
	 * The cells the tree's earlier roots stood for, first root first,
	 * each to the bit as it was while it was the root: a cell halved down
	 * from a later root that lies on one of them is that cell (nodeBox()),
	 * so the nodes built below it keep their cells when the region grows
	 * (growRegion()).
	 */
	std::vector<Box> formerRoots;
	/**
	 * The loose nodes a finer one was built for since the frame began,
	 * which prepareTree() folds into one.
	 */
	std::vector<std::uint32_t> forwarded;
	/**
	 * The free spans of each array, which nodes placed later write into.
	 */
	TreeStores stores;
	/**
	 * The program lists and cuts the nodes share.
	 */
	SharedPrograms shared;
	/**
	 * The tiles the tree has run and holds.
	 */
	HeldTiles tiles;
	/**
	 * The render's reach about the frame's camera (shotReach()), one that
	 * cuts nothing before the first frame (openReach()). Every object's
	 * whole program is cut to it, and tiles are made only where it meets
	 * a cell: a cell beyond it holds nothing.
	 */
	Reach reach;
	/**
	 * How many bytes the tree may hold (heldBytes()) before the nodes
	 * no ray entered for a few frames are forgotten (prepareTree()).
	 */
	std::size_t memory;
	/**
	 * How many nodes were built since the tree was made.
	 */
	std::uint64_t opened;
	/**
	 * How many built nodes were dropped since the tree was made, because
	 * an object in them changed or the view asks for finer ones.
	 */
	std::uint64_t reset;
	/**
	 * How many built nodes were forgotten since the tree was made,
	 * because the tree held more than its memory allows.
	 */
	std::uint64_t forgotten;
	/**
	 * How many times the region grew since the tree was made.
	 */
	std::uint64_t rooted;
	/**
	 * How many nodes were built when prepareTree() last handed the
	 * heap's free memory back.
	 */
	std::uint64_t trimmed;
	/**
	 * How many built nodes had been dropped and forgotten together
	 * (reset plus forgotten) when the held tiles were last swept.
	 */
	std::uint64_t swept;
	/**
	 * Held while a node is placed into the tree's arrays.
	 */
	std::mutex lock;
	/**
	 * How many of the walking threads wait for a node another thread
	 * builds and are not lent out: a node near the root plans its cells
	 * on that many more threads.
	 */
	std::atomic<int> idle;
};

/**
 * What a growing tree is made from.
 */
struct LazyInput {
	/**
	 * The scene's geometry.
	 */
	const Scene& scene;
	/**
	 * How fine each cell must be.
	 */
	const Detail& detail;
	/**
	 * The Build pass members that are on.
	 */
	const std::vector<PassMember>& passes;
	/**
	 * The boxes of the scene's objects (objectBoxes()).
	 */
	const ObjectBoxes& boxes;
	/**
	 * The box to grow over.
	 */
	Region region;
	/**
	 * The deepest a cell may stand below the region.
	 */
	int depthCap;
	/**
	 * How many threads will walk the tree, at least one.
	 */
	int workers;
	/**
	 * Whether derived faces are tried.
	 */
	bool faces;
	/**
	 * How many bytes the tree may hold before it forgets nodes.
	 */
	std::size_t memory;
	/**
	 * Whether a tile's error is printed once and the tile left empty, as
	 * the viewer wants, rather than ending the program.
	 */
	bool forgiving;
};

/**
 * Make a tree that grows where rays go: its root is a branch whose one
 * node is a placeholder for the whole region, naming every object.
 *
 * The tree's arrays, the records and the object lists are reserved here
 * once, together a quarter of the machine's memory at most, each as a
 * block of its own (largeBlocksMapped()), so only what nodes write is
 * resident; a tree that fills one of them ends the program. A dropped
 * node's spans are given back to the arrays' free stores and written
 * again by nodes placed later. Each node is passed on its own, so
 * SharedCoefficients shares runs within a node; program lists and cuts
 * are shared across the tree (sharedLists()).
 *
 * @param input The scene, the Detail, the passes and the region.
 */
Tree lazyTree(const LazyInput& input);

/**
 * Return the node a ray walks where its walk enters a node: the node
 * itself where it is built finely enough for the ray, else what
 * openedNode() returns. The node is stamped with the tree's frame.
 *
 * A hand-made tree's nodes are all built, so its rays never open one.
 *
 * @param geometry The tree and the scene.
 * @param node The node a branch slot or the root names.
 * @param sharp Whether the ray is sharp (Ray::sharp).
 */
std::uint32_t enteredNode(const Geometry& geometry, std::uint32_t node, bool sharp);

/**
 * Have a node built finely enough for a ray and return the node that
 * stands for it.
 *
 * An unbuilt node is claimed and built in place: the whole programs of
 * its objects whose box meets its cell are cut to the cell, its 8
 * children and the 8 children of each that branches are planned (on the waiting threads too where it
 * stands above the fourth level), the leaves written, passed and
 * packed, and the node placed and published. It is Loose where the ray
 * is not sharp and any of those cells was fitted (a leaf with a face
 * run) to more than a sharp ray allows, else Fine: a cell that
 * branches, or that no surface reaches, is the same for both. A loose node a sharp ray enters is left as it
 * is: a finer node is built at a new index, the loose one's record
 * names it and its state turns Forwarded, so rays inside the loose one
 * walk on undisturbed. A node another thread is building is waited
 * for, and a forwarded node gives its finer one.
 *
 * A node above the fourth level cuts most of the scene's programs for
 * each of its cells; once it is placed, the memory that freed is handed
 * back to the system (releasedFreeMemory()), else each thread's heap
 * keeps it for as long as the tree is drawn.
 *
 * A node that waits for its tiles (TileListing::Waiting) is built from
 * the scene's objects alone, and lists them (Listed), where its cell
 * meets no tile within the blocks' bounds (cellBesideTiles()); else
 * from the scene's objects alone where its cell lies beyond the reach
 * (Beyond); else, where its cell is too wide to run tiles
 * (cellAboveTiles()), it branches into every slot no object fills, and
 * else it runs the tiles its cell meets (ranTiles()), lists their
 * objects with its own and is built from them all (Ran).
 *
 * @param geometry The tree and the scene.
 * @param node The node.
 * @param sharp Whether the ray is sharp.
 */
std::uint32_t openedNode(const Geometry& geometry, std::uint32_t node, bool sharp);

/**
 * Build every node of a growing tree as a sharp ray would, level by
 * level from the root, until none is left unbuilt or loose; nothing
 * for a hand-made tree.
 *
 * The tree then holds the leaves any walk could come to, whatever the
 * number of workers. For tools and checks that read a whole tree.
 *
 * A mutable reference, as rule 46 allows for build-time accumulators.
 *
 * @param tree The tree.
 * @param workers How many threads build, at least one.
 */
void openEveryCell(Tree& tree, int workers);

/**
 * Have a kept tree follow its scene, whose shapes were replaced: every
 * built node a changed object decided or decides is dropped, to be
 * built again when a ray next enters it, and the rest is kept.
 *
 * The scene's objects are matched to the object slots (Growth::slots)
 * by key first: an object whose key a slot holds takes that slot,
 * wherever the scene lists it, and is not changed; of several objects of
 * one key, the first listed takes the lowest such slot. A slot whose key
 * the scene no longer holds is changed and freed, and each object left
 * over is changed and takes the lowest free slot, one freed here among
 * them, or a new one; the slots of tiles' objects are left to the
 * tiles. The changed slots' objects are bounded again
 * (boundObjects()), the held tiles follow the scene's tiles blocks and
 * give the slots they changed (followedTiles()), and each changed slot
 * goes down from the root. At a node it is followed where the node's list
 * names it (where it was) or where its box meets the node's cell and its
 * program, cut as a build cuts it, is not empty over the cell (where it
 * is); the list is set right. In a built node its program is cut on to
 * the node's 64 slots. The node is dropped as soon as any slot that is
 * not a branch is touched: a leaf whose programs name the object, or a
 * leaf or an empty slot the object reaches. Else the object goes on
 * into the branch slots alone. So a slot that is kept is decided by
 * objects that did not change, and a node that is built again is built
 * from the list a fresh tree gives it or a longer one, whose extra
 * objects its cut drops: every leaf a ray can come to is the leaf of a
 * fresh tree within its tolerance. Once it is followed, no node names a
 * slot whose object is gone.
 *
 * The forwards are folded first (prepareTree()), and the hidden
 * primitives are read again, since the scene's leaves may stand
 * elsewhere. It must not run while rays grow the tree.
 *
 * @param tree The tree, whose scene holds the new shapes.
 * @param keys The scene's keys (objectKeys()).
 */
void followScene(Tree& tree, const std::vector<std::uint64_t>& keys);

/**
 * Grow a kept tree's region until it holds a box; nothing for a
 * hand-made tree, or where the box is empty or not finite.
 *
 * Each step puts a new root two levels above the old one: its region is
 * four times as wide along each axis, the old root is kept as it is as
 * one of the new root's 64 branches, and the other 63 are unbuilt nodes
 * that list the old root's objects whose box meets their cell. Along
 * each axis the old root takes the quarter that leaves the least of the
 * box outside, and of those the one that best centres the box. The old
 * root's cell is kept to the bit (Growth::formerRoots), every node
 * stands two levels deeper and the deepest a cell may stand grows with
 * it, so every cell keeps its size, its least tolerance and its leaves.
 *
 * The forwards are folded first (prepareTree()). It must not run while
 * rays grow the tree.
 *
 * @param tree The tree.
 * @param held The box the region must hold.
 */
void growRegion(Tree& tree, const Box& held);

/**
 * Move a kept tree's reach (Growth::reach) to a frame's, between two
 * frames' rays; nothing for a hand-made tree or where the reach stays.
 *
 * Every whole program cut to the reach is moved to the new one
 * (movedReach()). Then, from the root down, a node whose cell lies
 * inside both reaches or beyond both is kept with all below it; else
 * its list takes its parent's objects whose box meets its cell, where
 * the new reach meets it, and a built node is dropped, to be built
 * again when a ray next enters it, where one of its slots that is not
 * a branch and lies neither inside both nor beyond both is a leaf, or
 * is empty but one of its listed objects cut to the new reach is not
 * empty over it; else the walk goes on into its branches.
 *
 * For a tree with tiles, every built node that was built beyond the
 * old reach (TileListing::Beyond) and whose cell the new reach meets is
 * dropped and waits again, to be built with its tiles when a ray next
 * enters it. Every node whose cell the new reach no longer meets and
 * that ran its tiles, or branched to wait for them, is dropped, lists
 * its tiles no more and waits again, to be built beyond the reach; the
 * held tiles are then swept (sweptTiles()), so a walk holds only the
 * tiles about its camera.
 *
 * The tiles then see the reach's eye (HeldTiles::eye). Where that moved
 * the eye snapped to a step some tile read it at (eyeMoved()), the stale
 * tiles run again and are followed as on a changed scene
 * (followScene()): the tiles that read the eye at a step where it moved
 * to another cell, and no other.
 *
 * @param tree The tree.
 * @param reach The frame's reach (shotReach()).
 */
void moveReach(Tree& tree, const Reach& reach);

/**
 * Ready a kept tree for a frame, between two frames' rays; nothing for
 * a hand-made tree.
 *
 * Each forwarded node takes its finer node's place: the finer node and
 * its record are copied to the loose one's index and the loose node's
 * cells are dropped. The Detail then looks through the frame's shot, for
 * the nodes built from here on. Where the camera moved or the frame
 * begins a shot, every built node that was allowed more than twice
 * what the shot allows its cell is dropped, from the root down. Where
 * the tree holds more than Growth::memory (heldBytes()) or any of its
 * arrays is three quarters full, the topmost built nodes no ray entered
 * in the last 3 frames are forgotten, farthest from the eye first,
 * until neither holds: they turn unbuilt, to be built again when a ray
 * enters them; one that ran its tiles lists them no more and waits for
 * them again. Where nodes were dropped or forgotten since the last
 * time, the held tiles no node lists or ran are forgotten, and those
 * only a node that ran them keeps drop their shapes (sweptTiles()),
 * between the rounds of forgetting too. The tree's frame
 * is counted on, and after every 256 nodes built the heap's free
 * memory is handed back to the system (releasedFreeMemory()).
 *
 * @param tree The tree.
 * @param shot The scene's shot with the frame's camera.
 * @param moved Whether the camera moved or the frame begins a shot
 *        (Change::Moved, Change::Cut).
 */
void prepareTree(Tree& tree, const Shot& shot, bool moved);

/**
 * Return how many bytes a tree holds: the elements of its arrays that
 * nodes name, the records, the object lists and the index over shared
 * lists and cuts, without the free spans, the object slots and boxes,
 * and its tiles (HeldTiles::bytes).
 *
 * @param tree The tree.
 */
std::size_t heldBytes(const Tree& tree);

/**
 * Return the tree of a world for growing: the one place that casts a
 * walk's const world to the tree its rays build.
 *
 * @param geometry The world.
 */
Tree& growingTree(const Geometry& geometry);

/**
 * Return the node that stands for a node between passes: itself where
 * it is built, the finer one a forwarded node names, and noNode where
 * it is not built.
 *
 * It must not run while rays grow the tree.
 *
 * @param tree The tree.
 * @param node The node.
 */
std::uint32_t standingNode(const Tree& tree, std::uint32_t node);

/**
 * Return the cell of a growing tree's node: the region halved along the
 * slots from the root down to it, where a cell that lies on a cell an
 * earlier root stood for is that cell (Growth::formerRoots).
 *
 * @param tree The tree.
 * @param node The node.
 */
Box nodeBox(const Tree& tree, std::uint32_t node);

}
