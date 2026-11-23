#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace resourceUsage
{
// Guest allocators are game-specific. This measures data occupancy, not free
// heap space: a block counts once if it contains at least one nonzero byte.
inline std::size_t occupiedBytes(const std::uint8_t *data, std::size_t size)
{
	constexpr std::size_t blockSize = 4096;
	std::size_t occupied = 0;
	for (std::size_t offset = 0; offset < size; offset += blockSize)
	{
		const std::size_t length = std::min(blockSize, size - offset);
		if (std::any_of(data + offset, data + offset + length,
				[](std::uint8_t value) { return value != 0; }))
			occupied += length;
	}
	return occupied;
}

class CpuMeter
{
	std::uint64_t previousCycles = 0, previousIdle = 0;
	bool initialized = false;
	double usage = 0;

public:
	bool sample(std::uint64_t cycles, std::uint64_t idle, std::uint64_t interval)
	{
		if (!initialized || cycles < previousCycles || idle < previousIdle)
		{
			initialized = true;
			previousCycles = cycles;
			previousIdle = idle;
			return false;
		}
		const auto elapsed = cycles - previousCycles;
		if (elapsed == 0 || elapsed < interval)
			return false;
		usage = 1.0 - double(std::min(idle - previousIdle, elapsed)) / double(elapsed);
		previousCycles = cycles;
		previousIdle = idle;
		return true;
	}
	double fraction() const { return usage; }
};
}
