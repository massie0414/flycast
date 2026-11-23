#include "resource_usage.h"
#include "resource_usage_metrics.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/aica/aica_if.h"
#include <atomic>
#include <mutex>

namespace resourceUsage
{
std::uint64_t sleepCycles = 0;
namespace
{
std::atomic<bool> enabled{false};
std::atomic<unsigned> generation{0};
unsigned sampledGeneration = 0;
CpuMeter cpu;
std::mutex snapshotMutex;
Snapshot latest;
}

void setEnabled(bool value)
{
	std::lock_guard<std::mutex> lock(snapshotMutex);
	if (enabled.exchange(value) != value)
	{
		++generation;
		latest = {};
	}
}

Snapshot snapshot()
{
	std::lock_guard<std::mutex> lock(snapshotMutex);
	return latest;
}

void reset()
{
	sleepCycles = 0;
	cpu = {};
	std::lock_guard<std::mutex> lock(snapshotMutex);
	latest = {};
}

void sample()
{
	if (!enabled.load(std::memory_order_relaxed))
		return;
	const unsigned currentGeneration = generation.load();
	if (sampledGeneration != currentGeneration)
	{
		cpu = {};
		sampledGeneration = currentGeneration;
	}
	if (!cpu.sample(sh4_sched_now64(), sleepCycles, SH4_MAIN_CLOCK / 2))
		return;

	// VBlank runs on the emulation thread, so SH-4, DMA, ARM7 and AICA writes
	// cannot race this scan. The native window only sees the completed copy.
	Snapshot next;
	next.valid = true;
	next.cpu = cpu.fraction();
	next.main = {static_cast<std::uint32_t>(occupiedBytes(&mem_b[0], RAM_SIZE)), RAM_SIZE};
	next.sound = {static_cast<std::uint32_t>(occupiedBytes(&aica::aica_ram[0], ARAM_SIZE)), ARAM_SIZE};
	std::lock_guard<std::mutex> lock(snapshotMutex);
	if (enabled && generation == currentGeneration)
		latest = next;
}
}
