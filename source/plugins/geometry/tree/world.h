#pragma once

#include "core/options.h"
#include "plugins/geometry/tree/detail.h"
#include "plugins/geometry/tree/tree.h"
#include "syntax/evaluator.h"

namespace kodanuki {

/**
 * The tree of a run and the detail it grows to.
 */
struct BuiltGeometry {
	/**
	 * How fine the tree's cells are asked to be.
	 */
	Detail detail;
	/**
	 * The tree.
	 */
	Tree tree;
};

/**
 * Make the tree a scene is drawn with: its objects' boxes
 * (objectBoxes()), its region from them and its tiles (boxedRegion(),
 * heldBoxes()), the Detail of its frame and a tree over them that grows
 * where rays go (lazyTree()).
 *
 * @param file The scene.
 * @param choices The members and settings.
 * @param forgiving Whether a tile's error is printed once and the tile
 *        left empty (the viewer) rather than ending the program.
 */
BuiltGeometry builtGeometry(const SceneFile& file, const Choices& choices, bool forgiving);

/**
 * Have the tree of a run follow its scene, whose shapes may have been
 * replaced, and return whether it could be kept.
 *
 * The tree drops what the objects whose key changed touch
 * (followScene()); a scene loaded again with the same shapes drops
 * nothing. A changed object outside the region is listed at the root
 * and comes in when the region grows to it (growRegion(), each frame).
 * The held tiles see the frame's eye (HeldTiles::eye) and follow the
 * scene's tiles blocks and that eye (followedTiles()), so a moving
 * reach (moveReach()) runs none of them again.
 * The tree is not kept where the scene is not the one the tree was
 * made from, or its tiles blocks have other steps; the caller then
 * makes a tree again, with the least tolerance of this one.
 *
 * @param built The tree and its detail.
 * @param file The scene.
 * @param eye The frame's camera's eye.
 */
bool keptGeometry(BuiltGeometry& built, const SceneFile& file, const math::Vector& eye);

}
