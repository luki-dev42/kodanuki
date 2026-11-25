#pragma once

#include "plugins/geometry/tree/grow.h"
#include "plugins/geometry/tree/tree.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kodanuki {

/**
 * The index that names no program list: what sharedLists() gives for a
 * node's list that none of its leaves names.
 */
constexpr std::uint32_t noList = 0xFFFFFFFFu;

/**
 * An index over one of a growing tree's arrays of program lists or cuts,
 * so that an element alike to one already held is not stored twice, and
 * how many hold each element, so that it is given back when none does.
 *
 * Slots are open addressed, a power of two of them, and hold one more
 * than the element they name, so that 0 is free; an element taken out
 * moves the ones after it back to where they belong.
 */
struct SharedIndex {
	/**
	 * The slots.
	 */
	std::vector<std::uint32_t> slots;
	/**
	 * Each element's hash, by the element's index.
	 */
	std::vector<std::uint32_t> hashes;
	/**
	 * How many hold each element, by its index: leaves for a list,
	 * lists for a cut.
	 */
	std::vector<std::uint32_t> uses;
	/**
	 * How many slots are taken.
	 */
	std::size_t taken;
};

/**
 * The program lists and cuts a growing tree's nodes share.
 */
struct SharedPrograms {
	/**
	 * Over Tree::lists, by the references each list names.
	 */
	SharedIndex lists;
	/**
	 * Over Tree::cuts, by each cut's object, result and entries.
	 */
	SharedIndex cuts;
};

/**
 * Return an index over nothing.
 */
SharedPrograms emptySharedPrograms();

/**
 * Store the program lists a node's leaves name, and the cuts they name,
 * into a growing tree's arrays, and return each list's index in
 * Tree::lists by its index in the node, noList for one no leaf names.
 *
 * A list or cut alike to one the tree holds is that one; else it is
 * written into a free span or at the array's end. Each leaf naming a
 * list counts as one use of it, and each list naming a cut as one of
 * the cut. It runs under the growth lock.
 *
 * @param tree The tree.
 * @param packed The node, packed, with its lists, references, cuts and
 *        entries numbered within it.
 */
std::vector<std::uint32_t> sharedLists(Tree& tree, const Subtree& packed);

/**
 * Give back one use of a program list: a leaf that named it is dropped.
 * A list no leaf names any more is given back with its references, and
 * so is a cut no list names any more, with its entries.
 *
 * @param tree The tree.
 * @param list The list's index in Tree::lists.
 */
void releasedList(Tree& tree, std::uint32_t list);

/**
 * Return how many bytes the index over a growing tree's lists and cuts
 * holds.
 *
 * @param shared The index.
 */
std::size_t sharedBytes(const SharedPrograms& shared);

}
