#include "check.h"
#include "plugins/transport/paths/paths.h"
#include "plugins/transport/paths/strategy.h"
#include "sdf/compare.h"
#include "sdf/scene.h"
#include "syntax/evaluator.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
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
	check(tiledError("tiled_coat", "\tcoat(shade(0.5), above(1)) ball(1);\n") == ":6:2: tiles may not make a coat", "tiles may not make a coat");
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

void checkTiles() {
	const std::string text   = ballText("let n = 1;\n\nshape counted() {\n\tlet n = n + 10;\n}\n\ntiles(step = [4, 8, 16]) {\n\tif tile.z == 0 {\n\t\tlet n = n + tile.x;\n\t\tcounted();\n\t\tat([n, 1, 2]) ball(0.5);\n\t}\n}\n\nlet after = 3;\n");
	const std::string path   = writeScene("tiles", text.c_str());
	const LoadedScene loaded = loadScene(path, 0);
	const Scene& scene       = loaded.file.scene;
	check(loaded.error.empty() && scene.objects.empty() && scene.tiles.size() == 1 && scene.tiles[0].step[1] == 8 && loaded.file.timed, "a scene of tiles alone loads, holds its block and is evaluated again at each time");
	const TileContent second = evaluatedTile(scene, tilePlace(2, 1, 0));
	const TileContent first  = evaluatedTile(scene, tilePlace(1, 1, 0));
	const double* moved      = second.scene.leaves[0].placement.origin;
	check(second.error.empty() && second.scene.objectPrograms.size() == 1 && moved[0] == 8 + 13 && moved[1] == 9 && moved[2] == 2, "a tile's shapes are moved by its index times the step, from the bindings where the block stands");
	check(first.scene.leaves[0].placement.origin[0] == 4 + 12 && evaluatedTile(scene, tilePlace(2, 1, 0)).scene.leaves[0].placement.origin[0] == moved[0], "a tile's changes to the bindings are its own");
	check(evaluatedTile(scene, tilePlace(0, 0, 1)).scene.objects.empty() && first.reads.contains("n") && !first.reads.contains("tile"), "a tile may make nothing, and notes the top-level names it read");
	const std::string late = writeScene("late_tiles", ballText("tiles(step = 1) {\n\tat([after, 0, 0]) ball(1);\n}\n\nlet after = 1;\n").c_str());
	check(evaluatedTile(loadScene(late, 0).file.scene, tilePlace(0, 0, 0)).error == late + ":6:2: unknown name 'after'", "a tile sees no binding made after its block");
	const std::string shaped = writeScene("shaped_material", ballText("shape red() {\n\tpaint(matte([1, 0, 0])) ball(1);\n}\n\ntiles(step = 1) {\n\tred();\n}\n").c_str());
	check(evaluatedTile(loadScene(shaped, 0).file.scene, tilePlace(0, 0, 0)).error == shaped + ":6:8: tiles may not make a material", "a shape a tile calls may not make a material");
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

void checkFunctions() {
	check(placedBy("returned", "function twice(a) {\n\tlet b = a * 2;\n\treturn b + 1;\n}\n\nat([twice(3), 0, 0]) ball(1);\n") == 7, "a function runs at load time and returns a value");
	check(placedBy("threaded", "function next(s) {\n\treturn s + 1;\n}\n\nlet s = 1;\nlet s = next(s);\nlet s = next(s);\nat([s, 0, 0]) ball(1);\n") == 3, "a function's result threads a value through the file");
	const Scene inlined = sceneOrExit(writeScene("inlined", ballText("function square(x) {\n\treturn x * x;\n}\n\nsurface blob(r) {\n\treturn length(p) - square(r);\n}\n\nblob(2);\n").c_str())).scene;

	const Near squared = {
		.got       = distanceAt(inlined, { 0, 0, 0 }),
		.want      = -4,
		.tolerance = 1e-12,
		.what      = "a function called in a surface is inlined",
	};

	checkNear(squared);
	check(loadError("global", "let k = 2;\nfunction f(a) {\n\treturn a + k;\n}\n\nat([f(1), 0, 0]) ball(1);\n") == ":7:13: a function sees only its parameters: 'k'", "a function sees no top-level binding");
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

void checkLanguage() {
	checkCompiledFn();
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
	checkFunctions();
	checkComprehensions();
}

}
