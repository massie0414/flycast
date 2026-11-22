#include "texture_preview.h"
#include "TexCache.h"
#include <mutex>

namespace texturePreview
{
namespace
{
std::mutex mutex;
std::atomic<u64> activeGeneration{0};
u64 nextGeneration = 0;
u64 nextRevision = 0;
std::unordered_map<const BaseTextureCacheData *, std::shared_ptr<const Image>> images;

u32 rgba(u32 color, bool directX)
{
	if (directX)
		return (color & 0xff00ff00) | ((color & 0xff) << 16) | ((color >> 16) & 0xff);
	return color;
}
}

void setEnabled(bool enabled)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (enabled == (activeGeneration.load() != 0))
		return;
	images.clear();
	activeGeneration = enabled ? ++nextGeneration : 0;
}

u64 generation() {
	return activeGeneration.load(std::memory_order_relaxed);
}

void remove(const BaseTextureCacheData *texture)
{
	if (generation() == 0)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	images.erase(texture);
}

void capture(const BaseTextureCacheData& texture, int width, int height,
		const u8 *data, bool mipmapsIncluded, int validHeight)
{
	const u64 captureGeneration = generation();
	if (captureGeneration == 0 || data == nullptr || width <= 0 || height <= 0)
		return;
	auto image = std::make_shared<Image>();
	image->address = texture.startAddress;
	image->width = width;
	image->height = height;
	image->format = texture.GetPixelFormatName();
	image->custom = texture.is_custom_replaced;
	const int largest = std::max(width, height);
	image->previewWidth = std::max(1, width * std::min(largest, MaxDimension) / largest);
	image->previewHeight = std::max(1, height * std::min(largest, MaxDimension) / largest);
	image->pixels.resize(image->previewWidth * image->previewHeight);

	// Flycast's decoded mip chains start with the smallest level.
	size_t offset = 0;
	if (mipmapsIncluded)
		for (int w = width / 2, h = height / 2; w > 0 && h > 0; w /= 2, h /= 2)
			offset += w * h;
	const bool directX = isDirectX(config::RendererType);
	const u32 palette = texture.tcw.PixelFmt == PixelPal4
			? texture.tcw.PalSelect * 16 : (texture.tcw.PalSelect >> 4) * 256;
	for (int y = 0; y < image->previewHeight; y++)
	{
		const int sourceY = y * height / image->previewHeight;
		for (int x = 0; x < image->previewWidth; x++)
		{
			const size_t index = offset + sourceY * width + x * width / image->previewWidth;
			u32 color = 0;
			if (sourceY < validHeight)
			{
				switch (texture.tex_type)
				{
				case TextureType::_8888:
					color = rgba(reinterpret_cast<const u32 *>(data)[index], directX);
					break;
				case TextureType::_8:
					color = rgba(palette32_ram[palette + data[index]], directX);
					break;
				case TextureType::_565:
					color = Unpacker565_32<RGBAPacker>::unpack(reinterpret_cast<const u16 *>(data)[index]);
					break;
				case TextureType::_5551:
				{
					u16 pixel = reinterpret_cast<const u16 *>(data)[index];
					if (!directX)
						pixel = (pixel >> 1) | ((pixel & 1) << 15);
					color = Unpacker1555_32<RGBAPacker>::unpack(pixel);
					break;
				}
				case TextureType::_4444:
				{
					u16 pixel = reinterpret_cast<const u16 *>(data)[index];
					if (!directX)
						pixel = (pixel >> 4) | ((pixel & 15) << 12);
					color = Unpacker4444_32<RGBAPacker>::unpack(pixel);
					break;
				}
				}
			}
			image->pixels[y * image->previewWidth + x] = color;
		}
	}
	std::lock_guard<std::mutex> lock(mutex);
	if (generation() != captureGeneration)
		return;
	image->revision = ++nextRevision;
	images[&texture] = std::move(image);
	if (images.size() > MaxEntries)
	{
		auto oldest = std::min_element(images.begin(), images.end(), [](const auto& a, const auto& b) {
			return a.second->revision < b.second->revision;
		});
		images.erase(oldest);
	}
}

std::vector<std::shared_ptr<const Image>> snapshot()
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<std::shared_ptr<const Image>> result;
	result.reserve(images.size());
	for (const auto& entry : images)
		result.push_back(entry.second);
	std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
		if (a->address != b->address)
			return a->address < b->address;
		return a->revision < b->revision;
	});
	return result;
}
}
