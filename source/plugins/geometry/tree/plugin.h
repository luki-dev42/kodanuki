#pragma once

#include "core/plugin.h"

namespace kodanuki {

/**
 * Return the plugin `geometry/tree` as the core lists it: its
 * buildWorld makes the tree of exact faces over the scene
 * (builtGeometry()), which grows where rays go, keeps it until the
 * world is built again and gives firstHit() as the kernel; its
 * prepareFrame grows the region to the objects out to the frame's reach
 * where no region was given by hand (growRegion()), moves the reach,
 * which cuts every object and where tiles are made, to the frame's
 * (moveReach()) and readies the tree for each
 * frame (prepareTree()). It owns the points Detail and Build, and
 * publishes the tree's size as `bytes.tree` (heldBytes()), how many
 * nodes were built, dropped by changes and forgotten as `nodes.opened`,
 * `nodes.reset` and `nodes.forgotten`, how often the region grew as
 * `tree.rooted`, and how many tiles were run, how many it holds and
 * their shapes' size as `tiles.evaluated`, `tiles.held` and
 * `bytes.tiles`. A tile's error ends the program, or, where a Sink
 * hosts the renderer (the viewer), is printed once and leaves the tile
 * empty.
 */
Plugin geometryTreePlugin();

} // namespace kodanuki
