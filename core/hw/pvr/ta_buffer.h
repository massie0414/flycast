#pragma once

#include "ta_structs.h"
#include <algorithm>
#include <vector>

// Diagnostic only: do not use these estimates to advance TA_ITP_CURRENT or
// raise overflow interrupts. Flycast does not generate native ISP/TSP data.
// Format reference: Dreamcast/Dev.Box System Architecture, sections 3.7.3.1,
// 3.7.9 and 8.4.3 (strip partitioning, parameter formats and register ranges).
inline u64 estimateTaParameterBytes(PCW pcw, u32 vertices, u32 stripTriangles)
{
	if (vertices < 3)
		return 0;
	const u32 volumes = pcw.Volume ? 2 : 1;
	const u32 uvBytes = pcw.Texture ? (pcw.UV_16bit ? 4 : 8) : 0;
	const u32 colorBytes = 4 + (pcw.Texture && pcw.Offset ? 4 : 0);
	const u32 vertexBytes = 12 + volumes * (uvBytes + colorBytes);
	// Allow for both TSP/TCW pairs in two-volume objects. Colors are packed
	// even when the submitted TA vertices contain floating point colors.
	const u32 headerBytes = 4 + volumes * 8;
	if (pcw.ParaType == 5) // Sprite: one native quad, including unused fields.
		return headerBytes + u64(4) * vertexBytes;

	const u64 triangles = vertices - 2;
	stripTriangles = std::max(1u, std::min(6u, stripTriangles));
	const u64 strips = (triangles + stripTriangles - 1) / stripTriangles;
	// Each hardware strip repeats its header and two leading vertices.
	return strips * headerBytes + (triangles + strips * 2) * vertexBytes;
}

struct TaBufferAllocation
{
	u32 base = 0;
	u32 capacityBytes = 0;
	bool valid = false;
	bool continuation = false;

	static TaBufferAllocation fromRegisters(u32 base, u32 limit, u32 vramSize, bool continuation = false)
	{
		// Preserve the end address (8 MiB is a legitimate limit on Dreamcast).
		// Applying VRAM_MASK would wrap that limit to zero.
		base &= 0x00fffffc;
		limit &= 0x00fffffc;
		return {base, limit > base ? limit - base : 0,
				base < limit && limit <= vramSize, continuation};
	}
};

struct TaBufferUsage
{
	TaBufferAllocation allocation;
	u64 usedBytes = 0;

	u64 remainingBytes() const {
		return usedBytes < allocation.capacityBytes ? allocation.capacityBytes - usedBytes : 0;
	}

	double fraction() const {
		return allocation.capacityBytes == 0 ? 0.0 : double(usedBytes) / allocation.capacityBytes;
	}
};

struct TaBufferStats
{
	std::vector<TaBufferUsage> buffers;
	bool complete = true;

	void add(const TaBufferAllocation& allocation, u64 bytes)
	{
		if (!allocation.valid)
		{
			complete = false;
			return;
		}
		for (TaBufferUsage& buffer : buffers)
		{
			if (buffer.allocation.base != allocation.base)
				continue;
			if (allocation.continuation && buffer.allocation.capacityBytes == allocation.capacityBytes)
				buffer.usedBytes += bytes;
			else
				// Reinitialization or a changing limit cannot be reconstructed
				// reliably from the contexts retained for this screen render.
				complete = false;
			return;
		}
		// A continuation without its initial context has uncounted earlier data.
		if (allocation.continuation)
			complete = false;
		buffers.push_back({allocation, bytes});
	}
};
