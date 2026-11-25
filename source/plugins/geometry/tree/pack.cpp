#include "plugins/geometry/tree/pack.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kodanuki {

struct Packed {
	std::vector<PackedNode> nodes;
	std::vector<SurfaceLeaf> surfaces;
	std::vector<SolidLeaf> solids;
};

struct Filling {
	PackedNode node;
	std::vector<std::uint32_t> deeper;
};

struct Adopting {
	const Subtree& octree;
	Packed& packed;
	Filling& filling;
};

void markSlot(PackedNode& node, int index, CellKind kind) {
	const std::uint64_t bit = std::uint64_t(1) << index;
	node.lowerKinds |= (int(kind) & 1) != 0 ? bit : 0;
	node.upperKinds |= (int(kind) & 2) != 0 ? bit : 0;
}

void adoptSlot(const Adopting& adopting, int index, std::uint32_t cell) {
	const CellEntry& entry = adopting.octree.cells[cell];
	markSlot(adopting.filling.node, index, entry.kind);
	switch (entry.kind) {
		case CellKind::Surface: adopting.packed.surfaces.push_back(adopting.octree.surfaces[entry.payload]); return;
		case CellKind::Filled: adopting.packed.solids.push_back(adopting.octree.solids[entry.payload]); return;
		case CellKind::Branch: adopting.filling.deeper.push_back(cell); return;
		case CellKind::Empty: return;
	}
}

void adoptChild(const Adopting& adopting, std::uint32_t child, int octant) {
	if (adopting.octree.cells[child].kind != CellKind::Branch) {
		adopting.filling.node.coarse |= std::uint8_t(1u << octant);
		adoptSlot(adopting, octant * 8, child);
		return;
	}
	for (int sub = 0; sub < 8; ++sub) {
		adoptSlot(adopting, octant * 8 + sub, adopting.octree.cells[child].payload + std::uint32_t(sub));
	}
}

std::uint8_t busyChildren(const PackedNode& node) {
	const std::uint64_t occupied = node.lowerKinds | node.upperKinds;
	std::uint8_t busy            = 0;
	for (int octant = 0; octant < 8; ++octant) {
		busy |= ((occupied >> (octant * 8)) & 0xFF) != 0 ? std::uint8_t(1u << octant) : 0;
	}
	return busy;
}

Filling blankFilling(const Packed& packed) {
	const PackedNode node = {
		.lowerKinds   = 0,
		.upperKinds   = 0,
		.firstBranch  = 0,
		.firstSurface = std::uint32_t(packed.surfaces.size()),
		.firstSolid   = std::uint32_t(packed.solids.size()),
		.coarse       = 0,
		.busy         = 0,
		.state        = std::uint8_t(NodeState::Fine),
		.stamp        = 0,
	};

	return Filling {
		.node   = node,
		.deeper = {},
	};
}

PackedNode packedNode(const Subtree& octree, Packed& packed, std::uint32_t cell) {
	Filling filling = blankFilling(packed);

	const Adopting adopting = {
		.octree  = octree,
		.packed  = packed,
		.filling = filling,
	};

	for (int octant = 0; octant < 8; ++octant) {
		adoptChild(adopting, octree.cells[cell].payload + std::uint32_t(octant), octant);
	}
	filling.node.busy        = busyChildren(filling.node);
	const std::size_t start  = packed.nodes.size();
	filling.node.firstBranch = std::uint32_t(start);
	packed.nodes.resize(start + filling.deeper.size());
	for (std::size_t k = 0; k < filling.deeper.size(); ++k) {
		const PackedNode row    = packedNode(octree, packed, filling.deeper[k]);
		packed.nodes[start + k] = row;
	}
	return filling.node;
}

RootRecord packedRoot(const Subtree& octree, Packed& packed) {
	RootRecord record = octree.root;
	record.kind       = octree.cells[0].kind;
	switch (record.kind) {
		case CellKind::Surface: record.surface = octree.surfaces[octree.cells[0].payload]; break;
		case CellKind::Filled: record.solid = octree.solids[octree.cells[0].payload]; break;
		case CellKind::Branch: record.node = packedNode(octree, packed, 0); break;
		case CellKind::Empty: break;
	}

	return record;
}

Subtree packedSubtree(const Subtree& subtree) {
	Packed packed = {
		.nodes    = {},
		.surfaces = {},
		.solids   = {},
	};

	const RootRecord root = packedRoot(subtree, packed);
	return Subtree {
		.box        = subtree.box,
		.depth      = subtree.depth,
		.cells      = {},
		.surfaces   = packed.surfaces,
		.solids     = packed.solids,
		.runs       = subtree.runs,
		.lists      = subtree.lists,
		.references = subtree.references,
		.cuts       = subtree.cuts,
		.cutSteps   = subtree.cutSteps,
		.nodes      = packed.nodes,
		.root       = root,
	};
}

}
