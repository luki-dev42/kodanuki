#pragma once

#include "plugins/geometry/tree/cut.h"
#include "plugins/geometry/tree/hit.h"
#include "plugins/geometry/tree/tree.h"
#include "sdf/program.h"
#include "sdf/scene.h"

#include <cstdint>
#include <vector>

namespace kodanuki {

/**
 * Return the boxes an object's surface may pass through, found from the
 * object's own field: what a preparation reads of where an object is,
 * since a tree holds only the cells rays entered.
 *
 * The region is halved level by level, and a box is kept where the
 * field at its middle is no further from 0 than half the box's
 * diagonal; a field that never overstates the distance to its surface
 * loses no box the surface crosses. The boxes of the last level come
 * back. A level that keeps more than 12500 boxes, whose halves would be
 * more than 100000, ends the descent, and its boxes come back, larger
 * than asked for.
 *
 * @param program The object's whole program (Scene::objectPrograms).
 * @param region The box that is halved: the tree's region.
 * @param levels How many times the region is halved along each axis.
 */
std::vector<Box> fieldBoxes(const Program& program, const Region& region, int levels);

/**
 * Read an object's own field at a point, with the scene leaf its value
 * comes from: one evaluation of its whole program in duals.
 *
 * @param program The object's whole program.
 * @param point The point.
 */
Sourced objectSurface(const Program& program, const math::Vector& point);

/**
 * Return the objects among some whose box (ObjectBoxes) meets a box,
 * in their order: the objects a cell there may be built from. A tree
 * that does not grow keeps no boxes, and all of them come back.
 *
 * @param geometry The tree and the scene.
 * @param among The objects, as indices into Scene::objectPrograms.
 * @param box The box.
 */
std::vector<std::uint32_t> objectsMeeting(const Geometry& geometry, const std::vector<std::uint32_t>& among, const Box& box);

/**
 * Return every object of a scene, ascending.
 *
 * @param scene The scene's geometry.
 */
std::vector<std::uint32_t> everyObject(const Scene& scene);

/**
 * Read some objects' field together at a point, the least of their own
 * fields, with the scene leaf its value comes from: objectSurface() of
 * the object whose field is least there. No object reads as a field of
 * infinity that no leaf gives.
 *
 * @param scene The scene's geometry.
 * @param objects The objects, as indices into Scene::objectPrograms.
 * @param point The point.
 */
Sourced objectsSurface(const Scene& scene, const std::vector<std::uint32_t>& objects, const math::Vector& point);

/**
 * Return how far a tree lets the surface in a box stray for a ray that
 * is not sharp (cellTolerance() of the Detail it grows by), 0 for a
 * hand-made tree.
 *
 * @param tree The tree.
 * @param box The box.
 */
double looseStray(const Tree& tree, const Box& box);

}
