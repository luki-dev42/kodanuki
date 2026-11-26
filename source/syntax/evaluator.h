#pragma once

#include "sdf/scene.h"
#include "syntax/parser.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace kodanuki {

struct Network;
struct Pattern;

/**
 * What kind of thing a value of the scene language is.
 */
enum class ValueKind : std::uint8_t {
	/**
	 * A number, known when the scene loads or held in a register of the
	 * function being compiled.
	 */
	Number,
	/**
	 * A list of values of one kind; a list of numbers is a vector.
	 */
	List,
	/**
	 * A record of named fields, each a value of any kind.
	 */
	Record,
	/**
	 * A material, as an index into the scene's materials.
	 */
	Material,
	/**
	 * A coat, as an index into the scene's coats.
	 */
	Coat,
	/**
	 * A call of a surface, to be emitted as a shape or read as a
	 * pattern.
	 */
	Pattern,
	/**
	 * A bare name such as an axis or a setting's word.
	 */
	Word,
	/**
	 * A network of lines with its spatial index, made once when the
	 * scene loads and shared read-only by every copy.
	 */
	Network,
};

/**
 * A value of the scene language while a file runs.
 */
struct Value {
	/**
	 * What kind of value it is; only the fields of that kind are read.
	 */
	ValueKind kind;
	/**
	 * The register of the function being compiled that holds the number,
	 * or -1 where the number is known.
	 */
	std::int32_t slot;
	/**
	 * The material, as an index into the scene's materials.
	 */
	std::uint32_t material;
	/**
	 * The coat, as an index into the scene's coats.
	 */
	std::uint32_t coat;
	/**
	 * The number, where it is known when the scene loads.
	 */
	double number;
	/**
	 * The elements of a list, or the field values of a record in the
	 * order of its field names.
	 */
	std::vector<Value> items;
	/**
	 * The field names of a record, sorted, one per item; shared by its
	 * copies, so a record copies as cheaply as a list.
	 */
	std::shared_ptr<const std::vector<std::string>> fields;
	/**
	 * The pattern.
	 */
	std::shared_ptr<const Pattern> pattern;
	/**
	 * The word, as written in the parsed statements, which outlive every
	 * value; null for every other kind.
	 */
	const std::string* word;
	/**
	 * The network, shared by every copy of the value and never changed.
	 */
	std::shared_ptr<const Network> network;
};

/**
 * A call of a surface kept to be compiled later: as a shape's leaf, a
 * ramp's pattern or a coat's mask.
 */
struct Pattern {
	/**
	 * The surface's definition.
	 */
	const Statement* function;
	/**
	 * The arguments it was called with.
	 */
	std::vector<Value> arguments;
	/**
	 * Where it was called, for errors.
	 */
	Location location;
	/**
	 * Whether it is read in world space rather than the painted shape's.
	 */
	bool world;
	/**
	 * The colours of a ramp over it, empty for a plain call.
	 */
	std::vector<Value> stops;
};

/**
 * The bindings of one block, by name.
 */
using Scope = std::unordered_map<std::string, Value>;

/**
 * A binding of a block below the top level: a parameter, a loop
 * variable or a `let`.
 */
struct Binding {
	/**
	 * The name, as written in the parsed statements, which outlive the
	 * binding.
	 */
	const std::string* name;
	/**
	 * The value.
	 */
	Value value;
};

/**
 * A name a surface's compile looked up outside the surface and the
 * functions it called: in the running tile's bindings, the top level or
 * the bindings shared by a tile's block, found or not.
 */
struct OuterRead {
	/**
	 * The name.
	 */
	std::string name;
	/**
	 * Whether it was bound.
	 */
	bool bound;
	/**
	 * Its value where it was bound.
	 */
	Value value;
};

/**
 * A program compiled for a surface and its arguments, with the names it
 * read outside them while it compiled; it serves the same call again
 * while those names hold the same values.
 */
struct Specialisation {
	/**
	 * The program, as an index into the scene's programs.
	 */
	std::uint32_t program;
	/**
	 * The names read outside the surface, each once.
	 */
	std::vector<OuterRead> reads;
};

/**
 * A surface being compiled into a program of steps.
 */
struct Compiler {
	/**
	 * The program so far.
	 */
	Program program;
	/**
	 * The step that holds each constant, so each is held once.
	 */
	std::map<double, std::uint32_t> constants;
	/**
	 * The point the surface reads, as a list of three registers.
	 */
	Value point;
	/**
	 * The surface normal a pattern reads, as a list of three registers.
	 */
	Value normal;
	/**
	 * The names the surface read outside itself so far, each once.
	 */
	std::vector<OuterRead> reads;
};

/**
 * What is around a statement that emits shapes: the placement, the
 * paint, the coats and where the shapes go.
 */
struct Context {
	/**
	 * The linear part of the placement from the shape's space to world
	 * space, by rows.
	 */
	double linear[3][3];
	/**
	 * Where the shape's origin lands in world space.
	 */
	double origin[3];
	/**
	 * What the shape's distance is multiplied by: the smallest scale,
	 * so the distance never grows past the true one.
	 */
	double stretch;
	/**
	 * The material, or -1 for the default grey.
	 */
	std::int32_t material;
	/**
	 * The coats, innermost first, as indices into the scene's coats.
	 */
	std::vector<std::uint32_t> coats;
	/**
	 * The rules the shapes say, Rule bits or'ed.
	 */
	std::uint8_t rules;
	/**
	 * The groups the shapes belong to, one bit per group.
	 */
	std::uint64_t groups;
	/**
	 * Where emitted nodes go: a join's children, or the scene's objects
	 * where it is null.
	 */
	std::vector<std::uint32_t>* sink;
};

/**
 * The files a scene was read from, with their statements, which
 * locations and definitions point into.
 */
struct ParsedFiles {
	/**
	 * The paths of the files read.
	 */
	std::list<std::string> files;
	/**
	 * The statements of every file read.
	 */
	std::list<std::vector<Statement>> trees;
	/**
	 * A key of every file's path and text, in the order they were read.
	 */
	std::uint64_t key;
};

/**
 * What the tiles of one `tiles` block run from: the block, and the
 * definitions and bindings of the file where it stands
 * (TileBlock::snapshot).
 */
struct TileSnapshot {
	/**
	 * The tiles statement.
	 */
	const Statement* block;
	/**
	 * The files the statements were read from, kept as long as a tile
	 * may run.
	 */
	std::shared_ptr<const ParsedFiles> parsed;
	/**
	 * The functions and shapes defined before the block, by name.
	 */
	std::unordered_map<std::string, const Statement*> definitions;
	/**
	 * The bindings of the top level where the block stands, `time` among
	 * them.
	 */
	Scope scope;
	/**
	 * The default grey material.
	 */
	std::int32_t defaultMaterial;
};

/**
 * One tile's shapes as its block made them.
 */
struct TileContent {
	/**
	 * The tile's own geometry: its programs, arguments, leaves, nodes
	 * and objects, each object compiled into one program as a load does,
	 * its leaves numbered from 0.
	 */
	Scene scene;
	/**
	 * The names of the top level the tile read, and a name for each step
	 * it read the eye at (eyeReadStep()).
	 */
	std::set<std::string> reads;
	/**
	 * The first error as `file:line:column: message`, empty where the
	 * tile ran; the scene then holds no object.
	 */
	std::string error;
};

/**
 * The state of a scene while its files run.
 */
struct Evaluator {
	/**
	 * The geometry being built.
	 */
	Scene& scene;
	/**
	 * The looks being built.
	 */
	Looks& looks;
	/**
	 * The lighting being built.
	 */
	Lighting& lighting;
	/**
	 * The shot being built.
	 */
	Shot& shot;
	/**
	 * The files read and their statements; null while a tile runs, which
	 * reads no file.
	 */
	std::shared_ptr<ParsedFiles> parsed;
	/**
	 * The files already imported, which are not read again.
	 */
	std::set<std::string> imported;
	/**
	 * The surfaces, shapes and functions, by name.
	 */
	std::unordered_map<std::string, const Statement*> definitions;
	/**
	 * The bindings of the file's top level, or of the running tile's own
	 * top level.
	 */
	Scope top;
	/**
	 * The bindings of the blocks below the top level, the innermost
	 * block's last; each name is bound once in its block.
	 */
	std::vector<Binding> locals;
	/**
	 * Where each block below the top level starts in the locals, the
	 * innermost last; empty at the top level.
	 */
	std::vector<std::size_t> blocks;
	/**
	 * The bindings a running tile reads below its own top level, shared
	 * with every tile of its block and never changed; null outside a tile.
	 */
	const Scope* shared;
	/**
	 * The bindings of the running tile, `tile`, `tile_origin` and
	 * `tile_step`, which its body and every definition it calls read
	 * below their own locals; null outside a tile.
	 */
	const Scope* tile;
	/**
	 * The first block below the top level the running definition sees,
	 * as an index into the blocks.
	 */
	std::size_t base;
	/**
	 * The surface being compiled, or null while shapes are built.
	 */
	Compiler* compiler;
	/**
	 * Whether the running definition is a function, which reads the top
	 * level but binds nothing there, and emits nothing.
	 */
	bool isolated;
	/**
	 * Whether the running surface or function has returned.
	 */
	bool returned;
	/**
	 * What it returned.
	 */
	Value result;
	/**
	 * Whether the running shape or tile body has met a bare `return;`,
	 * which skips the rest of it.
	 */
	bool ended;
	/**
	 * The programs compiled for each surface and distinct set of
	 * arguments, by a key of the surface and its arguments: numbers, and
	 * lists whose leaves are all numbers, count by their shape alone (they
	 * are passed when the shape runs), every other argument by its value.
	 * A key holds one program for each set of values of the names its
	 * compiles read outside the surface.
	 */
	std::map<std::string, std::vector<Specialisation>> specialised;
	/**
	 * The default grey material once it is made, or -1.
	 */
	std::int32_t defaultMaterial;
	/**
	 * Whether the file has read the name `time`.
	 */
	bool timed;
	/**
	 * Whether a tile is running: it may make no material, coat or group,
	 * say no rule and hold no setting or import.
	 */
	bool tiled;
	/**
	 * The names of the top level a running tile looked up, found or not,
	 * and a name for each step it read the eye at (eyeReadStep()).
	 */
	std::set<std::string> reads;
	/**
	 * The camera's eye that `eye(step)` snaps: the file's camera's once a
	 * camera block has run, the frame's camera's in a tile; none before.
	 */
	std::optional<math::Vector> eye;
	/**
	 * The `print` calls that have printed in this load or tile run.
	 */
	std::set<const Expression*> printed;
};

/**
 * One step of a built-in to fold when its operands are known, or to
 * compile otherwise.
 */
struct Operation {
	/**
	 * What the step does.
	 */
	Opcode opcode;
	/**
	 * The first operand.
	 */
	const Value& a;
	/**
	 * The second operand.
	 */
	const Value& b;
	/**
	 * The third operand.
	 */
	const Value& c;
	/**
	 * Where it was written, for errors.
	 */
	Location location;
};

/**
 * A stop of a list of stops, `[time, values...]`, read by `linear` and
 * `smooth`.
 */
struct TimedStop {
	/**
	 * Where the stop is.
	 */
	double time;
	/**
	 * What holds there.
	 */
	std::vector<double> values;
};

/**
 * A call of a surface, a shape or a function to run.
 */
struct Invocation {
	/**
	 * The definition.
	 */
	const Statement& definition;
	/**
	 * The arguments, in order, taken by the call: they are moved into its
	 * parameters.
	 */
	std::vector<Value>& arguments;
	/**
	 * The context its shapes are emitted in.
	 */
	const Context& context;
	/**
	 * Where it was called, for errors.
	 */
	Location location;
};

/**
 * A lattice of copies, as `repeat(count = ..., step = ...)` asks.
 */
struct RepeatCall {
	/**
	 * How many copies along each axis of the block's space, one or more
	 * each.
	 */
	int counts[3];
	/**
	 * How far one copy stands from the one before it along each axis of
	 * the block's space, in world space: the step along that axis turned
	 * and scaled by the placement around the block.
	 */
	double steps[3][3];
	/**
	 * Where the statement stands, for errors.
	 */
	Location location;
};

/**
 * A lattice to emit: the call, its block and the context around it.
 */
struct Repetition {
	/**
	 * The counts and the steps.
	 */
	const RepeatCall& call;
	/**
	 * The statements of the block.
	 */
	const std::vector<Statement>& block;
	/**
	 * The placement, paint and coats around the block, and where its
	 * shapes go.
	 */
	const Context& context;
};

/**
 * A scene file as loaded: the scene, or the error that stopped it.
 */
struct LoadedScene {
	/**
	 * The scene; empty where the load met an error.
	 */
	SceneFile file;
	/**
	 * The first error as `file:line:column: message`, empty where the
	 * scene loaded.
	 */
	std::string error;
};

/**
 * Run a scene file and everything it imports, and return its
 * geometry, looks, lighting and shot, or the first error.
 *
 * Every long hard union at the top of an object is then cut into
 * objects of its own (unionParts), and every object is compiled into
 * one program. The first error stops the load and comes back as
 * `file:line:column: message`, with an empty scene. The file starts
 * with one binding at its top level, `time`, which no `let` may
 * change; SceneFile.timed says whether the file read it, and is set
 * for a file with tiles, whose tiles may read it.
 *
 * A `tiles` block at the top level runs no tile: it keeps a snapshot of
 * the definitions and bindings where it stands
 * (Scene::tiles), from which evaluatedTile() runs each tile later. Its
 * body may not hold settings, imports, definitions or other tiles, nor
 * make materials or coats, say rules or put shapes in groups; the load
 * refuses what it can see of these in the body, and a running tile
 * refuses the rest.
 *
 * @param path The file.
 * @param time The number `time` is bound to: 0 for a still, a frame's
 *        time for a frame of a clip.
 */
LoadedScene loadScene(const std::string& path, double time);

/**
 * Run one tile of a scene's tiles blocks and return its shapes, or the
 * first error.
 *
 * The block's body runs from its snapshot (TileSnapshot) with one more
 * binding, `tile`, the list [i, j, k] of the tile's index, and every
 * shape it emits is moved by the index times the block's step. The tile
 * reads the snapshot's bindings without copying them, and a `let` of a
 * top-level name binds it in the tile's own top level, so tiles are run in
 * any order, on any thread, each alone. Its objects are cut and
 * compiled as a load does; its leaves name the snapshot's materials.
 * `eye(step)` in it snaps the eye given, and each step read is noted
 * among its reads (eyeReadStep()).
 *
 * @param scene The scene the block is of.
 * @param place The block and the tile.
 * @param eye The frame's camera's eye.
 */
TileContent evaluatedTile(const Scene& scene, const TilePlace& place, const math::Vector& eye);

/**
 * Return a camera's eye snapped to a grid of a step: the middle of the
 * grid's cell, step metres wide along each axis, that holds the eye, as
 * `eye(step)` gives it.
 *
 * @param eye The eye.
 * @param step The grid's step, above 0.
 */
math::Vector snappedEye(const math::Vector& eye, double step);

/**
 * Return the step of a read of the eye a tile noted (TileContent::reads),
 * or none for the name of a top-level binding.
 *
 * A tile notes `eye(step)` as `eye@` and the step written in full, a
 * name no binding can have.
 *
 * @param name The name read.
 */
std::optional<double> eyeReadStep(const std::string& name);

/**
 * Return the step of a read of `eye_band` a tile noted (TileContent::reads),
 * or none for any other name.
 *
 * A tile notes `eye_band(point, radii, step)` as `band@`, the step, the
 * point and the radii written in full, a name no binding can have.
 *
 * @param name The name read.
 */
std::optional<double> bandReadStep(const std::string& name);

/**
 * Return the band a noted read of `eye_band` (bandReadStep()) gives a tile
 * for an eye: how many of its radii the distance from its point to the
 * eye, snapped to its step, reaches.
 *
 * The point is noted in the tile's own space, so tiles that read the
 * band of the same place in them note the same name.
 *
 * @param name The name read.
 * @param eye The camera's eye.
 * @param corner The tile's corner in the world, its `tile_origin`.
 */
std::size_t bandOfRead(const std::string& name, const math::Vector& eye, const math::Vector& corner);

/**
 * Return where a tiles block stands in its file.
 *
 * @param block The block.
 */
const Location& tileBlockLocation(const TileBlock& block);

/**
 * Return a list of values.
 *
 * @param items The elements.
 */
Value listValue(std::vector<Value> items);

/**
 * Return a record of named fields, its fields sorted by name.
 *
 * The names are distinct; a record built by a built-in is made so.
 *
 * @param names The field names.
 * @param items The field values, one per name.
 */
Value recordValue(const std::vector<std::string>& names, const std::vector<Value>& items);

/**
 * Return a network as a value.
 *
 * @param network The network.
 */
Value networkValue(std::shared_ptr<const Network> network);

/**
 * Return the point of the Catmull-Rom curve through a list of stops,
 * as `smooth` reads it, a share of the way from one stop to the next:
 * the slope at a stop is taken from the stops either side, at an end
 * from the one stop beside it.
 *
 * @param stops The stops, their times rising.
 * @param at The stop before the point; another stop follows it.
 * @param share How far from that stop to the next, from 0 to 1.
 */
std::vector<double> hermiteValues(const std::vector<TimedStop>& stops, std::size_t at, double share);

/**
 * Return the word of a value, empty for a value that is not a word.
 *
 * @param value The value.
 */
const std::string& wordOf(const Value& value);

/**
 * Return a number known when the scene loads.
 *
 * @param number The number.
 */
Value numberValue(double number);

/**
 * Return the context of the top level of a file: no placement, the
 * default material, no coats, shapes going to the scene's objects.
 */
Context rootContext();

/**
 * Return a number held in a register of the function being compiled.
 *
 * @param slot The register.
 */
Value slotValue(std::uint32_t slot);

/**
 * Append a step to the program being compiled and return its register.
 *
 * @param compiler The function being compiled (rule 46).
 * @param step The step.
 */
std::uint32_t pushStep(Compiler& compiler, const Step& step);

/**
 * Return the register that holds a number in the function being
 * compiled, adding a constant step for a known number.
 *
 * @param evaluator The evaluator.
 * @param value The number.
 */
std::uint32_t slotOf(Evaluator& evaluator, const Value& value);

/**
 * Run a surface's or a function's body where it is called, inside the
 * surface being compiled or, for a function, at load time, and return
 * the value it returns. It fails where the body never returns.
 *
 * @param evaluator The evaluator.
 * @param call The definition, its arguments and where it was called.
 */
Value inlineFunction(Evaluator& evaluator, const Invocation& call);

/**
 * Whether every name a compile read outside its surface holds what it
 * held then: bound to the same value, or still unbound. In a tile, each
 * top-level name counts as read by the tile.
 *
 * @param evaluator The evaluator.
 * @param reads The names and what they held.
 */
bool readsHold(Evaluator& evaluator, const std::vector<OuterRead>& reads);

/**
 * Run a lattice's block once and emit its shapes once per copy.
 *
 * The block runs once, so everything it draws or changes happens once
 * and every copy is alike. Its nodes are then copied for each copy but
 * the first, x fastest, then y, then z: the same programs, arguments,
 * materials and coats, every leaf's placement moved by the copy's
 * steps. The copies stand side by side: inside a shape one hard merge,
 * at the top level separate objects.
 *
 * @param evaluator The evaluator.
 * @param repetition The call, the block and the context.
 */
void emitRepeated(Evaluator& evaluator, const Repetition& repetition);

}
