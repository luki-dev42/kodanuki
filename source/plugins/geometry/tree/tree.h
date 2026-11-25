#pragma once

#include "core/geometry.h"
#include "sdf/program.h"
#include "sdf/scene.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace kodanuki {

/**
 * What one cell of the tree holds.
 *
 * The values are the two bits a packed node keeps for each of its 64
 * slots, so their order is fixed: a slot is empty 00, filled 01, surface
 * 10 and branch 11.
 */
enum class CellKind : std::uint8_t {
	/**
	 * Wholly outside every object, so a ray passes through it.
	 */
	Empty,
	/**
	 * Wholly inside an object, so a ray stops where it enters it.
	 */
	Filled,
	/**
	 * The surface may pass through it, so it keeps a run of faces.
	 */
	Surface,
	/**
	 * Split into eight children.
	 */
	Branch,
};

/**
 * Where a program that a leaf names comes from.
 */
enum class ProgramSource : std::uint8_t {
	/**
	 * A whole object's program, named by its slot (ObjectSlot).
	 */
	Object,
	/**
	 * An object's program cut down to the leaf's cell by the program
	 * cutting pass, indexed in Tree::cuts.
	 */
	Cut,
};

/**
 * The form a face of a surface leaf is kept in.
 *
 * Every face is written over the cell's own coordinates, where the cell
 * is the unit cube from 0 at its lower side to 1 at its upper side, as
 * the walk reads it.
 */
enum class FaceKind : std::uint8_t {
	/**
	 * A plane, four floats: a, b, c and d, inside where a u + b v + c w
	 * + d is 0 or less.
	 */
	Plane,
	/**
	 * A quadric, ten floats: the terms of uu, vv, ww, uv, uw, vw, u, v,
	 * w and one, inside where it is 0 or less.
	 */
	Quadric,
	/**
	 * A patch, five or eleven words, inside where the patch is 0 or
	 * less (plugins/geometry/tree/patch.h).
	 */
	Patch,
	/**
	 * A ring, fourteen floats: where the cell's lower corner lies in the
	 * ring's frame, then how far the frame moves along a whole side of
	 * the cell in u, in v and in w, three each, then the major radius,
	 * always 1, and the minor radius. In the frame the ring lies flat
	 * about the z axis, and v is inside where (|v|^2 + 1 - minor^2)^2 -
	 * 4 (vx^2 + vy^2) is 0 or less; a negative minor radius turns the
	 * inside to the ring's outside.
	 */
	Ring,
};

/**
 * How far a packed node is built.
 *
 * The values are what PackedNode::state holds, so a zeroed node is
 * unbuilt.
 */
enum class NodeState : std::uint8_t {
	/**
	 * A placeholder: nothing of it is written but its record
	 * (plugins/geometry/tree/lazy.h). The first ray that enters it
	 * builds it.
	 */
	Unbuilt,
	/**
	 * A thread is building it, or a finer node in its place; a ray that
	 * enters it waits.
	 */
	Opening,
	/**
	 * Built for rays that are not sharp: some of its leaves stand outside
	 * the frame and are fitted to the looser tolerance there. A sharp ray
	 * that enters it has a finer node built in its place.
	 */
	Loose,
	/**
	 * Built as finely as any ray asks.
	 */
	Fine,
	/**
	 * A finer node stands for it, named by its record. Rays that were
	 * inside it keep walking what it holds.
	 */
	Forwarded,
};

/**
 * The slot that holds no object.
 */
constexpr std::uint32_t noObject = 0xFFFFFFFFu;

/**
 * The tile of an object of the scene itself (ObjectSlot::tile).
 */
constexpr std::uint32_t noTile = 0xFFFFFFFFu;

/**
 * One slot of a growing tree's objects: what its lists, cuts and leaves
 * name an object by, in place of the object's place in the scene's
 * list.
 *
 * An object keeps its slot for as long as its key does: the scene may
 * list it elsewhere between frames, and its slot follows it there. A
 * slot whose object is gone is free and is given to the next new object
 * (followScene(), plugins/geometry/tree/lazy.h). An object of a tile
 * holds its slot while the tree holds the tile and the tile makes an
 * object of its key (plugins/geometry/tree/tiles.h).
 */
struct ObjectSlot {
	/**
	 * The object's key (objectKey()), 0 for a free slot.
	 */
	std::uint64_t key;
	/**
	 * The object, as an index into Scene::objectPrograms, or into its
	 * tile's objects; noObject for a free slot.
	 */
	std::uint32_t object;
	/**
	 * The least scene leaf the object's program names (leastLeaf()): a
	 * leaf the tree keeps of the object is kept as how far it stands
	 * from this one, so it follows the object's leaves when the scene
	 * lists them elsewhere.
	 */
	std::uint32_t leaves;
	/**
	 * The held tile the object is of, noTile for an object of the scene.
	 */
	std::uint32_t tile;
	/**
	 * The object's whole program, null for a free slot.
	 */
	const Program* program;
};

/**
 * What a cell holds and where the rest of it is kept.
 *
 * It is a cell of the octree while a subtree grows, and the root of
 * the packed tree once it is built.
 */
struct CellEntry {
	/**
	 * What the cell holds.
	 */
	CellKind kind;
	/**
	 * Where the rest of it is: the first of eight children for a branch
	 * of the octree, a packed node for the root of the packed tree, the
	 * surface leaf for a surface cell and the solid leaf for a filled
	 * one. It is unread for an empty cell.
	 */
	std::uint32_t payload;
};

/**
 * One program a leaf names.
 */
struct ProgramReference {
	/**
	 * Where the program is, in the list its source names.
	 */
	std::uint32_t index;
	/**
	 * Which list it is in.
	 */
	ProgramSource source;
};

/**
 * An object's program cut to a cell, as the steps of the whole program
 * it keeps.
 *
 * The cut is run in the whole program's register numbering over the
 * kept steps alone (KeptSteps), held as runs of consecutive steps. A
 * join whose sides are apart over the cell is not run: it is rerouted
 * to the side that decides it, so a kept step that reads it reads that
 * side. The cut answers exactly what the whole program does at every
 * point of the cell, and its steps are the whole program's, shared by
 * every cut of the object.
 */
struct ProgramCut {
	/**
	 * The object, as its slot (ObjectSlot).
	 */
	std::uint32_t object;
	/**
	 * Where its entries begin in the cut steps of the tree.
	 */
	std::uint32_t first;
	/**
	 * How many runs of consecutive steps it keeps.
	 */
	std::uint16_t ranges;
	/**
	 * How many joins it reroutes.
	 */
	std::uint16_t rerouted;
	/**
	 * The step whose value is the cut's.
	 */
	std::uint32_t result;
};

/**
 * The programs a leaf evaluates at a hit, as a run of references.
 *
 * The field of the leaf is the minimum of the programs, since the
 * objects of a scene are a hard union. Two leaves with the same
 * programs name one list.
 */
struct ProgramList {
	/**
	 * The first reference of the run, in Tree::references.
	 */
	std::uint32_t first;
	/**
	 * How many references the run has, at least one.
	 */
	std::uint32_t count;
};

/**
 * A leaf of the tree that the surface passes through.
 */
struct SurfaceLeaf {
	/**
	 * The first word of the leaf's face run in Tree::runs.
	 */
	std::uint32_t run;
	/**
	 * The program list a hit evaluates, in Tree::lists.
	 */
	std::uint32_t programs;
	/**
	 * How far the leaf's surface, or a neighbouring patch of the same
	 * tolerance, may stray from the field: the leaf's tolerance,
	 * rounded up to a power of two and kept as the top 16 bits of a
	 * float. A ray leaving a hit here starts twice this much further
	 * off, so it starts above the patches beside it as well as its own.
	 */
	std::uint16_t loosened;
	/**
	 * Whether every face of the leaf is held within its tolerance.
	 */
	LeafKind kind;
};

/**
 * A leaf of the tree that is wholly inside an object.
 */
struct SolidLeaf {
	/**
	 * The program list a hit on the leaf evaluates for its leaf, in
	 * Tree::lists.
	 */
	std::uint32_t programs;
};

/**
 * One node of the packed tree: two octree levels, 64 slots.
 *
 * A slot's index has the coarse child in its high three bits and the
 * octant within it in its low three, each as x in bit 0, y in bit 1
 * and z in bit 2 (set means the upper half). A coarse child that is a
 * leaf on the first level writes its kind at the first of its eight
 * slots and leaves the other seven empty. A slot's payload is its
 * kind's base plus the number of slots of that kind below it.
 *
 * A branch slot may name a node that is not built yet (NodeState), so a
 * walk enters a node through enteredNode()
 * (plugins/geometry/tree/lazy.h).
 */
struct PackedNode {
	/**
	 * The low bit of each slot's kind.
	 */
	std::uint64_t lowerKinds;
	/**
	 * The high bit of each slot's kind.
	 */
	std::uint64_t upperKinds;
	/**
	 * Where the node's branch children begin in Tree::nodes.
	 */
	std::uint32_t firstBranch;
	/**
	 * Where the node's surface leaves begin in Tree::surfaces.
	 */
	std::uint32_t firstSurface;
	/**
	 * Where the node's solid leaves begin in Tree::solids.
	 */
	std::uint32_t firstSolid;
	/**
	 * One bit per coarse child, set where the child is a leaf on the
	 * first level.
	 */
	std::uint8_t coarse;
	/**
	 * One bit per coarse child, set where any of its slots holds
	 * anything. The walk enters only these.
	 */
	std::uint8_t busy;
	/**
	 * How far the node is built, a NodeState. Walks read it and builders
	 * write it at once, so both go through std::atomic_ref; every other
	 * field is written before the state says the node is built.
	 */
	std::uint8_t state;
	/**
	 * The frame a ray last entered the node in, as Tree::frame was.
	 */
	std::uint8_t stamp;
};

/**
 * How many words the slab after a face of a slabbed run takes.
 */
constexpr std::uint32_t slabWordCount = 3;

/**
 * The first word of a face run, read apart.
 *
 * A run is the header, then the truth table, then the faces in order.
 * The table has one bit per pattern of the faces' insides: bit i says
 * whether the cell is inside where face j is inside exactly when bit j
 * of i is set. It takes tableWordCount(count) words; a run of one face
 * has no table, and the cell is inside where that face is.
 */
struct RunHeader {
	/**
	 * How many faces the run holds, from 1 to 8.
	 */
	std::uint32_t count;
	/**
	 * Which faces are patches, face j's bit 1 << j.
	 */
	std::uint32_t patches;
	/**
	 * Which faces are planes, face j's bit 1 << j. A face in neither
	 * mask is a quadric, and a face in both is a ring.
	 */
	std::uint32_t planes;
	/**
	 * Which faces every inside entry of the table has inside. Where a
	 * ray is never inside such a face in the cell, it meets nothing
	 * there.
	 */
	std::uint32_t needed;
	/**
	 * Whether every face that is not a plane is followed by its slab,
	 * slabWordCount words that the Build pass member FaceSlabs keeps
	 * (plugins/geometry/tree/pass.h) and the leaf's solve asks before it solves the face.
	 */
	bool slabbed;
	/**
	 * How many words a Build pass member wrote after the table (0 to 7),
	 * read back through the point's dispatch (plugins/geometry/tree/pass.h).
	 */
	std::uint32_t passWords;
};

struct Growth;

/**
 * The packed tree of one scene, as rays walk it.
 *
 * A tree that grows (lazyTree(), plugins/geometry/tree/lazy.h) builds a
 * node when a ray first enters it. Its arrays are reserved once and
 * never move, so rays read them while other rays' nodes are written at
 * their ends; the order of its nodes and leaves follows the order rays
 * came in, and its leaves are the same whatever that order.
 */
struct Tree {
	/**
	 * The box the tree covers.
	 */
	Region region;
	/**
	 * What the whole region holds: a branch names the root node.
	 */
	CellEntry root;
	/**
	 * The packed nodes.
	 */
	std::vector<PackedNode> nodes;
	/**
	 * The surface leaves, in the order the nodes name them.
	 */
	std::vector<SurfaceLeaf> surfaces;
	/**
	 * The solid leaves, in the order the nodes name them.
	 */
	std::vector<SolidLeaf> solids;
	/**
	 * The face runs of the surface leaves, end to end.
	 */
	std::vector<std::uint32_t> runs;
	/**
	 * The program lists the leaves name.
	 */
	std::vector<ProgramList> lists;
	/**
	 * The references the program lists hold, end to end.
	 */
	std::vector<ProgramReference> references;
	/**
	 * The programs cut to a cell that references name.
	 */
	std::vector<ProgramCut> cuts;
	/**
	 * The entries of the cuts, end to end: for each cut a pair (first
	 * step, one past the last) for each run of kept steps, then a pair
	 * (step, the step it stands for) for each rerouted join.
	 */
	std::vector<std::uint16_t> cutSteps;
	/**
	 * The frame being drawn, which a node entered in it is stamped with.
	 */
	std::uint8_t frame;
	/**
	 * What the tree grows from; null for a tree made by hand, whose
	 * nodes are all built. A tree that grows is moved, never copied,
	 * since a copy's arrays have no room to grow into.
	 */
	std::shared_ptr<Growth> growth;
};

/**
 * A leaf of the packed tree and the cell it fills.
 */
struct PlacedLeaf {
	/**
	 * The leaf: a surface or a solid.
	 */
	CellEntry entry;
	/**
	 * How many times the region is halved along each axis to make the
	 * cell.
	 */
	int level;
	/**
	 * The cell's whole coordinates along x, y and z at its level.
	 */
	std::uint32_t place[3];
};

/**
 * Counts of a built tree, for the checks.
 */
struct TreeStats {
	/**
	 * How many packed nodes there are.
	 */
	std::size_t nodes;
	/**
	 * How many cells the octree the tree was packed from has: its root
	 * and eight for every branch.
	 */
	std::size_t cells;
	/**
	 * How many of those cells are branches.
	 */
	std::size_t branches;
	/**
	 * How many of those cells are empty.
	 */
	std::size_t empty;
	/**
	 * How many surface leaves there are.
	 */
	std::size_t surfaces;
	/**
	 * How many solid leaves there are.
	 */
	std::size_t solids;
	/**
	 * How many surface leaves are one patch over the whole cell.
	 */
	std::size_t patched;
	/**
	 * How many faces of each kind the runs hold, by FaceKind.
	 */
	std::size_t faces[4];
	/**
	 * How many surface leaves hold each number of faces, from 0 to 8.
	 */
	std::size_t byCount[9];
	/**
	 * How many surface leaves are floor leaves.
	 */
	std::size_t floors;
	/**
	 * How many megabytes the tree takes.
	 */
	double megabytes;
};

/**
 * Return what one slot of a packed node holds.
 *
 * @param node The node.
 * @param slot The slot, from 0 to 63.
 */
CellKind slotKind(const PackedNode& node, int slot);

/**
 * Return where one slot's payload is: its node, surface leaf or solid
 * leaf.
 *
 * The slot must not be empty.
 *
 * @param node The node.
 * @param slot The slot, from 0 to 63.
 */
std::uint32_t slotPayload(const PackedNode& node, int slot);

/**
 * Return how many words a face run's truth table takes, none for a run
 * of one face.
 *
 * @param count How many faces the run holds.
 */
std::uint32_t tableWordCount(std::uint32_t count);

/**
 * Return the word a run's first face starts at, past its header, its
 * table and its pass words.
 *
 * @param header The run's header.
 */
std::uint32_t firstFaceWord(const RunHeader& header);

/**
 * Read the first word of a face run apart.
 *
 * @param word The run's first word.
 */
RunHeader runHeader(std::uint32_t word);

/**
 * Return the first word of a face run.
 *
 * @param header The run's count and masks.
 */
std::uint32_t runHeaderWord(const RunHeader& header);

/**
 * Return the form one face of a run is kept in.
 *
 * @param header The run's header.
 * @param face The face, from 0 to the count.
 */
FaceKind faceKind(const RunHeader& header, std::uint32_t face);

/**
 * Return how many words one face of a run takes, read from the face
 * itself, its slab included where the run is slabbed.
 *
 * @param header The run's header.
 * @param face The face, from 0 to the count.
 * @param words The face's first word.
 */
std::uint32_t faceWordCount(const RunHeader& header, std::uint32_t face, const std::uint32_t* words);

/**
 * Return where each face of a run begins, as offsets from the run's
 * first word, and last where the run ends.
 *
 * @param run The run's first word.
 */
std::vector<std::uint32_t> faceOffsets(const std::uint32_t* run);

/**
 * Write how much further than the least tolerance a leaf may stray in
 * sixteen bits.
 *
 * The amount is rounded up to a power of two, so what is read back is
 * never less than what was written, and the bits are the top half of
 * the float that holds it. An amount of 0 or less is written as 0.
 *
 * @param amount How much further, in world units.
 */
std::uint16_t loosening(double amount);

/**
 * Return how much further than the least tolerance a leaf may stray, in
 * world units.
 *
 * @param leaf The leaf.
 */
double loosenedOf(const SurfaceLeaf& leaf);

/**
 * Return how many levels a tree's root stands above its first root:
 * two for each time its region grew (growRegion(), plugins/geometry/tree/lazy.h), none for a
 * hand-made tree. A grid of the region halved that many levels more
 * keeps the cell size of the first region.
 *
 * @param tree The tree.
 */
int grownLevels(const Tree& tree);

/**
 * Count the cells, leaves, faces and bytes of a tree.
 *
 * The cells are those of the nodes built so far; the nodes, leaves and
 * bytes are everything the tree's arrays hold, placeholders and nodes a
 * finer one stands for among them.
 *
 * @param tree The tree.
 */
TreeStats treeStats(const Tree& tree);

/**
 * Return every surface and solid leaf of a packed tree with its cell, in
 * the order of the tree.
 *
 * A node that is not built yet holds none, and a node a finer one
 * stands for gives the finer one's. It must not run while rays grow the
 * tree.
 *
 * @param tree The tree.
 */
std::vector<PlacedLeaf> placedLeaves(const Tree& tree);

/**
 * Return the program a reference names, as the steps to run of its
 * object's whole program (treeProgram()).
 *
 * @param scene The scene whose object programs the tree's cuts keep
 *        steps of.
 * @param tree The tree.
 * @param reference The reference.
 */
KeptSteps referencedSteps(const Scene& scene, const Tree& tree, const ProgramReference& reference);

/**
 * Return the object a reference's program is of, as its slot.
 *
 * @param tree The tree.
 * @param reference The reference.
 */
std::uint32_t referencedSlot(const Tree& tree, const ProgramReference& reference);

/**
 * Return the whole program of the object a slot holds: how builds and
 * walks read a slot as an object of the scene or of a tile. No slots at
 * all name the scene's objects by their index, as a tree made by hand
 * does.
 *
 * @param scene The scene the objects are of.
 * @param slots The slots.
 * @param slot The slot, which holds an object.
 */
const Program& slotProgram(const Scene& scene, const std::vector<ObjectSlot>& slots, std::uint32_t slot);

/**
 * Return the least scene leaf of the object a slot holds
 * (ObjectSlot::leaves); 0 where no slots are kept, so that a leaf is
 * kept as itself.
 *
 * @param slots The slots.
 * @param slot The slot, which holds an object.
 */
std::uint32_t slotLeastLeaf(const std::vector<ObjectSlot>& slots, std::uint32_t slot);

/**
 * Return the least scene leaf of the object a tree's slot holds
 * (slotLeastLeaf() over the slots of the tree's growth, none for a
 * hand-made tree).
 *
 * @param tree The tree.
 * @param slot The slot.
 */
std::uint32_t treeLeastLeaf(const Tree& tree, std::uint32_t slot);

/**
 * Return the whole program of the object a tree's slot holds
 * (slotProgram() over the slots of the tree's growth, none for a
 * hand-made tree).
 *
 * @param scene The scene the tree is of.
 * @param tree The tree.
 * @param slot The slot.
 */
const Program& treeProgram(const Scene& scene, const Tree& tree, std::uint32_t slot);

}
