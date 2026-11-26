#include "syntax/evaluator.h"

#include "math/random.h"
#include "sdf/compare.h"
#include "sdf/numbers.h"
#include "syntax/compile.h"
#include "syntax/network.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kodanuki {

Value numberValue(double number) {
	return Value {
		.kind     = ValueKind::Number,
		.slot     = -1,
		.material = 0,
		.coat     = 0,
		.number   = number,
		.items    = {},
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = nullptr,
	};
}

const Value& zeroValue() {
	static const Value zero = numberValue(0);
	return zero;
}

Value listValue(std::vector<Value> items) {
	return Value {
		.kind     = ValueKind::List,
		.slot     = -1,
		.material = 0,
		.coat     = 0,
		.number   = 0,
		.items    = std::move(items),
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = nullptr,
	};
}

std::vector<std::size_t> sortedOrder(const std::vector<std::string>& names) {
	std::vector<std::size_t> order(names.size());
	std::iota(order.begin(), order.end(), 0);
	std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
		return names[a] < names[b];
	});
	return order;
}

Value recordValue(const std::vector<std::string>& names, const std::vector<Value>& items) {
	std::vector<std::string> fields;
	std::vector<Value> values;
	fields.reserve(names.size());
	values.reserve(names.size());
	for (const std::size_t i : sortedOrder(names)) {
		fields.push_back(names[i]);
		values.push_back(items[i]);
	}
	return Value {
		.kind     = ValueKind::Record,
		.slot     = -1,
		.material = 0,
		.coat     = 0,
		.number   = 0,
		.items    = std::move(values),
		.fields   = std::make_shared<const std::vector<std::string>>(std::move(fields)),
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = nullptr,
	};
}

Value slotValue(std::uint32_t slot) {
	return Value {
		.kind     = ValueKind::Number,
		.slot     = std::int32_t(slot),
		.material = 0,
		.coat     = 0,
		.number   = 0,
		.items    = {},
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = nullptr,
	};
}

Value networkValue(std::shared_ptr<const Network> network) {
	return Value {
		.kind     = ValueKind::Network,
		.slot     = -1,
		.material = 0,
		.coat     = 0,
		.number   = 0,
		.items    = {},
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = std::move(network),
	};
}

Value patternValue(const Pattern& pattern) {
	return Value {
		.kind     = ValueKind::Pattern,
		.slot     = -1,
		.material = 0,
		.coat     = 0,
		.number   = 0,
		.items    = {},
		.fields   = nullptr,
		.pattern  = std::make_shared<const Pattern>(pattern),
		.word     = nullptr,
		.network  = nullptr,
	};
}

const std::string& wordOf(const Value& value) {
	static const std::string none;
	return value.word ? *value.word : none;
}

bool isKnown(const Value& value) {
	return value.kind == ValueKind::Number && value.slot < 0;
}

bool sameFields(const Value& a, const Value& b) {
	return a.fields == b.fields || (a.fields && b.fields && *a.fields == *b.fields);
}

bool sameValue(const Value& a, const Value& b);

bool sameItems(const Value& a, const Value& b) {
	if (a.items.size() != b.items.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.items.size(); ++i) {
		if (!sameValue(a.items[i], b.items[i])) {
			return false;
		}
	}
	return true;
}

bool sameValue(const Value& a, const Value& b) {
	const bool sameScalar = a.kind == b.kind && a.slot == b.slot && a.number == b.number;
	const bool sameObject = a.material == b.material && a.coat == b.coat && a.pattern == b.pattern && wordOf(a) == wordOf(b) && a.network == b.network && sameFields(a, b);
	return sameScalar && sameObject && sameItems(a, b);
}

const char* kindName(ValueKind kind) {
	switch (kind) {
		case ValueKind::Number: return "a number";
		case ValueKind::List: return "a list";
		case ValueKind::Record: return "a record";
		case ValueKind::Material: return "a material";
		case ValueKind::Coat: return "a coat";
		case ValueKind::Pattern: return "a pattern";
		case ValueKind::Word: return "a word";
		case ValueKind::Network: return "a network";
	}

	return "?";
}

struct Numbers {
	double a;
	double b;
	double c;
};

double fold(Opcode opcode, const Numbers& n) {
	switch (opcode) {
		case Opcode::Add: return n.a + n.b;
		case Opcode::Subtract: return n.a - n.b;
		case Opcode::Multiply: return n.a * n.b;
		case Opcode::Divide: return n.a / n.b;
		case Opcode::Negate: return -n.a;
		case Opcode::SquareRoot: return math::sqrt(n.a);
		case Opcode::Absolute: return math::abs(n.a);
		case Opcode::Floor: return math::floor(n.a);
		case Opcode::Sine: return math::sin(n.a);
		case Opcode::Cosine: return math::cos(n.a);
		case Opcode::Sign: return math::sign(n.a);
		case Opcode::ArcTangent: return math::atan2(n.a, n.b);
		case Opcode::Power: return math::pow(n.a, n.b);
		case Opcode::Minimum: return math::min(n.a, n.b);
		case Opcode::Maximum: return math::max(n.a, n.b);
		case Opcode::Less: return math::compare(math::Compare::Less, n.a, n.b);
		case Opcode::LessEqual: return math::compare(math::Compare::LessEqual, n.a, n.b);
		case Opcode::Equal: return math::compare(math::Compare::Equal, n.a, n.b);
		case Opcode::NotEqual: return math::compare(math::Compare::NotEqual, n.a, n.b);
		case Opcode::And: return math::compare(math::Compare::And, n.a, n.b);
		case Opcode::Or: return math::compare(math::Compare::Or, n.a, n.b);
		case Opcode::Not: return math::compare(math::Compare::Equal, n.a, 0);
		case Opcode::Select: return math::choose(n.a, n.b, n.c);
		default: return 0;
	}
}

std::uint32_t pushStep(Compiler& compiler, const Step& step) {
	compiler.program.steps.push_back(step);
	return std::uint32_t(compiler.program.steps.size() - 1);
}

std::uint32_t constantSlot(Compiler& compiler, double number) {
	const auto found = compiler.constants.find(number);
	if (found != compiler.constants.end()) {
		return found->second;
	}

	const Step constant = {
		.opcode = Opcode::Constant,
		.a      = 0,
		.b      = 0,
		.c      = 0,
		.value  = number,
		.leaf   = noLeaf,
	};

	const std::uint32_t slot = pushStep(compiler, constant);

	compiler.constants[number] = slot;
	return slot;
}

std::uint32_t slotOf(Evaluator& evaluator, const Value& value) {
	if (value.slot >= 0) {
		return std::uint32_t(value.slot);
	}
	return constantSlot(*evaluator.compiler, value.number);
}

void refusedInTiles(const Evaluator& evaluator, const Location& location, const std::string& what) {
	if (evaluator.tiled) {
		fail(location, "tiles may not " + what);
	}
}

void expectNumber(const Value& value, const Location& location) {
	if (value.kind == ValueKind::Word) {
		fail(location, "unknown name '" + wordOf(value) + "'");
	}
	if (value.kind != ValueKind::Number) {
		fail(location, std::string("expected a number, found ") + kindName(value.kind));
	}
}

Value emitOperation(Evaluator& evaluator, const Operation& operation) {
	const std::uint32_t a = slotOf(evaluator, operation.a);
	const std::uint32_t b = slotOf(evaluator, operation.b);
	const std::uint32_t c = slotOf(evaluator, operation.c);

	const Step step = {
		.opcode = operation.opcode,
		.a      = std::uint16_t(a),
		.b      = std::uint16_t(b),
		.c      = std::uint16_t(c),
		.value  = 0,
		.leaf   = noLeaf,
	};

	return slotValue(pushStep(*evaluator.compiler, step));
}

Value applyScalar(Evaluator& evaluator, const Operation& operation) {
	if (isKnown(operation.a) && isKnown(operation.b) && isKnown(operation.c)) {
		const Numbers operands = {
			.a = operation.a.number,
			.b = operation.b.number,
			.c = operation.c.number,
		};

		return numberValue(fold(operation.opcode, operands));
	}
	expectNumber(operation.a, operation.location);
	expectNumber(operation.b, operation.location);
	expectNumber(operation.c, operation.location);
	if (failed()) {
		return numberValue(0);
	}
	return emitOperation(evaluator, operation);
}

bool hasList(const Operation& operation) {
	return operation.a.kind == ValueKind::List || operation.b.kind == ValueKind::List || operation.c.kind == ValueKind::List;
}

std::size_t broadcastSize(const Operation& operation) {
	std::size_t size = 0;
	for (const Value* value : { &operation.a, &operation.b, &operation.c }) {
		if (value->kind != ValueKind::List) {
			continue;
		}
		if (size != 0 && size != value->items.size()) {
			fail(operation.location, "lists of different lengths");
			return 0;
		}
		size = value->items.size();
	}
	return size;
}

const Value& itemOrSelf(const Value& value, std::size_t index) {
	return value.kind == ValueKind::List ? value.items[index] : value;
}

Operation itemOperation(const Operation& operation, std::size_t i) {
	return Operation {
		.opcode   = operation.opcode,
		.a        = itemOrSelf(operation.a, i),
		.b        = itemOrSelf(operation.b, i),
		.c        = itemOrSelf(operation.c, i),
		.location = operation.location,
	};
}

Value apply(Evaluator& evaluator, const Operation& operation) {
	if (!hasList(operation)) {
		return applyScalar(evaluator, operation);
	}
	std::vector<Value> items;
	const std::size_t size = broadcastSize(operation);
	items.reserve(size);
	for (std::size_t i = 0; i < size; ++i) {
		items.push_back(apply(evaluator, itemOperation(operation, i)));
	}
	return listValue(std::move(items));
}

Value foldList(Evaluator& evaluator, const Operation& operation) {
	if (operation.a.kind != ValueKind::List || operation.a.items.empty()) {
		return operation.a;
	}
	Value total = operation.a.items[0];
	for (std::size_t i = 1; i < operation.a.items.size(); ++i) {
		const Operation combined = {
			.opcode   = operation.opcode,
			.a        = total,
			.b        = operation.a.items[i],
			.c        = zeroValue(),
			.location = operation.location,
		};

		total = apply(evaluator, combined);
	}
	return total;
}

Value dotValue(Evaluator& evaluator, const Operation& operation) {
	const Operation product = {
		.opcode   = Opcode::Multiply,
		.a        = operation.a,
		.b        = operation.b,
		.c        = zeroValue(),
		.location = operation.location,
	};

	const Value products = apply(evaluator, product);

	const Operation sum = {
		.opcode   = Opcode::Add,
		.a        = products,
		.b        = zeroValue(),
		.c        = zeroValue(),
		.location = operation.location,
	};

	return foldList(evaluator, sum);
}

Value lengthValue(Evaluator& evaluator, const Value& a, const Location& location) {
	if (a.kind != ValueKind::List) {
		const Operation magnitude = {
			.opcode   = Opcode::Absolute,
			.a        = a,
			.b        = zeroValue(),
			.c        = zeroValue(),
			.location = location,
		};

		return apply(evaluator, magnitude);
	}

	const Operation operands = {
		.opcode   = Opcode::Multiply,
		.a        = a,
		.b        = a,
		.c        = zeroValue(),
		.location = location,
	};

	const Value squared = dotValue(evaluator, operands);

	const Operation root = {
		.opcode   = Opcode::SquareRoot,
		.a        = squared,
		.b        = zeroValue(),
		.c        = zeroValue(),
		.location = location,
	};

	return apply(evaluator, root);
}

Value listItem(const Value& list, std::size_t index, const Location& location) {
	if (list.kind != ValueKind::List || index >= list.items.size()) {
		fail(location, "index " + std::to_string(index) + " is outside the list");
		return numberValue(0);
	}
	return list.items[index];
}

Value crossItem(Evaluator& evaluator, const Operation& operation, std::size_t i) {
	const std::size_t j = (i + 1) % 3;
	const std::size_t k = (i + 2) % 3;

	const Operation product = {
		.opcode   = Opcode::Multiply,
		.a        = listItem(operation.a, j, operation.location),
		.b        = listItem(operation.b, k, operation.location),
		.c        = zeroValue(),
		.location = operation.location,
	};

	const Value left = apply(evaluator, product);

	const Operation product2 = {
		.opcode   = Opcode::Multiply,
		.a        = listItem(operation.a, k, operation.location),
		.b        = listItem(operation.b, j, operation.location),
		.c        = zeroValue(),
		.location = operation.location,
	};

	const Value right = apply(evaluator, product2);

	const Operation difference = {
		.opcode   = Opcode::Subtract,
		.a        = left,
		.b        = right,
		.c        = zeroValue(),
		.location = operation.location,
	};

	return apply(evaluator, difference);
}

Value crossValue(Evaluator& evaluator, const Operation& operation) {
	return listValue({ crossItem(evaluator, operation, 0), crossItem(evaluator, operation, 1), crossItem(evaluator, operation, 2) });
}

std::size_t constantIndex(const Value& value, const Location& location) {
	if (!isKnown(value)) {
		fail(location, "an index must be known when the scene loads");
		return 0;
	}
	if (value.number < 0 || value.number != std::floor(value.number)) {
		fail(location, "an index is a whole number");
		return 0;
	}
	return std::size_t(value.number);
}

Value swizzle(const Value& value, const std::string& members, const Location& location) {
	std::vector<Value> items;
	items.reserve(members.size());
	for (const char member : members) {
		const std::size_t index = std::string_view("xyzw").find(member);
		if (index == std::string::npos) {
			fail(location, "unknown member '" + members + "'");
			return numberValue(0);
		}
		items.push_back(listItem(value, index, location));
	}
	return items.size() == 1 ? std::move(items[0]) : listValue(std::move(items));
}

Value fieldOf(const Value& record, const std::string& name, const Location& location) {
	const std::vector<std::string>& fields = *record.fields;
	const auto found                       = std::find(fields.begin(), fields.end(), name);
	if (found == fields.end()) {
		fail(location, "the record has no field '" + name + "'");
		return numberValue(0);
	}
	return record.items[std::size_t(found - fields.begin())];
}

double srgbToLinear(double c) {
	return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

Value colourValue(const std::string& hex) {
	std::vector<Value> items;
	for (std::size_t i = 0; i < 3; ++i) {
		const double channel = double(std::stoi(hex.substr(2 * i, 2), nullptr, 16)) / 255.0;
		items.push_back(numberValue(srgbToLinear(channel)));
	}
	return listValue(std::move(items));
}

Value* findInScope(Scope& scope, const std::string& name) {
	const auto found = scope.find(name);
	return found == scope.end() ? nullptr : &found->second;
}

std::size_t visibleStart(const Evaluator& evaluator) {
	return evaluator.base < evaluator.blocks.size() ? evaluator.blocks[evaluator.base] : evaluator.locals.size();
}

bool namedAs(const Binding& binding, const std::string& name) {
	return binding.name == &name || *binding.name == name;
}

Value* findLocal(Evaluator& evaluator, const std::string& name) {
	const std::size_t start = visibleStart(evaluator);
	for (std::size_t i = evaluator.locals.size(); i > start; --i) {
		if (namedAs(evaluator.locals[i - 1], name)) {
			return &evaluator.locals[i - 1].value;
		}
	}
	return nullptr;
}

void openBlock(Evaluator& evaluator) {
	evaluator.blocks.push_back(evaluator.locals.size());
}

void closeBlock(Evaluator& evaluator) {
	evaluator.locals.erase(evaluator.locals.begin() + std::ptrdiff_t(evaluator.blocks.back()), evaluator.locals.end());
	evaluator.blocks.pop_back();
}

void bindLocal(Evaluator& evaluator, const std::string& name, Value value) {
	evaluator.locals.emplace_back(&name, std::move(value));
}

Value* findTop(Evaluator& evaluator, const std::string& name) {
	if (evaluator.tiled) {
		evaluator.reads.insert(name);
	}
	return findInScope(evaluator.top, name);
}

Value* findName(Evaluator& evaluator, const std::string& name) {
	if (Value* found = findLocal(evaluator, name)) {
		return found;
	}
	return findTop(evaluator, name);
}

const Value* readInScope(const Scope* scope, const std::string& name) {
	if (!scope) {
		return nullptr;
	}
	const auto found = scope->find(name);
	return found == scope->end() ? nullptr : &found->second;
}

const Value* sharedName(const Evaluator& evaluator, const std::string& name) {
	return readInScope(evaluator.shared, name);
}

const Value* outerName(Evaluator& evaluator, const std::string& name) {
	if (const Value* found = readInScope(evaluator.tile, name)) {
		return found;
	}
	if (const Value* found = findTop(evaluator, name)) {
		return found;
	}
	return sharedName(evaluator, name);
}

bool readNoted(const Compiler& compiler, const std::string& name) {
	return std::any_of(compiler.reads.begin(), compiler.reads.end(), [&](const OuterRead& read) {
		return read.name == name;
	});
}

void noteRead(Evaluator& evaluator, const std::string& name, const Value* value) {
	if (!evaluator.compiler || readNoted(*evaluator.compiler, name)) {
		return;
	}

	const OuterRead read = {
		.name  = name,
		.bound = value != nullptr,
		.value = value ? *value : numberValue(0),
	};

	evaluator.compiler->reads.push_back(read);
}

bool readHolds(Evaluator& evaluator, const OuterRead& read) {
	const Value* value = outerName(evaluator, read.name);
	return value ? read.bound && sameValue(*value, read.value) : !read.bound;
}

bool readsHold(Evaluator& evaluator, const std::vector<OuterRead>& reads) {
	return std::all_of(reads.begin(), reads.end(), [&](const OuterRead& read) {
		return readHolds(evaluator, read);
	});
}

bool tileBinding(const std::string& name) {
	return name == "tile" || name == "tile_origin" || name == "tile_step";
}

std::optional<Value> builtinName(const Evaluator& evaluator, const std::string& name) {
	if (name == "pi") {
		return numberValue(3.14159265358979323846);
	}
	if (evaluator.compiler && name == "p") {
		return evaluator.compiler->point;
	}
	if (evaluator.compiler && name == "n") {
		return evaluator.compiler->normal;
	}
	return std::nullopt;
}

const Value* namedPlace(Evaluator& evaluator, const std::string& name) {
	evaluator.timed = evaluator.timed || name == "time";
	if (const Value* found = findLocal(evaluator, name)) {
		return found;
	}
	const Value* outer = outerName(evaluator, name);
	noteRead(evaluator, name, outer);
	return outer;
}

Value unboundName(Evaluator& evaluator, const Expression& expression) {
	const std::string& name = expression.text;
	if (std::optional<Value> builtin = builtinName(evaluator, name)) {
		return *builtin;
	}
	if (tileBinding(name)) {
		fail(expression.location, name + " is known only inside tiles");
	}
	return Value {
		.kind     = ValueKind::Word,
		.slot     = -1,
		.material = 0,
		.coat     = 0,
		.number   = 0,
		.items    = {},
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = &name,
		.network  = nullptr,
	};
}

Value lookup(Evaluator& evaluator, const Expression& expression) {
	if (const Value* found = namedPlace(evaluator, expression.text)) {
		return *found;
	}
	return unboundName(evaluator, expression);
}

bool bindsLocally(const Evaluator& evaluator) {
	return evaluator.isolated || evaluator.compiler;
}

Value* boundPlace(Evaluator& evaluator, const std::string& name) {
	return bindsLocally(evaluator) ? findLocal(evaluator, name) : findName(evaluator, name);
}

void bind(Evaluator& evaluator, const std::string& name, Value value) {
	if (Value* found = boundPlace(evaluator, name)) {
		*found = std::move(value);
		return;
	}
	const bool local  = bindsLocally(evaluator);
	const bool shared = !local && sharedName(evaluator, name);
	if (shared || evaluator.blocks.empty()) {
		evaluator.top[name] = std::move(value);
		return;
	}
	bindLocal(evaluator, name, std::move(value));
}

Value evaluateExpression(Evaluator& evaluator, const Expression& expression);

Opcode binaryOpcode(const std::string& symbol) {
	switch (symbol[0]) {
		case '+': return Opcode::Add;
		case '-': return Opcode::Subtract;
		case '*': return Opcode::Multiply;
		case '/': return Opcode::Divide;
		case '<': return symbol.size() == 1 ? Opcode::Less : Opcode::LessEqual;
		case '=': return Opcode::Equal;
		case '!': return Opcode::NotEqual;
		case '&': return Opcode::And;
		default: return Opcode::Or;
	}
}

bool decidesAlone(const Value& a, const std::string& symbol) {
	if (!isKnown(a)) {
		return false;
	}
	return (symbol[0] == '&' && a.number == 0) || (symbol[0] == '|' && a.number != 0);
}

bool readsOnly(const Expression& expression) {
	return expression.kind == ExpressionKind::Number || expression.kind == ExpressionKind::Name;
}

const Value& operandValue(Evaluator& evaluator, const Expression& expression, std::optional<Value>& held) {
	if (expression.kind != ExpressionKind::Name || failed()) {
		return held.emplace(evaluateExpression(evaluator, expression));
	}
	if (const Value* found = namedPlace(evaluator, expression.text)) {
		return *found;
	}
	return held.emplace(unboundName(evaluator, expression));
}

const Value& firstOperand(Evaluator& evaluator, const Expression& expression, std::optional<Value>& held) {
	if (readsOnly(expression.items[1])) {
		return operandValue(evaluator, expression.items[0], held);
	}
	return held.emplace(evaluateExpression(evaluator, expression.items[0]));
}

Value evaluateBinary(Evaluator& evaluator, const Expression& expression) {
	std::optional<Value> heldA;
	std::optional<Value> heldB;
	const Value& a = firstOperand(evaluator, expression, heldA);
	if (decidesAlone(a, expression.text)) {
		return numberValue(a.number == 0 ? 0 : 1);
	}
	const Value& b = operandValue(evaluator, expression.items[1], heldB);
	if (expression.text[0] == '.') {
		fail(expression.location, "a range is only for 'for'");
		return numberValue(0);
	}
	if (expression.text[0] == '>') {
		const Opcode opcode = expression.text.size() == 1 ? Opcode::Less : Opcode::LessEqual;

		const Operation combined = {
			.opcode   = opcode,
			.a        = b,
			.b        = a,
			.c        = zeroValue(),
			.location = expression.location,
		};

		return apply(evaluator, combined);
	}

	const Operation combined2 = {
		.opcode   = binaryOpcode(expression.text),
		.a        = a,
		.b        = b,
		.c        = zeroValue(),
		.location = expression.location,
	};

	return apply(evaluator, combined2);
}

Value evaluateUnary(Evaluator& evaluator, const Expression& expression) {
	const Value a = evaluateExpression(evaluator, expression.items[0]);

	const Operation combined = {
		.opcode   = expression.text == "-" ? Opcode::Negate : Opcode::Not,
		.a        = a,
		.b        = zeroValue(),
		.c        = zeroValue(),
		.location = expression.location,
	};

	return apply(evaluator, combined);
}

bool isRecord(const Value& value) {
	return value.kind == ValueKind::Record;
}

bool matchingRecords(const Value& first, const Value& item, const Location& location) {
	if (isRecord(item) != isRecord(first)) {
		fail(location, "a list that mixes records and other kinds");
		return false;
	}
	if (isRecord(item) && !sameFields(item, first)) {
		fail(location, "a list that mixes records of different fields");
		return false;
	}
	return true;
}

void checkRecords(const std::vector<Value>& items, const Location& location) {
	if (std::none_of(items.begin(), items.end(), isRecord)) {
		return;
	}
	for (const Value& item : items) {
		if (!matchingRecords(items[0], item, location)) {
			return;
		}
	}
}

Value checkedList(std::vector<Value> items, const Location& location) {
	checkRecords(items, location);
	return listValue(std::move(items));
}

Value evaluateList(Evaluator& evaluator, const Expression& expression) {
	std::vector<Value> items;
	items.reserve(expression.items.size());
	for (const Expression& item : expression.items) {
		items.push_back(evaluateExpression(evaluator, item));
	}
	return checkedList(std::move(items), expression.location);
}

Value evaluateRecord(Evaluator& evaluator, const Expression& expression) {
	std::vector<Value> items;
	items.reserve(expression.items.size());
	for (const Expression& item : expression.items) {
		items.push_back(evaluateExpression(evaluator, item));
	}
	return recordValue(expression.names, items);
}

const Value* itemPlace(const Value* list, const Value& index) {
	if (!list || list->kind != ValueKind::List || !isKnown(index) || index.number < 0 || index.number != std::floor(index.number)) {
		return nullptr;
	}
	return index.number < double(list->items.size()) ? &list->items[std::size_t(index.number)] : nullptr;
}

const Value* fieldPlace(const Value* record, const std::string& name) {
	if (!record || !isRecord(*record)) {
		return nullptr;
	}
	const std::vector<std::string>& fields = *record->fields;
	const auto found                       = std::find(fields.begin(), fields.end(), name);
	return found == fields.end() ? nullptr : &record->items[std::size_t(found - fields.begin())];
}

const Value* placeOf(Evaluator& evaluator, const Expression& expression) {
	if (evaluator.compiler) {
		return nullptr;
	}
	if (expression.kind == ExpressionKind::Name) {
		return namedPlace(evaluator, expression.text);
	}
	if (expression.kind == ExpressionKind::Index) {
		const Value index = evaluateExpression(evaluator, expression.items[1]);
		return itemPlace(placeOf(evaluator, expression.items[0]), index);
	}
	if (expression.kind == ExpressionKind::Member) {
		return fieldPlace(placeOf(evaluator, expression.items[0]), expression.text);
	}
	return nullptr;
}

Value memberOf(const Value& value, const Expression& member) {
	if (isRecord(value)) {
		return fieldOf(value, member.text, member.location);
	}
	return swizzle(value, member.text, member.location);
}

Value evaluateMember(Evaluator& evaluator, const Expression& member) {
	if (const Value* value = placeOf(evaluator, member.items[0])) {
		return memberOf(*value, member);
	}
	return memberOf(evaluateExpression(evaluator, member.items[0]), member);
}

Value evaluateIndex(Evaluator& evaluator, const Expression& expression) {
	if (evaluator.compiler) {
		const Value list  = evaluateExpression(evaluator, expression.items[0]);
		const Value index = evaluateExpression(evaluator, expression.items[1]);
		return listItem(list, constantIndex(index, expression.items[1].location), expression.location);
	}
	const Value index        = evaluateExpression(evaluator, expression.items[1]);
	const std::size_t number = failed() ? 0 : constantIndex(index, expression.items[1].location);
	if (const Value* list = failed() ? nullptr : placeOf(evaluator, expression.items[0])) {
		return listItem(*list, number, expression.location);
	}
	return listItem(evaluateExpression(evaluator, expression.items[0]), number, expression.location);
}

Value evaluateCall(Evaluator& evaluator, const Expression& expression);
Value coatValue(Evaluator& evaluator, const Expression& call);
Value evaluateComprehension(Evaluator& evaluator, const Expression& comprehension);

Value evaluateExpression(Evaluator& evaluator, const Expression& expression) {
	if (failed()) {
		return numberValue(0);
	}
	switch (expression.kind) {
		case ExpressionKind::Number: return numberValue(expression.number);
		case ExpressionKind::Colour: return colourValue(expression.text);
		case ExpressionKind::Name: return lookup(evaluator, expression);
		case ExpressionKind::List: return evaluateList(evaluator, expression);
		case ExpressionKind::Call: return evaluateCall(evaluator, expression);
		case ExpressionKind::Index: return evaluateIndex(evaluator, expression);
		case ExpressionKind::Member: return evaluateMember(evaluator, expression);
		case ExpressionKind::Unary: return evaluateUnary(evaluator, expression);
		case ExpressionKind::Binary: return evaluateBinary(evaluator, expression);
		case ExpressionKind::Comprehension: return evaluateComprehension(evaluator, expression);
		case ExpressionKind::Record: return evaluateRecord(evaluator, expression);
	}

	return numberValue(0);
}

struct Arguments {
	std::vector<Value> positional;
	std::map<std::string, Value> named;
};

struct TileGrid {
	std::vector<double> step;
	std::int64_t from[3];
	std::int64_t to[3];
};

struct Call {
	const Arguments& arguments;
	const Expression& expression;
};

Arguments evaluateArguments(Evaluator& evaluator, const Expression& call) {
	Arguments arguments = {
		.positional = {},
		.named      = {},
	};

	arguments.positional.reserve(call.items.size());
	for (std::size_t i = 0; i < call.items.size(); ++i) {
		Value value = evaluateExpression(evaluator, call.items[i]);
		if (call.names[i].empty()) {
			arguments.positional.push_back(std::move(value));
			continue;
		}
		arguments.named[call.names[i]] = std::move(value);
	}
	return arguments;
}

Value positional(const Call& call, std::size_t index) {
	if (index >= call.arguments.positional.size()) {
		fail(call.expression.location, call.expression.text + " needs " + std::to_string(index + 1) + " arguments");
		return numberValue(0);
	}
	return call.arguments.positional[index];
}

void expectCount(const Call& call, std::size_t count) {
	if (call.arguments.positional.size() != count || !call.arguments.named.empty()) {
		fail(call.expression.location, call.expression.text + " takes " + std::to_string(count) + " arguments");
	}
}

Operation operationOf(const Call& call, Opcode opcode, std::size_t count) {
	expectCount(call, count);
	const Value& zero = zeroValue();
	if (failed()) {
		return Operation {
			.opcode   = opcode,
			.a        = zero,
			.b        = zero,
			.c        = zero,
			.location = call.expression.location,
		};
	}
	return Operation {
		.opcode   = opcode,
		.a        = call.arguments.positional[0],
		.b        = count > 1 ? call.arguments.positional[1] : zero,
		.c        = count > 2 ? call.arguments.positional[2] : zero,
		.location = call.expression.location,
	};
}

const Value* unpickable(const Value& value) {
	if (value.kind == ValueKind::Number) {
		return nullptr;
	}
	if (value.kind != ValueKind::List) {
		return &value;
	}
	for (const Value& item : value.items) {
		if (const Value* found = unpickable(item)) {
			return found;
		}
	}
	return nullptr;
}

void expectPickable(const Value& value, const Location& location) {
	if (const Value* found = unpickable(value); found && found->kind != ValueKind::Word) {
		fail(location, std::string("select picks ") + kindName(found->kind) + " only by a condition known when the scene loads");
	}
}

Value selectValue(Evaluator& evaluator, const Operation& operation) {
	if (isKnown(operation.a)) {
		return operation.a.number != 0 ? operation.b : operation.c;
	}

	const Operation selection = {
		.opcode   = Opcode::Select,
		.a        = operation.a,
		.b        = operation.b,
		.c        = operation.c,
		.location = operation.location,
	};

	return apply(evaluator, selection);
}

Value pickedValue(Evaluator& evaluator, const Operation& operation) {
	if (!isKnown(operation.a)) {
		expectPickable(operation.b, operation.location);
		expectPickable(operation.c, operation.location);
	}
	return selectValue(evaluator, operation);
}

Value clampValue(Evaluator& evaluator, const Operation& operation) {
	const Operation larger = {
		.opcode   = Opcode::Maximum,
		.a        = operation.a,
		.b        = operation.b,
		.c        = zeroValue(),
		.location = operation.location,
	};

	const Value low = apply(evaluator, larger);

	const Operation smaller = {
		.opcode   = Opcode::Minimum,
		.a        = low,
		.b        = operation.c,
		.c        = zeroValue(),
		.location = operation.location,
	};

	return apply(evaluator, smaller);
}

Value mixValue(Evaluator& evaluator, const Operation& operation) {
	const Operation difference = {
		.opcode   = Opcode::Subtract,
		.a        = operation.b,
		.b        = operation.a,
		.c        = zeroValue(),
		.location = operation.location,
	};

	const Value span = apply(evaluator, difference);

	const Operation product = {
		.opcode   = Opcode::Multiply,
		.a        = span,
		.b        = operation.c,
		.c        = zeroValue(),
		.location = operation.location,
	};

	const Value step = apply(evaluator, product);

	const Operation sum = {
		.opcode   = Opcode::Add,
		.a        = operation.a,
		.b        = step,
		.c        = zeroValue(),
		.location = operation.location,
	};

	return apply(evaluator, sum);
}

Value normalizeValue(Evaluator& evaluator, const Operation& operation) {
	const Value length = lengthValue(evaluator, operation.a, operation.location);

	const Operation quotient = {
		.opcode   = Opcode::Divide,
		.a        = operation.a,
		.b        = length,
		.c        = zeroValue(),
		.location = operation.location,
	};

	return apply(evaluator, quotient);
}

Value countValue(const Operation& operation) {
	if (operation.a.kind != ValueKind::List) {
		fail(operation.location, "count takes a list");
		return numberValue(0);
	}
	return numberValue(double(operation.a.items.size()));
}

Value appendedList(Value list, const std::vector<Value>& added, const Location& location) {
	if (added.size() != 1) {
		fail(location, "append takes 2 arguments");
		return numberValue(0);
	}
	if (list.kind != ValueKind::List) {
		fail(location, "append takes a list and a value");
		return numberValue(0);
	}
	if (!list.items.empty() && !matchingRecords(list.items[0], added[0], location)) {
		return numberValue(0);
	}
	list.items.push_back(added[0]);
	return list;
}

bool joinable(const Value& list, const Value& more, const Location& location) {
	if (more.kind != ValueKind::List) {
		fail(location, "join takes lists");
		return false;
	}
	return list.items.empty() || more.items.empty() || matchingRecords(list.items[0], more.items[0], location);
}

Value joinedList(Value list, const std::vector<Value>& added, const Location& location) {
	if (list.kind != ValueKind::List) {
		fail(location, "join takes lists");
		return numberValue(0);
	}
	for (const Value& more : added) {
		if (!joinable(list, more, location)) {
			return numberValue(0);
		}
		list.items.insert(list.items.end(), more.items.begin(), more.items.end());
	}
	return list;
}

Value grownList(const Expression& call, Value list, const std::vector<Value>& added) {
	if (call.text == "append") {
		return appendedList(std::move(list), added, call.location);
	}
	return joinedList(std::move(list), added, call.location);
}

Value grownCall(const Call& call) {
	const std::vector<Value>& positional = call.arguments.positional;
	if (positional.empty() || !call.arguments.named.empty()) {
		fail(call.expression.location, call.expression.text == "append" ? "append takes 2 arguments" : "join takes lists");
		return numberValue(0);
	}
	const std::vector<Value> added(positional.begin() + 1, positional.end());
	return grownList(call.expression, positional[0], added);
}

std::size_t elementwiseCount(Opcode opcode) {
	return opcode == Opcode::ArcTangent || opcode == Opcode::Power || opcode == Opcode::Minimum || opcode == Opcode::Maximum ? 2 : 1;
}

double knownOf(const Value& value, const Location& location) {
	expectNumber(value, location);
	if (value.slot >= 0) {
		fail(location, "expected a number known when the scene loads");
	}
	return value.number;
}

bool shapedStop(const Value& stop, const std::vector<TimedStop>& read) {
	const bool listed = stop.kind == ValueKind::List && stop.items.size() >= 2;
	return listed && (read.empty() || stop.items.size() == read[0].values.size() + 1);
}

TimedStop timedStop(const Value& stop, const Location& location) {
	std::vector<double> numbers;
	for (const Value& item : stop.items) {
		numbers.push_back(knownOf(item, location));
	}
	return TimedStop {
		.time   = numbers[0],
		.values = std::vector<double>(numbers.begin() + 1, numbers.end()),
	};
}

std::vector<TimedStop> timedStops(const Value& stops, const Location& location) {
	if (stops.kind != ValueKind::List || stops.items.empty()) {
		fail(location, "the stops are a list of [time, values...]");
		return {};
	}
	std::vector<TimedStop> read;
	for (const Value& stop : stops.items) {
		if (!shapedStop(stop, read)) {
			fail(location, "a stop is [time, values...], as long as the first");
			return {};
		}
		read.push_back(timedStop(stop, location));
		if (read.size() > 1 && read.back().time < read[read.size() - 2].time) {
			fail(location, "the stops' times rise");
			return {};
		}
	}
	return read;
}

std::size_t stopAtOrBefore(const std::vector<TimedStop>& stops, double t) {
	std::size_t at = 0;
	while (at + 1 < stops.size() && stops[at + 1].time <= t) {
		at += 1;
	}
	return at;
}

std::vector<double> blendedValues(const std::vector<double>& a, const std::vector<double>& b, double share) {
	std::vector<double> blended;
	for (std::size_t i = 0; i < a.size(); ++i) {
		blended.push_back(a[i] + (b[i] - a[i]) * share);
	}
	return blended;
}

bool inCurve(const std::vector<TimedStop>& stops, std::size_t from, std::size_t to) {
	return to < stops.size() && stops[from].time != stops[to].time;
}

std::vector<double> slopeBetween(const TimedStop& a, const TimedStop& b) {
	std::vector<double> slope;
	for (std::size_t i = 0; i < a.values.size(); ++i) {
		slope.push_back((b.values[i] - a.values[i]) / (b.time - a.time));
	}
	return slope;
}

std::vector<double> slopeAt(const std::vector<TimedStop>& stops, std::size_t at) {
	const bool before      = at > 0 && inCurve(stops, at, at - 1);
	const bool after       = inCurve(stops, at, at + 1);
	const TimedStop& first = before ? stops[at - 1] : stops[at];
	const TimedStop& last  = after ? stops[at + 1] : stops[at];
	if (!before && !after) {
		return std::vector<double>(stops[at].values.size(), 0.0);
	}
	return slopeBetween(first, last);
}

std::vector<double> hermiteValues(const std::vector<TimedStop>& stops, std::size_t at, double share) {
	const double s                 = share;
	const double span              = stops[at + 1].time - stops[at].time;
	const double weights[4]        = { 2 * s * s * s - 3 * s * s + 1, (s * s * s - 2 * s * s + s) * span, -2 * s * s * s + 3 * s * s, (s * s * s - s * s) * span };
	const std::vector<double> from = slopeAt(stops, at);
	const std::vector<double> to   = slopeAt(stops, at + 1);
	std::vector<double> values;
	for (std::size_t i = 0; i < from.size(); ++i) {
		values.push_back(weights[0] * stops[at].values[i] + weights[1] * from[i] + weights[2] * stops[at + 1].values[i] + weights[3] * to[i]);
	}
	return values;
}

std::vector<double> stopsAt(const std::vector<TimedStop>& stops, double t, bool smooth) {
	const std::size_t at = stopAtOrBefore(stops, t);
	if (t <= stops[at].time || at + 1 == stops.size()) {
		return stops[at].values;
	}
	const double share = (t - stops[at].time) / (stops[at + 1].time - stops[at].time);
	return smooth ? hermiteValues(stops, at, share) : blendedValues(stops[at].values, stops[at + 1].values, share);
}

Value numbersValue(const std::vector<double>& numbers) {
	if (numbers.size() == 1) {
		return numberValue(numbers[0]);
	}
	std::vector<Value> items;
	items.reserve(numbers.size());
	for (const double number : numbers) {
		items.push_back(numberValue(number));
	}
	return listValue(std::move(items));
}

Value applied(Evaluator& evaluator, Opcode opcode, const Value& a, const Value& b) {
	const Operation operation = {
		.opcode   = opcode,
		.a        = a,
		.b        = b,
		.c        = zeroValue(),
		.location = {},
	};

	return apply(evaluator, operation);
}

Value stopShare(Evaluator& evaluator, const TimedStop& from, const TimedStop& to, const Value& t) {
	if (from.time == to.time) {
		return applied(evaluator, Opcode::LessEqual, numberValue(from.time), t);
	}
	const Value since = applied(evaluator, Opcode::Subtract, t, numberValue(from.time));
	const Value share = applied(evaluator, Opcode::Divide, since, numberValue(to.time - from.time));
	return applied(evaluator, Opcode::Minimum, applied(evaluator, Opcode::Maximum, share, numberValue(0)), numberValue(1));
}

Value scaledStop(const std::vector<double>& a, const std::vector<double>& b, double scale) {
	std::vector<double> scaled;
	for (std::size_t i = 0; i < b.size(); ++i) {
		scaled.push_back((b[i] - a[i]) * scale);
	}
	return numbersValue(scaled);
}

Value hermiteRise(Evaluator& evaluator, const std::vector<TimedStop>& stops, std::size_t at, const Value& s) {
	const double span              = stops[at + 1].time - stops[at].time;
	const std::vector<double> flat = std::vector<double>(stops[at].values.size(), 0.0);
	const Value squared            = applied(evaluator, Opcode::Multiply, s, s);
	const Value less               = applied(evaluator, Opcode::Subtract, s, numberValue(1));
	const Value rise               = applied(evaluator, Opcode::Subtract, numberValue(3), applied(evaluator, Opcode::Multiply, numberValue(2), s));
	const Value toward             = applied(evaluator, Opcode::Multiply, scaledStop(stops[at].values, stops[at + 1].values, 1), applied(evaluator, Opcode::Multiply, squared, rise));
	const Value leaving            = applied(evaluator, Opcode::Multiply, scaledStop(flat, slopeAt(stops, at), span), applied(evaluator, Opcode::Multiply, s, applied(evaluator, Opcode::Multiply, less, less)));
	const Value arriving           = applied(evaluator, Opcode::Multiply, scaledStop(flat, slopeAt(stops, at + 1), span), applied(evaluator, Opcode::Multiply, squared, less));
	return applied(evaluator, Opcode::Add, applied(evaluator, Opcode::Add, toward, leaving), arriving);
}

Value stopRise(Evaluator& evaluator, const std::vector<TimedStop>& stops, std::size_t at, const Value& t, bool smooth) {
	const Value share = stopShare(evaluator, stops[at], stops[at + 1], t);
	if (smooth && stops[at].time != stops[at + 1].time) {
		return hermiteRise(evaluator, stops, at, share);
	}
	return applied(evaluator, Opcode::Multiply, scaledStop(stops[at].values, stops[at + 1].values, 1), share);
}

Value compiledStops(Evaluator& evaluator, const std::vector<TimedStop>& stops, const Value& t, bool smooth) {
	Value sum = numbersValue(stops[0].values);
	for (std::size_t at = 0; at + 1 < stops.size(); ++at) {
		sum = applied(evaluator, Opcode::Add, sum, stopRise(evaluator, stops, at, t, smooth));
	}
	return sum;
}

Value stopsValue(Evaluator& evaluator, const Call& call, bool smooth) {
	expectCount(call, 2);
	if (failed()) {
		return numberValue(0);
	}
	const std::vector<TimedStop> stops = timedStops(call.arguments.positional[0], call.expression.location);
	const Value& t                     = call.arguments.positional[1];
	if (!failed() && t.kind == ValueKind::Number && t.slot >= 0) {
		return compiledStops(evaluator, stops, t, smooth);
	}
	const double at = knownOf(t, call.expression.location);
	if (failed()) {
		return numberValue(0);
	}
	return numbersValue(stopsAt(stops, at, smooth));
}

double loadedNumber(const Value& value, const Call& call) {
	if (value.kind == ValueKind::Number && value.slot >= 0) {
		fail(call.expression.location, call.expression.text + " is known only when the scene loads");
	}
	expectNumber(value, call.expression.location);
	return value.number;
}

std::vector<double> loadedNumbers(const Value& value, const Call& call) {
	if (value.kind != ValueKind::List) {
		return { loadedNumber(value, call) };
	}
	std::vector<double> numbers;
	for (const Value& item : value.items) {
		numbers.push_back(loadedNumber(item, call));
	}
	return numbers;
}

std::uint64_t hashedWith(std::uint64_t hash, double number) {
	const std::uint64_t bits = number == 0 ? 0 : std::bit_cast<std::uint64_t>(number);
	return math::mixedBits(hash ^ bits);
}

Value hashValue(const Call& call) {
	if (call.arguments.positional.empty() || !call.arguments.named.empty()) {
		fail(call.expression.location, "hash takes numbers and vectors");
		return numberValue(0);
	}
	std::uint64_t hash = 0;
	for (const Value& argument : call.arguments.positional) {
		for (const double number : loadedNumbers(argument, call)) {
			hash = hashedWith(hash, number);
		}
	}
	return numberValue(failed() ? 0 : math::unitOf(hash));
}

struct NoiseCell {
	std::vector<double> corner;
	std::vector<double> fade;
	std::uint64_t seed;
};

double quinticFade(double t) {
	return t * t * t * (t * (t * 6 - 15) + 10);
}

double cornerNoise(const NoiseCell& cell, std::size_t corner) {
	std::uint64_t hash = cell.seed;
	double weight      = 1;
	for (std::size_t i = 0; i < cell.corner.size(); ++i) {
		const bool upper = ((corner >> i) & 1) != 0;
		hash             = hashedWith(hash, cell.corner[i] + (upper ? 1 : 0));
		weight *= upper ? cell.fade[i] : 1 - cell.fade[i];
	}
	return weight * (2 * math::unitOf(hash) - 1);
}

double latticeNoise(const std::vector<double>& p, std::uint64_t seed) {
	NoiseCell cell = {
		.corner = {},
		.fade   = {},
		.seed   = seed,
	};

	for (const double x : p) {
		const double corner = std::floor(x);
		cell.corner.push_back(corner);
		cell.fade.push_back(quinticFade(x - corner));
	}
	double sum = 0;
	for (std::size_t corner = 0; corner < (std::size_t(1) << p.size()); ++corner) {
		sum += cornerNoise(cell, corner);
	}
	return sum;
}

double fractalNoise(const std::vector<double>& p, std::size_t octaves) {
	std::vector<double> q = p;
	double sum            = 0;
	double amplitude      = 1;
	double total          = 0;
	for (std::size_t octave = 0; octave < octaves; ++octave) {
		sum += amplitude * latticeNoise(q, octave);
		total += amplitude;
		amplitude *= 0.5;
		for (double& x : q) {
			x *= 2;
		}
	}
	return sum / total;
}

std::vector<double> noisePoint(const Call& call) {
	const std::vector<double> p = loadedNumbers(positional(call, 0), call);
	if (!failed() && p.size() != 2 && p.size() != 3) {
		fail(call.expression.location, call.expression.text + " takes a vector of 2 or 3 numbers");
	}
	return p;
}

Value noiseValue(const Call& call) {
	expectCount(call, 1);
	const std::vector<double> p = noisePoint(call);
	return numberValue(failed() ? 0 : latticeNoise(p, 0));
}

std::size_t octaveCount(const Call& call) {
	const double octaves = loadedNumber(positional(call, 1), call);
	if (!failed() && (octaves < 1 || octaves != std::floor(octaves))) {
		fail(call.expression.location, "fbm's octaves are a whole number of 1 or more");
	}
	return failed() ? 0 : std::size_t(octaves);
}

Value fbmValue(const Call& call) {
	expectCount(call, 2);
	const std::vector<double> p = noisePoint(call);
	const std::size_t octaves   = octaveCount(call);
	return numberValue(failed() ? 0 : fractalNoise(p, octaves));
}

std::string eyeReadName(double step) {
	char text[40];
	std::snprintf(text, sizeof(text), "eye@%.17g", step);
	return text;
}

std::optional<double> eyeReadStep(const std::string& name) {
	if (!name.starts_with("eye@")) {
		return std::nullopt;
	}
	return std::strtod(name.c_str() + 4, nullptr);
}

double snappedAxis(double x, double step) {
	return (std::floor(x / step) + 0.5) * step;
}

math::Vector snappedEye(const math::Vector& eye, double step) {
	return { snappedAxis(eye.x, step), snappedAxis(eye.y, step), snappedAxis(eye.z, step) };
}

double eyeStep(const Call& call) {
	expectCount(call, 1);
	const double step = loadedNumber(positional(call, 0), call);
	if (!failed() && !(step > 0 && std::isfinite(step))) {
		fail(call.expression.location, "eye's step is a number above 0");
	}
	return step;
}

Value eyeValue(Evaluator& evaluator, const Call& call) {
	if (evaluator.compiler) {
		fail(call.expression.location, "eye is not known in a surface");
		return numberValue(0);
	}
	const double step = eyeStep(call);
	if (!failed() && !evaluator.eye) {
		fail(call.expression.location, "eye reads the camera, and no camera block comes before it");
	}
	if (failed()) {
		return numberValue(0);
	}
	if (evaluator.tiled) {
		evaluator.reads.insert(eyeReadName(step));
	}
	const math::Vector snapped = snappedEye(*evaluator.eye, step);
	return numbersValue({ snapped.x, snapped.y, snapped.z });
}

std::string bandReadName(double step, const std::vector<double>& point, const std::vector<double>& radii) {
	std::string name = "band@";
	char text[40];
	std::snprintf(text, sizeof(text), "%.17g", step);
	name += text;
	for (const std::vector<double>& part : { point, radii }) {
		name += "@";
		for (const double number : part) {
			std::snprintf(text, sizeof(text), "%.17g,", number);
			name += text;
		}
	}
	return name;
}

std::vector<double> readNumbers(const char*& text) {
	std::vector<double> numbers;
	while (*text != '\0' && *text != '@') {
		char* end = nullptr;
		numbers.push_back(std::strtod(text, &end));
		text = *end == ',' ? end + 1 : end;
	}
	text = *text == '@' ? text + 1 : text;
	return numbers;
}

std::optional<double> bandReadStep(const std::string& name) {
	if (!name.starts_with("band@")) {
		return std::nullopt;
	}
	return std::strtod(name.c_str() + 5, nullptr);
}

std::size_t bandIndex(const std::vector<double>& point, const std::vector<double>& radii, const math::Vector& snapped) {
	const double dx       = point[0] - snapped.x;
	const double dy       = point[1] - snapped.y;
	const double dz       = point.size() == 3 ? point[2] - snapped.z : 0;
	const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
	return std::size_t(std::count_if(radii.begin(), radii.end(), [&](double radius) {
		return distance >= radius;
	}));
}

std::size_t bandOfRead(const std::string& name, const math::Vector& eye, const math::Vector& corner) {
	const char* text                = name.c_str() + 5;
	const double step               = std::strtod(text, nullptr);
	text                            = std::strchr(text, '@') + 1;
	std::vector<double> point       = readNumbers(text);
	const std::vector<double> radii = readNumbers(text);
	const double offsets[3]         = { corner.x, corner.y, corner.z };
	for (std::size_t axis = 0; axis < point.size(); ++axis) {
		point[axis] += offsets[axis];
	}
	return bandIndex(point, radii, snappedEye(eye, step));
}

std::vector<double> pointInTile(const Evaluator& evaluator, std::vector<double> point) {
	const Value* corner = readInScope(evaluator.tile, "tile_origin");
	for (std::size_t axis = 0; corner && axis < point.size(); ++axis) {
		point[axis] -= corner->items[axis].number;
	}
	return point;
}

bool risingNumbers(const std::vector<double>& numbers) {
	for (std::size_t i = 1; i < numbers.size(); ++i) {
		if (!(numbers[i] > numbers[i - 1])) {
			return false;
		}
	}
	return true;
}

Value bandValue(Evaluator& evaluator, const Call& call) {
	if (evaluator.compiler) {
		fail(call.expression.location, "eye_band is not known in a surface");
		return numberValue(0);
	}
	expectCount(call, 3);
	if (failed()) {
		return numberValue(0);
	}
	const std::vector<double> point = loadedNumbers(positional(call, 0), call);
	const std::vector<double> radii = loadedNumbers(positional(call, 1), call);
	const double step               = loadedNumber(positional(call, 2), call);
	if (!failed() && (point.size() < 2 || point.size() > 3 || positional(call, 1).kind != ValueKind::List || !risingNumbers(radii) || !(step > 0 && std::isfinite(step)))) {
		fail(call.expression.location, "eye_band takes a point of 2 or 3 numbers, a list of rising radii and a step above 0");
	}
	if (!failed() && !evaluator.eye) {
		fail(call.expression.location, "eye_band reads the camera, and no camera block comes before it");
	}
	if (failed()) {
		return numberValue(0);
	}
	if (evaluator.tiled) {
		evaluator.reads.insert(bandReadName(step, pointInTile(evaluator, point), radii));
	}
	return numberValue(double(bandIndex(point, radii, snappedEye(*evaluator.eye, step))));
}

std::uint64_t valueKey(const Value& value);

bool namedOnly(const Call& call, const std::string& name) {
	return call.arguments.named.empty() || (call.arguments.named.size() == 1 && call.arguments.named.contains(name));
}

double networkSpacing(const Call& call) {
	const auto found = call.arguments.named.find("spacing");
	return found == call.arguments.named.end() ? 8 : loadedNumber(found->second, call);
}

Value madeNetwork(const Call& call) {
	if (call.arguments.positional.size() != 1 || !namedOnly(call, "spacing")) {
		fail(call.expression.location, "network takes a list of lines and a spacing");
		return numberValue(0);
	}

	const NetworkSource source = {
		.lines    = call.arguments.positional[0],
		.spacing  = networkSpacing(call),
		.key      = valueKey(call.arguments.positional[0]),
		.location = call.expression.location,
	};

	const std::shared_ptr<const Network> network = failed() ? nullptr : builtNetwork(source);
	return network ? networkValue(network) : numberValue(0);
}

bool networkCall(const Call& call, std::size_t least, std::size_t most) {
	const std::vector<Value>& positional = call.arguments.positional;
	const bool counted                   = positional.size() >= least && positional.size() <= most && call.arguments.named.empty();
	return counted && positional[0].kind == ValueKind::Network;
}

std::vector<double> networkPoint(const Value& value, const Call& call) {
	if (value.kind != ValueKind::List || (value.items.size() != 2 && value.items.size() != 3)) {
		return {};
	}
	return loadedNumbers(value, call);
}

Value nearestCall(const Call& call) {
	const std::vector<Value>& positional = call.arguments.positional;
	const bool shaped                    = networkCall(call, 2, 3);
	const std::vector<double> q          = shaped ? networkPoint(positional[1], call) : std::vector<double> {};
	const double reach                   = shaped && positional.size() == 3 ? loadedNumber(positional[2], call) : std::numeric_limits<double>::infinity();
	if (!failed() && q.empty()) {
		fail(call.expression.location, "nearest takes a network, a point of 2 or 3 numbers and a reach");
	}
	if (failed()) {
		return numberValue(0);
	}

	const NetworkQuery query = {
		.x     = q[0],
		.y     = q[1],
		.reach = reach,
	};

	return nearestValue(*positional[0].network, query);
}

Value withinCall(const Call& call) {
	const std::vector<Value>& positional = call.arguments.positional;
	const bool shaped                    = networkCall(call, 3, 3);
	const std::vector<double> a          = shaped ? networkPoint(positional[1], call) : std::vector<double> {};
	const std::vector<double> b          = shaped ? networkPoint(positional[2], call) : std::vector<double> {};
	if (!failed() && (a.empty() || b.empty())) {
		fail(call.expression.location, "within takes a network and two corners of 2 or 3 numbers");
	}
	if (failed()) {
		return numberValue(0);
	}

	const NetworkBox box = {
		.lower = { std::min(a[0], b[0]), std::min(a[1], b[1]) },
		.upper = { std::max(a[0], b[0]), std::max(a[1], b[1]) },
	};

	return withinValue(*positional[0].network, box);
}

std::vector<float> floats(const Value& value, std::size_t count, const Location& location) {
	if (isKnown(value)) {
		return std::vector<float>(count, float(value.number));
	}
	if (value.kind != ValueKind::List || value.items.size() != count) {
		fail(location, "expected " + std::to_string(count) + " numbers");
		return std::vector<float>(count, 0.0f);
	}
	std::vector<float> out;
	for (const Value& item : value.items) {
		expectNumber(item, location);
		out.push_back(float(item.number));
	}
	return out;
}

float floatOf(const Value& value, const Location& location) {
	return float(knownOf(value, location));
}

std::vector<float> rampStops(const Pattern& pattern) {
	std::vector<float> stops;
	for (const Value& stop : pattern.stops) {
		const std::vector<float> colour = floats(stop, 3, pattern.location);
		stops.insert(stops.end(), colour.begin(), colour.end());
	}
	return stops;
}

Albedo colourAlbedo(const std::vector<float>& colour) {
	return Albedo {
		.stops     = colour,
		.program   = -1,
		.arguments = 0,
		.world     = false,
	};
}

Albedo patternAlbedo(Evaluator& evaluator, const Pattern& pattern) {
	Albedo albedo = {
		.stops     = rampStops(pattern),
		.program   = std::int32_t(specialise(evaluator, pattern)),
		.arguments = std::uint32_t(evaluator.scene.arguments.size()),
		.world     = pattern.world,
	};

	pushArguments(evaluator, pattern.arguments);
	return albedo;
}

Albedo rampAlbedo(Evaluator& evaluator, const Pattern& pattern) {
	if (pattern.stops.empty()) {
		fail(pattern.location, "a pattern paints through ramp(pattern, colours...)");
		return colourAlbedo({ 0, 0, 0 });
	}
	return patternAlbedo(evaluator, pattern);
}

Albedo makeAlbedo(Evaluator& evaluator, const Value& value, const Location& location) {
	if (value.kind != ValueKind::Pattern) {
		return Albedo {
			.stops     = floats(value, 3, location),
			.program   = -1,
			.arguments = 0,
			.world     = false,
		};
	}
	return rampAlbedo(evaluator, *value.pattern);
}

struct MaterialField {
	const std::string& name;
	const Value& value;
	const Location& location;
};

void expectGlass(const Material& material, const MaterialField& field) {
	if (material.kind != MaterialKind::Glass) {
		fail(field.location, "only a glass takes '" + field.name + "'");
	}
}

void expectFilmed(const Material& material, const MaterialField& field) {
	if (material.kind == MaterialKind::Matte) {
		fail(field.location, "a matte takes no film");
	}
}

void readMaterialField(Material& material, const MaterialField& field) {
	const std::string& name = field.name;
	if (name == "roughness" || name == "refraction" || name == "strength") {
		float& into = name == "roughness" ? material.roughness : (name == "refraction" ? material.refraction : material.strength);
		into        = floatOf(field.value, field.location);
		return;
	}
	if (name == "film") {
		expectFilmed(material, field);
		const std::vector<float> film = floats(field.value, 2, field.location);
		material.film[0]              = film[0];
		material.film[1]              = film[1];
		return;
	}
	if (name == "dispersion" || name == "scatter" || name == "lean") {
		expectGlass(material, field);
		float& into = name == "dispersion" ? material.dispersion : (name == "scatter" ? material.scatter : material.lean);
		into        = floatOf(field.value, field.location);
		if (material.scatter < 0 || !(std::abs(material.lean) < 1)) {
			fail(field.location, "a scatter is 0 or more and a lean between -1 and 1");
		}
		return;
	}
	if (name == "guided" || name == "relays" || name == "aimed") {
		bool& into = name == "guided" ? material.guided : (name == "relays" ? material.relays : material.aimed);
		into       = floatOf(field.value, field.location) != 0;
		return;
	}
	if (name == "absorb" || name == "glow") {
		Colour& into = name == "absorb" ? material.absorb : material.glow;
		into         = colourFromFloats(floats(field.value, 3, field.location));
		return;
	}
	fail(field.location, "a material has no field '" + name + "'");
}

bool makesMaterial(Builtin builtin) {
	return builtin == Builtin::Matte || builtin == Builtin::Plastic || builtin == Builtin::Metal || builtin == Builtin::Glass;
}

MaterialKind materialKind(Builtin builtin) {
	switch (builtin) {
		case Builtin::Plastic: return MaterialKind::Plastic;
		case Builtin::Metal: return MaterialKind::Metal;
		case Builtin::Glass: return MaterialKind::Glass;
		default: return MaterialKind::Matte;
	}
}

Value pushMaterial(Evaluator& evaluator, const Material& material) {
	evaluator.looks.materials.push_back(material);
	return Value {
		.kind     = ValueKind::Material,
		.slot     = -1,
		.material = std::uint32_t(evaluator.looks.materials.size() - 1),
		.coat     = 0,
		.number   = 0,
		.items    = {},
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = nullptr,
	};
}

Value madeMaterial(Evaluator& evaluator, const Call& call) {
	refusedInTiles(evaluator, call.expression.location, "make a material");

	Material material = {
		.kind   = materialKind(call.expression.builtin),
		.albedo = Albedo {
			.stops     = { 1, 1, 1 },
			.program   = -1,
			.arguments = 0,
			.world     = false,
		},

		.roughness  = 0.5f,
		.refraction = 1.5f,
		.dispersion = 0,
		.scatter    = 0,
		.lean       = 0,
		.absorb     = { 0, 0, 0 },
		.glow       = { 0, 0, 0 },
		.strength   = 0,
		.film       = { 0, 1 },
		.guided     = true,
		.relays     = true,
		.aimed      = true,
	};

	if (material.kind != MaterialKind::Glass) {
		material.albedo = makeAlbedo(evaluator, positional(call, 0), call.expression.location);
	}
	for (const auto& [name, value] : call.arguments.named) {
		const MaterialField field = {
			.name     = name,
			.value    = value,
			.location = call.expression.location,
		};

		readMaterialField(material, field);
	}
	return pushMaterial(evaluator, material);
}

std::optional<Pattern> patternArgument(const Call& call) {
	const Value first = positional(call, 0);
	if (first.kind != ValueKind::Pattern) {
		fail(call.expression.location, call.expression.text + " takes a pattern first");
		return std::nullopt;
	}
	return *first.pattern;
}

Value worldPattern(const Call& call) {
	expectCount(call, 1);
	const std::optional<Pattern> pattern = patternArgument(call);
	if (!pattern) {
		return numberValue(0);
	}
	Pattern world = *pattern;
	world.world   = true;
	return patternValue(world);
}

Value rampPattern(const Call& call) {
	if (call.arguments.positional.size() < 3) {
		fail(call.expression.location, "ramp takes a pattern and two colours or more");
		return numberValue(0);
	}
	const std::optional<Pattern> pattern = patternArgument(call);
	if (!pattern) {
		return numberValue(0);
	}
	Pattern ramp = *pattern;
	ramp.stops.assign(call.arguments.positional.begin() + 1, call.arguments.positional.end());
	return patternValue(ramp);
}

void runBlock(Evaluator& evaluator, const std::vector<Statement>& body, const Context& context);

void bindParameters(Evaluator& evaluator, const Invocation& call) {
	const Statement& definition = call.definition;
	if (call.arguments.size() != definition.names.size()) {
		fail(call.location, definition.text + " takes " + std::to_string(definition.names.size()) + " arguments");
		return;
	}
	for (std::size_t i = 0; i < call.arguments.size(); ++i) {
		bindLocal(evaluator, definition.names[i], std::move(call.arguments[i]));
	}
}

void runDefinition(Evaluator& evaluator, const Invocation& call) {
	const std::size_t savedBase = evaluator.base;
	const bool savedIsolated    = evaluator.isolated;
	evaluator.base              = evaluator.blocks.size();
	evaluator.isolated          = call.definition.kind == StatementKind::Function;
	openBlock(evaluator);
	bindParameters(evaluator, call);
	runBlock(evaluator, call.definition.body, call.context);
	closeBlock(evaluator);
	evaluator.base     = savedBase;
	evaluator.isolated = savedIsolated;
}

Value inlineFunction(Evaluator& evaluator, const Invocation& call) {
	const bool savedReturned = evaluator.returned;
	evaluator.returned       = false;
	runDefinition(evaluator, call);
	if (!evaluator.returned) {
		fail(call.definition.location, call.definition.text + " never returns");
	}
	evaluator.returned = savedReturned;
	return std::move(evaluator.result);
}

bool isComputed(const Statement& definition) {
	return definition.kind == StatementKind::Surface || definition.kind == StatementKind::Function;
}

const Statement* findFunction(const Evaluator& evaluator, const Expression& call) {
	const auto found = evaluator.definitions.find(call.text);
	if (found == evaluator.definitions.end() || !isComputed(*found->second)) {
		fail(call.location, "unknown function '" + call.text + "'");
		return nullptr;
	}
	return found->second;
}

Value callFunction(Evaluator& evaluator, const Expression& call, Arguments arguments) {
	const Statement* found = findFunction(evaluator, call);
	if (!found) {
		return numberValue(0);
	}
	const Statement& function = *found;
	if (!arguments.named.empty()) {
		fail(call.location, function.text + " takes positional arguments");
		return numberValue(0);
	}
	if (evaluator.compiler || function.kind == StatementKind::Function) {
		const Invocation invocation = {
			.definition = function,
			.arguments  = arguments.positional,
			.context    = rootContext(),
			.location   = call.location,
		};

		return inlineFunction(evaluator, invocation);
	}

	const Pattern pattern = {
		.function  = &function,
		.arguments = std::move(arguments.positional),
		.location  = call.location,
		.world     = false,
		.stops     = {},
	};

	return patternValue(pattern);
}

Value elementwise(Evaluator& evaluator, const Call& call, Opcode opcode) {
	return apply(evaluator, operationOf(call, opcode, elementwiseCount(opcode)));
}

Value printedValue(const Call& call) {
	fail(call.expression.location, "print stands alone as a statement");
	return numberValue(0);
}

Value builtinValue(Evaluator& evaluator, const Call& call) {
	switch (call.expression.builtin) {
		case Builtin::SquareRoot: return elementwise(evaluator, call, Opcode::SquareRoot);
		case Builtin::Absolute: return elementwise(evaluator, call, Opcode::Absolute);
		case Builtin::Floor: return elementwise(evaluator, call, Opcode::Floor);
		case Builtin::Sine: return elementwise(evaluator, call, Opcode::Sine);
		case Builtin::Cosine: return elementwise(evaluator, call, Opcode::Cosine);
		case Builtin::Sign: return elementwise(evaluator, call, Opcode::Sign);
		case Builtin::ArcTangent: return elementwise(evaluator, call, Opcode::ArcTangent);
		case Builtin::Power: return elementwise(evaluator, call, Opcode::Power);
		case Builtin::Minimum: return elementwise(evaluator, call, Opcode::Minimum);
		case Builtin::Maximum: return elementwise(evaluator, call, Opcode::Maximum);
		case Builtin::Clamp: return clampValue(evaluator, operationOf(call, Opcode::Add, 3));
		case Builtin::Mix: return mixValue(evaluator, operationOf(call, Opcode::Add, 3));
		case Builtin::Select: return pickedValue(evaluator, operationOf(call, Opcode::Select, 3));
		case Builtin::Dot: return dotValue(evaluator, operationOf(call, Opcode::Add, 2));
		case Builtin::Cross: return crossValue(evaluator, operationOf(call, Opcode::Add, 2));
		case Builtin::Length: return lengthValue(evaluator, operationOf(call, Opcode::Add, 1).a, call.expression.location);
		case Builtin::Normalize: return normalizeValue(evaluator, operationOf(call, Opcode::Add, 1));
		case Builtin::All: return foldList(evaluator, operationOf(call, Opcode::And, 1));
		case Builtin::Any: return foldList(evaluator, operationOf(call, Opcode::Or, 1));
		case Builtin::Count: return countValue(operationOf(call, Opcode::Add, 1));
		case Builtin::Append: return grownCall(call);
		case Builtin::Join: return grownCall(call);
		case Builtin::Linear: return stopsValue(evaluator, call, false);
		case Builtin::Smooth: return stopsValue(evaluator, call, true);
		case Builtin::Hash: return hashValue(call);
		case Builtin::Noise: return noiseValue(call);
		case Builtin::FractalNoise: return fbmValue(call);
		case Builtin::Eye: return eyeValue(evaluator, call);
		case Builtin::EyeBand: return bandValue(evaluator, call);
		case Builtin::Network: return madeNetwork(call);
		case Builtin::Nearest: return nearestCall(call);
		case Builtin::Within: return withinCall(call);
		case Builtin::Matte: return madeMaterial(evaluator, call);
		case Builtin::Plastic: return madeMaterial(evaluator, call);
		case Builtin::Metal: return madeMaterial(evaluator, call);
		case Builtin::Glass: return madeMaterial(evaluator, call);
		case Builtin::World: return worldPattern(call);
		case Builtin::Ramp: return rampPattern(call);
		case Builtin::Print: return printedValue(call);
		case Builtin::None: break;
	}

	return numberValue(0);
}

const Value* countedPlace(Evaluator& evaluator, const Expression& call) {
	if (call.builtin != Builtin::Count || call.items.size() != 1 || !call.names[0].empty()) {
		return nullptr;
	}
	const Value* list = placeOf(evaluator, call.items[0]);
	return list && list->kind == ValueKind::List ? list : nullptr;
}

Value evaluateCall(Evaluator& evaluator, const Expression& expression) {
	if (expression.text == "coat") {
		return coatValue(evaluator, expression);
	}
	if (const Value* list = countedPlace(evaluator, expression)) {
		return numberValue(double(list->items.size()));
	}
	Arguments arguments = evaluateArguments(evaluator, expression);
	if (expression.builtin == Builtin::None) {
		return callFunction(evaluator, expression, std::move(arguments));
	}

	const Call call = {
		.arguments  = arguments,
		.expression = expression,
	};

	return builtinValue(evaluator, call);
}

struct Affine {
	double linear[3][3];

	double offset[3];
};

void multiply(const double a[3][3], const double b[3][3], double out[3][3]) {
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
		}
	}
}

Context composed(const Context& context, const Affine& affine) {
	Context out = context;
	multiply(context.linear, affine.linear, out.linear);
	for (int i = 0; i < 3; ++i) {
		const double* row = context.linear[i];
		out.origin[i]     = context.origin[i] + row[0] * affine.offset[0] + row[1] * affine.offset[1] + row[2] * affine.offset[2];
	}
	return out;
}

std::vector<double> vector3(const Value& value, const Location& location) {
	floats(value, 3, location);
	if (failed()) {
		return { 0, 0, 0 };
	}
	if (value.kind == ValueKind::Number) {
		return { value.number, value.number, value.number };
	}
	return { value.items[0].number, value.items[1].number, value.items[2].number };
}

std::vector<double> namedAxis(const Value& value, const Location& location) {
	const std::string& word = wordOf(value);
	const std::size_t index = std::string("xyz").find(word);
	if (word.size() != 1 || index == std::string::npos) {
		fail(location, "an axis is x, y, z or a vector");
		return { 0, 0, 1 };
	}
	std::vector<double> axis = { 0, 0, 0 };
	axis[index]              = 1;
	return axis;
}

std::vector<double> axisOf(const Value& value, const Location& location) {
	if (value.kind == ValueKind::Word) {
		return namedAxis(value, location);
	}
	const std::vector<double> axis = vector3(value, location);
	const double length            = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	if (length == 0) {
		fail(location, "an axis must not be zero");
		return { 0, 0, 1 };
	}
	return { axis[0] / length, axis[1] / length, axis[2] / length };
}

Affine translation(const std::vector<double>& t) {
	return Affine {
		.linear = {
			{ 1, 0, 0 },
			{ 0, 1, 0 },
			{ 0, 0, 1 },
		},

		.offset = { t[0], t[1], t[2] },
	};
}

Affine rotation(const std::vector<double>& k, double angle) {
	const double c = std::cos(angle);
	const double s = std::sin(angle);
	const double v = 1 - c;

	return Affine {
		.linear = {
			{ c + k[0] * k[0] * v, k[0] * k[1] * v - k[2] * s, k[0] * k[2] * v + k[1] * s },
			{ k[1] * k[0] * v + k[2] * s, c + k[1] * k[1] * v, k[1] * k[2] * v - k[0] * s },
			{ k[2] * k[0] * v - k[1] * s, k[2] * k[1] * v + k[0] * s, c + k[2] * k[2] * v },
		},

		.offset = { 0, 0, 0 },
	};
}

Context scaled(const Context& context, const std::vector<double>& s, const Location& location) {
	if (s[0] == 0 || s[1] == 0 || s[2] == 0) {
		fail(location, "a scale must not be zero");
		return context;
	}

	const Affine stretched = {
		.linear = {
			{ s[0], 0, 0 },
			{ 0, s[1], 0 },
			{ 0, 0, s[2] },
		},

		.offset = { 0, 0, 0 },
	};

	Context out = composed(context, stretched);
	out.stretch *= std::min({ std::abs(s[0]), std::abs(s[1]), std::abs(s[2]) });
	return out;
}

Context painted(const Context& context, const Call& call) {
	expectCount(call, 1);
	if (failed()) {
		return context;
	}
	const Value& material = call.arguments.positional[0];
	if (material.kind != ValueKind::Material) {
		fail(call.expression.location, "paint takes a material");
		return context;
	}
	Context out  = context;
	out.material = std::int32_t(material.material);
	return out;
}

Coat blankCoat() {
	return Coat {
		.mask    = CoatMask::Pattern,
		.effect  = CoatEffect::Tint,
		.axis    = { 0, 0, 1 },
		.level   = 0,
		.width   = 0,
		.soft    = 0,
		.pattern = colourAlbedo({}),

		.material = 0,
		.colour   = { 1, 1, 1 },
		.amount   = 0,
		.ragged   = 0,
		.grain    = 1,
		.relief   = Relief::Grain,
		.span     = 1,
		.along    = { 1, 0, 0 },
		.height   = colourAlbedo({}),
	};
}

bool isCallOf(const Expression& expression, const std::vector<std::string>& names) {
	return expression.kind == ExpressionKind::Call && std::find(names.begin(), names.end(), expression.text) != names.end();
}

float namedFloat(const Call& call, const std::string& name, float otherwise) {
	const auto found = call.arguments.named.find(name);
	return found == call.arguments.named.end() ? otherwise : floatOf(found->second, call.expression.location);
}

std::vector<float> unitFloats(const Value& value, const Location& location) {
	const std::vector<float> axis = floats(value, 3, location);
	const float size              = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
	if (!(size > 0)) {
		fail(location, "an axis has a length");
		return { 0, 0, 1 };
	}
	return { axis[0] / size, axis[1] / size, axis[2] / size };
}

std::vector<float> coatAxis(const Call& call, bool positionalAxis) {
	const auto found   = call.arguments.named.find("axis");
	const bool given   = positionalAxis && !call.arguments.positional.empty();
	const Value up     = listValue({ numberValue(0), numberValue(0), numberValue(1) });
	const Value& value = given ? call.arguments.positional[0] : (found == call.arguments.named.end() ? up : found->second);
	return unitFloats(value, call.expression.location);
}

std::vector<float> namedUnit(const Call& call, const std::string& name, const std::vector<float>& otherwise) {
	const auto found = call.arguments.named.find(name);
	return found == call.arguments.named.end() ? otherwise : unitFloats(found->second, call.expression.location);
}

void checkNamed(const Call& call, const std::vector<std::string>& known) {
	for (const auto& [name, value] : call.arguments.named) {
		if (std::find(known.begin(), known.end(), name) == known.end()) {
			fail(call.expression.location, call.expression.text + " has no field '" + name + "'");
		}
	}
}

CoatEffect effectOf(const std::string& name) {
	static const std::map<std::string, CoatEffect> effects = {
		{ "tint", CoatEffect::Tint },
		{ "shade", CoatEffect::Shade },
		{ "rough", CoatEffect::Rough },
		{ "polish", CoatEffect::Rough },
		{ "wet", CoatEffect::Wet },
		{ "glow", CoatEffect::Glow },
	};

	return effects.at(name);
}

Coat namedEffect(const Coat& base, const Call& call) {
	const std::string& name = call.expression.text;
	checkNamed(call, { "strength" });
	Coat coat   = base;
	coat.effect = effectOf(name);
	if (name == "tint" || name == "glow") {
		coat.colour = colourFromFloats(floats(positional(call, 0), 3, call.expression.location));
		coat.amount = namedFloat(call, "strength", 1);
		return coat;
	}
	const float amount = floatOf(positional(call, 0), call.expression.location);
	coat.amount        = name == "polish" ? -amount : amount;
	return coat;
}

Relief reliefOf(const Value& value, const Location& location) {
	static const std::map<std::string, Relief> reliefs = {
		{ "grain", Relief::Grain },
		{ "pits", Relief::Pits },
		{ "streak", Relief::Streak },
	};

	const auto found = reliefs.find(wordOf(value));
	if (value.kind != ValueKind::Word || found == reliefs.end()) {
		fail(location, "a bump's relief is grain, pits, streak or a surface");
		return Relief::Grain;
	}
	return found->second;
}

Coat bumpEffect(Evaluator& evaluator, const Coat& base, const Call& call) {
	checkNamed(call, { "span", "along" });
	const Value relief = positional(call, 0);
	const bool pattern = relief.kind == ValueKind::Pattern;
	Coat coat          = base;
	coat.effect        = CoatEffect::Bump;
	coat.relief        = pattern ? Relief::Pattern : reliefOf(relief, call.expression.location);
	coat.height        = pattern ? patternAlbedo(evaluator, *relief.pattern) : coat.height;
	coat.amount        = floatOf(positional(call, 1), call.expression.location);
	coat.span          = namedFloat(call, "span", 1);
	coat.along         = math::vectorFromFloats(namedUnit(call, "along", { 1, 0, 0 }));
	if (!pattern && !(coat.span > 0)) {
		fail(call.expression.location, "a bump's span is more than nothing");
	}
	return coat;
}

Coat withEffect(Evaluator& evaluator, const Coat& base, const Expression& expression) {
	if (isCallOf(expression, { "bump" })) {
		const Arguments arguments = evaluateArguments(evaluator, expression);

		const Call call = {
			.arguments  = arguments,
			.expression = expression,
		};

		return bumpEffect(evaluator, base, call);
	}
	if (isCallOf(expression, { "tint", "shade", "rough", "polish", "wet", "glow" })) {
		const Arguments arguments = evaluateArguments(evaluator, expression);

		const Call call = {
			.arguments  = arguments,
			.expression = expression,
		};

		return namedEffect(base, call);
	}
	const Value value = evaluateExpression(evaluator, expression);
	if (value.kind != ValueKind::Material) {
		fail(expression.location, "a coat does tint, shade, rough, polish, wet, glow or a material");
		return base;
	}
	Coat coat     = base;
	coat.effect   = CoatEffect::Paint;
	coat.material = value.material;
	return coat;
}

Albedo maskPattern(Evaluator& evaluator, const Value& value, const Location& location) {
	if (value.kind != ValueKind::Pattern) {
		fail(location, "a coat's mask reads a surface");
		return colourAlbedo({ 0, 0, 0 });
	}
	return patternAlbedo(evaluator, *value.pattern);
}

Coat raggedMask(const Coat& base, const Call& call) {
	Coat coat   = base;
	coat.ragged = namedFloat(call, "ragged", 0);
	coat.grain  = namedFloat(call, "grain", 1);
	return coat;
}

Coat levelMask(const Coat& base, const Call& call) {
	const bool facing = call.expression.text == "facing";
	const float sign  = call.expression.text == "below" ? -1.0f : 1.0f;
	checkNamed(call, { "soft", "axis", "level", "ragged", "grain" });
	const std::vector<float> axis = coatAxis(call, facing);
	Coat coat                     = raggedMask(base, call);
	coat.axis                     = math::vectorFromFloats(axis) * double(sign);
	coat.mask                     = facing ? CoatMask::Facing : CoatMask::Above;
	coat.level                    = facing ? namedFloat(call, "level", 0.5f) : sign * floatOf(positional(call, 0), call.expression.location);
	coat.soft                     = namedFloat(call, "soft", facing ? 0.2f : 0.0f);
	return coat;
}

Coat shapeMask(Evaluator& evaluator, const Coat& base, const Call& call) {
	const bool along = call.expression.text == "along";
	checkNamed(call, { "soft", "ragged", "grain" });
	Coat coat          = raggedMask(base, call);
	coat.mask          = along ? CoatMask::Along : CoatMask::Inside;
	coat.pattern       = maskPattern(evaluator, positional(call, 0), call.expression.location);
	coat.pattern.world = true;
	coat.width         = along ? floatOf(positional(call, 1), call.expression.location) : 0.0f;
	coat.soft          = namedFloat(call, "soft", 0);
	return coat;
}

Coat withMask(Evaluator& evaluator, const Coat& base, const Expression& expression) {
	if (isCallOf(expression, { "above", "below", "facing", "inside", "along" })) {
		const Arguments arguments = evaluateArguments(evaluator, expression);

		const Call call = {
			.arguments  = arguments,
			.expression = expression,
		};

		return isCallOf(expression, { "inside", "along" }) ? shapeMask(evaluator, base, call) : levelMask(base, call);
	}
	Coat coat    = base;
	coat.mask    = CoatMask::Pattern;
	coat.pattern = maskPattern(evaluator, evaluateExpression(evaluator, expression), expression.location);
	return coat;
}

constexpr const char* coatForms = "coat takes an effect and a mask, a bump alone, or a coat";

bool unnamedItems(const Expression& modifier) {
	return std::all_of(modifier.names.begin(), modifier.names.end(), [](const std::string& name) {
		return name.empty();
	});
}

Coat everywhere(const Coat& base) {
	Coat coat = base;
	coat.mask = CoatMask::Everywhere;
	return coat;
}

bool bareBump(const Expression& call) {
	return call.items.size() == 1 && isCallOf(call.items[0], { "bump" });
}

bool makesCoat(const Expression& call) {
	return call.items.size() != 1 || bareBump(call);
}

Coat madeCoat(Evaluator& evaluator, const Expression& call) {
	const Coat effect = withEffect(evaluator, blankCoat(), call.items[0]);
	return bareBump(call) ? everywhere(effect) : withMask(evaluator, effect, call.items[1]);
}

Value pushCoat(Evaluator& evaluator, const Coat& coat) {
	evaluator.looks.coats.push_back(coat);
	return Value {
		.kind     = ValueKind::Coat,
		.slot     = -1,
		.material = 0,
		.coat     = std::uint32_t(evaluator.looks.coats.size() - 1),
		.number   = 0,
		.items    = {},
		.fields   = nullptr,
		.pattern  = nullptr,
		.word     = nullptr,
		.network  = nullptr,
	};
}

Value newCoat(Evaluator& evaluator, const Expression& call) {
	if (evaluator.tiled) {
		fail(call.location, "tiles may only use coats made before them");
		return numberValue(0);
	}
	if ((call.items.size() != 2 && !bareBump(call)) || !unnamedItems(call)) {
		fail(call.location, coatForms);
		return numberValue(0);
	}
	return pushCoat(evaluator, madeCoat(evaluator, call));
}

Value coatValue(Evaluator& evaluator, const Expression& call) {
	if (makesCoat(call)) {
		return newCoat(evaluator, call);
	}
	const Value value = evaluateExpression(evaluator, call.items[0]);
	if (value.kind != ValueKind::Coat || !unnamedItems(call)) {
		fail(call.location, coatForms);
	}
	return value;
}

Context coated(Evaluator& evaluator, const Context& context, const Expression& modifier) {
	const Value coat = coatValue(evaluator, modifier);
	if (failed()) {
		return context;
	}
	Context out = context;
	out.coats.insert(out.coats.begin(), coat.coat);
	return out;
}

Context turned(const Context& context, const Call& call) {
	expectCount(call, 2);
	if (failed()) {
		return context;
	}
	const std::vector<double> axis = axisOf(call.arguments.positional[0], call.expression.location);
	return composed(context, rotation(axis, floatOf(call.arguments.positional[1], call.expression.location)));
}

std::uint64_t groupBit(Looks& looks, const std::string& name, const Location& location) {
	const auto found = std::find(looks.groups.begin(), looks.groups.end(), name);
	if (found != looks.groups.end()) {
		return std::uint64_t(1) << (found - looks.groups.begin());
	}
	if (looks.groups.size() >= mostGroups) {
		fail(location, "a scene names at most 64 groups");
		return 0;
	}
	looks.groups.push_back(name);
	return std::uint64_t(1) << (looks.groups.size() - 1);
}

std::uint8_t ruleBit(const std::string& name, const Location& location) {
	static const std::map<std::string, Rule> rules = {
		{ "camera", Rule::Hidden },
		{ "shadows", Rule::Shadowless },
		{ "caustics", Rule::Causticless },
	};

	if (!rules.contains(name)) {
		fail(location, "a rule is camera, shadows or caustics");
		return 0;
	}
	return std::uint8_t(rules.at(name));
}

Context ruled(const Context& context, const Call& call) {
	if (!call.arguments.positional.empty() || call.arguments.named.empty()) {
		fail(call.expression.location, "rule takes named switches: camera, shadows or caustics");
		return context;
	}
	Context out = context;
	for (const auto& [name, value] : call.arguments.named) {
		const std::uint8_t bit = ruleBit(name, call.expression.location);
		const bool refused     = floatOf(value, call.expression.location) == 0;
		out.rules              = refused ? std::uint8_t(out.rules | bit) : std::uint8_t(out.rules & ~bit);
	}
	return out;
}

Context grouped(Evaluator& evaluator, const Context& context, const Call& call) {
	expectCount(call, 1);
	if (failed()) {
		return context;
	}
	const Value& name = call.arguments.positional[0];
	if (name.kind != ValueKind::Word) {
		fail(call.expression.location, "group takes a name");
		return context;
	}
	Context out = context;
	out.groups |= groupBit(evaluator.looks, wordOf(name), call.expression.location);
	return out;
}

Context modified(Evaluator& evaluator, const Context& context, const Expression& modifier) {
	if (modifier.text == "coat") {
		return coated(evaluator, context, modifier);
	}
	const Arguments arguments = evaluateArguments(evaluator, modifier);

	const Call call = {
		.arguments  = arguments,
		.expression = modifier,
	};

	if (modifier.text == "turn") {
		return turned(context, call);
	}
	if (modifier.text == "paint") {
		return painted(context, call);
	}
	if (modifier.text == "rule") {
		refusedInTiles(evaluator, modifier.location, "say rules");
		return ruled(context, call);
	}
	if (modifier.text == "group") {
		refusedInTiles(evaluator, modifier.location, "put shapes in a group");
		return grouped(evaluator, context, call);
	}
	expectCount(call, 1);
	if (failed()) {
		return context;
	}
	const std::vector<double> v = vector3(arguments.positional[0], modifier.location);
	if (modifier.text == "scale") {
		return scaled(context, v, modifier.location);
	}
	return composed(context, translation(v));
}

double determinant(const double m[3][3]) {
	const double a = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]);
	const double b = m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]);
	const double c = m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	return a - b + c;
}

double inverseEntry(const double m[3][3], int i, int j) {
	const int firstRow     = (j + 1) % 3;
	const int secondRow    = (j + 2) % 3;
	const int firstColumn  = (i + 1) % 3;
	const int secondColumn = (i + 2) % 3;
	return (m[firstRow][firstColumn] * m[secondRow][secondColumn] - m[firstRow][secondColumn] * m[secondRow][firstColumn]) / determinant(m);
}

Placement placementOf(const Context& context) {
	Placement placement = {
		.linear = {
			{ 0, 0, 0 },
			{ 0, 0, 0 },
			{ 0, 0, 0 },
		},

		.origin  = { 0, 0, 0 },
		.stretch = context.stretch,
	};

	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			placement.linear[i][j] = inverseEntry(context.linear, i, j);
		}
		placement.origin[i] = context.origin[i];
	}
	return placement;
}

std::uint32_t defaultMaterial(Evaluator& evaluator) {
	if (evaluator.defaultMaterial < 0) {
		const Material grey = {
			.kind       = MaterialKind::Matte,
			.albedo     = colourAlbedo({ 0.8f, 0.8f, 0.8f }),
			.roughness  = 0.5f,
			.refraction = 1.5f,
			.dispersion = 0,
			.scatter    = 0,
			.lean       = 0,
			.absorb     = { 0, 0, 0 },
			.glow       = { 0, 0, 0 },
			.strength   = 0,
			.film       = { 0, 1 },
			.guided     = true,
			.relays     = true,
			.aimed      = true,
		};

		const Value pushed = pushMaterial(evaluator, grey);

		evaluator.defaultMaterial = std::int32_t(pushed.material);
	}
	return std::uint32_t(evaluator.defaultMaterial);
}

std::uint32_t pushNode(Scene& scene, const Node& node) {
	scene.nodes.push_back(node);
	return std::uint32_t(scene.nodes.size() - 1);
}

void sinkNode(Evaluator& evaluator, const Context& context, std::uint32_t node) {
	std::vector<std::uint32_t>& sink = context.sink ? *context.sink : evaluator.scene.objects;
	sink.push_back(node);
}

std::uint32_t materialOf(Evaluator& evaluator, const Context& context) {
	return context.material < 0 ? defaultMaterial(evaluator) : std::uint32_t(context.material);
}

void emitLeaf(Evaluator& evaluator, const Pattern& pattern, const Context& context) {
	if (!pattern.stops.empty() || pattern.world) {
		fail(pattern.location, "a ramp or world pattern is no shape");
		return;
	}

	const Leaf leaf = {
		.program   = specialise(evaluator, pattern),
		.arguments = std::uint32_t(evaluator.scene.arguments.size()),
		.placement = placementOf(context),
		.material  = materialOf(evaluator, context),
		.coats     = context.coats,
		.rules     = context.rules,
		.groups    = context.groups,
	};

	pushArguments(evaluator, pattern.arguments);
	evaluator.scene.leaves.push_back(leaf);
	const std::uint32_t index = std::uint32_t(evaluator.scene.leaves.size() - 1);

	const Node node = {
		.join     = Join::Leaf,
		.radius   = 0,
		.leaf     = index,
		.children = {},
	};

	sinkNode(evaluator, context, pushNode(evaluator.scene, node));
}

struct Group {
	Join join;
	double radius;
	const std::vector<Statement>& body;
	Location location;
};

std::vector<std::uint32_t> collectChildren(Evaluator& evaluator, const std::vector<Statement>& body, const Context& context) {
	std::vector<std::uint32_t> children;
	Context inner = context;
	inner.sink    = &children;
	openBlock(evaluator);
	runBlock(evaluator, body, inner);
	closeBlock(evaluator);
	return children;
}

void emitGroup(Evaluator& evaluator, const Group& group, const Context& context) {
	const std::vector<std::uint32_t> children = collectChildren(evaluator, group.body, context);
	if (children.empty()) {
		fail(group.location, "this emits no shape");
		return;
	}
	if (children.size() == 1 && group.join == Join::Merge) {
		sinkNode(evaluator, context, children[0]);
		return;
	}

	const Node joined = {
		.join     = group.join,
		.radius   = group.radius,
		.leaf     = 0,
		.children = children,
	};

	sinkNode(evaluator, context, pushNode(evaluator.scene, joined));
}

struct Fields {
	std::map<std::string, Value> values;
	const Statement& setting;
};

bool hasField(const Fields& fields, const std::string& name) {
	return fields.values.contains(name);
}

void readColour(const Fields& fields, const std::string& name, Colour& into) {
	if (hasField(fields, name)) {
		into = colourFromFloats(floats(fields.values.at(name), 3, fields.setting.location));
	}
}

void readVector(const Fields& fields, const std::string& name, math::Vector& into) {
	if (hasField(fields, name)) {
		into = math::vectorFromFloats(floats(fields.values.at(name), 3, fields.setting.location));
	}
}

void readFloat(const Fields& fields, const std::string& name, float& into) {
	if (hasField(fields, name)) {
		into = floatOf(fields.values.at(name), fields.setting.location);
	}
}

void readInt(const Fields& fields, const std::string& name, int& into) {
	if (hasField(fields, name)) {
		into = int(floatOf(fields.values.at(name), fields.setting.location));
	}
}

void checkFields(const Fields& fields, const std::vector<std::string>& known) {
	for (const auto& [name, value] : fields.values) {
		if (std::find(known.begin(), known.end(), name) == known.end()) {
			fail(fields.setting.location, fields.setting.text + " has no field '" + name + "'");
		}
	}
}

bool splitNamed(int split) {
	return split == 0 || split == 1 || split == 2 || split == 4 || split == 8;
}

void readRender(Shot& shot, const Fields& fields) {
	checkFields(fields, { "width", "height", "tolerance", "margin", "seed", "far", "samples", "batch", "split", "surfaces", "collisions" });
	readInt(fields, "width", shot.render.width);
	readInt(fields, "height", shot.render.height);
	readFloat(fields, "tolerance", shot.render.tolerance);
	readFloat(fields, "margin", shot.render.margin);
	readInt(fields, "seed", shot.render.seed);
	readFloat(fields, "far", shot.render.far);
	readInt(fields, "samples", shot.render.samples);
	readInt(fields, "batch", shot.render.batch);
	readInt(fields, "split", shot.render.split);
	readInt(fields, "surfaces", shot.render.surfaces);
	readInt(fields, "collisions", shot.render.collisions);
	if (!(shot.render.margin > 0)) {
		fail(fields.setting.location, "a render's margin must be above zero");
	}
	if (shot.render.surfaces < 1) {
		fail(fields.setting.location, "a render's surfaces must be at least one");
	}
	if (shot.render.collisions < 1) {
		fail(fields.setting.location, "a render's collisions must be at least one");
	}
	if (!splitNamed(shot.render.split)) {
		fail(fields.setting.location, "a render's split must be 0, 1, 2, 4 or 8");
	}
}

void readCamera(Shot& shot, const Fields& fields) {
	checkFields(fields, { "eye", "target", "up", "fov", "aperture", "focus" });
	readVector(fields, "eye", shot.camera.eye);
	readVector(fields, "target", shot.camera.target);
	readVector(fields, "up", shot.camera.up);
	readFloat(fields, "fov", shot.camera.fov);
	readFloat(fields, "aperture", shot.camera.aperture);
	readFloat(fields, "focus", shot.camera.focus);
}

std::vector<double> clipCuts(const Fields& fields, double duration) {
	const Value& cuts = fields.values.at("cuts");
	if (cuts.kind != ValueKind::List) {
		fail(fields.setting.location, "cuts is a list of times");
		return {};
	}
	std::vector<double> times;
	for (const Value& cut : cuts.items) {
		times.push_back(knownOf(cut, fields.setting.location));
		const bool rising = times.size() < 2 || times.back() > times[times.size() - 2];
		if (!rising || !(times.back() > 0 && times.back() < duration)) {
			fail(fields.setting.location, "a clip's cuts rise and lie inside it");
			return {};
		}
	}
	return times;
}

void readClip(Shot& shot, const Fields& fields) {
	checkFields(fields, { "duration", "rate", "cuts" });
	if (!hasField(fields, "duration")) {
		fail(fields.setting.location, "a clip has a duration");
		return;
	}
	shot.clip.duration = knownOf(fields.values.at("duration"), fields.setting.location);
	shot.clip.rate     = hasField(fields, "rate") ? knownOf(fields.values.at("rate"), fields.setting.location) : shot.clip.rate;
	if (!(shot.clip.duration > 0) || !(shot.clip.rate > 0)) {
		fail(fields.setting.location, "a clip's duration and rate are above 0");
		return;
	}
	shot.clip.cuts = hasField(fields, "cuts") ? clipCuts(fields, shot.clip.duration) : std::vector<double> {};
}

Curve curveOf(const Fields& fields) {
	const std::string& curve = wordOf(fields.values.at("curve"));
	if (curve != "filmic" && curve != "clamped") {
		fail(fields.setting.location, "a curve is filmic or clamped");
	}
	return curve == "filmic" ? Curve::Filmic : Curve::Clamped;
}

void readGrade(Shot& shot, const Fields& fields) {
	checkFields(fields, { "exposure", "curve" });
	readFloat(fields, "exposure", shot.grade.exposure);
	if (hasField(fields, "curve")) {
		shot.grade.curve = curveOf(fields);
	}
}

SkyKind skyKind(const Fields& fields) {
	static const std::map<std::string, SkyKind> kinds = {
		{ "gradient", SkyKind::Gradient },
		{ "physical", SkyKind::Physical },
		{ "painted", SkyKind::Painted },
	};

	if (!kinds.contains(fields.setting.word)) {
		fail(fields.setting.location, "a sky is gradient, physical or painted");
		return SkyKind::Gradient;
	}
	return kinds.at(fields.setting.word);
}

SkyStop skyStopOf(const Value& value, const Location& location) {
	const std::vector<float> numbers = floats(value, 4, location);
	if (numbers[0] < -1 || numbers[0] > 1) {
		fail(location, "a stop's height is from -1 to 1");
	}
	return SkyStop {
		.height = numbers[0],
		.colour = { numbers[1], numbers[2], numbers[3] },
	};
}

void readStops(const Fields& fields, const std::string& name, std::vector<SkyStop>& into) {
	if (!hasField(fields, name)) {
		return;
	}
	const Value& run = fields.values.at(name);
	if (run.kind != ValueKind::List || run.items.size() < 2) {
		fail(fields.setting.location, "a run of a painted sky is two stops or more");
		return;
	}
	into.clear();
	for (const Value& item : run.items) {
		into.push_back(skyStopOf(item, fields.setting.location));
		if (into.size() > 1 && !(into.back().height > into[into.size() - 2].height)) {
			fail(fields.setting.location, "a painted sky's stops rise from one to the next");
			return;
		}
	}
}

void readPainted(Lighting& lighting, const Fields& fields) {
	checkFields(fields, { "day", "dusk", "night", "glow", "strength", "spread" });
	readStops(fields, "day", lighting.sky.painted.day);
	readStops(fields, "dusk", lighting.sky.painted.dusk);
	readStops(fields, "night", lighting.sky.painted.night);
}

void readSkyGlow(Lighting& lighting, const Fields& fields) {
	readColour(fields, "glow", lighting.sky.glow);
	readFloat(fields, "strength", lighting.sky.strength);
	readFloat(fields, "spread", lighting.sky.spread);
}

void readSky(Lighting& lighting, const Fields& fields) {
	lighting.sky.kind = skyKind(fields);
	readSkyGlow(lighting, fields);
	if (lighting.sky.kind == SkyKind::Painted) {
		readPainted(lighting, fields);
		return;
	}
	checkFields(fields, { "zenith", "horizon", "ground", "haze", "glow", "strength", "spread" });
	readColour(fields, "zenith", lighting.sky.zenith);
	readColour(fields, "horizon", lighting.sky.horizon);
	readColour(fields, "ground", lighting.sky.ground);
	readFloat(fields, "haze", lighting.sky.haze);
}

void readClouds(Lighting& lighting, const Fields& fields) {
	checkFields(fields, { "height", "thickness", "cover", "density", "span" });
	lighting.clouds.present = true;
	readFloat(fields, "height", lighting.clouds.height);
	readFloat(fields, "thickness", lighting.clouds.thickness);
	readFloat(fields, "cover", lighting.clouds.cover);
	readFloat(fields, "density", lighting.clouds.density);
	readFloat(fields, "span", lighting.clouds.span);
}

void readStars(Lighting& lighting, const Fields& fields) {
	checkFields(fields, { "count", "strength", "angle", "axis" });
	lighting.stars.present = true;
	readFloat(fields, "count", lighting.stars.count);
	readFloat(fields, "strength", lighting.stars.strength);
	readFloat(fields, "angle", lighting.stars.angle);
	readVector(fields, "axis", lighting.stars.axis);
	if (!(lighting.stars.count >= 1)) {
		fail(fields.setting.location, "stars number one or more");
	}
}

std::uint64_t reachedGroups(Looks& looks, const Fields& fields) {
	const Value& names = fields.values.at("reaches");
	if (names.kind != ValueKind::List) {
		fail(fields.setting.location, "reaches is a list of group names");
		return 0;
	}
	std::uint64_t reached = 0;
	for (const Value& name : names.items) {
		if (name.kind != ValueKind::Word) {
			fail(fields.setting.location, "reaches is a list of group names");
			return 0;
		}
		reached |= groupBit(looks, wordOf(name), fields.setting.location);
	}
	return reached;
}

LightKind lightKind(const Fields& fields) {
	static const std::map<std::string, LightKind> kinds = {
		{ "point", LightKind::Point },
		{ "directional", LightKind::Directional },
		{ "sphere", LightKind::Sphere },
		{ "quad", LightKind::Quad },
		{ "sun", LightKind::Sun },
		{ "moon", LightKind::Moon },
	};

	if (!kinds.contains(fields.setting.word)) {
		fail(fields.setting.location, "a light is point, directional, sphere, quad, sun or moon");
		return LightKind::Point;
	}
	return kinds.at(fields.setting.word);
}

void readLight(Lighting& lighting, Looks& looks, const Fields& fields) {
	checkFields(fields, { "at", "direction", "along", "across", "radius", "angle", "power", "reaches", "caustics" });

	Light light = {
		.kind      = lightKind(fields),
		.at        = { 0, 0, 0 },
		.direction = { 0, 0, 1 },
		.along     = { 1, 0, 0 },
		.across    = { 0, 1, 0 },
		.radius    = 0,
		.angle     = 0.0093f,
		.power     = { 1, 1, 1 },
		.reaches   = 0,
		.caustics  = true,
	};

	readVector(fields, "at", light.at);
	readVector(fields, "direction", light.direction);
	readVector(fields, "along", light.along);
	readVector(fields, "across", light.across);
	readFloat(fields, "radius", light.radius);
	readFloat(fields, "angle", light.angle);
	readColour(fields, "power", light.power);
	light.reaches  = hasField(fields, "reaches") ? reachedGroups(looks, fields) : 0;
	light.caustics = !hasField(fields, "caustics") || floatOf(fields.values.at("caustics"), fields.setting.location) != 0;
	lighting.lights.push_back(light);
}

Fields evaluateFields(Evaluator& evaluator, const Statement& setting) {
	Fields fields = {
		.values  = {},
		.setting = setting,
	};

	for (std::size_t i = 0; i < setting.names.size(); ++i) {
		fields.values[setting.names[i]] = evaluateExpression(evaluator, setting.expressions[i]);
	}
	return fields;
}

void readShotSetting(Shot& shot, const std::string& name, const Fields& fields) {
	using Reader = void (*)(Shot&, const Fields&);

	static const std::map<std::string, Reader> readers = {
		{ "render", readRender },
		{ "camera", readCamera },
		{ "grade", readGrade },
		{ "clip", readClip },
	};

	readers.at(name)(shot, fields);
}

void readLightingSetting(Lighting& lighting, const std::string& name, const Fields& fields) {
	using Reader = void (*)(Lighting&, const Fields&);

	static const std::map<std::string, Reader> readers = {
		{ "sky", readSky },
		{ "clouds", readClouds },
		{ "stars", readStars },
	};

	readers.at(name)(lighting, fields);
}

bool shotSetting(const std::string& name) {
	return name == "render" || name == "camera" || name == "grade" || name == "clip";
}

void runSetting(Evaluator& evaluator, const Statement& setting) {
	const Fields fields = evaluateFields(evaluator, setting);
	if (setting.text == "light") {
		readLight(evaluator.lighting, evaluator.looks, fields);
		return;
	}
	if (setting.text == "camera") {
		readShotSetting(evaluator.shot, setting.text, fields);
		evaluator.eye = evaluator.shot.camera.eye;
		return;
	}
	if (shotSetting(setting.text)) {
		readShotSetting(evaluator.shot, setting.text, fields);
		return;
	}
	readLightingSetting(evaluator.lighting, setting.text, fields);
}

void runFile(Evaluator& evaluator, const std::string& path, const Location& location);

std::string directoryOf(const std::string& path) {
	const std::size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

std::string importedPath(const std::string& from, const std::string& path) {
	return path.starts_with('/') ? path : directoryOf(from) + path;
}

struct LoopLevel {
	const Statement& loop;
	std::size_t level;
};

struct LoopItem {
	const Statement& loop;
	std::size_t level;
	const Value& value;
};

void runLoopLevel(Evaluator& evaluator, const LoopLevel& at, const Context& context);

void runLoopItem(Evaluator& evaluator, const LoopItem& item, const Context& context) {
	openBlock(evaluator);
	bindLocal(evaluator, item.loop.names[item.level], item.value);
	if (item.level + 1 == item.loop.names.size()) {
		runBlock(evaluator, item.loop.body, context);
	}
	if (item.level + 1 < item.loop.names.size()) {
		const LoopLevel level = {
			.loop  = item.loop,
			.level = item.level + 1,
		};

		runLoopLevel(evaluator, level, context);
	}
	closeBlock(evaluator);
}

std::vector<Value> rangeValues(Evaluator& evaluator, const Expression& range) {
	const Value from = evaluateExpression(evaluator, range.items[0]);
	const Value to   = evaluateExpression(evaluator, range.items[1]);
	if (!isKnown(from) || !isKnown(to)) {
		fail(range.location, "a loop's bounds must be known when the scene loads");
		return {};
	}
	std::vector<Value> values;
	for (double i = from.number; i < to.number; i += 1) {
		values.push_back(numberValue(i));
	}
	return values;
}

std::vector<Value> loopValues(Evaluator& evaluator, const Expression& source) {
	if (source.kind == ExpressionKind::Binary && source.text == "..") {
		return rangeValues(evaluator, source);
	}
	Value list = evaluateExpression(evaluator, source);
	if (list.kind != ValueKind::List) {
		fail(source.location, "a loop runs over a range or a list");
		return {};
	}
	return std::move(list.items);
}

struct ComprehensionLevel {
	const Expression& comprehension;
	std::size_t level;
};

bool kept(Evaluator& evaluator, const Expression& comprehension) {
	if (comprehension.text != "if") {
		return true;
	}
	const Value condition = evaluateExpression(evaluator, comprehension.items.back());
	if (condition.kind != ValueKind::Number || !isKnown(condition)) {
		fail(comprehension.items.back().location, "a comprehension's condition must be known when the scene loads");
		return false;
	}
	return condition.number != 0;
}

std::vector<Value> comprehendLevel(Evaluator& evaluator, const ComprehensionLevel& at);

std::vector<Value> innerElements(Evaluator& evaluator, const ComprehensionLevel& at) {
	if (at.level + 1 < at.comprehension.names.size()) {
		const ComprehensionLevel next = {
			.comprehension = at.comprehension,
			.level         = at.level + 1,
		};

		return comprehendLevel(evaluator, next);
	}
	if (!kept(evaluator, at.comprehension)) {
		return {};
	}
	std::vector<Value> element;
	element.push_back(evaluateExpression(evaluator, at.comprehension.items[0]));
	return element;
}

std::vector<Value> comprehendLevel(Evaluator& evaluator, const ComprehensionLevel& at) {
	std::vector<Value> elements;
	for (Value& value : loopValues(evaluator, at.comprehension.items[at.level + 1])) {
		openBlock(evaluator);
		bindLocal(evaluator, at.comprehension.names[at.level], std::move(value));
		std::vector<Value> inner = innerElements(evaluator, at);
		closeBlock(evaluator);
		elements.insert(elements.end(), std::make_move_iterator(inner.begin()), std::make_move_iterator(inner.end()));
	}
	return elements;
}

Value evaluateComprehension(Evaluator& evaluator, const Expression& comprehension) {
	const ComprehensionLevel first = {
		.comprehension = comprehension,
		.level         = 0,
	};

	return checkedList(comprehendLevel(evaluator, first), comprehension.location);
}

void runLoopLevel(Evaluator& evaluator, const LoopLevel& at, const Context& context) {
	for (const Value& value : loopValues(evaluator, at.loop.expressions[at.level])) {
		if (failed() || evaluator.ended || evaluator.returned) {
			return;
		}

		const LoopItem item = {
			.loop  = at.loop,
			.level = at.level,
			.value = value,
		};

		runLoopItem(evaluator, item, context);
	}
}

void runLoop(Evaluator& evaluator, const Statement& loop, const Context& context) {
	const LoopLevel first = {
		.loop  = loop,
		.level = 0,
	};

	runLoopLevel(evaluator, first, context);
}

void runScoped(Evaluator& evaluator, const std::vector<Statement>& body, const Context& context) {
	openBlock(evaluator);
	runBlock(evaluator, body, context);
	closeBlock(evaluator);
}

const Value* valueBefore(const Evaluator& evaluator, const Scope& before, const std::string& name) {
	const auto old = before.find(name);
	if (old != before.end()) {
		return &old->second;
	}
	return sharedName(evaluator, name);
}

struct Bindings {
	Scope top;
	std::vector<Binding> locals;
};

void mergeValue(Evaluator& evaluator, const Operation& select, const Value& old, Value& after) {
	if (sameValue(old, after)) {
		return;
	}

	const Operation operands = {
		.opcode   = Opcode::Select,
		.a        = select.a,
		.b        = after,
		.c        = old,
		.location = select.location,
	};

	after = selectValue(evaluator, operands);
}

void mergeBranch(Evaluator& evaluator, const Bindings& before, const Operation& select) {
	for (auto& [name, after] : evaluator.top) {
		if (const Value* old = valueBefore(evaluator, before.top, name)) {
			mergeValue(evaluator, select, *old, after);
		}
	}
	for (std::size_t i = 0; i < before.locals.size(); ++i) {
		mergeValue(evaluator, select, before.locals[i].value, evaluator.locals[i].value);
	}
}

void runUnknownIf(Evaluator& evaluator, const Statement& branch, const Value& condition) {
	const Bindings before = {
		.top    = evaluator.top,
		.locals = evaluator.locals,
	};

	runScoped(evaluator, branch.body, rootContext());
	if (evaluator.returned) {
		fail(branch.location, "a return inside an if not known when the scene loads; use select");
	}

	const Operation operands = {
		.opcode   = Opcode::Select,
		.a        = condition,
		.b        = zeroValue(),
		.c        = zeroValue(),
		.location = branch.location,
	};

	mergeBranch(evaluator, before, operands);
}

void runIf(Evaluator& evaluator, const Statement& branch, const Context& context) {
	const Value condition = evaluateExpression(evaluator, branch.expressions[0]);
	expectNumber(condition, branch.location);
	if (failed()) {
		return;
	}
	if (!isKnown(condition)) {
		runUnknownIf(evaluator, branch, condition);
		return;
	}
	if (condition.number != 0) {
		runScoped(evaluator, branch.body, context);
	}
}

void expectBuilding(const Evaluator& evaluator, const Statement& statement) {
	if (evaluator.compiler || evaluator.isolated) {
		fail(statement.location, "a surface or a function only computes; shapes are emitted in a shape or at the top");
	}
}

const Statement* findShape(const Evaluator& evaluator, const Expression& call) {
	const auto found   = evaluator.definitions.find(call.text);
	const bool isShape = found != evaluator.definitions.end() && found->second->kind == StatementKind::Shape;
	return isShape ? found->second : nullptr;
}

struct ShapeCall {
	const Statement& shape;
	const Expression& call;
};

void runShape(Evaluator& evaluator, const ShapeCall& shapeCall, const Context& context);

void printValue(std::ostringstream& out, const Value& value);

void printItems(std::ostringstream& out, const Value& value) {
	for (std::size_t i = 0; i < value.items.size(); ++i) {
		out << (i > 0 ? ", " : "");
		if (value.kind == ValueKind::Record) {
			out << (*value.fields)[i] << " = ";
		}
		printValue(out, value.items[i]);
	}
}

void printValue(std::ostringstream& out, const Value& value) {
	const bool record = value.kind == ValueKind::Record;
	if (value.kind == ValueKind::Number) {
		out << value.number;
		return;
	}
	if (value.kind == ValueKind::Word) {
		out << wordOf(value);
		return;
	}
	if (value.kind != ValueKind::List && !record) {
		out << '(' << kindName(value.kind) << ')';
		return;
	}
	out << (record ? '{' : '[');
	printItems(out, value);
	out << (record ? '}' : ']');
}

void runPrint(Evaluator& evaluator, const Expression& call) {
	if (evaluator.compiler) {
		fail(call.location, "a surface does not print");
		return;
	}
	const Arguments arguments = evaluateArguments(evaluator, call);
	if (!arguments.named.empty()) {
		fail(call.location, "print takes values, not named ones");
	}
	if (failed() || !evaluator.printed.insert(&call).second) {
		return;
	}
	std::ostringstream out;
	out << *call.location.file << ':' << call.location.line << ':';
	for (const Value& value : arguments.positional) {
		out << ' ';
		printValue(out, value);
	}
	std::fprintf(stderr, "%s\n", out.str().c_str());
}

void runCall(Evaluator& evaluator, const Statement& statement, const Context& context) {
	if (statement.expressions[0].builtin == Builtin::Print) {
		runPrint(evaluator, statement.expressions[0]);
		return;
	}
	expectBuilding(evaluator, statement);
	if (failed()) {
		return;
	}
	const Expression& call = statement.expressions[0];
	if (const Statement* shape = findShape(evaluator, call)) {
		const ShapeCall shapeCall = {
			.shape = *shape,
			.call  = call,
		};

		runShape(evaluator, shapeCall, context);
		return;
	}
	const Value value = evaluateExpression(evaluator, call);
	if (failed()) {
		return;
	}
	if (value.kind != ValueKind::Pattern) {
		fail(call.location, "only a surface or a shape can stand as a statement");
		return;
	}
	emitLeaf(evaluator, *value.pattern, context);
}

void runShape(Evaluator& evaluator, const ShapeCall& shapeCall, const Context& context) {
	Arguments arguments = evaluateArguments(evaluator, shapeCall.call);
	std::vector<std::uint32_t> children;
	Context inner = context;
	inner.sink    = &children;

	const Invocation invocation = {
		.definition = shapeCall.shape,
		.arguments  = arguments.positional,
		.context    = inner,
		.location   = shapeCall.call.location,
	};

	runDefinition(evaluator, invocation);
	evaluator.ended = false;
	if (children.empty()) {
		return;
	}

	const Node merged = {
		.join     = Join::Merge,
		.radius   = 0,
		.leaf     = 0,
		.children = children,
	};

	const std::uint32_t node = children.size() == 1 ? children[0] : pushNode(evaluator.scene, merged);

	sinkNode(evaluator, context, node);
}

Context modifiedBy(Evaluator& evaluator, const std::vector<Expression>& modifiers, const Context& context) {
	Context inner = context;
	for (const Expression& modifier : modifiers) {
		inner = modified(evaluator, inner, modifier);
	}
	return inner;
}

void runPlace(Evaluator& evaluator, const Statement& place, const Context& context) {
	expectBuilding(evaluator, place);
	if (failed()) {
		return;
	}
	const Context inner = modifiedBy(evaluator, place.expressions, context);
	if (!context.sink) {
		runScoped(evaluator, place.body, inner);
		return;
	}

	const Group group = {
		.join     = Join::Merge,
		.radius   = 0,
		.body     = place.body,
		.location = place.location,
	};

	emitGroup(evaluator, group, inner);
}

Join joinKind(const std::string& name) {
	return name == "merge" ? Join::Merge : (name == "cut" ? Join::Cut : Join::Clip);
}

int repeatCount(double count, const Location& location) {
	if (!(count >= 1) || count != std::floor(count)) {
		fail(location, "a repeat count is a whole number of one or more");
		return 1;
	}
	return int(count);
}

Value namedArgument(const Arguments& arguments, const std::string& name, const Location& location) {
	if (!arguments.positional.empty() || arguments.named.size() != 2 || !arguments.named.contains(name)) {
		fail(location, "repeat takes count and step");
		return numberValue(1);
	}
	return arguments.named.at(name);
}

RepeatCall repeatCallOf(Evaluator& evaluator, const Expression& call, const Context& context) {
	const Arguments arguments        = evaluateArguments(evaluator, call);
	const std::vector<double> counts = vector3(namedArgument(arguments, "count", call.location), call.location);
	const std::vector<double> step   = vector3(namedArgument(arguments, "step", call.location), call.location);

	RepeatCall repeat = {
		.counts   = { 1, 1, 1 },
		.steps    = {},
		.location = call.location,
	};

	for (int axis = 0; axis < 3; ++axis) {
		repeat.counts[axis] = repeatCount(counts[std::size_t(axis)], call.location);
		for (int row = 0; row < 3; ++row) {
			repeat.steps[axis][row] = context.linear[row][axis] * step[std::size_t(axis)];
		}
	}
	return repeat;
}

std::uint32_t copiedNode(Evaluator& evaluator, std::uint32_t index, const double offset[3]) {
	Scene& scene    = evaluator.scene;
	const Node node = scene.nodes[index];
	if (node.join != Join::Leaf) {
		Node copy     = node;
		copy.children = {};
		for (const std::uint32_t child : node.children) {
			copy.children.push_back(copiedNode(evaluator, child, offset));
		}
		return pushNode(scene, copy);
	}
	Leaf leaf = scene.leaves[node.leaf];
	for (int i = 0; i < 3; ++i) {
		leaf.placement.origin[i] += offset[i];
	}
	scene.leaves.push_back(leaf);
	Node copy = node;
	copy.leaf = std::uint32_t(scene.leaves.size() - 1);
	return pushNode(scene, copy);
}

void copyOffset(const RepeatCall& call, const int at[3], double offset[3]) {
	for (int row = 0; row < 3; ++row) {
		offset[row] = call.steps[0][row] * at[0] + call.steps[1][row] * at[1] + call.steps[2][row] * at[2];
	}
}

std::vector<std::uint32_t> repeatedCopies(Evaluator& evaluator, const RepeatCall& call, const std::vector<std::uint32_t>& first) {
	std::vector<std::uint32_t> copies = first;
	for (int k = 0; k < call.counts[2]; ++k) {
		for (int j = 0; j < call.counts[1]; ++j) {
			for (int i = 0; i < call.counts[0]; ++i) {
				if (i == 0 && j == 0 && k == 0) {
					continue;
				}
				const int at[3] = { i, j, k };
				double offset[3];
				copyOffset(call, at, offset);
				for (const std::uint32_t node : first) {
					copies.push_back(copiedNode(evaluator, node, offset));
				}
			}
		}
	}
	return copies;
}

void sinkMerged(Evaluator& evaluator, const Context& context, const std::vector<std::uint32_t>& nodes) {
	if (!context.sink || nodes.size() == 1) {
		for (const std::uint32_t node : nodes) {
			sinkNode(evaluator, context, node);
		}
		return;
	}

	const Node merged = {
		.join     = Join::Merge,
		.radius   = 0,
		.leaf     = 0,
		.children = nodes,
	};

	sinkNode(evaluator, context, pushNode(evaluator.scene, merged));
}

void emitRepeated(Evaluator& evaluator, const Repetition& repetition) {
	const std::vector<std::uint32_t> first = collectChildren(evaluator, repetition.block, repetition.context);
	if (first.empty()) {
		fail(repetition.call.location, "this emits no shape");
		return;
	}
	sinkMerged(evaluator, repetition.context, repeatedCopies(evaluator, repetition.call, first));
}

void runRepeat(Evaluator& evaluator, const Statement& repeat, const Context& context) {
	const RepeatCall call = repeatCallOf(evaluator, repeat.expressions[0], context);

	const Repetition repetition = {
		.call    = call,
		.block   = repeat.body,
		.context = context,
	};

	emitRepeated(evaluator, repetition);
}

void runJoin(Evaluator& evaluator, const Statement& join, const Context& context) {
	expectBuilding(evaluator, join);
	if (failed()) {
		return;
	}
	if (join.text == "repeat") {
		runRepeat(evaluator, join, context);
		return;
	}
	const double radius = join.expressions.empty() ? 0 : floatOf(evaluateExpression(evaluator, join.expressions[0]), join.location);

	const Group group = {
		.join     = joinKind(join.text),
		.radius   = radius,
		.body     = join.body,
		.location = join.location,
	};

	emitGroup(evaluator, group, context);
}

bool isBuiltin(const std::string& name) {
	return builtinOf(name) != Builtin::None || name == "coat" || name == "pi";
}

void define(Evaluator& evaluator, const Statement& statement) {
	if (isBuiltin(statement.text)) {
		fail(statement.location, statement.text + " is a built-in and cannot be defined");
		return;
	}
	if (!evaluator.blocks.empty()) {
		fail(statement.location, "surface, shape and function are defined at the top of a file");
		return;
	}
	if (evaluator.definitions.contains(statement.text)) {
		fail(statement.location, statement.text + " is defined twice");
		return;
	}
	evaluator.definitions[statement.text] = &statement;
}

bool computing(const Evaluator& evaluator) {
	return evaluator.compiler || evaluator.isolated;
}

bool building(const Evaluator& evaluator, const Context& context) {
	return !computing(evaluator) && (evaluator.tiled || context.sink);
}

std::string returnRefusal(const Evaluator& evaluator, const Statement& statement, const Context& context) {
	const bool bare = statement.expressions.empty();
	if (computing(evaluator)) {
		return bare ? "a surface or a function must return a value" : "";
	}
	if (building(evaluator, context)) {
		return bare ? "" : "a shape or a tile body returns no value";
	}
	return "return belongs in a surface, a function, a shape or a tile body";
}

void runReturn(Evaluator& evaluator, const Statement& statement, const Context& context) {
	const std::string refused = returnRefusal(evaluator, statement, context);
	if (!refused.empty()) {
		fail(statement.location, refused);
		return;
	}
	if (statement.expressions.empty()) {
		evaluator.ended = true;
		return;
	}
	evaluator.result   = evaluateExpression(evaluator, statement.expressions[0]);
	evaluator.returned = true;
}

std::string letRefusal(const Evaluator& evaluator, const std::string& name) {
	if (name == "time") {
		return "time belongs to the clip and cannot be changed";
	}
	const bool top = evaluator.blocks.empty();
	if (top && isBuiltin(name)) {
		return name + " is a built-in and cannot be bound";
	}
	return top && tileBinding(name) ? name + " belongs to the tiles and may not be bound at the top level" : "";
}

bool growsItself(const Statement& statement) {
	const Expression& call = statement.expressions[0];
	if (call.kind != ExpressionKind::Call || call.items.empty() || call.items[0].kind != ExpressionKind::Name) {
		return false;
	}
	return (call.text == "append" || call.text == "join") && call.items[0].text == statement.text && unnamedItems(call);
}

std::vector<Value> addedArguments(Evaluator& evaluator, const Expression& call) {
	std::vector<Value> added;
	for (std::size_t i = 1; i < call.items.size(); ++i) {
		added.push_back(evaluateExpression(evaluator, call.items[i]));
	}
	return added;
}

Value takenList(Evaluator& evaluator, const Expression& name) {
	Value* bound = boundPlace(evaluator, name.text);
	if (bound && bound == namedPlace(evaluator, name.text) && bound->kind == ValueKind::List) {
		return std::move(*bound);
	}
	return evaluateExpression(evaluator, name);
}

Value grownItself(Evaluator& evaluator, const Expression& call) {
	const std::vector<Value> added = addedArguments(evaluator, call);
	if (failed()) {
		return numberValue(0);
	}
	return grownList(call, takenList(evaluator, call.items[0]), added);
}

Value letValue(Evaluator& evaluator, const Statement& statement) {
	if (growsItself(statement)) {
		return grownItself(evaluator, statement.expressions[0]);
	}
	return evaluateExpression(evaluator, statement.expressions[0]);
}

const Value* reboundPlace(Evaluator& evaluator, const std::string& name) {
	if (const Value* found = boundPlace(evaluator, name)) {
		return found;
	}
	return bindsLocally(evaluator) ? nullptr : sharedName(evaluator, name);
}

void checkKind(const Statement& statement, std::optional<ValueKind> before, const Value& value) {
	if (before && *before != value.kind && !failed()) {
		fail(statement.location, statement.text + " holds " + kindName(*before) + " and cannot be bound to " + kindName(value.kind));
	}
}

void runLet(Evaluator& evaluator, const Statement& statement) {
	const std::string refused = letRefusal(evaluator, statement.text);
	if (!refused.empty()) {
		fail(statement.location, refused);
		return;
	}
	const Value* old                      = reboundPlace(evaluator, statement.text);
	const std::optional<ValueKind> before = old ? std::optional(old->kind) : std::nullopt;
	Value value                           = letValue(evaluator, statement);
	checkKind(statement, before, value);
	if (!failed()) {
		bind(evaluator, statement.text, std::move(value));
	}
}

void runSettingStatement(Evaluator& evaluator, const Statement& statement) {
	expectBuilding(evaluator, statement);
	refusedInTiles(evaluator, statement.location, "hold settings");
	if (failed()) {
		return;
	}
	runSetting(evaluator, statement);
}

std::uint64_t textKey(std::uint64_t key, const std::string& text) {
	return mixedKey(key, std::hash<std::string> {}(text));
}

const Expression* materialCall(const Expression& expression) {
	if (expression.kind == ExpressionKind::Call && makesMaterial(expression.builtin)) {
		return &expression;
	}
	for (const Expression& item : expression.items) {
		if (const Expression* found = materialCall(item)) {
			return found;
		}
	}
	return nullptr;
}

std::string statementRefusal(const Statement& statement) {
	switch (statement.kind) {
		case StatementKind::Import: return "import";
		case StatementKind::Function:
		case StatementKind::Surface:
		case StatementKind::Shape: return "define a function, a surface or a shape";
		case StatementKind::Setting: return "hold settings";
		case StatementKind::Tiles: return "hold tiles";
		default: break;
	}

	return {};
}

std::string modifierRefusal(const Statement& statement, const Expression& modifier) {
	if (statement.kind != StatementKind::Place) {
		return {};
	}
	if (modifier.text == "rule") {
		return "say rules";
	}
	return modifier.text == "group" ? "put shapes in a group" : "";
}

const Expression* coatCall(const Expression& expression) {
	if (isCallOf(expression, { "coat" }) && makesCoat(expression)) {
		return &expression;
	}
	for (const Expression& item : expression.items) {
		if (const Expression* found = coatCall(item)) {
			return found;
		}
	}
	return nullptr;
}

void checkTileBody(const std::vector<Statement>& body);

void checkTileExpression(const Statement& statement, const Expression& expression) {
	const std::string modifier = modifierRefusal(statement, expression);
	if (!modifier.empty()) {
		fail(expression.location, "tiles may not " + modifier);
		return;
	}
	if (const Expression* material = materialCall(expression)) {
		fail(material->location, "tiles may not make a material");
		return;
	}
	if (const Expression* coat = coatCall(expression)) {
		fail(coat->location, "tiles may only use coats made before them");
	}
}

void checkTileStatement(const Statement& statement) {
	const std::string refused = statementRefusal(statement);
	if (!refused.empty()) {
		fail(statement.location, "tiles may not " + refused);
		return;
	}
	for (const Expression& expression : statement.expressions) {
		checkTileExpression(statement, expression);
	}
	checkTileBody(statement.body);
}

void checkTileBody(const std::vector<Statement>& body) {
	for (const Statement& statement : body) {
		if (failed()) {
			return;
		}
		checkTileStatement(statement);
	}
}

bool positiveStep(const std::vector<double>& step) {
	for (const double side : step) {
		if (!(side > 0 && std::isfinite(side))) {
			return false;
		}
	}
	return true;
}

bool tileArguments(const Arguments& arguments) {
	if (!arguments.positional.empty() || !arguments.named.contains("step")) {
		return false;
	}
	return std::all_of(arguments.named.begin(), arguments.named.end(), [](const auto& named) {
		return named.first == "step" || named.first == "from" || named.first == "to";
	});
}

bool wholeIndex(const Value& value) {
	return isKnown(value) && std::floor(value.number) == value.number && std::abs(value.number) <= double(highestTile);
}

bool tileBound(const Value& value) {
	return value.kind == ValueKind::List && value.items.size() == 3 && std::all_of(value.items.begin(), value.items.end(), wholeIndex);
}

TileGrid boundedGrid(const Arguments& arguments, const Location& location, TileGrid grid) {
	if (arguments.named.contains("from") != arguments.named.contains("to")) {
		fail(location, "tiles take from and to together");
		return grid;
	}
	if (!arguments.named.contains("from")) {
		return grid;
	}
	const Value& from = arguments.named.at("from");
	const Value& to   = arguments.named.at("to");
	if (!tileBound(from) || !tileBound(to)) {
		fail(location, "a tile's from and to are 3 whole numbers each");
		return grid;
	}
	for (std::size_t axis = 0; axis < 3; ++axis) {
		grid.from[axis] = std::int64_t(from.items[axis].number);
		grid.to[axis]   = std::int64_t(to.items[axis].number);
		if (grid.from[axis] > grid.to[axis]) {
			fail(location, "a tile's from is at most its to along x, y and z");
			return grid;
		}
	}
	return grid;
}

TileGrid tileGrid(Evaluator& evaluator, const Expression& call) {
	const Arguments arguments = evaluateArguments(evaluator, call);

	TileGrid grid = {
		.step = { 1, 1, 1 },
		.from = { lowestTile, lowestTile, lowestTile },
		.to   = { highestTile, highestTile, highestTile },
	};

	if (!tileArguments(arguments)) {
		fail(call.location, "tiles take a step");
		return grid;
	}
	grid.step = vector3(arguments.named.at("step"), call.location);
	if (!positiveStep(grid.step)) {
		fail(call.location, "a tile's step is above 0 along x, y and z");
	}
	return boundedGrid(arguments, call.location, grid);
}

std::uint64_t valueKey(const Value& value);

std::uint64_t patternKey(const Pattern& pattern) {
	std::uint64_t key = textKey(std::uint64_t(pattern.world), pattern.function->text);
	key               = mixedKey(mixedKey(key, std::uint64_t(pattern.function->location.line)), std::uint64_t(pattern.function->location.column));
	for (const Value& argument : pattern.arguments) {
		key = mixedKey(key, valueKey(argument));
	}
	for (const Value& stop : pattern.stops) {
		key = mixedKey(key, valueKey(stop));
	}
	return key;
}

std::uint64_t fieldsKey(std::uint64_t key, const Value& value) {
	if (!value.fields) {
		return key;
	}
	for (const std::string& field : *value.fields) {
		key = textKey(key, field);
	}
	return key;
}

std::uint64_t valueKey(const Value& value) {
	std::uint64_t key = mixedKey(std::uint64_t(value.kind), std::bit_cast<std::uint64_t>(value.number));
	key               = textKey(mixedKey(mixedKey(mixedKey(key, std::uint64_t(value.slot)), value.material), value.coat), wordOf(value));
	key               = value.pattern ? mixedKey(key, patternKey(*value.pattern)) : key;
	key               = fieldsKey(key, value);
	key               = value.network ? mixedKey(key, value.network->key) : key;
	for (const Value& item : value.items) {
		key = mixedKey(key, valueKey(item));
	}
	return key;
}

std::map<std::string, std::uint64_t> bindingKeys(const Scope& scope) {
	std::map<std::string, std::uint64_t> keys;
	for (const auto& [name, value] : scope) {
		keys[name] = valueKey(value);
	}
	return keys;
}

TileBlock tileBlock(const Evaluator& evaluator, const Statement& tiles, const TileGrid& grid) {
	const TileSnapshot snapshot = {
		.block           = &tiles,
		.parsed          = evaluator.parsed,
		.definitions     = evaluator.definitions,
		.scope           = evaluator.top,
		.defaultMaterial = evaluator.defaultMaterial,
	};

	const TileReads reads = {
		.names = {},
	};

	return TileBlock {
		.step     = { grid.step[0], grid.step[1], grid.step[2] },
		.from     = { grid.from[0], grid.from[1], grid.from[2] },
		.to       = { grid.to[0], grid.to[1], grid.to[2] },
		.source   = 0,
		.bindings = bindingKeys(evaluator.top),
		.reads    = std::make_shared<TileReads>(reads),
		.snapshot = std::make_shared<const TileSnapshot>(snapshot),
	};
}

void runTiles(Evaluator& evaluator, const Statement& tiles, const Context& context) {
	if (evaluator.compiler || evaluator.tiled || context.sink || !evaluator.blocks.empty()) {
		fail(tiles.location, "tiles stand at the top level of a file");
		return;
	}
	checkTileBody(tiles.body);
	const TileGrid grid = tileGrid(evaluator, tiles.expressions[0]);
	if (failed()) {
		return;
	}
	defaultMaterial(evaluator);
	evaluator.scene.tiles.push_back(tileBlock(evaluator, tiles, grid));
	const TileLeaves none = {
		.leaves = {},
	};

	evaluator.scene.tileLeaves = std::make_shared<TileLeaves>(none);
}

void runStatement(Evaluator& evaluator, const Statement& statement, const Context& context) {
	if (failed() || evaluator.ended || evaluator.returned) {
		return;
	}
	switch (statement.kind) {
		case StatementKind::Import: return runFile(evaluator, importedPath(*statement.location.file, statement.text), statement.location);
		case StatementKind::Let: return runLet(evaluator, statement);
		case StatementKind::For: return runLoop(evaluator, statement, context);
		case StatementKind::If: return runIf(evaluator, statement, context);
		case StatementKind::Return: return runReturn(evaluator, statement, context);
		case StatementKind::Surface: return define(evaluator, statement);
		case StatementKind::Shape: return define(evaluator, statement);
		case StatementKind::Function: return define(evaluator, statement);
		case StatementKind::Setting: return runSettingStatement(evaluator, statement);
		case StatementKind::Place: return runPlace(evaluator, statement, context);
		case StatementKind::Join: return runJoin(evaluator, statement, context);
		case StatementKind::Call: return runCall(evaluator, statement, context);
		case StatementKind::Tiles: return runTiles(evaluator, statement, context);
	}
}

void runBlock(Evaluator& evaluator, const std::vector<Statement>& body, const Context& context) {
	for (const Statement& statement : body) {
		runStatement(evaluator, statement, context);
	}
}

std::string readText(const std::string& path, const Location& location) {
	std::ifstream in(path);
	if (!in) {
		fail(location, "cannot read " + path);
		return {};
	}
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

void runFile(Evaluator& evaluator, const std::string& path, const Location& location) {
	refusedInTiles(evaluator, location, "import");
	if (failed() || evaluator.imported.contains(path)) {
		return;
	}
	evaluator.imported.insert(path);
	const std::string text = readText(path, location);
	if (failed()) {
		return;
	}
	ParsedFiles& parsed = *evaluator.parsed;
	parsed.key          = textKey(textKey(parsed.key, path), text);
	parsed.files.push_back(path);
	parsed.trees.push_back(parseSource(text, &parsed.files.back()));
	runBlock(evaluator, parsed.trees.back(), rootContext());
}

Context rootContext() {
	return Context {
		.linear = {
			{ 1, 0, 0 },
			{ 0, 1, 0 },
			{ 0, 0, 1 },
		},

		.origin   = { 0, 0, 0 },
		.stretch  = 1,
		.material = -1,
		.coats    = {},
		.rules    = 0,
		.groups   = 0,
		.sink     = nullptr,
	};
}

Render defaultRender() {
	return Render {
		.width      = 960,
		.height     = 600,
		.tolerance  = 2,
		.margin     = 1,
		.seed       = 1,
		.far        = 1000,
		.samples    = 0,
		.batch      = 0,
		.split      = 0,
		.surfaces   = 10,
		.collisions = 256,
	};
}

Camera defaultCamera() {
	return Camera {
		.eye      = { 0, -5, 1 },
		.target   = { 0, 0, 0 },
		.up       = { 0, 0, 1 },
		.fov      = 0.8f,
		.aperture = 0,
		.focus    = 0,
	};
}

Grade defaultGrade() {
	return Grade {
		.exposure = 1,
		.curve    = Curve::Filmic,
	};
}

std::vector<SkyStop> stopsOf(const std::vector<std::array<float, 4>>& rows) {
	std::vector<SkyStop> stops;
	for (const std::array<float, 4>& row : rows) {
		const SkyStop stop = {
			.height = row[0],
			.colour = { row[1], row[2], row[3] },
		};

		stops.push_back(stop);
	}
	return stops;
}

PaintedSky defaultPainted() {
	const std::vector<std::array<float, 4>> day = {
		{ -1, 0.05f, 0.045f, 0.04f },
		{ -0.02f, 0.10f, 0.09f, 0.08f },
		{ 0, 0.22f, 0.32f, 0.50f },
		{ 0.12f, 0.06f, 0.16f, 0.45f },
		{ 1, 0.01f, 0.05f, 0.30f },
	};

	const std::vector<std::array<float, 4>> dusk = {
		{ -1, 0.015f, 0.01f, 0.01f },
		{ -0.02f, 0.05f, 0.025f, 0.015f },
		{ 0, 0.60f, 0.22f, 0.05f },
		{ 0.08f, 0.40f, 0.12f, 0.07f },
		{ 0.30f, 0.08f, 0.05f, 0.14f },
		{ 1, 0.012f, 0.02f, 0.08f },
	};

	const std::vector<std::array<float, 4>> night = {
		{ -1, 0.002f, 0.002f, 0.003f },
		{ 0, 0.010f, 0.014f, 0.030f },
		{ 1, 0.002f, 0.004f, 0.012f },
	};

	return PaintedSky {
		.day   = stopsOf(day),
		.dusk  = stopsOf(dusk),
		.night = stopsOf(night),
	};
}

Sky defaultSky() {
	return Sky {
		.kind     = SkyKind::Gradient,
		.zenith   = { 0.05f, 0.13f, 0.34f },
		.horizon  = { 0.26f, 0.40f, 0.62f },
		.ground   = { 0.10f, 0.09f, 0.08f },
		.haze     = 3,
		.glow     = { 0, 0, 0 },
		.strength = 1,
		.spread   = 0.12f,
		.painted  = defaultPainted(),
	};
}

Stars defaultStars() {
	return Stars {
		.present  = false,
		.count    = 3000,
		.strength = 2,
		.angle    = 0.003f,
		.axis     = { 0, 0.4f, 0.9f },
	};
}

Clouds defaultClouds() {
	return Clouds {
		.present   = false,
		.height    = 1500,
		.thickness = 1200,
		.cover     = 0.45f,
		.density   = 0.03f,
		.span      = 3000,
	};
}

SceneFile emptySceneFile() {
	const Scene scene = {
		.programs       = {},
		.arguments      = {},
		.leaves         = {},
		.nodes          = {},
		.objects        = {},
		.objectPrograms = {},
		.tiles          = {},
		.tileLeaves     = nullptr,
	};

	const Looks looks = {
		.materials = {},
		.coats     = {},
		.groups    = {},
	};

	const Lighting lighting = {
		.lights = {},
		.sky    = defaultSky(),
		.clouds = defaultClouds(),
		.stars  = defaultStars(),
	};

	const Clip still = {
		.duration = 0,
		.rate     = 24,
		.cuts     = {},
	};

	const Shot shot = {
		.camera = defaultCamera(),
		.render = defaultRender(),
		.grade  = defaultGrade(),
		.clip   = still,
	};

	return SceneFile {
		.scene    = scene,
		.looks    = looks,
		.lighting = lighting,
		.shot     = shot,
		.timed    = false,
	};
}

bool hasSun(const Lighting& lighting) {
	for (const Light& light : lighting.lights) {
		if (light.kind == LightKind::Sun) {
			return true;
		}
	}
	return false;
}

std::string sceneError(const SceneFile& file, const std::string& path) {
	if (file.scene.objects.empty() && file.scene.tiles.empty()) {
		return path + ": the scene has no shapes";
	}
	if (file.lighting.sky.kind == SkyKind::Physical && !hasSun(file.lighting)) {
		return path + ": a physical sky is lit by a sun light";
	}
	return {};
}

std::string longObjectError(const Scene& scene, const std::string& path) {
	for (const Program& program : scene.objectPrograms) {
		if (program.steps.size() > mostProgramSteps) {
			return path + ": " + program.name + " has more than 65535 steps";
		}
	}
	return {};
}

LoadedScene failedLoad(const std::string& error) {
	return LoadedScene {
		.file  = emptySceneFile(),
		.error = error,
	};
}

Scope timedScope(double time) {
	Scope scope;
	scope["time"] = numberValue(time);
	return scope;
}

LoadedScene loadScene(const std::string& path, double time) {
	takenFailure();
	SceneFile file = emptySceneFile();
	Scene& scene   = file.scene;

	const ParsedFiles parsed = {
		.files = {},
		.trees = {},
		.key   = 0,
	};

	Evaluator evaluator = {
		.scene           = scene,
		.looks           = file.looks,
		.lighting        = file.lighting,
		.shot            = file.shot,
		.parsed          = std::make_shared<ParsedFiles>(parsed),
		.imported        = {},
		.definitions     = {},
		.top             = timedScope(time),
		.locals          = {},
		.blocks          = {},
		.shared          = nullptr,
		.tile            = nullptr,
		.base            = 0,
		.compiler        = nullptr,
		.isolated        = false,
		.returned        = false,
		.result          = numberValue(0),
		.ended           = false,
		.specialised     = {},
		.defaultMaterial = -1,
		.timed           = false,
		.tiled           = false,
		.reads           = {},
		.eye             = std::nullopt,
		.printed         = {},
	};

	const Location start = {
		.file   = nullptr,
		.line   = 1,
		.column = 1,
	};

	runFile(evaluator, path, start);
	if (failed()) {
		return failedLoad(takenFailure());
	}
	const std::string error = sceneError(file, path);
	if (!error.empty()) {
		return failedLoad(error);
	}
	scene.objects             = unionParts(scene);
	scene.objectPrograms      = objectPrograms(scene);
	const std::string tooLong = longObjectError(scene, path);
	if (!tooLong.empty()) {
		return failedLoad(tooLong);
	}
	for (TileBlock& block : scene.tiles) {
		block.source = evaluator.parsed->key;
	}
	file.timed = evaluator.timed || !scene.tiles.empty();

	return LoadedScene {
		.file  = std::move(file),
		.error = {},
	};
}

std::vector<double> tileOrigin(const TileBlock& block, const TilePlace& place) {
	return { block.step[0] * double(place.index[0]), block.step[1] * double(place.index[1]), block.step[2] * double(place.index[2]) };
}

Scope tileScope(const TileBlock& block, const TilePlace& place) {
	const std::vector<double> index = { double(place.index[0]), double(place.index[1]), double(place.index[2]) };
	const std::vector<double> step  = { block.step[0], block.step[1], block.step[2] };
	Scope scope;
	scope["tile"]        = numbersValue(index);
	scope["tile_origin"] = numbersValue(tileOrigin(block, place));
	scope["tile_step"]   = numbersValue(step);
	return scope;
}

Context tileContext(const TileBlock& block, const TilePlace& place) {
	return composed(rootContext(), translation(tileOrigin(block, place)));
}

TileContent failedTile(const std::string& error) {
	return TileContent {
		.scene = emptySceneFile().scene,
		.reads = {},
		.error = error,
	};
}

const Location& tileBlockLocation(const TileBlock& block) {
	return block.snapshot->block->location;
}

TileContent evaluatedTile(const Scene& scene, const TilePlace& place, const math::Vector& eye) {
	takenFailure();
	const TileBlock& block       = scene.tiles[place.block];
	const TileSnapshot& snapshot = *block.snapshot;
	SceneFile file               = emptySceneFile();
	const Scope names            = tileScope(block, place);

	Evaluator evaluator = {
		.scene           = file.scene,
		.looks           = file.looks,
		.lighting        = file.lighting,
		.shot            = file.shot,
		.parsed          = nullptr,
		.imported        = {},
		.definitions     = snapshot.definitions,
		.top             = {},
		.locals          = {},
		.blocks          = { 0 },
		.shared          = &snapshot.scope,
		.tile            = &names,
		.base            = 0,
		.compiler        = nullptr,
		.isolated        = false,
		.returned        = false,
		.result          = numberValue(0),
		.ended           = false,
		.specialised     = {},
		.defaultMaterial = snapshot.defaultMaterial,
		.timed           = false,
		.tiled           = true,
		.reads           = {},
		.eye             = eye,
		.printed         = {},
	};

	runBlock(evaluator, snapshot.block->body, tileContext(block, place));
	if (failed()) {
		return failedTile(takenFailure());
	}
	file.scene.objects        = unionParts(file.scene);
	file.scene.objectPrograms = objectPrograms(file.scene);
	const std::string tooLong = longObjectError(file.scene, *snapshot.block->location.file);
	if (!tooLong.empty()) {
		return failedTile(tooLong);
	}
	return TileContent {
		.scene = std::move(file.scene),
		.reads = std::move(evaluator.reads),
		.error = {},
	};
}

}
