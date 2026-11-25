#pragma once

#include "core/geometry.h"
#include "sdf/scene.h"

#include <vector>

namespace kodanuki {

/**
 * How near a surface a marched ray stops, in world units: how far a
 * hit may stand from the field's zero (Hit.loosened). It lies far below
 * any tolerance `geometry/tree` builds to, so the tree's hits are
 * judged against these.
 */
constexpr double sphereTolerance = 1e-6;

/**
 * The most steps a ray is marched; a ray that takes more, as one that
 * skims along a surface for its whole length, meets nothing.
 */
constexpr int sphereSteps = 1 << 16;

/**
 * The world `geometry/sphere_trace` marches: the scene's object
 * programs and the reach, the sphere of radius `far` about the eye that
 * `geometry/tree` cuts every object to.
 */
struct SphereWorld {
	/**
	 * The scene's object programs.
	 */
	const std::vector<Program>* programs;
	/**
	 * The middle of the reach: the camera's eye.
	 */
	math::Vector eye;
	/**
	 * The reach's radius, infinity for none.
	 */
	double far;
};

/**
 * Return the cube that holds the render's reach: twice `far` wide about
 * the camera's eye.
 *
 * @param shot The scene's shot: the camera's eye and the render's
 *        reach.
 */
Region reachRegion(const Shot& shot);

/**
 * Return the world of a scene as a shot sees it: its object programs
 * and the reach about the shot's eye.
 *
 * @param scene The scene, whose object programs the world points at.
 * @param shot The shot: the camera's eye and the render's reach.
 */
SphereWorld shotWorld(const Scene& scene, const Shot& shot);

/**
 * Return the world of a scene with no reach, which cuts nothing: what
 * a check that compares against a tree before its first frame marches.
 *
 * @param scene The scene, whose object programs the world points at.
 */
SphereWorld openWorld(const Scene& scene);

/**
 * Return the world of a scene's own field: the scene, the region rays
 * are clipped to, the sphere world as the plugin's world and
 * sphereTraced() as the kernel.
 *
 * @param scene The scene.
 * @param region The box the world lies in.
 * @param world The object programs and the reach, which must outlive
 *        the geometry.
 */
Geometry sphereGeometry(const Scene& scene, const Region& region, const SphereWorld& world);

/**
 * Find the first surface along a ray by sphere tracing the scene's
 * field: the kernel of `geometry/sphere_trace` for one ray.
 *
 * The field is the least of the scene's object programs. The ray is
 * clipped to the region and to the reach, as `geometry/tree` cuts
 * every object to it, and marched by the distance to the nearest
 * object's surface until that is within sphereTolerance.
 *
 * A ray from outside ends there: the hit is a Surface at that point.
 * A ray from inside stands on the surface of one of the solids it is
 * in or beside, and the field is read a little past it. Where every
 * object is outside there, the ray leaves the solids and the hit is a
 * Surface at the point. Where the nearest object past it is of
 * another material than the one the ray travels in (Ray.within), the
 * hit is that solid's, read past the point, as where a glass rests on
 * a floor. Otherwise the ray goes on from past the surface.
 *
 * The hit's normal is the gradient of the nearest object's program,
 * run once in duals, faced as Hit.normal says; its leaf, the leaf
 * across a smooth seam and its share are that program's source there.
 * Objects whose values tie at the point are coplanar, and the tie goes
 * to the least of them a little further along the ray. No cell and no
 * face stand behind the hit: its cell is noLeaf and its leaf kind
 * Held.
 *
 * @param geometry The world sphereGeometry() made.
 * @param ray The ray.
 */
Hit sphereTraced(const Geometry& geometry, const Ray& ray);

} // namespace kodanuki
