#include <gtest/gtest.h>
#include "hw/pvr/ta_ctx.h"

static void addStrip(std::vector<PolyParam>& list, u32 vertices)
{
	list.emplace_back();
	list.back().init();
	list.back().count = vertices;
}

TEST(GeometryStatsTest, CountsStripsAndSpritesInsteadOfDrawCalls)
{
	rend_context ctx;
	ctx.Clear();
	ctx.global_param_op.front().count = 4; // Generated background quad.
	addStrip(ctx.global_param_op, 3);
	addStrip(ctx.global_param_op, 5);
	addStrip(ctx.global_param_pt, 4); // A sprite expands to four vertices.
	addStrip(ctx.global_param_tr, 6);
	RenderPass pass{};
	pass.op_count = 3;
	pass.pt_count = 1;
	pass.tr_count = 1;
	ctx.countGeometry(pass, {});
	EXPECT_EQ(ctx.geometryStats.opaque.vertices, 8u);
	EXPECT_EQ(ctx.geometryStats.opaque.triangles, 4u);
	EXPECT_EQ(ctx.geometryStats.punchThrough.triangles, 2u);
	EXPECT_EQ(ctx.geometryStats.translucent.triangles, 4u);
	EXPECT_EQ(ctx.geometryStats.total().vertices, 18u);
	EXPECT_EQ(ctx.geometryStats.total().triangles, 10u);
}

TEST(GeometryStatsTest, AccumulatesPassesWithoutRecountingMergedIndices)
{
	rend_context ctx;
	ctx.Clear();
	addStrip(ctx.global_param_op, 4);
	RenderPass first{};
	first.op_count = 2;
	ctx.countGeometry(first, {});
	// The preceding pass now contains GPU indices, including strip connectors.
	ctx.global_param_op[1].count = 200;
	addStrip(ctx.global_param_op, 3);
	addStrip(ctx.global_param_tr, 4);
	RenderPass second{};
	second.op_count = 3;
	second.tr_count = 1;
	ctx.countGeometry(second, first);
	EXPECT_EQ(ctx.geometryStats.total().vertices, 11u);
	EXPECT_EQ(ctx.geometryStats.total().triangles, 5u);
	EXPECT_EQ(ctx.geometryStats.renderPasses, 2u);
}

TEST(GeometryStatsTest, EmptyAndShortStripsDoNotUnderflow)
{
	rend_context ctx;
	ctx.Clear();
	for (u32 count : {0u, 1u, 2u})
		addStrip(ctx.global_param_tr, count);
	RenderPass pass{};
	pass.op_count = 1;
	pass.tr_count = 3;
	ctx.countGeometry(pass, {});
	EXPECT_EQ(ctx.geometryStats.total().vertices, 3u);
	EXPECT_EQ(ctx.geometryStats.total().triangles, 0u);
}

TEST(GeometryStatsTest, IncludesModifiersAndClearsRecycledContexts)
{
	rend_context ctx;
	ctx.Clear();
	ctx.geometryStats.opaque = {4, 2};
	ctx.geometryStats.modifierVolumes = {9, 3};
	ctx.geometryStats.valid = true;
	ctx.geometryStats.renderPasses = 2;
	EXPECT_EQ(ctx.geometryStats.total().vertices, 13u);
	EXPECT_EQ(ctx.geometryStats.total().triangles, 5u);
	ctx.Clear();
	EXPECT_EQ(ctx.geometryStats.total().vertices, 0u);
	EXPECT_EQ(ctx.geometryStats.total().triangles, 0u);
	EXPECT_EQ(ctx.geometryStats.renderPasses, 0u);
	EXPECT_FALSE(ctx.geometryStats.valid);
}

TEST(TaBufferStatsTest, UsesConfiguredRangeAndKeepsTheVramEndAddress)
{
	auto allocation = TaBufferAllocation::fromRegisters(0x00780000, 0x00800000, 0x00800000);
	ASSERT_TRUE(allocation.valid);
	EXPECT_EQ(allocation.capacityBytes, 512u * 1024);
	allocation = TaBufferAllocation::fromRegisters(0xff100003, 0xff180003, 0x00800000);
	ASSERT_TRUE(allocation.valid);
	EXPECT_EQ(allocation.base, 0x100000u);
	EXPECT_EQ(allocation.capacityBytes, 512u * 1024);
	EXPECT_FALSE(TaBufferAllocation::fromRegisters(0, 0, 0x800000).valid);
	EXPECT_FALSE(TaBufferAllocation::fromRegisters(0x200000, 0x100000, 0x800000).valid);
	EXPECT_FALSE(TaBufferAllocation::fromRegisters(0x700000, 0x900000, 0x800000).valid);
}

TEST(TaBufferStatsTest, ConvertsToNativePackedColorAndSplitsLongStrips)
{
	PCW pcw{};
	pcw.ParaType = 4;
	EXPECT_EQ(estimateTaParameterBytes(pcw, 3, 6), 60u); // 12-byte header + 3 * 16.
	EXPECT_EQ(estimateTaParameterBytes(pcw, 4, 2), 76u);
	EXPECT_EQ(estimateTaParameterBytes(pcw, 4, 1), 120u); // Two separate hardware strips.
	EXPECT_EQ(estimateTaParameterBytes(pcw, 10, 6), 216u);
	EXPECT_EQ(estimateTaParameterBytes(pcw, 10, 1), 480u);
	pcw.Col_Type = 1; // Floating point input color still occupies one output word.
	EXPECT_EQ(estimateTaParameterBytes(pcw, 3, 6), 60u);
	pcw.Texture = 1;
	pcw.Offset = 1;
	EXPECT_EQ(estimateTaParameterBytes(pcw, 3, 6), 96u);
	pcw.UV_16bit = 1;
	EXPECT_EQ(estimateTaParameterBytes(pcw, 3, 6), 84u);
	pcw.Volume = 1;
	EXPECT_EQ(estimateTaParameterBytes(pcw, 4, 2), 164u);
}

TEST(TaBufferStatsTest, CountsSpritesAsQuadsAndIgnoresIncompleteStrips)
{
	PCW pcw{};
	pcw.ParaType = 5;
	pcw.Texture = 1;
	pcw.UV_16bit = 1;
	EXPECT_EQ(estimateTaParameterBytes(pcw, 4, 1), 92u);
	pcw.ParaType = 4;
	EXPECT_EQ(estimateTaParameterBytes(pcw, 0, 0), 0u);
	EXPECT_EQ(estimateTaParameterBytes(pcw, 2, 0), 0u);
	EXPECT_EQ(estimateTaParameterBytes(pcw, 3, 0), 72u);
}

TEST(TaBufferStatsTest, TracksContinuationsWithoutPoolingIndependentCapacities)
{
	TaBufferStats stats;
	auto first = TaBufferAllocation::fromRegisters(0x100000, 0x180000, 0x800000);
	stats.add(first, 300 * 1024);
	first.continuation = true;
	stats.add(first, 250 * 1024);
	stats.add(TaBufferAllocation::fromRegisters(0x200000, 0x300000, 0x800000), 1024);
	ASSERT_TRUE(stats.complete);
	ASSERT_EQ(stats.buffers.size(), 2u);
	EXPECT_EQ(stats.buffers[0].usedBytes, 550u * 1024);
	EXPECT_EQ(stats.buffers[0].remainingBytes(), 0u);
	EXPECT_GT(stats.buffers[0].fraction(), 1.0);
	EXPECT_EQ(stats.buffers[1].remainingBytes(), 1023u * 1024);
	EXPECT_DOUBLE_EQ(TaBufferUsage{}.fraction(), 0.0);
}

TEST(TaBufferStatsTest, RejectsIncompleteSnapshotsAndChangingAllocations)
{
	auto allocation = TaBufferAllocation::fromRegisters(0x100000, 0x180000, 0x800000, true);
	TaBufferStats missingInitial;
	missingInitial.add(allocation, 100);
	EXPECT_FALSE(missingInitial.complete);
	TaBufferStats missingRegisters;
	missingRegisters.add({}, 100);
	EXPECT_FALSE(missingRegisters.complete);
	allocation.continuation = false;
	TaBufferStats changedLimit;
	changedLimit.add(allocation, 100);
	allocation.continuation = true;
	allocation.capacityBytes /= 2;
	changedLimit.add(allocation, 100);
	EXPECT_FALSE(changedLimit.complete);
}

TEST(TaBufferStatsTest, CountsBeforeIndexMergingAndExcludesBackground)
{
	rend_context ctx;
	ctx.Clear();
	ctx.global_param_op.front().count = 4;
	addStrip(ctx.global_param_op, 4);
	ctx.global_param_op.back().taStripTriangles = 2;
	RenderPass first{};
	first.op_count = 2;
	ctx.countGeometry(first, {});
	ctx.global_param_op.back().count = 999; // GPU index generation cannot change the snapshot.
	addStrip(ctx.global_param_op, 3);
	RenderPass second{};
	second.op_count = 3;
	ctx.countGeometry(second, first);
	EXPECT_EQ(ctx.geometryStats.total().parameterBytes, 136u);
	ctx.geometryStats.taBuffers.add(TaBufferAllocation::fromRegisters(0, 0x80000, 0x800000), 136);
	ctx.Clear();
	EXPECT_EQ(ctx.geometryStats.total().parameterBytes, 0u);
	EXPECT_TRUE(ctx.geometryStats.taBuffers.buffers.empty());
}
