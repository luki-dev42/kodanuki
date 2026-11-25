#include "plugins/geometry/tree/sharing.h"
#include "plugins/geometry/tree/lazy.h"
#include "plugins/geometry/tree/store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kodanuki {

constexpr std::size_t leastSlots = 1024;

struct ListKey {
	const ProgramReference* references;
	std::uint32_t count;
};

struct CutKey {
	const ProgramCut& cut;
	const std::uint16_t* entries;
};

SharedIndex emptySharedIndex() {
	return SharedIndex {
		.slots  = std::vector<std::uint32_t>(leastSlots, 0),
		.hashes = {},
		.uses   = {},
		.taken  = 0,
	};
}

SharedPrograms emptySharedPrograms() {
	return SharedPrograms {
		.lists = emptySharedIndex(),
		.cuts  = emptySharedIndex(),
	};
}

std::uint64_t mixedValue(std::uint64_t hash, std::uint64_t value) {
	return (hash ^ value) * 0x100000001B3ull;
}

std::uint32_t foldedHash(std::uint64_t hash) {
	return std::uint32_t(hash ^ (hash >> 32));
}

std::uint32_t listHash(const ListKey& key) {
	std::uint64_t hash = 0xCBF29CE484222325ull;
	for (std::uint32_t k = 0; k < key.count; ++k) {
		hash = mixedValue(mixedValue(hash, key.references[k].index), std::uint64_t(key.references[k].source));
	}
	return foldedHash(hash);
}

std::uint32_t cutEntryCount(const ProgramCut& cut) {
	return 2 * std::uint32_t(cut.ranges) + 2 * std::uint32_t(cut.rerouted);
}

std::uint32_t cutHash(const CutKey& key) {
	std::uint64_t hash = mixedValue(mixedValue(0xCBF29CE484222325ull, key.cut.object), key.cut.result);
	hash               = mixedValue(hash, std::uint64_t(key.cut.ranges) | std::uint64_t(key.cut.rerouted) << 16);
	for (std::uint32_t k = 0; k < cutEntryCount(key.cut); ++k) {
		hash = mixedValue(hash, key.entries[k]);
	}
	return foldedHash(hash);
}

bool sameList(const Tree& tree, std::uint32_t held, const ListKey& key) {
	const ProgramList& list = tree.lists[held];
	if (list.count != key.count) {
		return false;
	}
	for (std::uint32_t k = 0; k < list.count; ++k) {
		const ProgramReference& reference = tree.references[list.first + k];
		if (reference.index != key.references[k].index || reference.source != key.references[k].source) {
			return false;
		}
	}
	return true;
}

bool sameCut(const Tree& tree, std::uint32_t held, const CutKey& key) {
	const ProgramCut& cut = tree.cuts[held];
	const bool alike      = cut.object == key.cut.object && cut.result == key.cut.result && cut.ranges == key.cut.ranges && cut.rerouted == key.cut.rerouted;
	return alike && std::equal(key.entries, key.entries + cutEntryCount(cut), tree.cutSteps.begin() + cut.first);
}

std::size_t slotMask(const SharedIndex& index) {
	return index.slots.size() - 1;
}

void insertedSlot(SharedIndex& index, std::uint32_t element) {
	std::size_t slot = index.hashes[element] & slotMask(index);
	while (index.slots[slot] != 0) {
		slot = (slot + 1) & slotMask(index);
	}
	index.slots[slot] = element + 1;
}

void grownIndex(SharedIndex& index) {
	if ((index.taken + 1) * 2 <= index.slots.size()) {
		return;
	}
	const std::vector<std::uint32_t> held = index.slots;
	index.slots.assign(held.size() * 2, 0);
	for (const std::uint32_t slot : held) {
		if (slot != 0) {
			insertedSlot(index, slot - 1);
		}
	}
}

void heldElement(SharedIndex& index, std::uint32_t element, std::uint32_t hash) {
	if (element >= index.hashes.size()) {
		index.hashes.resize(element + 1, 0);
		index.uses.resize(element + 1, 0);
	}
	index.hashes[element] = hash;
	index.uses[element]   = 0;
	grownIndex(index);
	insertedSlot(index, element);
	index.taken += 1;
}

std::size_t elementSlot(const SharedIndex& index, std::uint32_t element) {
	std::size_t slot = index.hashes[element] & slotMask(index);
	while (index.slots[slot] != element + 1) {
		slot = (slot + 1) & slotMask(index);
	}
	return slot;
}

bool staysBehind(const SharedIndex& index, std::size_t hole, std::size_t slot) {
	const std::size_t home = index.hashes[index.slots[slot] - 1] & slotMask(index);
	return ((slot - home) & slotMask(index)) < ((slot - hole) & slotMask(index));
}

void removedElement(SharedIndex& index, std::uint32_t element) {
	std::size_t hole = elementSlot(index, element);
	for (std::size_t slot = (hole + 1) & slotMask(index); index.slots[slot] != 0; slot = (slot + 1) & slotMask(index)) {
		if (!staysBehind(index, hole, slot)) {
			index.slots[hole] = index.slots[slot];
			hole              = slot;
		}
	}
	index.slots[hole] = 0;
	index.taken -= 1;
}

std::uint32_t foundList(const Tree& tree, const ListKey& key, std::uint32_t hash) {
	const SharedIndex& index = tree.growth->shared.lists;
	for (std::size_t slot = hash & slotMask(index); index.slots[slot] != 0; slot = (slot + 1) & slotMask(index)) {
		const std::uint32_t held = index.slots[slot] - 1;
		if (index.hashes[held] == hash && sameList(tree, held, key)) {
			return held;
		}
	}
	return noList;
}

std::uint32_t foundCut(const Tree& tree, const CutKey& key, std::uint32_t hash) {
	const SharedIndex& index = tree.growth->shared.cuts;
	for (std::size_t slot = hash & slotMask(index); index.slots[slot] != 0; slot = (slot + 1) & slotMask(index)) {
		const std::uint32_t held = index.slots[slot] - 1;
		if (index.hashes[held] == hash && sameCut(tree, held, key)) {
			return held;
		}
	}
	return noList;
}

std::uint32_t storedCut(Tree& tree, const CutKey& key, std::uint32_t hash) {
	TreeStores& stores = tree.growth->stores;
	const std::vector<std::uint16_t> entries(key.entries, key.entries + cutEntryCount(key.cut));
	ProgramCut cut                     = key.cut;
	cut.first                          = storedRow(tree.cutSteps, stores.cutSteps, entries, "cut entries").first;
	const std::vector<ProgramCut> cuts = { cut };
	const std::uint32_t stored         = storedRow(tree.cuts, stores.cuts, cuts, "program cuts").first;
	heldElement(tree.growth->shared.cuts, stored, hash);
	return stored;
}

std::uint32_t sharedCut(Tree& tree, const CutKey& key) {
	const std::uint32_t hash  = cutHash(key);
	const std::uint32_t found = foundCut(tree, key, hash);
	return found != noList ? found : storedCut(tree, key, hash);
}

std::vector<std::uint32_t> sharedCuts(Tree& tree, const Subtree& packed) {
	std::vector<std::uint32_t> cuts;
	for (const ProgramCut& cut : packed.cuts) {
		const CutKey key = {
			.cut     = cut,
			.entries = packed.cutSteps.data() + cut.first,
		};

		cuts.push_back(sharedCut(tree, key));
	}
	return cuts;
}

void droppedCut(Tree& tree, std::uint32_t cut) {
	TreeStores& stores    = tree.growth->stores;
	const ProgramCut held = tree.cuts[cut];
	removedElement(tree.growth->shared.cuts, cut);
	const Span entries = {
		.first = held.first,
		.count = cutEntryCount(held),
	};

	const Span one = {
		.first = cut,
		.count = 1,
	};

	freedRow(tree.cutSteps, stores.cutSteps, entries);
	freedRow(tree.cuts, stores.cuts, one);
}

void releasedCut(Tree& tree, std::uint32_t cut) {
	std::uint32_t& uses = tree.growth->shared.cuts.uses[cut];
	uses -= 1;
	if (uses == 0) {
		droppedCut(tree, cut);
	}
}

void unusedCutsDropped(Tree& tree, const std::vector<std::uint32_t>& cuts) {
	std::vector<std::uint32_t> named = cuts;
	std::sort(named.begin(), named.end());
	named.erase(std::unique(named.begin(), named.end()), named.end());
	for (const std::uint32_t cut : named) {
		if (tree.growth->shared.cuts.uses[cut] == 0) {
			droppedCut(tree, cut);
		}
	}
}

std::vector<std::uint32_t> leafUses(const Subtree& packed) {
	std::vector<std::uint32_t> uses(packed.lists.size(), 0);
	for (const SurfaceLeaf& leaf : packed.surfaces) {
		uses[leaf.programs] += 1;
	}
	for (const SolidLeaf& leaf : packed.solids) {
		uses[leaf.programs] += 1;
	}
	return uses;
}

std::vector<ProgramReference> placedReferences(const Subtree& packed, const ProgramList& list, const std::vector<std::uint32_t>& cuts) {
	std::vector<ProgramReference> references(packed.references.begin() + list.first, packed.references.begin() + list.first + list.count);
	for (ProgramReference& reference : references) {
		reference.index = reference.source == ProgramSource::Cut ? cuts[reference.index] : reference.index;
	}
	return references;
}

std::uint32_t storedList(Tree& tree, const std::vector<ProgramReference>& references, std::uint32_t hash) {
	TreeStores& stores     = tree.growth->stores;
	const ProgramList list = {
		.first = storedRow(tree.references, stores.references, references, "program references").first,
		.count = std::uint32_t(references.size()),
	};

	const std::vector<ProgramList> lists = { list };
	const std::uint32_t stored           = storedRow(tree.lists, stores.lists, lists, "program lists").first;
	heldElement(tree.growth->shared.lists, stored, hash);
	for (const ProgramReference& reference : references) {
		if (reference.source == ProgramSource::Cut) {
			tree.growth->shared.cuts.uses[reference.index] += 1;
		}
	}
	return stored;
}

std::uint32_t usedList(Tree& tree, const std::vector<ProgramReference>& references, std::uint32_t uses) {
	const ListKey key = {
		.references = references.data(),
		.count      = std::uint32_t(references.size()),
	};

	const std::uint32_t hash  = listHash(key);
	const std::uint32_t found = foundList(tree, key, hash);
	const std::uint32_t list  = found != noList ? found : storedList(tree, references, hash);
	tree.growth->shared.lists.uses[list] += uses;
	return list;
}

std::vector<std::uint32_t> sharedLists(Tree& tree, const Subtree& packed) {
	const std::vector<std::uint32_t> cuts = sharedCuts(tree, packed);
	const std::vector<std::uint32_t> uses = leafUses(packed);
	std::vector<std::uint32_t> lists(packed.lists.size(), noList);
	for (std::size_t k = 0; k < packed.lists.size(); ++k) {
		if (uses[k] > 0) {
			lists[k] = usedList(tree, placedReferences(packed, packed.lists[k], cuts), uses[k]);
		}
	}
	unusedCutsDropped(tree, cuts);
	return lists;
}

void releasedList(Tree& tree, std::uint32_t list) {
	TreeStores& stores  = tree.growth->stores;
	std::uint32_t& uses = tree.growth->shared.lists.uses[list];
	uses -= 1;
	if (uses > 0) {
		return;
	}
	const ProgramList held = tree.lists[list];
	removedElement(tree.growth->shared.lists, list);
	for (std::uint32_t k = 0; k < held.count; ++k) {
		const ProgramReference reference = tree.references[held.first + k];
		if (reference.source == ProgramSource::Cut) {
			releasedCut(tree, reference.index);
		}
	}
	const Span references = {
		.first = held.first,
		.count = held.count,
	};

	const Span one = {
		.first = list,
		.count = 1,
	};

	freedRow(tree.references, stores.references, references);
	freedRow(tree.lists, stores.lists, one);
}

std::size_t indexBytes(const SharedIndex& index) {
	return (index.slots.size() + index.hashes.size() + index.uses.size()) * sizeof(std::uint32_t);
}

std::size_t sharedBytes(const SharedPrograms& shared) {
	return indexBytes(shared.lists) + indexBytes(shared.cuts);
}

}
