#pragma once

#include "core/geometry.h"
#include "plugins/geometry/tree/boxes.h"
#include "sdf/program.h"
#include "sdf/scene.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kodanuki {

/**
 * Hand the memory the heap holds free back to the system: malloc_trim
 * where glibc is the C library, nothing elsewhere.
 *
 * Opening a node near the root frees far more than it keeps, spread
 * over the threads' heaps, and glibc keeps those pages; what is
 * allocated after it would come on top of them.
 */
void releasedFreeMemory();

/**
 * Have every block of 1 MB and more mapped on its own, and unmapped as
 * soon as it is freed, where glibc is the C library (mallopt with
 * M_MMAP_THRESHOLD, which also stops glibc from raising the threshold as
 * large blocks are freed); nothing elsewhere.
 *
 * The tree's arrays are reserved far larger than they are filled, each
 * as one such block, so that the pages no node was written to are never
 * resident.
 */
void largeBlocksMapped();

/**
 * Return how many bytes of memory the machine has, 8 GB where the
 * system does not say.
 */
std::size_t machineMemory();

/**
 * The render's reach: the sphere of radius Render::far about the
 * camera's eye. The tree cuts every object to it (reachedProgram()).
 */
struct Reach {
	/**
	 * The sphere's centre, the camera's eye.
	 */
	math::Vector eye;
	/**
	 * The sphere's radius, infinity for a reach that cuts nothing.
	 */
	double far;
};

/**
 * Return the reach of a shot: the sphere of radius far about the
 * camera's eye.
 *
 * @param shot The scene's shot.
 */
Reach shotReach(const Shot& shot);

/**
 * Return the reach that cuts nothing, about a region's middle: a tree's
 * reach until its first frame moves it (moveReach()).
 *
 * @param region The tree's region.
 */
Reach openReach(const Region& region);

/**
 * Return whether a box meets a reach: its nearest point lies within
 * the sphere.
 *
 * @param reach The reach.
 * @param box The box.
 */
bool reachMeets(const Reach& reach, const Box& box);

/**
 * Return whether a reach holds a whole box: its farthest corner lies
 * within the sphere.
 *
 * @param reach The reach.
 * @param box The box.
 */
bool reachHolds(const Reach& reach, const Box& box);

/**
 * Return an object's whole program cut to a reach: the program, then
 * the sphere's field, length(p - eye) - far, and the maximum of the two
 * as its result. The sphere's steps paint the object's least leaf. A
 * program too long to take the steps comes back as it was.
 *
 * @param whole The object's whole program.
 * @param reach The reach.
 */
Program reachedProgram(const Program& whole, const Reach& reach);

/**
 * Move the sphere of a program cut to a reach (reachedProgram()) to
 * another reach, in place; nothing for a program not cut.
 *
 * A mutable reference, as rule 46 allows for build-time accumulators.
 *
 * @param program The cut program.
 * @param reach The new reach.
 */
void movedReach(Program& program, const Reach& reach);

/**
 * Return the hull of the object boxes out to the render's reach: each
 * box cut to the reach's bounding cube (reachCube()).
 *
 * @param boxes The boxes of the scene's objects (ObjectBoxes::boxes).
 * @param shot The scene's shot: the camera's eye and the render's
 *        reach.
 */
Box reachedHull(const std::vector<Box>& boxes, const Shot& shot);

/**
 * Return the cube that bounds the render's reach: the cube of twice
 * Render::far about the camera's eye.
 *
 * @param shot The scene's shot.
 */
Box reachCube(const Shot& shot);

/**
 * Return the boxes a region must hold for a scene: its objects' boxes,
 * and where the scene has tiles, which may stand anywhere, the box of
 * all space, which the reach cuts down to the cube about the camera's
 * eye (reachedHull()).
 *
 * @param boxes The boxes of the scene's objects, or of a tree's slots.
 * @param scene The scene.
 */
std::vector<Box> heldBoxes(const std::vector<Box>& boxes, const Scene& scene);

/**
 * Find the cube a scene's geometry lies in, out to the render's reach:
 * the cube about the hull of the object boxes, each cut to the reach's
 * bounding cube (reachCube()), widened by a hundredth.
 *
 * @param boxes The boxes of the scene's objects (ObjectBoxes::boxes).
 * @param shot The scene's shot: the camera's eye and the render's
 *        reach.
 */
Region boxedRegion(const std::vector<Box>& boxes, const Shot& shot);

/**
 * Find the cube a scene's geometry lies in, out to the render's reach
 * (boxedRegion()), bounding its objects first (objectBoxes()).
 *
 * @param scene The scene's geometry.
 * @param shot The scene's shot: the camera's eye and the render's
 *        reach.
 * @param workers How many threads bound the objects.
 */
Region sceneRegion(const Scene& scene, const Shot& shot, int workers);

/**
 * Return whether a box lies inside a region; an empty box lies inside
 * any region.
 *
 * @param box The box.
 * @param region The region.
 */
bool boxInRegion(const Box& box, const Region& region);

/**
 * Return the deepest a cell may stand, from the least tolerance: the
 * depth at which a cell is four least
 * tolerances wide along the region's longest side, to the nearest.
 *
 * @param region The box the tree covers.
 * @param least The least tolerance.
 */
int depthCapOf(const Region& region, double least);

}
