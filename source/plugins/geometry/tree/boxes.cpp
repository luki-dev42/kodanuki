#include "plugins/geometry/tree/boxes.h"

#include "math/vector.h"
#include "sdf/compare.h"
#include "shared/parallel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace kodanuki {

constexpr double localReach    = 4096;
constexpr double unboundedEdge = 0.99 * localReach;
constexpr int sideDepth        = 8;
constexpr int cellDepth        = 6;
constexpr int boxPasses        = 7;
constexpr double boxShrink     = 0.9;

struct Primitive {
	std::uint32_t leaf;
	double level;
};

struct Unbounded {
	std::vector<double> key;
	std::uint32_t program;
};

struct PrimitiveSearch {
	const Program& program;
	const double* arguments;
	double level;
};

struct SideSearch {
	const PrimitiveSearch& primitive;
	int axis;
	bool upper;
};

struct SearchCell {
	Box box;
	int depth;
};

struct Bounding {
	const Scene& scene;
	const std::map<std::vector<double>, Box>& primitives;
};

struct Inverse {
	math::Vector columns[3];
	bool found;
};

math::Interval spanFrom(double lower, double upper) {
	return math::Interval {
		.lower = lower,
		.upper = upper,
	};
}

Box nothingBox() {
	const double far          = std::numeric_limits<double>::infinity();
	const math::Interval none = spanFrom(far, -far);
	return Box {
		.axes = { none, none, none },
	};
}

Box everywhereBox() {
	const double far           = std::numeric_limits<double>::infinity();
	const math::Interval whole = spanFrom(-far, far);
	return Box {
		.axes = { whole, whole, whole },
	};
}

bool boxEmpty(const Box& box) {
	for (const math::Interval& span : box.axes) {
		if (!(span.lower <= span.upper)) {
			return true;
		}
	}
	return false;
}

Box boxHull(const Box& a, const Box& b) {
	if (boxEmpty(a) || boxEmpty(b)) {
		return boxEmpty(a) ? b : a;
	}
	Box hull = a;
	for (int axis = 0; axis < 3; ++axis) {
		hull.axes[axis] = spanFrom(std::min(a.axes[axis].lower, b.axes[axis].lower), std::max(a.axes[axis].upper, b.axes[axis].upper));
	}
	return hull;
}

Box boxOverlap(const Box& a, const Box& b) {
	Box shared = a;
	for (int axis = 0; axis < 3; ++axis) {
		shared.axes[axis] = spanFrom(std::max(a.axes[axis].lower, b.axes[axis].lower), std::min(a.axes[axis].upper, b.axes[axis].upper));
	}
	return shared;
}

bool boxesMeet(const Box& a, const Box& b) {
	return !boxEmpty(boxOverlap(a, b));
}

double sideKey(const SideSearch& search, const Box& box) {
	return search.upper ? -box.axes[search.axis].upper : box.axes[search.axis].lower;
}

bool partBelowLevel(const PrimitiveSearch& primitive, const Box& box) {
	thread_local std::vector<math::Interval> registers;
	Inputs<math::Interval> inputs = boxInputs(box);
	inputs.arguments              = primitive.arguments;
	return !(evaluate(primitive.program, inputs, registers).lower >= primitive.level);
}

int splitAxis(const Box& box) {
	for (int axis = 0; axis < 3; ++axis) {
		if (box.axes[axis].lower < 0 && box.axes[axis].upper > 0) {
			return axis;
		}
	}
	return -1;
}

bool belowLevel(const PrimitiveSearch& primitive, const Box& box) {
	const int axis = splitAxis(box);
	if (axis < 0) {
		return partBelowLevel(primitive, box);
	}
	Box below        = box;
	Box above        = box;
	below.axes[axis] = spanFrom(box.axes[axis].lower, 0);
	above.axes[axis] = spanFrom(0, box.axes[axis].upper);
	return belowLevel(primitive, below) || belowLevel(primitive, above);
}

double cellKey(const SideSearch& search, const SearchCell& cell, double best);

double octantsKey(const SideSearch& search, const SearchCell& cell, double best) {
	const int near = search.upper ? 1 : 0;
	double found   = best;
	for (int rank = 0; rank < 16; ++rank) {
		const int octant = rank & 7;
		if ((((octant >> search.axis) & 1) == near) != (rank < 8)) {
			continue;
		}

		const SearchCell child = {
			.box   = octantBox(cell.box, octant),
			.depth = cell.depth - 1,
		};

		found = cellKey(search, child, found);
	}
	return found;
}

double cellKey(const SideSearch& search, const SearchCell& cell, double best) {
	if (sideKey(search, cell.box) >= best || !belowLevel(search.primitive, cell.box)) {
		return best;
	}
	if (cell.depth == 0) {
		return sideKey(search, cell.box);
	}
	return octantsKey(search, cell, best);
}

Box sideMoved(const Box& box, const SideSearch& search, double key) {
	Box moved               = box;
	moved.axes[search.axis] = search.upper ? spanFrom(box.axes[search.axis].lower, -key) : spanFrom(key, box.axes[search.axis].upper);
	return moved;
}

Box cellBox(const PrimitiveSearch& primitive, const Box& cube) {
	const SearchCell whole = {
		.box   = cube,
		.depth = cellDepth,
	};

	Box found = cube;
	for (int side = 0; side < 6; ++side) {
		const SideSearch search = {
			.primitive = primitive,
			.axis      = side % 3,
			.upper     = side >= 3,
		};

		const double key = cellKey(search, whole, std::numeric_limits<double>::infinity());
		if (key == std::numeric_limits<double>::infinity()) {
			return nothingBox();
		}
		found = sideMoved(found, search, key);
	}
	return found;
}

Box halfBox(const Box& box, int axis, int half) {
	const double middle = 0.5 * (box.axes[axis].lower + box.axes[axis].upper);
	Box part            = box;
	part.axes[axis]     = half == 0 ? spanFrom(box.axes[axis].lower, middle) : spanFrom(middle, box.axes[axis].upper);
	return part;
}

double extremeKey(const SideSearch& search, const Box& slab, int depth) {
	if (!belowLevel(search.primitive, slab)) {
		return std::numeric_limits<double>::infinity();
	}
	if (depth == 0) {
		return sideKey(search, slab);
	}
	const int near   = search.upper ? 1 : 0;
	const double key = extremeKey(search, halfBox(slab, search.axis, near), depth - 1);
	return key < std::numeric_limits<double>::infinity() ? key : extremeKey(search, halfBox(slab, search.axis, 1 - near), depth - 1);
}

Box sweptBox(const PrimitiveSearch& primitive, const Box& box) {
	Box found = box;
	for (int side = 0; side < 6; ++side) {
		const SideSearch search = {
			.primitive = primitive,
			.axis      = side % 3,
			.upper     = side >= 3,
		};

		const double key = extremeKey(search, found, sideDepth);
		if (key == std::numeric_limits<double>::infinity()) {
			return nothingBox();
		}
		found = sideMoved(found, search, key);
	}
	return found;
}

double sidesTotal(const Box& box) {
	return box.axes[0].upper - box.axes[0].lower + box.axes[1].upper - box.axes[1].lower + box.axes[2].upper - box.axes[2].lower;
}

Box refinedBox(const PrimitiveSearch& primitive, const Box& found, int passes) {
	if (passes == 0 || boxEmpty(found)) {
		return found;
	}
	const Box tighter = sweptBox(primitive, found);
	if (boxEmpty(tighter) || !(sidesTotal(tighter) < boxShrink * sidesTotal(found))) {
		return tighter;
	}
	return refinedBox(primitive, tighter, passes - 1);
}

Box zoomedBox(const PrimitiveSearch& primitive, const Box& found, int passes) {
	if (passes == 0 || boxEmpty(found)) {
		return found;
	}
	const Box tighter = cellBox(primitive, found);
	if (boxEmpty(tighter) || !(sidesTotal(tighter) < boxShrink * sidesTotal(found))) {
		return tighter;
	}
	return zoomedBox(primitive, tighter, passes - 1);
}

bool reachesEdge(const Box& box) {
	for (const math::Interval& span : box.axes) {
		if (span.lower <= -unboundedEdge || span.upper >= unboundedEdge) {
			return true;
		}
	}
	return false;
}

Box openedSides(const Box& box) {
	const double far = std::numeric_limits<double>::infinity();
	Box opened       = box;
	for (int axis = 0; axis < 3; ++axis) {
		opened.axes[axis] = spanFrom(box.axes[axis].lower <= -unboundedEdge ? -far : box.axes[axis].lower, box.axes[axis].upper >= unboundedEdge ? far : box.axes[axis].upper);
	}
	return opened;
}

Box primitiveBox(const Scene& scene, const Unbounded& unbounded) {
	const PrimitiveSearch primitive = {
		.program   = scene.programs[unbounded.program],
		.arguments = unbounded.key.data() + 3,
		.level     = unbounded.key[2],
	};

	const Box reach = {
		.axes = { spanFrom(-localReach, localReach), spanFrom(-localReach, localReach), spanFrom(-localReach, localReach) },
	};

	const Box slabbed = refinedBox(primitive, reach, boxPasses);
	if (boxEmpty(slabbed) || !reachesEdge(slabbed)) {
		return slabbed;
	}
	return openedSides(zoomedBox(primitive, slabbed, boxPasses));
}

std::vector<double> primitiveKey(const Scene& scene, const Primitive& primitive) {
	const Leaf& leaf            = scene.leaves[primitive.leaf];
	const auto arguments        = scene.arguments.begin() + leaf.arguments;
	const std::uint64_t program = programKey(scene.programs[leaf.program]);
	std::vector<double> key     = { double(program >> 32), double(program & 0xFFFFFFFFu), primitive.level / leaf.placement.stretch };
	key.insert(key.end(), arguments, arguments + scene.programs[leaf.program].arguments);
	return key;
}

double childLevel(const Node& node, std::size_t child, double level) {
	if (node.join != Join::Merge || node.radius <= 0) {
		return level;
	}
	const std::size_t joins = child == 0 ? node.children.size() - 1 : node.children.size() - child;
	return level + double(joins) * node.radius / 4;
}

std::size_t boundingChildren(const Node& node) {
	return node.join == Join::Cut ? 1 : node.children.size();
}

std::vector<Primitive> nodePrimitives(const Scene& scene, std::uint32_t node, double level) {
	const Node& held = scene.nodes[node];
	if (held.join == Join::Leaf) {
		const Primitive primitive = {
			.leaf  = held.leaf,
			.level = level,
		};

		return { primitive };
	}
	std::vector<Primitive> found;
	for (std::size_t child = 0; child < boundingChildren(held); ++child) {
		const std::vector<Primitive> below = nodePrimitives(scene, held.children[child], childLevel(held, child, level));
		found.insert(found.end(), below.begin(), below.end());
	}
	return found;
}

bool placedSearch(const Leaf& leaf) {
	return leaf.placement.stretch > 0;
}

std::vector<Unbounded> missingPrimitives(const ObjectBoxes& boxes, const Scene& scene, const std::vector<std::uint32_t>& objects) {
	std::vector<Unbounded> missing;
	for (const std::uint32_t object : objects) {
		for (const Primitive& primitive : nodePrimitives(scene, scene.objects[object], 0)) {
			const std::vector<double> key = placedSearch(scene.leaves[primitive.leaf]) ? primitiveKey(scene, primitive) : std::vector<double>();
			const Unbounded unbounded     = {
				.key     = key,
				.program = scene.leaves[primitive.leaf].program,
			};

			if (!key.empty() && !boxes.primitives.contains(key)) {
				missing.push_back(unbounded);
			}
		}
	}
	std::stable_sort(missing.begin(), missing.end(), [](const Unbounded& a, const Unbounded& b) {
		return a.key < b.key;
	});
	missing.erase(std::unique(missing.begin(), missing.end(), [](const Unbounded& a, const Unbounded& b) {
					  return a.key == b.key;
				  }),
				  missing.end());
	return missing;
}

Inverse inverseOf(const Placement& placement) {
	const math::Vector rows[3] = { { placement.linear[0][0], placement.linear[0][1], placement.linear[0][2] }, { placement.linear[1][0], placement.linear[1][1], placement.linear[1][2] }, { placement.linear[2][0], placement.linear[2][1], placement.linear[2][2] } };
	const double determinant   = math::dot(rows[0], math::cross(rows[1], rows[2]));
	return Inverse {
		.columns = { math::cross(rows[1], rows[2]) / determinant, math::cross(rows[2], rows[0]) / determinant, math::cross(rows[0], rows[1]) / determinant },
		.found   = determinant != 0 && std::isfinite(1 / determinant),
	};
}

math::Interval scaledSpan(double scale, const math::Interval& span) {
	if (scale == 0) {
		return spanFrom(0, 0);
	}
	return scale > 0 ? spanFrom(scale * span.lower, scale * span.upper) : spanFrom(scale * span.upper, scale * span.lower);
}

math::Interval worldSpan(const Inverse& inverse, const Box& local, int axis) {
	math::Interval span = spanFrom(0, 0);
	for (int k = 0; k < 3; ++k) {
		const math::Interval term = scaledSpan(math::component(inverse.columns[k], axis), local.axes[k]);
		span                      = spanFrom(span.lower + term.lower, span.upper + term.upper);
	}
	return span;
}

Box placedBox(const Placement& placement, const Box& local) {
	const Inverse inverse = inverseOf(placement);
	if (boxEmpty(local) || !inverse.found) {
		return boxEmpty(local) ? nothingBox() : everywhereBox();
	}
	Box placed = local;
	for (int axis = 0; axis < 3; ++axis) {
		const math::Interval span = worldSpan(inverse, local, axis);
		placed.axes[axis]         = spanFrom(placement.origin[axis] + span.lower, placement.origin[axis] + span.upper);
	}
	return placed;
}

Box leafBox(const Bounding& bounding, const Primitive& primitive) {
	const Leaf& leaf = bounding.scene.leaves[primitive.leaf];
	if (!placedSearch(leaf)) {
		return everywhereBox();
	}
	return placedBox(leaf.placement, bounding.primitives.at(primitiveKey(bounding.scene, primitive)));
}

Box joinedNodeBox(const Bounding& bounding, std::uint32_t node, double level) {
	const Node& held = bounding.scene.nodes[node];
	if (held.join == Join::Leaf) {
		const Primitive primitive = {
			.leaf  = held.leaf,
			.level = level,
		};

		return leafBox(bounding, primitive);
	}
	Box box = joinedNodeBox(bounding, held.children[0], childLevel(held, 0, level));
	for (std::size_t child = 1; child < boundingChildren(held); ++child) {
		const Box next = joinedNodeBox(bounding, held.children[child], childLevel(held, child, level));
		box            = held.join == Join::Merge ? boxHull(box, next) : boxOverlap(box, next);
	}
	return box;
}

std::vector<Box> boundObjects(ObjectBoxes& boxes, const Scene& scene, const std::vector<std::uint32_t>& objects, int workers) {
	const std::vector<Unbounded> missing = missingPrimitives(boxes, scene, objects);
	std::vector<Box> found(missing.size(), nothingBox());
	inParallel(std::max(1, workers), missing.size(), [&](std::size_t k) {
		found[k] = primitiveBox(scene, missing[k]);
	});

	for (std::size_t k = 0; k < missing.size(); ++k) {
		boxes.primitives.emplace(missing[k].key, found[k]);
	}

	const Bounding bounding = {
		.scene      = scene,
		.primitives = boxes.primitives,
	};

	std::vector<Box> bound(objects.size(), nothingBox());
	inParallel(std::max(1, workers), objects.size(), [&](std::size_t k) {
		bound[k] = joinedNodeBox(bounding, scene.objects[objects[k]], 0);
	});

	return bound;
}

ObjectBoxes objectBoxes(const Scene& scene, int workers) {
	ObjectBoxes boxes = {
		.boxes      = {},
		.primitives = {},
	};

	std::vector<std::uint32_t> every;
	for (std::uint32_t object = 0; object < scene.objects.size(); ++object) {
		every.push_back(object);
	}
	boxes.boxes = boundObjects(boxes, scene, every, workers);
	return boxes;
}

}
