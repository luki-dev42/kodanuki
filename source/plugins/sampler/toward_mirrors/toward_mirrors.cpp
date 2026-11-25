#include "plugins/sampler/toward_mirrors/toward_mirrors.h"

#include "plugins/geometry/tree/reach.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

namespace kodanuki {

constexpr double leastOfRange = 1e-4;
constexpr int finestLevels    = 12;
constexpr int reachLevels     = 6;

struct Relaying {
	const Geometry& geometry;
	std::vector<Program> programs;
	std::vector<std::vector<std::uint32_t>> objects;
	std::vector<double> blends;
	double least;
	double finest;
};

struct Grown {
	std::vector<Box> boxes;
	std::vector<bool> grown;
};

struct RelayCell {
	std::uint32_t leaf;
	double stray;
};

struct RelayCone {
	math::Vector axis;
	double fall;
};

MirrorBalls noMirrorBalls() {
	return MirrorBalls {
		.balls = {},
	};
}

bool sharpOrLaddered(const Looks& looks, const Materials& materials, std::uint32_t material) {
	const bool laddered = sceneRoughens(looks) && looks.materials[material].kind == MaterialKind::Metal;
	return looks.materials[material].relays && (isSharp(materials, material) || laddered);
}

bool mayRelay(const Looks& looks, const Materials& materials, const Leaf& leaf) {
	if ((leaf.rules & std::uint8_t(Rule::Causticless)) != 0) {
		return false;
	}
	if (leaf.coats.empty()) {
		return looks.materials[leaf.material].relays && isSharp(materials, leaf.material);
	}
	if (sharpOrLaddered(looks, materials, leaf.material)) {
		return true;
	}
	for (const std::uint32_t coat : leaf.coats) {
		const Coat& layer = looks.coats[coat];
		if (layer.effect == CoatEffect::Paint && sharpOrLaddered(looks, materials, layer.material)) {
			return true;
		}
	}
	return false;
}

std::vector<std::uint32_t> namedRelays(const std::vector<Step>& steps, const std::vector<bool>& relays) {
	std::vector<std::uint32_t> named;
	for (const Step& step : steps) {
		if (step.leaf != noLeaf && relays[step.leaf]) {
			named.push_back(step.leaf);
		}
	}
	std::sort(named.begin(), named.end());
	named.erase(std::unique(named.begin(), named.end()), named.end());
	return named;
}

std::vector<std::vector<std::uint32_t>> namedByPrograms(const std::vector<Program>& programs, const std::vector<bool>& relays) {
	std::vector<std::vector<std::uint32_t>> named;
	for (const Program& program : programs) {
		named.push_back(namedRelays(program.steps, relays));
	}
	return named;
}

void reachedBlends(const Scene& scene, std::uint32_t node, double reach, std::vector<double>& blends) {
	const Node& at = scene.nodes[node];
	if (at.join == Join::Leaf) {
		blends[at.leaf] = std::max(blends[at.leaf], reach);
		return;
	}
	for (const std::uint32_t child : at.children) {
		reachedBlends(scene, child, std::max(reach, at.radius), blends);
	}
}

std::vector<double> leafBlends(const Scene& scene) {
	std::vector<double> blends(scene.leaves.size(), 0.0);
	for (const std::uint32_t object : scene.objects) {
		reachedBlends(scene, object, 0.0, blends);
	}
	return blends;
}

double regionSide(const Region& region) {
	return std::max({ region.size[0], region.size[1], region.size[2] });
}

double leastTolerance(const Tree& tree) {
	return leastOfRange * std::ldexp(regionSide(tree.region), -grownLevels(tree));
}

Program noProgram() {
	return Program {
		.name      = {},
		.steps     = {},
		.result    = 0,
		.arguments = 0,
	};
}

std::vector<Program> relayPrograms(const Scene& scene, const std::vector<bool>& relays) {
	std::vector<Program> programs(relays.size(), noProgram());
	for (std::uint32_t leaf = 0; leaf < relays.size(); ++leaf) {
		programs[leaf] = relays[leaf] ? leafProgram(scene, leaf) : programs[leaf];
	}
	return programs;
}

Relaying relayingOf(const Geometry& geometry, const std::vector<bool>& relays) {
	return Relaying {
		.geometry = geometry,
		.programs = relayPrograms(geometry.scene, relays),
		.objects  = namedByPrograms(geometry.scene.objectPrograms, relays),
		.blends   = leafBlends(geometry.scene),
		.least    = leastTolerance(treeOf(geometry)),
		.finest   = std::ldexp(regionSide(treeOf(geometry).region), -finestLevels - grownLevels(treeOf(geometry))),
	};
}

bool holdsBox(const Box& outer, const Box& inner) {
	for (int axis = 0; axis < 3; ++axis) {
		if (inner.axes[axis].lower < outer.axes[axis].lower || inner.axes[axis].upper > outer.axes[axis].upper) {
			return false;
		}
	}
	return true;
}

Box joinedBox(const Box& a, const Box& b) {
	Box joined = a;
	for (int axis = 0; axis < 3; ++axis) {
		joined.axes[axis].lower = std::min(a.axes[axis].lower, b.axes[axis].lower);
		joined.axes[axis].upper = std::max(a.axes[axis].upper, b.axes[axis].upper);
	}
	return joined;
}

bool nearSurface(const Relaying& relaying, const RelayCell& cell, const Box& box) {
	const Program& program                     = relaying.programs[cell.leaf];
	const std::vector<math::Interval> interval = intervalsOver(program, box);
	const double slack                         = relaying.blends[cell.leaf] + relaying.least + cell.stray;
	return interval[program.result].lower <= slack && interval[program.result].upper >= -slack;
}

double largestSide(const Box& box) {
	return std::max({ box.axes[0].upper - box.axes[0].lower, box.axes[1].upper - box.axes[1].lower, box.axes[2].upper - box.axes[2].lower });
}

void growBox(Grown& grown, std::uint32_t leaf, const Box& box) {
	grown.boxes[leaf] = grown.grown[leaf] ? joinedBox(grown.boxes[leaf], box) : box;
	grown.grown[leaf] = true;
}

void growNear(const Relaying& relaying, const RelayCell& cell, const Box& box, Grown& grown) {
	if (grown.grown[cell.leaf] && holdsBox(grown.boxes[cell.leaf], box)) {
		return;
	}
	if (!nearSurface(relaying, cell, box)) {
		return;
	}
	if (largestSide(box) <= relaying.finest) {
		growBox(grown, cell.leaf, box);
		return;
	}
	for (int octant = 0; octant < 8; ++octant) {
		growNear(relaying, cell, octantBox(box, octant), grown);
	}
}

void growObject(const Relaying& relaying, std::uint32_t object, Grown& grown) {
	if (relaying.objects[object].empty()) {
		return;
	}
	for (const Box& box : fieldBoxes(relaying.geometry.scene.objectPrograms[object], treeOf(relaying.geometry).region, reachLevels + grownLevels(treeOf(relaying.geometry)))) {
		for (const std::uint32_t leaf : relaying.objects[object]) {
			const RelayCell cell = {
				.leaf  = leaf,
				.stray = looseStray(treeOf(relaying.geometry), box),
			};

			growNear(relaying, cell, box, grown);
		}
	}
}

Box emptyBox() {
	const math::Interval none = {
		.lower = 0,
		.upper = 0,
	};

	return Box {
		.axes = { none, none, none },
	};
}

Ball ballAround(const Box& box) {
	const math::Vector low  = { box.axes[0].lower, box.axes[1].lower, box.axes[2].lower };
	const math::Vector high = { box.axes[0].upper, box.axes[1].upper, box.axes[2].upper };
	return Ball {
		.middle = (low + high) * 0.5,
		.radius = 0.5 * math::length(high - low),
	};
}

MirrorBalls ballsOf(const Grown& grown) {
	MirrorBalls mirrors = noMirrorBalls();
	for (std::size_t leaf = 0; leaf < grown.boxes.size(); ++leaf) {
		if (grown.grown[leaf]) {
			mirrors.balls.push_back(ballAround(grown.boxes[leaf]));
		}
	}
	return mirrors;
}

MirrorBalls relayingBalls(const Geometry& geometry, const Looks& looks, const Materials& materials) {
	std::vector<bool> relays(geometry.scene.leaves.size(), false);
	for (std::uint32_t leaf = 0; leaf < relays.size(); ++leaf) {
		relays[leaf] = mayRelay(looks, materials, geometry.scene.leaves[leaf]);
	}
	if (std::find(relays.begin(), relays.end(), true) == relays.end()) {
		return noMirrorBalls();
	}
	const Relaying relaying = relayingOf(geometry, relays);

	Grown grown = {
		.boxes = std::vector<Box>(relays.size(), emptyBox()),
		.grown = std::vector<bool>(relays.size(), false),
	};

	for (std::uint32_t object = 0; object < relaying.objects.size(); ++object) {
		growObject(relaying, object, grown);
	}
	return ballsOf(grown);
}

RelayCone coneOf(const Ball& ball, const math::Vector& origin) {
	const math::Vector apart = ball.middle - origin;
	const double square      = math::dot(apart, apart);
	const double near        = ball.radius * ball.radius;
	if (square <= near) {
		return RelayCone {
			.axis = { 0, 0, 1 },
			.fall = 2,
		};
	}
	const double sideways = near / square;
	return RelayCone {
		.axis = apart / std::sqrt(square),
		.fall = sideways / (1 + std::sqrt(1 - sideways)),
	};
}

bool facesPoint(const RelayCone& cone, const LampPoint& point) {
	const bool round = point.normal.x == 0 && point.normal.y == 0 && point.normal.z == 0;
	return round || cone.fall >= 2 || math::dot(cone.axis, point.normal) >= -std::sqrt(cone.fall * (2 - cone.fall));
}

bool holdsDirection(const RelayCone& cone, const math::Vector& direction) {
	return cone.fall >= 2 || math::dot(cone.axis, direction) >= 1 - cone.fall;
}

double facingAngle(const MirrorBalls& mirrors, const LampPoint& point) {
	double total = 0;
	for (const Ball& ball : mirrors.balls) {
		const RelayCone cone = coneOf(ball, point.point);
		total += facesPoint(cone, point) ? 2 * math::pi * cone.fall : 0;
	}
	return total;
}

RelayCone pickedCone(const MirrorBalls& mirrors, const MirrorDraw& draw, double total) {
	const double choice = draw.pick * total;
	double running      = 0;
	RelayCone picked    = coneOf(mirrors.balls.front(), draw.point.point);
	for (const Ball& ball : mirrors.balls) {
		const RelayCone cone = coneOf(ball, draw.point.point);
		if (!facesPoint(cone, draw.point)) {
			continue;
		}
		picked = cone;
		running += 2 * math::pi * cone.fall;
		if (choice < running) {
			break;
		}
	}
	return picked;
}

math::Vector inMirrorCone(const RelayCone& cone, const math::UnitPair& pair) {
	const double height      = 1 - pair.u * cone.fall;
	const double ring        = std::sqrt(std::max(1 - height * height, 0.0));
	const double angle       = 2 * math::pi * pair.v;
	const math::Vector local = { ring * std::cos(angle), ring * std::sin(angle), height };
	return math::normalized(math::toWorld(math::basisAround(cone.axis), local));
}

MirrorDirection mirrorDirection(const MirrorBalls& mirrors, const MirrorDraw& draw) {
	const double total = facingAngle(mirrors, draw.point);
	if (!(total > 0)) {
		return MirrorDirection {
			.found     = false,
			.direction = { 0, 0, 1 },
		};
	}
	return MirrorDirection {
		.found     = true,
		.direction = inMirrorCone(pickedCone(mirrors, draw, total), draw.pair),
	};
}

double mirrorDensity(const MirrorBalls& mirrors, const MirrorAsk& ask) {
	double total    = 0;
	double covering = 0;
	for (const Ball& ball : mirrors.balls) {
		const RelayCone cone = coneOf(ball, ask.point.point);
		if (!facesPoint(cone, ask.point)) {
			continue;
		}
		total += 2 * math::pi * cone.fall;
		covering += holdsDirection(cone, ask.direction) ? 1 : 0;
	}
	return total > 0 ? covering / total : 0;
}

std::shared_ptr<void> openedMirrorBalls() {
	return std::make_shared<MirrorBalls>(noMirrorBalls());
}

MirrorBalls& mirrorBallsState(States& states) {
	return *static_cast<MirrorBalls*>(heldState(states, "sampler/toward_mirrors", openedMirrorBalls));
}

}
