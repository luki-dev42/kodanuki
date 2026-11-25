#include "plugins/geometry/tree/lazy.h"
#include "plugins/geometry/tree/boxes.h"
#include "plugins/geometry/tree/build.h"
#include "plugins/geometry/tree/cut.h"
#include "plugins/geometry/tree/grow.h"
#include "plugins/geometry/tree/pack.h"
#include "plugins/geometry/tree/tiles.h"
#include "plugins/geometry/tree/walk.h"
#include "sdf/compare.h"
#include "shared/parallel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace kodanuki {

constexpr int parallelDepth         = 4;
constexpr std::size_t mostReserved  = std::size_t(16) << 30;
constexpr std::size_t reservedParts = 64;
constexpr std::uint64_t trimEvery   = 256;
constexpr std::uint8_t forgetAge    = 3;
constexpr double formerShare        = 1e-6;
constexpr int deepestCap            = 250;
constexpr std::size_t followedChunk = 4096;

struct Opening {
	std::uint32_t node;
	bool sharp;
	bool refined;
};

struct PendingChild {
	int slot;
	std::vector<std::uint32_t> objects;
};

struct NodePlanning {
	const GrowContext& context;
	Box box;
	int depth;
	int threads;
};

struct PlannedNode {
	std::vector<Plan> children;
	std::vector<Plan> slots;
};

struct TileLoad {
	bool beside;
	bool forced;
	bool beyond;
	bool ran;
	std::vector<std::uint32_t> listed;
};

struct BuiltNode {
	Subtree packed;
	std::vector<PendingChild> pending;
	double tolerance;
	bool loose;
	TileLoad tiles;
};

struct PassRound {
	const std::vector<PassMember>& passes;
	PassMoment moment;
};

struct Visit {
	std::uint32_t node;
	Box box;
};

struct Aged {
	std::uint32_t node;
	double footprint;
};

struct Moved {
	std::uint32_t object;
	bool reaches;
};

struct Keyed {
	std::uint64_t key;
	std::uint32_t place;
};

struct MovedIn {
	std::vector<std::uint32_t> objects;
	std::vector<std::uint64_t> slots;
};

bool keyedBefore(const Keyed& a, const Keyed& b) {
	return a.key < b.key;
}

Tree& growingTree(const Geometry& geometry) {
	return const_cast<Tree&>(treeOf(geometry));
}

template <typename T>
void reserveParts(std::vector<T>& held, std::size_t parts) {
	const std::size_t bytes = std::min(machineMemory() / 4, mostReserved) / reservedParts * parts;
	held.reserve(bytes / sizeof(T));
}

void reserveTiles(Tree& tree);

void reserveTree(Tree& tree) {
	reserveTiles(tree);
	reserveParts(tree.nodes, 2);
	reserveParts(tree.growth->records, 4);
	reserveParts(tree.growth->objects, 2);
	reserveParts(tree.surfaces, 4);
	reserveParts(tree.solids, 1);
	reserveParts(tree.runs, 21);
	reserveParts(tree.lists, 2);
	reserveParts(tree.references, 4);
	reserveParts(tree.cuts, 5);
	reserveParts(tree.cutSteps, 19);
}

Span spanOf(std::uint32_t first, std::size_t count) {
	return Span {
		.first = first,
		.count = std::uint32_t(count),
	};
}

NodeSpans noSpans() {
	return NodeSpans {
		.nodes    = spanOf(0, 0),
		.surfaces = spanOf(0, 0),
		.solids   = spanOf(0, 0),
		.runs     = spanOf(0, 0),
		.objects  = spanOf(0, 0),
	};
}

PackedNode unbuiltNode() {
	return PackedNode {
		.lowerKinds   = 0,
		.upperKinds   = 0,
		.firstBranch  = 0,
		.firstSurface = 0,
		.firstSolid   = 0,
		.coarse       = 0,
		.busy         = 0,
		.state        = std::uint8_t(NodeState::Unbuilt),
		.stamp        = 0,
	};
}

Tree emptyTree(const Region& region) {
	const CellEntry root = {
		.kind    = CellKind::Empty,
		.payload = 0,
	};

	return Tree {
		.region     = region,
		.root       = root,
		.nodes      = {},
		.surfaces   = {},
		.solids     = {},
		.runs       = {},
		.lists      = {},
		.references = {},
		.cuts       = {},
		.cutSteps   = {},
		.frame      = 0,
		.growth     = nullptr,
	};
}

Box slotBox(const Box& box, int slot) {
	return octantBox(octantBox(box, slot >> 3), slot & 7);
}

bool nearSpan(const math::Interval& span, const math::Interval& former) {
	const double slack = formerShare * (former.upper - former.lower);
	return std::abs(span.lower - former.lower) <= slack && std::abs(span.upper - former.upper) <= slack;
}

bool nearBox(const Box& box, const Box& former) {
	return nearSpan(box.axes[0], former.axes[0]) && nearSpan(box.axes[1], former.axes[1]) && nearSpan(box.axes[2], former.axes[2]);
}

Box childBox(const Tree& tree, const Box& box, int slot) {
	const Box halved = slotBox(box, slot);
	for (const Box& former : tree.growth->formerRoots) {
		if (nearBox(halved, former)) {
			return former;
		}
	}
	return halved;
}

int grownLevels(const Tree& tree) {
	return tree.growth ? 2 * int(tree.growth->formerRoots.size()) : 0;
}

Box nodeBox(const Tree& tree, std::uint32_t node) {
	const NodeRecord& record = tree.growth->records[node];
	if (record.parent == noNode) {
		return regionBox(tree.region);
	}
	return childBox(tree, nodeBox(tree, record.parent), record.slot);
}

std::uint32_t standingNode(const Tree& tree, std::uint32_t node) {
	switch (NodeState(tree.nodes[node].state)) {
		case NodeState::Loose:
		case NodeState::Fine: return node;
		case NodeState::Forwarded: return standingNode(tree, tree.growth->records[node].forward);
		case NodeState::Unbuilt:
		case NodeState::Opening: break;
	}

	return noNode;
}

std::vector<std::uint32_t> spanObjects(const Growth& growth, const Span& list) {
	return std::vector<std::uint32_t>(growth.objects.begin() + list.first, growth.objects.begin() + list.first + list.count);
}

std::vector<std::uint32_t> unitedList(const Growth& growth, const Span& list, const std::vector<std::uint32_t>& more) {
	std::vector<std::uint32_t> united = spanObjects(growth, list);
	united.insert(united.end(), more.begin(), more.end());
	std::sort(united.begin(), united.end());
	united.erase(std::unique(united.begin(), united.end()), united.end());
	return united;
}

CellPrograms listedPrograms(const Growth& growth, const std::vector<std::uint32_t>& objects, const Box& box) {
	CellPrograms listed = {
		.objects  = {},
		.programs = {},
	};

	for (const std::uint32_t object : objects) {
		if (boxesMeet(growth.boxes.boxes[object], box)) {
			listed.objects.push_back(object);
			listed.programs.push_back(borrowedProgram(slotProgram(growth.scene, growth.slots, object)));
		}
	}
	return listed;
}

std::vector<Plan> plannedChildren(const NodePlanning& planning, const std::shared_ptr<const CellPrograms>& programs) {
	std::vector<Plan> plans(8, pendingPlan());
	inParallel(planning.threads, 8, [&](std::size_t octant) {
		const Sprout child = {
			.box      = octantBox(planning.box, int(octant)),
			.depth    = planning.depth + 1,
			.programs = programs,
		};

		plans[octant] = planCell(planning.context, child);
	});

	return plans;
}

std::vector<Plan> plannedSlots(const NodePlanning& planning, const std::vector<Plan>& children) {
	std::vector<std::shared_ptr<const CellPrograms>> cut(8);
	std::vector<int> slots;
	for (int slot = 0; slot < 64; ++slot) {
		if (children[std::size_t(slot >> 3)].kind != CellKind::Branch) {
			continue;
		}
		cut[std::size_t(slot >> 3)] = (slot & 7) == 0 ? std::make_shared<const CellPrograms>(children[std::size_t(slot >> 3)].programs) : cut[std::size_t(slot >> 3)];
		slots.push_back(slot);
	}
	std::vector<Plan> plans(64, pendingPlan());
	inParallel(planning.threads, slots.size(), [&](std::size_t k) {
		const Sprout cell = {
			.box      = slotBox(planning.box, slots[k]),
			.depth    = planning.depth + 2,
			.programs = cut[std::size_t(slots[k] >> 3)],
		};

		plans[std::size_t(slots[k])]                   = planCell(planning.context, cell);
		plans[std::size_t(slots[k])].programs.programs = {};
	});

	return plans;
}

PlannedNode plannedNode(Growth& growth, const NodePlanning& planning, const std::shared_ptr<const CellPrograms>& programs) {
	const int lent      = planning.depth < parallelDepth ? growth.idle.exchange(0) : 0;
	NodePlanning spread = planning;
	spread.threads      = 1 + std::max(lent, 0);

	PlannedNode planned = {
		.children = plannedChildren(spread, programs),
		.slots    = {},
	};

	planned.slots = plannedSlots(spread, planned.children);
	growth.idle.fetch_add(lent);
	return planned;
}

Plan forcedPlan(const CellPrograms& programs, const Box& box) {
	const CellCut cut = cutCell(programs, box);
	Plan plan         = pendingPlan();
	plan.kind         = cut.occupancy == Occupancy::Filled ? CellKind::Filled : CellKind::Branch;
	plan.programs     = cut.programs;
	return plan;
}

PlannedNode forcedNode(const NodePlanning& planning, const CellPrograms& programs) {
	PlannedNode planned = {
		.children = {},
		.slots    = std::vector<Plan>(64, pendingPlan()),
	};

	for (int octant = 0; octant < 8; ++octant) {
		planned.children.push_back(forcedPlan(programs, octantBox(planning.box, octant)));
	}
	for (int slot = 0; slot < 64; ++slot) {
		const Plan& child = planned.children[std::size_t(slot >> 3)];
		if (child.kind == CellKind::Branch) {
			planned.slots[std::size_t(slot)] = forcedPlan(child.programs, slotBox(planning.box, slot));
		}
	}
	return planned;
}

TileLoad tileLoad(Tree& tree, const NodeRecord& record, const Box& box) {
	TileLoad load = {
		.beside = false,
		.forced = false,
		.beyond = false,
		.ran    = false,
		.listed = spanObjects(*tree.growth, record.objects),
	};

	if (record.tiles == TileListing::Listed || record.tiles == TileListing::Ran || tree.growth->tiles.refused) {
		return load;
	}
	if (cellBesideTiles(tree.growth->scene, box)) {
		load.beside = true;
		return load;
	}
	if (!reachMeets(tree.growth->reach, box)) {
		load.beyond = true;
		return load;
	}
	if (cellAboveTiles(tree.growth->scene, box) && record.depth + 2 < tree.growth->depthCap) {
		load.forced = true;
		return load;
	}
	load.ran    = true;
	load.listed = unitedList(*tree.growth, record.objects, ranTiles(tree, box));
	return load;
}

bool branches(const PlannedNode& planned, int slot) {
	return planned.children[std::size_t(slot >> 3)].kind == CellKind::Branch;
}

Subtree plannedOctree(const Sprout& root, const PlannedNode& planned) {
	Subtree octree            = emptySubtree(root);
	const std::uint32_t first = openBranch(octree, 0);
	for (int slot = 0; slot < 64; slot += 8) {
		if (!branches(planned, slot)) {
			writeLeaf(octree, first + std::uint32_t(slot >> 3), planned.children[std::size_t(slot >> 3)]);
			continue;
		}
		const std::uint32_t below = openBranch(octree, first + std::uint32_t(slot >> 3));
		for (int octant = 0; octant < 8; ++octant) {
			writeLeaf(octree, below + std::uint32_t(octant), planned.slots[std::size_t(slot + octant)]);
		}
	}
	return octree;
}

std::vector<PendingChild> pendingChildren(const PlannedNode& planned) {
	std::vector<PendingChild> pending;
	for (int slot = 0; slot < 64; ++slot) {
		if (!branches(planned, slot) || planned.slots[std::size_t(slot)].kind != CellKind::Branch) {
			continue;
		}

		const PendingChild child = {
			.slot    = slot,
			.objects = planned.slots[std::size_t(slot)].programs.objects,
		};

		pending.push_back(child);
	}
	return pending;
}

bool fittedLooser(const Detail& detail, const Plan& plan, const Box& box) {
	return plan.kind != CellKind::Branch && !plan.run.empty() && cellTolerance(detail, box) > sharpTolerance(detail, box);
}

bool anyLooser(const NodePlanning& planning, const PlannedNode& planned) {
	if (planning.context.sharp) {
		return false;
	}
	for (int slot = 0; slot < 64; ++slot) {
		const bool child = (slot & 7) == 0 && fittedLooser(planning.context.detail, planned.children[std::size_t(slot >> 3)], octantBox(planning.box, slot >> 3));
		if (child || (branches(planned, slot) && fittedLooser(planning.context.detail, planned.slots[std::size_t(slot)], slotBox(planning.box, slot)))) {
			return true;
		}
	}
	return false;
}

Subtree passedSubtree(const PassRound& round, const PassQuery& query, PassState& state) {
	Subtree passed = query.subtree;
	for (const PassMember member : round.passes) {
		if (passMoment(member) != round.moment) {
			continue;
		}

		const PassQuery asked = {
			.context  = query.context,
			.subtree  = passed,
			.programs = query.programs,
			.tree     = query.tree,
		};

		passed = passSubtree(member, asked, state);
	}
	return passed;
}

Subtree finishedNode(const std::vector<PassMember>& passes, const PassQuery& query) {
	PassState state = emptyPassState();

	const PassRound grown = {
		.passes = passes,
		.moment = PassMoment::Grown,
	};

	const Subtree packed = packedSubtree(passedSubtree(grown, query, state));

	const PassQuery asked = {
		.context  = query.context,
		.subtree  = packed,
		.programs = query.programs,
		.tree     = query.tree,
	};

	const PassRound placed = {
		.passes = passes,
		.moment = PassMoment::Placed,
	};

	return passedSubtree(placed, asked, state);
}

BuiltNode builtNode(Tree& tree, const Opening& opening) {
	Growth& growth           = *tree.growth;
	const NodeRecord& record = growth.records[opening.node];
	const Box box            = nodeBox(tree, opening.node);

	const GrowContext context = {
		.scene    = growth.scene,
		.slots    = growth.slots,
		.detail   = growth.detail,
		.depthCap = growth.depthCap,
		.faces    = growth.faces,
		.sharp    = opening.sharp,
	};

	const NodePlanning planning = {
		.context = context,
		.box     = box,
		.depth   = record.depth,
		.threads = 1,
	};

	const TileLoad tiles                               = tileLoad(tree, record, box);
	const std::shared_ptr<const CellPrograms> programs = std::make_shared<const CellPrograms>(cutCell(listedPrograms(growth, tiles.listed, box), box).programs);
	const PlannedNode planned                          = tiles.forced ? forcedNode(planning, *programs) : plannedNode(growth, planning, programs);

	const Sprout root = {
		.box      = box,
		.depth    = record.depth,
		.programs = programs,
	};

	const Subtree octree = plannedOctree(root, planned);
	const Tree none      = emptyTree(tree.region);

	const PassQuery query = {
		.context  = context,
		.subtree  = octree,
		.programs = *programs,
		.tree     = none,
	};

	const bool loose = anyLooser(planning, planned);
	return BuiltNode {
		.packed    = finishedNode(growth.passes, query),
		.pending   = pendingChildren(planned),
		.tolerance = loose ? cellTolerance(growth.detail, box) : sharpTolerance(growth.detail, box),
		.loose     = loose,
		.tiles     = tiles,
	};
}

std::vector<SurfaceLeaf> placedSurfaces(const Subtree& packed, std::uint32_t runs, const std::vector<std::uint32_t>& lists) {
	std::vector<SurfaceLeaf> surfaces = packed.surfaces;
	for (SurfaceLeaf& leaf : surfaces) {
		leaf.run += runs;
		leaf.programs = lists[leaf.programs];
	}
	return surfaces;
}

std::vector<SolidLeaf> placedSolids(const Subtree& packed, const std::vector<std::uint32_t>& lists) {
	std::vector<SolidLeaf> solids = packed.solids;
	for (SolidLeaf& leaf : solids) {
		leaf.programs = lists[leaf.programs];
	}
	return solids;
}

NodeSpans storedLeaves(Tree& tree, const Subtree& packed, const std::vector<std::uint32_t>& lists) {
	TreeStores& stores = tree.growth->stores;
	NodeSpans owned    = noSpans();
	owned.runs         = storedRow(tree.runs, stores.runs, packed.runs, "face runs");
	owned.surfaces     = storedRow(tree.surfaces, stores.surfaces, placedSurfaces(packed, owned.runs.first, lists), "surface leaves");
	owned.solids       = storedRow(tree.solids, stores.solids, placedSolids(packed, lists), "solid leaves");
	return owned;
}

Span storedNodes(Tree& tree, const std::vector<NodeRecord>& records, const std::vector<PackedNode>& nodes) {
	const std::uint32_t first = placedSpan(tree.growth->stores.nodes, tree.nodes.size(), nodes.size());
	writtenRow(tree.growth->records, first, records, "node records");
	return writtenRow(tree.nodes, first, nodes, "nodes");
}

std::vector<std::uint32_t> childObjects(const std::vector<PendingChild>& pending) {
	std::vector<std::uint32_t> objects;
	for (const PendingChild& child : pending) {
		objects.insert(objects.end(), child.objects.begin(), child.objects.end());
	}
	return objects;
}

NodeSpans writtenPlaceholders(Tree& tree, std::uint32_t parent, const std::vector<PendingChild>& pending, TileListing listing) {
	Growth& growth           = *tree.growth;
	const std::uint8_t depth = parent == noNode ? 0 : std::uint8_t(growth.records[parent].depth + 2);
	NodeSpans written        = noSpans();
	written.objects          = storedRow(growth.objects, growth.stores.objects, childObjects(pending), "object lists");
	std::uint32_t listed     = written.objects.first;
	std::vector<NodeRecord> records;
	for (const PendingChild& child : pending) {
		const NodeRecord record = {
			.parent    = parent,
			.forward   = noNode,
			.objects   = spanOf(listed, child.objects.size()),
			.tolerance = 0,
			.slot      = std::uint8_t(child.slot),
			.depth     = depth,
			.tiles     = listing,
			.owned     = noSpans(),
		};

		records.push_back(record);
		listed += std::uint32_t(child.objects.size());
	}
	written.nodes = storedNodes(tree, records, std::vector<PackedNode>(pending.size(), unbuiltNode()));
	return written;
}

std::uint32_t refinedPlace(Tree& tree, std::uint32_t loose) {
	NodeRecord record = tree.growth->records[loose];
	record.forward    = noNode;
	record.owned      = noSpans();
	PackedNode node   = unbuiltNode();
	node.state        = std::uint8_t(NodeState::Fine);

	const std::vector<NodeRecord> records = { record };
	const std::vector<PackedNode> nodes   = { node };
	return storedNodes(tree, records, nodes).first;
}

PackedNode branchingNode(const PackedNode& packed, const std::vector<PendingChild>& pending) {
	PackedNode node = packed;
	for (const PendingChild& child : pending) {
		const std::uint64_t bit = std::uint64_t(1) << child.slot;
		node.lowerKinds |= bit;
		node.upperKinds |= bit;
		node.busy |= std::uint8_t(1u << (child.slot >> 3));
	}
	return node;
}

void writeNode(PackedNode& held, const PackedNode& node) {
	held.lowerKinds   = node.lowerKinds;
	held.upperKinds   = node.upperKinds;
	held.firstBranch  = node.firstBranch;
	held.firstSurface = node.firstSurface;
	held.firstSolid   = node.firstSolid;
	held.coarse       = node.coarse;
	held.busy         = node.busy;
}

void relistNode(Growth& growth, std::uint32_t node, const std::vector<std::uint32_t>& listed);

void ranInto(Tree& tree, std::uint32_t node, const TileLoad& tiles) {
	if (tiles.beside) {
		tree.growth->records[node].tiles = TileListing::Listed;
	}
	if (tiles.beyond) {
		tree.growth->records[node].tiles = TileListing::Beyond;
	}
	if (!tiles.ran) {
		return;
	}
	relistNode(*tree.growth, node, tiles.listed);
	tree.growth->records[node].tiles = TileListing::Ran;
}

TileListing childListing(const TileLoad& tiles) {
	return tiles.forced || tiles.beyond ? TileListing::Waiting : TileListing::Listed;
}

std::uint32_t placedNode(Tree& tree, const Opening& opening, const BuiltNode& built) {
	NodeSpans owned            = storedLeaves(tree, built.packed, sharedLists(tree, built.packed));
	const std::uint32_t placed = opening.refined ? refinedPlace(tree, opening.node) : opening.node;
	ranInto(tree, placed, built.tiles);
	const NodeSpans children = writtenPlaceholders(tree, placed, built.pending, childListing(built.tiles));
	owned.nodes              = children.nodes;
	owned.objects            = children.objects;
	PackedNode node          = branchingNode(built.packed.root.node, built.pending);
	node.firstBranch         = owned.nodes.first;
	node.firstSurface += owned.surfaces.first;
	node.firstSolid += owned.solids.first;
	writeNode(tree.nodes[placed], node);
	tree.growth->records[placed].owned     = owned;
	tree.growth->records[placed].tolerance = float(built.tolerance);
	return placed;
}

void publishNode(Tree& tree, const Opening& opening, const BuiltNode& built) {
	const std::lock_guard<std::mutex> hold(tree.growth->lock);
	const std::uint32_t placed                 = placedNode(tree, opening, built);
	const NodeState state                      = opening.refined ? NodeState::Forwarded : built.loose ? NodeState::Loose
																									  : NodeState::Fine;
	tree.growth->records[opening.node].forward = opening.refined ? placed : noNode;
	tree.growth->opened += 1;
	if (opening.refined) {
		tree.growth->forwarded.push_back(opening.node);
	}
	std::atomic_ref<std::uint8_t> published(tree.nodes[opening.node].state);
	published.store(std::uint8_t(state), std::memory_order_release);
	published.notify_all();
}

void awaitNode(Tree& tree, std::uint32_t node) {
	tree.growth->idle.fetch_add(1);
	std::atomic_ref<std::uint8_t>(tree.nodes[node].state).wait(std::uint8_t(NodeState::Opening), std::memory_order_acquire);
	tree.growth->idle.fetch_sub(1);
}

std::uint32_t openedCell(Tree& tree, std::uint32_t node, bool sharp) {
	std::atomic_ref<std::uint8_t> state(tree.nodes[node].state);
	std::uint8_t found = state.load(std::memory_order_acquire);
	switch (NodeState(found)) {
		case NodeState::Fine: return node;
		case NodeState::Forwarded: return openedCell(tree, tree.growth->records[node].forward, sharp);
		case NodeState::Opening:
			awaitNode(tree, node);
			return openedCell(tree, node, sharp);
		case NodeState::Loose:
			if (!sharp) {
				return node;
			}
			break;
		case NodeState::Unbuilt: break;
	}

	if (state.compare_exchange_strong(found, std::uint8_t(NodeState::Opening), std::memory_order_acq_rel)) {
		const Opening opening = {
			.node    = node,
			.sharp   = sharp,
			.refined = NodeState(found) == NodeState::Loose,
		};

		publishNode(tree, opening, builtNode(tree, opening));
		if (tree.growth->records[node].depth < parallelDepth) {
			releasedFreeMemory();
		}
	}
	return openedCell(tree, node, sharp);
}

std::uint32_t openedNode(const Geometry& geometry, std::uint32_t node, bool sharp) {
	return openedCell(growingTree(geometry), node, sharp);
}

bool walkedAs(std::uint8_t state, bool sharp) {
	return NodeState(state) == NodeState::Fine || (NodeState(state) == NodeState::Loose && !sharp);
}

std::uint32_t enteredNode(const Geometry& geometry, std::uint32_t node, bool sharp) {
	Tree& tree                  = growingTree(geometry);
	const std::uint8_t state    = std::atomic_ref<std::uint8_t>(tree.nodes[node].state).load(std::memory_order_acquire);
	const std::uint32_t entered = walkedAs(state, sharp) ? node : openedCell(tree, node, sharp);
	std::atomic_ref<std::uint8_t> stamp(tree.nodes[entered].stamp);
	if (stamp.load(std::memory_order_relaxed) != tree.frame) {
		stamp.store(tree.frame, std::memory_order_relaxed);
	}
	return entered;
}

std::vector<Program> reachedPrograms(const std::vector<Program>& programs, const Reach& reach) {
	std::vector<Program> reached;
	for (const Program& program : programs) {
		reached.push_back(reachedProgram(program, reach));
	}
	return reached;
}

std::vector<ObjectSlot> sceneSlots(const Scene& scene, const std::vector<Program>& reached) {
	std::vector<ObjectSlot> slots;
	for (const std::uint64_t key : objectKeys(scene)) {
		const ObjectSlot slot = {
			.key     = key,
			.object  = std::uint32_t(slots.size()),
			.leaves  = leastLeaf(scene.objectPrograms[slots.size()]),
			.tile    = noTile,
			.program = &reached[slots.size()],
		};

		slots.push_back(slot);
	}
	return slots;
}

TileListing rootListing(const Scene& scene) {
	return scene.tiles.empty() ? TileListing::Listed : TileListing::Waiting;
}

void reserveTiles(Tree& tree) {
	Growth& growth = *tree.growth;
	if (growth.scene.tiles.empty()) {
		return;
	}
	reserveParts(growth.slots, 1);
	reserveParts(growth.boxes.boxes, 2);
	growth.scene.tileLeaves->leaves.clear();
	reserveParts(growth.scene.tileLeaves->leaves, 1);
}

TileSource tileSource(const LazyInput& input) {
	return TileSource {
		.scene     = input.scene,
		.least     = input.detail.least,
		.forgiving = input.forgiving,
	};
}

Tree lazyTree(const LazyInput& input) {
	largeBlocksMapped();
	Tree tree            = emptyTree(input.region);
	Growth* const growth = new Growth {
		.scene       = input.scene,
		.detail      = input.detail,
		.passes      = input.passes,
		.depthCap    = input.depthCap,
		.workers     = std::max(1, input.workers),
		.faces       = input.faces,
		.records     = {},
		.objects     = {},
		.slots       = {},
		.reached     = reachedPrograms(input.scene.objectPrograms, openReach(input.region)),
		.objectSlots = rootPrograms(input.scene).objects,
		.boxes       = input.boxes,
		.formerRoots = {},
		.forwarded   = {},
		.stores      = emptyStores(),
		.shared      = emptySharedPrograms(),
		.tiles       = noTiles(tileSource(input)),
		.reach       = openReach(input.region),
		.memory      = input.memory,
		.opened      = 0,
		.reset       = 0,
		.forgotten   = 0,
		.rooted      = 0,
		.trimmed     = 0,
		.swept       = 0,
		.lock        = {},
		.idle        = 0,
	};

	tree.growth   = std::shared_ptr<Growth>(growth);
	growth->slots = sceneSlots(input.scene, growth->reached);
	reserveTree(tree);

	const PendingChild whole = {
		.slot    = 0,
		.objects = rootPrograms(input.scene).objects,
	};

	const std::vector<PendingChild> pending = { whole };
	tree.root.kind                          = CellKind::Branch;
	tree.root.payload                       = writtenPlaceholders(tree, noNode, pending, rootListing(input.scene)).nodes.first;
	return tree;
}

void freedNodes(Tree& tree, const Span& span) {
	if (span.count == 0) {
		return;
	}
	SpanStore& store  = tree.growth->stores.nodes;
	const Span joined = givenSpan(store, span);
	releasedRow(tree.nodes, joined, span);
	releasedRow(tree.growth->records, joined, span);
	droppedEnd(store, joined, tree.nodes.size());
}

void releasedLeafLists(Tree& tree, const NodeSpans& owned) {
	for (std::uint32_t k = owned.surfaces.first; k < owned.surfaces.first + owned.surfaces.count; ++k) {
		releasedList(tree, tree.surfaces[k].programs);
	}
	for (std::uint32_t k = owned.solids.first; k < owned.solids.first + owned.solids.count; ++k) {
		releasedList(tree, tree.solids[k].programs);
	}
}

void releasedRows(Tree& tree, const NodeSpans& owned) {
	TreeStores& stores = tree.growth->stores;
	releasedLeafLists(tree, owned);
	freedRow(tree.surfaces, stores.surfaces, owned.surfaces);
	freedRow(tree.solids, stores.solids, owned.solids);
	freedRow(tree.runs, stores.runs, owned.runs);
	freedRow(tree.growth->objects, stores.objects, owned.objects);
	freedNodes(tree, owned.nodes);
}

bool listedAside(const Growth& growth, std::uint32_t node) {
	const NodeRecord& record = growth.records[node];
	if (record.objects.count == 0) {
		return false;
	}
	if (record.parent == noNode) {
		return true;
	}
	const Span& lists = growth.records[record.parent].owned.objects;
	return record.objects.first < lists.first || record.objects.first >= lists.first + lists.count;
}

void dropList(Growth& growth, std::uint32_t node) {
	if (listedAside(growth, node)) {
		freedRow(growth.objects, growth.stores.objects, growth.records[node].objects);
	}
	growth.records[node].objects = spanOf(0, 0);
}

bool builtState(std::uint8_t state) {
	return NodeState(state) == NodeState::Loose || NodeState(state) == NodeState::Fine || NodeState(state) == NodeState::Forwarded;
}

std::uint32_t droppedBelow(Tree& tree, std::uint32_t node) {
	Growth& growth = *tree.growth;
	if (!builtState(tree.nodes[node].state)) {
		return 0;
	}
	std::uint32_t dropped     = 1;
	const std::uint32_t finer = growth.records[node].forward;
	if (finer != noNode) {
		dropped += droppedBelow(tree, finer);
		growth.records[finer].objects = spanOf(0, 0);
		freedNodes(tree, spanOf(finer, 1));
	}
	const Span children = growth.records[node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		dropped += droppedBelow(tree, child);
		dropList(growth, child);
	}
	releasedRows(tree, growth.records[node].owned);
	growth.records[node].owned     = noSpans();
	growth.records[node].forward   = noNode;
	growth.records[node].tolerance = 0;
	tree.nodes[node].state         = std::uint8_t(NodeState::Unbuilt);
	return dropped;
}

std::uint32_t clearedNode(Tree& tree, std::uint32_t node) {
	const std::uint32_t dropped = droppedBelow(tree, node);
	const std::uint8_t stamp    = tree.nodes[node].stamp;
	tree.nodes[node]            = unbuiltNode();
	tree.nodes[node].stamp      = stamp;
	return dropped;
}

void resetNode(Tree& tree, std::uint32_t node) {
	tree.growth->reset += clearedNode(tree, node);
}

void collapseForward(Tree& tree, std::uint32_t loose) {
	Growth& growth = *tree.growth;
	if (NodeState(tree.nodes[loose].state) != NodeState::Forwarded) {
		return;
	}
	const std::uint32_t finer     = growth.records[loose].forward;
	growth.records[loose].forward = noNode;
	droppedBelow(tree, loose);
	tree.nodes[loose]               = tree.nodes[finer];
	growth.records[loose].owned     = growth.records[finer].owned;
	growth.records[loose].tolerance = growth.records[finer].tolerance;
	const Span children             = growth.records[loose].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		growth.records[child].parent = loose;
	}
	growth.records[finer].owned   = noSpans();
	growth.records[finer].objects = spanOf(0, 0);
	tree.nodes[finer]             = unbuiltNode();
	freedNodes(tree, spanOf(finer, 1));
}

void collapseForwards(Tree& tree) {
	const std::vector<std::uint32_t> forwarded = std::exchange(tree.growth->forwarded, {});
	for (const std::uint32_t loose : forwarded) {
		collapseForward(tree, loose);
	}
}

void resetStale(Tree& tree, const Visit& visit) {
	const Growth& growth  = *tree.growth;
	const NodeState state = NodeState(tree.nodes[visit.node].state);
	if (!builtState(tree.nodes[visit.node].state)) {
		return;
	}
	const double asked = state == NodeState::Loose ? cellTolerance(growth.detail, visit.box) : sharpTolerance(growth.detail, visit.box);
	if (growth.records[visit.node].tolerance > 2 * asked) {
		resetNode(tree, visit.node);
		return;
	}
	const Span children = growth.records[visit.node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		const Visit below = {
			.node = child,
			.box  = childBox(tree, visit.box, growth.records[child].slot),
		};

		resetStale(tree, below);
	}
}

Visit rootVisit(const Tree& tree) {
	return Visit {
		.node = tree.root.payload,
		.box  = regionBox(tree.region),
	};
}

std::size_t arrayBytes(const Tree& tree) {
	const std::size_t leaves   = tree.nodes.size() * sizeof(PackedNode) + tree.surfaces.size() * sizeof(SurfaceLeaf) + tree.solids.size() * sizeof(SolidLeaf);
	const std::size_t programs = tree.lists.size() * sizeof(ProgramList) + tree.references.size() * sizeof(ProgramReference);
	const std::size_t cuts     = tree.cuts.size() * sizeof(ProgramCut) + tree.cutSteps.size() * sizeof(std::uint16_t);
	return leaves + tree.runs.size() * sizeof(std::uint32_t) + programs + cuts;
}

std::size_t freeBytes(const TreeStores& stores) {
	const std::size_t leaves   = stores.nodes.free * (sizeof(PackedNode) + sizeof(NodeRecord)) + stores.surfaces.free * sizeof(SurfaceLeaf) + stores.solids.free * sizeof(SolidLeaf);
	const std::size_t programs = stores.lists.free * sizeof(ProgramList) + stores.references.free * sizeof(ProgramReference);
	const std::size_t cuts     = stores.cuts.free * sizeof(ProgramCut) + stores.cutSteps.free * sizeof(std::uint16_t);
	return leaves + (stores.runs.free + stores.objects.free) * sizeof(std::uint32_t) + programs + cuts;
}

std::size_t growthBytes(const Growth& growth) {
	const std::size_t records = growth.records.size() * sizeof(NodeRecord) + growth.objects.size() * sizeof(std::uint32_t);
	return records + sharedBytes(growth.shared) - freeBytes(growth.stores);
}

std::size_t slotBytes(const Growth& growth) {
	return growth.slots.size() * sizeof(ObjectSlot) + growth.boxes.boxes.size() * sizeof(Box);
}

std::size_t heldBytes(const Tree& tree) {
	return arrayBytes(tree) + (tree.growth ? growthBytes(*tree.growth) + slotBytes(*tree.growth) + tree.growth->tiles.bytes : 0);
}

template <typename T>
bool nearlyFull(const std::vector<T>& held, const SpanStore& store) {
	return (held.size() - store.free) * 4 > held.capacity() * 3;
}

bool arraysNearlyFull(const Tree& tree) {
	const TreeStores& stores = tree.growth->stores;
	const bool leaves        = nearlyFull(tree.nodes, stores.nodes) || nearlyFull(tree.surfaces, stores.surfaces) || nearlyFull(tree.solids, stores.solids) || nearlyFull(tree.runs, stores.runs);
	const bool programs      = nearlyFull(tree.lists, stores.lists) || nearlyFull(tree.references, stores.references) || nearlyFull(tree.cuts, stores.cuts) || nearlyFull(tree.cutSteps, stores.cutSteps);
	return leaves || programs || nearlyFull(tree.growth->objects, stores.objects);
}

bool overMemory(const Tree& tree) {
	return heldBytes(tree) > tree.growth->memory || arraysNearlyFull(tree);
}

bool enteredLately(const Tree& tree, std::uint32_t node) {
	return std::uint8_t(tree.frame - tree.nodes[node].stamp) < forgetAge;
}

void agedBelow(Tree& tree, std::uint32_t node) {
	if (!builtState(tree.nodes[node].state)) {
		return;
	}
	tree.nodes[node].stamp = enteredLately(tree, node) ? tree.nodes[node].stamp : std::uint8_t(tree.frame - forgetAge);
	const Span children    = tree.growth->records[node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		agedBelow(tree, child);
	}
}

std::vector<Aged> agedNodes(Tree& tree, const Visit& visit) {
	if (!builtState(tree.nodes[visit.node].state)) {
		return {};
	}
	if (!enteredLately(tree, visit.node)) {
		agedBelow(tree, visit.node);

		const Aged aged = {
			.node      = visit.node,
			.footprint = footprintOver(tree.growth->detail.view, visit.box),
		};

		return { aged };
	}
	std::vector<Aged> aged;
	const Span children = tree.growth->records[visit.node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		const Visit below = {
			.node = child,
			.box  = childBox(tree, visit.box, tree.growth->records[child].slot),
		};

		const std::vector<Aged> found = agedNodes(tree, below);
		aged.insert(aged.end(), found.begin(), found.end());
	}
	return aged;
}

void markedTiles(Tree& tree, const Visit& visit) {
	const NodeRecord& record = tree.growth->records[visit.node];
	markListedTiles(tree, record.objects);
	if (record.tiles == TileListing::Ran) {
		markRanTiles(tree, visit.box);
	}
	if (!builtState(tree.nodes[visit.node].state)) {
		return;
	}
	const Span children = record.owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		const Visit below = {
			.node = child,
			.box  = childBox(tree, visit.box, tree.growth->records[child].slot),
		};

		markedTiles(tree, below);
	}
}

void unlistedTiles(Tree& tree, std::uint32_t node) {
	Growth& growth = *tree.growth;
	if (growth.records[node].tiles != TileListing::Ran) {
		return;
	}
	std::vector<std::uint32_t> listed;
	for (const std::uint32_t object : spanObjects(growth, growth.records[node].objects)) {
		if (growth.slots[object].tile == noTile) {
			listed.push_back(object);
		}
	}
	relistNode(growth, node, listed);
	growth.records[node].tiles = TileListing::Waiting;
}

void sweepTiles(Tree& tree) {
	markedTiles(tree, rootVisit(tree));
	sweptTiles(tree);
	tree.growth->swept = tree.growth->reset + tree.growth->forgotten;
}

void forgetTiles(Tree& tree) {
	const Growth& growth = *tree.growth;
	if (growth.scene.tiles.empty() || growth.reset + growth.forgotten == growth.swept) {
		return;
	}
	sweepTiles(tree);
}

void forgetOld(Tree& tree) {
	std::vector<Aged> aged = agedNodes(tree, rootVisit(tree));
	if (!overMemory(tree)) {
		return;
	}
	std::stable_sort(aged.begin(), aged.end(), [](const Aged& a, const Aged& b) {
		return a.footprint > b.footprint;
	});
	for (std::size_t k = 0; k < aged.size() && overMemory(tree);) {
		const std::size_t round = k + std::max<std::size_t>(1, (aged.size() - k) / 4);
		for (; k < round; ++k) {
			tree.growth->forgotten += clearedNode(tree, aged[k].node);
			unlistedTiles(tree, aged[k].node);
		}
		forgetTiles(tree);
	}
}

void leftReach(Tree& tree, std::uint32_t node) {
	const TileListing tiles = tree.growth->records[node].tiles;
	if (tiles == TileListing::Listed || tiles == TileListing::Beyond) {
		return;
	}
	if (builtState(tree.nodes[node].state)) {
		resetNode(tree, node);
	}
	unlistedTiles(tree, node);
}

void reachedBelow(Tree& tree, const Visit& visit) {
	Growth& growth = *tree.growth;
	if (!reachMeets(growth.reach, visit.box)) {
		leftReach(tree, visit.node);
		return;
	}
	if (!builtState(tree.nodes[visit.node].state)) {
		return;
	}
	if (growth.records[visit.node].tiles == TileListing::Beyond) {
		resetNode(tree, visit.node);
		growth.records[visit.node].tiles = TileListing::Waiting;
		return;
	}
	const Span children = growth.records[visit.node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		const Visit below = {
			.node = child,
			.box  = childBox(tree, visit.box, growth.records[child].slot),
		};

		reachedBelow(tree, below);
	}
}

bool sameReach(const Reach& a, const Reach& b) {
	return a.eye.x == b.eye.x && a.eye.y == b.eye.y && a.eye.z == b.eye.z && a.far == b.far;
}

bool settledBox(const Box& box, const Reach& old, const Reach& now) {
	return (reachHolds(old, box) && reachHolds(now, box)) || (!reachMeets(old, box) && !reachMeets(now, box));
}

void movedPrograms(Growth& growth) {
	for (Program& program : growth.reached) {
		movedReach(program, growth.reach);
	}
	for (const std::unique_ptr<HeldTile>& tile : growth.tiles.tiles) {
		if (!tile) {
			continue;
		}
		for (Program& program : tile->content.scene.objectPrograms) {
			movedReach(program, growth.reach);
		}
	}
}

bool reachedEmpty(const Growth& growth, std::uint32_t node, const Box& box) {
	return cutCell(listedPrograms(growth, spanObjects(growth, growth.records[node].objects), box), box).occupancy == Occupancy::Empty;
}

bool recutSlots(const Tree& tree, const Visit& visit, const Reach& old) {
	const PackedNode& node = tree.nodes[visit.node];
	for (int slot = 0; slot < 64; ++slot) {
		const Box box = slotBox(visit.box, slot);
		if (slotKind(node, slot) == CellKind::Branch || settledBox(box, old, tree.growth->reach)) {
			continue;
		}
		if (slotKind(node, slot) != CellKind::Empty || !reachedEmpty(*tree.growth, visit.node, box)) {
			return true;
		}
	}
	return false;
}

std::vector<std::uint32_t> listedMeeting(const Growth& growth, const Span& list, const Box& box);

void relistedFromParent(Growth& growth, const Visit& visit) {
	const std::uint32_t parent = growth.records[visit.node].parent;
	if (parent == noNode || !reachMeets(growth.reach, visit.box)) {
		return;
	}
	relistNode(growth, visit.node, unitedList(growth, growth.records[visit.node].objects, listedMeeting(growth, growth.records[parent].objects, visit.box)));
}

void recutBelow(Tree& tree, const Visit& visit, const Reach& old) {
	Growth& growth = *tree.growth;
	if (settledBox(visit.box, old, growth.reach)) {
		return;
	}
	relistedFromParent(growth, visit);
	if (growth.records[visit.node].tiles == TileListing::Ran && newTilesReached(tree, visit.box, old)) {
		resetNode(tree, visit.node);
		unlistedTiles(tree, visit.node);
		return;
	}
	if (!builtState(tree.nodes[visit.node].state)) {
		return;
	}
	if (recutSlots(tree, visit, old)) {
		resetNode(tree, visit.node);
		return;
	}
	const Span children = growth.records[visit.node].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		const Visit below = {
			.node = child,
			.box  = childBox(tree, visit.box, growth.records[child].slot),
		};

		recutBelow(tree, below, old);
	}
}

void reachedTiles(Tree& tree) {
	if (tree.growth->scene.tiles.empty() || tree.growth->tiles.refused) {
		return;
	}
	reachedBelow(tree, rootVisit(tree));
	sweepTiles(tree);
}

void moveReach(Tree& tree, const Reach& reach) {
	if (!tree.growth || sameReach(reach, tree.growth->reach)) {
		return;
	}
	const Reach old    = tree.growth->reach;
	tree.growth->reach = reach;
	movedPrograms(*tree.growth);
	collapseForwards(tree);
	reachedTiles(tree);
	recutBelow(tree, rootVisit(tree), old);
}

void prepareTree(Tree& tree, const Shot& shot, bool moved) {
	if (!tree.growth) {
		return;
	}
	collapseForwards(tree);
	tree.growth->detail = viewedDetail(tree.growth->detail, shot);
	if (moved) {
		resetStale(tree, rootVisit(tree));
	}
	forgetOld(tree);
	forgetTiles(tree);
	if (tree.growth->opened - tree.growth->trimmed >= trimEvery) {
		releasedFreeMemory();
		tree.growth->trimmed = tree.growth->opened;
	}
	tree.frame = std::uint8_t(tree.frame + 1);
}

std::vector<Keyed> keyedSlots(const std::vector<ObjectSlot>& slots) {
	std::vector<Keyed> keyed;
	for (std::uint32_t slot = 0; slot < slots.size(); ++slot) {
		const Keyed held = {
			.key   = slots[slot].key,
			.place = slot,
		};

		if (slots[slot].object != noObject && slots[slot].tile == noTile) {
			keyed.push_back(held);
		}
	}
	std::stable_sort(keyed.begin(), keyed.end(), keyedBefore);
	return keyed;
}

std::vector<Keyed> keyedObjects(const std::vector<std::uint64_t>& keys) {
	std::vector<Keyed> keyed;
	for (std::uint32_t object = 0; object < keys.size(); ++object) {
		const Keyed listed = {
			.key   = keys[object],
			.place = object,
		};

		keyed.push_back(listed);
	}
	std::stable_sort(keyed.begin(), keyed.end(), keyedBefore);
	return keyed;
}

std::vector<std::uint32_t> keptObjects(const std::vector<ObjectSlot>& slots, const std::vector<std::uint64_t>& keys) {
	const std::vector<Keyed> held   = keyedSlots(slots);
	const std::vector<Keyed> listed = keyedObjects(keys);
	std::vector<std::uint32_t> kept(slots.size(), noObject);
	std::size_t next = 0;
	for (const Keyed& slot : held) {
		while (next < listed.size() && listed[next].key < slot.key) {
			next += 1;
		}
		if (next < listed.size() && listed[next].key == slot.key) {
			kept[slot.place] = listed[next].place;
			next += 1;
		}
	}
	return kept;
}

std::uint32_t freeSlot(std::vector<ObjectSlot>& slots, std::uint32_t from) {
	std::uint32_t slot = from;
	while (slot < slots.size() && slots[slot].object != noObject) {
		slot += 1;
	}
	const ObjectSlot free = {
		.key     = 0,
		.object  = noObject,
		.leaves  = noLeaf,
		.tile    = noTile,
		.program = nullptr,
	};

	if (slot == slots.size()) {
		slots.push_back(free);
	}
	return slot;
}

std::vector<std::uint32_t> keptSlots(Growth& growth, const std::vector<std::uint64_t>& keys) {
	const std::vector<std::uint32_t> kept = keptObjects(growth.slots, keys);
	std::vector<std::uint32_t> freed;
	growth.objectSlots.assign(keys.size(), noObject);
	for (std::uint32_t slot = 0; slot < kept.size(); ++slot) {
		if (growth.slots[slot].tile != noTile) {
			continue;
		}
		if (kept[slot] == noObject && growth.slots[slot].object != noObject) {
			freed.push_back(slot);
			growth.slots[slot].key = 0;
		}
		if (kept[slot] != noObject) {
			growth.objectSlots[kept[slot]] = slot;
			growth.slots[slot].leaves      = leastLeaf(growth.scene.objectPrograms[kept[slot]]);
		}
		growth.slots[slot].object  = kept[slot];
		growth.slots[slot].program = kept[slot] == noObject ? nullptr : &growth.reached[kept[slot]];
	}
	return freed;
}

std::vector<std::uint32_t> placedObjects(Growth& growth, const std::vector<std::uint64_t>& keys) {
	std::vector<std::uint32_t> changed = keptSlots(growth, keys);
	std::uint32_t free                 = 0;
	for (std::uint32_t object = 0; object < keys.size(); ++object) {
		if (growth.objectSlots[object] != noObject) {
			continue;
		}
		free                       = freeSlot(growth.slots, free);
		growth.slots[free].key     = keys[object];
		growth.slots[free].object  = object;
		growth.slots[free].leaves  = leastLeaf(growth.scene.objectPrograms[object]);
		growth.slots[free].program = &growth.reached[object];
		growth.objectSlots[object] = free;
		changed.push_back(free);
	}
	std::sort(changed.begin(), changed.end());
	changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
	return changed;
}

void boxedSlots(Growth& growth, const std::vector<std::uint32_t>& changed) {
	std::vector<std::uint32_t> objects;
	for (const std::uint32_t slot : changed) {
		if (growth.slots[slot].object != noObject) {
			objects.push_back(growth.slots[slot].object);
		}
	}
	const std::vector<Box> bound = boundObjects(growth.boxes, growth.scene, objects, growth.workers);
	growth.boxes.boxes.resize(growth.slots.size(), nothingBox());
	for (const std::uint32_t slot : changed) {
		growth.boxes.boxes[slot] = nothingBox();
	}
	for (std::size_t k = 0; k < objects.size(); ++k) {
		growth.boxes.boxes[growth.objectSlots[objects[k]]] = bound[k];
	}
}

bool listedIn(const Growth& growth, const Span& list, std::uint32_t object) {
	return std::binary_search(growth.objects.begin() + list.first, growth.objects.begin() + list.first + list.count, object);
}

std::vector<Moved> movedAt(const Growth& growth, std::uint32_t node, const std::vector<Moved>& arriving) {
	const bool waiting = growth.records[node].tiles == TileListing::Waiting;
	std::vector<Moved> moved;
	for (const Moved& one : arriving) {
		if (one.reaches || waiting || listedIn(growth, growth.records[node].objects, one.object)) {
			moved.push_back(one);
		}
	}
	return moved;
}

std::vector<std::uint32_t> movedObjects(const std::vector<Moved>& moved) {
	std::vector<std::uint32_t> objects;
	for (const Moved& one : moved) {
		objects.push_back(one.object);
	}
	return objects;
}

bool listsTiles(const Growth& growth, std::uint32_t node) {
	const TileListing tiles = growth.records[node].tiles;
	return tiles != TileListing::Waiting && tiles != TileListing::Beyond;
}

std::vector<std::uint32_t> relisted(const Growth& growth, std::uint32_t node, const std::vector<Moved>& moved) {
	const Span list                          = growth.records[node].objects;
	const bool tiled                         = listsTiles(growth, node);
	const std::vector<std::uint32_t> objects = movedObjects(moved);
	std::vector<std::uint32_t> listed;
	for (std::uint32_t k = 0; k < list.count; ++k) {
		if (!std::binary_search(objects.begin(), objects.end(), growth.objects[list.first + k])) {
			listed.push_back(growth.objects[list.first + k]);
		}
	}
	for (const Moved& one : moved) {
		if (one.reaches && (tiled || growth.slots[one.object].tile == noTile)) {
			listed.push_back(one.object);
		}
	}
	std::sort(listed.begin(), listed.end());
	return listed;
}

void relistNode(Growth& growth, std::uint32_t node, const std::vector<std::uint32_t>& listed) {
	const Span list  = growth.records[node].objects;
	const bool aside = listedAside(growth, node);
	if (listed.size() == list.count && std::equal(listed.begin(), listed.end(), growth.objects.begin() + list.first)) {
		return;
	}
	if (listed.size() > list.count) {
		dropList(growth, node);
		growth.records[node].objects = storedRow(growth.objects, growth.stores.objects, listed, "object lists");
		return;
	}
	std::copy(listed.begin(), listed.end(), growth.objects.begin() + list.first);
	growth.records[node].objects.count = std::uint32_t(listed.size());
	if (aside) {
		freedRow(growth.objects, growth.stores.objects, spanOf(list.first + std::uint32_t(listed.size()), list.count - listed.size()));
	}
}

std::uint64_t reachedOctant(const CellPrograms& programs, const Box& box, bool coarse) {
	const CellCut cut = cutCell(programs, box);
	if (cut.occupancy == Occupancy::Empty) {
		return 0;
	}
	if (coarse) {
		return 1;
	}
	std::uint64_t reached = 0;
	for (int octant = 0; octant < 8; ++octant) {
		reached |= cutCell(cut.programs, octantBox(box, octant)).occupancy == Occupancy::Empty ? 0 : std::uint64_t(1) << octant;
	}
	return reached;
}

std::uint64_t reachedSlots(const Tree& tree, const Visit& visit, const Moved& moved) {
	if (!moved.reaches || !boxesMeet(tree.growth->boxes.boxes[moved.object], visit.box)) {
		return 0;
	}

	const CellPrograms alone = {
		.objects  = { moved.object },
		.programs = { borrowedProgram(slotProgram(tree.growth->scene, tree.growth->slots, moved.object)) },
	};

	const CellCut whole   = cutCell(alone, visit.box);
	std::uint64_t reached = 0;
	for (int octant = 0; octant < 8 && whole.occupancy != Occupancy::Empty; ++octant) {
		const bool coarse = ((tree.nodes[visit.node].coarse >> octant) & 1) != 0;
		reached |= reachedOctant(whole.programs, octantBox(visit.box, octant), coarse) << (8 * octant);
	}
	return reached;
}

MovedIn movedIn(const Tree& tree, const Visit& visit, const std::vector<Moved>& moved) {
	MovedIn in = {
		.objects = movedObjects(moved),
		.slots   = {},
	};

	for (const Moved& one : moved) {
		in.slots.push_back(reachedSlots(tree, visit, one));
	}
	return in;
}

bool namesMoved(const Tree& tree, std::uint32_t list, const std::vector<std::uint32_t>& objects) {
	const ProgramList& programs = tree.lists[list];
	for (std::uint32_t k = 0; k < programs.count; ++k) {
		if (std::binary_search(objects.begin(), objects.end(), referencedSlot(tree, tree.references[programs.first + k]))) {
			return true;
		}
	}
	return false;
}

std::uint32_t slotList(const Tree& tree, const PackedNode& node, int slot) {
	switch (slotKind(node, slot)) {
		case CellKind::Surface: return tree.surfaces[slotPayload(node, slot)].programs;
		case CellKind::Filled: return tree.solids[slotPayload(node, slot)].programs;
		case CellKind::Empty:
		case CellKind::Branch: break;
	}

	return noList;
}

bool touchedNode(const Tree& tree, std::uint32_t node, const MovedIn& in) {
	std::uint64_t reached = 0;
	for (const std::uint64_t slots : in.slots) {
		reached |= slots;
	}
	const PackedNode& held = tree.nodes[node];
	for (int slot = 0; slot < 64; ++slot) {
		const std::uint32_t list = slotList(tree, held, slot);
		const bool leaf          = slotKind(held, slot) != CellKind::Branch;
		if (leaf && (((reached >> slot) & 1) != 0 || (list != noList && namesMoved(tree, list, in.objects)))) {
			return true;
		}
	}
	return false;
}

std::vector<Moved> movedInto(const MovedIn& in, int slot) {
	std::vector<Moved> moved;
	for (std::size_t k = 0; k < in.objects.size(); ++k) {
		const Moved one = {
			.object  = in.objects[k],
			.reaches = ((in.slots[k] >> slot) & 1) != 0,
		};

		moved.push_back(one);
	}
	return moved;
}

void followMoved(Tree& tree, const Visit& visit, const std::vector<Moved>& arriving);

void followBelow(Tree& tree, const Visit& visit, const MovedIn& in) {
	for (int slot = 0; slot < 64; ++slot) {
		if (slotKind(tree.nodes[visit.node], slot) != CellKind::Branch) {
			continue;
		}

		const Visit below = {
			.node = slotPayload(tree.nodes[visit.node], slot),
			.box  = childBox(tree, visit.box, slot),
		};

		followMoved(tree, below, movedInto(in, slot));
	}
}

void followMoved(Tree& tree, const Visit& visit, const std::vector<Moved>& arriving) {
	Growth& growth                 = *tree.growth;
	const std::vector<Moved> moved = movedAt(growth, visit.node, arriving);
	if (moved.empty()) {
		return;
	}
	relistNode(growth, visit.node, relisted(growth, visit.node, moved));
	if (!builtState(tree.nodes[visit.node].state)) {
		return;
	}
	const MovedIn in = movedIn(tree, visit, moved);
	if (touchedNode(tree, visit.node, in)) {
		resetNode(tree, visit.node);
		return;
	}
	followBelow(tree, visit, in);
}

void followSlots(Tree& tree, const std::vector<std::uint32_t>& changed) {
	if (changed.empty()) {
		return;
	}
	collapseForwards(tree);
	std::vector<Moved> moved;
	for (const std::uint32_t slot : changed) {
		const Moved one = {
			.object  = slot,
			.reaches = tree.growth->slots[slot].object != noObject,
		};

		moved.push_back(one);
	}
	followMoved(tree, rootVisit(tree), moved);
}

void followTiles(Tree& tree) {
	const std::vector<std::uint32_t> stale = staleTiles(tree);
	for (std::size_t first = 0; first < stale.size(); first += followedChunk) {
		const std::vector<std::uint32_t> chunk(stale.begin() + std::ptrdiff_t(first), stale.begin() + std::ptrdiff_t(std::min(stale.size(), first + followedChunk)));
		followSlots(tree, followedTiles(tree, chunk));
		sweepTiles(tree);
	}
}

void followScene(Tree& tree, const std::vector<std::uint64_t>& keys) {
	Growth& growth                           = *tree.growth;
	growth.reached                           = reachedPrograms(growth.scene.objectPrograms, growth.reach);
	const std::vector<std::uint32_t> changed = placedObjects(growth, keys);
	growth.detail.hidden                     = growth.detail.hidden.leaves ? hiddenPrimitives(growth.scene) : noHiddenPrimitives();
	if (!changed.empty()) {
		boxedSlots(growth, changed);
	}
	followSlots(tree, changed);
	followTiles(tree);
}

struct Quarter {
	int quarter;
	double missed;
	double off;
};

Quarter quarterOf(const math::Interval& span, const math::Interval& held, int quarter) {
	const double side  = span.upper - span.lower;
	const double lower = span.lower - quarter * side;
	const double upper = span.upper + (3 - quarter) * side;
	return Quarter {
		.quarter = quarter,
		.missed  = std::max(0.0, lower - held.lower) + std::max(0.0, held.upper - upper),
		.off     = std::abs((lower + upper) - (held.lower + held.upper)),
	};
}

int heldQuarter(const math::Interval& span, const math::Interval& held) {
	Quarter best = quarterOf(span, held, 0);
	for (int quarter = 1; quarter < 4; ++quarter) {
		const Quarter tried = quarterOf(span, held, quarter);
		best                = tried.missed < best.missed || (tried.missed == best.missed && tried.off < best.off) ? tried : best;
	}
	return best.quarter;
}

int rootSlot(const int* quarters) {
	int slot = 0;
	for (int axis = 0; axis < 3; ++axis) {
		slot |= (quarters[axis] >> 1) << (axis + 3);
		slot |= (quarters[axis] & 1) << axis;
	}
	return slot;
}

Region widerRegion(const Region& region, const int* quarters) {
	Region wider = region;
	for (int axis = 0; axis < 3; ++axis) {
		wider.lower[axis] = region.lower[axis] - quarters[axis] * region.size[axis];
		wider.size[axis]  = 4 * region.size[axis];
	}
	return wider;
}

std::vector<std::uint32_t> listedMeeting(const Growth& growth, const Span& list, const Box& box) {
	std::vector<std::uint32_t> meeting;
	for (std::uint32_t k = 0; k < list.count; ++k) {
		const std::uint32_t object = growth.objects[list.first + k];
		if (boxesMeet(growth.boxes.boxes[object], box)) {
			meeting.push_back(object);
		}
	}
	return meeting;
}

std::uint32_t placedRoot(Tree& tree, const Span& list) {
	Growth& growth = *tree.growth;
	const std::vector<std::uint32_t> listed(growth.objects.begin() + list.first, growth.objects.begin() + list.first + list.count);

	const NodeRecord record = {
		.parent    = noNode,
		.forward   = noNode,
		.objects   = storedRow(growth.objects, growth.stores.objects, listed, "object lists"),
		.tolerance = 0,
		.slot      = 0,
		.depth     = 0,
		.tiles     = rootListing(growth.scene),
		.owned     = noSpans(),
	};

	const std::vector<NodeRecord> records = { record };
	const std::vector<PackedNode> nodes   = { unbuiltNode() };
	return storedNodes(tree, records, nodes).first;
}

void movedNode(Tree& tree, std::uint32_t from, std::uint32_t to) {
	Growth& growth            = *tree.growth;
	const NodeRecord placed   = growth.records[to];
	tree.nodes[to]            = tree.nodes[from];
	growth.records[to]        = growth.records[from];
	growth.records[to].parent = placed.parent;
	growth.records[to].slot   = placed.slot;
	const Span children       = growth.records[to].owned.nodes;
	for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
		growth.records[child].parent = to;
	}
	growth.records[from].objects = spanOf(0, 0);
	growth.records[from].owned   = noSpans();
	tree.nodes[from]             = unbuiltNode();
	freedNodes(tree, spanOf(from, 1));
}

void rootAbove(Tree& tree, const int* quarters) {
	Growth& growth          = *tree.growth;
	const std::uint32_t old = tree.root.payload;
	const int slot          = rootSlot(quarters);
	growth.formerRoots.push_back(regionBox(tree.region));
	tree.region = widerRegion(tree.region, quarters);
	for (NodeRecord& record : growth.records) {
		record.depth = std::uint8_t(record.depth + 2);
	}
	growth.depthCap += 2;
	const std::uint32_t root = placedRoot(tree, growth.records[old].objects);
	const Box box            = regionBox(tree.region);
	std::vector<PendingChild> pending;
	for (int k = 0; k < 64; ++k) {
		const PendingChild child = {
			.slot    = k,
			.objects = k == slot ? std::vector<std::uint32_t>() : listedMeeting(growth, growth.records[root].objects, childBox(tree, box, k)),
		};

		pending.push_back(child);
	}
	const NodeSpans owned = writtenPlaceholders(tree, root, pending, rootListing(growth.scene));
	movedNode(tree, old, owned.nodes.first + std::uint32_t(slot));
	PackedNode node            = branchingNode(unbuiltNode(), pending);
	node.firstBranch           = owned.nodes.first;
	node.state                 = std::uint8_t(NodeState::Fine);
	node.stamp                 = tree.frame;
	tree.nodes[root]           = node;
	growth.records[root].owned = owned;
	tree.root.payload          = root;
	growth.rooted += 1;
}

bool finiteBox(const Box& box) {
	for (int axis = 0; axis < 3; ++axis) {
		if (!std::isfinite(box.axes[axis].lower) || !std::isfinite(box.axes[axis].upper)) {
			return false;
		}
	}
	return true;
}

void growRegion(Tree& tree, const Box& held) {
	if (!tree.growth || boxEmpty(held) || !finiteBox(held)) {
		return;
	}
	collapseForwards(tree);
	while (!boxInRegion(held, tree.region) && tree.growth->depthCap + 2 <= deepestCap) {
		const Box box         = regionBox(tree.region);
		const int quarters[3] = { heldQuarter(box.axes[0], held.axes[0]), heldQuarter(box.axes[1], held.axes[1]), heldQuarter(box.axes[2], held.axes[2]) };
		rootAbove(tree, quarters);
	}
}

void openRound(Tree& tree, const std::vector<std::uint32_t>& round, int workers) {
	const int spare = round.size() < std::size_t(workers) ? workers - int(round.size()) : 0;
	tree.growth->idle.fetch_add(spare);
	inParallel(workers, round.size(), [&](std::size_t k) {
		openedCell(tree, round[k], true);
	});

	tree.growth->idle.fetch_sub(spare);
}

std::vector<std::uint32_t> nodesBelow(const Tree& tree, const std::vector<std::uint32_t>& round) {
	std::vector<std::uint32_t> below;
	for (const std::uint32_t node : round) {
		const std::uint32_t standing = standingNode(tree, node);
		const Span children          = standing == noNode ? spanOf(0, 0) : tree.growth->records[standing].owned.nodes;
		for (std::uint32_t child = children.first; child < children.first + children.count; ++child) {
			below.push_back(child);
		}
	}
	return below;
}

void openEveryCell(Tree& tree, int workers) {
	if (!tree.growth) {
		return;
	}
	for (std::vector<std::uint32_t> round = { tree.root.payload }; !round.empty(); round = nodesBelow(tree, round)) {
		openRound(tree, round, std::max(1, workers));
	}
}

}
