# The scene language

A scene is one `.sdf` file, written by hand. It is C-like: `//` and
`/** ... */` comments (any `/* ... */`, not nested), `;` ends a
statement, braces make a block. It runs once when it loads, and once
more for every frame of a clip (see Time). What it leaves behind is a flat list of shapes, each a
surface placed and painted, plus the camera, lights, sky and
settings, and any `tiles` blocks, which make shapes where rays come
(see Tiles). Space is z-up, lengths are metres, angles are radians.

Draft for milestone 1. "Open" marks what is still to be decided.

## Values

| Value | Written | Notes |
| --- | --- | --- |
| number | `1`, `0.5`, `1e-3`, `pi` | one kind of number |
| list | `[1, 2, 3]`, `[[0, 0], [0.2, 0.05]]` | one kind of element; a list of numbers is a vector |
| record | `{ at = [0, 0, 0], radius = 300 }` | named fields, each a value of any kind |
| colour | `#C41E3A` | a list of three numbers, sRGB turned linear; `[r, g, b]` is linear |
| material | `plastic(#FFFFFF, roughness = 0.7)` | see Materials |
| coat | `coat(wet(0.6), above(0.3))` | see Coats |
| network | `network(lines, spacing = 8)` | lines with an index by place, see Networks |
| pattern | `checker(4)` | a call of a surface kept as a value: a ramp's pattern or a mask, see Materials |
| word | `x`, `filmic` | an axis, a setting's word or a group's name |

Lists are vectors. `+ - * /` work element-wise and a number spreads
over a list. A list may be built from others by loops written inside
it, as in Python:

```
let doubled = [x * 2 for x in xs];
let grid    = [[x, y, 0] for x in 0..4, y in 0..3];   // nested, x outermost
let near    = [q for q in points if length(q) < 1];
```

The loops take what `for` takes, a range or a list; the `if`, which
may be left out, keeps the elements its condition holds for, and must
be known when the scene loads.

`append(list, value)` gives a new list with the value added at the
end, and `join(a, b, ...)` the lists one after another; `[]` is the
empty list to start from. The value and the lists' elements follow
the rules of a list's elements (`a list that mixes records of
different fields`), and other arguments are errors (`append takes a
list and a value`, `append takes 2 arguments`, `join takes lists`).
Both give a new list and leave their arguments as they were;
`let L = append(L, x);` and `let L = join(L, M);` grow `L` where it is
held, so a loop that builds a list runs in time linear in its length:

```
let lows = [];
for i in 0..count(heights) {
	let lows = append(lows, min(heights[i], 40));
}

let all = join(lows, [0], extra);
```

A surface may append and join its lists too, and their elements may be
worked out at every point (`append([r], p.x)`); a list grown inside an
`if` that is not known when the scene loads is an error (`lists of
different lengths`).

`a[i]` indexes, `count(a)` counts, and `.x .y .z .w` and swizzles such
as `.xz` or `.zyx` read the first four elements. A list that mixes
kinds (`[1, [2, 3]]`) is an error. There are no strings except the
path of an `import`.

A record holds named fields, each a value of any kind, a record too.
`r.radius` reads a field. A member of a record is always a field, so
`r.x` reads the field `x`; a member of a list is a swizzle. A field
is written once in a record (`the field 'a' is written twice`), and
reading one it lacks is an error (`the record has no field 'b'`). A
list of records holds records of the same fields, in any order
(`a list that mixes records of different fields`). Records take no
arithmetic and no comparison; they are values known when the scene
loads, read at the top level, in shapes, functions and tiles, and a
record of numbers may be passed to a surface (see Surfaces):

```
let CITIES = [
	{ at = [-4000, 2500, 0], radius = 900, style = 1 },
	{ at = [3000, -1200, 0], radius = 400, style = 2 },
];

function largest(cities) {
	let most = 0;
	for c in cities {
		let most = max(most, c.radius);
	}
	return most;
}

let radii = [c.radius for c in CITIES];
```

Booleans are numbers: comparisons `< <= > >= == !=` give 0 or 1, and
`&& || !` treat 0 as false and give 0 or 1. `&&` and `||` short-circuit
when their left side is known when the scene loads (see below): the
right side is not evaluated when the left decides (`&&` on 0, `||` on
anything else), so a guard may stand before what it guards:

```
let found = i >= 0 && L[i].x > 0;   // fine for i = -1
```

When the left side is not known then (inside a surface, it depends on
`p` or an argument), both sides are worked out at every point.

A value is *known when the scene loads* when the file works it out as
it runs: at the top level, in a shape, a function or a tile body (a
tile runs later, when rays first come, but works its values out the
same way). Inside a surface, `p`, `n`, the arguments that stay
arguments (numbers, and lists and records of them; see Surfaces) and
what is computed from them are not known then: they are worked out at
every point.

## Statements

```
import "standard.sdf";           // definitions of another file, by path from this one
let size = 0.5;             // a binding; `let size = 0.6;` changes it
for i in 0..4 { ... }       // 0, 1, 2, 3
for x in 0..3, y in 0..3 { ... }   // nested, x outermost
for square in board { ... } // each element of a list
if a < b { ... }            // no else
```

There is no `else` (`there is no else`). Where one would stand, end a
shape or a tile body early with `return;` (see Builders), a function
or a surface with `return value;` (see Functions), pick a value
with `select(c, a, b)`, call a function that returns the value, or
index a list by a number.

Every binding and every change of one is written with `let`; there
is no bare `x = ...`. A `let` of a name already bound in this block or
one around it changes that binding, so a loop can carry a value out:
`let d = min(d, e);`. A new name lives until its block ends.

A change keeps the binding's kind (number, list, record, material,
coat, pattern, word or network; a list of numbers and a list of lists
are both lists): `door holds a material and cannot be bound to a
number`. A function or a surface that binds a top-level name makes a
name of its own, so that is no change. The names of the built-ins
(`sqrt`, `matte`, `coat`, `pi`, ...) are taken: a `function`, `surface`
or `shape` of that name, or a top-level `let` of it, is an error
(`sqrt is a built-in and cannot be defined`, `... cannot be bound`).

## Surfaces: `surface`

```
surface sphere(r) { return length(p) - r; }
```

A `surface` is a function of a point. Inside it `p` is the point (a
vector of three), and when it is used as a pattern `n` is the surface
normal. It must `return` a number (see Functions for an early return).
Used as a shape, the number is a
distance. Used as a pattern, it is a value in 0..1 for a ramp. Both
kinds are the same thing.

A shape's distance must never be larger than the true distance
(standard SDFs are fine). The builder relies on this to drop a shape
from cells it cannot reach. A displaced shape must be scaled down by
hand to keep to it.

The body may use `let`, `for` and `if`; a `let` of a top-level name
makes a name of the surface's own, as in a function. Every loop bound
must be known when the scene loads: a literal, a top-level number, or
`count` of a list argument. Loops are unrolled, and `if` becomes a
select. A `surface` is compiled once per distinct set of arguments,
where a number, and a list whose elements are all numbers or again such lists, counts only
by its shape: those stay arguments, so a thousand spheres share one
program, and so do tiles that pass their own 4 × 4 list of heights
(`[[...], [...], [...], [...]]`). A record whose fields are all
numbers, or lists of numbers or such records, counts by its field
names and their shapes alone, so `disc({ at = [0, 0, 0], radius = 1 })`
and `disc({ at = [9, 0, 0], radius = 2 })` share one program; a record
holding anything else is refused (`a record passed to a surface holds
only numbers and lists of numbers`). Any other argument (a material, a
pattern, a word) is built into the program, one per distinct value; a
coat or a network is refused, also inside a list (`a surface takes no
coat`, `a surface takes no network`). A top-level binding the
surface reads, itself or through a function it calls, and in a tile
`tile`, `tile_origin` and `tile_step`, is built in too: the surface is
compiled once more for each distinct value such a binding holds where
it is called, so `let k = 1; s(1); let k = 2; s(1);` makes two
programs, each with its `k`.

Built-ins: `sqrt abs floor sin cos atan2 pow min max clamp mix
length dot cross normalize select all any sign`, and `pi`; `linear` and
`smooth` read a list of stops (see Time); `print` writes values (see
Errors); `hash`, `noise` and `fbm` take numbers known when the scene
loads (see Hash and noise), and so do `network`, `nearest` and
`within` (see Networks).

`select(c, a, b)` is `a` where `c` is not 0 and `b` where it is 0.
With `c` known when the scene loads it picks any two values: two
materials (`paint(select(width > 5, asphalt, gravel))`), coats, lists
of other lengths, records. With `c` depending on `p` or on a number
argument both sides are numbers or lists of numbers, picked element
by element; anything else is an error (`select picks a material only
by a condition known when the scene loads`).

## Functions: `function`

```
function next(seed) {
	let seed = seed * 16807;
	return seed - floor(seed / 2147483647) * 2147483647;
}
```

A `function` computes a value: a number, a list, a list of lists or
any other value. It sees its parameters and the bindings of the top
level, `time` among them, and in a tile the tile's (see Tiles), but not
its caller's. It changes nothing
outside it: a `let` of a top-level name makes a name of the
function's own, which lives until its block ends, and a value comes
out only as what it `return`s. A `return` may stand inside an `if`
or a `for`: the first one that runs ends the function with its value.
Every way through must return (`next never returns`), and nothing
may follow a return in its own block (`nothing may follow a return`).
It emits no shapes and is no
statement; it is called inside an expression: `let seed =
next(seed);`. Called from a surface, its body is inlined there, so it may work on `p`, and a
top-level binding it reads is built into the program, as the
surface's own reads are (see Surfaces).
In a tile, a top-level binding it reads counts as read by the tile
(see Tiles). The body may use `let`, `for` and `if`, as a surface's
may.

```
function slot(roofs, kind) {
	for i in 0..count(roofs) {
		if roofs[i] == kind {
			return i;
		}
	}
	return -1;
}
```

At load time any condition may end it so. Compiled into a surface,
the same holds for an `if` known when the scene loads (a top-level
number, a loop's index): the branch is decided then. A return inside
an `if` that depends on `p` or on a number argument cannot end the
program early and is an error (`a return inside an if not known when
the scene loads; use select`); pick the value with `select` instead.

```
let ROUGHNESS = 3;

function heights(seed) {
	return [[ROUGHNESS * hash(seed, i, j) for j in 0..4] for i in 0..4];
}
```

## Hash and noise

```
tiles(step = [64, 64, 400], from = [-100, -100, 0], to = [99, 99, 0]) {
	let height = 40 * fbm(tile.xy / 8, 5);
	if hash(tile, 3) < 0.3 {
		at([32, 32, height]) tree(hash(tile, 4));
	}
}
```

`hash(a, ...)` turns numbers and vectors into a number in [0, 1). The
same arguments give the same number at every load and on every
machine, and arguments that differ at all give unrelated numbers. A
vector counts as its numbers in order, so `hash([1, 2])` is
`hash(1, 2)`, and `-0` is `0`. It mixes the 64 bits of each number in
turn (the splitmix64 finaliser).

`noise(q)` is value noise for a vector of 2 or 3 numbers, in cells one
unit across: a random value in [-1, 1] at every whole point and a smooth
blend between them (the quintic fade), so it is continuous, its slope
too, and lies in [-1, 1]. Divide the point for larger cells:
`noise(q.xy / 50)` has cells 50 metres across. `fbm(q, octaves)` adds
`octaves` layers of it, each of twice the frequency and half the height
of the one before and with values of its own, and divides by the sum of
the heights, so it lies in [-1, 1] too. `octaves` is a whole number of 1
or more.

All three run when the scene loads: at the top level, in shapes,
functions and tiles. In a surface they take only numbers known then; a
number that depends on `p` or on a surface's argument is an error,
`noise is known only when the scene loads`. Open: noise in surfaces.

## Networks

```
let asphalt = matte(#3A3A3C);
let wheat   = matte(#C8A951);

let ROADS = network([
	{ points = [[-900, -40], [-300, 10], [200, 180], [900, 260]], width = 8 },
	{ points = [[200, 180], [260, 700]], width = 5 },
], spacing = 8);

tiles(step = [64, 64, 400], from = [-16, -16, 0], to = [15, 15, 0]) {
	let corner = tile_origin.xy;
	for s in within(ROADS, corner, corner + [64, 64]) {
		let a = s.a - corner;
		let b = s.b - corner;
		paint(asphalt) clip {
			capsule([a.x, a.y, 0], [b.x, b.y, 0], s.fields.width / 2);
			at([32, 32, 0]) box([64, 64, 20], 0);
		}
	}
	let road = nearest(ROADS, corner + [32, 32], 300);
	if !road.found || road.distance < 30 {
		return;                                  // wild land, or the verge
	}
	let strip = floor(road.along / 40);          // fields 40 m wide along the road
	if hash(road.line, strip) < 0.6 {
		paint(wheat) at([32, 32, 0.2]) box([60, 60, 0.4], 0);
	}
}
```

`network(lines, spacing = 8)` makes a network of lines, such as roads
or rivers, drawn by hand as waypoints. Each line is a list of waypoints
`[x, y]` or `[x, y, z]`, or a record whose field `points` holds them,
with any other fields beside (`width`, `kind`). The lines of a network
all take one form, and its waypoints all hold 2 numbers or all 3. A
line follows the curve `smooth` draws through its waypoints (see Time),
its time the distance along the straight pieces between them, and is
cut into straight segments about `spacing` metres long (default 8): the
stretch between two waypoints into round(its length / spacing) pieces,
at least one, so the line passes through every waypoint. A line whose
last waypoint is its first closes into a loop, with a corner there, as
a `smooth` curve takes its slope at an end from the one stop beside it.
To close a loop without a corner, close it on a straight stretch: let
its first waypoint lie on the straight line between the last but one
and the second, as in `[[50, 0], [100, 0], [100, 100], [0, 100], [0,
0], [50, 0]]`; the slopes at its two ends then agree.

The network is built once, where `network` runs, with an index of its
segments by place. It is a value: a `let` binds it, and a list or a
record may hold it, a function or a shape take it, and a tile read it
without a copy; a tile that reads it notes it as it notes any binding.
It takes no arithmetic, and a surface takes no network (`a surface
takes no network`). All three built-ins run when the scene loads and,
like `hash`, take only numbers known then (`nearest is known only when
the scene loads`).

`nearest(net, q)` finds the point of the network nearest to `q`, a
vector of 2 or 3 numbers, in x and y (z is ignored), and returns a
record:

| Field | Holds |
| --- | --- |
| `distance` | how far it lies from `q` in x and y |
| `along` | how far it lies from its line's start, along the segments (with their heights) |
| `side` | 1 where `q` lies left of the line's direction, -1 right, 0 on it |
| `line` | the index of its line in `lines` |
| `at` | the point, as many numbers as a waypoint, its height taken along the segment |
| `direction` | the unit direction of its segment, as many numbers as a waypoint |
| `fields` | its line's record without `points`; an empty record for a line of plain waypoints |
| `found` | 1 |

The line's fields stay in `fields`, so `r.fields.width`, and never
clash with the others. Of two points equally near, the one of the
earlier line, or earlier along it, wins. `nearest(net, q, reach)`
looks no farther than `reach`, and costs only the index's cells within
it: where nothing lies within reach, `found` is 0, `distance` is
`reach`, `line` is -1, `fields` an empty record and every other number
0. Asking whether a point is within 20 m of a road is
`nearest(ROADS, q, 20).found`.

`within(net, lower, upper)` returns the segments that meet the box from
corner `lower` to corner `upper` in x and y (z is ignored), edges
included, each once, in the order of their lines and along each: a list
of records `{ a, b, line, along, fields }`, `a` and `b` the segment's
ends, as many numbers as a waypoint, and `along` how far `a` lies from
its line's start. A segment is the same record in every tile that meets
it, so a tile draws the pieces that reach into it, clipped to its box.

The errors: `network takes a list of lines and a spacing`, `a line has 2
points or more`, `a line's record holds its points in 'points'`, `a
line's points are [x, y] or [x, y, z]`, `a network's points all have 2
numbers or all 3`, `a line's points in a row differ`, `a network's
spacing is above 0`, `nearest takes a network, a point of 2 or 3
numbers and a reach` and `within takes a network and two corners of 2
or 3 numbers`. Lines of both forms in one list are a list that mixes
records and other kinds.

## Builders: `shape`

```
shape pawn() {
	merge(0.02) {
		revolve(PAWN);
		at([0, 0, 0.45]) sphere(0.095);
	}
}
```

A `shape` runs when the scene loads, or when the tile that calls it
runs, and emits shapes. It may call surfaces
(emitting one shape), functions and other shapes, loop and branch. It
returns no value, and it may emit nothing. A bare `return;` ends it
there, from inside any `if`, `for` or `with`: the shapes it emitted
before stay, the rest of it does not run, and its caller goes on:

```
shape house(seed) {
	box([4, 4, 3], 0);
	if hash(seed) < 0.3 {
		return;                 // no chimney
	}
	at([1, 1, 3]) box([0.3, 0.3, 1], 0);
}
```

A `return;` may not stand inside a join or a repeat (`return may not
stand inside a join or a repeat`), whose shapes are collected first;
a shape called there may return. A value return in a shape is an error
(`a shape or a tile body returns no value`), and so is a bare one in a
surface or a function (`a surface or a function must return a value`)
or a return at the top level of a file, outside a shape and a tile
body (`return belongs in a surface, a function, a shape or a tile
body`). A shape sees the bindings at the top level of the file, and
in a tile the tile's (see Tiles), but not its caller's, and a `let` of
a top-level name changes it, so a shape can carry a value out
(`scenes/city.sdf` draws its random numbers so):

```
let seed = 49534;
shape draw() {
	let seed = seed * 16807;
	let seed = seed - floor(seed / 2147483647) * 2147483647;
}
```

## Emitting a shape

A statement that calls a `surface` or a `shape` emits it. Modifiers before
it place and paint it. They read left to right, the outermost first:

```
paint(gold) at([0, 0, 1]) turn(z, pi / 4) box([1, 1, 1], 0.05);
```

| Modifier | Does |
| --- | --- |
| `at(v)` | moves by v |
| `turn(axis, angle)` | turns about `x`, `y`, `z` or a vector, right-handed |
| `scale(s)` | a number or a vector; a negative element mirrors |
| `paint(m)` | the material of everything under it that has none closer |
| `coat(effect, mask)` | a change of its look where the mask says (see Coats) |
| `rule(camera = 0, shadows = 0, caustics = 0)` | rules the paths that meet it obey (see Rules) |
| `group(name)` | puts it in a named group, which a light may reach alone |

For a block, the chain starts with `with`:

```
with paint(black) at([0, 0, 2]) {
	box([0.5, 0.5, 0.5], 0.05);
	sphere(0.1);
}
```

Statements side by side are merged. Inside a `shape` that is a hard
merge; at the top level it means a separate object. The difference
matters only for smooth joins.

## Joins

```
merge { ... }        merge(0.02) { ... }   // smooth with radius 0.02
cut { base; a; b; }  cut(0.005) { ... }    // base minus the rest
clip { a; b; }       clip(0.01) { ... }    // what all of them share
```

A join is a statement, so modifiers prefix it without `with`:
`at([0, 0, 0.56]) merge { ... }`.

A hard union at the top of an object whose program has 512 steps or
more is cut into objects of its own, one per operand, and an operand
that is again such a union is cut the same way. The field is the same
either way; a cell of the tree then holds only the parts near it.
Smooth joins, cuts and clips keep their operands in one object.

An object's program holds at most 65,535 steps; a longer one is an
error, `name has more than 65535 steps`.

## Repeats

```
repeat(count = [3, 1, 4], step = [2.6, 0, 3.3]) {
	at([0, -0.12, 0]) paint(pane) box([1.45, 0.24, 2.1], 0);
	paint(interior) box([3, 3.2, 2.4], 0);
}
```

A `repeat` runs its block once and copies the shapes it emits
`count.x × count.y × count.z` times; copy (i, j, k) is moved by
`i · step.x` along the block's x axis, `j · step.y` along y and
`k · step.z` along z, in the block's own space after the modifiers
before it. Anything the block draws or changes, such as a random number
or a binding, happens once, so every copy is alike; a `for` loop runs
its body once per pass instead. A count is a whole number of one or
more, and a number spreads over all three. Like a join, a `repeat`
takes modifiers without `with`, and its copies stand side by side as
statements do.

## Tiles

```
tiles(step = [64, 64, 400], from = [-100, -100, 0], to = [99, 99, 0]) {
	at([32, 32, 0]) house(tile.x * 7919 + tile.y);
}
```

A `tiles` block makes shapes where rays come, one tile at a time, for
a world larger than a load could hold. `step` cuts space into boxes, a
number spreading over all three and each above 0: tile `[i, j, k]`
holds the points from `[i, j, k] * step` up to the next. The block's
body runs once for each tile the renderer needs, with three more
bindings: `tile`, the list `[i, j, k]`; `tile_origin`, the corner of the
tile in the world, `tile * step`; and `tile_step`, the step. Every shape
it emits is moved by `tile_origin`. It starts from the bindings of the top level where the
block stands, none made after it, and its `let`s are its own, even of
a top-level name: tiles run in any order, each alone.

- A block stands at the top level of a file, not in a loop, a shape or
  another block, and takes `step`, and `from` and `to` (below).
- `from` and `to` bound the block: the tiles from index `from` up to
  index `to`, both included, run, and no other. They come together
  (`tiles take from and to together`), each a vector of 3 whole numbers
  (`a tile's from and to are 3 whole numbers each`), `from` at most `to`
  along every axis (`a tile's from is at most its to along x, y and
  z`). Without them every index runs. Bound a finite world: outside the
  bounds the renderer spends nothing on tiles, while without them every
  cell a ray crosses, the sky's too, is cut down to 4 steps wide to run
  the tiles it meets, even empty ones.
- Its body, and the shapes it calls, may not hold settings or imports,
  define functions, surfaces or shapes, hold `tiles`, make materials,
  say rules or put shapes in groups (`tiles may not ...`); it paints
  with the materials made before it. It applies coats bound before it
  (see Coats) and makes none (`tiles may only use coats made before
  them`).
- A tile's shapes are trusted to stay inside its box and are not
  checked; one that reaches out leaves wrong cells along the edge (steps,
  holes), so a shape that crosses a tile's edge is clipped to the tile
  and drawn by every tile it touches.
- Tiles run on the render threads when a ray first enters a cell of
  the tree that meets them, and are run and held only where their box
  meets the reach (the sphere of radius `far` about the camera's eye),
  which follows the camera. A tile runs again when the scene's text or a top-level
  binding it read, `time` among them, changes, or the eye it read
  (`eye`, below) moved to another cell, or a band it read (`eye_band`,
  below) changed; a binding read by a
  function or a shape it calls counts.
- A step is at least 16 least tolerances (1e-7 of the first region,
  or `--least`) along every axis, else
  `a tile's step is at least 16 times the least tolerance, ... m`.
- The shapes and functions the body calls, however deep, read `tile`,
  `tile_origin` and `tile_step` too. A called shape works in the tile's
  own space, as the body does: `at([32, 32, 0])` is 32 m into the tile,
  wherever the tile lies. `tile_origin` is for what is computed in the
  world, the same across tiles: noise, roads, a terrain lookup.

  ```
  shape lawn() {
  	let middle = tile_origin.xy + [32, 32];       // in the world
  	at([32, 32, 40 * fbm(middle / 500, 4)]) box([30, 30, 0.1], 0);
  }
  ```

  Reading them is no read of the top level. Outside a tile body and
  what it calls, reading one is an error (`tile is known only inside
  tiles`), and so is a top-level binding of one (`tile belongs to the
  tiles and may not be bound at the top level`); a local of the same
  name hides it, as any local does.
- A bare `return;` ends the tile body there, as it ends a shape (see
  Builders): the shapes emitted before it stay. A `return;` in a shape
  the body calls ends only that shape.
- `eye(step)` is the camera's eye snapped to a grid of `step` metres:
  the middle of the grid's cell that holds the eye, `(floor(e / step) +
  0.5) * step` along each axis, so `eye(100)` of an eye at `[130, -30,
  7]` is `[150, -50, 50]`. `step` is a number above 0 (`eye's step is a
  number above 0`). In a tile, and the shapes and functions it calls,
  the eye is that of the frame being drawn: the clip's camera, or the
  camera flown in the viewer. A tile notes each step it read the eye at
  and runs again only where the eye snapped to one of those steps
  changes, as on a changed binding: a tile that reads `eye(128)` runs
  again when the camera crosses into another 128 m cell, not while it
  moves inside one, and a tile that reads no eye never runs again for
  a moving camera. Choose the detail by the distance to the snapped
  eye, so a tile's detail changes only as the eye crosses into another
  cell, two bands here:

  ```
  shape house(seed) {
  	let middle = tile_origin + [32, 32, 0];
  	box([6, 6, 4], 0.1);
  	if length(middle - eye(1024)) > 3000 {
  		return;
  	}
  	at([0, 0, 4]) roof(seed);
  	if length(middle - eye(128)) > 600 {
  		return;
  	}
  	windows(seed);
  }
  ```

  A tile beyond 3 km of the eye snapped to 1024 m reads only that step;
  a nearer one reads 128 m too.
- `eye_band(point, radii, step)` is the band a point lies in about the
  eye snapped to `step`: how many of the rising `radii` its distance to
  `eye(step)` reaches, in x and y for a point of 2 numbers and in x, y
  and z for 3, so 0 inside the first radius and `count(radii)` beyond
  the last. A tile notes the band it read, not the eye's cell, and runs
  again only where the band changes: a tile that reads
  `eye_band(tile_origin.xy + [32, 32], [300, 1000, 3600], 128)` keeps its
  detail while the camera flies on, until its distance crosses one of
  the radii. The point is noted in the tile's own space, so tiles that
  read the band of the same place in them share one note; give the
  middle of the tile, not a height worked out per tile. Like `eye` it
  reads the frame's camera, at the top level the file's
  (`eye_band reads the camera, and no camera block comes before it`),
  and is an error in a surface (`eye_band is not known in a surface`);
  a point of other than 2 or 3 numbers, radii that do not rise or a step
  not above 0 are an error (`eye_band takes a point of 2 or 3 numbers,
  a list of rising radii and a step above 0`). At the top level, and in the shapes and
  functions it calls, `eye` reads the file's camera as the camera block
  before it set it, the clip's camera at the frame's time (`eye reads
  the camera, and no camera block comes before it`); a value bound
  there is a binding like any other, and does not follow the camera the
  viewer flies. In a surface it is an error (`eye is not known in a
  surface`): pass the snapped eye as an argument.
- An error in a tile ends `kodanuki` with exit 1; the viewer prints it
  once and leaves the tile empty.
- `sampler/glowing_surfaces`, `sampler/toward_mirrors` and
  `geometry/sphere_trace` see only the scene's own objects: a glowing
  tile shape is met, never aimed at. The tree's region grows with the
  camera and never shrinks.

## Materials

```
let red   = plastic(#C41E3A, roughness = 0.7);
let steel = metal([0.9, 0.9, 0.9], roughness = 0.25);
let floor = matte(ramp(checker(4), #5C4636, #C9B89A));
let lens  = glass(refraction = 1.5, absorb = [0.1, 0.3, 0.2], roughness = 0);
let neon  = matte(#000000, glow = #FF2080, strength = 8);
```

The kinds are `matte(albedo)`, `plastic(albedo, roughness)`,
`metal(albedo, roughness)` and `glass(refraction, absorb, roughness)`. Every kind
takes `glow` and `strength`, which makes the surface a light. A glass of
roughness 0 is a true mirror and an exact passage (`--use
smooth_glass=lobe` makes it the narrowest GGX lobe instead, for
comparisons).

A glass also takes `dispersion`, Cauchy's B in square micrometres: its
index at a wavelength is `refraction + dispersion / λ²`, λ in
micrometres (crown glass about 0.004, dense flint about 0.01). A path
bent through a dispersing surface keeps only one of its wavelengths
from there on. Only a glass takes `dispersion`.

A glass's body may also turn light aside: `scatter` is how much of a
metre of travel it turns aside (per metre, at every wavelength alike,
default 0), and `lean` how forward it turns it, from -1 (back) through
0 (evenly every way, the default) to 1 (onward), Henyey-Greenstein's
g. Milk, jade or smoke are such bodies. Only a glass takes `scatter`
and `lean`. A glass of `refraction = 1` without dispersion or film,
of any roughness, bends nothing: its surface is only the boundary of
its body, as of a fog, which paths and the lines to the lights cross
as if it were not there, and a path aims at the lights from where it
turns aside inside it.

A metal, a plastic or a glass takes `film = [thickness, index]`: a
layer `thickness` nanometres deep of that index, thin enough that light
off its two faces interferes, as on a soap bubble or an oil slick. On a
metal it lies over the facets, on a plastic it is the coat, and on a
glass it is the boundary (a path through it keeps one wavelength).

Every kind also takes three switches, each 1 (the default) or 0, which
change how much noise a frame keeps and never what it shows: `guided`
(the highlight guide is built on the surface), `relays` (paths from the
lights are drawn toward it, where it is sharp) and `aimed` (paths aim
at its glow rather than only meeting it).

An albedo is a colour or a ramp: `ramp(pattern, c0, c1, ...)` spreads
the colours evenly over 0..1. The pattern is a call of a surface,
`checker(4)`, read in the painted shape's own space. `world(checker(4))`
reads it in world space instead. A pattern may read the normal `n`.

## Coats

A coat changes how a surface looks where a mask says, and nothing
about where the surface is. It is a
modifier, `coat(effect, mask)`, and the effect and the mask are
written in it:

```
coat(wet(0.8), below(0.3, soft = 0.1)) paint(stone) box([2, 2, 2], 0);
coat(gold, facing([0, 0, 1], level = 0.7)) paint(rock) sphere(1);
```

The mask weighs each point of the surface from 0 to 1:

| Mask | Worn |
| --- | --- |
| `above(height, soft = 0, axis = [0, 0, 1])` | past a height along a world axis |
| `below(height, soft = 0, axis = [0, 0, 1])` | under it |
| `facing(axis, level = 0.5, soft = 0.2)` | where the normal's cosine to the axis passes `level` (axis default `[0, 0, 1]`) |
| `inside(f, soft = 0)` | where the surface `f`, read at the world point, is below 0 |
| `along(f, width, soft = 0)` | in a band `width` wide about the surface of `f` |
| a surface call | its value clamped to 0..1, in the painted shape's space (or `world(...)`) |

`soft` is how far the edge takes to fade, half of it either side, by a
smooth step; 0 is a hard edge. Every mask but a surface call also
takes `ragged` and `grain` (defaults 0 and 1): value noise of cells
`grain` metres across moves the edge by up to `ragged`, in the units
the mask's reach is measured in, for a torn or worn edge. The effect is
taken as far as the weight:

| Effect | Does |
| --- | --- |
| a material | mixes its albedo and glow in, and becomes it past a half |
| `tint(colour)` | multiplies the albedo |
| `shade(amount)` | darkens toward black (below 0, to -1) or lightens toward white (to 1) |
| `rough(amount)`, `polish(amount)` | move the roughness toward 1 or toward 0 |
| `wet(amount)` | halves the albedo and takes the roughness down by four fifths |
| `glow(colour, strength = 1)` | adds light |
| `bump(relief, amount, span = 1, along = [1, 0, 0])` | tilts the shading normal along the slope of a relief `amount` high |

Several coats are laid on innermost first: in
`coat(a, ...) coat(b, ...) sphere(1)` the sphere wears `b`, then `a`.
A roughness a coat changes is drawn by one of 17 copies of the
material at roughness 0, 1/16, ... 1, the two either side of it picked
by a hash of the point. This is the plugin `feature/coats`, Biased in
part (its colours, tints and glows are exact, its laddered roughness is
not); built off (reference_architecture.md), every material keeps its
own roughness.

A bump's relief is `grain` (two octaves of value noise), `pits`
(hollows where the noise is high), `streak` (noise drawn out along the
axis `along`, as on brushed metal), each with cells `span` metres across
in world space, or a surface call read as a height, in the painted
shape's space (or `world(...)`). The surface itself does not move; only
the normal it is shaded with tilts, and it never tilts away from the
ray. A bump may leave its mask out and is then worn everywhere:

```
coat(bump(grain, 0.02, span = 0.05)) paint(plaster) box([2, 2, 2], 0);
coat(bump(streak, 0.01, span = 0.02, along = [1, 0, 0])) paint(steel) cylinder(1, 0.3);
coat(bump(pits, 0.03, span = 0.1), above(0.5, soft = 0.1)) paint(stone) sphere(1);
coat(rust, facing([0, 0, 1], level = 0.3, soft = 0.1, ragged = 0.25, grain = 0.12)) paint(iron) sphere(1);
```

A coat is also a value: `coat(effect, mask)`, or a bump alone, written
as an expression makes one where it runs, which a `let` binds, and
`coat(c)` with one argument that is a coat applies it. One coat applied
to many shapes is made once. Bound coats stack with written ones,
innermost first, and a coat may stand in a list or a record and be
passed to a function or a shape, but not to a surface:

```
let wet_road = coat(wet(0.6), facing([0, 0, 1], level = 0.9));
let worn     = coat(bump(grain, 0.02, span = 0.05));

coat(wet_road) coat(worn) paint(asphalt) box([8, 2, 0.1], 0);
```

`coat` with one argument takes a bump or a coat, told apart by its kind
(`coat takes an effect and a mask, a bump alone, or a coat`). Tiles
apply only coats made before the block, as they paint only with
materials made before it: a tile body, or a shape it calls, that makes
a coat is an error (`tiles may only use coats made before them`):

```
tiles(step = [64, 64, 400]) {
	coat(wet_road) paint(asphalt) at([32, 32, 0]) box([30, 4, 0.1], 0);
}
```

## Rules

A shape may say rules about the paths that meet it, each a switch of
`rule` that is 1 (the default) or 0. Inner rules override outer ones.
Rules change what the physics would draw: they are the Biased plugin
`feature/path_rules`, and built off (reference_architecture.md) they are
ignored.

```
rule(camera = 0) paint(pane) box([2, 0.02, 1.5], 0);   // the camera sees through it
rule(shadows = 0) at([0, 0, 3]) sphere(0.2);           // it casts no shadow
rule(caustics = 0) paint(gold) sphere(1);              // no caustic through it or onto it
group(stage) paint(marble) box([4, 4, 0.2], 0);        // a shape of the group stage
```

- `camera = 0`: the camera's rays and the lines to the lens pass through
  the shape, as if it were not there; everything else sees it.
- `shadows = 0`: lines to the lights pass through the shape, and a ray
  that scatters toward it looks past it for the lights before it stands
  on it, so it casts no shadow, direct or indirect. On a body that
  turns light aside (a fog), it is the body's shadow on what lies
  outside it that goes: a line to a light, or a look past it, from
  outside crosses the body undimmed, while light reaching a point
  inside it is still dimmed by the body as far as its boundary, and
  no farther.
- `caustics = 0`: no caustic is focused by the shape or lands on it: a
  path that has passed a dull surface and then only sharp ones may not
  end on a light where one of those surfaces, or the dull one, says so.

A light may say `reaches = [names]`, the groups whose shapes it lights
alone (the empty list, the default, lights every shape), and
`caustics = 0`, so that no caustic ends on it. A group name is a word,
and a scene names at most 64 groups.

## Time

Every file starts with one binding at its top level, `time`: a number
in seconds. It is 0 for a still, the frame's time for a frame of a
clip, and the number given with `--time`. Anything may read it: a
place, a turn, a size, a material, a light, the camera, a loop bound.
The file runs again for every frame, so what it leaves behind may
differ from frame to frame in any way.

```
at([0, 0, 1 + abs(sin(pi * time))]) sphere(0.3);
```

`time` cannot be changed: `let time = ...` is an error (`time belongs
to the clip and cannot be changed`). The `render`
and `clip` blocks must come out the same at every time.

Two built-ins read a value off a list of stops:

```
let eyes = [
	[0, -2.3, -4.9, 1.5],
	[4.8, 0.4, -4.2, 1.3],
	[8.8, 0.8, -3.6, 0.6],
];

camera {
	eye    = smooth(eyes, time);
	target = [0, 0, 0.95];
}
```

A stop is `[t, values...]`: where its time is and what holds there.
The times rise. `linear(stops, t)` joins the stops by straight lines;
`smooth(stops, t)` by a curve through every stop that turns no
corners (Catmull-Rom, its slope at a stop taken from the stops either
side, at an end from the one stop beside it). Both give a number where
a stop holds one value and a vector where it holds more. Before the
first stop the first holds, past the last the last.

Two stops with the same time are a jump: before that time the first
holds, from it on the second. A `smooth` curve ends at a jump and a
new one starts.

The second argument is any number, not only `time`: `linear(sway,
beat)` reads a dance by the beat. In a surface it may depend on `p`
or a number argument, `smooth(BANKS, p.x)`; the stops must still be
known when the scene loads. That compiles to one clamped share per
pair of stops (a step for a jump) and their sum, so a long list makes
a long program.

## Settings

Blocks of fields, one each, at the top level:

```
render { width = 1920; height = 1200; tolerance = 2; seed = 1; }
camera { eye = [3, 3, 4]; target = [0, 0, 2]; up = [0, 0, 1]; fov = 0.8;
         aperture = 0; focus = 0; }
grade  { exposure = 1; curve = filmic; }        // filmic | clamped
clip   { duration = 12.8; rate = 24; cuts = [4.8, 8.8]; }
sky gradient { zenith = [...]; horizon = [...]; ground = [...]; }
sky physical { haze = 3; ground = [...]; }      // lit by the sun light
sky painted { day = [...]; dusk = [...]; night = [...]; }
clouds { height = 1500; thickness = 1200; cover = 0.45; density = 0.03; span = 3000; }
stars  { count = 3000; strength = 2; angle = 0.003; }
light directional { direction = [0.4, -0.5, 0.7]; power = [2.4, 2.3, 2.1]; }
light point  { at = [...]; power = [...]; }
light sphere { at = [-2.2, -1.6, 3]; radius = 0.3; power = [18, 17, 15]; }
light quad   { at = [...]; along = [...]; across = [...]; power = [...]; }
light sun    { direction = [...]; angle = 0.0093; power = [...]; }
light moon   { direction = [...]; angle = 0.0093; power = [...]; }
```

Every light also takes `reaches` and `caustics` (see Rules).

`render` also takes `far`, the radius of the reach (default 1000), `samples`, the most samples a pixel draws, `surfaces`, the most
surfaces a path stands on (default 10, at least 1; it scatters from one
fewer), and `collisions`, the most times a path turns aside inside
scattering bodies (default 256, at least 1). Collisions are counted
apart from surfaces: a path ends at its last surface or at its last
collision, whichever comes first, so a dense fog or milk needs no
deeper surface count.
`tolerance` is the noise a pixel may keep: a pixel draws samples until
1.96 standard errors of its mean span no more than twice `tolerance`
steps of a byte after the grade, on its worst channel. `batch` is how
many samples a batch holds: every pixel first draws a trial
of two batches, whose batch means give its noise, and its count is set
from what the trials of the eight pixels around it want, in whole
batches, at most `samples`. Where `samples` or `batch` is not given,
the tolerance sets it: one effort e = log(62.5 / tolerance) / log(125),
from 0 up to 2^0.66, gives batches of round(1 + 7e)^2 samples, rounded
up to a multiple of 8, and at most 32 x 512^e samples a pixel, so the
default tolerance of 2 asks for batches of 40 and at most 2732 samples
(a tolerance of 0 or less: 64 and 4096).
`split` is how many samples of a pixel share a first vertex: a group's
first sample traces the camera ray and lights the first surface once,
and every sample of the group leaves it by a direction of its own. The
mean is that of as many samples alone; what changes is where the work
goes. 1, 2, 4 or 8 holds for every pixel and must divide `batch`; 0,
the default, lets the stopping choose: the trial draws groups of 2,
and each pixel's count pass takes the power of two up to 8 that its
neighbours' trials say costs least, from how their noise parts between
the shared stretch and the rest (a fixed count without a trial takes
1). A pixel's count is cut to whole groups. The reach is the sphere
of radius `far` about the camera's eye: every object is cut to it, so
nothing farther than `far` from the eye is met, behind the eye as much
as ahead, and shapes that never end, such as a ground plane, end at
one even distance (haze hides the edge). It moves with the eye. The
tree's region covers the objects out to the reach's bounding cube and
grows as the reach or a moved object leaves it
(reference_geometry.md).

`clip` makes the file a video. `duration` is its length in seconds
and `rate` its frames a second; it has `round(duration * rate)`
frames, and frame k is the scene at `time = k / rate`. `cuts` is a
list of times, rising and inside the clip, where one shot ends and the
next begins (default none): the frame that reaches a cut starts over
and keeps nothing of the frames before it. A file without `clip` is a
still. A `clip` block holds fields; `clip` with statements in its
braces is the join (see Joins).

The command line overrides every `render` field but `far`: `--size w
h`, `--tolerance t`, `--samples n`, `--batch n`, `--split n`, `--surfaces n`,
`--collisions n` and `--seed n` (surfaces and collisions below 1 are
taken as 1). `make render` draws the scene with the config's settings
into `output/`; rule 51 lists the settings a config takes.

A physical sky is the air over the scene lit by its first `sun`
light, scattered once, worked out per wavelength. `haze` is
how murky the air is, about 1 for mountain air and 10 for smog
(default 3); `ground` is the fraction of light the ground below the
horizon sends back. It reads no other fields, and a scene with a
physical sky must have a sun. The sun's `power` is what it sends above
the air; the air tints it on its way down.

A `sun` lies along `direction` (toward the sun) as a disc `angle`
across (radians, full angle), and `power` is the light it lands on a
surface facing it. A `directional` light is the same without a disc.
A `moon` is a disc like a sun, and `power` is what it lands when full:
only the part of its ball that faces the first sun shines, so it is
full opposite the sun and new beside it (in a scene without a sun it
is full).

A painted sky is three runs of colour over the sine of the elevation
(-1 straight down, 0 level, 1 straight up): `day`, shown with the
first sun above 12 degrees, `night`, below -14 degrees, and `dusk`
between them, shown toward the sun and shared out to day and night
away from it. A run is a list of two stops or more, each
`[height, r, g, b]` with the heights rising, and a run left out keeps
its default (a deep blue day, an orange dusk, a dark blue night). A
scene without a sun shows the day. A painted sky tints the first sun
and every moon: they fade as they sink below the horizon, and the sun
takes on the colour of the dusk horizon as it sets. It reads no other
sky fields but the glow's.

`stars` is a point of light in each of about `count` equal cells of
the sky, `angle` across (radians), of which the brightest sends
`strength`; most are far dimmer. They show only above the horizon, and
only as the first sun sets, from 2 to 12 degrees below the horizon (in
full in a scene without a sun).

Every sky takes `glow`, `strength` and `spread`: a glow about the
first sun of colour `glow` times `strength`, falling off as
exp(-angle / spread) from it (default black, so none; spread 0.12
radians). It fades out as the sun sinks to 8 degrees below the horizon,
and shows only above the horizon.

`clouds` is a layer of cloud between `height` and `height + thickness`
metres over a round planet, lit by the first sun (or by the first moon
where there is no sun or it has set) and by the sky under it. `cover`
is the share of the sky it covers (0 to 1; from about 1.4 up the layer
is uniform), `density` the light it turns aside per metre where it is
thickest, and `span` how far across the largest clouds run. The clouds
veil the sky behind them and thin the light of every sun and moon that
reaches a point through them. A camera ray, and a ray that has passed
only exact surfaces since, marches through them in full; any other ray
reads them off a table laid out once. The table is the Biased plugin
`feature/clouds`; built off (reference_architecture.md), every ray marches.

`camera.aperture` is the radius of the lens in metres; 0 (the default)
is a pinhole, sharp at every distance. `focus` is how far in front of
the eye the plane that stays sharp lies, measured along the view; 0
(the default) puts it through the target.

## The standard library

`scenes/standard.sdf` is plain scene code. It defines:
- surfaces emitted as shapes: `sphere`, `box`, `cylinder`, `capsule`,
  `cone` (height, base and tip radii, standing about its middle),
  `ellipsoid` (radii, the distance scaled by the smallest), `torus`,
  `plane`, `revolve` (a profile of [radius, height] points spun about
  z) and `sweep` (a round cone between two points);
- surfaces read as patterns: `checker` and `lean` (the sine of the
  angle between `n` and an axis, less `onset`, over `spread`).

## Formatting

The examples in this document are condensed. Scene files follow
these rules:

1. After `{` comes a new line, indented one tab; no block stays on one
   line, not even a one-statement `for` or `if`.
2. The same holds for `[` when the list's elements are lists, records
   or materials (profiles, boards, normals, colours, cities): one
   element per line, with a trailing comma. A vector such as `[0, 0, 0.45]` stays
   inline.
3. An empty line follows each block, unless a closing `}` comes next.
   After an `if` or `for` block the empty line is optional. After a
   `with` or a join (`merge`, `cut`, `clip`) there is none, unless the
   block stands at the top level of the file.
4. In a run of consecutive `let`s or fields, the `=` line
   up. An empty line or a block starts a new run.
5. No artificial else: never `if a { } if !a { }`, nor complementary
   conditions such as `if x < 5 { } if x >= 5 { }`. Use an early
   `return;`, `select`, a function that returns the value, or a list
   indexed by a number.

## Errors

The first error stops the load, which returns it as
`file:line:column: message` (rule 19).

`print(value, ...)` stands as a statement at the top level, in a
shape, a function or a tile, and writes one line to stderr: the file,
the line and the values, `scenes/example.sdf:12: 3.5 [1, 2] {x = 1}`.
Numbers have six significant digits; words print as themselves, other
values by their kind, `(a material)`. Each `print` writes once per
load and once each time a tile runs, the first time it is reached, so
one in a loop writes its first round only. A `print` met while a surface is
compiled, also in a function it calls, is an error (`a surface does
not print`), and so is one used as a value (`print stands alone as a
statement`).

## Clips

`kodanuki scene.sdf --out directory` draws the clip of a file that
has one and writes `frame-0000.ppm` and `frame-0000.pfm` onward into
the directory. `--time t` draws the still at t instead, of a file
with a clip or without, and `--out` is then an image.
`target/tools/clip-video.py directory --scene scene.sdf` joins the
frames into `clip.mp4` beside them with ffmpeg, at the clip's rate.

Each frame is drawn as the still of its time would be, by the same
stopping. While nothing changes from one frame to the next, a frame
continues the pixels' estimates; the frame of a cut starts over.

The viewer plays a clip by the clock and starts again at its end.
Where a frame takes longer to build than the clip gives it, frames
are left out. The camera is the clip's until a key moves it. `P`
pauses the time, and the frame then clears as a still does; the left
and right arrow keys step one frame back and forward.

`--reconstruction mean|temporal|temporal+denoise|temporal+denoise+upscale`
chooses how the frames are shown: `mean` (the default) is the running
mean above; `temporal` is the viewer's temporal accumulation;
`temporal+denoise` runs the denoiser after it;
`temporal+denoise+upscale`, the viewer's, then upscales the result to
the size. All but `mean` keep the guides while the camera moves, as the
viewer does. `--factor 1|2|3|4` (default 1) draws each frame at the
size divided by the factor, jittered, the phases in the Bayer order,
and writes it at the size; a factor above 1 needs
`temporal+denoise+upscale`, and a size that is a multiple of the factor.

`target/tools/track-stops.py cameras.txt` turns a camera track, a
text file of six numbers a line (the eye and the target), into two
lists of stops to paste into a scene.
