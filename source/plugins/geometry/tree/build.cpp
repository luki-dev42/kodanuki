#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/cut.h"
#include "sdf/compare.h"
#include "sdf/numbers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(__unix__)
#include <unistd.h>
#endif

namespace kodanuki {

constexpr double regionMargin       = 0.01;
constexpr double floorTolerances    = 4;
constexpr int mappedBlock           = 1 << 20;
constexpr std::size_t unknownMemory = std::size_t(8) << 30;
constexpr std::size_t reachSteps    = 18;

Reach shotReach(const Shot& shot) {
	return Reach {
		.eye = shot.camera.eye,
		.far = shot.render.far,
	};
}

Reach openReach(const Region& region) {
	return Reach {
		.eye = { region.lower[0] + region.size[0] / 2, region.lower[1] + region.size[1] / 2, region.lower[2] + region.size[2] / 2 },
		.far = std::numeric_limits<double>::infinity(),
	};
}

double nearestOffset(const math::Interval& span, double at) {
	return std::max({ span.lower - at, at - span.upper, 0.0 });
}

double farthestOffset(const math::Interval& span, double at) {
	return std::max(std::abs(span.lower - at), std::abs(span.upper - at));
}

bool reachMeets(const Reach& reach, const Box& box) {
	double nearest = 0;
	for (int axis = 0; axis < 3; ++axis) {
		const double offset = nearestOffset(box.axes[axis], math::component(reach.eye, axis));
		nearest += offset * offset;
	}
	return nearest <= reach.far * reach.far;
}

bool reachHolds(const Reach& reach, const Box& box) {
	double farthest = 0;
	for (int axis = 0; axis < 3; ++axis) {
		const double offset = farthestOffset(box.axes[axis], math::component(reach.eye, axis));
		farthest += offset * offset;
	}
	return farthest <= reach.far * reach.far;
}

Step reachStep(Opcode opcode, std::size_t a, std::size_t b) {
	return Step {
		.opcode = opcode,
		.a      = std::uint16_t(a),
		.b      = std::uint16_t(b),
		.c      = 0,
		.value  = 0,
		.leaf   = noLeaf,
	};
}

Program reachedProgram(const Program& whole, const Reach& reach) {
	if (whole.steps.size() + reachSteps + 2 > mostProgramSteps) {
		return whole;
	}
	Program cut            = whole;
	const std::size_t at   = whole.steps.size();
	const Opcode points[3] = { Opcode::PointX, Opcode::PointY, Opcode::PointZ };
	for (int k = 0; k < 4; ++k) {
		cut.steps.push_back(reachStep(Opcode::Constant, 0, 0));
	}
	for (int axis = 0; axis < 3; ++axis) {
		cut.steps.push_back(reachStep(points[axis], 0, 0));
	}
	for (std::size_t axis = 0; axis < 3; ++axis) {
		cut.steps.push_back(reachStep(Opcode::Subtract, at + 4 + axis, at + axis));
	}
	for (std::size_t axis = 0; axis < 3; ++axis) {
		cut.steps.push_back(reachStep(Opcode::Multiply, at + 7 + axis, at + 7 + axis));
	}
	cut.steps.push_back(reachStep(Opcode::Add, at + 10, at + 11));
	cut.steps.push_back(reachStep(Opcode::Add, at + 13, at + 12));
	cut.steps.push_back(reachStep(Opcode::SquareRoot, at + 14, 0));
	cut.steps.push_back(reachStep(Opcode::Subtract, at + 15, at + 3));
	cut.steps.push_back(reachStep(Opcode::Maximum, whole.result, at + 16));
	cut.steps[at + 16].leaf = leastLeaf(whole);
	cut.result              = std::uint32_t(at + 17);
	movedReach(cut, reach);
	return cut;
}

bool cutToReach(const Program& program) {
	const std::size_t size = program.steps.size();
	return size >= reachSteps && program.result == size - 1 && program.steps[size - 1].opcode == Opcode::Maximum && program.steps[size - 1].b == size - 2 && program.steps[size - reachSteps + 4].opcode == Opcode::PointX && program.steps[size - reachSteps + 15].opcode == Opcode::SquareRoot;
}

void movedReach(Program& program, const Reach& reach) {
	if (!cutToReach(program)) {
		return;
	}
	const std::size_t at        = program.steps.size() - reachSteps;
	program.steps[at].value     = reach.eye.x;
	program.steps[at + 1].value = reach.eye.y;
	program.steps[at + 2].value = reach.eye.z;
	program.steps[at + 3].value = reach.far;
}

math::Interval reachSpan(const Shot& shot, int axis) {
	const double centre = math::component(shot.camera.eye, axis);
	return math::Interval {
		.lower = centre - shot.render.far,
		.upper = centre + shot.render.far,
	};
}

Box reachCube(const Shot& shot) {
	return Box {
		.axes = { reachSpan(shot, 0), reachSpan(shot, 1), reachSpan(shot, 2) },
	};
}

Region regionOf(const Shot& shot, const Box& bounds) {
	if (boxEmpty(bounds)) {
		return Region {
			.lower = { shot.camera.eye.x - 0.5, shot.camera.eye.y - 0.5, shot.camera.eye.z - 0.5 },
			.size  = { 1, 1, 1 },
		};
	}
	const double extent = std::max({ bounds.axes[0].upper - bounds.axes[0].lower, bounds.axes[1].upper - bounds.axes[1].lower, bounds.axes[2].upper - bounds.axes[2].lower });
	const double size   = extent * (1 + 2 * regionMargin);

	Region region = {
		.lower = { 0, 0, 0 },
		.size  = { size, size, size },
	};

	for (int axis = 0; axis < 3; ++axis) {
		region.lower[axis] = (bounds.axes[axis].lower + bounds.axes[axis].upper) / 2 - size / 2;
	}
	return region;
}

Box reachedHull(const std::vector<Box>& boxes, const Shot& shot) {
	const Box reach = reachCube(shot);
	Box bounds      = nothingBox();
	for (const Box& box : boxes) {
		bounds = boxHull(bounds, boxOverlap(box, reach));
	}
	return bounds;
}

std::vector<Box> heldBoxes(const std::vector<Box>& boxes, const Scene& scene) {
	std::vector<Box> held = boxes;
	if (!scene.tiles.empty()) {
		const double far = std::numeric_limits<double>::infinity();
		Box everywhere   = nothingBox();
		for (math::Interval& axis : everywhere.axes) {
			axis.lower = -far;
			axis.upper = far;
		}
		held.push_back(everywhere);
	}
	return held;
}

Region boxedRegion(const std::vector<Box>& boxes, const Shot& shot) {
	return regionOf(shot, reachedHull(boxes, shot));
}

Region sceneRegion(const Scene& scene, const Shot& shot, int workers) {
	return boxedRegion(objectBoxes(scene, workers).boxes, shot);
}

bool boxInRegion(const Box& box, const Region& region) {
	if (boxEmpty(box)) {
		return true;
	}
	for (int axis = 0; axis < 3; ++axis) {
		if (box.axes[axis].lower < region.lower[axis] || box.axes[axis].upper > region.lower[axis] + region.size[axis]) {
			return false;
		}
	}
	return true;
}

int depthCapOf(const Region& region, double least) {
	const double longest = std::max({ region.size[0], region.size[1], region.size[2] });
	return std::max(0, int(std::nearbyint(std::log2(longest / (floorTolerances * least)))));
}

void releasedFreeMemory() {
#if defined(__GLIBC__)
	malloc_trim(0);
#endif
}

void largeBlocksMapped() {
#if defined(__GLIBC__)
	mallopt(M_MMAP_THRESHOLD, mappedBlock);
#endif
}

std::size_t machineMemory() {
#if defined(__unix__)
	return std::size_t(sysconf(_SC_PHYS_PAGES)) * std::size_t(sysconf(_SC_PAGE_SIZE));
#else
	return unknownMemory;
#endif
}

}
