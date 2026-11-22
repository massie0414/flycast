#include <gtest/gtest.h>
#include "rend/TexCache.h"
#include "rend/texture_preview.h"

namespace
{
class PreviewTexture : public BaseTextureCacheData
{
public:
	PreviewTexture() : BaseTextureCacheData(TSP{}, TCW{}, 0) {}
	std::string GetId() override { return "test"; }
	void UploadToGPU(int, int, const u8 *, bool, bool) override {}
};

class TexturePreviewTest : public ::testing::Test
{
protected:
	RenderType previousRenderer;
	void SetUp() override
	{
		previousRenderer = config::RendererType;
		config::RendererType = RenderType::OpenGL;
		texturePreview::setEnabled(true);
	}
	void TearDown() override
	{
		texturePreview::setEnabled(false);
		config::RendererType = previousRenderer;
	}

	u32 capturePixel(PreviewTexture& texture, TextureType type, u32 pixel)
	{
		texture.tex_type = type;
		texturePreview::capture(texture, 1, 1, (const u8 *)&pixel, false, 1);
		return texturePreview::snapshot().front()->pixels.front();
	}
};

TEST_F(TexturePreviewTest, PackedColorsMatchAcrossRenderers)
{
	PreviewTexture texture;
	for (bool directX : {false, true})
	{
		config::RendererType = directX ? RenderType::DirectX11 : RenderType::OpenGL;
		EXPECT_EQ(capturePixel(texture, TextureType::_565, 0xf800), 0xff0000ffu);
		EXPECT_EQ(capturePixel(texture, TextureType::_5551, directX ? 0xfc00 : 0xf801), 0xff0000ffu);
		EXPECT_EQ(capturePixel(texture, TextureType::_4444, directX ? 0xaf21 : 0xf21a), 0xaa1122ffu);
		EXPECT_EQ(capturePixel(texture, TextureType::_8888, directX ? 0xaaff2211 : 0xaa1122ff), 0xaa1122ffu);
	}
}

TEST_F(TexturePreviewTest, PaletteSelectionIsApplied)
{
	PreviewTexture texture;
	texture.tcw.PixelFmt = PixelPal4;
	texture.tcw.PalSelect = 2;
	const u32 previous = palette32_ram[35];
	palette32_ram[35] = 0xff112233;
	EXPECT_EQ(capturePixel(texture, TextureType::_8, 3), 0xff112233u);
	palette32_ram[35] = previous;
}

TEST_F(TexturePreviewTest, PreviewsUseTopDownRowsAcrossRenderers)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_8888;
	// Asymmetric grayscale pixels distinguish every row and column in either
	// RGBA or BGRA order. Custom replacements have the same source orientation.
	const std::vector<u32> pixels{
		0xff111111, 0xff222222,
		0xff333333, 0xff444444,
		0xff555555, 0xff666666,
	};
	const std::vector<u32> expected{
		0xff555555, 0xff666666,
		0xff333333, 0xff444444,
		0xff111111, 0xff222222,
	};
	for (RenderType renderer : {RenderType::OpenGL, RenderType::Vulkan, RenderType::DirectX11})
		for (bool custom : {false, true})
		{
			config::RendererType = renderer;
			texture.is_custom_replaced = custom;
			texturePreview::capture(texture, 2, 3, (const u8 *)pixels.data(), false, 3);
			EXPECT_EQ(texturePreview::snapshot().front()->pixels, expected);
		}
}

TEST_F(TexturePreviewTest, DownsampledPreviewStartsAtTopSourceRow)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_8888;
	std::vector<u32> pixels(2 * 256, 0xff000000);
	pixels[0] = pixels[2] = 0xff0000ff; // Bottom two rows.
	pixels[1] = pixels[3] = 0xff00ff00;
	pixels[2 * 255] = 0xffff0000; // Top row must not be skipped.
	pixels[2 * 255 + 1] = 0xffffffff;
	texturePreview::capture(texture, 2, 256, (const u8 *)pixels.data(), false, 256);
	const auto image = texturePreview::snapshot().front();
	ASSERT_EQ(image->previewWidth, 1);
	ASSERT_EQ(image->previewHeight, 128);
	EXPECT_EQ(image->pixels.front(), 0xffff0000u);
	EXPECT_EQ(image->pixels.back(), 0xff0000ffu);
}

TEST_F(TexturePreviewTest, UsesLargestMipmap)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_8888;
	// 1x1 + 2x2 + 4x4, smallest level first.
	std::vector<u32> pixels(21, 0xff0000ff);
	std::fill(pixels.begin() + 5, pixels.end(), 0xff00ff00);
	texturePreview::capture(texture, 4, 4, (const u8 *)pixels.data(), true, 4);
	const auto image = texturePreview::snapshot().front();
	ASSERT_EQ(image->pixels.size(), 16u);
	for (u32 pixel : image->pixels)
		EXPECT_EQ(pixel, 0xff00ff00u);
}

TEST_F(TexturePreviewTest, BoundsThumbnailAndPreservesAspectRatio)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_8888;
	std::vector<u32> pixels(512 * 256, 0xff123456);
	texturePreview::capture(texture, 512, 256, (const u8 *)pixels.data(), false, 256);
	const auto image = texturePreview::snapshot().front();
	EXPECT_EQ(image->width, 512);
	EXPECT_EQ(image->height, 256);
	EXPECT_EQ(image->previewWidth, 128);
	EXPECT_EQ(image->previewHeight, 64);
	EXPECT_EQ(image->pixels.back(), 0xff123456u);
}

TEST_F(TexturePreviewTest, DoesNotReadBeyondValidRows)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_8888;
	u32 pixel = 0xff123456;
	texturePreview::capture(texture, 1, 2, (const u8 *)&pixel, false, 1);
	const auto image = texturePreview::snapshot().front();
	ASSERT_EQ(image->pixels.size(), 2u);
	EXPECT_EQ(image->pixels[0], 0u);
	EXPECT_EQ(image->pixels[1], pixel);
}

TEST_F(TexturePreviewTest, SnapshotsSurviveUpdatesAndDeletion)
{
	PreviewTexture texture;
	capturePixel(texture, TextureType::_8888, 0xff0000ff);
	const auto previous = texturePreview::snapshot().front();
	capturePixel(texture, TextureType::_8888, 0xff00ff00);
	EXPECT_EQ(previous->pixels[0], 0xff0000ffu);
	EXPECT_NE(previous->revision, texturePreview::snapshot().front()->revision);
	texture.Delete();
	EXPECT_TRUE(texturePreview::snapshot().empty());
	EXPECT_EQ(previous->pixels[0], 0xff0000ffu);
}

TEST_F(TexturePreviewTest, DisabledCaptureClearsDataAndReopeningStartsNewGeneration)
{
	PreviewTexture texture;
	capturePixel(texture, TextureType::_8888, 0xff0000ff);
	const u64 generation = texturePreview::generation();
	texturePreview::setEnabled(false);
	u32 pixel = 0;
	texturePreview::capture(texture, 1, 1, (const u8 *)&pixel, false, 1);
	EXPECT_TRUE(texturePreview::snapshot().empty());
	EXPECT_EQ(texturePreview::generation(), 0u);
	texturePreview::setEnabled(true);
	EXPECT_GT(texturePreview::generation(), generation);
}

TEST_F(TexturePreviewTest, EvictsOldestPreviewAtLimit)
{
	std::vector<std::unique_ptr<PreviewTexture>> textures;
	for (size_t i = 0; i <= texturePreview::MaxEntries; i++)
	{
		auto texture = std::make_unique<PreviewTexture>();
		texture->startAddress = (u32)i;
		capturePixel(*texture, TextureType::_8888, 0xff0000ff);
		textures.push_back(std::move(texture));
	}
	const auto images = texturePreview::snapshot();
	EXPECT_EQ(images.size(), texturePreview::MaxEntries);
	EXPECT_EQ(images.front()->address, 1u);
}

TEST_F(TexturePreviewTest, EnablingViewerDoesNotOverwriteRenderTargets)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_565;
	texture.dirty = 0;
	texture.markRenderTarget();
	EXPECT_FALSE(texture.NeedsUpdate());
	texturePreview::setEnabled(false);
	texturePreview::setEnabled(true);
	EXPECT_FALSE(texture.NeedsUpdate());
	texture.dirty = 1;
	EXPECT_TRUE(texture.NeedsUpdate());
}

TEST_F(TexturePreviewTest, EnablingViewerRefreshesAlreadyCachedTextures)
{
	PreviewTexture texture;
	texture.tex_type = TextureType::_565;
	texture.dirty = 0;
	EXPECT_TRUE(texture.NeedsUpdate());
	texturePreview::setEnabled(false);
	EXPECT_FALSE(texture.NeedsUpdate());
}
}
