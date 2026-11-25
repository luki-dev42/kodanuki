#pragma once

#include "plugins/geometry/tree/cut.h"
#include "sdf/scene.h"

#include <cstdint>
#include <map>
#include <vector>

namespace kodanuki {

/**
 * The boxes a scene's objects lie in, with the boxes of the primitives
 * they are made of, kept so that an object that moved or changed is
 * bounded again from the primitives already bounded.
 *
 * A primitive is bounded once in its own space, by interval bounds of
 * its program; its box is carried through its placement, and an
 * object's box is put together along its joins: a merge holds its
 * children's boxes, a cut its first child's, a clip what its children's
 * boxes share. A smooth merge lowers its field by at most a quarter of
 * its radius at each join, so its children are bounded where their
 * field is below that much more: the boxes hold whatever field the
 * scene's shapes have, distance or not.
 */
struct ObjectBoxes {
	/**
	 * One box for each object of Scene::objectPrograms, at the same
	 * index: every point where the object's field is below 0 lies in it.
	 * A side nothing bounds is infinite; an object with no inside has a
	 * box that holds no point (boxEmpty()).
	 */
	std::vector<Box> boxes;
	/**
	 * The box of each primitive bounded so far, in the primitive's own
	 * space: every point where its program is below a level lies in it.
	 * The key is the program's key (programKey()) in two halves, the level
	 * and the program's arguments, so a primitive is found again wherever
	 * its program stands in Scene::programs.
	 */
	std::map<std::vector<double>, Box> primitives;
};

/**
 * Bound every object of a scene (ObjectBoxes).
 *
 * @param scene The scene's geometry.
 * @param workers How many threads bound the primitives.
 */
ObjectBoxes objectBoxes(const Scene& scene, int workers);

/**
 * Return the boxes of some objects of a scene (ObjectBoxes::boxes), one
 * for each, as of the scene's shapes now. Primitives bounded before are
 * not bounded again; the others are bounded and kept. The boxes the
 * objects had are left as they were.
 *
 * A mutable reference, as rule 46 allows: the primitives' boxes are
 * kept and grow.
 *
 * @param boxes The boxes, whose primitives grow.
 * @param scene The scene's geometry.
 * @param objects The objects, as indices into Scene::objectPrograms.
 * @param workers How many threads bound the primitives.
 */
std::vector<Box> boundObjects(ObjectBoxes& boxes, const Scene& scene, const std::vector<std::uint32_t>& objects, int workers);

/**
 * Return the box that holds no point: every lower end at infinity and
 * every upper end at minus infinity, so that it adds nothing to a hull.
 */
Box nothingBox();

/**
 * Return whether a box holds no point: whether on some axis its lower
 * end lies above its upper one.
 *
 * @param box The box.
 */
bool boxEmpty(const Box& box);

/**
 * Return the least box that holds two boxes; a box that holds no point
 * adds nothing.
 *
 * @param a The one box.
 * @param b The other box.
 */
Box boxHull(const Box& a, const Box& b);

/**
 * Return what two boxes share, a box that holds no point where they do
 * not meet.
 *
 * @param a The one box.
 * @param b The other box.
 */
Box boxOverlap(const Box& a, const Box& b);

/**
 * Return whether two boxes share a point; boxes that only touch do.
 *
 * @param a The one box.
 * @param b The other box.
 */
bool boxesMeet(const Box& a, const Box& b);

}
