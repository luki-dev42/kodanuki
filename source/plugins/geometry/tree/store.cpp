#include "plugins/geometry/tree/store.h"
#include "plugins/geometry/tree/lazy.h"
#include "plugins/geometry/tree/tree.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(__unix__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace kodanuki {

struct ByteRange {
	std::uintptr_t lower;
	std::uintptr_t upper;
};

SpanStore emptyStore() {
	return SpanStore {
		.spans   = {},
		.lengths = {},
		.free    = 0,
	};
}

TreeStores emptyStores() {
	return TreeStores {
		.nodes      = emptyStore(),
		.surfaces   = emptyStore(),
		.solids     = emptyStore(),
		.runs       = emptyStore(),
		.lists      = emptyStore(),
		.references = emptyStore(),
		.cuts       = emptyStore(),
		.cutSteps   = emptyStore(),
		.objects    = emptyStore(),
	};
}

void keptSpan(SpanStore& store, const Span& span) {
	store.spans[span.first] = span.count;
	store.lengths.insert({ span.count, span.first });
}

void forgottenSpan(SpanStore& store, const Span& span) {
	store.spans.erase(span.first);
	store.lengths.erase({ span.count, span.first });
}

std::uint32_t takenSpan(SpanStore& store, std::uint32_t count) {
	const auto found = store.lengths.lower_bound({ count, 0 });
	if (found == store.lengths.end()) {
		return noSpan;
	}
	const Span span = {
		.first = found->second,
		.count = found->first,
	};

	forgottenSpan(store, span);
	if (span.count > count) {
		const Span rest = {
			.first = span.first + count,
			.count = span.count - count,
		};

		keptSpan(store, rest);
	}
	store.free -= count;
	return span.first;
}

Span joinedAfter(SpanStore& store, const Span& span) {
	const auto after = store.spans.find(span.first + span.count);
	if (after == store.spans.end()) {
		return span;
	}
	const Span next = {
		.first = after->first,
		.count = after->second,
	};

	forgottenSpan(store, next);
	return Span {
		.first = span.first,
		.count = span.count + next.count,
	};
}

Span joinedBefore(SpanStore& store, const Span& span) {
	const auto after = store.spans.lower_bound(span.first);
	if (after == store.spans.begin()) {
		return span;
	}
	const auto before = std::prev(after);
	const Span last   = {
		.first = before->first,
		.count = before->second,
	};

	if (last.first + last.count != span.first) {
		return span;
	}
	forgottenSpan(store, last);
	return Span {
		.first = last.first,
		.count = last.count + span.count,
	};
}

Span givenSpan(SpanStore& store, const Span& span) {
	const Span joined = joinedBefore(store, joinedAfter(store, span));
	keptSpan(store, joined);
	store.free += span.count;
	return joined;
}

std::uint32_t placedSpan(SpanStore& store, std::size_t end, std::size_t count) {
	const std::uint32_t taken = count == 0 ? noSpan : takenSpan(store, std::uint32_t(count));
	return taken == noSpan ? std::uint32_t(end) : taken;
}

void droppedEnd(SpanStore& store, const Span& joined, std::size_t end) {
	if (end != joined.first || store.spans.count(joined.first) == 0) {
		return;
	}
	forgottenSpan(store, joined);
	store.free -= joined.count;
}

[[noreturn]] void refusedGrowth(const char* name) {
	std::fprintf(stderr, "geometry/tree: the tree's %s are full; it holds no more than a quarter of the machine's memory\n", name);
	std::exit(1);
}

std::uintptr_t pageSize() {
#if defined(__unix__)
	return std::uintptr_t(sysconf(_SC_PAGE_SIZE));
#else
	return 0;
#endif
}

ByteRange spanBytes(const void* data, std::size_t size, const Span& span) {
	const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(data);
	return ByteRange {
		.lower = base + span.first * size,
		.upper = base + (std::size_t(span.first) + span.count) * size,
	};
}

ByteRange releasedBytes(const ByteRange& joined, const ByteRange& given, std::uintptr_t page) {
	const std::uintptr_t lower = std::max((joined.lower + page - 1) / page * page, given.lower / page * page);
	const std::uintptr_t upper = std::min(joined.upper / page * page, (given.upper + page - 1) / page * page);
	return ByteRange {
		.lower = lower,
		.upper = std::max(lower, upper),
	};
}

void releasedPages(const void* data, std::size_t size, const Span& joined, const Span& given) {
	const std::uintptr_t page = pageSize();
	if (page == 0) {
		return;
	}
	const ByteRange released = releasedBytes(spanBytes(data, size, joined), spanBytes(data, size, given), page);
#if defined(__unix__)
	if (released.upper > released.lower) {
		madvise(reinterpret_cast<void*>(released.lower), released.upper - released.lower, MADV_DONTNEED);
	}
#endif
}

template <typename T>
Span writtenRow(std::vector<T>& held, std::uint32_t first, const std::vector<T>& row, const char* name) {
	const Span span = {
		.first = first,
		.count = std::uint32_t(row.size()),
	};

	if (std::size_t(first) + row.size() <= held.size()) {
		std::copy(row.begin(), row.end(), held.begin() + first);
		return span;
	}
	if (held.capacity() - held.size() < row.size()) {
		refusedGrowth(name);
	}
	held.insert(held.end(), row.begin(), row.end());
	return span;
}

template <typename T>
Span storedRow(std::vector<T>& held, SpanStore& store, const std::vector<T>& row, const char* name) {
	return writtenRow(held, placedSpan(store, held.size(), row.size()), row, name);
}

template <typename T>
void releasedRow(std::vector<T>& held, const Span& joined, const Span& given) {
	releasedPages(held.data(), sizeof(T), joined, given);
	if (std::size_t(joined.first) + joined.count == held.size()) {
		held.resize(joined.first);
	}
}

template <typename T>
void freedRow(std::vector<T>& held, SpanStore& store, const Span& span) {
	if (span.count == 0) {
		return;
	}
	const Span joined = givenSpan(store, span);
	releasedRow(held, joined, span);
	droppedEnd(store, joined, held.size());
}

template Span writtenRow(std::vector<PackedNode>&, std::uint32_t, const std::vector<PackedNode>&, const char*);
template Span writtenRow(std::vector<NodeRecord>&, std::uint32_t, const std::vector<NodeRecord>&, const char*);
template Span storedRow(std::vector<SurfaceLeaf>&, SpanStore&, const std::vector<SurfaceLeaf>&, const char*);
template Span storedRow(std::vector<SolidLeaf>&, SpanStore&, const std::vector<SolidLeaf>&, const char*);
template Span storedRow(std::vector<std::uint32_t>&, SpanStore&, const std::vector<std::uint32_t>&, const char*);
template Span storedRow(std::vector<ProgramList>&, SpanStore&, const std::vector<ProgramList>&, const char*);
template Span storedRow(std::vector<ProgramReference>&, SpanStore&, const std::vector<ProgramReference>&, const char*);
template Span storedRow(std::vector<ProgramCut>&, SpanStore&, const std::vector<ProgramCut>&, const char*);
template Span storedRow(std::vector<std::uint16_t>&, SpanStore&, const std::vector<std::uint16_t>&, const char*);
template void releasedRow(std::vector<PackedNode>&, const Span&, const Span&);
template void releasedRow(std::vector<NodeRecord>&, const Span&, const Span&);
template void freedRow(std::vector<SurfaceLeaf>&, SpanStore&, const Span&);
template void freedRow(std::vector<SolidLeaf>&, SpanStore&, const Span&);
template void freedRow(std::vector<std::uint32_t>&, SpanStore&, const Span&);
template void freedRow(std::vector<ProgramList>&, SpanStore&, const Span&);
template void freedRow(std::vector<ProgramReference>&, SpanStore&, const Span&);
template void freedRow(std::vector<ProgramCut>&, SpanStore&, const Span&);
template void freedRow(std::vector<std::uint16_t>&, SpanStore&, const Span&);
template Span storedRow(std::vector<TileLeaf>&, SpanStore&, const std::vector<TileLeaf>&, const char*);
template void freedRow(std::vector<TileLeaf>&, SpanStore&, const Span&);

}
