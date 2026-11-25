#include "check.h"
#include "core/frame.h"
#include "plugins/build/face_slabs/face_slabs.h"
#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/classify.h"
#include "plugins/geometry/tree/cut.h"
#include "plugins/geometry/tree/grow.h"
#include "plugins/geometry/tree/lazy.h"
#include "plugins/geometry/tree/pack.h"
#include "plugins/geometry/tree/pass.h"
#include "plugins/geometry/tree/patch.h"
#include "plugins/geometry/tree/reach.h"
#include "plugins/geometry/tree/walk.h"
#include "plugins/transport/paths/paths.h"
#include "sdf/compare.h"
#include "sdf/scene.h"
#include "syntax/evaluator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kodanuki {

using Spot = std::array<double, 3>;

struct Sampler {
	std::mt19937 random;
	std::vector<Spot> centres;
	double spread;
};

struct Level {
	const Program& whole;
	const Cut& cut;
	const Box& box;
};

struct Tally {
	double worst;
	std::size_t empty;
	std::size_t filled;
	std::size_t wrong;
	std::size_t before;
	std::size_t after;
};

Spot leafOrigin(const Leaf& leaf) {
	return { leaf.placement.origin[0], leaf.placement.origin[1], leaf.placement.origin[2] };
}

std::vector<Spot> nodeCentres(const Scene& scene, std::uint32_t index) {
	const Node& node = scene.nodes[index];
	if (node.join == Join::Leaf) {
		return { leafOrigin(scene.leaves[node.leaf]) };
	}
	std::vector<Spot> centres;
	for (const std::uint32_t child : node.children) {
		const std::vector<Spot> more = nodeCentres(scene, child);
		centres.insert(centres.end(), more.begin(), more.end());
	}
	return centres;
}

Sampler samplerFor(const std::vector<Spot>& centres, double spread) {
	return Sampler {
		.random  = std::mt19937(11),
		.centres = centres,
		.spread  = spread,
	};
}

std::vector<Spot> sceneCentres(const Scene& scene) {
	std::vector<Spot> centres;
	for (const Leaf& leaf : scene.leaves) {
		centres.push_back(leafOrigin(leaf));
	}
	return centres;
}

Spot nearSpot(Sampler& sampler) {
	std::uniform_int_distribution<std::size_t> pick(0, sampler.centres.size() - 1);
	std::uniform_real_distribution<double> offset(-sampler.spread, sampler.spread);
	const Spot& centre = sampler.centres[pick(sampler.random)];
	return { centre[0] + offset(sampler.random), centre[1] + offset(sampler.random), centre[2] + offset(sampler.random) };
}

double insideSpan(Sampler& sampler, const math::Interval& span) {
	std::uniform_real_distribution<double> unit(0, 1);
	return span.lower + (span.upper - span.lower) * unit(sampler.random);
}

Spot insideBox(Sampler& sampler, const Box& box) {
	return { insideSpan(sampler, box.axes[0]), insideSpan(sampler, box.axes[1]), insideSpan(sampler, box.axes[2]) };
}

double programAt(const Program& program, const Spot& p) {
	std::vector<double> registers;

	const Probe probe = {
		.p         = { p[0], p[1], p[2] },
		.registers = registers,
	};

	return programDistance(program, probe);
}

double nodeAt(const Scene& scene, std::uint32_t node, const Spot& p) {
	std::vector<double> registers;

	const Probe probe = {
		.p         = { p[0], p[1], p[2] },
		.registers = registers,
	};

	return nodeDistance(scene, node, probe);
}

void checkObjectPrograms(const char* path) {
	const SceneFile file = sceneOrExit(path);
	const Scene& scene   = file.scene;
	Sampler sampler      = samplerFor(sceneCentres(scene), 0.6);
	double worst         = 0;
	for (int s = 0; s < 300; ++s) {
		const Spot p = nearSpot(sampler);
		for (std::uint32_t i = 0; i < scene.objects.size(); ++i) {
			worst = std::max(worst, std::abs(programAt(scene.objectPrograms[i], p) - nodeAt(scene, scene.objects[i], p)));
		}
	}

	const Near agreement = {
		.got       = worst,
		.want      = 0,
		.tolerance = 1e-9,
		.what      = path,
	};

	checkNear(agreement);
}

math::Interval cellSpan(double coordinate, double size) {
	const double lower = std::floor(coordinate / size) * size;
	return math::Interval {
		.lower = lower,
		.upper = lower + size,
	};
}

Box cellAround(const Spot& p, double size) {
	return Box {
		.axes = { cellSpan(p[0], size), cellSpan(p[1], size), cellSpan(p[2], size) },
	};
}

bool agreesWithOccupancy(Occupancy occupancy, double value) {
	switch (occupancy) {
		case Occupancy::Empty: return value > 0;
		case Occupancy::Filled: return value < 0;
		case Occupancy::Surface: return true;
	}

	return true;
}

void checkLevel(Tally& tally, Sampler& sampler, const Level& level) {
	tally.empty += level.cut.occupancy == Occupancy::Empty ? 1 : 0;
	tally.filled += level.cut.occupancy == Occupancy::Filled ? 1 : 0;
	tally.before += level.whole.steps.size();
	tally.after += level.cut.program.steps.size();
	for (int s = 0; s < 8; ++s) {
		const Spot p       = insideBox(sampler, level.box);
		const double whole = programAt(level.whole, p);
		tally.worst        = std::max(tally.worst, std::abs(programAt(level.cut.program, p) - whole));
		tally.wrong += agreesWithOccupancy(level.cut.occupancy, whole) ? 0 : 1;
	}
}

void checkCutChain(std::vector<Tally>& tallies, Sampler& sampler, const Program& whole) {
	const double sizes[3] = { 1, 0.25, 0.0625 };
	const Spot p          = nearSpot(sampler);
	Program parent        = whole;
	for (int k = 0; k < 3; ++k) {
		const Box box = cellAround(p, sizes[k]);
		const Cut cut = cutProgram(parent, box);

		const Level level = {
			.whole = whole,
			.cut   = cut,
			.box   = box,
		};

		checkLevel(tallies[k], sampler, level);
		parent = cut.program;
	}
}

Tally emptyTally() {
	return Tally {
		.worst  = 0,
		.empty  = 0,
		.filled = 0,
		.wrong  = 0,
		.before = 0,
		.after  = 0,
	};
}

void checkCuts() {
	const SceneFile file = sceneOrExit("scenes/chess-board.sdf");
	std::vector<Tally> tallies(3, emptyTally());
	for (std::uint32_t i = 0; i < file.scene.objectPrograms.size(); ++i) {
		Sampler sampler = samplerFor(nodeCentres(file.scene, file.scene.objects[i]), 0.4);
		for (int s = 0; s < 40; ++s) {
			checkCutChain(tallies, sampler, file.scene.objectPrograms[i]);
		}
	}
	for (const Tally& tally : tallies) {
		check(tally.worst <= 1e-12, "a cut program equals the whole one inside its box");
		check(tally.wrong == 0, "empty and filled cells agree with point samples");
	}
	check(tallies[2].after * 4 < tallies[2].before, "small cells cut programs to under a quarter");
	check(tallies[2].empty > 0 && tallies[2].filled > 0, "small cells are found empty and filled");
}

math::Interval pointAt(double x) {
	return math::Interval {
		.lower = x,
		.upper = x,
	};
}

Box boxAt(const Spot& p, const Spot& shift) {
	return Box {
		.axes = { pointAt(p[0] - shift[0]), pointAt(p[1] - shift[1]), pointAt(p[2] - shift[2]) },
	};
}

Box grownBox(const Box& box, const Spot& grow) {
	Box grown = box;
	for (int axis = 0; axis < 3; ++axis) {
		grown.axes[axis].lower -= grow[axis];
		grown.axes[axis].upper += grow[axis];
	}
	return grown;
}

Spot lensDirection(const Lens& lens, double u, double v) {
	Spot d = { 0, 0, 0 };
	for (int axis = 0; axis < 3; ++axis) {
		d[axis] = math::component(lens.forward, axis) + math::component(lens.right, axis) * u * lens.reach * lens.width / lens.height + math::component(lens.up, axis) * v * lens.reach;
	}
	return d;
}

void checkFocusOnView() {
	const SceneFile file = sceneOrExit(writeScene("view", R"(
		render {
			width  = 64;
			height = 40;
		}

		camera {
			eye    = [0, -10, 3];
			target = [1, 0, 0];
			fov    = 0.7;
		}

		surface sphere(r) {
			return length(p) - r;
		}

		sphere(1);
	)"));

	const Region region = {
		.lower = { -50, -50, -50 },
		.size  = { 100, 100, 100 },
	};

	const Detail detail = testDetail(file, region, defaultDetailMembers());
	const Lens lens     = lensOf(file.shot.camera, file.shot.render);
	std::mt19937 random(5);
	std::uniform_real_distribution<double> unit(-1, 1);
	std::uniform_real_distribution<double> depth(0.01, 60);
	int hidden = 0;
	for (int s = 0; s < 4000; ++s) {
		const bool corner = s % 4 == 0;
		const double u    = corner ? (unit(random) < 0 ? -1 : 1) : unit(random);
		const double v    = corner ? (unit(random) < 0 ? -1 : 1) : unit(random);
		const Spot d      = lensDirection(lens, u, v);
		const double t    = depth(random);
		const Spot p      = { lens.eye.x + d[0] * t, lens.eye.y + d[1] * t, lens.eye.z + d[2] * t };
		const Spot grow   = { 0.5 + unit(random) * 0.5, 0.5 + unit(random) * 0.5, 0.5 + unit(random) * 0.5 };
		const Spot shift  = { unit(random) * grow[0], unit(random) * grow[1], unit(random) * grow[2] };
		const Box box     = grownBox(boxAt(p, shift), grow);
		hidden += cellBeyond(detail.screen, box) ? 1 : 0;
	}
	std::printf("tree view: %d of 4000 cells on camera rays counted unseen\n", hidden);
	check(hidden == 0, "a cell any camera ray passes through keeps the seen tolerance");

	const Spot behind = { lens.eye.x - lens.forward.x * 5, lens.eye.y - lens.forward.y * 5, lens.eye.z - lens.forward.z * 5 };
	const Spot side   = lensDirection(lens, 1.3, 0);
	const Spot beside = { lens.eye.x + side[0] * 20, lens.eye.y + side[1] * 20, lens.eye.z + side[2] * 20 };
	const Spot half   = { 0.3, 0.3, 0.3 };
	const Box back    = grownBox(boxAt(behind, { 0, 0, 0 }), half);
	const Box out     = grownBox(boxAt(beside, { 0, 0, 0 }), half);
	const double seen = detail.share * footprintOver(detail.view, out);
	check(cellBeyond(detail.screen, back) && cellBeyond(detail.screen, out) && cellTolerance(detail, out) > 10 * seen, "cells behind the eye or beside the frame are unseen and held looser");
}

Program sphereProgram() {
	const SceneFile file = sceneOrExit(writeScene("patch", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		sphere(1);
	)"));

	return file.scene.objectPrograms[0];
}

Box cubeAt(const Spot& lower, double size) {
	return Box {
		.axes = { math::Interval { .lower = lower[0], .upper = lower[0] + size }, math::Interval { .lower = lower[1], .upper = lower[1] + size }, math::Interval { .lower = lower[2], .upper = lower[2] + size } },
	};
}

double patchAtWorld(const Patch& patch, const Box& box, const Spot& p) {
	double u[3];
	for (int axis = 0; axis < 3; ++axis) {
		u[axis] = (p[std::size_t(axis)] - box.axes[axis].lower) / (box.axes[axis].upper - box.axes[axis].lower);
	}
	return patchValue(patch, u);
}

void checkPatchFit() {
	const std::vector<Program> programs                      = { sphereProgram() };
	const std::vector<std::shared_ptr<const Program>> shared = { std::make_shared<const Program>(programs[0]) };
	const Box box                                            = cubeAt({ 0.55, 0.55, 0.55 }, 0.05);

	const PatchQuery query = {
		.programs = shared,
		.box      = box,
	};

	const Fitted fitted = fitPatch(query);
	std::mt19937 random(3);
	double worst             = 0;
	bool netted              = true;
	const math::Interval net = patchNet(fitted.patch);
	for (int s = 0; s < 500; ++s) {
		Sampler sampler = samplerFor({}, 0);
		sampler.random  = std::mt19937(unsigned(s));
		const Spot p    = insideBox(sampler, box);
		const double v  = patchAtWorld(fitted.patch, box, p);
		worst           = std::max(worst, std::abs(v - programAt(programs[0], p)));
		netted          = netted && v >= net.lower && v <= net.upper;
	}
	std::printf("tree patch: sphere over a 0.05 cell strays %.3g at the fit's points, %.3g at random points\n", fitted.stray, worst);
	check(fitted.patch.bent && fitted.stray < 1e-5 && worst < 2e-5, "a patch over a small cell of a sphere bends and strays little");
	check(netted, "a patch's Bernstein net holds every value it takes over its cell");

	const std::vector<std::uint32_t> words = patchWords(fitted.patch);
	const Patch read                       = readPatch(words.data());
	bool same                              = read.bent == fitted.patch.bent && read.step == fitted.patch.step && words.size() == patchWordCount(words.data());
	for (int k = 0; k < 20; ++k) {
		same = same && read.weights[k] == fitted.patch.weights[k];
	}
	check(same && words.size() == 11, "a bent patch face is eleven words and reads back the same to the bit");

	const Box beside = cubeAt({ 0.6, 0.55, 0.55 }, 0.05);

	const PatchQuery next = {
		.programs = shared,
		.box      = beside,
	};

	const Fitted neighbour = fitPatch(next);
	double gap             = 0;
	for (int s = 0; s < 100; ++s) {
		const Spot p = { 0.6, 0.55 + 0.05 * (s % 10) / 9.0, 0.55 + 0.05 * (s / 10) / 9.0 };
		gap          = std::max(gap, std::abs(patchAtWorld(fitted.patch, box, p) - patchAtWorld(neighbour.patch, beside, p)));
	}
	check(gap <= 10 * double(fitted.patch.step + neighbour.patch.step), "two patches of one size read the same field on the face they share, up to their rounding");
}

double zeroAlongW(const Patch& patch, double u, double v) {
	for (int k = 0; k < 200; ++k) {
		double low        = k / 200.0;
		double high       = (k + 1) / 200.0;
		const double a[3] = { u, v, low };
		const double b[3] = { u, v, high };
		if ((patchValue(patch, a) < 0) == (patchValue(patch, b) < 0)) {
			continue;
		}
		for (int step = 0; step < 50; ++step) {
			const double middle = (low + high) / 2;
			const double m[3]   = { u, v, middle };
			const bool below    = (patchValue(patch, m) < 0) == (patchValue(patch, a) < 0);
			low                 = below ? middle : low;
			high                = below ? high : middle;
		}
		return (low + high) / 2;
	}
	return -1;
}

void checkPatchSlab() {
	const std::vector<std::shared_ptr<const Program>> programs = { std::make_shared<const Program>(sphereProgram()) };
	const Box box                                              = cubeAt({ 0.5, 0.5, 0.55 }, 0.1);

	const PatchQuery query = {
		.programs = programs,
		.box      = box,
	};

	const Patch patch = fitPatch(query).patch;
	const Slab slab   = patchSlab(patch);
	int outside       = 0;
	int zeros         = 0;
	for (int i = 0; i <= 40; ++i) {
		for (int j = 0; j <= 40; ++j) {
			const double zero = zeroAlongW(patch, i / 40.0, j / 40.0);
			if (zero < 0) {
				continue;
			}
			const double rise = slab.slope[0] * i / 40.0 + slab.slope[1] * j / 40.0 + slab.slope[2] * zero + slab.offset;
			zeros += 1;
			outside += std::abs(rise) > slab.stray ? 1 : 0;
		}
	}
	std::printf("tree slab: %d zeros, %d outside a stray of %g (slope %g %g %g, offset %g)\n", zeros, outside, slab.stray, slab.slope[0], slab.slope[1], slab.slope[2], slab.offset);
	check(zeros > 100 && slab.stray < 0.01 && outside == 0, "every zero of a patch lies within its slab's stray of the slab's plane");
}

double quadricAt(const std::vector<float>& f, const double* u) {
	const double square = f[0] * u[0] * u[0] + f[1] * u[1] * u[1] + f[2] * u[2] * u[2];
	const double cross  = f[3] * u[0] * u[1] + f[4] * u[0] * u[2] + f[5] * u[1] * u[2];
	return square + cross + f[6] * u[0] + f[7] * u[1] + f[8] * u[2] + f[9];
}

double ringAt(const std::vector<float>& f, const double* u) {
	double v[3];
	for (int k = 0; k < 3; ++k) {
		v[k] = f[std::size_t(k)] + f[std::size_t(3 + k)] * u[0] + f[std::size_t(6 + k)] * u[1] + f[std::size_t(9 + k)] * u[2];
	}
	const double major = f[12];
	const double minor = f[13];
	const double q     = v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + major * major - minor * minor;
	const double t     = q * q - 4 * major * major * (v[0] * v[0] + v[1] * v[1]);
	return minor < 0 ? -t : t;
}

struct SlabCount {
	int decided;
	int wrong;
};

std::vector<float> randomQuadric(std::mt19937& random) {
	std::uniform_real_distribution<float> coefficient(-1, 1);
	std::vector<float> f;
	for (int k = 0; k < 10; ++k) {
		f.push_back(coefficient(random));
	}
	return f;
}

std::vector<float> randomRing(std::mt19937& random) {
	std::uniform_real_distribution<float> origin(-1.2f, 1.2f);
	std::uniform_real_distribution<float> axis(-0.4f, 0.4f);
	std::uniform_real_distribution<float> minor(0.1f, 0.5f);
	std::vector<float> f = { origin(random), origin(random), origin(random) };
	for (int k = 0; k < 9; ++k) {
		f.push_back(axis(random));
	}
	f.push_back(1);
	f.push_back(minor(random) * (random() % 2 == 0 ? 1.0f : -1.0f));
	return f;
}

void countSlab(SlabCount& count, const Slab& slab, double value, const double* u) {
	const double plane = slab.slope[0] * u[0] + slab.slope[1] * u[1] + slab.slope[2] * u[2] + slab.offset;
	if (std::abs(plane) <= slab.stray) {
		return;
	}
	count.decided += 1;
	count.wrong += (plane > 0) == (value > 0) ? 0 : 1;
}

void checkFaceSlabs() {
	std::mt19937 random(31);
	std::uniform_real_distribution<double> widened(-slabReach, 1 + slabReach);

	SlabCount quadrics = {
		.decided = 0,
		.wrong   = 0,
	};

	SlabCount rings = quadrics;
	for (int face = 0; face < 200; ++face) {
		const std::vector<float> quadric = randomQuadric(random);
		const std::vector<float> ring    = randomRing(random);
		const Slab quadricBound          = quadricSlab(quadric.data());
		const Slab ringBound             = ringSlab(ring.data());
		for (int k = 0; k < 200; ++k) {
			const double u[3] = { widened(random), widened(random), widened(random) };
			countSlab(quadrics, quadricBound, quadricAt(quadric, u), u);
			countSlab(rings, ringBound, ringAt(ring, u), u);
		}
	}
	std::printf("tree face slabs: of 40000 points the slab decides %d for quadrics (%d wrong) and %d for rings (%d wrong)\n", quadrics.decided, quadrics.wrong, rings.decided, rings.wrong);
	check(quadrics.decided > 10000 && rings.decided > 4000 && quadrics.wrong == 0 && rings.wrong == 0, "where a quadric's or a ring's slab plane stands further off than the stray, the face has the plane's sign");
}

struct Shared {
	std::size_t differ;
	std::size_t hits;
};

struct Passing {
	const Scene& scene;
	const Detail& detail;
	const std::vector<PassMember>& passes;
	Region region;
};

Tree openedTree(const LazyInput& input) {
	Tree tree = lazyTree(input);
	openEveryCell(tree, input.workers);
	return tree;
}

Tree passedTree(const Passing& passing) {
	const ObjectBoxes boxes = objectBoxes(passing.scene, 8);

	const LazyInput input = {
		.scene     = passing.scene,
		.detail    = passing.detail,
		.passes    = passing.passes,
		.boxes     = boxes,
		.region    = passing.region,
		.depthCap  = depthCapOf(passing.region, passing.detail.least),
		.workers   = 8,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	return openedTree(input);
}

Detail testDetail(const SceneFile& file, const Region& region, const std::vector<DetailMember>& members) {
	const SceneDetailQuery query = {
		.scene   = file.scene,
		.shot    = file.shot,
		.region  = region,
		.members = members,
	};

	return sceneDetail(query);
}

TestTree passedTestTree(const SceneFile& file, const std::vector<PassMember>& passes, int workers) {
	const ObjectBoxes boxes = objectBoxes(file.scene, 4);
	const Region region     = boxedRegion(boxes.boxes, file.shot);
	const Detail detail     = testDetail(file, region, defaultDetailMembers());

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = workers,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	return TestTree {
		.detail = detail,
		.tree   = openedTree(input),
	};
}

TestTree testTree(const SceneFile& file, int workers) {
	return passedTestTree(file, defaultPassMembers(), workers);
}

std::vector<PassMember> passesWithoutFaceSlabs() {
	std::vector<PassMember> passes = defaultPassMembers();
	std::erase(passes, PassMember::FaceSlabs);
	return passes;
}

CellPrograms allObjects(const std::vector<Program>& programs) {
	CellPrograms cell = {
		.objects  = {},
		.programs = {},
	};

	for (std::uint32_t i = 0; i < programs.size(); ++i) {
		cell.objects.push_back(i);
		cell.programs.push_back(std::make_shared<const Program>(programs[i]));
	}
	return cell;
}

struct Asked {
	Box box;
	double tolerance;
	bool deeper;
};

Plan classified(const std::vector<Program>& programs, const Asked& asked) {
	const CellPrograms cell = allObjects(programs);
	const std::vector<ObjectSlot> slots;

	const CellQuery query = {
		.programs  = cell,
		.slots     = slots,
		.box       = asked.box,
		.tolerance = asked.tolerance,
		.deeper    = asked.deeper,
		.faces     = true,
	};

	return classifyCell(query);
}

bool hasPatchFace(const Plan& plan) {
	return plan.kind == CellKind::Surface && !plan.patched && runHeader(plan.run[0]).patches != 0;
}

void checkClassifySphere() {
	const std::vector<Program> programs = { sphereProgram() };

	const Asked asked = {
		.box       = cubeAt({ 0.53, 0.53, 0.53 }, 0.1),
		.tolerance = 1e-4,
		.deeper    = true,
	};

	const Plan plan        = classified(programs, asked);
	const RunHeader header = runHeader(plan.run.empty() ? 0 : plan.run[0]);
	check(plan.kind == CellKind::Surface && !plan.patched && plan.leafKind == LeafKind::Held && header.count == 1 && faceKind(header, 0) == FaceKind::Quadric, "a cell on a sphere is one proven quadric face");
}

int unprovenFaces(const std::vector<Program>& programs, const Asked& asked) {
	const CellCut cut = cutCell(allObjects(programs), asked.box);

	const Cell cell = {
		.programs  = cut.programs.programs,
		.box       = asked.box,
		.tolerance = asked.tolerance,
	};

	int unproven = 0;
	for (const Face& face : cellFaces(cell).faces) {
		unproven += face.proven ? 0 : 1;
	}
	return unproven;
}

void checkClassifyBlend() {
	const SceneFile file = sceneOrExit(writeScene("blend", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		merge(0.3) {
			at([-0.4, 0, 0]) sphere(0.5);
			at([0.4, 0, 0]) sphere(0.5);
		}

	)"));

	int patched  = 0;
	int wrong    = 0;
	int surfaces = 0;
	for (int i = 0; i < 14; ++i) {
		for (int j = 0; j < 5; ++j) {
			const Asked asked = {
				.box       = cubeAt({ -0.35 + 0.05 * i, 0.3 + 0.05 * j, 0 }, 0.05),
				.tolerance = 1e-4,
				.deeper    = true,
			};

			const Plan plan = classified(file.scene.objectPrograms, asked);
			surfaces += plan.kind == CellKind::Surface ? 1 : 0;
			if (hasPatchFace(plan)) {
				patched += 1;
				wrong += plan.leafKind == LeafKind::Held && unprovenFaces(file.scene.objectPrograms, asked) > 0 ? 0 : 1;
			}
		}
	}
	std::printf("tree classify: %d of %d surface cells of a smooth join keep a patch face, %d without an unproven face\n", patched, surfaces, wrong);
	check(patched > 0 && wrong == 0, "a smooth join keeps a patch face where its Taylor quadric is not proven");
}

void checkClassifyCrowded() {
	const SceneFile file = sceneOrExit(writeScene("crowd", R"(
		surface box(size) {
			let q = abs(p) - size / 2;
			return length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0);
		}

		for i in 0..3 {
			for j in 0..3 {
				at([0.1 * i, 0.1 * j, 0]) box([0.06, 0.06, 0.06]);
			}
		}
	)"));

	Asked asked = {
		.box       = cubeAt({ -0.05, -0.05, -0.05 }, 0.3),
		.tolerance = 1e-4,
		.deeper    = true,
	};

	const Plan deeper = classified(file.scene.objectPrograms, asked);
	asked.deeper      = false;
	const Plan floor  = classified(file.scene.objectPrograms, asked);
	check(deeper.kind == CellKind::Branch, "a cell crowded with more faces than a leaf holds splits");
	check(floor.kind == CellKind::Surface && floor.patched && floor.leafKind == LeafKind::Floor, "a crowded cell at the depth cap is a floor leaf of one patch");
}

void checkPatchSkip() {
	const SceneFile file = sceneOrExit(writeScene("facets", R"(
		surface plane(normal, offset) {
			return dot(p, normal) - offset;
		}

		merge {
			for i in 0..9 {
				plane([0.004 * cos(i * pi / 4.5), 0.004 * sin(i * pi / 4.5), 1], 0);
			}
		}
	)"));

	Asked asked = {
		.box       = cubeAt({ -0.05, -0.05, -0.05 }, 0.1),
		.tolerance = 0.1 / 16,
		.deeper    = true,
	};

	const Plan wide   = classified(file.scene.objectPrograms, asked);
	asked.tolerance   = 0.1 / 8;
	const Plan narrow = classified(file.scene.objectPrograms, asked);
	check(wide.kind == CellKind::Branch, "a crowded cell of 16 tolerances or more splits without a patch");
	check(narrow.kind == CellKind::Surface && narrow.patched && narrow.leafKind == LeafKind::Held, "a crowded cell under 16 tolerances wide keeps the patch that holds");
}

std::vector<float> sidedQuadric(std::mt19937& random, bool plane) {
	std::uniform_real_distribution<float> coefficient(-0.4f, 0.4f);
	std::uniform_real_distribution<float> offset(-2, 2);
	std::vector<float> f;
	for (int k = 0; k < 9; ++k) {
		f.push_back(plane && k < 6 ? 0 : coefficient(random));
	}
	f.push_back(offset(random));
	return f;
}

std::vector<std::uint32_t> sidedRun(std::mt19937& random, std::uint32_t count) {
	RunHeader header = {
		.count     = count,
		.patches   = 0,
		.planes    = 0,
		.needed    = 0,
		.slabbed   = false,
		.passWords = 0,
	};

	std::vector<std::uint32_t> words = { 0 };
	for (std::uint32_t k = 0; k < tableWordCount(count); ++k) {
		words.push_back(std::uint32_t(random()));
	}
	for (std::uint32_t face = 0; face < count; ++face) {
		const bool plane           = random() % 2 == 0;
		const std::vector<float> f = sidedQuadric(random, plane);
		header.planes |= plane ? 1u << face : 0u;
		for (std::size_t k = plane ? 6 : 0; k < 10; ++k) {
			words.push_back(std::bit_cast<std::uint32_t>(f[k]));
		}
	}
	words[0] = runHeaderWord(header);
	return words;
}

std::vector<float> runFace(const std::vector<std::uint32_t>& run, std::uint32_t face) {
	const bool plane     = faceKind(runHeader(run[0]), face) == FaceKind::Plane;
	const float* written = reinterpret_cast<const float*>(run.data() + faceOffsets(run.data())[face]);
	std::vector<float> f(10, 0.0f);
	std::copy(written, written + (plane ? 4 : 10), f.begin() + (plane ? 6 : 0));
	return f;
}

bool sampledInside(const std::vector<std::uint32_t>& run, const double* u) {
	const std::uint32_t count = runHeader(run[0]).count;
	std::uint32_t bits        = 0;
	for (std::uint32_t face = 0; face < count; ++face) {
		bits |= quadricAt(runFace(run, face), u) < 0 ? 1u << face : 0u;
	}
	return count == 1 ? bits == 1 : ((run[1 + bits / 32] >> (bits % 32)) & 1u) != 0;
}

int surfacelessWrong(std::mt19937& random, const std::vector<std::uint32_t>& run) {
	std::uniform_real_distribution<double> unit(0, 1);
	const int side = surfacelessSide(run);
	int wrong      = 0;
	for (int i = 0; i < 1000 && side != 0; ++i) {
		const double u[3] = { unit(random), unit(random), unit(random) };
		wrong += sampledInside(run, u) == (side == 2) ? 0 : 1;
	}
	return wrong;
}

std::vector<std::uint32_t> onePlaneRun(float offset) {
	const RunHeader header = {
		.count     = 1,
		.patches   = 0,
		.planes    = 1,
		.needed    = 1,
		.slabbed   = false,
		.passWords = 0,
	};

	return { runHeaderWord(header), std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(1.0f), std::bit_cast<std::uint32_t>(offset) };
}

void checkSurfaceless() {
	std::mt19937 random(11);
	int proven[3] = { 0, 0, 0 };
	int single    = 0;
	int wrong     = 0;
	for (int i = 0; i < 4000; ++i) {
		const std::uint32_t count            = 1 + std::uint32_t(i % 5);
		const std::vector<std::uint32_t> run = sidedRun(random, count);
		const int side                       = surfacelessSide(run);
		proven[side] += 1;
		single += side != 0 && count == 1 ? 1 : 0;
		wrong += surfacelessWrong(random, run);
	}
	std::printf("tree surfaceless: %d runs proven empty, %d filled (%d of one face), %d left, %d wrong points\n", proven[1], proven[2], single, proven[0], wrong);
	check(proven[1] > 100 && proven[2] > 100 && single > 100 && proven[0] > 100 && wrong == 0, "a run proven surfaceless is on its claimed side at every sampled point");
	check(surfacelessSide(onePlaneRun(-2)) == 2 && surfacelessSide(onePlaneRun(0.5)) == 1 && surfacelessSide(onePlaneRun(-0.5)) == 0, "a run of one face is filled where the face is negative, empty where positive, and unproven where it crosses");
}

bool sameSurface(const SurfaceLeaf& a, const SurfaceLeaf& b) {
	return a.run == b.run && a.programs == b.programs && a.loosened == b.loosened && a.kind == b.kind;
}

struct PackPair {
	const Subtree& octree;
	const Subtree& packed;
};

int nodeMismatches(const PackPair& pair, std::uint32_t cell, const PackedNode& node);

struct NodeSlot {
	const PackedNode& node;
	int slot;
};

NodeSlot slotOf(const PackedNode& node, int slot) {
	return NodeSlot {
		.node = node,
		.slot = slot,
	};
}

int slotMismatches(const PackPair& pair, std::uint32_t cell, const NodeSlot& at) {
	const PackedNode& node = at.node;
	const int slot         = at.slot;
	const CellEntry& entry = pair.octree.cells[cell];
	const CellKind kind    = slotKind(node, slot);
	if (kind != entry.kind) {
		return 1;
	}
	switch (kind) {
		case CellKind::Surface: return sameSurface(pair.octree.surfaces[entry.payload], pair.packed.surfaces[slotPayload(node, slot)]) ? 0 : 1;
		case CellKind::Filled: return pair.octree.solids[entry.payload].programs == pair.packed.solids[slotPayload(node, slot)].programs ? 0 : 1;
		case CellKind::Branch: return nodeMismatches(pair, cell, pair.packed.nodes[slotPayload(node, slot)]);
		case CellKind::Empty: return 0;
	}

	return 1;
}

int coarseMismatches(const PackPair& pair, std::uint32_t child, const NodeSlot& at) {
	const PackedNode& node = at.node;
	const int octant       = at.slot;
	const bool coarse      = ((node.coarse >> octant) & 1) != 0;
	if (pair.octree.cells[child].kind != CellKind::Branch) {
		int padding = 0;
		for (int sub = 1; sub < 8; ++sub) {
			padding += slotKind(node, octant * 8 + sub) == CellKind::Empty ? 0 : 1;
		}
		return (coarse ? 0 : 1) + padding + slotMismatches(pair, child, slotOf(node, octant * 8));
	}
	int wrong = coarse ? 1 : 0;
	for (int sub = 0; sub < 8; ++sub) {
		wrong += slotMismatches(pair, pair.octree.cells[child].payload + std::uint32_t(sub), slotOf(node, octant * 8 + sub));
	}
	return wrong;
}

int nodeMismatches(const PackPair& pair, std::uint32_t cell, const PackedNode& node) {
	int wrong = 0;
	for (int octant = 0; octant < 8; ++octant) {
		wrong += coarseMismatches(pair, pair.octree.cells[cell].payload + std::uint32_t(octant), slotOf(node, octant));
		const bool busy = ((node.busy >> octant) & 1) != 0;
		const bool held = (((node.lowerKinds | node.upperKinds) >> (octant * 8)) & 0xFF) != 0;
		wrong += busy == held ? 0 : 1;
	}
	return wrong;
}

void checkPackRoundTrip() {
	const SceneFile file = sceneOrExit("scenes/chess-board.sdf");
	const Region region  = sceneRegion(file.scene, file.shot, 4);
	const Detail detail  = testDetail(file, region, defaultDetailMembers());
	const std::vector<ObjectSlot> slots;

	const GrowContext context = {
		.scene    = file.scene,
		.slots    = slots,
		.detail   = detail,
		.depthCap = 7,
		.faces    = true,
		.sharp    = false,
	};

	const Subtree octree = growSubtree(context, rootSprout(context, region));
	const Subtree packed = packedSubtree(octree);

	const PackPair pair = {
		.octree = octree,
		.packed = packed,
	};

	const bool branch = packed.root.kind == CellKind::Branch && octree.cells[0].kind == CellKind::Branch;
	const int wrong   = branch ? nodeMismatches(pair, 0, packed.root.node) : 1;
	std::printf("tree pack: %zu octree cells, %zu packed nodes, %d leaves or bits apart\n", octree.cells.size(), packed.nodes.size() + 1, wrong);
	check(wrong == 0 && packed.surfaces.size() == octree.surfaces.size() && packed.solids.size() == octree.solids.size(), "a packed subtree holds every leaf of its octree at the slot its cell stands in");
}

void pushReference(const Tree& tree, const ProgramReference& reference, std::vector<std::uint32_t>& words) {
	words.push_back(std::uint32_t(reference.source));
	words.push_back(referencedSlot(tree, reference));
	if (reference.source != ProgramSource::Cut) {
		return;
	}
	const ProgramCut& cut = tree.cuts[reference.index];
	words.push_back(cut.result);
	words.push_back(cut.ranges);
	words.push_back(cut.rerouted);
	words.insert(words.end(), tree.cutSteps.begin() + cut.first, tree.cutSteps.begin() + cut.first + 2 * (cut.ranges + cut.rerouted));
}

void pushList(const Tree& tree, std::uint32_t list, std::vector<std::uint32_t>& words) {
	for (std::uint32_t k = 0; k < tree.lists[list].count; ++k) {
		pushReference(tree, tree.references[tree.lists[list].first + k], words);
	}
}

std::vector<std::uint32_t> leafWords(const Tree& tree, const PlacedLeaf& leaf) {
	std::vector<std::uint32_t> words = { std::uint32_t(leaf.level), leaf.place[0], leaf.place[1], leaf.place[2], std::uint32_t(leaf.entry.kind) };
	if (leaf.entry.kind == CellKind::Filled) {
		pushList(tree, tree.solids[leaf.entry.payload].programs, words);
		return words;
	}
	const SurfaceLeaf& surface = tree.surfaces[leaf.entry.payload];
	const std::uint32_t* run   = tree.runs.data() + surface.run;
	words.push_back(surface.loosened);
	words.push_back(std::uint32_t(surface.kind));
	words.insert(words.end(), run, run + faceOffsets(run).back());
	pushList(tree, surface.programs, words);
	return words;
}

std::vector<std::vector<std::uint32_t>> sortedLeaves(const Tree& tree) {
	std::vector<std::vector<std::uint32_t>> leaves;
	for (const PlacedLeaf& leaf : placedLeaves(tree)) {
		leaves.push_back(leafWords(tree, leaf));
	}
	std::sort(leaves.begin(), leaves.end());
	return leaves;
}

bool sameTrees(const Tree& a, const Tree& b) {
	return sortedLeaves(a) == sortedLeaves(b);
}

void checkDeterministic() {
	SceneFile file          = sceneOrExit("scenes/magic-cube.sdf");
	file.shot.render.width  = 320;
	file.shot.render.height = 200;
	const TestTree one      = testTree(file, 1);
	const TestTree eight    = testTree(file, 8);
	std::printf("tree build: %zu nodes, %zu surface leaves on 1 and on 8 threads\n", one.tree.nodes.size(), one.tree.surfaces.size());
	check(one.tree.nodes.size() > 100 && sameTrees(one.tree, eight.tree), "a tree opened in full holds the same leaves to the bit on 1 and 8 threads");
}

Plan seamPlan(const std::vector<Program>& programs, const Box& box) {
	const CellPrograms cell = allObjects(programs);
	const std::vector<ObjectSlot> slots;

	const CellQuery query = {
		.programs  = cell,
		.slots     = slots,
		.box       = box,
		.tolerance = 2e-4,
		.deeper    = true,
		.faces     = false,
	};

	return classifyCell(query);
}

bool isSeamPair(const Plan& plan) {
	const RunHeader header = runHeader(plan.run.empty() ? 0 : plan.run[0]);
	return plan.kind == CellKind::Surface && !plan.patched && plan.leafKind == LeafKind::Held && header.count == 2 && header.patches == 3 && plan.fitted.stray > 2e-4;
}

void checkSeamPair() {
	const Scene apart = sceneOrExit(writeScene("apart", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		at([-0.8, 0, 0]) sphere(1);
		at([0.8, 0, 0]) sphere(1);
	)"))
							.scene;

	const Scene joined = sceneOrExit(writeScene("joined", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		cut {
			at([-0.6, 0, 0]) sphere(1);
			at([0.6, 0, 0]) sphere(1);
		}

	)"))
							 .scene;

	const Box box     = cubeAt({ -0.05, 0.55, -0.05 }, 0.1);
	const Box cutAway = cubeAt({ -0.05, 0.75, -0.05 }, 0.1);
	const bool merged = isSeamPair(seamPlan(apart.objectPrograms, box));
	const bool shell  = joined.objectPrograms.size() == 1 && isSeamPair(seamPlan(joined.objectPrograms, cutAway));
	check(merged && shell, "a cell on the crease of a union of two objects, or of a cut inside one object, is a held pair of patches where one patch strays");
}

bool sameHit(const Hit& a, const Hit& b) {
	const bool where = a.distance == b.distance && a.point.x == b.point.x && a.point.y == b.point.y && a.point.z == b.point.z;
	const bool what  = a.normal.x == b.normal.x && a.normal.y == b.normal.y && a.normal.z == b.normal.z && a.leaf == b.leaf;
	return a.kind == b.kind && (a.kind == HitKind::Missed || (where && what && a.loosened == b.loosened && a.leafKind == b.leafKind));
}

Shared sharedHits(const SceneFile& file, const Tree& plain, const Tree& shared) {
	const Lens lens = lensOf(file.shot.camera, file.shot.render);

	const Geometry one = treeGeometry(file.scene, plain);

	const Geometry other = treeGeometry(file.scene, shared);

	Shared tally = {
		.differ = 0,
		.hits   = 0,
	};

	for (int y = 0; y < lens.height; ++y) {
		for (int x = 0; x < lens.width; ++x) {
			const Ray ray = pixelRay(lens, x, y);
			const Hit a   = intersect(one, ray);
			tally.differ += sameHit(a, intersect(other, ray)) ? 0 : 1;
			tally.hits += a.kind == HitKind::Missed ? 0 : 1;
		}
	}
	return tally;
}

Tree treeWith(const Passing& passing, const std::vector<PassMember>& passes) {
	const Passing asked = {
		.scene  = passing.scene,
		.detail = passing.detail,
		.passes = passes,
		.region = passing.region,
	};

	return passedTree(asked);
}

void checkSharedPasses() {
	SceneFile file          = sceneOrExit("scenes/magic-cube.sdf");
	file.shot.render.width  = 640;
	file.shot.render.height = 400;
	const Region region     = sceneRegion(file.scene, file.shot, 4);
	const Detail detail     = testDetail(file, region, defaultDetailMembers());

	const std::vector<PassMember> none = {};
	const std::vector<PassMember> runs = { PassMember::SharedCoefficients };

	const Passing passing = {
		.scene  = file.scene,
		.detail = detail,
		.passes = none,
		.region = region,
	};

	const Tree plain    = treeWith(passing, none);
	const double whole  = treeStats(plain).megabytes;
	const Tree folded   = treeWith(passing, runs);
	const double shared = treeStats(folded).megabytes;
	const Shared tally  = sharedHits(file, plain, folded);
	std::printf("tree shared: %.3f MB, %.3f with runs shared within a node; %zu of %zu pixel hits differ\n", whole, shared, tally.differ, tally.hits);
	check(tally.hits > 10000 && tally.differ == 0 && shared < 0.75 * whole, "shared coefficients keep every hit to the bit in less memory");
}

void checkCutPass() {
	SceneFile file          = sceneOrExit("scenes/chess-board.sdf");
	file.shot.render.width  = 320;
	file.shot.render.height = 200;
	const Region region     = sceneRegion(file.scene, file.shot, 4);
	const Detail detail     = testDetail(file, region, defaultDetailMembers());

	const std::vector<PassMember> none = {};
	const std::vector<PassMember> cut  = { PassMember::CutLongPrograms };

	const Passing passing = {
		.scene  = file.scene,
		.detail = detail,
		.passes = none,
		.region = region,
	};

	const Tree whole   = treeWith(passing, none);
	const Tree cuts    = treeWith(passing, cut);
	const Shared tally = sharedHits(file, whole, cuts);
	std::printf("cut programs: %zu cuts in %zu entries; %zu of %zu pixel hits differ\n", cuts.cuts.size(), cuts.cutSteps.size(), tally.differ, tally.hits);
	check(tally.hits > 10000 && tally.differ == 0 && !cuts.cuts.empty(), "cut programs keep every hit of the whole programs to the bit");
}

SceneFile hiddenBumpScene() {
	return sceneOrExit(writeScene("bump", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		surface ground(height) {
			return p.z - height;
		}

		ground(0.49);
		at([0.75, 0.75, 0.6]) sphere(0.2);
		at([0.375, 0.125, 0.49]) sphere(0.02);
	)"));
}

Detail leastDetail(const SceneFile& file, const Region& region) {
	Detail detail = testDetail(file, region, {});
	detail.least  = 1e-3;
	return detail;
}

CellKind plannedKind(const Scene& scene, const Detail& detail) {
	const Region region = {
		.lower = { 0, 0, 0 },
		.size  = { 0.5, 0.5, 0.5 },
	};

	const std::vector<ObjectSlot> slots;

	const GrowContext context = {
		.scene    = scene,
		.slots    = slots,
		.detail   = detail,
		.depthCap = 8,
		.faces    = false,
		.sharp    = false,
	};

	return planCell(context, rootSprout(context, region)).kind;
}

void checkSplitHidden() {
	const SceneFile file = hiddenBumpScene();

	const Region unit = {
		.lower = { 0, 0, 0 },
		.size  = { 1, 1, 1 },
	};

	const Detail blind = leastDetail(file, unit);
	Detail seeing      = testDetail(file, unit, { DetailMember::SplitHiddenPrimitives });
	seeing.least       = blind.least;
	const CellKind off = plannedKind(file.scene, blind);
	const CellKind on  = plannedKind(file.scene, seeing);
	check(off == CellKind::Surface && on == CellKind::Branch, "a patch that holds a plane but misses a small sphere between its points splits where split_hidden_primitives is on");
}

Ray betweenSpots(const Spot& from, const Spot& to, bool sharp) {
	const math::Vector origin = { from[0], from[1], from[2] };
	const math::Vector target = { to[0], to[1], to[2] };
	return Ray {
		.origin    = origin,
		.direction = math::normalized(target - origin),
		.far       = std::numeric_limits<float>::infinity(),
		.inside    = false,
		.sharp     = std::uint8_t(sharp ? 1 : 0),
		.within    = 0,
	};
}

std::vector<Ray> lazyRays(const SceneFile& file, const Region& region, bool sharp) {
	const Lens lens = lensOf(file.shot.camera, file.shot.render);
	Sampler sampler = samplerFor({}, 0);
	std::uniform_real_distribution<double> unit(0, 1);
	std::vector<Ray> rays;
	for (int k = 0; k < 5000; ++k) {
		Ray seen   = pixelRay(lens, int(unit(sampler.random) * lens.width), int(unit(sampler.random) * lens.height));
		seen.sharp = std::uint8_t(sharp ? 1 : 0);
		rays.push_back(seen);
		rays.push_back(betweenSpots(insideBox(sampler, regionBox(region)), insideBox(sampler, regionBox(region)), sharp));
	}
	return rays;
}

std::vector<Hit> hitsOf(const Geometry& geometry, const std::vector<Ray>& rays) {
	std::vector<Hit> hits;
	for (const Ray& ray : rays) {
		hits.push_back(intersect(geometry, ray));
	}
	return hits;
}

Shared differingHits(const std::vector<Hit>& a, const std::vector<Hit>& b) {
	Shared tally = {
		.differ = 0,
		.hits   = 0,
	};

	for (std::size_t k = 0; k < a.size(); ++k) {
		tally.differ += sameHit(a[k], b[k]) ? 0 : 1;
		tally.hits += a[k].kind == HitKind::Missed ? 0 : 1;
	}
	return tally;
}

struct LazyCase {
	const SceneFile& file;
	ObjectBoxes boxes;
	Region region;
	Detail detail;
	std::vector<PassMember> passes;
};

SceneFile smallScene(const char* path) {
	SceneFile file          = sceneOrExit(path);
	file.shot.render.width  = 320;
	file.shot.render.height = 200;
	return file;
}

LazyCase lazyCase(const SceneFile& file) {
	const ObjectBoxes boxes = objectBoxes(file.scene, 4);
	const Region region     = boxedRegion(boxes.boxes, file.shot);
	return LazyCase {
		.file   = file,
		.boxes  = boxes,
		.region = region,
		.detail = testDetail(file, region, defaultDetailMembers()),
		.passes = defaultPassMembers(),
	};
}

LazyInput lazyInput(const LazyCase& made) {
	return LazyInput {
		.scene     = made.file.scene,
		.detail    = made.detail,
		.passes    = made.passes,
		.boxes     = made.boxes,
		.region    = made.region,
		.depthCap  = depthCapOf(made.region, made.detail.least),
		.workers   = 6,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};
}

math::Vector gridPoint(const Box& box, int k) {
	const int index[3] = { k % 16, k / 16 % 16, k / 256 };
	double at[3]       = { 0, 0, 0 };
	for (int axis = 0; axis < 3; ++axis) {
		const double side = box.axes[axis].upper - box.axes[axis].lower + 1e-3;
		at[axis]          = box.axes[axis].lower - side + 3 * side * (index[axis] + 0.5) / 16;
	}
	return { at[0], at[1], at[2] };
}

bool insideBox(const Box& box, const math::Vector& p) {
	return box.axes[0].lower <= p.x && p.x <= box.axes[0].upper && box.axes[1].lower <= p.y && p.y <= box.axes[1].upper && box.axes[2].lower <= p.z && p.z <= box.axes[2].upper;
}

std::size_t pointsOutside(const Program& program, const Box& box) {
	std::size_t outside = 0;
	for (int k = 0; k < 4096; ++k) {
		const math::Vector p = gridPoint(box, k);
		outside += !insideBox(box, p) && objectSurface(program, p).value < 0 ? 1 : 0;
	}
	return outside;
}

const char* blobScenePath() {
	return writeScene("blob", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		camera {
			eye    = [0, -6, 2];
			target = [0.5, 0, 0];
			fov    = 0.5;
		}

		merge(2) {
			sphere(0.5);
			at([1, 0, 0]) sphere(0.5);
		}
	)");
}

void checkObjectBoxes() {
	std::size_t outside = 0;
	std::size_t open    = 0;
	std::size_t objects = 0;
	for (const char* path : { "scenes/chess-board.sdf", "scenes/showcase.sdf", blobScenePath() }) {
		const SceneFile file    = sceneOrExit(path);
		const ObjectBoxes boxes = objectBoxes(file.scene, 4);
		for (std::size_t object = 0; object < boxes.boxes.size(); ++object) {
			const bool finite = std::isfinite(boxes.boxes[object].axes[0].lower + boxes.boxes[object].axes[1].lower + boxes.boxes[object].axes[2].lower + boxes.boxes[object].axes[0].upper + boxes.boxes[object].axes[1].upper + boxes.boxes[object].axes[2].upper);
			outside += finite ? pointsOutside(file.scene.objectPrograms[object], boxes.boxes[object]) : 0;
			open += finite ? 0 : 1;
		}
		objects += boxes.boxes.size();
	}
	std::printf("object boxes: %zu of %zu objects unbounded on some side, %zu points inside an object outside its box\n", open, objects, outside);
	check(outside == 0, "every point where an object's field is below 0 lies in the object's box, smooth merges, cuts and clips included");
	check(open <= 1, "only the ground's half-space is unbounded");
}

void checkLazyHits() {
	const SceneFile file        = smallScene("scenes/chess-board.sdf");
	const LazyCase made         = lazyCase(file);
	const std::vector<Ray> rays = lazyRays(made.file, made.region, true);
	const Tree whole            = openedTree(lazyInput(made));
	Tree lazy                   = lazyTree(lazyInput(made));
	const Shared tally          = differingHits(hitsOf(treeGeometry(made.file.scene, whole), rays), hitsOf(treeGeometry(made.file.scene, lazy), rays));
	const std::size_t grown     = placedLeaves(lazy).size();
	const std::size_t every     = placedLeaves(whole).size();
	std::printf("lazy tree: %zu of %zu hits differ from the tree opened in full; %zu of its %zu leaves were built\n", tally.differ, tally.hits, grown, every);
	check(tally.hits > 3000 && tally.differ == 0, "a tree grown by 10,000 sharp rays gives each the hit of the tree opened in full");
	check(grown > 100 && grown < every, "a tree builds only the nodes its rays enter");
}

std::vector<std::vector<Hit>> hitsOnThreads(const Geometry& geometry, const std::vector<std::vector<Ray>>& rays) {
	std::vector<std::vector<Hit>> hits(rays.size());
	std::vector<std::thread> threads;
	for (std::size_t k = 0; k < rays.size(); ++k) {
		threads.emplace_back([&, k]() {
			hits[k] = hitsOf(geometry, rays[k]);
		});
	}
	for (std::thread& thread : threads) {
		thread.join();
	}
	return hits;
}

void checkLazyThreads() {
	const SceneFile file         = smallScene("scenes/chess-board.sdf");
	const LazyCase made          = lazyCase(file);
	const std::vector<Ray> sharp = lazyRays(made.file, made.region, true);
	const std::vector<Ray> blunt = lazyRays(made.file, made.region, false);
	Tree alone                   = lazyTree(lazyInput(made));
	const std::vector<Hit> hits  = hitsOf(treeGeometry(made.file.scene, alone), sharp);
	Tree shared                  = lazyTree(lazyInput(made));
	std::size_t differ           = 0;
	for (const std::vector<Hit>& found : hitsOnThreads(treeGeometry(made.file.scene, shared), std::vector<std::vector<Ray>>(6, sharp))) {
		differ += differingHits(hits, found).differ;
	}
	Tree mixed = lazyTree(lazyInput(made));
	hitsOnThreads(treeGeometry(made.file.scene, mixed), { sharp, blunt, sharp, blunt, sharp, blunt });
	const std::size_t after = differingHits(hits, hitsOf(treeGeometry(made.file.scene, mixed), sharp)).differ;
	std::printf("lazy tree on 6 threads: %zu hits differ from one thread's; %zu after sharp and other rays grew it together\n", differ, after);
	check(differ == 0, "six threads growing one tree with the same sharp rays each get the hits one thread gets");
	check(after == 0, "sharp rays get the same hits after sharp and other rays grew the tree together");
}

const char* asideScenePath() {
	return writeScene("aside", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		surface box(size) {
			let q = abs(p) - size / 2;
			return length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0);
		}

		camera {
			eye    = [0, -12, 6];
			target = [-5, 0, 0];
			fov    = 0.3;
		}

		at([10, 0, -0.5]) box([60, 20, 1]);
		at([0, 0, 1]) sphere(1);
		at([20, 3, 1]) sphere(1);
	)");
}

SceneFile asideScene() {
	return sceneOrExit(asideScenePath());
}

bool sameBuilt(const Tree& before, const Tree& after) {
	bool same = std::equal(before.surfaces.begin(), before.surfaces.end(), after.surfaces.begin(), sameSurface) && std::equal(before.runs.begin(), before.runs.end(), after.runs.begin());
	for (std::size_t k = 0; k < before.nodes.size(); ++k) {
		const PackedNode& a = before.nodes[k];
		const PackedNode& b = after.nodes[k];
		same                = same && a.lowerKinds == b.lowerKinds && a.upperKinds == b.upperKinds && a.firstBranch == b.firstBranch && a.firstSurface == b.firstSurface && a.firstSolid == b.firstSolid && a.coarse == b.coarse && a.busy == b.busy;
	}
	return same;
}

Tree heldCopy(const Tree& tree) {
	return Tree {
		.region     = tree.region,
		.root       = tree.root,
		.nodes      = tree.nodes,
		.surfaces   = tree.surfaces,
		.solids     = tree.solids,
		.runs       = tree.runs,
		.lists      = tree.lists,
		.references = tree.references,
		.cuts       = tree.cuts,
		.cutSteps   = tree.cutSteps,
		.frame      = 0,
		.growth     = nullptr,
	};
}

void checkSharpRefines() {
	SceneFile file                       = asideScene();
	file.shot.render.width               = 320;
	file.shot.render.height              = 200;
	const ObjectBoxes boxes              = objectBoxes(file.scene, 4);
	const Region region                  = boxedRegion(boxes.boxes, file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	Tree tree               = lazyTree(input);
	const Geometry geometry = treeGeometry(file.scene, tree);
	const Ray blunt         = betweenSpots({ 20, 3, 10 }, { 20, 3, 0 }, false);
	const Ray sharp         = betweenSpots({ 20, 3, 10 }, { 20, 3, 0 }, true);
	const Hit loose         = intersect(geometry, blunt);
	const Tree before       = heldCopy(tree);
	const Hit fine          = intersect(geometry, sharp);
	const Hit again         = intersect(geometry, blunt);
	const bool forwarded    = NodeState(tree.nodes[tree.root.payload].state) == NodeState::Forwarded;
	std::printf("sharp ray: a ray that is not sharp meets a leaf outside the frame held to %g at %.5f, a sharp ray one held to %g at %.5f\n", loose.loosened, loose.distance, fine.loosened, fine.distance);
	check(loose.kind != HitKind::Missed && fine.kind != HitKind::Missed && fine.loosened < loose.loosened, "a sharp ray into a cell outside the frame that was built loosely meets a finer leaf");
	check(std::abs(fine.distance - 8) < 4 * fine.loosened + 1e-3 && std::abs(loose.distance - 8) < 4 * loose.loosened + 1e-3, "both leaves hold the surface within their tolerance");
	check(forwarded && sameBuilt(before, tree) && sameHit(again, fine), "the loose node is left as it was and forwards to the finer one");
}

void checkNearPixel() {
	const SceneFile file                 = sceneOrExit(writeScene("near_pixel", R"(
		surface sphere(r) {
			return length(p) - r;
		}

		surface box(size) {
			let q = abs(p) - size / 2;
			return length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0);
		}

		camera {
			eye    = [0, 0, 1];
			target = [1, 0, 1];
			fov    = 0.7;
		}

		at([0, 0, -0.5]) box([400, 400, 1]);
		at([5, 0, 1]) sphere(0.5);
	)"));
	const ObjectBoxes boxes              = objectBoxes(file.scene, 4);
	const Region region                  = boxedRegion(boxes.boxes, file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	Tree tree               = lazyTree(input);
	const Geometry geometry = treeGeometry(file.scene, tree);
	const Ray ray           = betweenSpots({ 0, 0, 1 }, { 5, 0.2, 1.2 }, false);
	const Hit hit           = intersect(geometry, ray);
	const double pixel      = pointTolerance(detail, ray.origin + ray.direction * hit.distance);
	const double side       = std::max({ region.size[0], region.size[1], region.size[2] });
	std::printf("near pixel: in a region %g wide, a sphere 4.5 from the eye is held to %g where a pixel asks for %g, the floor is %g\n", side, hit.loosened, pixel, detail.least);
	check(hit.kind != HitKind::Missed && hit.loosened <= 2 * pixel && hit.loosened < 1e-5 * side, "an object near the eye in a big region gets leaves at a pixel's tolerance, not the region's");
}

SceneFile fieldScene(double place, bool added) {
	const std::string head  = R"(
		surface sphere(r) {
			return length(p) - r;
		}

		surface box(size) {
			let q = abs(p) - size / 2;
			return length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0);
		}

		camera {
			eye    = [0, -40, 25];
			target = [0, 0, 0];
			fov    = 0.6;
		}

		at([0, 0, -0.5]) box([64, 64, 1]);
	)";
	const std::string grid  = R"(
		for i in 0..6 {
			for j in 0..6 {
				at([8 * i - 20, 8 * j - 20, 1]) sphere(1);
			}
		}
	)";
	const std::string moved = "at([" + std::to_string(place) + ", 1.5, 1]) sphere(1);";
	const std::string text  = head + moved + grid + (added ? "at([14, -10, 1.5]) box([2, 2, 3]);" : "");
	return smallScene(writeScene("field", text.c_str()));
}

bool closeHit(const Hit& a, const Hit& b) {
	return a.kind == b.kind && (a.kind == HitKind::Missed || (a.leaf == b.leaf && std::abs(a.distance - b.distance) <= 4 * std::max(a.loosened, b.loosened) + 1e-3));
}

std::size_t apartHits(const std::vector<Hit>& a, const std::vector<Hit>& b) {
	std::size_t apart = 0;
	for (std::size_t k = 0; k < a.size(); ++k) {
		apart += closeHit(a[k], b[k]) ? 0 : 1;
	}
	return apart;
}

struct Followed {
	std::size_t differ;
	std::size_t apart;
	std::size_t hits;
};

Followed followedHits(Tree& kept, SceneFile& file, const SceneFile& next, const std::vector<Ray>& rays) {
	file.scene = next.scene;
	followScene(kept, objectKeys(file.scene));

	const LazyCase made = {
		.file   = file,
		.boxes  = objectBoxes(file.scene, 4),
		.region = kept.region,
		.detail = testDetail(file, kept.region, defaultDetailMembers()),
		.passes = defaultPassMembers(),
	};

	Tree fresh                   = lazyTree(lazyInput(made));
	const std::vector<Hit> wants = hitsOf(treeGeometry(file.scene, fresh), rays);
	const std::vector<Hit> hits  = hitsOf(treeGeometry(file.scene, kept), rays);
	const Shared tally           = differingHits(wants, hits);
	return Followed {
		.differ = tally.differ,
		.apart  = apartHits(wants, hits),
		.hits   = tally.hits,
	};
}

void checkKeptTree() {
	SceneFile file              = fieldScene(-3, false);
	const LazyCase made         = lazyCase(file);
	const std::vector<Ray> rays = lazyRays(file, made.region, true);
	Tree kept                   = lazyTree(lazyInput(made));
	hitsOf(treeGeometry(file.scene, kept), rays);
	const std::uint64_t open  = kept.growth->opened;
	const Followed moved      = followedHits(kept, file, fieldScene(-2.5, false), rays);
	const std::uint64_t reset = kept.growth->reset;
	const Followed added      = followedHits(kept, file, fieldScene(-2.5, true), rays);
	const std::uint64_t more  = kept.growth->reset;
	const Followed removed    = followedHits(kept, file, fieldScene(-2.5, false), rays);
	const std::uint64_t last  = kept.growth->reset;
	const Followed again      = followedHits(kept, file, fieldScene(-2.5, false), rays);
	std::printf("kept tree: of %zu open nodes a moved sphere drops %zu, an added box %zu and its removal %zu; %zu, %zu and %zu of %zu hits differ from a fresh tree's, %zu by more than the tolerance\n", std::size_t(open), std::size_t(reset), std::size_t(more - reset), std::size_t(last - more), moved.differ, added.differ, removed.differ, moved.hits, moved.apart + added.apart + removed.apart);
	check(moved.hits > 3000 && moved.differ == 0, "a kept tree whose scene moved one sphere gives 10,000 rays the hits of a fresh tree");
	check(reset > 0 && 4 * reset < open, "a moved sphere drops far fewer nodes than are open");
	check(more > reset && added.differ == 0, "an object added at the end of the scene leaves the hits of a fresh tree");
	check(last > more && removed.apart == 0, "an object removed from the end of the scene leaves the hits of a fresh tree within the tolerance: the cells it split stay split");
	check(kept.growth->reset == last && again.apart == 0 && again.differ == removed.differ, "a scene loaded again with the same shapes drops no node");
}

bool anyForwarded(const Tree& tree) {
	return std::any_of(tree.nodes.begin(), tree.nodes.end(), [](const PackedNode& node) {
		return NodeState(node.state) == NodeState::Forwarded;
	});
}

void checkCollapse() {
	const SceneFile file                 = smallScene(asideScenePath());
	const ObjectBoxes boxes              = objectBoxes(file.scene, 4);
	const Region region                  = boxedRegion(boxes.boxes, file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	Tree tree               = lazyTree(input);
	const Geometry geometry = treeGeometry(file.scene, tree);
	const Ray blunt         = betweenSpots({ 20, 3, 10 }, { 20, 3, 0 }, false);
	const Ray sharp         = betweenSpots({ 20, 3, 10 }, { 20, 3, 0 }, true);
	intersect(geometry, blunt);
	const Hit fine       = intersect(geometry, sharp);
	const bool forwarded = anyForwarded(tree);
	prepareTree(tree, file.shot, false);
	check(forwarded && !anyForwarded(tree) && tree.frame == 1, "between frames a forwarded node takes its finer node's place");
	check(sameHit(intersect(geometry, sharp), fine) && sameHit(intersect(geometry, blunt), fine), "sharp and other rays meet the finer leaf after the forward was folded");
}

float widestHeld(const std::vector<Hit>& hits) {
	float widest = 0;
	for (const Hit& hit : hits) {
		widest = hit.kind == HitKind::Missed ? widest : std::max(widest, float(hit.loosened));
	}
	return widest;
}

void checkFlownCamera() {
	const SceneFile file        = fieldScene(-3, false);
	const LazyCase made         = lazyCase(file);
	const std::vector<Ray> rays = lazyRays(file, made.region, true);
	Tree fresh                  = lazyTree(lazyInput(made));
	Tree flown                  = lazyTree(lazyInput(made));
	Shot distant                = file.shot;
	distant.camera.eye          = { 0, -400, 250 };
	prepareTree(flown, distant, false);
	const std::vector<Hit> near = hitsOf(treeGeometry(file.scene, fresh), rays);
	const std::vector<Hit> far  = hitsOf(treeGeometry(file.scene, flown), rays);
	prepareTree(flown, file.shot, true);
	const std::size_t reset     = std::size_t(flown.growth->reset);
	const std::vector<Hit> back = hitsOf(treeGeometry(file.scene, flown), rays);
	std::printf("flown camera: cells opened under a camera ten times as far are held to %g, under the scene's to %g; flying back drops %zu of %zu nodes and leaves them held to %g\n", widestHeld(far), widestHeld(near), reset, std::size_t(fresh.growth->opened), widestHeld(back));
	check(widestHeld(far) > 2 * widestHeld(near), "cells opened after the camera flew away are held to the flown camera's view");
	check(reset > 0 && widestHeld(back) <= 2 * widestHeld(near) && differingHits(near, back).differ == 0, "a camera that moves drops the nodes held more than twice as loosely as its view asks, and they are built again to it");
}

std::vector<Ray> everyOtherRay(const std::vector<Ray>& rays, std::size_t first) {
	std::vector<Ray> taken;
	for (std::size_t k = first; k < rays.size(); k += 2) {
		taken.push_back(rays[k]);
	}
	return taken;
}

struct Forgetting {
	std::size_t grown;
	std::size_t held;
	std::size_t nodes;
	std::size_t runs;
	std::uint64_t forgotten;
};

Forgetting forgottenCycle(Tree& tree, const SceneFile& file, const std::vector<Ray>& rays) {
	const Geometry geometry       = treeGeometry(file.scene, tree);
	const std::vector<Ray> camera = everyOtherRay(rays, 0);
	hitsOf(geometry, rays);
	const std::size_t grown = heldBytes(tree);
	prepareTree(tree, file.shot, false);
	for (int frame = 0; frame < 3; ++frame) {
		hitsOf(geometry, camera);
		prepareTree(tree, file.shot, false);
	}
	return Forgetting {
		.grown     = grown,
		.held      = heldBytes(tree),
		.nodes     = tree.nodes.size(),
		.runs      = tree.runs.size(),
		.forgotten = tree.growth->forgotten,
	};
}

void checkForgetting() {
	const SceneFile file        = smallScene("scenes/chess-board.sdf");
	const LazyCase made         = lazyCase(file);
	const std::vector<Ray> rays = lazyRays(made.file, made.region, true);
	Tree fresh                  = lazyTree(lazyInput(made));
	const std::vector<Hit> want = hitsOf(treeGeometry(file.scene, fresh), rays);
	LazyInput tiny              = lazyInput(made);
	tiny.memory                 = 1;
	Tree tree                   = lazyTree(tiny);
	const Forgetting first      = forgottenCycle(tree, file, rays);
	const Forgetting again      = forgottenCycle(tree, file, rays);
	const Shared tally          = differingHits(want, hitsOf(treeGeometry(file.scene, tree), rays));
	std::printf("forgetting: a tree of 1 byte at most holds %.2f MB of the %.2f MB all rays built after 3 frames of camera rays alone; %zu nodes forgotten in two rounds; %zu of %zu hits differ from a fresh tree's; %zu and %zu nodes, %zu and %zu run words after each round\n", double(first.held) / 1048576, double(first.grown) / 1048576, std::size_t(again.forgotten), tally.differ, tally.hits, first.nodes, again.nodes, first.runs, again.runs);
	check(first.forgotten > 0 && first.held < first.grown, "a tree above its memory forgets the nodes no ray entered for 3 frames");
	check(tally.hits > 3000 && tally.differ == 0, "rays into forgotten nodes build them again and get the hits of a fresh tree");
	check(again.forgotten > first.forgotten && again.nodes <= first.nodes && again.runs <= first.runs, "nodes built again write into the spans forgotten ones gave back");
}

Region partRegion(const Region& whole) {
	Region part = whole;
	for (int axis = 0; axis < 3; ++axis) {
		part.lower[axis] = whole.lower[axis] + 0.35 * whole.size[axis];
		part.size[axis]  = 0.4 * whole.size[axis];
	}
	return part;
}

std::size_t farHits(const std::vector<Hit>& a, const std::vector<Hit>& b) {
	std::size_t far = 0;
	for (std::size_t k = 0; k < a.size(); ++k) {
		const bool met  = (a[k].kind == HitKind::Missed) == (b[k].kind == HitKind::Missed);
		const bool near = a[k].kind == HitKind::Missed || (a[k].leaf == b[k].leaf && std::abs(a[k].distance - b[k].distance) <= 4 * std::max(a[k].loosened, b[k].loosened) + 1e-3);
		far += met && near ? 0 : 1;
	}
	return far;
}

bool keptNodes(const Tree& before, const Tree& after, std::uint32_t moved) {
	bool same = std::equal(before.surfaces.begin(), before.surfaces.end(), after.surfaces.begin(), sameSurface) && std::equal(before.runs.begin(), before.runs.end(), after.runs.begin());
	for (std::uint32_t k = 0; k < before.nodes.size(); ++k) {
		const PackedNode& a = before.nodes[k];
		const PackedNode& b = after.nodes[k == before.root.payload ? moved : k];
		same                = same && a.state == b.state && a.lowerKinds == b.lowerKinds && a.upperKinds == b.upperKinds && a.firstBranch == b.firstBranch && a.firstSurface == b.firstSurface && a.firstSolid == b.firstSolid && a.coarse == b.coarse && a.busy == b.busy;
	}
	return same;
}

std::uint32_t formerRootAt(const Tree& tree) {
	const std::uint32_t root = tree.root.payload;
	for (int slot = 0; slot < 64; ++slot) {
		const std::uint32_t child = slotPayload(tree.nodes[root], slot);
		if (tree.growth->records[child].owned.nodes.count > 0) {
			return child;
		}
	}
	return noNode;
}

void checkGrownRegion() {
	const SceneFile file        = smallScene("scenes/chess-board.sdf");
	const LazyCase whole        = lazyCase(file);
	LazyInput input             = lazyInput(whole);
	input.region                = partRegion(whole.region);
	input.depthCap              = depthCapOf(input.region, whole.detail.least);
	const Box held              = regionBox(whole.region);
	const std::vector<Ray> rays = lazyRays(file, whole.region, true);
	Tree grown                  = lazyTree(input);
	growRegion(grown, held);
	openEveryCell(grown, input.workers);
	Tree kept = lazyTree(input);
	openEveryCell(kept, input.workers);
	const Tree before = heldCopy(kept);
	growRegion(kept, held);
	const bool same          = keptNodes(before, kept, formerRootAt(kept));
	const std::uint64_t open = kept.growth->opened;
	const Shared tally       = differingHits(hitsOf(treeGeometry(file.scene, grown), rays), hitsOf(treeGeometry(file.scene, kept), rays));
	Tree fresh               = lazyTree(lazyInput(whole));
	const std::size_t apart  = farHits(hitsOf(treeGeometry(file.scene, fresh), rays), hitsOf(treeGeometry(file.scene, kept), rays));
	std::printf("grown region: %d steps up to a region %.1f times as wide; %zu of %zu hits differ from the tree grown first and opened in full, %zu from a fresh tree's by more than the tolerance; %zu nodes built after the growth\n", int(kept.growth->rooted), kept.region.size[0] / input.region.size[0], tally.differ, tally.hits, apart, std::size_t(kept.growth->opened - open));
	check(kept.growth->rooted > 0 && boxInRegion(held, kept.region) && kept.growth->depthCap == input.depthCap + 2 * int(kept.growth->rooted) && kept.growth->detail.least == whole.detail.least, "a region grows by steps of four until it holds the box, its cells keeping their depth cap and least tolerance");
	check(same && kept.growth->reset == 0, "a grown region keeps every node and leaf built before it grew");
	check(tally.hits > 3000 && tally.differ == 0, "rays get the same hits from a tree that grew after its cells were built as from one that grew before");
	check(apart == 0, "a grown region's hits are a fresh tree's over the whole scene within the tolerance, a cell inside an object kept whole or as a surface leaf");
}

void checkGrownFollow() {
	SceneFile file      = fieldScene(-3, false);
	const LazyCase made = lazyCase(file);
	Tree kept           = lazyTree(lazyInput(made));
	hitsOf(treeGeometry(file.scene, kept), lazyRays(file, made.region, true));
	file.scene = fieldScene(60, false).scene;
	followScene(kept, objectKeys(file.scene));
	const Box held = reachedHull(kept.growth->boxes.boxes, file.shot);
	growRegion(kept, held);
	const std::vector<Ray> rays = lazyRays(file, kept.region, true);
	const std::vector<Ray> out  = { betweenSpots({ 60, 1.5, 10 }, { 60, 1.5, 0 }, true) };
	const LazyCase after        = {
		.file   = file,
		.boxes  = objectBoxes(file.scene, 4),
		.region = made.region,
		.detail = testDetail(file, made.region, defaultDetailMembers()),
		.passes = made.passes,
	};

	Tree grown = lazyTree(lazyInput(after));
	growRegion(grown, held);
	const Shared tally      = differingHits(hitsOf(treeGeometry(file.scene, grown), rays), hitsOf(treeGeometry(file.scene, kept), rays));
	const std::size_t apart = apartHits(hitsOf(treeGeometry(file.scene, grown), rays), hitsOf(treeGeometry(file.scene, kept), rays));
	const Hit moved         = intersect(treeGeometry(file.scene, kept), out[0]);
	std::printf("grown region after a change: a sphere moved out of the region is met at %.4f after %d steps up; %zu of %zu hits differ from a tree made with it and grown alike, %zu by more than the tolerance\n", moved.distance, int(kept.growth->rooted), tally.differ, tally.hits, apart);
	check(moved.kind != HitKind::Missed && std::abs(moved.distance - 8) < 1e-2, "a sphere moved out of the region is met once the region grows to it");
	check(tally.hits > 3000 && apart == 0, "a kept tree that followed a sphere out of its region and grew gives the hits of a tree made with the change and grown alike within the tolerance: the cells the sphere split stay split");
}

SceneFile reachScene() {
	return smallScene(writeScene("reach", R"(
		render {
			far = 20;
		}

		surface sphere(r) {
			return length(p) - r;
		}

		surface box(size) {
			let q = abs(p) - size / 2;
			return length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0);
		}

		camera {
			eye    = [0, 0, 1];
			target = [0, 10, 1];
			fov    = 0.6;
		}

		at([0, 0, -0.5]) box([200, 200, 1]);
		at([15, 15, 1]) sphere(1);
		at([0, -10, 1]) sphere(1);
		at([0, 10, 1]) sphere(1);
	)"));
}

double hitDistance(const Geometry& geometry, const Spot& from, const Spot& to) {
	const Hit hit = intersect(geometry, betweenSpots(from, to, true));
	return hit.kind == HitKind::Missed ? -1 : hit.distance;
}

void checkReachSphere() {
	const SceneFile file   = reachScene();
	const LazyCase made    = lazyCase(file);
	Tree uncut             = lazyTree(lazyInput(made));
	const double unreached = hitDistance(treeGeometry(file.scene, uncut), { 0, 0, 1 }, { 15, 15, 1 });
	Tree kept              = lazyTree(lazyInput(made));
	moveReach(kept, shotReach(file.shot));
	const Geometry geometry = treeGeometry(file.scene, kept);
	const double corner     = hitDistance(geometry, { 0, 0, 1 }, { 15, 15, 1 });
	const double behind     = hitDistance(geometry, { 0, 0, 1 }, { 0, -10, 1 });
	const double ahead      = hitDistance(geometry, { 0, 0, 1 }, { 0, 10, 1 });
	const double near       = hitDistance(geometry, { 0, 0, 1 }, { 5, 15, 0 });
	const double beyond     = hitDistance(geometry, { 0, 0, 1 }, { 5, 25, 0 });
	std::vector<Ray> rays   = lazyRays(file, made.region, true);
	rays.push_back(betweenSpots({ 8, 8, 1 }, { 15, 15, 1 }, true));
	hitsOf(geometry, rays);
	const std::uint64_t built = kept.growth->opened;
	Shot moved                = file.shot;
	moved.camera.eye          = { 8, 8, 1 };
	moveReach(kept, shotReach(moved));
	Tree fresh = lazyTree(lazyInput(made));
	moveReach(fresh, shotReach(moved));
	const std::vector<Hit> wants = hitsOf(treeGeometry(file.scene, fresh), rays);
	const std::vector<Hit> hits  = hitsOf(geometry, rays);
	const Shared tally           = differingHits(wants, hits);
	const std::size_t apart      = apartHits(wants, hits);
	std::printf("reach sphere: the corner sphere is met at %.3f without the reach and %.3f with it, the one behind the eye at %.3f, ahead at %.3f, the ground at %.3f and %.3f; moving the eye drops %zu of %zu nodes, the corner sphere is met at %.3f, %zu of %zu hits differ from a fresh tree's, %zu by more than the tolerance\n", unreached, corner, behind, ahead, near, beyond, std::size_t(kept.growth->reset), std::size_t(built), hits.back().distance, tally.differ, tally.hits, apart);
	check(unreached > 0 && corner < 0, "an object inside the reach's bounding cube but beyond the sphere of far about the eye is cut away");
	check(std::abs(behind - 9) < 1e-2 && std::abs(ahead - 9) < 1e-2, "an object behind the eye within far is kept, as is one ahead");
	check(std::abs(near - std::sqrt(251.0)) < 1e-2 && beyond < 0, "a ground that runs on is cut off at far from the eye");
	check(kept.growth->reset > 0 && hits.back().kind != HitKind::Missed, "a moved eye drops the nodes along the reach's edge and lets in what it now reaches");
	check(tally.hits > 3000 && apart == 0, "a kept tree whose reach moved gives the hits of a fresh tree made at the new reach");
}

void checkReachTiles() {
	const SceneFile file                 = smallScene(writeScene("reach_tiles", R"(
		render {
			far = 30;
		}

		camera {
			eye    = [0, 0, 1];
			target = [0, 1000, 1];
			fov    = 0.6;
		}

		surface ball(r) {
			return length(p) - r;
		}

		tiles(step = [10, 10, 10]) {
			if tile.z == 0 {
				at([5, 5, 1]) ball(1);
			}
		}
	)"));
	const ObjectBoxes boxes              = objectBoxes(file.scene, 1);
	const Region region                  = boxedRegion(heldBoxes(boxes.boxes, file.scene), file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	Tree tree = lazyTree(input);
	moveReach(tree, shotReach(file.shot));
	const Geometry geometry = treeGeometry(file.scene, tree);
	const double ahead      = hitDistance(geometry, { 0, 0, 1 }, { 5, 5, 1 });
	const double behind     = hitDistance(geometry, { 0, 0, 1 }, { -5, -5, 1 });
	const double aside      = hitDistance(geometry, { 0, 0, 1 }, { 15, 5, 1 });
	const double beyond     = hitDistance(geometry, { 0, 0, 1 }, { 35, 5, 1 });
	std::printf("reach tiles: with the target 1000 ahead, tile balls are met at %.3f ahead, %.3f behind, %.3f aside and %.3f beyond far\n", ahead, behind, aside, beyond);
	check(std::abs(ahead - (std::sqrt(50.0) - 1)) < 1e-2 && std::abs(behind - (std::sqrt(50.0) - 1)) < 1e-2 && std::abs(aside - (std::sqrt(250.0) - 1)) < 1e-2, "tiles are made about the eye, not the far target: balls near the eye are met ahead, behind and aside");
	check(beyond < 0, "a tile's ball beyond far from the eye is not met");
}

void checkReachedTiles() {
	const SceneFile file                 = smallScene(writeScene("reached_tiles", R"(
		render {
			far = 25;
		}

		camera {
			eye    = [0, 0, 1];
			target = [1000, 1000, 1];
			fov    = 0.6;
		}

		surface ball(r) {
			return length(p) - r;
		}

		tiles(step = [10, 10, 10]) {
			if tile.z == 0 {
				at([2, 2, 1]) ball(1);
			}
		}
	)"));
	const ObjectBoxes boxes              = objectBoxes(file.scene, 1);
	const Region region                  = boxedRegion(heldBoxes(boxes.boxes, file.scene), file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	Tree tree = lazyTree(input);
	moveReach(tree, shotReach(file.shot));
	const Geometry geometry = treeGeometry(file.scene, tree);
	const double above      = hitDistance(geometry, { 0, 0, 5 }, { 22, 22, 5 });
	Shot moved              = file.shot;
	moved.camera.eye        = { 8, 8, 1 };
	moveReach(tree, shotReach(moved));
	const double reached = hitDistance(geometry, { 16, 22, 1.5 }, { 22, 22, 1.5 });
	std::printf("reached tiles: a ray over the tile balls meets nothing (%.3f), and one at a ball beyond far meets it at %.3f once the eye moved toward it\n", above, reached);
	check(above < 0 && std::abs(reached - (6 - std::sqrt(0.75))) < 1e-2, "a node that ran its tiles under the old reach runs the tiles the moved reach meets anew");
}

SceneFile shownTileScene(int shown) {
	const std::string text = "let SHOWN = " + std::to_string(shown) + R"(;

		render {
			far = 30;
		}

		camera {
			eye    = [0, 0, 1];
			target = [0, 1000, 1];
			fov    = 0.6;
		}

		surface ball(r) {
			return length(p) - r;
		}

		tiles(step = [10, 10, 10]) {
			if tile.z == 0 {
				if SHOWN > 0 {
					at([5, 5, 1]) ball(1);
				}
			}
		}
	)";
	return smallScene(writeScene("shown_tiles", text.c_str()));
}

std::size_t freedSlotLeaves(const Tree& tree) {
	std::size_t named = 0;
	for (const PlacedLeaf& leaf : placedLeaves(tree)) {
		const std::uint32_t list = leaf.entry.kind == CellKind::Surface ? tree.surfaces[leaf.entry.payload].programs : tree.solids[leaf.entry.payload].programs;
		for (std::uint32_t k = 0; k < tree.lists[list].count; ++k) {
			named += tree.growth->slots[referencedSlot(tree, tree.references[tree.lists[list].first + k])].object == noObject ? 1 : 0;
		}
	}
	return named;
}

void checkFollowedTiles() {
	SceneFile file                       = shownTileScene(1);
	const ObjectBoxes boxes              = objectBoxes(file.scene, 1);
	const Region region                  = boxedRegion(heldBoxes(boxes.boxes, file.scene), file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = false,
	};

	Tree tree = lazyTree(input);
	moveReach(tree, shotReach(file.shot));
	const Geometry geometry = treeGeometry(file.scene, tree);
	const double shown      = hitDistance(geometry, { 0, 0, 1 }, { 5, 5, 1 });
	file.scene              = shownTileScene(0).scene;
	followScene(tree, objectKeys(file.scene));
	const std::size_t named = freedSlotLeaves(tree);
	const double hidden     = named == 0 ? hitDistance(geometry, { 0, 0, 1 }, { 5, 5, 1 }) : 0;
	std::printf("followed tiles: a tile ball is met at %.3f, and after a binding the tile read hides it, %zu leaves name a freed slot and it is met at %.3f\n", shown, named, hidden);
	check(shown > 0 && named == 0 && hidden < 0, "a tile that runs again without an object drops the nodes below the tile level that held it, though the nodes above list no tile object");
}

std::vector<std::string> gridShapes() {
	std::vector<std::string> shapes = { "at([0, 0, -0.5]) box([64, 64, 1]);" };
	for (int k = 0; k < 36; ++k) {
		shapes.push_back("at([" + std::to_string(8 * (k % 6) - 20) + ", " + std::to_string(8 * (k / 6) - 20) + ", 1]) sphere(" + std::to_string(1 + 0.25 * (k % 2)) + ");");
	}
	return shapes;
}

SceneFile gridScene(const std::vector<std::string>& shapes) {
	std::string text = R"(
		surface sphere(r) {
			return length(p) - r;
		}

		surface box(size) {
			let q = abs(p) - size / 2;
			return length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0);
		}

		camera {
			eye    = [0, -40, 25];
			target = [0, 0, 0];
			fov    = 0.6;
		}
	)";
	for (const std::string& shape : shapes) {
		text += shape + "\n";
	}
	return smallScene(writeScene("grid", text.c_str()));
}

std::vector<std::string> withoutShape(const std::vector<std::string>& shapes, std::size_t dropped) {
	std::vector<std::string> kept = shapes;
	kept.erase(kept.begin() + std::ptrdiff_t(dropped));
	return kept;
}

std::vector<std::string> shapeLast(const std::vector<std::string>& shapes, std::size_t moved) {
	std::vector<std::string> last = withoutShape(shapes, moved);
	last.push_back(shapes[moved]);
	return last;
}

bool sameLeafPlace(const Scene& a, std::uint32_t one, const Scene& b, std::uint32_t other) {
	if (one == noLeaf || other == noLeaf) {
		return one == other;
	}
	const double* p = a.leaves[one].placement.origin;
	const double* q = b.leaves[other].placement.origin;
	return p[0] == q[0] && p[1] == q[1] && p[2] == q[2];
}

std::size_t movedHits(const std::vector<Hit>& before, const Scene& was, const std::vector<Hit>& after, const Scene& is) {
	std::size_t differ = 0;
	for (std::size_t k = 0; k < before.size(); ++k) {
		Hit named         = after[k];
		named.leaf        = before[k].leaf;
		named.other       = before[k].other;
		const bool leaves = before[k].kind == HitKind::Missed || (sameLeafPlace(was, before[k].leaf, is, after[k].leaf) && sameLeafPlace(was, before[k].other, is, after[k].other));
		differ += sameHit(before[k], named) && leaves ? 0 : 1;
	}
	return differ;
}

std::vector<std::uint32_t> builtBelow(const Tree& tree, std::uint32_t node) {
	const NodeState state = NodeState(tree.nodes[node].state);
	if (state != NodeState::Loose && state != NodeState::Fine) {
		return {};
	}
	std::vector<std::uint32_t> built = { node };
	const Span children              = tree.growth->records[node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		const std::vector<std::uint32_t> below = builtBelow(tree, child);
		built.insert(built.end(), below.begin(), below.end());
	}
	return built;
}

std::vector<std::uint32_t> builtAway(const Tree& tree, const Box& box) {
	std::vector<std::uint32_t> away;
	for (const std::uint32_t node : builtBelow(tree, tree.root.payload)) {
		if (!boxesMeet(nodeBox(tree, node), box)) {
			away.push_back(node);
		}
	}
	return away;
}

bool stillBuilt(const Tree& tree, const std::vector<std::uint32_t>& nodes) {
	return std::all_of(nodes.begin(), nodes.end(), [&](std::uint32_t node) {
		return NodeState(tree.nodes[node].state) == NodeState::Loose || NodeState(tree.nodes[node].state) == NodeState::Fine;
	});
}

void checkReorderedScene() {
	std::vector<std::string> shapes = gridShapes();
	SceneFile file                  = gridScene(shapes);
	const Scene was                 = file.scene;
	const LazyCase made             = lazyCase(file);
	const std::vector<Ray> rays     = lazyRays(file, made.region, true);
	Tree kept                       = lazyTree(lazyInput(made));
	const std::vector<Hit> hits     = hitsOf(treeGeometry(file.scene, kept), rays);
	const std::uint64_t open        = kept.growth->opened;
	std::reverse(shapes.begin(), shapes.end());
	file.scene = gridScene(shapes).scene;
	followScene(kept, objectKeys(file.scene));
	const std::vector<Hit> again = hitsOf(treeGeometry(file.scene, kept), rays);
	const std::size_t differ     = movedHits(hits, was, again, file.scene);
	const bool turned            = kept.growth->slots[0].object == std::uint32_t(file.scene.objectPrograms.size() - 1);
	std::printf("reordered scene: the same shapes listed back to front drop %zu nodes and build %zu; %zu of %zu hits differ\n", std::size_t(kept.growth->reset), std::size_t(kept.growth->opened - open), differ, hits.size());
	check(turned && kept.growth->reset == 0 && kept.growth->opened == open, "a scene that lists its objects in another order keeps every slot's object and drops no node");
	check(differ == 0, "a scene that lists its objects in another order gives every ray the hit it had, bit for bit");
}

std::uint64_t removalResets(const std::vector<std::string>& shapes, std::size_t dropped, const std::vector<Ray>& rays) {
	SceneFile file      = gridScene(shapes);
	const LazyCase made = lazyCase(file);
	Tree kept           = lazyTree(lazyInput(made));
	hitsOf(treeGeometry(file.scene, kept), rays);
	file.scene = gridScene(withoutShape(shapes, dropped)).scene;
	followScene(kept, objectKeys(file.scene));
	return kept.growth->reset;
}

void checkRemovedSlot() {
	const std::vector<std::string> shapes = gridShapes();
	SceneFile file                        = gridScene(shapes);
	const LazyCase made                   = lazyCase(file);
	const std::vector<Ray> rays           = lazyRays(file, made.region, true);
	Tree kept                             = lazyTree(lazyInput(made));
	hitsOf(treeGeometry(file.scene, kept), rays);
	const std::size_t slots   = kept.growth->slots.size();
	const std::size_t built   = builtBelow(kept, kept.root.payload).size();
	const Followed removed    = followedHits(kept, file, gridScene(withoutShape(shapes, 15)), rays);
	const bool freed          = kept.growth->slots.size() == slots && kept.growth->slots[15].object == noObject && kept.growth->slots[16].object == 15;
	const std::uint64_t reset = kept.growth->reset;
	const std::uint64_t last  = removalResets(shapeLast(shapes, 15), shapes.size() - 1, rays);
	const Followed added      = followedHits(kept, file, gridScene(shapes), rays);
	const bool taken          = kept.growth->slots.size() == slots && kept.growth->slots[15].object == 15;
	std::printf("removed slot: of %zu built nodes, a sphere removed from the middle of the scene drops %zu, from its end %zu, and its return %zu; %zu and %zu hits differ from a fresh tree's by more than the tolerance\n", built, std::size_t(reset), std::size_t(last), std::size_t(kept.growth->reset - reset), removed.apart, added.apart);
	check(freed && reset > 0 && 4 * reset < built, "an object removed from the middle of the scene frees its slot alone and drops few nodes");
	check(reset == last, "an object removed from the middle of the scene drops the nodes its removal from the end drops");
	check(removed.apart == 0 && added.apart == 0, "a kept tree that lost an object and got it back gives the hits of a fresh tree within the tolerance");
	check(taken, "an object added later takes the freed slot");
}

LazyCase tiledCase(const SceneFile& file) {
	const ObjectBoxes boxes = objectBoxes(file.scene, 1);
	const Region region     = boxedRegion(heldBoxes(boxes.boxes, file.scene), file.shot);
	return LazyCase {
		.file   = file,
		.boxes  = boxes,
		.region = region,
		.detail = testDetail(file, region, defaultDetailMembers()),
		.passes = defaultPassMembers(),
	};
}

std::string boundedTileText(const std::string& far, const std::string& grid) {
	return "render {\n\tfar = " + far + ";\n}\n\ncamera {\n\teye    = [0, 0, 3];\n\ttarget = [1000, 0, 3];\n\tfov    = 0.6;\n}\n\nsurface ball(r) {\n\treturn length(p) - r;\n}\n\ntiles(" + grid + ") {\n\tat([5, 5, 1]) ball(1);\n}\n";
}

bool heldWithin(const Tree& tree, const TileBlock& block) {
	for (const auto& [place, number] : tree.growth->tiles.numbers) {
		for (int axis = 0; axis < 3; ++axis) {
			if (place[std::size_t(axis + 1)] < block.from[axis] || place[std::size_t(axis + 1)] > block.to[axis]) {
				return false;
			}
		}
	}
	return true;
}

void checkBoundedTiles() {
	const SceneFile file = smallScene(writeScene("bounded_reach", boundedTileText("100", "step = [10, 10, 10], from = [0, -1, 0], to = [2, 0, 0]").c_str()));
	const LazyCase made  = tiledCase(file);
	Tree tree            = lazyTree(lazyInput(made));
	moveReach(tree, shotReach(file.shot));
	const Geometry geometry = treeGeometry(file.scene, tree);
	const double inside     = hitDistance(geometry, { 0, 0, 1 }, { 25, 5, 1 });
	const double below      = hitDistance(geometry, { 0, 0, 1 }, { 5, -5, 1 });
	const double past       = hitDistance(geometry, { 0, 0, 1 }, { 35, 5, 1 });
	const double beside     = hitDistance(geometry, { 0, 0, 1 }, { 5, 15, 1 });
	const double above      = hitDistance(geometry, { 0, 0, 11 }, { 5, 5, 11 });
	hitsOf(geometry, lazyRays(file, made.region, false));
	std::printf("bounded tiles: balls in bounded tiles are met at %.3f and %.3f, ones past, beside and above the bounds at %.3f, %.3f and %.3f; %zu tiles held\n", inside, below, past, beside, above, tree.growth->tiles.numbers.size());
	check(std::abs(inside - (std::sqrt(625.0 + 25.0) - 1)) < 1e-2 && std::abs(below - (std::sqrt(50.0) - 1)) < 1e-2, "tiles within a block's bounds run");
	check(past < 0 && beside < 0 && above < 0 && heldWithin(tree, file.scene.tiles[0]) && tree.growth->tiles.numbers.size() <= 6, "tiles outside a block's bounds never run");
	const SceneFile moved = sceneOrExit(writeScene("bounded_moved", boundedTileText("100", "step = [10, 10, 10], from = [0, -1, 0], to = [3, 0, 0]").c_str()));
	check(sameTileSteps(tree.growth->tiles, file.scene) && !sameTileSteps(tree.growth->tiles, moved.scene), "other bounds make the tree again");
}

std::vector<Ray> skyRays() {
	std::vector<Ray> rays;
	for (int k = 0; k < 200; ++k) {
		const double turn = 0.0314 * k;
		const double rise = 0.002 + 0.4 * (k % 20) / 20.0;
		rays.push_back(betweenSpots({ 0, 0, 3 }, { 3 + std::cos(turn), std::sin(turn), 3 + rise }, false));
	}
	return rays;
}

std::uint64_t skyNodes(const char* name, const std::string& grid) {
	const SceneFile file = smallScene(writeScene(name, boundedTileText("1000", grid).c_str()));
	const LazyCase made  = tiledCase(file);
	Tree tree            = lazyTree(lazyInput(made));
	moveReach(tree, shotReach(file.shot));
	hitsOf(treeGeometry(file.scene, tree), skyRays());
	return tree.growth->opened;
}

void checkBoundedSky() {
	const std::uint64_t open    = skyNodes("open_sky", "step = [4, 4, 100]");
	const std::uint64_t bounded = skyNodes("bounded_sky", "step = [4, 4, 100], from = [-2, -2, 0], to = [1, 1, 0]");
	std::printf("bounded sky: 200 rays into the sky over tiles 4 m wide with far 1000 open %llu nodes without bounds and %llu with the tiles bounded to 16 m x 16 m x 100 m\n", (unsigned long long)open, (unsigned long long)bounded);
	check(bounded * 20 < open, "cells outside a tiles block's bounds are not split down to the tiles, however far the reach");
}

SceneFile walkedTileScene(int frame) {
	const std::string text = "let LOD = " + std::to_string(frame / 3 % 2) + ";\n\nrender {\n\tfar = 30;\n}\n\ncamera {\n\teye    = [" + std::to_string(6 * frame) + R"(, 0, 3];
	target = [1000, 0, 3];
	fov    = 0.6;
}

surface ball(r) {
	return length(p) - r;
}

tiles(step = [10, 10, 10]) {
	if tile.z == 0 {
		at([5, 5, 1]) ball(1 + 0.25 * LOD);
		at([2, 2, 1]) ball(0.5);
	}
}
)";
	return smallScene(writeScene("walked_tiles", text.c_str()));
}

std::size_t tileRecordsBytes(const HeldTiles& tiles) {
	std::size_t bytes = 0;
	for (const std::unique_ptr<HeldTile>& tile : tiles.tiles) {
		bytes += tile ? tile->bytes + tileRecordBytes : 0;
	}
	return bytes;
}

std::vector<Ray> walkedRays(const SceneFile& file) {
	const Lens lens = lensOf(file.shot.camera, file.shot.render);
	std::vector<Ray> rays;
	for (int k = 0; k < 400; ++k) {
		rays.push_back(pixelRay(lens, k * 37 % lens.width, k * 53 % lens.height));
	}
	return rays;
}

void checkWalkedTiles() {
	SceneFile file      = walkedTileScene(0);
	const LazyCase made = tiledCase(file);
	LazyInput input     = lazyInput(made);
	input.memory        = 1;
	Tree tree           = lazyTree(input);
	std::vector<std::size_t> held;
	std::vector<std::size_t> bytes;
	bool counted = true;
	for (int frame = 0; frame < 40; ++frame) {
		SceneFile next = walkedTileScene(frame);
		carriedTiles(file.scene, next.scene);
		file.scene = std::move(next.scene);
		file.shot  = next.shot;
		followScene(tree, objectKeys(file.scene));
		growRegion(tree, reachedHull(heldBoxes(tree.growth->boxes.boxes, file.scene), file.shot));
		moveReach(tree, shotReach(file.shot));
		hitsOf(treeGeometry(file.scene, tree), walkedRays(file));
		prepareTree(tree, file.shot, true);
		held.push_back(tree.growth->tiles.numbers.size());
		bytes.push_back(heldBytes(tree));
		counted = counted && tree.growth->tiles.bytes == tileRecordsBytes(tree.growth->tiles);
	}
	const std::size_t early     = *std::max_element(held.begin() + 10, held.begin() + 20);
	const std::size_t late      = *std::max_element(held.begin() + 30, held.end());
	const std::size_t earlyHeld = *std::max_element(bytes.begin() + 10, bytes.begin() + 20);
	const std::size_t lateHeld  = *std::max_element(bytes.begin() + 30, bytes.end());
	std::printf("walked tiles: an eye walking 240 m over endless tiles with far 30, their shapes changing every 3 frames, holds at most %zu tiles and %.1f kB in frames 10-19 and %zu tiles and %.1f kB in frames 30-39\n", early, double(earlyHeld) / 1024, late, double(lateHeld) / 1024);
	check(counted, "bytes.tiles counts every tile the tree knows, the dropped ones too");
	check(late <= early + early / 4 && lateHeld <= earlyHeld + earlyHeld / 4, "the tiles and bytes a tree holds stay flat while its reach walks on and its tiles run again");
}

bool tileStepsRefused(const char* name, const char* step) {
	const std::string text               = std::string("surface ball(r) {\n\treturn length(p) - r;\n}\n\ntiles(step = ") + step + ") {\n\tif tile.z == 0 {\n\t\tat([0.5, 0.5, 0.5]) ball(0.2);\n\t}\n}\n";
	const SceneFile file                 = sceneOrExit(writeScene(name, text.c_str()));
	const ObjectBoxes boxes              = objectBoxes(file.scene, 1);
	const Region region                  = boxedRegion(heldBoxes(boxes.boxes, file.scene), file.shot);
	const Detail detail                  = testDetail(file, region, defaultDetailMembers());
	const std::vector<PassMember> passes = defaultPassMembers();

	const LazyInput input = {
		.scene     = file.scene,
		.detail    = detail,
		.passes    = passes,
		.boxes     = boxes,
		.region    = region,
		.depthCap  = depthCapOf(region, detail.least),
		.workers   = 1,
		.faces     = true,
		.memory    = std::size_t(1) << 40,
		.forgiving = true,
	};

	const Tree tree = lazyTree(input);
	return tree.growth->tiles.refused && detail.least > 0;
}

void checkTileSteps() {
	check(tileStepsRefused("narrow_tiles", "0.001"), "a tile's step below 16 least tolerances is refused, and the viewer then runs no tile");
	check(!tileStepsRefused("wide_tiles", "100"), "a tile's step of many least tolerances is taken");
}

void checkTree() {
	checkObjectBoxes();
	checkLazyHits();
	checkLazyThreads();
	checkSharpRefines();
	checkNearPixel();
	checkKeptTree();
	checkCollapse();
	checkFlownCamera();
	checkForgetting();
	checkGrownRegion();
	checkGrownFollow();
	checkReachSphere();
	checkReachTiles();
	checkReachedTiles();
	checkFollowedTiles();
	checkReorderedScene();
	checkRemovedSlot();
	checkSharedPasses();
	checkCutPass();
	checkSplitHidden();
	checkClassifySphere();
	checkClassifyBlend();
	checkClassifyCrowded();
	checkPatchSkip();
	checkSurfaceless();
	checkSeamPair();
	checkPackRoundTrip();
	checkDeterministic();
	checkPatchFit();
	checkPatchSlab();
	checkFaceSlabs();
	checkFocusOnView();
	checkObjectPrograms("scenes/magic-cube.sdf");
	checkObjectPrograms("scenes/chess-board.sdf");
	checkCuts();
	checkTileSteps();
	checkBoundedTiles();
	checkBoundedSky();
	checkWalkedTiles();
}

}
