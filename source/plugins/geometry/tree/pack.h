#pragma once

#include "plugins/geometry/tree/grow.h"
#include "plugins/geometry/tree/tree.h"

namespace kodanuki {

/**
 * Pack a grown subtree into 64-nodes, two octree levels a node.
 *
 * A branch is one node whose 64 slots are its grandchildren, a coarse
 * child that is a leaf writing its kind at the first of its eight. The
 * nodes it names are written in a row, then packed in turn, depth
 * first. The leaves come across as they are, reordered into the order
 * the nodes name them; nothing is fitted again. The octree is dropped
 * and the root's record kept apart.
 *
 * @param subtree The grown subtree.
 */
Subtree packedSubtree(const Subtree& subtree);

}
