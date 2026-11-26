# Geometry

How rays find surfaces. The tree (patches, program cells, level of
detail, compression; reference_architecture.md) takes its exact faces from the
derivation below.

## Aims

- Nothing at render time iterates toward the surface. A ray descends
  a tree and, near the surface, solves the cell's functions exactly in
  closed form; a hit evaluates the SDF once (normal, primitive, seam
  share).
- Uniform: it works from the programs alone, with no hints from the
  scene file, and nothing is guessed. Every approximation is proven
  within the tolerance.
- A cell holds several functions, so edges and corners don't force
  subdivision.

## The pieces

1. **One program per object.** The CSG of an object compiles into one
   straight-line program. A leaf is inlined with its placement as
   steps, hard joins become `min`/`max`, and smooth joins one smooth
   minimum step. Every step carries the leaf it came from, for materials.
   A step is 24 bytes with 16-bit operands, so a program holds at most
   65,535 steps; the load refuses a longer one.
   The smooth minimum is not a crease: its value and gradient are
   continuous, so its Hessian bounds (the hull of both sides across the
   band edge) still prove a Taylor quadric.
2. **Four number types, one interpreter.** `sdf/program` evaluates a
   program as point (double), dual (value and gradient), interval, or
   hyperdual (value, gradient and Hessian).
3. **Cutting a program to a cell.** An interval evaluation over the
   cell resolves every crease (`min`, `max`, `abs`, comparison,
   `select`) whose sides are separated, and drops the dead steps.
   Children cut their parent's cut program, so programs shrink down the
   tree. A cell whose interval excludes 0 is empty or filled.
4. **Creases left are the faces.** The remaining creases make the
   program piecewise smooth. Every combination of crease sides is a
   smooth piece. A cell's faces are the distinct smooth functions whose
   sign can change in it: the pieces and the crease conditions `a - b`.
   The truth table is the program's decision over those signs. Faces
   the table doesn't depend on are dropped, counting only sign patterns
   a point of the cell can have: parallel exact planes order their signs
   along their normal, so stacked slabs leave the one face that shows. A clamp squared,
   `max(x, 0)²` (rounded boxes, lengths of clamped vectors), is C1 and
   so one positive-square step, not a crease; it is split like one only
   where a square root over it may reach 0 (sharp edges).
5. **A face is a quadric.** A piece affine in one square root,
   `a sqrt(S) + b` with `-b/a > 0` on the cell, has the sign of
   `S - (b/a)^2`; that is the face (exact for spheres, cylinders, cones,
   capsules and rounded corners), held to the tolerance times
   `(-b/a)/|a|`. A hyperdual evaluation at the cell centre
   gives value, gradient and Hessian: the 10 coefficients of a Taylor
   quadric, in the cell's unit coordinates. An interval evaluation of
   (piece minus quadric) over the cell proves the error within the
   tolerance, or the cell is split.
   A piece that is `a sqrt(P^2 + Z^2) + b` with `P = c sqrt(U^2 + V^2) + d`,
   for affine `U`, `V`, `Z` and constants with `R = -d/c > r = -b/(a |c|)
   > 0`, is a torus under some affine map, whatever the placement or
   stretch: it is kept as a ring face (the map, `R` and `r`), exact at
   any size, where its Taylor quadric is not proven. The walk solves
   its quartic `(|v|^2 + R^2 - r^2)^2 - 4 R^2 (v_x^2 + v_y^2)`.
6. **Leaf or split.** The tree branches 4 x 4 x 4 (a 64-tree). A cell
   is a leaf when every face is proven, it has at most 8 faces, and
   either every face is proven within 1% of the tolerance (exact fits
   stop whatever the cell's size) or a cost model says solving its k
   faces per ray is cheaper than descending into its children
   (estimated from their face counts one level down). At the floor (the
   depth limit or a cell of 4 tolerances) a cell of more than 8 faces
   first merges a face into another of the same look whose quadric is
   proven within the tolerance of it; what is left over 8 faces, or
   unproven, becomes an unproven floor leaf.
   A cell whose faces are crowded or unproven is fitted with a patch
   over its whole field, unless it is at least 16 tolerances wide or
   cuts more than 8 programs: a patch is checked only at its 35
   readings, and a feature as small as a window falls between them
   there, so such a cell splits. A surface leaf whose faces keep one
   sign each over the cell, proven in intervals, is empty or filled by
   its table instead.
   The tolerance is half a pixel's footprint, at least the least
   tolerance, 1e-7 of the first region (or `--least`), so the pixel
   and not the scene's size sets it near the eye: on a region of
   400 m the floor is 0.04 mm and holds only within about 1 m of the
   eye; the depth limit is the depth where a cell is 4 least
   tolerances wide. A cell wholly outside the frame (and 3 pixels
   around it) is held to 4 times that,
   as are the offsets of rays leaving it, except for sharp rays (The
   kept tree).
7. **Materials are edges.** Faces of different looks (material and
   coats) are never one face, so where two paints meet the table
   decides which face the ray crosses. Inside a smooth join of two
   looks the cell splits on `a - b`: the blend face is gathered twice,
   painted by each operand on its side.
8. **Render.** Descend the 64-tree, skipping empty children by their
   occupancy bits. In a surface leaf, solve each face's quadratic along
   the ray inside the cell, and walk the roots in order until the truth
   table turns inside (the signs before the first root are read in the
   widest gap between roots and unwound, so a root just before the
   cell's entry, within the slack, is not counted twice); of roots
   within the slack, the first face that turns it wins (a tie of
   coplanar faces goes to the union's left operand). The normal is the
   face quadric's gradient. A hit returns the leaf of its face and that
   leaf's material. Rays are not cut at `far`; the objects are (Reach).
   A ray from inside crosses roots within the slack of one another
   together. It leaves where they turn the table outside, and it also
   stops where they keep it inside but the solid just past them, read
   from the leaf's programs, is of another material than the one it
   travels in: where a glass rests on a floor, the ray meets the floor.

## The kept tree

`geometry/tree` (plugins/geometry/tree/lazy.h) builds a node, a cell
and its 64 slots (piece 6), when a ray first enters it, and keeps it
across frames.

- **Opening.** A packed node holds a state (`NodeState`: Unbuilt,
  Opening, Loose, Fine, Forwarded) and the frame a ray last entered it
  (`stamp`); its record (`NodeRecord`) holds its parent, slot, depth and
  the objects that may reach its cell. A walk enters a node through
  `enteredNode()`. The first ray claims an unbuilt node, cuts the whole
  programs of its listed objects whose box meets the cell, plans,
  passes and packs the node outside the lock, places it under the lock
  and publishes its state last; other rays wait. A node above the
  fourth level plans on the waiting workers too. `execution/packets`
  has a node opened for the whole packet before it descends.
- **Sharp rays.** A ray is sharp (`Ray::sharp`) while every bounce of
  its path was drawn within a half angle of 0.1 (`Path::widest`), as
  through a mirror or a lens: it shows what it meets as the camera
  does. A sharp ray builds every cell as one in the frame
  (`sharpTolerance()`); another keeps the looser tolerance outside the
  frame, and its node is Loose where that loosened a fit. A sharp ray
  that enters a loose node has a finer node built at a new index, which
  the loose one forwards to; between frames the finer node takes the
  loose one's place.
- **Storage.** The arrays are reserved once, together at most a
  quarter of the machine's memory, and never move, so rays read them
  while other nodes are placed; pages no node wrote are not resident.
  A dropped node's spans go to each array's free store (`SpanStore`),
  which later nodes write into, and large free spans are handed back to
  the system. Program lists and cuts are shared across the tree,
  `build/shared_coefficients` within one node.
- **Changes.** The tree names an object by a slot (`ObjectSlot`),
  matched across frames by the object's key (`objectKey()`: its
  program, arguments and leaves), so a scene that lists its objects in
  another order changes nothing. A changed object is followed down from
  the root where a node lists it or its box and cut reach the cell; a
  built node where it touches a leaf or an empty slot is dropped, to be
  built again when a ray enters it (`followScene()`). A node waiting for
  its tiles passes every changed object on to the nodes below it, so the
  nodes that ran tiles under it learn of their tiles' changes, but lists
  no tile object itself.
- **Each frame** (`prepareTree()`, the tree's `prepareFrame`): forwards
  are folded, the region grows where it must, the reach follows the
  eye, and the Detail looks through the frame's camera; where the
  camera moved or a shot begins, nodes built more than twice as loose
  as the view allows are dropped. Then the frame is counted on.
- **Forgetting.** Where the tree holds more than its memory target
  (`--memory` in megabytes, the config's `memory`; 0, the default, is a
  quarter of the machine's memory) or one of its arrays is three
  quarters full, the topmost built nodes no ray entered in the last 3
  frames turn unbuilt, farthest from the eye first, until neither
  holds; the tiles no standing node lists or ran go with them. What the
  tree holds counts its tiles: each held tile's shapes with its objects'
  keys, boxes and slots, and a record for every tile it knows, held or
  not.
- **Region.** Each object has a box (`ObjectBoxes`), put together from
  its primitives' interval boxes along its joins. The region is the
  hull of the object boxes, each cut to the reach's bounding cube (the
  cube of twice `far` about the camera's eye), made a cube and widened
  by 1%; a scene with tiles holds that whole cube. Where that hull leaves the
  region in a later frame (without `--region`), a new root is placed two levels up with the old
  root as one of its 64 branches, and the old root's cell is kept to
  the bit (`Growth::formerRoots`), so every cell keeps its size and its
  leaves. The least tolerance stays the first region's, and
  `sampler/glowing_surfaces` and `toward_mirrors`, which bound object
  fields over the region (`fieldBoxes()`), keep the first region's cell
  size. The region never shrinks.
- **Reach.** The reach is the sphere of radius `far` about the eye
  (`Growth::reach`, `moveReach()`). Each object's whole program the
  tree names is the object's program cut to it: max(f, |p - eye| -
  far), the sphere's steps appended and painted with the object's
  least leaf. A cell beyond the sphere is empty, a cell inside it
  resolves the cut away, so only the cells on its edge carry it. Until
  the first frame the reach cuts nothing. When the eye moves, the
  sphere's constants are rewritten in place, and from the root down a
  node whose cell lies inside both spheres or beyond both is kept;
  else its list takes its parent's objects whose box meets it, and a
  built node is dropped where a slot that is not a branch lies on
  either sphere's edge or between them and is a leaf, or is empty but
  a listed object reaches it now; else the walk goes on into its
  branches.
- **Tiles.** A node whose cell meets a `tiles` block's tiles
  (reference_language.md) within the block's bounds (`from`, `to`), and
  whose part inside the bounds' box is at most 4 steps wide along each
  axis, runs those tiles on its thread, and their objects take slots and
  join its list. A wider one waits: it lists no tile object, branches
  into every slot no object fills, and passes the objects that change
  on to the nodes below it. A node whose cell meets no tile within the
  bounds lists the scene's objects alone, as every node below it does,
  and splits only where they need it: the sky over a bounded block and
  the space beside it cost no more than over a scene without tiles. Only
  tiles whose box meets the reach are run, their objects cut to it; a
  node beyond the reach lists the scene's objects alone. When the reach
  moves, a node that ran its tiles and whose cell meets a tile the new
  reach meets but the old did not is dropped and waits again, to run
  it.
- **Held tiles.** A tile is held while a node lists one of its objects.
  While only nodes that ran it stand, it keeps no shapes, only what it
  was run from: its block's source, the top-level names it read (one
  list shared by the tiles that read the same names) and a key of their
  bindings; with the last of those nodes it is forgotten. A tile runs
  again when its block's source or a binding it read changes, or the
  eye it read (`eye(step)`) snapped to a step it read it at: tiles see
  the frame's eye (`HeldTiles::eye`), and a step a tile read counts as a
  binding keyed by the snapped eye. The eye is checked where the scene
  is followed (`keptGeometry()`, with the frame's eye first) and where
  the reach moves (`moveReach()`), which runs the stale tiles only where
  the eye moved to another cell at a step some tile read
  (`eyeMoved()`); a flown camera thus runs again the tiles that read
  the eye there and no other. Between frames, the stale tiles run a few
  thousand at a time, and each batch is followed down the tree and the
  tiles swept before the next, so the old and new shapes of every tile
  are never held at once.
- **Hits.** `build/faced_hits` keeps a face's leaf as the program of the
  cell's list it comes from (8 bits) and how far it stands from that
  object's least leaf (16 bits), so it follows the object wherever the
  scene lists it; a face whose leaf does not fit reads it from the field.
- **Counters.** `bytes.tree` (what the tree holds, free spans left out,
  tiles in), `nodes.opened`, `nodes.reset`, `nodes.forgotten`,
  `tree.rooted`, `tiles.evaluated`, `tiles.seconds` (the wall time of the
  tile runs, summed over the threads), `tiles.held` and `bytes.tiles`.
- `openEveryCell()` builds every node as a sharp ray would, for the
  tools and checks that read a whole tree.

## Seams for later

- Other fits behind the face seam (least squares, higher order).
- Coarser approximations (a face's plane part) for far cells.
- The cost model's weights.
