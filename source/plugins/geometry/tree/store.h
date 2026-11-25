#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace kodanuki {

/**
 * The first element of no span: what takenSpan() gives where no free
 * span is long enough.
 */
constexpr std::uint32_t noSpan = 0xFFFFFFFFu;

/**
 * A run of consecutive elements of one of a tree's arrays.
 */
struct Span {
	/**
	 * The first element.
	 */
	std::uint32_t first;
	/**
	 * How many elements there are.
	 */
	std::uint32_t count;
};

/**
 * The free spans of one array a growing tree holds: what dropped nodes
 * gave back, which nodes placed later write into again.
 *
 * Free spans that touch are joined. A free span that reaches the
 * array's end is not kept: the array is shortened instead.
 */
struct SpanStore {
	/**
	 * Each free span's length by its first element.
	 */
	std::map<std::uint32_t, std::uint32_t> spans;
	/**
	 * Each free span as its length and first element, so that the
	 * shortest one long enough is found first.
	 */
	std::set<std::pair<std::uint32_t, std::uint32_t>> lengths;
	/**
	 * How many elements the free spans hold together.
	 */
	std::size_t free;
};

/**
 * One free store for each array of a growing tree. The nodes and their
 * records share one, since a node and its record stand at one index.
 */
struct TreeStores {
	/**
	 * Over Tree::nodes and Growth::records.
	 */
	SpanStore nodes;
	/**
	 * Over Tree::surfaces.
	 */
	SpanStore surfaces;
	/**
	 * Over Tree::solids.
	 */
	SpanStore solids;
	/**
	 * Over Tree::runs.
	 */
	SpanStore runs;
	/**
	 * Over Tree::lists.
	 */
	SpanStore lists;
	/**
	 * Over Tree::references.
	 */
	SpanStore references;
	/**
	 * Over Tree::cuts.
	 */
	SpanStore cuts;
	/**
	 * Over Tree::cutSteps.
	 */
	SpanStore cutSteps;
	/**
	 * Over Growth::objects.
	 */
	SpanStore objects;
};

/**
 * Return a store with no free span.
 */
SpanStore emptyStore();

/**
 * Return the stores of a tree that has given nothing back.
 */
TreeStores emptyStores();

/**
 * Take the shortest free span of at least count elements and return
 * its first element; what is left of it stays free. noSpan where none
 * is long enough.
 *
 * @param store The store.
 * @param count How many elements are asked for, at least one.
 */
std::uint32_t takenSpan(SpanStore& store, std::uint32_t count);

/**
 * Give a span back and return the free span it is now part of, joined
 * with the free spans it touches.
 *
 * @param store The store.
 * @param span The span, which no node names any more.
 */
Span givenSpan(SpanStore& store, const Span& span);

/**
 * Return where a row of count elements goes: a free span long enough,
 * else the array's end.
 *
 * @param store The array's store.
 * @param end How many elements the array holds.
 * @param count The row's length.
 */
std::uint32_t placedSpan(SpanStore& store, std::size_t end, std::size_t count);

/**
 * Write a row at an element of an array that is free or at its end,
 * and return its span. The array is reserved once and never moves; a
 * row that would pass its reserve ends the program, naming the array.
 *
 * @param held The array.
 * @param first Where the row goes (placedSpan()).
 * @param row The row.
 * @param name What the array holds, for the message.
 */
template <typename T>
Span writtenRow(std::vector<T>& held, std::uint32_t first, const std::vector<T>& row, const char* name);

/**
 * Write a row into the shortest free span long enough, else at the
 * array's end, and return its span.
 *
 * @param held The array.
 * @param store The array's store.
 * @param row The row.
 * @param name What the array holds, for the message.
 */
template <typename T>
Span storedRow(std::vector<T>& held, SpanStore& store, const std::vector<T>& row, const char* name);

/**
 * Give an array's span back. The whole memory pages of the free span it
 * joins that it reaches are handed back to the system, and a free span
 * at the array's end shortens the array.
 *
 * @param held The array.
 * @param store The array's store.
 * @param span The span, which no node names any more.
 */
template <typename T>
void freedRow(std::vector<T>& held, SpanStore& store, const Span& span);

/**
 * Hand back the memory of an array that a span given back freed: the
 * whole pages of the joined free span that the given span reaches, and
 * the array's end where the joined span reaches it. The store keeps the
 * span; droppedEnd() forgets it once the array is shortened.
 *
 * @param held The array.
 * @param joined The free span the given one joined (givenSpan()).
 * @param given The span given back.
 */
template <typename T>
void releasedRow(std::vector<T>& held, const Span& joined, const Span& given);

/**
 * Forget a free span that an array was shortened by, if the array now
 * ends where it begins.
 *
 * @param store The array's store.
 * @param joined The free span.
 * @param end How many elements the array holds.
 */
void droppedEnd(SpanStore& store, const Span& joined, std::size_t end);

/**
 * End the program because an array of the tree is full.
 *
 * @param name What the array holds.
 */
[[noreturn]] void refusedGrowth(const char* name);

}
