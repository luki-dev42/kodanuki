#pragma once

#include "sdf/scene.h"

#include <cstdint>
#include <vector>

namespace kodanuki {

/**
 * Return whether two scenes hold the same shapes: the same programs,
 * arguments, leaves, nodes and objects, each field equal, and the same
 * tiles blocks: the same steps, bounds and source, and equal keys of every
 * binding the tiles of the first have read so far (TileBlock::reads).
 *
 * @param a One scene.
 * @param b The other.
 */
bool sameShapes(const Scene& a, const Scene& b);

/**
 * Have a scene take over what the tiles of the scene before it gathered:
 * where both have tiles blocks of the same steps and bounds, each block's reads
 * (TileBlock::reads), and where both have tiles, the tile leaves
 * (Scene::tileLeaves). Run where a scene evaluated again replaces the
 * one before.
 *
 * A mutable reference, as rule 46 allows: only the shared parts change.
 *
 * @param before The scene before.
 * @param next The scene that replaces it.
 */
void carriedTiles(const Scene& before, Scene& next);

/**
 * Return a key with one more word mixed in.
 *
 * @param key The key so far.
 * @param word The word.
 */
std::uint64_t mixedKey(std::uint64_t key, std::uint64_t word);

/**
 * Return a key of one program alone: a hash of its steps, result and
 * arguments.
 *
 * Two programs of equal keys are taken to be the same program.
 *
 * @param program The program.
 */
std::uint64_t programKey(const Program& program);

/**
 * Return the least scene leaf a program's steps name, noLeaf where they
 * name none.
 *
 * @param program The program.
 */
std::uint32_t leastLeaf(const Program& program);

/**
 * Return a key of one object's shape and paint: a hash of its program's
 * steps, result and arguments, and of every number of each scene leaf
 * the steps name (its placement, material, coats, rules and groups).
 * The key is the object's own, wherever it stands in the scene: a step
 * names its leaf by how far it stands from the least leaf the program
 * names (leastLeaf()), and a leaf's program and arguments, which the
 * steps hold, are not read by their place in the scene's lists.
 *
 * Two objects of equal keys are taken to be the same object.
 *
 * @param scene The scene.
 * @param object The object, as an index into Scene::objectPrograms.
 */
std::uint64_t objectKey(const Scene& scene, std::uint32_t object);

/**
 * Return the key of every object of a scene (objectKey()), matching
 * Scene::objectPrograms one for one.
 *
 * @param scene The scene.
 */
std::vector<std::uint64_t> objectKeys(const Scene& scene);

/**
 * Return whether two scenes look the same: the same materials, coats
 * and groups, each field equal.
 *
 * @param a One scene's looks.
 * @param b The other's.
 */
bool sameLooks(const Looks& a, const Looks& b);

/**
 * Return whether two scenes are lit the same: the same lights, sky,
 * clouds and stars, each field equal.
 *
 * @param a One scene's lighting.
 * @param b The other's.
 */
bool sameLighting(const Lighting& a, const Lighting& b);

/**
 * Return whether two shots draw alike: the same render settings and the
 * same clip. The camera and the grade are left out.
 *
 * @param a One shot.
 * @param b The other.
 */
bool sameSettings(const Shot& a, const Shot& b);

}
