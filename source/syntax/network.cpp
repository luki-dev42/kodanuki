#include "syntax/network.h"

#include "sdf/compare.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kodanuki {

struct ReadLine {
	std::vector<std::vector<double>> points;
	Value fields;
};

const Value* fieldNamed(const Value& record, const std::string& name) {
	const std::vector<std::string>& names = *record.fields;
	const auto found                      = std::find(names.begin(), names.end(), name);
	return found == names.end() ? nullptr : &record.items[std::size_t(found - names.begin())];
}

Value otherFields(const Value& record) {
	std::vector<std::string> names;
	std::vector<Value> items;
	for (std::size_t i = 0; i < record.items.size(); ++i) {
		if ((*record.fields)[i] != "points") {
			names.push_back((*record.fields)[i]);
			items.push_back(record.items[i]);
		}
	}
	return recordValue(names, items);
}

std::vector<double> waypoint(const Value& point, const Location& location) {
	const bool listed = point.kind == ValueKind::List && (point.items.size() == 2 || point.items.size() == 3);
	if (!listed) {
		fail(location, "a line's points are [x, y] or [x, y, z]");
		return {};
	}
	std::vector<double> numbers;
	for (const Value& item : point.items) {
		if (item.kind != ValueKind::Number) {
			fail(location, "a line's points are [x, y] or [x, y, z]");
			return {};
		}
		if (item.slot >= 0) {
			fail(location, "network is known only when the scene loads");
			return {};
		}
		numbers.push_back(item.number);
	}
	return numbers;
}

std::vector<std::vector<double>> waypoints(const Value& points, const Location& location) {
	if (points.kind != ValueKind::List || points.items.size() < 2) {
		fail(location, "a line has 2 points or more");
		return {};
	}
	std::vector<std::vector<double>> read;
	for (const Value& point : points.items) {
		read.push_back(waypoint(point, location));
		if (failed()) {
			return {};
		}
		if (read.size() > 1 && read.back() == read[read.size() - 2]) {
			fail(location, "a line's points in a row differ");
			return {};
		}
	}
	return read;
}

ReadLine readLine(const Value& line, bool records, const Location& location) {
	if ((line.kind == ValueKind::Record) != records) {
		fail(location, "a network's lines are all lists of points or all records with points");
		return {};
	}
	const Value* points = records ? fieldNamed(line, "points") : &line;
	if (!points) {
		fail(location, "a line's record holds its points in 'points'");
		return {};
	}
	return ReadLine {
		.points = waypoints(*points, location),
		.fields = records ? otherFields(line) : recordValue({}, {}),
	};
}

bool sameDimensions(const std::vector<ReadLine>& lines) {
	for (const ReadLine& line : lines) {
		for (const std::vector<double>& point : line.points) {
			if (point.size() != lines[0].points[0].size()) {
				return false;
			}
		}
	}
	return true;
}

std::vector<ReadLine> readLines(const NetworkSource& source) {
	if (source.lines.kind != ValueKind::List || source.lines.items.empty()) {
		fail(source.location, "network takes a list of lines and a spacing");
		return {};
	}
	const bool records = source.lines.items[0].kind == ValueKind::Record;
	std::vector<ReadLine> lines;
	for (const Value& line : source.lines.items) {
		lines.push_back(readLine(line, records, source.location));
		if (failed()) {
			return {};
		}
	}
	if (!sameDimensions(lines)) {
		fail(source.location, "a network's points all have 2 numbers or all 3");
		return {};
	}
	return lines;
}

double gap(const std::vector<double>& a, const std::vector<double>& b) {
	double sum = 0;
	for (std::size_t i = 0; i < a.size(); ++i) {
		sum += (b[i] - a[i]) * (b[i] - a[i]);
	}
	return std::sqrt(sum);
}

std::vector<TimedStop> chordStops(const std::vector<std::vector<double>>& points) {
	std::vector<TimedStop> stops;
	double time = 0;
	for (std::size_t i = 0; i < points.size(); ++i) {
		time += i == 0 ? 0 : gap(points[i - 1], points[i]);

		const TimedStop stop = {
			.time   = time,
			.values = points[i],
		};

		stops.push_back(stop);
	}
	return stops;
}

math::Vector vectorOf(const std::vector<double>& values) {
	return math::Vector { values[0], values[1], values.size() == 3 ? values[2] : 0 };
}

std::size_t pieceCount(double length, double spacing) {
	return std::max<std::size_t>(1, std::size_t(std::llround(length / spacing)));
}

std::vector<math::Vector> resampledLine(const std::vector<std::vector<double>>& points, double spacing) {
	const std::vector<TimedStop> stops = chordStops(points);
	std::vector<math::Vector> resampled;
	for (std::size_t at = 0; at + 1 < stops.size(); ++at) {
		const std::size_t pieces = pieceCount(stops[at + 1].time - stops[at].time, spacing);
		resampled.push_back(vectorOf(stops[at].values));
		for (std::size_t k = 1; k < pieces; ++k) {
			resampled.push_back(vectorOf(hermiteValues(stops, at, double(k) / double(pieces))));
		}
	}
	resampled.push_back(vectorOf(stops.back().values));
	return resampled;
}

void appendSegments(std::vector<NetworkSegment>& segments, const std::vector<math::Vector>& points, std::uint32_t line) {
	double along = 0;
	for (std::size_t i = 0; i + 1 < points.size(); ++i) {
		const double length = math::length(points[i + 1] - points[i]);
		if (length == 0) {
			continue;
		}

		const NetworkSegment segment = {
			.a     = points[i],
			.b     = points[i + 1],
			.along = along,
			.line  = line,
		};

		segments.push_back(segment);
		along += length;
	}
}

NetworkBox segmentBox(const NetworkSegment& segment) {
	return NetworkBox {
		.lower = { std::min(segment.a.x, segment.b.x), std::min(segment.a.y, segment.b.y) },
		.upper = { std::max(segment.a.x, segment.b.x), std::max(segment.a.y, segment.b.y) },
	};
}

NetworkBox segmentsExtent(const std::vector<NetworkSegment>& segments) {
	NetworkBox extent = segmentBox(segments[0]);
	for (const NetworkSegment& segment : segments) {
		const NetworkBox box = segmentBox(segment);
		for (int axis = 0; axis < 2; ++axis) {
			extent.lower[axis] = std::min(extent.lower[axis], box.lower[axis]);
			extent.upper[axis] = std::max(extent.upper[axis], box.upper[axis]);
		}
	}
	return extent;
}

double cellSize(const NetworkBox& extent, double spacing, std::size_t segments) {
	const double area  = (extent.upper[0] - extent.lower[0]) * (extent.upper[1] - extent.lower[1]);
	const double cells = 4.0 * double(segments) + 16;
	return std::max(4 * spacing, std::sqrt(area / cells));
}

std::int64_t cellIndex(const NetworkGrid& grid, double x, int axis) {
	const double cells = std::clamp((x - grid.origin[axis]) / grid.cell, -1e12, 1e12);
	return std::int64_t(std::floor(cells));
}

std::int64_t clampedCell(const NetworkGrid& grid, double x, int axis) {
	return std::clamp<std::int64_t>(cellIndex(grid, x, axis), 0, (axis == 0 ? grid.width : grid.height) - 1);
}

struct NetworkSpan {
	std::int64_t lower[2];
	std::int64_t upper[2];
};

NetworkSpan cellSpan(const NetworkGrid& grid, const NetworkBox& box) {
	return NetworkSpan {
		.lower = { clampedCell(grid, box.lower[0], 0), clampedCell(grid, box.lower[1], 1) },
		.upper = { clampedCell(grid, box.upper[0], 0), clampedCell(grid, box.upper[1], 1) },
	};
}

std::size_t cellOf(const NetworkGrid& grid, std::int64_t x, std::int64_t y) {
	return std::size_t(y * grid.width + x);
}

void countSegments(NetworkGrid& grid, const std::vector<NetworkSegment>& segments) {
	for (const NetworkSegment& segment : segments) {
		const NetworkSpan span = cellSpan(grid, segmentBox(segment));
		for (std::int64_t y = span.lower[1]; y <= span.upper[1]; ++y) {
			for (std::int64_t x = span.lower[0]; x <= span.upper[0]; ++x) {
				grid.starts[cellOf(grid, x, y) + 1] += 1;
			}
		}
	}
	for (std::size_t i = 1; i < grid.starts.size(); ++i) {
		grid.starts[i] += grid.starts[i - 1];
	}
}

void fillSegments(NetworkGrid& grid, const std::vector<NetworkSegment>& segments) {
	std::vector<std::uint32_t> next(grid.starts.begin(), grid.starts.end() - 1);
	grid.members.resize(grid.starts.back());
	for (std::uint32_t i = 0; i < segments.size(); ++i) {
		const NetworkSpan span = cellSpan(grid, segmentBox(segments[i]));
		for (std::int64_t y = span.lower[1]; y <= span.upper[1]; ++y) {
			for (std::int64_t x = span.lower[0]; x <= span.upper[0]; ++x) {
				grid.members[next[cellOf(grid, x, y)]++] = i;
			}
		}
	}
}

NetworkGrid segmentGrid(const std::vector<NetworkSegment>& segments, double spacing) {
	const NetworkBox extent = segmentsExtent(segments);
	const double cell       = cellSize(extent, spacing, segments.size());
	const std::int64_t wide = std::int64_t(std::floor((extent.upper[0] - extent.lower[0]) / cell)) + 1;
	const std::int64_t high = std::int64_t(std::floor((extent.upper[1] - extent.lower[1]) / cell)) + 1;

	NetworkGrid grid = {
		.origin  = { extent.lower[0], extent.lower[1] },
		.cell    = cell,
		.width   = wide,
		.height  = high,
		.starts  = std::vector<std::uint32_t>(std::size_t(wide * high) + 1, 0),
		.members = {},
	};

	countSegments(grid, segments);
	fillSegments(grid, segments);
	return grid;
}

std::shared_ptr<const Network> builtNetwork(const NetworkSource& source) {
	if (!(source.spacing > 0)) {
		fail(source.location, "a network's spacing is above 0");
		return nullptr;
	}
	const std::vector<ReadLine> lines = readLines(source);
	if (failed()) {
		return nullptr;
	}
	Network network = {
		.segments   = {},
		.fields     = {},
		.dimensions = std::uint32_t(lines[0].points[0].size()),
		.grid       = {},
		.key        = mixedKey(source.key, std::bit_cast<std::uint64_t>(source.spacing)),
	};

	for (std::uint32_t line = 0; line < lines.size(); ++line) {
		appendSegments(network.segments, resampledLine(lines[line].points, source.spacing), line);
		network.fields.push_back(lines[line].fields);
	}
	network.grid = segmentGrid(network.segments, source.spacing);
	return std::make_shared<const Network>(std::move(network));
}

NetworkHit segmentHit(const NetworkSegment& segment, const NetworkQuery& query) {
	const double dx     = segment.b.x - segment.a.x;
	const double dy     = segment.b.y - segment.a.y;
	const double square = dx * dx + dy * dy;
	const double share  = square > 0 ? std::clamp(((query.x - segment.a.x) * dx + (query.y - segment.a.y) * dy) / square, 0.0, 1.0) : 0;
	return NetworkHit {
		.segment  = noSegment,
		.distance = std::hypot(query.x - (segment.a.x + share * dx), query.y - (segment.a.y + share * dy)),
		.share    = share,
	};
}

bool closer(const NetworkHit& a, const NetworkHit& b) {
	return a.distance < b.distance || (a.distance == b.distance && a.segment < b.segment);
}

NetworkHit noHit(double distance) {
	return NetworkHit {
		.segment  = noSegment,
		.distance = distance,
		.share    = 0,
	};
}

struct RingCell {
	std::int64_t x;
	std::int64_t y;
};

NetworkHit cellHit(const Network& network, const NetworkQuery& query, const RingCell& cell) {
	const std::size_t index = cellOf(network.grid, cell.x, cell.y);
	NetworkHit best         = noHit(std::numeric_limits<double>::infinity());
	for (std::uint32_t i = network.grid.starts[index]; i < network.grid.starts[index + 1]; ++i) {
		NetworkHit hit = segmentHit(network.segments[network.grid.members[i]], query);
		hit.segment    = network.grid.members[i];
		best           = closer(hit, best) ? hit : best;
	}
	return best;
}

bool inGrid(const NetworkGrid& grid, const RingCell& cell) {
	return cell.x >= 0 && cell.x < grid.width && cell.y >= 0 && cell.y < grid.height;
}

struct NetworkRing {
	RingCell centre;
	std::int64_t radius;
};

struct RingRow {
	std::int64_t centre;
	std::int64_t y;
	std::int64_t radius;
	bool edge;
};

NetworkHit rowHit(const Network& network, const NetworkQuery& query, const RingRow& row) {
	const std::int64_t step  = row.edge ? 1 : 2 * row.radius;
	const std::int64_t first = row.edge ? std::max<std::int64_t>(row.centre - row.radius, 0) : row.centre - row.radius;
	const std::int64_t last  = row.edge ? std::min(row.centre + row.radius, network.grid.width - 1) : row.centre + row.radius;
	NetworkHit best          = noHit(std::numeric_limits<double>::infinity());
	for (std::int64_t x = first; x <= last; x += step) {
		const RingCell cell = {
			.x = x,
			.y = row.y,
		};

		const NetworkHit hit = inGrid(network.grid, cell) ? cellHit(network, query, cell) : best;
		best                 = closer(hit, best) ? hit : best;
	}
	return best;
}

NetworkHit ringHit(const Network& network, const NetworkQuery& query, const NetworkRing& ring) {
	const std::int64_t first = std::max<std::int64_t>(ring.centre.y - ring.radius, 0);
	const std::int64_t last  = std::min(ring.centre.y + ring.radius, network.grid.height - 1);
	NetworkHit best          = noHit(std::numeric_limits<double>::infinity());
	for (std::int64_t y = first; y <= last; ++y) {
		const RingRow row = {
			.centre = ring.centre.x,
			.y      = y,
			.radius = ring.radius,
			.edge   = std::abs(y - ring.centre.y) == ring.radius,
		};

		const NetworkHit hit = rowHit(network, query, row);
		best                 = closer(hit, best) ? hit : best;
	}
	return best;
}

std::int64_t outside(std::int64_t at, std::int64_t count) {
	return std::max<std::int64_t>({ 0, -at, at - (count - 1) });
}

std::int64_t farthest(std::int64_t at, std::int64_t count) {
	return std::max(std::abs(at), std::abs(at - (count - 1)));
}

NetworkHit nearestSegment(const Network& network, const NetworkQuery& query) {
	const NetworkGrid& grid = network.grid;
	const RingCell centre   = {
		.x = cellIndex(grid, query.x, 0),
		.y = cellIndex(grid, query.y, 1),
	};

	const std::int64_t first = std::max(outside(centre.x, grid.width), outside(centre.y, grid.height));
	const std::int64_t last  = std::max(farthest(centre.x, grid.width), farthest(centre.y, grid.height));
	NetworkHit best          = noHit(std::numeric_limits<double>::infinity());
	for (std::int64_t radius = first; radius <= last; ++radius) {
		if (double(radius - 1) * grid.cell >= std::min(best.distance, query.reach)) {
			break;
		}

		const NetworkRing ring = {
			.centre = centre,
			.radius = radius,
		};

		const NetworkHit hit = ringHit(network, query, ring);
		best                 = closer(hit, best) ? hit : best;
	}
	return best.distance <= query.reach ? best : noHit(query.reach);
}

bool slabRange(double start, double delta, const double bounds[2], double range[2]) {
	if (delta == 0) {
		return start >= bounds[0] && start <= bounds[1];
	}
	const double ta = (bounds[0] - start) / delta;
	const double tb = (bounds[1] - start) / delta;
	range[0]        = std::max(range[0], std::min(ta, tb));
	range[1]        = std::min(range[1], std::max(ta, tb));
	return range[0] <= range[1];
}

bool segmentMeetsBox(const NetworkSegment& segment, const NetworkBox& box) {
	double range[2]    = { 0, 1 };
	const double xs[2] = { box.lower[0], box.upper[0] };
	const double ys[2] = { box.lower[1], box.upper[1] };
	const bool alongX  = slabRange(segment.a.x, segment.b.x - segment.a.x, xs, range);
	return alongX && slabRange(segment.a.y, segment.b.y - segment.a.y, ys, range);
}

bool boxMeetsGrid(const NetworkGrid& grid, const NetworkBox& box) {
	const bool xs = cellIndex(grid, box.upper[0], 0) >= 0 && cellIndex(grid, box.lower[0], 0) < grid.width;
	const bool ys = cellIndex(grid, box.upper[1], 1) >= 0 && cellIndex(grid, box.lower[1], 1) < grid.height;
	return xs && ys;
}

std::vector<std::uint32_t> boxCandidates(const NetworkGrid& grid, const NetworkBox& box) {
	const NetworkSpan span = cellSpan(grid, box);
	std::vector<std::uint32_t> found;
	for (std::int64_t y = span.lower[1]; y <= span.upper[1]; ++y) {
		for (std::int64_t x = span.lower[0]; x <= span.upper[0]; ++x) {
			const std::size_t cell = cellOf(grid, x, y);
			found.insert(found.end(), grid.members.begin() + grid.starts[cell], grid.members.begin() + grid.starts[cell + 1]);
		}
	}
	std::sort(found.begin(), found.end());
	found.erase(std::unique(found.begin(), found.end()), found.end());
	return found;
}

std::vector<std::uint32_t> segmentsWithin(const Network& network, const NetworkBox& box) {
	if (!boxMeetsGrid(network.grid, box)) {
		return {};
	}
	std::vector<std::uint32_t> met;
	for (const std::uint32_t segment : boxCandidates(network.grid, box)) {
		if (segmentMeetsBox(network.segments[segment], box)) {
			met.push_back(segment);
		}
	}
	return met;
}

Value pointValue(const math::Vector& point, std::uint32_t dimensions) {
	std::vector<Value> items = { numberValue(point.x), numberValue(point.y) };
	if (dimensions == 3) {
		items.push_back(numberValue(point.z));
	}
	return listValue(std::move(items));
}

Value nearestRecord(std::vector<Value> items) {
	static const std::vector<std::string> names = { "along", "at", "direction", "distance", "fields", "found", "line", "side" };
	static const Value shaped                   = recordValue(names, std::vector<Value>(names.size(), numberValue(0)));
	Value record                                = shaped;
	record.items                                = std::move(items);
	return record;
}

double sideOf(const NetworkSegment& segment, const math::Vector& at, const NetworkQuery& query) {
	const double turn = (segment.b.x - segment.a.x) * (query.y - at.y) - (segment.b.y - segment.a.y) * (query.x - at.x);
	return turn > 0 ? 1 : turn < 0 ? -1
								   : 0;
}

Value missedValue(const Network& network, const NetworkQuery& query) {
	const Value zero = pointValue(math::Vector { 0, 0, 0 }, network.dimensions);
	return nearestRecord({ numberValue(0), zero, zero, numberValue(query.reach), recordValue({}, {}), numberValue(0), numberValue(-1), numberValue(0) });
}

Value nearestValue(const Network& network, const NetworkQuery& query) {
	const NetworkHit hit = nearestSegment(network, query);
	if (hit.segment == noSegment) {
		return missedValue(network, query);
	}
	const NetworkSegment& segment = network.segments[hit.segment];
	const math::Vector run        = segment.b - segment.a;
	const math::Vector at         = segment.a + run * hit.share;
	const double along            = segment.along + hit.share * math::length(run);
	const Value direction         = pointValue(math::normalized(run), network.dimensions);
	return nearestRecord({ numberValue(along), pointValue(at, network.dimensions), direction, numberValue(hit.distance), network.fields[segment.line], numberValue(1), numberValue(segment.line), numberValue(sideOf(segment, at, query)) });
}

Value segmentRecord(const Network& network, std::uint32_t index) {
	static const std::vector<std::string> names = { "a", "along", "b", "fields", "line" };
	static const Value shaped                   = recordValue(names, std::vector<Value>(names.size(), numberValue(0)));
	const NetworkSegment& segment               = network.segments[index];
	Value record                                = shaped;
	record.items                                = { pointValue(segment.a, network.dimensions), numberValue(segment.along), pointValue(segment.b, network.dimensions), network.fields[segment.line], numberValue(segment.line) };
	return record;
}

Value withinValue(const Network& network, const NetworkBox& box) {
	std::vector<Value> items;
	for (const std::uint32_t segment : segmentsWithin(network, box)) {
		items.push_back(segmentRecord(network, segment));
	}
	return listValue(std::move(items));
}

}
