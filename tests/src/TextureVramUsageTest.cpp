#include <gtest/gtest.h>
#include "rend/texture_vram_usage.h"
#include <array>
#include <limits>
#include <random>
#include <vector>

using texturePreview::VramUsageTracker;

TEST(TextureVramUsageTest, CountsOverlappingAndPaletteVariantsOnlyOnce)
{
	VramUsageTracker tracker;
	tracker.reset(1024);
	int a, b, paletteVariant;
	tracker.track(&a, 100, 200);
	tracker.track(&b, 200, 250);
	tracker.track(&paletteVariant, 100, 200);
	EXPECT_EQ(tracker.snapshot().usedBytes, 350u);
	EXPECT_EQ(tracker.snapshot().remainingBytes(), 674u);
	tracker.remove(&a);
	EXPECT_EQ(tracker.snapshot().usedBytes, 350u);
	tracker.remove(&paletteVariant);
	EXPECT_EQ(tracker.snapshot().usedBytes, 250u);
	tracker.remove(&b);
	EXPECT_EQ(tracker.snapshot().usedBytes, 0u);
	EXPECT_EQ(tracker.snapshot().peakBytes, 350u);
}

TEST(TextureVramUsageTest, RetainsPeaksBetweenUiRefreshesAndResetsOnReopen)
{
	VramUsageTracker tracker;
	tracker.reset(8 * 1024 * 1024);
	int a, b;
	tracker.track(&a, 0, 1024 * 1024);
	tracker.track(&b, 1024 * 1024, 2 * 1024 * 1024);
	tracker.remove(&b); // No UI snapshot was taken at the peak.
	EXPECT_EQ(tracker.snapshot().usedBytes, 1024u * 1024);
	EXPECT_EQ(tracker.snapshot().peakBytes, 3u * 1024 * 1024);
	tracker.reset(16 * 1024 * 1024);
	EXPECT_EQ(tracker.snapshot().peakBytes, 0u);
	EXPECT_EQ(tracker.snapshot().remainingBytes(), 16u * 1024 * 1024);
}

TEST(TextureVramUsageTest, ReplacingRangesDoesNotAccumulateOldUploads)
{
	VramUsageTracker tracker;
	tracker.reset(1024);
	int a, b;
	tracker.track(&a, 0, 400);
	tracker.track(&a, 0, 400);
	tracker.track(&b, 200, 100);
	tracker.track(&a, 800, 100);
	EXPECT_EQ(tracker.snapshot().usedBytes, 200u);
	EXPECT_EQ(tracker.snapshot().peakBytes, 400u);
	tracker.track(&a, 0, 0);
	EXPECT_EQ(tracker.snapshot().usedBytes, 100u);
	tracker.remove(&a);
	tracker.remove(&a);
	EXPECT_EQ(tracker.snapshot().usedBytes, 100u);
}

TEST(TextureVramUsageTest, ClampsMalformedRangesWithoutOverflow)
{
	VramUsageTracker tracker;
	tracker.reset(1024);
	int a, b;
	tracker.track(&a, 1000, std::numeric_limits<uint64_t>::max());
	tracker.track(&b, std::numeric_limits<uint32_t>::max(), 100);
	EXPECT_EQ(tracker.snapshot().usedBytes, 24u);
	tracker.track(&b, 0, std::numeric_limits<uint64_t>::max());
	EXPECT_EQ(tracker.snapshot().usedBytes, 1024u);
	EXPECT_EQ(tracker.snapshot().remainingBytes(), 0u);
	tracker.remove(&b);
	EXPECT_EQ(tracker.snapshot().usedBytes, 24u);
	tracker.reset(0);
	tracker.track(&a, 0, 100);
	EXPECT_EQ(tracker.snapshot().remainingBytes(), 0u);
}

TEST(TextureVramUsageTest, CountsMoreThanThePreviewDisplayLimit)
{
	VramUsageTracker tracker;
	tracker.reset(8 * 1024 * 1024);
	std::array<int, 1024> keys;
	for (size_t i = 0; i < keys.size(); i++)
		tracker.track(&keys[i], (uint32_t)i * 4096, 4096);
	EXPECT_EQ(tracker.snapshot().usedBytes, 4u * 1024 * 1024);
	for (size_t i = 0; i < keys.size(); i += 2)
		tracker.remove(&keys[i]);
	EXPECT_EQ(tracker.snapshot().usedBytes, 2u * 1024 * 1024);
	EXPECT_EQ(tracker.snapshot().peakBytes, 4u * 1024 * 1024);
}

TEST(TextureVramUsageTest, MatchesByteLevelOracleAcrossUploadsAndInvalidations)
{
	constexpr uint32_t capacity = 4096;
	VramUsageTracker tracker;
	tracker.reset(capacity);
	struct Allocation { uint32_t start = 0, length = 0; };
	std::array<Allocation, 40> allocations;
	std::mt19937 random(4852);
	uint32_t peak = 0;
	for (int step = 0; step < 3000; step++)
	{
		auto& allocation = allocations[random() % allocations.size()];
		if (random() % 3 == 0)
		{
			allocation.length = 0;
			tracker.remove(&allocation);
		}
		else
		{
			allocation.start = random() % (capacity + 500);
			allocation.length = random() % 500;
			tracker.track(&allocation, allocation.start, allocation.length);
		}
		std::vector<bool> occupied(capacity);
		for (const auto& item : allocations)
			for (uint32_t address = item.start; address < std::min(capacity, item.start + item.length); address++)
				occupied[address] = true;
		uint32_t bytes = (uint32_t)std::count(occupied.begin(), occupied.end(), true);
		peak = std::max(peak, bytes);
		ASSERT_EQ(tracker.snapshot().usedBytes, bytes) << "step " << step;
		ASSERT_EQ(tracker.snapshot().peakBytes, peak) << "step " << step;
	}
}
