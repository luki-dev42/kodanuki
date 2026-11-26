#include "syntax/compile.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace kodanuki {

bool isRuntime(const Value& value) {
	if (value.kind == ValueKind::Number) {
		return true;
	}
	if (value.kind != ValueKind::List && value.kind != ValueKind::Record) {
		return false;
	}
	return std::all_of(value.items.begin(), value.items.end(), isRuntime);
}

bool holdsRecord(const Value& value) {
	return value.kind == ValueKind::Record || std::any_of(value.items.begin(), value.items.end(), holdsRecord);
}

bool holdsCoat(const Value& value) {
	return value.kind == ValueKind::Coat || std::any_of(value.items.begin(), value.items.end(), holdsCoat);
}

bool holdsNetwork(const Value& value) {
	return value.kind == ValueKind::Network || std::any_of(value.items.begin(), value.items.end(), holdsNetwork);
}

bool unpassable(const Value& argument) {
	return holdsRecord(argument) && !isRuntime(argument);
}

void describeShape(std::ostringstream& out, const Value& value);

void describeFields(std::ostringstream& out, const Value& record) {
	out << '{';
	for (std::size_t i = 0; i < record.items.size(); ++i) {
		out << (*record.fields)[i] << '=';
		describeShape(out, record.items[i]);
		out << ',';
	}
	out << '}';
}

void describeShape(std::ostringstream& out, const Value& value) {
	if (value.kind == ValueKind::Record) {
		describeFields(out, value);
		return;
	}
	if (value.kind != ValueKind::List) {
		return;
	}
	out << '[' << value.items.size();
	for (const Value& item : value.items) {
		describeShape(out, item);
	}
	out << ']';
}

void describe(std::ostringstream& out, const Value& value) {
	if (value.kind == ValueKind::List) {
		out << '[';
		for (const Value& item : value.items) {
			describe(out, item);
			out << ',';
		}
		out << ']';
		return;
	}
	if (value.kind == ValueKind::Word) {
		out << wordOf(value);
		return;
	}
	out << value.number << '/' << value.material;
}

void describeArgument(std::ostringstream& out, const Value& argument) {
	if (isRuntime(argument)) {
		out << '#';
		describeShape(out, argument);
		return;
	}
	describe(out, argument);
}

std::string specialisationKey(const Pattern& pattern) {
	std::ostringstream out;
	out.precision(17);
	out << pattern.function->text << '(';
	for (const Value& argument : pattern.arguments) {
		describeArgument(out, argument);
		out << ',';
	}
	out << ')';
	return out.str();
}

Value argumentValue(Compiler& compiler, const Value& argument) {
	if (!isRuntime(argument)) {
		return argument;
	}
	if (argument.kind == ValueKind::Number) {
		const std::uint32_t index = compiler.program.arguments++;

		const Step argument = {
			.opcode = Opcode::Argument,
			.a      = std::uint16_t(index),
			.b      = 0,
			.c      = 0,
			.value  = 0,
			.leaf   = noLeaf,
		};

		return slotValue(pushStep(compiler, argument));
	}
	Value value = argument;
	for (Value& item : value.items) {
		item = argumentValue(compiler, item);
	}
	return value;
}

std::vector<Value> argumentValues(Compiler& compiler, const std::vector<Value>& arguments) {
	std::vector<Value> values;
	for (const Value& argument : arguments) {
		values.push_back(argumentValue(compiler, argument));
	}
	return values;
}

Value inputVector(Compiler& compiler, Opcode x) {
	std::vector<Value> items;
	for (int i = 0; i < 3; ++i) {
		const Step input = {
			.opcode = Opcode(int(x) + i),
			.a      = 0,
			.b      = 0,
			.c      = 0,
			.value  = 0,
			.leaf   = noLeaf,
		};

		items.push_back(slotValue(pushStep(compiler, input)));
	}
	return listValue(std::move(items));
}

void startProgram(Compiler& compiler, const Pattern& pattern) {
	compiler.program.name = pattern.function->text;
	compiler.point        = inputVector(compiler, Opcode::PointX);
	compiler.normal       = inputVector(compiler, Opcode::NormalX);
}

Compiler emptyCompiler() {
	const Program program = {
		.name      = {},
		.steps     = {},
		.result    = 0,
		.arguments = 0,
	};

	return Compiler {
		.program   = program,
		.constants = {},
		.point     = numberValue(0),
		.normal    = numberValue(0),
		.reads     = {},
	};
}

Compiler compileFunction(Evaluator& evaluator, const Pattern& pattern) {
	Compiler compiler            = emptyCompiler();
	Compiler* const saved        = evaluator.compiler;
	evaluator.compiler           = &compiler;
	std::vector<Value> arguments = argumentValues(compiler, pattern.arguments);
	startProgram(compiler, pattern);

	const Invocation invocation = {
		.definition = *pattern.function,
		.arguments  = arguments,
		.context    = rootContext(),
		.location   = pattern.location,
	};

	const Value result = inlineFunction(evaluator, invocation);
	if (result.kind != ValueKind::Number) {
		fail(pattern.function->location, pattern.function->text + " must return a number");
	}
	if (compiler.program.steps.size() > mostProgramSteps) {
		fail(pattern.location, pattern.function->text + " has more than 65535 steps");
	}
	compiler.program.result = slotOf(evaluator, result);
	evaluator.compiler      = saved;
	return compiler;
}

std::optional<std::uint32_t> heldProgram(Evaluator& evaluator, const std::string& key) {
	const auto found = evaluator.specialised.find(key);
	if (found == evaluator.specialised.end()) {
		return std::nullopt;
	}
	for (const Specialisation& held : found->second) {
		if (readsHold(evaluator, held.reads)) {
			return held.program;
		}
	}
	return std::nullopt;
}

std::uint32_t compiledProgram(Evaluator& evaluator, const Pattern& pattern, const std::string& key) {
	const Compiler compiled = compileFunction(evaluator, pattern);
	evaluator.scene.programs.push_back(compiled.program);

	const Specialisation made = {
		.program = std::uint32_t(evaluator.scene.programs.size() - 1),
		.reads   = compiled.reads,
	};

	evaluator.specialised[key].push_back(made);
	return made.program;
}

std::uint32_t specialise(Evaluator& evaluator, const Pattern& pattern) {
	if (failed()) {
		return 0;
	}
	if (std::any_of(pattern.arguments.begin(), pattern.arguments.end(), holdsNetwork)) {
		fail(pattern.location, "a surface takes no network");
		return 0;
	}
	if (std::any_of(pattern.arguments.begin(), pattern.arguments.end(), unpassable)) {
		fail(pattern.location, "a record passed to a surface holds only numbers and lists of numbers");
		return 0;
	}
	if (std::any_of(pattern.arguments.begin(), pattern.arguments.end(), holdsCoat)) {
		fail(pattern.location, "a surface takes no coat");
		return 0;
	}
	const std::string key = specialisationKey(pattern);
	if (const std::optional<std::uint32_t> held = heldProgram(evaluator, key)) {
		return *held;
	}
	return compiledProgram(evaluator, pattern, key);
}

void pushArgument(Evaluator& evaluator, const Value& argument) {
	if (argument.kind == ValueKind::Number) {
		evaluator.scene.arguments.push_back(argument.number);
		return;
	}
	for (const Value& item : argument.items) {
		pushArgument(evaluator, item);
	}
}

void pushArguments(Evaluator& evaluator, const std::vector<Value>& arguments) {
	for (const Value& argument : arguments) {
		if (isRuntime(argument)) {
			pushArgument(evaluator, argument);
		}
	}
}

}
