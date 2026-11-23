#pragma once

#include <cstdint>

namespace resourceUsage
{
struct Memory
{
	std::uint32_t occupied = 0, capacity = 0;
	double fraction() const { return capacity == 0 ? 0 : double(occupied) / capacity; }
};

struct Snapshot
{
	bool valid = false;
	double cpu = 0;
	Memory main, sound;
};

// UI thread: enable collection and retrieve a copy without accessing guest RAM.
void setEnabled(bool enabled);
Snapshot snapshot();

// Emulation thread only (or while stopped). No guest state is modified or saved.
extern std::uint64_t sleepCycles;
void sample();
void reset();
}
