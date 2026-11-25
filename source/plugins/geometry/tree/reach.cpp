#include "plugins/geometry/tree/reach.h"

#include "plugins/geometry/tree/detail.h"
#include "plugins/geometry/tree/lazy.h"
#include "sdf/numbers.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace kodanuki {

constexpr std::size_t mostFieldBoxes = 100000;

math::Vector boxMiddle(const Box& box) {
	return { 0.5 * (box.axes[0].lower + box.axes[0].upper), 0.5 * (box.axes[1].lower + box.axes[1].upper), 0.5 * (box.axes[2].lower + box.axes[2].upper) };
}

double halfDiagonal(const Box& box) {
	const math::Vector across = { box.axes[0].upper - box.axes[0].lower, box.axes[1].upper - box.axes[1].lower, box.axes[2].upper - box.axes[2].lower };
	return 0.5 * math::length(across);
}

double objectFieldAt(const Program& program, const math::Vector& point) {
	thread_local std::vector<double> registers;

	const Inputs<double> inputs = {
		.arguments = nullptr,
		.p         = { point.x, point.y, point.z },
		.n         = { 0, 0, 0 },
	};

	return evaluateKept(wholeSteps(program), inputs, registers);
}

std::vector<Box> keptBoxes(const Program& program, const std::vector<Box>& boxes) {
	std::vector<Box> kept;
	for (const Box& box : boxes) {
		if (std::abs(objectFieldAt(program, boxMiddle(box))) <= halfDiagonal(box)) {
			kept.push_back(box);
		}
	}
	return kept;
}

std::vector<Box> halvedBoxes(const std::vector<Box>& boxes) {
	std::vector<Box> halved;
	for (const Box& box : boxes) {
		for (int octant = 0; octant < 8; ++octant) {
			halved.push_back(octantBox(box, octant));
		}
	}
	return halved;
}

std::vector<Box> fieldBoxes(const Program& program, const Region& region, int levels) {
	std::vector<Box> boxes = { regionBox(region) };
	for (int level = 0; level < levels; ++level) {
		const std::vector<Box> kept = keptBoxes(program, boxes);
		if (kept.size() * 8 > mostFieldBoxes) {
			return kept;
		}
		boxes = halvedBoxes(kept);
	}
	return keptBoxes(program, boxes);
}

Sourced objectSurface(const Program& program, const math::Vector& point) {
	thread_local std::vector<math::Dual> registers;
	const KeptSteps whole = wholeSteps(program);
	const math::Dual zero = math::constantAs<math::Dual>(0);

	const Inputs<math::Dual> inputs = {
		.arguments = nullptr,
		.p         = { math::coordinateAs<math::Dual>(point.x, 0), math::coordinateAs<math::Dual>(point.y, 1), math::coordinateAs<math::Dual>(point.z, 2) },
		.n         = { zero, zero, zero },
	};

	const math::Dual value = evaluateKept(whole, inputs, registers);
	const Source source    = keptSourceOf(whole, registers);
	return Sourced {
		.value    = value.value,
		.gradient = { value.gradient[0], value.gradient[1], value.gradient[2] },
		.leaf     = source.leaf,
		.other    = source.other,
		.share    = source.share,
	};
}

std::vector<std::uint32_t> objectsMeeting(const Geometry& geometry, const std::vector<std::uint32_t>& among, const Box& box) {
	const Tree& tree = treeOf(geometry);
	if (!tree.growth) {
		return among;
	}
	std::vector<std::uint32_t> meeting;
	for (const std::uint32_t object : among) {
		if (boxesMeet(tree.growth->boxes.boxes[tree.growth->objectSlots[object]], box)) {
			meeting.push_back(object);
		}
	}
	return meeting;
}

std::vector<std::uint32_t> everyObject(const Scene& scene) {
	std::vector<std::uint32_t> every;
	for (std::uint32_t object = 0; object < scene.objectPrograms.size(); ++object) {
		every.push_back(object);
	}
	return every;
}

Sourced nowhereSurface() {
	return Sourced {
		.value    = std::numeric_limits<double>::infinity(),
		.gradient = { 0, 0, 0 },
		.leaf     = noLeaf,
		.other    = noLeaf,
		.share    = 0,
	};
}

Sourced objectsSurface(const Scene& scene, const std::vector<std::uint32_t>& objects, const math::Vector& point) {
	double least           = std::numeric_limits<double>::infinity();
	std::uint32_t deciding = 0;
	for (const std::uint32_t object : objects) {
		const double value = objectFieldAt(scene.objectPrograms[object], point);
		deciding           = value < least ? object : deciding;
		least              = std::min(least, value);
	}
	return least < std::numeric_limits<double>::infinity() ? objectSurface(scene.objectPrograms[deciding], point) : nowhereSurface();
}

double looseStray(const Tree& tree, const Box& box) {
	return tree.growth ? cellTolerance(tree.growth->detail, box) : 0;
}

}
