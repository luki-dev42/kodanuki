#include "sdf/compare.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

namespace kodanuki {

std::vector<double> placedNumbers(const Leaf& leaf) {
	const Placement& place      = leaf.placement;
	std::vector<double> numbers = { double(leaf.material), double(leaf.rules), place.stretch };
	numbers.insert(numbers.end(), &place.linear[0][0], &place.linear[0][0] + 9);
	numbers.insert(numbers.end(), place.origin, place.origin + 3);
	numbers.insert(numbers.end(), leaf.coats.begin(), leaf.coats.end());
	return numbers;
}

std::vector<double> leafNumbers(const Leaf& leaf) {
	std::vector<double> numbers      = { double(leaf.program), double(leaf.arguments) };
	const std::vector<double> placed = placedNumbers(leaf);
	numbers.insert(numbers.end(), placed.begin(), placed.end());
	return numbers;
}

bool sameLeaf(const Leaf& a, const Leaf& b) {
	return a.groups == b.groups && leafNumbers(a) == leafNumbers(b);
}

bool sameNode(const Node& a, const Node& b) {
	return a.join == b.join && a.radius == b.radius && a.leaf == b.leaf && a.children == b.children;
}

bool samePrograms(const std::vector<Program>& a, const std::vector<Program>& b) {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), sameProgram);
}

bool sameSteps(const TileBlock& a, const TileBlock& b) {
	const bool steps = a.step[0] == b.step[0] && a.step[1] == b.step[1] && a.step[2] == b.step[2];
	return steps && std::equal(a.from, a.from + 3, b.from) && std::equal(a.to, a.to + 3, b.to);
}

bool sameTileLayout(const std::vector<TileBlock>& a, const std::vector<TileBlock>& b) {
	return std::equal(a.begin(), a.end(), b.begin(), b.end(), sameSteps);
}

bool sameBinding(const TileBlock& a, const TileBlock& b, const std::string& name) {
	const auto first  = a.bindings.find(name);
	const auto second = b.bindings.find(name);
	const bool absent = first == a.bindings.end();
	return absent == (second == b.bindings.end()) && (absent || first->second == second->second);
}

bool sameTileBlock(const TileBlock& a, const TileBlock& b) {
	if (a.source != b.source) {
		return false;
	}
	return std::all_of(a.reads->names.begin(), a.reads->names.end(), [&](const std::string& name) {
		return sameBinding(a, b, name);
	});
}

bool sameTiles(const std::vector<TileBlock>& a, const std::vector<TileBlock>& b) {
	return sameTileLayout(a, b) && std::equal(a.begin(), a.end(), b.begin(), b.end(), sameTileBlock);
}

bool sameShapes(const Scene& a, const Scene& b) {
	const bool leaves = std::equal(a.leaves.begin(), a.leaves.end(), b.leaves.begin(), b.leaves.end(), sameLeaf);
	const bool nodes  = std::equal(a.nodes.begin(), a.nodes.end(), b.nodes.begin(), b.nodes.end(), sameNode);
	const bool held   = a.arguments == b.arguments && a.objects == b.objects && leaves && nodes;
	return held && samePrograms(a.programs, b.programs) && samePrograms(a.objectPrograms, b.objectPrograms) && sameTiles(a.tiles, b.tiles);
}

void carriedTiles(const Scene& before, Scene& next) {
	if (sameTileLayout(before.tiles, next.tiles)) {
		for (std::size_t block = 0; block < next.tiles.size(); ++block) {
			next.tiles[block].reads = before.tiles[block].reads;
		}
	}
	if (before.tileLeaves && next.tileLeaves) {
		next.tileLeaves = before.tileLeaves;
	}
}

std::uint64_t mixedKey(std::uint64_t key, std::uint64_t word) {
	const std::uint64_t mixed = (key ^ word) * 0x9E3779B97F4A7C15ull;
	return mixed ^ (mixed >> 29);
}

std::uint64_t leafKey(std::uint64_t key, const Leaf& leaf) {
	std::uint64_t mixed = mixedKey(key, leaf.groups);
	for (const double number : placedNumbers(leaf)) {
		mixed = mixedKey(mixed, std::bit_cast<std::uint64_t>(number));
	}
	return mixed;
}

std::uint64_t stepKey(std::uint64_t key, const Step& step, std::uint32_t leaf) {
	const std::uint64_t operands = std::uint64_t(step.opcode) | std::uint64_t(step.a) << 16 | std::uint64_t(step.b) << 32 | std::uint64_t(step.c) << 48;
	return mixedKey(mixedKey(mixedKey(key, operands), std::bit_cast<std::uint64_t>(step.value)), leaf);
}

std::uint64_t headKey(const Program& program) {
	return mixedKey(program.steps.size(), std::uint64_t(program.result) << 32 | program.arguments);
}

std::uint64_t programKey(const Program& program) {
	std::uint64_t key = headKey(program);
	for (const Step& step : program.steps) {
		key = stepKey(key, step, step.leaf);
	}
	return key;
}

std::uint32_t leastLeaf(const Program& program) {
	std::uint32_t least = noLeaf;
	for (const Step& step : program.steps) {
		least = std::min(least, step.leaf);
	}
	return least;
}

std::uint64_t objectKey(const Scene& scene, std::uint32_t object) {
	const Program& program    = scene.objectPrograms[object];
	const std::uint32_t least = leastLeaf(program);
	std::uint64_t key         = headKey(program);
	std::uint32_t named       = noLeaf;
	for (const Step& step : program.steps) {
		key   = stepKey(key, step, step.leaf == noLeaf ? noLeaf : step.leaf - least);
		key   = step.leaf != named && step.leaf != noLeaf ? leafKey(key, scene.leaves[step.leaf]) : key;
		named = step.leaf;
	}
	return key;
}

std::vector<std::uint64_t> objectKeys(const Scene& scene) {
	std::vector<std::uint64_t> keys;
	for (std::uint32_t object = 0; object < scene.objectPrograms.size(); ++object) {
		keys.push_back(objectKey(scene, object));
	}
	return keys;
}

std::vector<double> colourNumbers(const Colour& colour) {
	return { colour.red, colour.green, colour.blue };
}

std::vector<double> vectorNumbers(const math::Vector& vector) {
	return { vector.x, vector.y, vector.z };
}

std::vector<double> joinedNumbers(const std::vector<std::vector<double>>& parts) {
	std::vector<double> numbers;
	for (const std::vector<double>& part : parts) {
		numbers.insert(numbers.end(), part.begin(), part.end());
	}
	return numbers;
}

std::vector<double> albedoNumbers(const Albedo& albedo) {
	const std::vector<double> stops = { albedo.stops.begin(), albedo.stops.end() };
	return joinedNumbers({ { double(albedo.program), double(albedo.arguments), double(albedo.world), double(stops.size()) }, stops });
}

std::vector<double> materialNumbers(const Material& material) {
	const std::vector<double> fields = { double(material.kind), material.roughness, material.refraction, material.dispersion, material.scatter, material.lean, material.strength, material.film[0], material.film[1], double(material.guided), double(material.relays), double(material.aimed) };
	return joinedNumbers({ fields, albedoNumbers(material.albedo), colourNumbers(material.absorb), colourNumbers(material.glow) });
}

bool sameMaterial(const Material& a, const Material& b) {
	return materialNumbers(a) == materialNumbers(b);
}

std::vector<double> coatNumbers(const Coat& coat) {
	const std::vector<double> fields = { double(coat.mask), double(coat.effect), coat.level, coat.width, coat.soft, double(coat.material), coat.amount, coat.ragged, coat.grain, double(coat.relief), coat.span };
	return joinedNumbers({ fields, vectorNumbers(coat.axis), vectorNumbers(coat.along), colourNumbers(coat.colour), albedoNumbers(coat.pattern), albedoNumbers(coat.height) });
}

bool sameCoat(const Coat& a, const Coat& b) {
	return coatNumbers(a) == coatNumbers(b);
}

bool sameLooks(const Looks& a, const Looks& b) {
	const bool materials = std::equal(a.materials.begin(), a.materials.end(), b.materials.begin(), b.materials.end(), sameMaterial);
	const bool coats     = std::equal(a.coats.begin(), a.coats.end(), b.coats.begin(), b.coats.end(), sameCoat);
	return materials && coats && a.groups == b.groups;
}

std::vector<double> lightNumbers(const Light& light) {
	const std::vector<double> fields = { double(light.kind), light.radius, light.angle, double(light.caustics) };
	return joinedNumbers({ fields, vectorNumbers(light.at), vectorNumbers(light.direction), vectorNumbers(light.along), vectorNumbers(light.across), colourNumbers(light.power) });
}

bool sameLight(const Light& a, const Light& b) {
	return a.reaches == b.reaches && lightNumbers(a) == lightNumbers(b);
}

std::vector<double> stopsNumbers(const std::vector<SkyStop>& stops) {
	std::vector<double> numbers = { double(stops.size()) };
	for (const SkyStop& stop : stops) {
		numbers.push_back(stop.height);
		const std::vector<double> colour = colourNumbers(stop.colour);
		numbers.insert(numbers.end(), colour.begin(), colour.end());
	}
	return numbers;
}

std::vector<double> skyNumbers(const Sky& sky) {
	const std::vector<double> fields  = { double(sky.kind), sky.haze, sky.strength, sky.spread };
	const std::vector<double> colours = joinedNumbers({ colourNumbers(sky.zenith), colourNumbers(sky.horizon), colourNumbers(sky.ground), colourNumbers(sky.glow) });
	return joinedNumbers({ fields, colours, stopsNumbers(sky.painted.day), stopsNumbers(sky.painted.dusk), stopsNumbers(sky.painted.night) });
}

std::vector<double> cloudsNumbers(const Clouds& clouds) {
	return { double(clouds.present), clouds.height, clouds.thickness, clouds.cover, clouds.density, clouds.span };
}

std::vector<double> starsNumbers(const Stars& stars) {
	return joinedNumbers({ { double(stars.present), stars.count, stars.strength, stars.angle }, vectorNumbers(stars.axis) });
}

bool sameLighting(const Lighting& a, const Lighting& b) {
	const bool lights = std::equal(a.lights.begin(), a.lights.end(), b.lights.begin(), b.lights.end(), sameLight);
	const bool sky    = skyNumbers(a.sky) == skyNumbers(b.sky);
	return lights && sky && cloudsNumbers(a.clouds) == cloudsNumbers(b.clouds) && starsNumbers(a.stars) == starsNumbers(b.stars);
}

std::vector<double> renderNumbers(const Render& render) {
	return { double(render.width), double(render.height), render.tolerance, render.margin, double(render.seed), render.far, double(render.samples), double(render.batch), double(render.split), double(render.surfaces), double(render.collisions) };
}

bool sameSettings(const Shot& a, const Shot& b) {
	const bool clip = a.clip.duration == b.clip.duration && a.clip.rate == b.clip.rate && a.clip.cuts == b.clip.cuts;
	return clip && renderNumbers(a.render) == renderNumbers(b.render);
}

}
