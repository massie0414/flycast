#include "level_meter.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace audioMeter
{
namespace
{
std::atomic<std::uint32_t> generation{}; // Odd while collecting.
std::mutex mutex;
Levels published{};
// Owned exclusively by the emulation thread.
Levels pending{};
std::uint32_t producerGeneration{};
unsigned sampleCount{};
}

void setEnabled(bool enabled)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (bool(generation.load(std::memory_order_relaxed) & 1) != enabled)
	{
		generation.fetch_add(1, std::memory_order_relaxed);
		published = {};
	}
}

std::uint32_t beginSample()
{
	const auto token = generation.load(std::memory_order_relaxed);
	return token & 1 ? token : 0;
}

void record(std::uint32_t token, const Levels& samples)
{
	if (token == 0)
		return;
	if (producerGeneration != token)
	{
		producerGeneration = token;
		pending = {};
		sampleCount = 0;
	}
	for (unsigned source = 0; source < SourceCount; source++)
		for (unsigned channel = 0; channel < 2; channel++)
			pending[source][channel] = std::max(pending[source][channel], std::abs(samples[source][channel]) / 32768.f);
	// Publish every 20 ms. Never block the audio producer on the window thread.
	if (++sampleCount >= 882)
	{
		std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		if (generation.load(std::memory_order_relaxed) == token)
			for (unsigned source = 0; source < SourceCount; source++)
				for (unsigned channel = 0; channel < 2; channel++)
					published[source][channel] = std::max(published[source][channel], pending[source][channel]);
		pending = {};
		sampleCount = 0;
	}
}

Levels consume()
{
	std::lock_guard<std::mutex> lock(mutex);
	const Levels result = published;
	published = {};
	return result;
}
}
