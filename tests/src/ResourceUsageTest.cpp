#include <gtest/gtest.h>
#include "profiler/resource_usage_metrics.h"
#include <vector>

TEST(ResourceUsageTest, CountsOccupiedBlocksIncludingPartialLastBlock)
{
	std::vector<std::uint8_t> ram(4096 * 2 + 17);
	EXPECT_EQ(resourceUsage::occupiedBytes(ram.data(), ram.size()), 0u);
	ram[4095] = 1;
	ram[0] = 2; // The same block must not be counted twice.
	EXPECT_EQ(resourceUsage::occupiedBytes(ram.data(), ram.size()), 4096u);
	ram.back() = 1;
	EXPECT_EQ(resourceUsage::occupiedBytes(ram.data(), ram.size()), 4096u + 17);
	ram[4096] = 1;
	EXPECT_EQ(resourceUsage::occupiedBytes(ram.data(), ram.size()), ram.size());
	std::fill(ram.begin(), ram.end(), 0);
	EXPECT_EQ(resourceUsage::occupiedBytes(ram.data(), ram.size()), 0u);
	EXPECT_EQ(resourceUsage::occupiedBytes(nullptr, 0), 0u);
}

TEST(ResourceUsageTest, MeasuresActiveCyclesAndRequiresAFullInterval)
{
	resourceUsage::CpuMeter cpu;
	EXPECT_FALSE(cpu.sample(1000, 100, 100));
	EXPECT_FALSE(cpu.sample(1050, 125, 100));
	EXPECT_TRUE(cpu.sample(1100, 125, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 0.75);
	EXPECT_TRUE(cpu.sample(1200, 225, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 0.0);
	EXPECT_TRUE(cpu.sample(1300, 225, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 1.0);
}

TEST(ResourceUsageTest, HandlesPausedClocksRewindsAndIdleCounterResets)
{
	resourceUsage::CpuMeter cpu;
	EXPECT_FALSE(cpu.sample(1000, 500, 100));
	EXPECT_FALSE(cpu.sample(1000, 500, 100));
	EXPECT_FALSE(cpu.sample(0, 0, 100)); // Reset / save-state load.
	EXPECT_TRUE(cpu.sample(100, 25, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 0.75);
	EXPECT_FALSE(cpu.sample(200, 0, 100));
	EXPECT_TRUE(cpu.sample(300, 0, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 1.0);
}

TEST(ResourceUsageTest, ClampsIdleTimeAndSupportsLongRunningGames)
{
	resourceUsage::CpuMeter cpu;
	const std::uint64_t base = std::uint64_t{1} << 40;
	EXPECT_FALSE(cpu.sample(base, base, 100));
	EXPECT_TRUE(cpu.sample(base + 100, base + 101, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 0.0);
	EXPECT_TRUE(cpu.sample(base + 200, base + 151, 100));
	EXPECT_DOUBLE_EQ(cpu.fraction(), 0.5);
}
