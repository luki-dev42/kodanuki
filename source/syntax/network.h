#pragma once

#include "math/vector.h"
#include "syntax/evaluator.h"
#include "syntax/parser.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace kodanuki {

/**
 * What `network(lines, spacing = ...)` builds a network from.
 */
struct NetworkSource {
	/**
	 * The lines: a list of lists of waypoints, or of records whose field
	 * `points` holds the waypoints.
	 */
	Value lines;
	/**
	 * About how far apart the points of a resampled line stand, in metres.
	 */
	double spacing;
	/**
	 * A key of the lines' values, the same for the same lines at every
	 * load.
	 */
	std::uint64_t key;
	/**
	 * Where the network is made, for errors.
	 */
	Location location;
};

/**
 * One straight piece of a resampled line.
 */
struct NetworkSegment {
	/**
	 * Where it starts; z is 0 on a network of flat points.
	 */
	math::Vector a;
	/**
	 * Where it ends.
	 */
	math::Vector b;
	/**
	 * How far its start lies from the start of its line, in metres along
	 * the resampled line.
	 */
	double along;
	/**
	 * The index of its line in the network's lines.
	 */
	std::uint32_t line;
};

/**
 * A uniform grid of square cells over the network's extent in x and y,
 * each holding the segments whose bounding box meets it.
 */
struct NetworkGrid {
	/**
	 * The lower corner of cell (0, 0), x then y.
	 */
	double origin[2];
	/**
	 * The side of a cell, in metres.
	 */
	double cell;
	/**
	 * How many cells lie along x.
	 */
	std::int64_t width;
	/**
	 * How many cells lie along y.
	 */
	std::int64_t height;
	/**
	 * Where each cell's segments start in `members`, row by row, x
	 * fastest, with one more entry for the end of the last cell.
	 */
	std::vector<std::uint32_t> starts;
	/**
	 * The segments of every cell, each cell's in rising order.
	 */
	std::vector<std::uint32_t> members;
};

/**
 * A network of lines resampled along Catmull-Rom curves, with a grid of
 * its segments for finding them by place.
 */
struct Network {
	/**
	 * The segments of every line, line by line, each line's from its
	 * start; a segment's index is its order along the network.
	 */
	std::vector<NetworkSegment> segments;
	/**
	 * Each line's record without its `points`, or an empty record for a
	 * line written as a list of points.
	 */
	std::vector<Value> fields;
	/**
	 * How many numbers a waypoint holds: 2 or 3.
	 */
	std::uint32_t dimensions;
	/**
	 * The grid of segments.
	 */
	NetworkGrid grid;
	/**
	 * A key of the lines and the spacing, the same for the same network
	 * at every load.
	 */
	std::uint64_t key;
};

/**
 * A point to find the nearest segment of a network to.
 */
struct NetworkQuery {
	/**
	 * The point's x.
	 */
	double x;
	/**
	 * The point's y.
	 */
	double y;
	/**
	 * How far from the point a segment may lie and still be found; an
	 * infinite reach finds the nearest segment wherever it is.
	 */
	double reach;
};

/**
 * A segment found nearest to a point.
 */
struct NetworkHit {
	/**
	 * The segment, or noSegment where none lies within the reach.
	 */
	std::uint32_t segment;
	/**
	 * Its distance from the point in x and y.
	 */
	double distance;
	/**
	 * How far along the segment its nearest point lies, from 0 at its
	 * start to 1 at its end.
	 */
	double share;
};

/**
 * An axis box in x and y.
 */
struct NetworkBox {
	/**
	 * The lower corner, x then y.
	 */
	double lower[2];
	/**
	 * The upper corner, x then y.
	 */
	double upper[2];
};

/**
 * The segment of a hit that found none.
 */
constexpr std::uint32_t noSegment = 0xFFFFFFFFu;

/**
 * Build a network from its lines, or fail with the first error and
 * return null.
 *
 * Each line is resampled along the Catmull-Rom curve `smooth` draws
 * through its waypoints, taking the distance from the first waypoint
 * along the straight pieces between them as the curve's time: the stretch
 * between two waypoints is cut into round(its length / spacing) pieces,
 * at least one, so the curve passes through every waypoint. The grid's
 * cells are four spacings across, or wider where that would make more
 * than about four cells a segment.
 *
 * @param source The lines, the spacing and where the network is made.
 */
std::shared_ptr<const Network> builtNetwork(const NetworkSource& source);

/**
 * Return the segment nearest to a point in x and y, within its reach:
 * on a tie the segment of the lower index.
 *
 * @param network The network.
 * @param query The point and the reach.
 */
NetworkHit nearestSegment(const Network& network, const NetworkQuery& query);

/**
 * Return the segments that meet a box in x and y, edges included, each
 * once, in rising order.
 *
 * @param network The network.
 * @param box The box.
 */
std::vector<std::uint32_t> segmentsWithin(const Network& network, const NetworkBox& box);

/**
 * Return what `nearest` gives: the record `{ distance, along, side,
 * line, at, direction, fields, found }` of the nearest point of the
 * network to a point, or with `found` 0, `distance` the reach and the
 * rest 0 (`line` -1, `fields` an empty record) where none lies within it.
 *
 * @param network The network.
 * @param query The point and the reach.
 */
Value nearestValue(const Network& network, const NetworkQuery& query);

/**
 * Return what `within` gives: the list of records `{ a, b, line, along,
 * fields }` of the segments that meet a box, in the order of their lines
 * and along each line.
 *
 * @param network The network.
 * @param box The box.
 */
Value withinValue(const Network& network, const NetworkBox& box);

}
