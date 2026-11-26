#include "check.h"
#include "plugins/feature/coats/coats.h"
#include "plugins/transport/paths/paths.h"
#include "plugins/transport/paths/strategy.h"
#include "sdf/compare.h"
#include "sdf/scene.h"
#include "syntax/evaluator.h"
#include "syntax/network.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace kodanuki {

double distanceAt(const Scene& scene, const std::array<double, 3>& p) {
	std::vector<double> registers;
	return sceneDistance(scene, {
									.p         = { p[0], p[1], p[2] },
									.registers = registers,
								});
}

void checkCompiledFn() {
	const Scene scene = sceneOrExit(writeScene("surface", R"(
		surface f(k) {
			let s = 0;
			for i in 0..3 {
				let s = s + i * k;
			}
			if p.x > 0 {
				let s = s * 2;
			}
			return s + length(p.yz);
		}

		at([1, 0, 0]) f(2);
	)"))
							.scene;
	check(scene.programs.size() == 1, "one program for one surface");

	const Near doubled = {
		.got       = distanceAt(scene, { 2, 3, 4 }),
		.want      = 17,
		.tolerance = 1e-12,
		.what      = "loop sum doubled location local x > 0",
	};

	checkNear(doubled);

	const Near single = {
		.got       = distanceAt(scene, { 0, 3, 4 }),
		.want      = 11,
		.tolerance = 1e-12,
		.what      = "loop sum location local x < 0",
	};

	checkNear(single);
}

void checkBlocks() {
	const Scene scene = sceneOrExit(writeScene("blocks", R"(
		function h(b) {
			let a = b + 1;
			return a;
		}

		function g(a) {
			let b = a * 2;
			return b + h(b);
		}

		surface f(k) {
			let a = g(k);
			let b = 1;
			for i in 0..2 {
				let c = i;
				if p.x > 0 {
					let a = a + c;
					let b = b * 3;
				}
			}
			return a + b + length(p.yz);
		}

		f(2);
	)"))
							.scene;

	const Near merged = {
		.got       = distanceAt(scene, { 2, 3, 4 }),
		.want      = 24,
		.tolerance = 1e-12,
		.what      = "two locals changed in an if on the point merge, the callees' names stay their own",
	};

	checkNear(merged);

	const Near kept = {
		.got       = distanceAt(scene, { -2, 3, 4 }),
		.want      = 15,
		.tolerance = 1e-12,
		.what      = "the locals keep their values where the if on the point fails",
	};

	checkNear(kept);
}

void checkPlacement() {
	const Scene scene = sceneOrExit(writeScene("place", R"(
		surface ball(r) {
			return length(p) - r;
		}

		scale(2) ball(1);
		at([10, 0, 0]) turn(z, pi / 2) scale([1, 3, 1]) ball(0.5);
		with at([0, 10, 0]) {
			for i in 0..2 {
				at([i * 4, 0, 0]) ball(1);
			}
		}

	)"))
							.scene;
	check(scene.programs.size() == 1, "numbers stay arguments");
	check(scene.objects.size() == 4, "each top-level shape an object");

	const Near scaledBall = {
		.got       = distanceAt(scene, { 3, 0, 0 }),
		.want      = 1,
		.tolerance = 1e-12,
		.what      = "scale multiplies the distance back",
	};

	checkNear(scaledBall);

	const Near turnedBall = {
		.got       = distanceAt(scene, { 13, 0, 0 }),
		.want      = 0.5,
		.tolerance = 1e-12,
		.what      = "turned: the long axis now runs along x",
	};

	checkNear(turnedBall);

	const Near loopBall = {
		.got       = distanceAt(scene, { 4, 12, 0 }),
		.want      = 1,
		.tolerance = 1e-12,
		.what      = "with and loop compose",
	};

	checkNear(loopBall);
}

void checkJoins() {
	const Scene scene = sceneOrExit(writeScene("joins", R"(
		surface ball(r) {
			return length(p) - r;
		}

		cut {
			ball(2);
			at([2, 0, 0]) ball(1);
		}

		at([0, 20, 0]) merge(1) {
			ball(1);
			at([2, 0, 0]) ball(1);
		}

	)"))
							.scene;

	const Near cutBall = {
		.got       = distanceAt(scene, { 1.5, 0, 0 }),
		.want      = 0.5,
		.tolerance = 1e-12,
		.what      = "cut removes the second ball",
	};

	checkNear(cutBall);

	const Near blended = {
		.got       = distanceAt(scene, { 1, 20, 0 }),
		.want      = -0.25,
		.tolerance = 1e-12,
		.what      = "smooth merge between equal balls",
	};

	checkNear(blended);
}

void checkRepeat() {
	const Scene scene = sceneOrExit(writeScene("repeat", R"(
		surface ball(r) {
			return length(p) - r;
		}

		let seed = 1;

		shape drawn() {
			let seed = seed + 1;
			at([seed, 0, 0]) ball(0.25);
		}

		shape lattice() {
			turn(z, pi / 2) repeat(count = [3, 1, 4], step = [1, 0, 2]) {
				drawn();
			}
		}

		lattice();
		at([0, 0, 40]) repeat(count = [2, 1, 1], step = [5, 0, 0]) {
			ball(1);
		}

	)"))
							.scene;
	check(scene.leaves.size() == 14, "a repeat of [3, 1, 4] makes 12 copies");
	check(scene.objects.size() == 3, "copies in a shape are one merge, copies at the top level objects of their own");
	bool shared = true;
	bool placed = true;
	for (std::size_t k = 0; k < 12; ++k) {
		const Leaf& leaf = scene.leaves[k];
		const int i      = int(k % 3);
		const int z      = int(k / 3);
		shared           = shared && leaf.program == scene.leaves[0].program && leaf.arguments == scene.leaves[0].arguments;
		placed           = placed && std::abs(leaf.placement.origin[0]) < 1e-6 && std::abs(leaf.placement.origin[1] - (2 + i)) < 1e-6 && std::abs(leaf.placement.origin[2] - 2 * z) < 1e-6;
	}
	check(shared, "the copies share their program and arguments");
	check(placed, "copies stand at the turned steps, x fastest, and the block draws once");

	const Near copy = {
		.got       = distanceAt(scene, { 0, 4, 6 }),
		.want      = -0.25,
		.tolerance = 1e-6,
		.what      = "the last copy is a shape",
	};

	checkNear(copy);
}

void checkUnionParts() {
	const Scene scene = sceneOrExit(writeScene("unions", R"(
		surface ball(r) {
			return length(p) - r;
		}

		shape row(n) {
			for i in 0..n {
				at([i, 0, 0]) ball(0.3);
			}
		}

		row(80);
		at([0, 10, 0]) merge(0.1) {
			for i in 0..80 {
				at([i, 0, 0]) ball(0.3);
			}
		}

		at([0, 20, 0]) row(5);
	)"))
							.scene;
	check(scene.objects.size() == 82, "a long hard union is cut into objects, a smooth one and a short one stay whole");
	check(scene.objectPrograms[80].steps.size() >= fewestUnionSteps, "the smooth union is as long as the one cut");
	check(scene.objectPrograms[0].steps.size() < fewestUnionSteps && scene.objectPrograms[81].steps.size() < fewestUnionSteps, "parts and the short union are short");

	const Near part = {
		.got       = distanceAt(scene, { 79, 0, 0 }),
		.want      = -0.3,
		.tolerance = 1e-12,
		.what      = "the cut union keeps its field",
	};

	checkNear(part);
}

void checkRules() {
	const SceneFile file           = sceneOrExit(writeScene("rules", R"(
		surface ball(r) {
			return length(p) - r;
		}

		light sphere {
			at       = [0, 0, 4];
			radius   = 0.3;
			reaches  = [stage, props];
			caustics = 0;
		}

		light point {
			at = [0, 0, 5];
		}

		group(stage) rule(camera = 0, shadows = 0) ball(1);
		with group(props) rule(caustics = 0) {
			rule(caustics = 1, camera = 0) ball(2);
			group(stage) ball(3);
		}

	)"));
	const std::uint8_t hidden      = std::uint8_t(Rule::Hidden);
	const std::uint8_t shadowless  = std::uint8_t(Rule::Shadowless);
	const std::uint8_t causticless = std::uint8_t(Rule::Causticless);
	check(file.looks.groups.size() == 2 && file.looks.groups[0] == "stage" && file.looks.groups[1] == "props", "groups are named in the order first said");
	check(file.lighting.lights[0].reaches == 3 && !file.lighting.lights[0].caustics, "a light reaches its groups and refuses caustics");
	check(file.lighting.lights[1].reaches == 0 && file.lighting.lights[1].caustics, "a light without fields reaches every shape");
	check(file.scene.leaves[0].rules == (hidden | shadowless) && file.scene.leaves[0].groups == 1, "a leaf says its rules and group");
	check(file.scene.leaves[1].rules == hidden && file.scene.leaves[1].groups == 2, "an inner rule clears and sets bits");
	check(file.scene.leaves[2].rules == causticless && file.scene.leaves[2].groups == 3, "groups nest");
}

void checkAbsoluteImport() {
	const std::string imported = writeScene("imported", R"(
		surface ball(r) {
			return length(p) - r;
		}

	)");
	const std::string text     = "import \"" + imported + "\";\nball(1);\n";
	const std::string importer = writeScene("importer", text.c_str());
	const Scene scene          = sceneOrExit(importer).scene;
	check(importer.starts_with('/') && scene.leaves.size() == 1, "an import by absolute path is read from that path, also from a scene given by an absolute path");
}

void checkRenderCaps() {
	const SceneFile capped       = sceneOrExit(writeScene("capped", R"(
		surface ball(r) {
			return length(p) - r;
		}

		render { surfaces = 3; collisions = 5; }
		ball(1);
	)"));
	const SceneFile plain        = sceneOrExit(writeScene("plain", "surface ball(r) {\n\treturn length(p) - r;\n}\n\nball(1);\n"));
	const TransportLimits limits = transportLimits(capped.shot.render);
	const TransportLimits common = transportLimits(plain.shot.render);
	check(limits.maximumDepth == 3 && limits.maximumCollisions == 5, "a render's surfaces and collisions cap the walk");
	check(common.maximumDepth == 10 && common.maximumCollisions == 256, "the caps default to 10 surfaces and 256 collisions");
}

void checkLoadErrors() {
	const std::string unended = writeScene("unended", "surface ball(r) {\n\treturn length(p) - r;\n}\n\nball(1)\n");
	const LoadedScene parsed  = loadScene(unended, 0);
	check(parsed.error == unended + ":6:1: expected ';'" && parsed.file.scene.objects.empty(), "a parse error comes back as a value with its location");

	const std::string looped = writeScene("looped", "for i in 0..100000 {\n\tfor j in 0..100000 {\n\t\tblob(1);\n\t}\n}\n");
	const LoadedScene run    = loadScene(looped, 0);
	check(run.error == looped + ":3:3: unknown function 'blob'", "an error inside loops stops the load at the first");

	const std::string empty = writeScene("empty", "let a = 1;\n");
	check(loadScene(empty, 0).error == empty + ": the scene has no shapes", "a scene without shapes is an error");

	const std::string endless = writeScene("endless", "surface ball(r) {\n\tlet d = length(p) - r;\n\tfor i in 0..40000 {\n\t\tlet d = abs(d) - r;\n\t}\n\treturn d;\n}\n\nball(1);\n");
	check(loadScene(endless, 0).error == endless + ":9:1: ball has more than 65535 steps", "a surface of more than 65535 steps is an error");

	const std::string fine = writeScene("fine", "surface ball(r) {\n\treturn length(p) - r;\n}\n\nball(1);\n");
	check(loadScene(fine, 0).error.empty(), "a load after a failed one starts clean");
}

std::string ballText(const std::string& body) {
	return "surface ball(r) {\n\treturn length(p) - r;\n}\n\n" + body;
}

double placedX(const std::string& name, const std::string& stops, const std::string& at) {
	const std::string text = ballText("let s = " + stops + ";\nat([" + at + ", 0, 0]) ball(1);\n");
	return sceneOrExit(writeScene(name.c_str(), text.c_str())).scene.leaves[0].placement.origin[0];
}

void checkTime() {
	const std::string timed = writeScene("timed", ballText("at([time, 0, 0]) ball(1);\n").c_str());
	const SceneFile still   = sceneOrExit(timed);
	const SceneFile moved   = sceneAtOrExit(timed, 2.5);
	check(still.scene.leaves[0].placement.origin[0] == 0 && moved.scene.leaves[0].placement.origin[0] == 2.5, "a scene's time is 0, or the number it is loaded at");
	check(still.timed && !sceneOrExit(writeScene("untimed", ballText("ball(1);\n").c_str())).timed, "the load notes whether the file read time");

	const std::string changed = writeScene("changed", ballText("let time = 1;\nball(1);\n").c_str());
	check(loadScene(changed, 0).error == changed + ":5:1: time belongs to the clip and cannot be changed", "let time is an error");
}

void checkClip() {
	const SceneFile clipped = sceneOrExit(writeScene("clipped", ballText("clip {\n\tduration = 12.8;\n\trate     = 24;\n\tcuts     = [4.8, 8.8];\n}\n\nclip {\n\tball(1);\n\tat([0.5, 0, 0]) ball(1);\n}\n").c_str()));
	const Clip& clip        = clipped.shot.clip;
	check(clip.duration == 12.8 && clip.rate == 24 && clip.cuts == std::vector<double> { 4.8, 8.8 } && clipFrames(clip) == 307, "a clip block holds its duration, rate and cuts, and has round(duration * rate) frames");
	check(clipped.scene.leaves.size() == 2 && sceneOrExit(writeScene("unclipped", ballText("ball(1);\n").c_str())).shot.clip.duration == 0, "clip with statements stays a join, and a file without a clip block is a still");

	const std::string falling = writeScene("falling", ballText("clip {\n\tduration = 4;\n\tcuts     = [3, 2];\n}\n\nball(1);\n").c_str());
	check(!loadScene(falling, 0).error.empty(), "a clip's cuts rise");
}

void checkStops() {
	const std::string line = "[[0, 0], [2, 4], [3, 4], [3, 10]]";
	check(placedX("before", line, "linear(s, -1)") == 0 && placedX("between", line, "linear(s, 1)") == 2 && placedX("past", line, "linear(s, 9)") == 10, "linear joins the stops by straight lines, the first before them and the last past them");
	check(placedX("leading", line, "linear(s, 2.99)") == 4 && placedX("jumped", line, "linear(s, 3)") == 10, "two stops of one time are a jump");
	const std::string curve = "[[0, 0], [1, 1], [2, 2], [3, 0]]";
	check(placedX("through", curve, "smooth(s, 1)") == 1 && std::abs(placedX("straight", "[[0, 0], [1, 1], [2, 2]]", "smooth(s, 0.5)") - 0.5) < 1e-12, "smooth passes through every stop and follows a straight run");
	check(placedX("bends", curve, "smooth(s, 2.5)") > placedX("cornered", curve, "linear(s, 2.5)"), "smooth turns no corner at a stop");
	check(placedX("vector", "[[0, 1, 2], [1, 3, 6]]", "linear(s, 0.5)[1]") == 4, "a stop of several values gives a vector");
}

void checkCompared() {
	const std::string text = ballText("light point {\n\tat    = [0, 0, 3];\n\tpower = [1, 1, 1] * (1 + time);\n}\n\npaint(matte([0.5, 0.5, 0.5] * (1 + select(time > 2, 1, 0)))) at([0, 0, select(time > 4, 1, 0)]) ball(1);\n");
	const std::string path = writeScene("compared", text.c_str());
	const SceneFile start  = sceneOrExit(path);
	const SceneFile lit    = sceneAtOrExit(path, 1);
	const SceneFile looked = sceneAtOrExit(path, 3);
	const SceneFile moved  = sceneAtOrExit(path, 5);
	check(sameShapes(start.scene, lit.scene) && sameLooks(start.looks, lit.looks) && !sameLighting(start.lighting, lit.lighting), "a light that changes leaves the shapes and the looks the same");
	check(sameShapes(start.scene, looked.scene) && !sameLooks(start.looks, looked.looks), "a material that changes leaves the shapes the same");
	check(!sameShapes(start.scene, moved.scene) && sameShapes(moved.scene, sceneAtOrExit(path, 6).scene), "a shape that moves changes the shapes, and the same evaluation twice compares equal");
}

std::string tileError(const char* name, const std::string& body) {
	const std::string text = ballText("let grey = matte([0.5, 0.5, 0.5]);\n" + body);
	return loadScene(writeScene(name, text.c_str()), 0).error;
}

std::string tiledError(const char* name, const std::string& body) {
	const std::string text = ballText("tiles(step = 4) {\n" + body + "}\n");
	const std::string path = writeScene(name, text.c_str());
	return loadScene(path, 0).error.substr(path.size());
}

void checkTileErrors() {
	check(tiledError("tiled_setting", "\trender {\n\t\twidth = 10;\n\t}\n") == ":6:2: tiles may not hold settings", "tiles may not hold settings");
	check(tiledError("tiled_import", "\timport \"other.sdf\";\n") == ":6:2: tiles may not import", "tiles may not import");
	check(tiledError("tiled_function", "\tfunction f() {\n\t\treturn 1;\n\t}\n") == ":6:2: tiles may not define a function, a surface or a shape", "tiles may not define a function");
	check(tiledError("tiled_shape", "\tshape s() {\n\t\tball(1);\n\t}\n") == ":6:2: tiles may not define a function, a surface or a shape", "tiles may not define a shape");
	check(tiledError("tiled_material", "\tpaint(matte([1, 0, 0])) ball(1);\n") == ":6:8: tiles may not make a material", "tiles may not make a material");
	check(tiledError("tiled_coat", "\tcoat(shade(0.5), above(1)) ball(1);\n") == ":6:2: tiles may only use coats made before them", "tiles may not make a coat");
	check(tiledError("tiled_group", "\tgroup(stage) ball(1);\n") == ":6:2: tiles may not put shapes in a group", "tiles may not put shapes in a group");
	check(tiledError("tiled_rule", "\trule(camera = 0) ball(1);\n") == ":6:2: tiles may not say rules", "tiles may not say rules");
	check(tiledError("tiled_tiles", "\ttiles(step = 1) {\n\t\tball(1);\n\t}\n") == ":6:2: tiles may not hold tiles", "tiles may not hold tiles");
	check(tileError("looped_tiles", "for i in 0..1 {\n\ttiles(step = 1) {\n\t\tball(1);\n\t}\n}\n").ends_with(":7:2: tiles stand at the top level of a file"), "tiles inside a loop are refused");
	check(tileError("shaped_tiles", "shape s() {\n\ttiles(step = 1) {\n\t\tball(1);\n\t}\n}\n\ns();\n").ends_with(":7:2: tiles stand at the top level of a file"), "tiles inside a shape are refused");
	check(tileError("stepless_tiles", "tiles(size = 1) {\n\tball(1);\n}\n").ends_with(":6:1: tiles take a step"), "tiles take a step");
	check(tileError("flat_tiles", "tiles(step = [1, 0, 1]) {\n\tball(1);\n}\n").ends_with(":6:1: a tile's step is above 0 along x, y and z"), "a tile's step is above 0");
}

std::string boundsError(const char* name, const std::string& bounds) {
	return tileError(name, "tiles(step = 4" + bounds + ") {\n\tball(1);\n}\n");
}

void checkTileBounds() {
	check(boundsError("from_alone", ", from = [0, 0, 0]").ends_with(":6:1: tiles take from and to together"), "tiles take from only with to");
	check(boundsError("to_alone", ", to = [0, 0, 0]").ends_with(":6:1: tiles take from and to together"), "tiles take to only with from");
	check(boundsError("spread_bounds", ", from = 0, to = [1, 1, 1]").ends_with(":6:1: a tile's from and to are 3 whole numbers each"), "a tile bound is a vector, not one number");
	check(boundsError("short_bounds", ", from = [0, 0], to = [1, 1]").ends_with(":6:1: a tile's from and to are 3 whole numbers each"), "a tile bound has 3 numbers");
	check(boundsError("broken_bounds", ", from = [0, 0.5, 0], to = [1, 1, 1]").ends_with(":6:1: a tile's from and to are 3 whole numbers each"), "a tile bound is whole");
	check(boundsError("turned_bounds", ", from = [0, 2, 0], to = [1, 1, 1]").ends_with(":6:1: a tile's from is at most its to along x, y and z"), "a tile's from is at most its to");
	check(boundsError("other_bounds", ", size = [1, 1, 1]").ends_with(":6:1: tiles take a step"), "tiles take no other name");
	const std::string path   = writeScene("bounded_tiles", ballText("tiles(step = [4, 8, 16], from = [-2, 0, 0], to = [3, 1, 0]) {\n\tball(1);\n}\n").c_str());
	const LoadedScene loaded = loadScene(path, 0);
	const TileBlock& block   = loaded.file.scene.tiles.at(0);
	check(loaded.error.empty() && block.from[0] == -2 && block.from[1] == 0 && block.to[0] == 3 && block.to[1] == 1 && block.to[2] == 0, "a tiles block keeps its from and to");
	const SceneFile unbounded = sceneOrExit(writeScene("unbounded_tiles", ballText("tiles(step = 4) {\n\tball(1);\n}\n").c_str()));
	check(unbounded.scene.tiles.at(0).from[2] == lowestTile && unbounded.scene.tiles.at(0).to[2] == highestTile, "a block without bounds runs every index");
}

TilePlace tilePlace(std::int64_t i, std::int64_t j, std::int64_t k) {
	return TilePlace {
		.block = 0,
		.index = { i, j, k },
	};
}

TileContent stillTile(const Scene& scene, const TilePlace& place) {
	const math::Vector eye = { 0, 0, 0 };
	return evaluatedTile(scene, place, eye);
}

void checkTiles() {
	const std::string text   = ballText("let n = 1;\n\nshape counted() {\n\tlet n = n + 10;\n}\n\ntiles(step = [4, 8, 16]) {\n\tif tile.z == 0 {\n\t\tlet n = n + tile.x;\n\t\tcounted();\n\t\tat([n, 1, 2]) ball(0.5);\n\t}\n}\n\nlet after = 3;\n");
	const std::string path   = writeScene("tiles", text.c_str());
	const LoadedScene loaded = loadScene(path, 0);
	const Scene& scene       = loaded.file.scene;
	check(loaded.error.empty() && scene.objects.empty() && scene.tiles.size() == 1 && scene.tiles[0].step[1] == 8 && loaded.file.timed, "a scene of tiles alone loads, holds its block and is evaluated again at each time");
	const TileContent second = stillTile(scene, tilePlace(2, 1, 0));
	const TileContent first  = stillTile(scene, tilePlace(1, 1, 0));
	const double* moved      = second.scene.leaves[0].placement.origin;
	check(second.error.empty() && second.scene.objectPrograms.size() == 1 && moved[0] == 8 + 13 && moved[1] == 9 && moved[2] == 2, "a tile's shapes are moved by its index times the step, from the bindings where the block stands");
	check(first.scene.leaves[0].placement.origin[0] == 4 + 12 && stillTile(scene, tilePlace(2, 1, 0)).scene.leaves[0].placement.origin[0] == moved[0], "a tile's changes to the bindings are its own");
	check(stillTile(scene, tilePlace(0, 0, 1)).scene.objects.empty() && first.reads.contains("n") && !first.reads.contains("tile"), "a tile may make nothing, and notes the top-level names it read");
	const std::string late = writeScene("late_tiles", ballText("tiles(step = 1) {\n\tat([after, 0, 0]) ball(1);\n}\n\nlet after = 1;\n").c_str());
	check(stillTile(loadScene(late, 0).file.scene, tilePlace(0, 0, 0)).error == late + ":6:2: unknown name 'after'", "a tile sees no binding made after its block");
	const std::string shaped = writeScene("shaped_material", ballText("shape red() {\n\tpaint(matte([1, 0, 0])) ball(1);\n}\n\ntiles(step = 1) {\n\tred();\n}\n").c_str());
	check(stillTile(loadScene(shaped, 0).file.scene, tilePlace(0, 0, 0)).error == shaped + ":6:8: tiles may not make a material", "a shape a tile calls may not make a material");
	const SceneFile later = sceneAtOrExit(path, 1);
	check(sameShapes(scene, later.scene), "tiles that read nothing of the time make the same shapes at another time");
	scene.tiles[0].reads->names.insert("time");
	check(!sameShapes(scene, later.scene), "tiles that read the time make other shapes at another time");
	checkTileErrors();
	checkTileBounds();
}

double placedBy(const char* name, const std::string& body) {
	return sceneOrExit(writeScene(name, ballText(body).c_str())).scene.leaves[0].placement.origin[0];
}

std::string loadError(const char* name, const std::string& body) {
	const std::string path = writeScene(name, ballText(body).c_str());
	return loadScene(path, 0).error.substr(path.size());
}

void checkTileNames() {
	const std::string text   = ballText("function west() {\n\treturn tile_origin.x + tile.y;\n}\n\nshape inner() {\n\tat([west(), tile.x * tile_origin.y, tile_step.z]) ball(0.5);\n}\n\nshape outer() {\n\tinner();\n}\n\ntiles(step = [4, 8, 16]) {\n\touter();\n}\n");
	const LoadedScene loaded = loadScene(writeScene("tile_names", text.c_str()), 0);
	const TileContent tile   = stillTile(loaded.file.scene, tilePlace(-3, 2, 0));
	const double* origin     = tile.scene.leaves.at(0).placement.origin;
	check(tile.error.empty() && origin[0] == -12 - 12 + 2 && origin[1] == 16 - 3 * 16 && origin[2] == 16, "shapes and functions a tile calls, two deep, read its index, origin and step, also for a negative index");
	check(!tile.reads.contains("tile") && !tile.reads.contains("tile_origin") && !tile.reads.contains("tile_step"), "reading the tile's own names is no read of the top level");
	check(loadError("untiled", "at([tile.x, 0, 0]) ball(1);\n") == ":5:5: tile is known only inside tiles", "tile outside tiles is an error");
	check(loadError("untiled_function", "function f() {\n\treturn tile_origin.x;\n}\n\nat([f(), 0, 0]) ball(1);\n") == ":6:9: tile_origin is known only inside tiles", "a function called outside tiles may not read the tile");
	check(loadError("bound_tile", "let tile_step = 1;\nball(1);\n") == ":5:1: tile_step belongs to the tiles and may not be bound at the top level", "a top-level binding of a tile's name is an error");
	check(placedBy("local_tile", "shape s() {\n\tlet tile = 2;\n\tat([tile, 0, 0]) ball(1);\n}\n\ns();\n") == 2, "a local of a tile's name is fine outside tiles");
}

void checkTopLevelFunctions() {
	check(placedBy("global", "let k = 2;\nfunction f(a) {\n\treturn a + k;\n}\n\nat([f(1), 0, 0]) ball(1);\n") == 3, "a function reads a top-level binding");
	check(placedBy("shadowed", "let k = 2;\nfunction f(a) {\n\tlet k = k + a;\n\treturn k;\n}\n\nlet x = f(1);\nat([x * 10 + k, 0, 0]) ball(1);\n") == 32, "a let of a top-level name in a function makes a name of its own");
	check(loadError("caller", "function f(a) {\n\treturn a + hidden;\n}\n\nshape s() {\n\tlet hidden = 5;\n\tat([f(1), 0, 0]) ball(1);\n}\n\ns();\n") == ":6:11: unknown name 'hidden'", "a function sees no binding of its caller");
	const std::string timed = writeScene("timed_function", ballText("function later(a) {\n\treturn a + time;\n}\n\nat([later(1), 0, 0]) ball(1);\n").c_str());
	check(sceneAtOrExit(timed, 2.5).scene.leaves[0].placement.origin[0] == 3.5 && sceneOrExit(timed).timed, "a function reads the time");
	const Scene inlined = sceneOrExit(writeScene("inlined_global", ballText("let k = 3;\nfunction grown(x) {\n\treturn x + k;\n}\n\nsurface blob(r) {\n\treturn length(p) - grown(r);\n}\n\nblob(1);\n").c_str())).scene;

	const Near grown = {
		.got       = distanceAt(inlined, { 0, 0, 0 }),
		.want      = -4,
		.tolerance = 1e-12,
		.what      = "a function inlined in a surface reads a top-level binding as a constant",
	};

	checkNear(grown);
	const std::string tiled = writeScene("tiled_function", ballText("let k = 2;\nfunction f(a) {\n\treturn a + k;\n}\n\ntiles(step = 4) {\n\tat([f(1), 0, 0]) ball(1);\n}\n").c_str());
	const TileContent tile  = stillTile(loadScene(tiled, 0).file.scene, tilePlace(0, 0, 0));
	check(tile.error.empty() && tile.scene.leaves[0].placement.origin[0] == 3 && tile.reads.contains("k"), "a tile notes a top-level binding a function it calls read");
}

void checkListFunctions() {
	check(placedBy("listed", "function pair(a) {\n\treturn [a, a * 2];\n}\n\nlet q = pair(3);\nat([q[1], 0, 0]) ball(1);\n") == 6, "a function returns a list");
	check(placedBy("gridded", "function grid(a) {\n\treturn [[a, 1], [2, a * 4]];\n}\n\nlet g = grid(2);\nat([g[1][1] + g[0][1], 0, 0]) ball(1);\n") == 9, "a function returns a list of lists");
	const Scene compiled = sceneOrExit(writeScene("listed_surface", ballText("function corners(r) {\n\treturn [[r, p.x], [0, r * 2]];\n}\n\nsurface blob(r) {\n\tlet c = corners(r);\n\treturn length(p) - c[1][1] - c[0][1];\n}\n\nblob(1.5);\n").c_str())).scene;

	const Near cornered = {
		.got       = distanceAt(compiled, { 2, 0, 0 }),
		.want      = -3,
		.tolerance = 1e-12,
		.what      = "a function inlined in a surface returns a list of lists",
	};

	checkNear(cornered);
}

void checkNestedArguments() {
	const Scene scene = sceneOrExit(writeScene("nested_arguments", ballText("function controls(s) {\n\treturn [[(i * 4 + j) * s for j in 0..4] for i in 0..4];\n}\n\nsurface lifted(c) {\n\treturn length(p) - c[1][2] - c[3][3];\n}\n\nlifted(controls(0.1));\nat([100, 0, 0]) lifted(controls(0.2));\n").c_str())).scene;
	check(scene.programs.size() == 1, "two lists of lists of numbers of one shape share one program");

	const Near first = {
		.got       = distanceAt(scene, { 0, 0, 0 }),
		.want      = -2.1,
		.tolerance = 1e-12,
		.what      = "a list of lists of numbers is an argument of the program",
	};

	checkNear(first);

	const Near second = {
		.got       = distanceAt(scene, { 100, 0, 0 }),
		.want      = -4.2,
		.tolerance = 1e-12,
		.what      = "a second list of lists of numbers gives its own distances",
	};

	checkNear(second);
	const Scene ragged = sceneOrExit(writeScene("ragged_arguments", ballText("surface last(c) {\n\treturn length(p) - c[1][count(c[1]) - 1];\n}\n\nlast([[1, 2], [3]]);\nat([100, 0, 0]) last([[1], [2, 4]]);\n").c_str())).scene;
	check(ragged.programs.size() == 2 && distanceAt(ragged, { 100, 0, 0 }) == -4, "lists of lists of other shapes compile programs of their own");
}

void checkFunctions() {
	check(placedBy("returned", "function twice(a) {\n\tlet b = a * 2;\n\treturn b + 1;\n}\n\nat([twice(3), 0, 0]) ball(1);\n") == 7, "a function runs at load time and returns a value");
	const Scene kept = sceneOrExit(writeScene("surface_let", ballText("let k = 1;\nsurface s(r) {\n\tlet k = 7;\n\treturn length(p) - r;\n}\n\ns(1);\nat([k, 0, 0]) ball(1);\n").c_str())).scene;
	check(kept.leaves[1].placement.origin[0] == 1, "a surface's let of a top-level name is its own");
	check(placedBy("threaded", "function next(s) {\n\treturn s + 1;\n}\n\nlet s = 1;\nlet s = next(s);\nlet s = next(s);\nat([s, 0, 0]) ball(1);\n") == 3, "a function's result threads a value through the file");
	const Scene inlined = sceneOrExit(writeScene("inlined", ballText("function square(x) {\n\treturn x * x;\n}\n\nsurface blob(r) {\n\treturn length(p) - square(r);\n}\n\nblob(2);\n").c_str())).scene;

	const Near squared = {
		.got       = distanceAt(inlined, { 0, 0, 0 }),
		.want      = -4,
		.tolerance = 1e-12,
		.what      = "a function called in a surface is inlined",
	};

	checkNear(squared);
	checkTopLevelFunctions();
	checkListFunctions();
	check(loadError("emitting", "function f(a) {\n\tball(a);\n\treturn a;\n}\n\nat([f(1), 0, 0]) ball(1);\n") == ":6:2: a surface or a function only computes; shapes are emitted in a shape or at the top", "a function emits nothing");
	check(loadError("unreturned", "function f(a) {\n\tlet b = a;\n}\n\nat([f(1), 0, 0]) ball(1);\n") == ":5:1: f never returns", "a function must return");
	check(loadError("stated", "function f(a) {\n\treturn a;\n}\n\nf(1);\n") == ":9:1: only a surface or a shape can stand as a statement", "a function is no statement");
}

void checkComprehensions() {
	check(placedBy("mapped", "let xs = [x * 2 for x in [1, 2, 3]];\nat([xs[2], 0, 0]) ball(1);\n") == 6, "a comprehension maps a list");
	check(placedBy("nested", "let g = [[x, y] for x in 0..2, y in 0..3];\nat([count(g) * 10 + g[5][0] + g[5][1], 0, 0]) ball(1);\n") == 63, "a comprehension's loops nest, the first outermost");
	check(placedBy("filtered", "let odd = [x for x in 0..10 if x - floor(x / 2) * 2 == 1];\nat([count(odd) * 10 + odd[4], 0, 0]) ball(1);\n") == 59, "a comprehension keeps the elements its condition holds for");
	const Scene compiled = sceneOrExit(writeScene("comprehended", ballText("surface far(r) {\n\tlet q = [p.x * k for k in [1, 2]];\n\treturn q[1] - r;\n}\n\nfar(0.5);\n").c_str())).scene;

	const Near doubled = {
		.got       = distanceAt(compiled, { 1, 0, 0 }),
		.want      = 1.5,
		.tolerance = 1e-12,
		.what      = "a comprehension in a surface holds registers",
	};

	checkNear(doubled);
	check(loadError("unknowable", "surface far(r) {\n\tlet q = [k for k in [1, 2] if p.x > 0];\n\treturn q[0] - r;\n}\n\nfar(0.5);\n") == ":6:36: a comprehension's condition must be known when the scene loads", "a comprehension's condition is known when the scene loads");
}

std::vector<std::array<double, 3>> placedOrigins(const std::string& path) {
	std::vector<std::array<double, 3>> origins;
	for (const Leaf& leaf : sceneOrExit(path).scene.leaves) {
		origins.push_back({ leaf.placement.origin[0], leaf.placement.origin[1], leaf.placement.origin[2] });
	}
	return origins;
}

bool noisesInRange(const std::vector<std::array<double, 3>>& origins) {
	double lowest  = 1;
	double highest = -1;
	for (const std::array<double, 3>& o : origins) {
		if (o[0] < 0 || o[0] >= 1 || std::abs(o[1]) > 1 || std::abs(o[2]) > 1) {
			return false;
		}
		lowest  = std::min(lowest, o[1]);
		highest = std::max(highest, o[1]);
	}
	return lowest < -0.5 && highest > 0.5;
}

bool distinctHashes(std::vector<std::array<double, 3>> origins) {
	std::ranges::sort(origins);
	for (std::size_t i = 1; i < origins.size(); ++i) {
		if (origins[i][0] == origins[i - 1][0]) {
			return false;
		}
	}
	return true;
}

bool closePairs(const std::vector<std::array<double, 3>>& origins) {
	for (const std::array<double, 3>& o : origins) {
		if (std::abs(o[0] - o[1]) > 1e-5) {
			return false;
		}
	}
	return true;
}

void checkNoiseErrors() {
	check(loadError("noisy_surface", "surface bumpy(r) {\n\treturn length(p) - r + 0.1 * noise(p.xy);\n}\n\nbumpy(1);\n") == ":6:31: noise is known only when the scene loads", "noise in a surface that depends on p is an error");
	check(loadError("hashed_surface", "surface spotted(r) {\n\treturn length(p) - hash(r);\n}\n\nspotted(1);\n") == ":6:21: hash is known only when the scene loads", "hash of a surface's argument is an error");
	check(loadError("flat_noise", "at([noise(1), 0, 0]) ball(1);\n") == ":5:5: noise takes a vector of 2 or 3 numbers", "noise takes a vector of 2 or 3 numbers");
	check(loadError("no_octaves", "at([fbm([1, 2], 0), 0, 0]) ball(1);\n") == ":5:5: fbm's octaves are a whole number of 1 or more", "fbm takes at least one octave");
}

void checkNoise() {
	const std::string sampled                       = writeScene("sampled", ballText("for i in 0..200 {\n\tat([hash(i, 7), noise([i * 0.37, i * 0.11]), fbm([i * 0.53, 1.3, -i * 0.29], 5)]) ball(1);\n}\n").c_str());
	const std::vector<std::array<double, 3>> values = placedOrigins(sampled);
	check(values.size() == 200 && values == placedOrigins(sampled), "hash, noise and fbm give the same values at every load");
	check(noisesInRange(values), "hash lies in [0, 1), noise and fbm in [-1, 1], and noise spreads over it");
	check(distinctHashes(values), "neighbouring inputs hash to different values");
	check(placedBy("listed_hash", "at([hash([1, 2]) - hash(1, 2), 0, 0]) ball(1);\n") == 0 && placedBy("ordered_hash", "at([hash(1, 2) - hash(2, 1), 0, 0]) ball(1);\n") != 0, "a vector hashes as its numbers, in order");
	const std::string near = writeScene("near_noise", ballText("let qs = [[0.3, 0.7, 0.1], [2.9999999, 1.5, -3], [-1, -4.2, 7.0000001], [12.5, -0.0000001, 0]];\nfor q in qs {\n\tat([noise(q.xy), noise(q.xy + 2e-7), 0]) ball(1);\n\tat([noise(q), noise(q - 2e-7), 0]) ball(1);\n\tat([fbm(q.xy, 5), fbm(q.xy + 2e-7, 5), 0]) ball(1);\n}\n").c_str());
	check(closePairs(placedOrigins(near)), "noise and fbm are continuous, across cell edges too");
	check(placedBy("noise_function", "function height(x) {\n\treturn fbm([x, 2], 4);\n}\n\nat([height(0.5) - fbm([0.5, 2], 4), 0, 0]) ball(1);\n") == 0, "a function may call fbm");
	const std::string tiled  = writeScene("noisy_tiles", ballText("at([hash(2), 0, 0]) ball(1);\n\ntiles(step = 4) {\n\tat([hash(tile.x), 0, 0]) ball(1);\n}\n").c_str());
	const LoadedScene loaded = loadScene(tiled, 0);
	const TileContent tile   = stillTile(loaded.file.scene, tilePlace(2, 0, 0));
	check(tile.error.empty() && tile.scene.leaves[0].placement.origin[0] == 8 + loaded.file.scene.leaves[0].placement.origin[0], "a tile may hash its index");
	checkNoiseErrors();
}

void checkReturnErrors() {
	check(loadError("bare_function", "function f(a) {\n\treturn;\n}\n\nat([f(1), 0, 0]) ball(1);\n") == ":6:2: a surface or a function must return a value", "a function's return has a value");
	check(loadError("bare_surface", "surface s(r) {\n\treturn;\n}\n\ns(1);\n") == ":6:2: a surface or a function must return a value", "a surface's return has a value");
	check(loadError("valued_shape", "shape s() {\n\treturn 1;\n}\n\ns();\n") == ":6:2: a shape or a tile body returns no value", "a shape's return has no value");
	check(loadError("top_return", "ball(1);\nreturn;\n") == ":6:1: return belongs in a surface, a function, a shape or a tile body", "a return at the top level is an error");
	check(loadError("else", "if 1 {\n\tball(1);\n} else {\n\tball(2);\n}\n") == ":7:3: there is no else", "an else names itself as the error");
	check(loadError("merged_return", "shape s() {\n\tmerge {\n\t\tball(1);\n\t\tif 1 {\n\t\t\treturn;\n\t\t}\n\t}\n}\n\ns();\n") == ":9:4: return may not stand inside a join or a repeat", "a return inside a join is an error");
	check(loadError("repeated_return", "shape s() {\n\trepeat(count = 2, step = 3) {\n\t\tball(1);\n\t\treturn;\n\t}\n}\n\ns();\n") == ":8:3: return may not stand inside a join or a repeat", "a return inside a repeat is an error");
	check(loadError("followed_return", "function f(a) {\n\treturn a;\n\tlet b = a;\n}\n\nat([f(1), 0, 0]) ball(1);\n") == ":7:2: nothing may follow a return", "nothing may follow a function's return");
	const std::string valued = writeScene("valued_tile", ballText("tiles(step = 4) {\n\tball(1);\n\treturn 1;\n}\n").c_str());
	check(stillTile(loadScene(valued, 0).file.scene, tilePlace(0, 0, 0)).error == valued + ":7:2: a shape or a tile body returns no value", "a tile body's return has no value");
}

void checkValueReturn() {
	check(placedBy("returned_if", "function f(x) {\n\tif x > 1 {\n\t\treturn 1;\n\t}\n\treturn 2;\n}\n\nat([f(3) * 10 + f(0), 0, 0]) ball(1);\n") == 12, "a function returns early from an if at load time");
	check(placedBy("returned_for", "function find(l, v) {\n\tfor i in 0..count(l) {\n\t\tif l[i] == v {\n\t\t\treturn i;\n\t\t}\n\t}\n\treturn -1;\n}\n\nat([find([4, 5, 6], 5) * 10 + find([4], 9), 0, 0]) ball(1);\n") == 9, "a function returns early from a for loop at load time");
	const Scene scene = sceneOrExit(writeScene("returned_surface", ballText("let BIG = 1;\nfunction shrink(d) {\n\tif BIG > 0 {\n\t\treturn d - 0.5;\n\t}\n\treturn d;\n}\n\nsurface s(r) {\n\tfor i in 0..4 {\n\t\tif i == 2 {\n\t\t\treturn shrink(length(p) - r) - i;\n\t\t}\n\t}\n\treturn 0;\n}\n\ns(1);\n").c_str())).scene;

	const Near near = {
		.got       = distanceAt(scene, { 0, 0, 0 }),
		.want      = -3.5,
		.tolerance = 1e-12,
		.what      = "a surface and a function inlined into it return early where the condition is known when the scene loads",
	};

	checkNear(near);
	check(loadError("point_return", "function g(d) {\n\tif d > 0 {\n\t\treturn d;\n\t}\n\treturn 0;\n}\n\nsurface s(r) {\n\treturn g(length(p) - r);\n}\n\ns(1);\n") == ":6:2: a return inside an if not known when the scene loads; use select", "a return inside an if that depends on the point is an error");
	check(loadError("early_unreturned", "function f(x) {\n\tif x > 1 {\n\t\treturn 1;\n\t}\n}\n\nat([f(0), 0, 0]) ball(1);\n") == ":5:1: f never returns", "a function that returns early on one path must still return on every path");
}

void checkEarlyReturn() {
	const std::string ended                          = writeScene("ended_shape", ballText("shape row(n) {\n\tfor i in 0..5 {\n\t\tif i == n {\n\t\t\treturn;\n\t\t}\n\t\tat([i, 0, 0]) ball(1);\n\t}\n\tat([9, 0, 0]) ball(1);\n}\n\nshape placed() {\n\twith at([0, 5, 0]) {\n\t\tball(1);\n\t\treturn;\n\t}\n\tball(2);\n}\n\nrow(2);\nplaced();\nat([20, 0, 0]) ball(1);\n").c_str());
	const std::vector<std::array<double, 3>> origins = placedOrigins(ended);
	const std::vector<std::array<double, 3>> want    = {
		{ 0, 0, 0 },
		{ 1, 0, 0 },
		{ 0, 5, 0 },
		{ 20, 0, 0 },
	};
	check(origins == want, "a bare return ends its shape inside if, for and with, keeps the shapes before it and drops those after");
	const std::string tiled  = writeScene("ended_tile", ballText("shape half() {\n\tball(1);\n\treturn;\n\tball(2);\n}\n\ntiles(step = 4) {\n\thalf();\n\tif tile.x == 1 {\n\t\treturn;\n\t}\n\tat([1, 0, 0]) ball(1);\n}\n").c_str());
	const LoadedScene loaded = loadScene(tiled, 0);
	const TileContent whole  = stillTile(loaded.file.scene, tilePlace(0, 0, 0));
	const TileContent ending = stillTile(loaded.file.scene, tilePlace(1, 0, 0));
	check(whole.error.empty() && whole.scene.leaves.size() == 2, "a return in a shape a tile calls ends only that shape");
	check(ending.error.empty() && ending.scene.leaves.size() == 1 && ending.scene.leaves[0].placement.origin[0] == 4, "a bare return ends a tile body and keeps the shapes before it");
	checkReturnErrors();
	checkValueReturn();
}

void checkRecordErrors() {
	check(loadError("mixed_fields", "let l = [{ a = 1 }, { b = 1 }];\nball(1);\n") == ":5:9: a list that mixes records of different fields", "a list of records with different fields is an error");
	check(loadError("mixed_records", "let l = [{ a = 1 }, 1];\nball(1);\n") == ":5:9: a list that mixes records and other kinds", "a list that mixes records and numbers is an error");
	check(loadError("unknown_field", "let r = { a = 1 };\nat([r.b, 0, 0]) ball(1);\n") == ":6:5: the record has no field 'b'", "an unknown field is an error naming it");
	check(loadError("twice_field", "let r = { a = 1, a = 2 };\nball(1);\n") == ":5:18: the field 'a' is written twice", "a field written twice is an error");
	check(loadError("added_record", "let r = { a = 1 } + 1;\nball(1);\n").ends_with("expected a number, found a record"), "records take no arithmetic");
	check(loadError("compared_record", "let r = { a = 1 } == { a = 1 };\nball(1);\n").ends_with("expected a number, found a record"), "records take no comparison");
	check(loadError("painted_record", "let grey = matte([0.5, 0.5, 0.5]);\nsurface disc(c) {\n\treturn length(p) - 1;\n}\n\ndisc({ m = grey });\n") == ":10:1: a record passed to a surface holds only numbers and lists of numbers", "a record holding a material is no surface argument");
}

void checkRecordArguments() {
	const Scene scene = sceneOrExit(writeScene("record_arguments", ballText("surface disc(c) {\n\treturn length(p - c.at) - c.size.radius;\n}\n\ndisc({ at = [0, 0, 0], size = { radius = 1 } });\ndisc({ size = { radius = 2 }, at = [100, 0, 0] });\n").c_str())).scene;
	check(scene.programs.size() == 1 && scene.programs[0].arguments == 4, "records of the same fields passed to a surface share one program, their numbers its arguments");

	const Near first = {
		.got       = distanceAt(scene, { 0, 0, 0 }),
		.want      = -1,
		.tolerance = 1e-12,
		.what      = "a record's fields read in a surface are its arguments",
	};

	checkNear(first);

	const Near second = {
		.got       = distanceAt(scene, { 100, 0, 0 }),
		.want      = -2,
		.tolerance = 1e-12,
		.what      = "a second record gives its own distances",
	};

	checkNear(second);
}

void checkReadsInPlace() {
	check(placedBy("read_in_place", "let L = [[1, 2], [3, 4]];\nlet R = { items = [5, 6, 7], inner = [{ k = 8 }] };\nat([L[1][0] + count(R.items) * 10 + R.inner[0].k * 100, 0, 0]) ball(1);\n") == 833, "an element of a nested list, a field of a listed record and the count of a field are read where they are held");
	check(loadError("index_in_place", "let L = [1, 2];\nat([L[2], 0, 0]) ball(1);\n") == ":6:5: index 2 is outside the list", "an index read in place outside its list is an error");
	const std::string tiled = writeScene("tiled_in_place", ballText("let L = [[1, 2], [3, 4]];\n\ntiles(step = 4) {\n\tat([L[1][1], 0, 0]) ball(count(L) / 2);\n}\n").c_str());
	const TileContent tile  = stillTile(loadScene(tiled, 0).file.scene, tilePlace(1, 0, 0));
	check(tile.error.empty() && tile.scene.leaves[0].placement.origin[0] == 8 && tile.reads.contains("L"), "a tile that reads an element of a top-level list notes the list");
}

void checkRecords() {
	check(placedBy("record", "let c = { at = [3, 0, 0], radius = 2, style = 1 };\nat([c.at.x + c.radius * 10 + c.style * 100, 0, 0]) ball(1);\n") == 123, "a record's fields are read by name");
	check(placedBy("nested_record", "function city(x) {\n\treturn { at = [x, 0, 0], inner = { k = 4, m = matte([1, 1, 1]) } };\n}\n\nlet r = city(5);\npaint(r.inner.m) at([r.at.x + r.inner.k * 10, 0, 0]) ball(1);\n") == 45, "a function returns a record whose fields hold records and materials");
	check(placedBy("listed_records", "let CITIES = [\n\t{ at = [1, 0, 0], radius = 2 },\n\t{ radius = 5, at = [2, 0, 0] },\n];\nlet radii = [c.radius for c in CITIES];\nat([radii[0] + radii[1] * 10 + CITIES[1].at.x * 100, 0, 0]) ball(1);\n") == 252, "a list of records of the same fields, in any order, takes a comprehension");
	check(placedBy("swizzled_record", "let v = [1, 2, 3];\nlet r = { x = 7, xz = 9 };\nat([v.xz[1] + r.x * 10 + r.xz * 100, 0, 0]) ball(1);\n") == 973, "a member of a list is a swizzle, of a record a field");
	const std::string tiled = writeScene("tiled_record", ballText("let C = { x = 2, r = 1 };\n\ntiles(step = 4) {\n\tat([C.x, 0, 0]) ball(C.r);\n}\n").c_str());
	const TileContent tile  = stillTile(loadScene(tiled, 0).file.scene, tilePlace(1, 0, 0));
	check(tile.error.empty() && tile.scene.leaves[0].placement.origin[0] == 6 && tile.reads.contains("C"), "a tile reads a top-level record and notes its name");
	checkRecordArguments();
	checkRecordErrors();
}

SceneFile coatedScene(const char* name, const std::string& body) {
	return sceneOrExit(writeScene(name, ballText(body).c_str()));
}

bool woreCoats(const Leaf& leaf, const std::vector<std::uint32_t>& coats) {
	return leaf.coats == coats;
}

void checkCoatErrors() {
	check(loadError("surface_coat", "let c = coat(wet(1), above(0));\nball(c);\n") == ":6:1: a surface takes no coat", "a coat passed to a surface is an error");
	check(loadError("material_coat", "let grey = matte([0.5, 0.5, 0.5]);\ncoat(grey) ball(1);\n") == ":6:1: coat takes an effect and a mask, a bump alone, or a coat", "a one-argument coat of no coat and no bump is an error");
	check(tiledError("tiled_let_coat", "\tlet c = coat(wet(1), above(0));\n") == ":6:10: tiles may only use coats made before them", "a tile body may not bind a new coat");
	const std::string shaped = writeScene("shaped_coat", ballText("shape soaked() {\n\tcoat(wet(1), above(0)) ball(1);\n}\n\ntiles(step = 1) {\n\tsoaked();\n}\n").c_str());
	check(stillTile(loadScene(shaped, 0).file.scene, tilePlace(0, 0, 0)).error == shaped + ":6:2: tiles may only use coats made before them", "a shape a tile calls may not make a coat");
}

void checkTiledCoats() {
	const std::string path   = writeScene("tiled_coats", ballText("let wet_road = coat(wet(0.6), facing([0, 0, 1], level = 0.9));\n\ntiles(step = 4) {\n\tcoat(wet_road) at([2, 2, 2]) ball(1);\n}\n").c_str());
	const LoadedScene loaded = loadScene(path, 0);
	const TileContent tile   = stillTile(loaded.file.scene, tilePlace(1, 0, 0));
	check(tile.error.empty() && woreCoats(tile.scene.leaves.at(0), { 0 }) && tile.reads.contains("wet_road"), "a tile applies a coat made before it, and its leaf carries the coat");
	check(sceneWearsCoats(loaded.file.scene, loaded.file.looks) && !sceneWearsCoats(loaded.file.scene, Looks {}), "a scene with tiles and coats turns the coats on");
}

void checkBoundCoats() {
	const SceneFile bound = coatedScene("bound_coat", "let wet_road = coat(wet(0.6), facing([0, 0, 1], level = 0.9));\ncoat(wet_road) ball(1);\ncoat(wet_road) at([3, 0, 0]) ball(1);\n");
	check(bound.looks.coats.size() == 1 && bound.looks.coats[0].effect == CoatEffect::Wet && bound.looks.coats[0].mask == CoatMask::Facing, "a bound coat is made once where the let runs");
	check(woreCoats(bound.scene.leaves[0], { 0 }) && woreCoats(bound.scene.leaves[1], { 0 }), "shapes apply a bound coat");
	const SceneFile stacked = coatedScene("stacked_coats", "let a = coat(tint([1, 0, 0]), above(0));\nlet b = coat(wet(0.5), below(0));\ncoat(a) coat(shade(0.5), above(1)) coat(b) ball(1);\n");
	check(woreCoats(stacked.scene.leaves[0], { 1, 2, 0 }), "bound and written coats stack innermost first");
	const SceneFile bumped = coatedScene("bumped_coat", "let b = coat(bump(grain, 0.02, span = 0.05));\ncoat(bump(pits, 0.01)) coat(b) ball(1);\n");
	check(bumped.looks.coats.size() == 2 && bumped.looks.coats[1].mask == CoatMask::Everywhere && bumped.looks.coats[1].relief == Relief::Pits && woreCoats(bumped.scene.leaves[0], { 0, 1 }), "a bump alone still makes a coat, bound or written");
	const SceneFile passed = coatedScene("passed_coat", "let cs = [coat(wet(1), above(0)), coat(shade(1), above(0))];\nlet r = { c = cs[1] };\nfunction pick(x) {\n\treturn x;\n}\n\nshape coated(c) {\n\tcoat(c) ball(1);\n}\n\ncoated(pick(r.c));\n");
	check(woreCoats(passed.scene.leaves[0], { 1 }), "a coat in a list and a record, passed through a function and a shape, stays a coat");
	checkTiledCoats();
	checkCoatErrors();
}

std::string readingText(const std::string& lets) {
	return "let k = 1;\nfunction grown(x) {\n\treturn x + k;\n}\n\nsurface direct(r) {\n\treturn length(p) - r - k;\n}\n\nsurface inlined(r) {\n\treturn length(p) - grown(r);\n}\n\n" + lets;
}

void checkReadDistances(const Scene& scene, const char* what) {
	const Near first = {
		.got       = distanceAt(scene, { 0, 0, 0 }),
		.want      = -2,
		.tolerance = 1e-12,
		.what      = what,
	};

	checkNear(first);

	const Near second = {
		.got       = distanceAt(scene, { 10, 0, 0 }),
		.want      = -3,
		.tolerance = 1e-12,
		.what      = what,
	};

	checkNear(second);
}

void checkTopLevelReads() {
	const Scene direct = sceneOrExit(writeScene("read_direct", readingText("direct(1);\nlet k = 2;\nat([10, 0, 0]) direct(1);\n").c_str())).scene;
	check(direct.programs.size() == 2, "a surface that read a top-level binding compiles again when the binding changes");
	checkReadDistances(direct, "each program of a surface keeps the top-level value it read");
	const Scene inlined = sceneOrExit(writeScene("read_inlined", readingText("inlined(1);\nlet k = 2;\nat([10, 0, 0]) inlined(1);\n").c_str())).scene;
	check(inlined.programs.size() == 2, "a surface whose inlined function read a top-level binding compiles again when the binding changes");
	checkReadDistances(inlined, "each program keeps the top-level value its inlined function read");
	const Scene same = sceneOrExit(writeScene("read_same", readingText("direct(1);\nlet k = 1;\nlet j = 5;\ninlined(1);\nat([10, 0, 0]) direct(2);\nat([20, 0, 0]) inlined(3);\n").c_str())).scene;
	check(same.programs.size() == 2, "a surface whose top-level reads hold the same values shares its program");
}

void checkTiledReads() {
	const std::string text   = readingText("surface placed(r) {\n\treturn length(p) - r - k - tile_origin.x;\n}\n\ntiles(step = 4) {\n\tplaced(1);\n\tlet k = 2;\n\tat([2, 0, 0]) placed(1);\n\tat([3, 0, 0]) direct(1);\n}\n");
	const LoadedScene loaded = loadScene(writeScene("read_tiled", text.c_str()), 0);
	const TileContent tile   = stillTile(loaded.file.scene, tilePlace(1, 0, 0));
	check(tile.error.empty() && tile.scene.programs.size() == 3 && tile.reads.contains("k"), "a tile compiles a surface again when a top-level binding it read changes in the tile");
	check(distanceAt(tile.scene, { 4, 0, 0 }) == -6 && distanceAt(tile.scene, { 6, 0, 0 }) == -7, "each program in a tile keeps the top-level value and the tile origin it read");
	const TileContent other = stillTile(loaded.file.scene, tilePlace(2, 0, 0));
	check(distanceAt(other.scene, { 8, 0, 0 }) == -10, "another tile compiles its programs with its own origin");
}

bool holdsConstant(const Program& program, double value) {
	return std::any_of(program.steps.begin(), program.steps.end(), [&](const Step& step) {
		return step.opcode == Opcode::Constant && step.value == value;
	});
}

void checkPatternReads() {
	const SceneFile file = sceneOrExit(writeScene("read_pattern", ballText("let k = 0.25;\nsurface stripes(w) {\n\treturn p.x * k * w;\n}\n\nlet a = matte(ramp(stripes(1), #000000, #FFFFFF));\nlet k = 0.75;\nlet b = matte(ramp(stripes(1), #000000, #FFFFFF));\npaint(a) ball(1);\npaint(b) at([3, 0, 0]) ball(1);\n").c_str()));
	const std::int32_t a = file.looks.materials.at(0).albedo.program;
	const std::int32_t b = file.looks.materials.at(1).albedo.program;
	check(a != b && holdsConstant(file.scene.programs.at(std::size_t(a)), 0.25) && holdsConstant(file.scene.programs.at(std::size_t(b)), 0.75), "a pattern that read a top-level binding compiles again when the binding changes");
}

void checkReads() {
	checkTopLevelReads();
	checkTiledReads();
	checkPatternReads();
}

using Waypoints = std::vector<std::array<double, 3>>;

Value pointsValue(const Waypoints& points, bool raised) {
	std::vector<Value> items;
	for (const std::array<double, 3>& point : points) {
		std::vector<Value> numbers = { numberValue(point[0]), numberValue(point[1]) };
		if (raised) {
			numbers.push_back(numberValue(point[2]));
		}
		items.push_back(listValue(numbers));
	}
	return listValue(items);
}

std::shared_ptr<const Network> testNetwork(const Value& lines, double spacing) {
	const NetworkSource source = {
		.lines    = lines,
		.spacing  = spacing,
		.key      = 7,
		.location = Location { nullptr, 0, 0 },
	};

	return builtNetwork(source);
}

Waypoints wanderingLine(std::mt19937_64& random, int count) {
	std::uniform_real_distribution<double> turn(-0.8, 0.8);
	std::uniform_real_distribution<double> start(-2000, 2000);
	std::uniform_real_distribution<double> step(20, 300);
	Waypoints points = { { start(random), start(random), 0 } };
	double heading   = turn(random) * 4;
	for (int i = 1; i < count; ++i) {
		heading += turn(random);
		const double length = step(random);
		points.push_back({ points.back()[0] + length * std::cos(heading), points.back()[1] + length * std::sin(heading), points.back()[2] + 0.1 * length * turn(random) });
	}
	return points;
}

Waypoints loopLine() {
	Waypoints points;
	for (int i = 0; i <= 12; ++i) {
		const double angle = 2 * std::acos(-1.0) * (i % 12) / 12;
		points.push_back({ 300 + 200 * std::cos(angle), -100 + 200 * std::sin(angle), 0 });
	}
	return points;
}

Value networkField(const Value& record, const std::string& name) {
	const auto found = std::find(record.fields->begin(), record.fields->end(), name);
	return record.items.at(std::size_t(found - record.fields->begin()));
}

struct SegmentPoint {
	double distance;
	double along;
	double side;
	math::Vector at;
};

SegmentPoint bruteSegmentPoint(const NetworkSegment& segment, double x, double y) {
	const math::Vector run = segment.b - segment.a;
	const double square    = run.x * run.x + run.y * run.y;
	const double t         = std::clamp(((x - segment.a.x) * run.x + (y - segment.a.y) * run.y) / square, 0.0, 1.0);
	const math::Vector at  = segment.a + run * t;
	const double turn      = run.x * (y - at.y) - run.y * (x - at.x);
	return SegmentPoint {
		.distance = std::sqrt((x - at.x) * (x - at.x) + (y - at.y) * (y - at.y)),
		.along    = segment.along + t * math::length(run),
		.side     = turn > 0 ? 1.0 : turn < 0 ? -1.0
											  : 0.0,
		.at       = at,
	};
}

double bruteDistance(const Network& network, double x, double y) {
	double best = std::numeric_limits<double>::infinity();
	for (const NetworkSegment& segment : network.segments) {
		best = std::min(best, bruteSegmentPoint(segment, x, y).distance);
	}
	return best;
}

bool matchesSegment(const NetworkSegment& segment, const Value& got, const std::array<double, 3>& query) {
	const SegmentPoint want = bruteSegmentPoint(segment, query[0], query[1]);
	const Value at          = networkField(got, "at");
	const bool placed       = std::abs(want.along - networkField(got, "along").number) < 1e-9 && networkField(got, "side").number == want.side;
	const bool raised       = at.items.size() == 2 || std::abs(at.items[2].number - want.at.z) < 1e-9;
	return std::abs(want.distance - query[2]) < 1e-9 && networkField(got, "line").number == segment.line && placed && raised;
}

bool nearestHolds(const Network& network, const std::array<double, 2>& q) {
	const NetworkQuery query = {
		.x     = q[0],
		.y     = q[1],
		.reach = std::numeric_limits<double>::infinity(),
	};

	const Value got     = nearestValue(network, query);
	const double best   = bruteDistance(network, q[0], q[1]);
	const bool distance = std::abs(networkField(got, "distance").number - best) < 1e-9 && networkField(got, "found").number == 1;
	return distance && std::any_of(network.segments.begin(), network.segments.end(), [&](const NetworkSegment& segment) {
			   return matchesSegment(segment, got, { q[0], q[1], best });
		   });
}

bool reachHolds(const Network& network, const std::array<double, 2>& q) {
	const NetworkQuery query = {
		.x     = q[0],
		.y     = q[1],
		.reach = 20,
	};

	const Value got  = nearestValue(network, query);
	const bool found = bruteDistance(network, q[0], q[1]) <= 20;
	return networkField(got, "found").number == (found ? 1 : 0) && (found || (networkField(got, "distance").number == 20 && networkField(got, "line").number == -1));
}

bool bruteMeets(const NetworkSegment& segment, const NetworkBox& box) {
	const bool overlaps = std::max(segment.a.x, segment.b.x) >= box.lower[0] && std::min(segment.a.x, segment.b.x) <= box.upper[0] && std::max(segment.a.y, segment.b.y) >= box.lower[1] && std::min(segment.a.y, segment.b.y) <= box.upper[1];
	int above           = 0;
	int below           = 0;
	for (int corner = 0; corner < 4; ++corner) {
		const double x    = corner & 1 ? box.upper[0] : box.lower[0];
		const double y    = corner & 2 ? box.upper[1] : box.lower[1];
		const double turn = (segment.b.x - segment.a.x) * (y - segment.a.y) - (segment.b.y - segment.a.y) * (x - segment.a.x);
		above += turn > 0 ? 1 : 0;
		below += turn < 0 ? 1 : 0;
	}
	return overlaps && above < 4 && below < 4;
}

bool withinHolds(const Network& network, const NetworkBox& box) {
	std::vector<std::uint32_t> want;
	for (std::uint32_t i = 0; i < network.segments.size(); ++i) {
		if (bruteMeets(network.segments[i], box)) {
			want.push_back(i);
		}
	}
	const Value got = withinValue(network, box);
	bool same       = segmentsWithin(network, box) == want && got.items.size() == want.size();
	for (std::size_t i = 0; same && i < want.size(); ++i) {
		const NetworkSegment& segment = network.segments[want[i]];
		same                          = networkField(got.items[i], "line").number == segment.line && networkField(got.items[i], "along").number == segment.along && networkField(got.items[i], "a").items[0].number == segment.a.x;
	}
	return same;
}

bool networkAgreesWithBrute(const Network& network, std::mt19937_64& random) {
	const NetworkGrid& grid = network.grid;
	std::uniform_real_distribution<double> xs(grid.origin[0] - 300, grid.origin[0] + grid.cell * double(grid.width) + 300);
	std::uniform_real_distribution<double> ys(grid.origin[1] - 300, grid.origin[1] + grid.cell * double(grid.height) + 300);
	std::uniform_real_distribution<double> sizes(0, 64);
	bool agrees = true;
	for (int i = 0; i < 300 && agrees; ++i) {
		const std::array<double, 2> q = { xs(random), ys(random) };
		const double size             = i % 10 == 0 ? 0 : sizes(random);

		const NetworkBox box = {
			.lower = { q[0], q[1] },
			.upper = { q[0] + size, q[1] + 64 - size },
		};

		agrees = nearestHolds(network, q) && reachHolds(network, q) && withinHolds(network, box);
	}
	return agrees;
}

bool passesWaypoints(const Network& network, const Waypoints& points, std::uint32_t line) {
	return std::all_of(points.begin(), points.end(), [&](const std::array<double, 3>& point) {
		return std::any_of(network.segments.begin(), network.segments.end(), [&](const NetworkSegment& segment) {
			const bool start = segment.a.x == point[0] && segment.a.y == point[1];
			const bool end   = segment.b.x == point[0] && segment.b.y == point[1];
			return segment.line == line && (start || end);
		});
	});
}

bool spacedNear(const Network& network, double spacing) {
	return std::all_of(network.segments.begin(), network.segments.end(), [&](const NetworkSegment& segment) {
		const double length = math::length(segment.b - segment.a);
		return length > 0.6 * spacing && length < 1.5 * spacing;
	});
}

bool sameSegments(const Network& a, const Network& b) {
	bool same = a.segments.size() == b.segments.size() && a.key == b.key;
	for (std::size_t i = 0; same && i < a.segments.size(); ++i) {
		const NetworkSegment& s = a.segments[i];
		const NetworkSegment& t = b.segments[i];
		same                    = s.a.x == t.a.x && s.a.y == t.a.y && s.a.z == t.a.z && s.b.x == t.b.x && s.b.y == t.b.y && s.along == t.along && s.line == t.line;
	}
	return same;
}

Value recordLines(const std::vector<Waypoints>& lines) {
	std::vector<Value> items;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		items.push_back(recordValue({ "points", "width", "kind" }, { pointsValue(lines[i], true), numberValue(4 + double(i)), numberValue(double(i % 2)) }));
	}
	return listValue(items);
}

void checkNetworkQueries() {
	std::mt19937_64 random(5);
	const std::vector<Waypoints> wandering      = { wanderingLine(random, 30), wanderingLine(random, 12), wanderingLine(random, 2) };
	const Value flatLines                       = listValue({ pointsValue(wandering[0], false), pointsValue(wandering[1], false), pointsValue(wandering[2], false) });
	const std::shared_ptr<const Network> flat   = testNetwork(flatLines, 8);
	const std::shared_ptr<const Network> loop   = testNetwork(listValue({ pointsValue(loopLine(), false) }), 5);
	const std::shared_ptr<const Network> raised = testNetwork(recordLines(wandering), 12);
	check(flat && loop && raised && raised->dimensions == 3 && networkField(raised->fields[1], "width").number == 5 && raised->fields[1].items.size() == 2, "networks build from lists of points and from records, whose other fields they keep");
	check(networkAgreesWithBrute(*flat, random), "nearest and within on a network of flat lines agree with a scan of every segment");
	check(networkAgreesWithBrute(*loop, random), "nearest and within on a closed loop agree with a scan of every segment");
	check(networkAgreesWithBrute(*raised, random), "nearest and within on lines with heights agree with a scan of every segment, the height interpolated");
	check(passesWaypoints(*flat, wandering[0], 0) && passesWaypoints(*raised, wandering[1], 1) && passesWaypoints(*loop, loopLine(), 0), "the resampled curve passes through every waypoint");
	check(spacedNear(*flat, 8) && spacedNear(*raised, 12), "the resampled points stand about the spacing apart");
	check(sameSegments(*flat, *testNetwork(flatLines, 8)), "the same lines build the same network");
}

std::string networkText(const std::string& spacing) {
	return "let ROADS = network([\n\t{ points = [[0, 0], [100, 0], [100, 100]], width = 6 },\n\t{ points = [[0, 50], [-80, 60]], width = 4 },\n], spacing = " + spacing + ");\n";
}

void checkTiledNetwork() {
	const std::string body   = networkText("8") + "let top = nearest(ROADS, [50, 10]);\nat([top.distance, top.along, count(within(ROADS, [0, 0], [64, 64]))]) ball(top.fields.width);\n\ntiles(step = 64) {\n\tlet n = nearest(ROADS, tile_origin.xy + [50, 10]);\n\tat([n.distance, n.along, count(within(ROADS, tile_origin.xy, tile_origin.xy + [64, 64]))]) ball(n.fields.width);\n}\n";
	const std::string path   = writeScene("tiled_network", ballText(body).c_str());
	const LoadedScene loaded = loadScene(path, 0);
	const TileContent tile   = stillTile(loaded.file.scene, tilePlace(0, 0, 0));
	const double* top        = loaded.file.scene.leaves.at(0).placement.origin;
	const double* tiled      = tile.scene.leaves.at(0).placement.origin;
	check(tile.error.empty() && tiled[0] == top[0] && tiled[1] == top[1] && tiled[2] == top[2] && top[2] > 0, "a tile reading a network gets the answers the top level gets");
	check(tile.reads.contains("ROADS"), "a tile notes the network it read");
	const std::uint64_t key = loaded.file.scene.tiles[0].bindings.at("ROADS");
	check(key == loadScene(path, 0).file.scene.tiles[0].bindings.at("ROADS"), "a network's key is the same at every load");
	const std::string spaced = writeScene("spaced_network", ballText(networkText("4") + "tiles(step = 64) {\n\tball(1);\n}\n").c_str());
	check(key != loadScene(spaced, 0).file.scene.tiles[0].bindings.at("ROADS"), "a network of another spacing has another key");
}

std::string networkError(const char* name, const std::string& lines) {
	return loadError(name, "let r = network(" + lines + ");\nball(1);\n");
}

void checkNetworkErrors() {
	check(networkError("short_line", "[[[0, 0]]]") == ":5:9: a line has 2 points or more", "a line of one point is an error");
	check(networkError("mixed_lines", "[[[0, 0], [1, 0]], { points = [[0, 0], [1, 1]] }]") == ":5:17: a list that mixes records and other kinds", "lines of mixed forms are an error");
	check(networkError("pointless_line", "[{ width = 1 }]") == ":5:9: a line's record holds its points in 'points'", "a line record without points is an error");
	check(networkError("nested_point", "[[[0, 0], [1, [2, 3]]]]") == ":5:9: a line's points are [x, y] or [x, y, z]", "a point that is not numbers is an error");
	check(networkError("flat_point", "[[[0, 0], [1]]]") == ":5:9: a line's points are [x, y] or [x, y, z]", "a point of one number is an error");
	check(networkError("mixed_points", "[[[0, 0], [1, 0, 0]]]") == ":5:9: a network's points all have 2 numbers or all 3", "points of 2 and 3 numbers in one network are an error");
	check(networkError("doubled_point", "[[[0, 0], [0, 0], [1, 1]]]") == ":5:9: a line's points in a row differ", "a point repeated in a row is an error");
	check(networkError("unspaced", "[[[0, 0], [1, 0]]], spacing = 0") == ":5:9: a network's spacing is above 0", "a spacing of 0 is an error");
	check(networkError("listless", "1") == ":5:9: network takes a list of lines and a spacing", "network takes a list");
	check(loadError("surface_network", "let r = network([[[0, 0], [1, 0]]]);\nball(r);\n") == ":6:1: a surface takes no network", "a network passed to a surface is an error");
	check(loadError("nearest_number", "at([nearest(1, [0, 0]).distance, 0, 0]) ball(1);\n") == ":5:5: nearest takes a network, a point of 2 or 3 numbers and a reach", "nearest takes a network");
	check(loadError("within_corner", "let r = network([[[0, 0], [1, 0]]]);\nat([count(within(r, [0, 0], 1)), 0, 0]) ball(1);\n") == ":6:11: within takes a network and two corners of 2 or 3 numbers", "within takes two corners");
}

void checkNetworks() {
	checkNetworkQueries();
	checkTiledNetwork();
	checkNetworkErrors();
}

std::string eyeText(const std::string& body) {
	return ballText("camera {\n\teye    = [130, -30, 7.5];\n\ttarget = [0, 0, 0];\n}\n\n" + body);
}

void checkBandErrors() {
	check(loadError("falling_band", "camera {\n\teye = [1, 2, 3];\n}\n\nat([eye_band([0, 0], [20, 10], 1), 0, 0]) ball(1);\n") == ":9:5: eye_band takes a point of 2 or 3 numbers, a list of rising radii and a step above 0", "eye_band's radii rise");
	check(loadError("flat_band", "camera {\n\teye = [1, 2, 3];\n}\n\nat([eye_band([0, 0], [10], 0), 0, 0]) ball(1);\n") == ":9:5: eye_band takes a point of 2 or 3 numbers, a list of rising radii and a step above 0", "eye_band's step is above 0");
	check(loadError("early_band", "at([eye_band([0, 0], [10], 1), 0, 0]) ball(1);\n\ncamera {\n\teye = [1, 2, 3];\n}\n") == ":5:5: eye_band reads the camera, and no camera block comes before it", "eye_band at the top level before the camera block is an error");
	check(loadError("surface_band", "camera {\n\teye = [1, 2, 3];\n}\n\nsurface near(r) {\n\treturn p.x - eye_band([0, 0], [10], 1) - r;\n}\n\nnear(1);\n") == ":10:15: eye_band is not known in a surface", "eye_band in a surface is an error");
}

void checkBand() {
	check(placedBy("top_band", "camera {\n\teye    = [130, -30, 7.5];\n\ttarget = [0, 0, 0];\n}\n\nat([eye_band([130, 0], [10, 20], 10) + 10 * eye_band([135, -35, 35], [1, 2], 10) + 100 * eye_band([136, -25], [5, 50], 10), 0, 0]) ball(1);\n") == 22, "eye_band counts the radii that the distance from a point, in x and y or in x, y and z, to the snapped eye reaches");
	const std::string text   = ballText("tiles(step = 10) {\n\tat([eye_band(tile_origin.xy + [5, 5], [200], 100), 0, 0]) ball(1);\n}\n");
	const LoadedScene loaded = loadScene(writeScene("tiled_band", text.c_str()), 0);
	const math::Vector eye   = { 120, 0, 3 };
	const TileContent near   = evaluatedTile(loaded.file.scene, tilePlace(1, 0, 0), eye);
	const TileContent far    = evaluatedTile(loaded.file.scene, tilePlace(-6, 0, 0), eye);
	const std::string name   = near.reads.empty() ? std::string() : *near.reads.begin();
	check(near.error.empty() && near.scene.leaves.at(0).placement.origin[0] == 10 && far.scene.leaves.at(0).placement.origin[0] == -59, "a tile reads the band of a point by the eye it is run with");
	check(near.reads == far.reads && bandReadStep(name) == 100.0 && !eyeReadStep(name) && bandOfRead(name, eye, { 10, 0, 0 }) == 0 && bandOfRead(name, eye, { -60, 0, 0 }) == 1, "tiles note the band of the same place in them as one name, and the band follows from it, the eye and the tile's corner");
	checkBandErrors();
}

void checkEyeErrors() {
	check(loadError("early_eye", "at(eye(10)) ball(1);\n\ncamera {\n\teye = [1, 2, 3];\n}\n") == ":5:4: eye reads the camera, and no camera block comes before it", "eye at the top level before the camera block is an error");
	check(loadError("flat_eye", "camera {\n\teye = [1, 2, 3];\n}\n\nat(eye(0)) ball(1);\n") == ":9:4: eye's step is a number above 0", "eye's step is above 0");
	check(loadError("sunk_eye", "camera {\n\teye = [1, 2, 3];\n}\n\nat(eye(-4)) ball(1);\n") == ":9:4: eye's step is a number above 0", "eye's step is not negative");
	check(loadError("bare_eye", "camera {\n\teye = [1, 2, 3];\n}\n\nat(eye()) ball(1);\n") == ":9:4: eye takes 1 arguments", "eye takes a step");
	check(loadError("surface_eye", "camera {\n\teye = [1, 2, 3];\n}\n\nsurface near(r) {\n\treturn length(p - eye(10)) - r;\n}\n\nnear(1);\n") == ":10:20: eye is not known in a surface", "eye in a surface is an error");
}

void checkEye() {
	const std::vector<std::array<double, 3>> top = placedOrigins(writeScene("top_eye", eyeText("let e = eye(100);\nat(e) ball(1);\nat(eye(0.5)) ball(1);\n").c_str()));
	check(top.size() == 2 && top[0] == std::array<double, 3> { 150, -50, 50 } && top[1] == std::array<double, 3> { 130.25, -29.75, 7.75 }, "eye at the top level gives the file's eye snapped to the middle of its grid cell, below 0 too");
	const math::Vector eye     = { -0.1, -100, 99.999 };
	const math::Vector snapped = snappedEye(eye, 100);
	check(snapped.x == -50 && snapped.y == -50 && snapped.z == 50 && snappedEye(eye, 0.25).x == -0.125, "a negative eye snaps to the middle of the cell below it, and a cell's lower edge belongs to it");
	const std::string text   = ballText("function near() {\n\treturn eye(1000);\n}\n\ntiles(step = 10) {\n\tat(eye(100) - tile_origin) ball(1);\n\tat(near() - tile_origin) ball(1);\n}\n");
	const LoadedScene loaded = loadScene(writeScene("tiled_eye", text.c_str()), 0);
	const math::Vector flown = { -130, 250, 3 };
	const TileContent tile   = evaluatedTile(loaded.file.scene, tilePlace(1, 0, 0), flown);
	const double* first      = tile.scene.leaves.at(0).placement.origin;
	const double* second     = tile.scene.leaves.at(1).placement.origin;
	check(tile.error.empty() && first[0] == -150 && first[1] == 250 && first[2] == 50 && second[0] == -500 && second[1] == 500 && second[2] == 500, "a tile and the functions it calls see the eye they are run with, without a camera block");
	check(tile.reads.contains("eye@100") && tile.reads.contains("eye@1000") && eyeReadStep("eye@100") == 100.0 && !eyeReadStep("time"), "a tile notes each step it read the eye at");
	checkEyeErrors();
}

std::size_t surfaceSteps(const char* name, const std::string& body) {
	return sceneOrExit(writeScene(name, ballText(body).c_str())).scene.programs.back().steps.size();
}

void checkShortCircuitSurfaces() {
	const std::string guarded = "let L = [2];\nlet i = -1;\nsurface s(r) {\n\treturn length(p) - r - select(i >= 0 && L[i] > p.x, 1, 0);\n}\n\ns(1);\n";
	check(surfaceSteps("guarded_surface", guarded) == surfaceSteps("plain_surface", "surface s(r) {\n\treturn length(p) - r - 0;\n}\n\ns(1);\n"), "a load-time left side of && in a surface skips the right side and leaves no steps");
	const std::string both = "surface s(r) {\n\treturn length(p) - r - select(p.x > 0 && p.y > 0, 1, 0) - select(p.x > 0 || p.y > 0, 1, 0);\n}\n\ns(1);\n";
	const Scene scene      = sceneOrExit(writeScene("register_left", ballText(both).c_str())).scene;
	check(scene.programs.back().steps.size() == 26, "a register left side compiles both sides");
	check(distanceAt(scene, { 2, 2, 0 }) == std::sqrt(8.0) - 3 && distanceAt(scene, { 2, -2, 0 }) == std::sqrt(8.0) - 2 && distanceAt(scene, { -2, -2, 0 }) == std::sqrt(8.0) - 1, "a register left side gives && and || at every point");
}

void checkShortCircuit() {
	check(placedBy("short_skipped", "let L = [3];\nlet i = -1;\nlet a = i >= 0 && L[i] > 0;\nlet b = i < 0 || L[i] > 0;\nlet c = 0 && missing(1);\nlet d = 2 || missing(1);\nat([a * 1000 + b * 100 + c * 10 + d, 0, 0]) ball(1);\n") == 101, "&& and || skip the right side at load time when the left decides");
	check(placedBy("short_reached", "let L = [3];\nlet i = 0;\nlet a = i >= 0 && L[i];\nlet b = i > 0 || L[i];\nlet c = 2 && 0;\nlet d = 0 || 0;\nat([a * 1000 + b * 100 + c * 10 + d, 0, 0]) ball(1);\n") == 1100, "&& and || evaluate the right side when the left does not decide, and give 0 or 1");
	check(loadError("short_needed", "let i = 0;\nlet a = i >= 0 && missing(i);\nball(1);\n").ends_with("unknown function 'missing'"), "a right side that is reached still fails");
	check(placedBy("short_function", "let L = [{ x = 4 }];\nfunction has(i) {\n\treturn i >= 0 && L[i].x > 0;\n}\n\nshape s(i) {\n\tif i < 0 || L[i].x > 0 {\n\t\tat([has(i) + 2 * has(0), 0, 0]) ball(1);\n\t}\n}\n\ns(-1);\n") == 2, "functions and shapes skip the right side");
	const std::string tiled  = writeScene("short_tile", ballText("let L = [1];\ntiles(step = 4) {\n\tif tile.x >= 0 && L[tile.x] > 0 {\n\t\tball(1);\n\t}\n}\n").c_str());
	const LoadedScene loaded = loadScene(tiled, 0);
	const TileContent before = stillTile(loaded.file.scene, tilePlace(-1, 0, 0));
	const TileContent inside = stillTile(loaded.file.scene, tilePlace(0, 0, 0));
	check(before.error.empty() && before.scene.leaves.empty() && inside.error.empty() && inside.scene.leaves.size() == 1, "a tile body skips the right side");
	checkShortCircuitSurfaces();
}

void checkGrowErrors() {
	check(loadError("append_fields", "let L = [{ a = 1 }];\nlet L = append(L, { b = 1 });\nball(1);\n") == ":6:9: a list that mixes records of different fields", "appending a record of other fields is an error");
	check(loadError("append_kinds", "let L = [1];\nlet M = append(L, { a = 1 });\nball(1);\n") == ":6:9: a list that mixes records and other kinds", "appending a record to a list of numbers is an error");
	check(loadError("join_kinds", "let J = join([{ a = 1 }], [], [2]);\nball(1);\n") == ":5:9: a list that mixes records and other kinds", "joining records and numbers is an error");
	check(loadError("append_number", "let L = append(1, 2);\nball(1);\n") == ":5:9: append takes a list and a value", "appending to a number is an error");
	check(loadError("append_alone", "let L = [];\nlet L = append(L);\nball(1);\n") == ":6:9: append takes 2 arguments", "append takes a list and one value");
	check(loadError("join_number", "let J = join([1], 2);\nball(1);\n") == ":5:9: join takes lists", "join takes lists only");
	check(loadError("join_nothing", "let J = join();\nball(1);\n") == ":5:9: join takes lists", "join takes at least one list");
	check(loadError("append_branch", "surface s(r) {\n\tlet L = [r];\n\tif p.x > 0 {\n\t\tlet L = append(L, 1);\n\t}\n\treturn length(p) - L[0];\n}\n\ns(1);\n") == ":7:2: lists of different lengths", "a list grown in an if not known when the scene loads is an error");
}

double appendSeconds(const char* name, const std::string& body) {
	const std::string path                            = writeScene(name, ballText(body).c_str());
	const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
	const SceneFile file                              = sceneOrExit(path);
	const std::chrono::duration<double> elapsed       = std::chrono::steady_clock::now() - start;
	return file.scene.leaves[0].placement.origin[0] == 39998 ? elapsed.count() : 1e9;
}

void checkGrowSpeed() {
	check(appendSeconds("appended_top", "let L = [];\nfor i in 0..20000 {\n\tlet L = append(L, i * 2);\n}\nat([L[19999], 0, 0]) ball(1);\n") < 1, "20000 appends at the top level take linear time");
	check(appendSeconds("appended_function", "function evens(n) {\n\tlet L = [];\n\tfor i in 0..n {\n\t\tlet L = join(L, [i * 2]);\n\t}\n\treturn L;\n}\n\nat([evens(20000)[19999], 0, 0]) ball(1);\n") < 1, "20000 joins in a function take linear time");
}

void checkGrowing() {
	check(placedBy("appended", "let L = [];\nfor i in 0..4 {\n\tlet L = append(L, i * 2);\n}\nlet J = join([], L, [9], []);\nat([count(L) * 1000 + count(J) * 100 + J[3] * 10 + J[4], 0, 0]) ball(1);\n") == 4569, "append adds at the end, join concatenates, from the empty list");
	check(placedBy("appended_copies", "let L = [1];\nlet M = L;\nlet N = append(L, 2);\nlet L = append(L, 3);\nat([count(M) * 1000 + N[1] * 100 + L[1] * 10 + count(L), 0, 0]) ball(1);\n") == 1232, "append gives a new list and changes no other binding");
	check(placedBy("appended_records", "let L = [];\nlet L = append(L, { a = 1, b = 2 });\nlet L = append(L, { b = 3, a = 4 });\nlet J = join(L, [{ a = 5, b = 6 }]);\nat([J[1].a + J[2].b * 10 + count(J) * 100, 0, 0]) ball(1);\n") == 364, "lists of records of the same fields append and join");
	check(placedBy("appended_top_function", "let T = [1];\nfunction more() {\n\tlet T = append(T, 2);\n\treturn count(T);\n}\n\nat([more() * 10 + count(T), 0, 0]) ball(1);\n") == 21, "a function appending to a top-level list makes its own and leaves the top level");
	const std::string tiled  = writeScene("appended_tile", ballText("let B = [1];\n\ntiles(step = 4) {\n\tlet B = append(B, tile.x);\n\tlet B = join(B, [5]);\n\tat([count(B) * 10 + B[1], 0, 0]) ball(1);\n}\n").c_str());
	const LoadedScene loaded = loadScene(tiled, 0);
	const TileContent first  = stillTile(loaded.file.scene, tilePlace(1, 0, 0));
	const TileContent second = stillTile(loaded.file.scene, tilePlace(2, 0, 0));
	check(first.error.empty() && second.error.empty() && first.scene.leaves[0].placement.origin[0] == 4 + 31 && second.scene.leaves[0].placement.origin[0] == 8 + 32 && first.reads.contains("B"), "a tile appends to and joins a top-level list as its own copy");
	const Scene scene = sceneOrExit(writeScene("appended_surface", ballText("surface s(r) {\n\tlet L = append([r], r * 2);\n\treturn length(p) - L[1];\n}\n\ns(1);\n").c_str())).scene;

	const Near doubled = {
		.got       = distanceAt(scene, { 0, 0, 0 }),
		.want      = -2,
		.tolerance = 1e-12,
		.what      = "a surface appends a register to a list",
	};

	checkNear(doubled);
	checkGrowErrors();
	checkGrowSpeed();
}

void checkShadowing() {
	check(loadError("builtin_function", "function sqrt(x) {\n\treturn x;\n}\n\nball(1);\n") == ":5:1: sqrt is a built-in and cannot be defined", "a function named like a built-in is an error");
	check(loadError("builtin_shape", "shape plastic() {\n\tball(1);\n}\n\nplastic();\n") == ":5:1: plastic is a built-in and cannot be defined", "a shape named like a built-in is an error");
	check(loadError("builtin_let", "let max = 2;\nball(1);\n") == ":5:1: max is a built-in and cannot be bound", "a top-level binding named like a built-in is an error");
	check(loadError("kind_shape", "let door = matte([1, 0, 0]);\nshape s() {\n\tlet door = 1.5;\n}\n\ns();\nball(1);\n") == ":7:2: door holds a material and cannot be bound to a number", "a let that turns a material into a number is an error");
	check(loadError("kind_top", "let a = 1;\nlet a = [1];\nball(1);\n") == ":6:1: a holds a number and cannot be bound to a list", "a let that turns a number into a list is an error");
	check(placedBy("kind_same", "let d = 1;\nlet d = d + 1;\nlet L = [1];\nlet L = [[1, 2]];\nlet door = matte([1, 0, 0]);\nfunction f() {\n\tlet door = 3;\n\treturn door;\n}\n\nat([d * 10 + f() + count(L[0]) * 100, 0, 0]) ball(1);\n") == 223, "a let of the same kind, and a function's own name, are fine");
}

void checkSelectValues() {
	const std::string text = "let a = matte([1, 0, 0]);\nlet b = matte([0, 1, 0]);\nshape s(w) {\n\tpaint(select(w > 5, b, a)) ball(1);\n}\n\ns(7);\ns(2);\n";
	const Scene scene      = sceneOrExit(writeScene("select_material", ballText(text).c_str())).scene;
	check(scene.leaves[0].material == 1 && scene.leaves[1].material == 0, "select picks a material by a load-time condition");
	check(placedBy("select_values", "let L = select(1, [1, 2, 3], [4]);\nlet r = select(0, { x = 1 }, { x = 5 });\nat([count(L) * 10 + r.x, 0, 0]) ball(1);\n") == 35, "select picks lists of other lengths and records by a load-time condition");
	check(loadError("select_point", "let a = matte([1, 0, 0]);\nsurface s(r) {\n\tlet m = select(p.x > 0, a, a);\n\treturn length(p) - r;\n}\n\ns(1);\n") == ":7:10: select picks a material only by a condition known when the scene loads", "select of materials by a point condition is an error");
	check(loadError("select_point_list", "let a = matte([1, 0, 0]);\nsurface s(r) {\n\tlet m = select(p.x > 0, [1], [a]);\n\treturn length(p) - r;\n}\n\ns(1);\n") == ":7:10: select picks a material only by a condition known when the scene loads", "select of a list holding a material by a point condition is an error");
}

double stopsInSurface(const std::string& stops, const std::string& call, double x) {
	const std::string text = ballText("let s = " + stops + ";\nsurface w(r) {\n\treturn length(p) - r + " + call + ";\n}\n\nw(1);\n");
	const Scene scene      = sceneOrExit(writeScene("stops_surface", text.c_str())).scene;
	return distanceAt(scene, { x, 0, 0 }) - std::abs(x) + 1;
}

bool stopsMatch(const std::string& stops, const std::string& name, const std::string& index) {
	for (const double x : { -1.0, 0.0, 0.25, 1.0, 1.5, 2.0, 2.99, 3.0, 3.5, 4.0, 9.0 }) {
		const double want = placedX("stops_loaded", stops, name + "(s, " + std::to_string(x) + ")" + index);
		if (std::abs(stopsInSurface(stops, name + "(s, p.x)" + index, x) - want) > 1e-12) {
			return false;
		}
	}
	return true;
}

void checkStopsInSurfaces() {
	const std::string jumped = "[[0, 0], [1, 2], [3, 4], [3, 10], [4, 8]]";
	check(stopsMatch(jumped, "linear", "") && stopsMatch(jumped, "smooth", ""), "linear and smooth by a point argument match the load-time values, jumps included");
	const std::string vector = "[[0, 1, 2], [2, 3, 6], [4, -1, 0]]";
	check(stopsMatch(vector, "linear", "[1]") && stopsMatch(vector, "smooth", "[1]") && stopsMatch("[[1, 5]]", "smooth", ""), "linear and smooth by a point argument give vectors, and one stop holds everywhere");
	check(loadError("stops_point", "surface s(r) {\n\treturn length(p) - r + linear([[0, p.x], [1, 2]], p.y);\n}\n\ns(1);\n") == ":6:25: expected a number known when the scene loads", "stops that depend on the point are an error");
}

std::string printedBy(const char* name, const std::string& body, bool tiled) {
	const std::string path = writeScene(name, ballText(body).c_str());
	const std::string log  = path + ".printed";
	std::fflush(stderr);
	const int saved  = dup(2);
	FILE* const file = std::fopen(log.c_str(), "w");
	dup2(fileno(file), 2);
	const LoadedScene loaded = loadScene(path, 0);
	const std::string error  = tiled && loaded.error.empty() ? stillTile(loaded.file.scene, tilePlace(1, 0, 0)).error : loaded.error;
	std::fflush(stderr);
	dup2(saved, 2);
	close(saved);
	std::fclose(file);
	std::ostringstream printed;
	printed << std::ifstream(log).rdbuf();
	std::string out = error.empty() ? printed.str() : error;
	for (std::size_t at = out.find(path); at != std::string::npos; at = out.find(path)) {
		out.erase(at, path.size());
	}
	return out;
}

void checkPrint() {
	const std::string values = "let a = matte([1, 0, 0]);\nfunction f(x) {\n\tprint(x * 2);\n\treturn x;\n}\n\nfor i in 0..3 {\n\tprint(i, [i, 0.5], { x = f(i) }, filmic, a);\n}\nball(1);\n";
	check(printedBy("print_values", values, false) == ":7: 0\n:12: 0 [0, 0.5] {x = 0} filmic (a material)\n", "print writes file:line and its values once per call site");
	check(printedBy("print_tile", "tiles(step = 4) {\n\tfor i in 0..2 {\n\t\tprint(tile.x + i);\n\t}\n\tball(1);\n}\n", true) == ":7: 1\n", "print in a tile writes once per tile run");
	check(printedBy("print_surface", "function f(x) {\n\tprint(x);\n\treturn x;\n}\n\nsurface s(r) {\n\treturn length(p) - f(r);\n}\n\ns(1);\n", false) == ":6:2: a surface does not print", "print inside a surface is an error");
	check(printedBy("print_expression", "let a = print(1);\nball(1);\n", false) == ":5:9: print stands alone as a statement", "print as a value is an error");
	check(printedBy("print_named", "print(x = 1);\nball(1);\n", false) == ":5:1: print takes values, not named ones", "print with a named argument is an error");
}

void checkLanguage() {
	checkCompiledFn();
	checkBlocks();
	checkPlacement();
	checkJoins();
	checkRepeat();
	checkUnionParts();
	checkRules();
	checkAbsoluteImport();
	checkRenderCaps();
	checkLoadErrors();
	checkTime();
	checkClip();
	checkStops();
	checkCompared();
	checkTiles();
	checkTileNames();
	checkFunctions();
	checkNestedArguments();
	checkComprehensions();
	checkNoise();
	checkEarlyReturn();
	checkRecords();
	checkReadsInPlace();
	checkBoundCoats();
	checkReads();
	checkNetworks();
	checkEye();
	checkBand();
	checkShortCircuit();
	checkGrowing();
	checkShadowing();
	checkSelectValues();
	checkStopsInSurfaces();
	checkPrint();
}

}
