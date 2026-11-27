#include "core/worker.h"

#include "core/frame.h"
#include "core/plugin.h"
#include "core/render.h"
#include "core/step.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace kodanuki {

std::vector<Tile> tilesOf(int width, int height, int size) {
	std::vector<Tile> tiles;
	for (int y = 0; y < height; y += size) {
		for (int x = 0; x < width; x += size) {
			const Tile tile = {
				.x      = x,
				.y      = y,
				.width  = std::min(size, width - x),
				.height = std::min(size, height - y),
			};

			tiles.push_back(tile);
		}
	}
	return tiles;
}

void tracedTile(const Renderer& renderer, const Pass& pass, Worker& worker) {
	while (launchWalks(renderer, pass, worker)) {
		extendRays(renderer, worker);
		arriveAtHits(renderer, worker);
		connectLights(renderer, worker);
		scatterWalks(renderer, worker);
		foldWalks(renderer, pass, worker);
	}
}

PixelWork idlePixels(int capacity) {
	const std::size_t walks = std::size_t(capacity);
	return PixelWork {
		.jobs      = {},
		.slots     = std::vector<PixelSlot>(walks / std::size_t(slotSamples), idleSlot()),
		.colours   = std::vector<Colour>(walks),
		.shared    = std::vector<Colour>(walks),
		.estimates = {},
		.firsts    = {},
		.next      = 0,
	};
}

Worker freshWorker(const Renderer& renderer, int index, std::uint32_t executor, std::uint32_t transporter) {
	const Tile none = {
		.x      = 0,
		.y      = 0,
		.width  = 0,
		.height = 0,
	};

	return Worker {
		.index       = index,
		.executor    = executor,
		.transporter = transporter,
		.tile        = none,
		.taken       = 0,
		.pool        = emptyPool(poolCapacity),
		.walks       = renderer.transport.hooks.openWorker(),
		.registers   = std::vector<double>(longestProgram(renderer.file.scene), 0.0),
		.pixels      = idlePixels(poolCapacity),
		.held        = nullptr,
	};
}

void hostedWorkers(int count, const std::function<void(int worker)>& run) {
	std::vector<std::thread> threads;
	for (int worker = 0; worker < count; ++worker) {
		threads.emplace_back(run, worker);
	}
	for (std::thread& thread : threads) {
		thread.join();
	}
}

void onWorkers(Renderer& renderer, const Pass& pass) {
	const Renderer& reading         = renderer;
	const std::vector<Tile>& tiles  = renderer.drawing.tiles;
	const std::uint32_t executor    = pluginIndex(renderer.execution.name);
	const std::uint32_t transporter = pluginIndex(renderer.transport.name);
	const double begun              = frameStepBegun(renderer.observing, LoopStep::Pass, corePlugin, std::uint32_t(tiles.size()));
	std::atomic<std::size_t> next   = 0;
	std::mutex placing;
	const auto run = [&](int index) {
		Worker worker = freshWorker(reading, index, executor, transporter);
		for (std::size_t k = next++; k < tiles.size(); k = next++) {
			worker.tile  = tiles[k];
			worker.taken = k;
			openTile(reading, pass, worker);
			tracedTile(reading, pass, worker);
			placing.lock();
			placeTile(renderer, pass, worker);
			placing.unlock();
		}
	};

	hostedWorkers(renderer.options.choices.threads, run);
	frameStepEnded(renderer.observing, LoopStep::Pass, corePlugin, std::uint32_t(tiles.size()), begun);
}

} // namespace kodanuki
