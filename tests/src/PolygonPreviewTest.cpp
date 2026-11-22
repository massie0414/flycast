#include <gtest/gtest.h>
#include "rend/polygon_preview.h"
#include "hw/pvr/ta_ctx.h"
#include "cfg/option.h"
#include <cmath>
#include <limits>

class PolygonPreviewTest : public ::testing::Test
{
protected:
	rend_context ctx{};
	bool widescreen = config::Widescreen, rotate = config::Rotate90, framebuffer = config::EmulateFramebuffer;
	int stretching = config::ScreenStretching;

	void SetUp() override
	{
		config::Widescreen = false;
		config::Rotate90 = false;
		config::EmulateFramebuffer = true;
		config::ScreenStretching = 100;
		ctx.Clear();
		ctx.globClip = {640, 480};
		ctx.framebufferWidth = 640;
		ctx.framebufferHeight = 480;
		polygonPreview::setEnabled(false);
		polygonPreview::setEnabled(true);
	}

	void TearDown() override
	{
		polygonPreview::setEnabled(false);
		config::Widescreen = widescreen;
		config::Rotate90 = rotate;
		config::EmulateFramebuffer = framebuffer;
		config::ScreenStretching = stretching;
	}

	void strip(std::vector<PolyParam>& list, std::initializer_list<glm::vec3> positions)
	{
		PolyParam poly;
		poly.init();
		poly.first = (u32)ctx.verts.size();
		poly.count = (u32)positions.size();
		list.push_back(poly);
		for (auto position : positions)
		{
			Vertex vertex{};
			vertex.x = position.x;
			vertex.y = position.y;
			vertex.z = position.z;
			ctx.verts.push_back(vertex);
		}
	}

	RenderPass pass()
	{
		RenderPass p{};
		p.op_count = (u32)ctx.global_param_op.size();
		p.pt_count = (u32)ctx.global_param_pt.size();
		p.tr_count = (u32)ctx.global_param_tr.size();
		return p;
	}

	std::shared_ptr<const polygonPreview::Frame> capture()
	{
		polygonPreview::Capture capture(ctx);
		capture.addPass(pass(), {});
		capture.publish();
		return polygonPreview::snapshot();
	}
};

TEST_F(PolygonPreviewTest, CapturesOriginalStripsAcrossPassesAndAllColorLists)
{
	ctx.global_param_op[0].count = 4; // Exclude generated background.
	strip(ctx.global_param_op, {{0, 0, 1}, {640, 0, 1}, {0, 480, 1}, {640, 480, 1}});
	polygonPreview::Capture capture(ctx);
	auto previous = pass();
	capture.addPass(previous, {});
	ctx.global_param_op[1].first = 0xffffffff; // GPU index generation changed this pass.
	ctx.global_param_op[1].count = 1000;
	strip(ctx.global_param_pt, {{0, 0, 1}, {100, 0, 1}, {0, 100, 1}});
	strip(ctx.global_param_tr, {{200, 200, 1}, {300, 200, 1}, {200, 300, 1}});
	capture.addPass(pass(), previous);
	capture.publish();
	auto frame = polygonPreview::snapshot();
	ASSERT_NE(frame, nullptr);
	EXPECT_EQ(frame->triangles, 4u);
	EXPECT_EQ(frame->edges.size(), 12u);
	EXPECT_FLOAT_EQ(frame->edges[0].a.x, 0.f);
	EXPECT_FLOAT_EQ(frame->edges[0].b.x, 1.f);
	EXPECT_FLOAT_EQ(frame->edges[1].b.y, 1.f);
}

TEST_F(PolygonPreviewTest, ClipsOffscreenEdgesAndRejectsNonfiniteVertices)
{
	strip(ctx.global_param_op, {{-640, 240, 1}, {1280, 240, 1}, {320, 960, 1}});
	strip(ctx.global_param_pt, {{0, 0, 1}, {std::numeric_limits<float>::infinity(), 0, 1}, {0, 480, 1}});
	strip(ctx.global_param_tr, {{-100, -100, 1}, {-200, -100, 1}, {-100, -200, 1}});
	auto frame = capture();
	ASSERT_NE(frame, nullptr);
	ASSERT_FALSE(frame->edges.empty());
	EXPECT_FLOAT_EQ(frame->edges[0].a.x, 0.f);
	EXPECT_FLOAT_EQ(frame->edges[0].b.x, 1.f);
	for (auto edge : frame->edges)
		for (auto p : {edge.a, edge.b})
		{
			EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
			EXPECT_GE(p.x, 0.f);
			EXPECT_LE(p.x, 1.f);
			EXPECT_GE(p.y, 0.f);
			EXPECT_LE(p.y, 1.f);
		}
}

TEST_F(PolygonPreviewTest, IgnoresInvalidStripsAndRenderToTexture)
{
	strip(ctx.global_param_op, {{0, 0, 1}, {640, 0, 1}, {0, 480, 1}});
	ctx.isRTT = true;
	EXPECT_EQ(capture(), nullptr);
	ctx.isRTT = false;
	ctx.global_param_op[1].first = 0xffffffff;
	strip(ctx.global_param_pt, {{0, 0, 1}, {100, 0, 1}});
	auto frame = capture();
	ASSERT_NE(frame, nullptr);
	EXPECT_TRUE(frame->edges.empty());
}

TEST_F(PolygonPreviewTest, DoesNotRepublishAfterClosingOrClearing)
{
	strip(ctx.global_param_op, {{0, 0, 1}, {640, 0, 1}, {0, 480, 1}});
	polygonPreview::Capture old(ctx);
	old.addPass(pass(), {});
	polygonPreview::clear();
	old.publish();
	EXPECT_EQ(polygonPreview::snapshot(), nullptr);
	ASSERT_NE(capture(), nullptr);
	polygonPreview::setEnabled(false);
	EXPECT_EQ(capture(), nullptr);
	polygonPreview::setEnabled(true);
	ASSERT_NE(capture(), nullptr);
}

TEST_F(PolygonPreviewTest, LimitsMemoryForLargeFrames)
{
	strip(ctx.global_param_op, {{0, 0, 1}, {640, 0, 1}, {0, 480, 1}});
	for (u32 i = 0; i < polygonPreview::MaxTriangles; i++)
		ctx.verts.push_back(ctx.verts[4 + i % 3]);
	ctx.global_param_op[1].count += polygonPreview::MaxTriangles;
	auto frame = capture();
	ASSERT_NE(frame, nullptr);
	EXPECT_TRUE(frame->limited);
	EXPECT_EQ(frame->triangles, polygonPreview::MaxTriangles);
	EXPECT_LE(frame->edges.size(), polygonPreview::MaxTriangles * 3u);
}

TEST_F(PolygonPreviewTest, ProjectsNaomi2MatricesAndClipsBehindCamera)
{
	strip(ctx.global_param_op, {{0, 0, 1}, {640, 0, 1}, {0, 480, 1}});
	N2Matrix identity{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
	ctx.matrices.push_back(identity);
	ctx.matrices.push_back(identity);
	ctx.matrices[1].mat[12] = 64; // Model-view translation.
	ctx.global_param_op[1].mvMatrix = 1;
	ctx.global_param_op[1].projMatrix = 0;
	auto frame = capture();
	ASSERT_NE(frame, nullptr);
	ASSERT_FALSE(frame->edges.empty());
	EXPECT_NEAR(frame->edges[0].a.x, 0.1f, 1e-5f);
	polygonPreview::clear();
	ctx.matrices[0].mat[15] = -1;
	frame = capture();
	ASSERT_NE(frame, nullptr);
	EXPECT_TRUE(frame->edges.empty());
	polygonPreview::clear();
	ctx.matrices[0].mat[15] = 0;
	ctx.matrices[0].mat[11] = 1; // w = z; one vertex crosses the camera plane.
	ctx.verts[4].z = -1;
	frame = capture();
	ASSERT_NE(frame, nullptr);
	ASSERT_FALSE(frame->edges.empty());
	for (auto edge : frame->edges)
		for (auto p : {edge.a, edge.b})
			EXPECT_TRUE(std::isfinite(p.x) && std::isfinite(p.y));
}

TEST_F(PolygonPreviewTest, RotatesPortraitGames)
{
	config::Rotate90 = true;
	strip(ctx.global_param_op, {{0, 0, 1}, {640, 0, 1}, {0, 480, 1}});
	auto frame = capture();
	ASSERT_NE(frame, nullptr);
	ASSERT_FALSE(frame->edges.empty());
	EXPECT_FLOAT_EQ(frame->aspectRatio, 0.75f);
	EXPECT_EQ(frame->edges[0].a, glm::vec2(0, 1));
	EXPECT_EQ(frame->edges[0].b, glm::vec2(0, 0));
}
