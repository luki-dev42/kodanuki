#pragma once

#include "plugins/geometry/tree/classify.h"
#include "plugins/geometry/tree/cut.h"
#include "plugins/geometry/tree/detail.h"
#include "plugins/geometry/tree/tree.h"
#include "sdf/scene.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace kodanuki {

/**
 * What every cell of a growing subtree reads, shared by all workers.
 */
struct GrowContext {
	/**
	 * The scene's geometry.
	 */
	const Scene& scene;
	/**
	 * The slots the cells' objects are named by (slotProgram()), none
	 * where they are named by their index in the scene.
	 */
	const std::vector<ObjectSlot>& slots;
	/**
	 * How fine each cell must be.
	 */
	const Detail& detail;
	/**
	 * The deepest a cell may stand below the region.
	 */
	int depthCap;
	/**
	 * Whether derived faces are tried (`--use faces=off` turns them
	 * off).
	 */
	bool faces;
	/**
	 * Whether the cells are grown for a sharp ray, and so held to
	 * sharpTolerance(): as finely outside the frame as inside it.
	 */
	bool sharp;
};

/**
 * A cell still to be grown.
 */
struct Sprout {
	/**
	 * The cell.
	 */
	Box box;
	/**
	 * How deep it stands below the region.
	 */
	int depth;
	/**
	 * Its parent's programs, which it cuts, shared by the eight
	 * children of one branch so that a level of the top of the tree
	 * holds each parent's programs once.
	 */
	std::shared_ptr<const CellPrograms> programs;
};

/**
 * The record of a subtree's root, kept apart from its nodes.
 *
 * A packed node's children stand in a row their parent wrote, so a
 * subtree's root node is written into the place its parent keeps for it
 * when the subtree is placed (plugins/geometry/tree/lazy.h).
 */
struct RootRecord {
	/**
	 * What the root holds.
	 */
	CellKind kind;
	/**
	 * The root's node, read for a branch.
	 */
	PackedNode node;
	/**
	 * The root's leaf, read for a surface.
	 */
	SurfaceLeaf surface;
	/**
	 * The root's leaf, read for a filled root.
	 */
	SolidLeaf solid;
};

/**
 * A subtree grown by one worker, first as an octree, then packed.
 *
 * Its indices are its own until it is placed into the tree
 * (plugins/geometry/tree/lazy.h).
 */
struct Subtree {
	/**
	 * The subtree's root cell.
	 */
	Box box;
	/**
	 * How deep the root stands below the region, always even, so that a
	 * packed node covers two whole levels.
	 */
	int depth;
	/**
	 * The octree, root first, a branch's eight children in a row. It is
	 * emptied when the subtree is packed.
	 */
	std::vector<CellEntry> cells;
	/**
	 * The surface leaves, in the order they were grown and then in node
	 * order once packed.
	 */
	std::vector<SurfaceLeaf> surfaces;
	/**
	 * The solid leaves, likewise.
	 */
	std::vector<SolidLeaf> solids;
	/**
	 * The face runs of the surface leaves.
	 */
	std::vector<std::uint32_t> runs;
	/**
	 * The program lists the leaves name.
	 */
	std::vector<ProgramList> lists;
	/**
	 * The references the lists hold.
	 */
	std::vector<ProgramReference> references;
	/**
	 * The programs cut to a leaf's cell that references name.
	 */
	std::vector<ProgramCut> cuts;
	/**
	 * The entries of the cuts, end to end, as Tree::cutSteps.
	 */
	std::vector<std::uint16_t> cutSteps;
	/**
	 * The packed nodes below the root, empty until the subtree is
	 * packed.
	 */
	std::vector<PackedNode> nodes;
	/**
	 * The root's record once the subtree is packed.
	 */
	RootRecord root;
};

/**
 * Grow the whole octree below one cell.
 *
 * Each cell asks Detail how far it may stray, is classified, and asks
 * Detail's verdict; a leaf is written down, and a branch's eight
 * children are grown depth first in octant order, so the subtree is
 * the same however it is scheduled. Leaves with the same programs share
 * a program list.
 *
 * @param context What every cell reads.
 * @param sprout The subtree's root cell.
 */
Subtree growSubtree(const GrowContext& context, const Sprout& sprout);

/**
 * Find what one cell holds: its tolerance, its plan and Detail's
 * verdict, which turns a kept cell into a branch. A surface cell whose
 * run proves that no face crosses it (surfacelessSide) is an empty or
 * a filled leaf.
 *
 * @param context What every cell reads.
 * @param sprout The cell.
 */
Plan planCell(const GrowContext& context, const Sprout& sprout);

/**
 * Return a subtree that holds only its root cell, still empty.
 *
 * @param sprout The root cell.
 */
Subtree emptySubtree(const Sprout& sprout);

/**
 * Write one planned leaf into a subtree's octree: its kind, its leaf
 * record, its run and its program list.
 *
 * A mutable reference, as rule 46 allows for build-time accumulators.
 *
 * @param subtree The subtree.
 * @param cell The cell's index in the octree.
 * @param plan The leaf's plan.
 */
void writeLeaf(Subtree& subtree, std::uint32_t cell, const Plan& plan);

/**
 * Turn one cell of a subtree's octree into a branch, with eight empty
 * children in a row at its end, and return the first child's index.
 *
 * @param subtree The subtree.
 * @param cell The cell's index in the octree.
 */
std::uint32_t openBranch(Subtree& subtree, std::uint32_t cell);

/**
 * Return the box of the region's root cell.
 *
 * @param context What every cell reads, for nothing but the scene.
 * @param region The region.
 */
Sprout rootSprout(const GrowContext& context, const Region& region);

}
