#pragma once

#include "types.h"
#include "texture_vram_usage.h"
#include <memory>
#include <vector>

class BaseTextureCacheData;

// CPU snapshots only: the UI never borrows a game texture's GPU resource.
namespace texturePreview
{
constexpr size_t MaxEntries = 512;
constexpr int MaxDimension = 128;

struct Image
{
	u64 revision;
	u32 address;
	int width, height;
	int previewWidth, previewHeight;
	const char *format;
	bool custom;
	std::vector<u32> pixels; // RGBA, independent of the renderer's color order
};

void setEnabled(bool enabled);
u64 generation(); // zero while disabled
void capture(const BaseTextureCacheData& texture, int width, int height,
		const u8 *data, bool mipmapsIncluded, int validHeight);
void remove(const BaseTextureCacheData *texture);
void trackVram(const BaseTextureCacheData& texture);
void invalidateVram(const BaseTextureCacheData *texture);
VramUsage vramUsage();
std::vector<std::shared_ptr<const Image>> snapshot();
}
