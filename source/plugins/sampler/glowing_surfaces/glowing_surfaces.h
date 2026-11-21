#pragma once

#include "core/state.h"
#include "math/random.h"
#include "math/vector.h"
#include "plugins/geometry/tree/tree.h"
#include "plugins/geometry/tree/walk.h"
#include "plugins/transport/paths/light/light.h"

#include <cstdint>
#include <vector>

namespace kodanuki {

/**
 * The cells a glowing surface passes through, aimed at as one light.
 *
 * The region is halved the same number of times along each axis, so
 * every cell is a box of one size, found from a point. Each cell keeps
 * its box, its guess of brightness and the running sum of the weights
 * it is drawn by. A scene where nothing glows has none.
 */
struct GlowCells {
	/**
	 * The region the grid divides.
	 */
	Region region;
	/**
	 * How many times the grid halves the region along each axis.
	 */
	int depth;
	/**
	 * Each cell's place, ascending: x + n (y + n z) with n cells an
	 * axis.
	 */
	std::vector<std::uint64_t> places;
	/**
	 * The middle of each cell's box.
	 */
	std::vector<math::Vector> middles;
	/**
	 * Half the extent of each cell's box along each axis.
	 */
	std::vector<math::Vector> halves;
	/**
	 * How bright each cell is guessed to glow, above zero: the brightest
	 * glow it may hold.
	 */
	std::vector<double> guesses;
	/**
	 * The running sum of the weights cells are drawn by, its guess, up to
	 * and including each cell.
	 */
	std::vector<double> running;
	/**
	 * The middle of each glow ball, one per glowing object, `--use
	 * glow_balls=`; none where they are off. Where there are any, a glow
	 * aim draws a ball by its guess times the solid angle of its cone, then
	 * a direction evenly inside the cone, and the cells are not drawn.
	 */
	std::vector<math::Vector> ballCentres;
	/**
	 * Each ball's radius: it reaches the farthest corner of the boxes its
	 * object's surface passes through.
	 */
	std::vector<double> ballRadii;
	/**
	 * Each ball's guess of brightness: its object's brightest glow.
	 */
	std::vector<double> ballGuesses;
};

/**
 * What a glance at a glow ball counts: whether the place lies in the
 * ball aimed at, the ball's guess and the density of the balls at the
 * place.
 */
struct GlowCount {
	/**
	 * Whether the place lies in what was aimed at.
	 */
	bool in;
	/**
	 * The guess of what was aimed at.
	 */
	double guess;
	/**
	 * glowDensity() at the place, zero where it lies elsewhere.
	 */
	double density;
};

/**
 * The numbers a direction toward the glow cells is drawn with.
 */
struct GlowDraw {
	/**
	 * The point aimed from.
	 */
	math::Vector from;
	/**
	 * The number that picks the cell.
	 */
	double part;
	/**
	 * The numbers that place the direction inside the cell's cone.
	 */
	math::UnitPair pair;
};

/**
 * A ray that met a glowing surface, whose aim density is asked.
 */
struct GlowSeen {
	/**
	 * The point the ray was drawn at.
	 */
	math::Vector from;
	/**
	 * The unit direction.
	 */
	math::Vector toward;
	/**
	 * Where it met the surface.
	 */
	math::Vector place;
};

/**
 * Return no cells: the glows are met only.
 */
GlowCells noGlowCells();

/**
 * Keep every grid cell a surface leaf lies in or covers whose programs
 * reach a glowing scene leaf, by its material or a coat, and whose field
 * at the middle comes within the cell's ball. A cell is guessed as bright
 * as the brightest glow the field there is made of.
 *
 * @param geometry The tree and the scene.
 * @param looks The scene's looks: its materials and coats.
 * @param depth The halvings, 6.
 */
GlowCells gatheredGlowGrid(const Geometry& geometry, const Looks& looks, int depth);

/**
 * Return the cells with a glow ball around each glowing object: the
 * tree's region halved ten times, or until a level would keep more than
 * 100000 boxes, keeping each box whose middle the object's field puts
 * within half the box's diagonal; the ball is round the middle of the
 * kept boxes' bounds and reaches their farthest corner.
 *
 * @param geometry The geometry: the tree and the scene's objects.
 * @param looks The scene's looks: its materials and coats.
 * @param cells The cells gatheredGlowGrid() found.
 */
GlowCells withGlowBalls(const Geometry& geometry, const Looks& looks, const GlowCells& cells);

/**
 * Draw a cell by its weight with the part, then a direction evenly
 * inside the cone of the ball holding it with the pair.
 *
 * Nothing is drawn from inside the ball, nor along a direction that
 * misses the cell's box. distance is where the direction leaves the
 * box, cell is the cell, and what arrives is the guess over the density.
 *
 * @param glows The cells, at least one.
 * @param draw The point and the numbers.
 */
LightAim glowAim(const GlowCells& glows, const GlowDraw& draw);

/**
 * Return the density glowAim() draws a direction with, for a ray that
 * met a glowing surface: that of the cell holding the place, zero where
 * none holds it.
 *
 * @param glows The cells.
 * @param seen The ray and where it met the surface.
 */
double glowDensity(const GlowCells& glows, const GlowSeen& seen);

/**
 * Return what a glance at a cell counts of the glow at a place, per
 * unit of glow: one over the cell's guess where the cell holds the place,
 * else zero.
 *
 * @param glows The cells.
 * @param cell The cell aimed at.
 * @param place Where the glance met a surface.
 */
double glowCounted(const GlowCells& glows, std::uint32_t cell, const math::Vector& place);

/**
 * Return what a glance at a cell or ball counts where there are glow
 * balls: whether the place lies in the cell or ball aimed at (a ball
 * holds the places it is the first ball to hold), its guess, and
 * glowDensity().
 *
 * @param glows The cells and balls.
 * @param cell The cell aimed at, or the cell count plus the ball.
 * @param seen The ray and where it met the surface.
 */
GlowCount glowMixedCount(const GlowCells& glows, std::uint32_t cell, const GlowSeen& seen);

/**
 * Return the glow cells the plugin keeps among the kept states, opened empty
 * where no preparation filled it yet: the plugin's one accessor.
 *
 * @param states The kept states.
 */
GlowCells& glowCellsState(States& states);

}
