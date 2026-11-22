#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <unordered_map>

namespace texturePreview
{
struct VramUsage
{
	uint32_t usedBytes = 0;
	uint32_t peakBytes = 0;
	uint32_t totalBytes = 0;
	uint32_t remainingBytes() const { return totalBytes - usedBytes; }
};

// Counts the union of referenced VRAM ranges, not decoded host/GPU images.
// Reference counts prevent palette variants and overlapping textures from
// double-counting memory. The caller provides synchronization.
class VramUsageTracker
{
public:
	void reset(uint32_t capacity)
	{
		ranges.clear();
		coverage.clear();
		coverage.emplace(0, 0);
		coverage.emplace(capacity, 0);
		usage = {0, 0, capacity};
	}

	void track(const void *key, uint32_t start, uint64_t length)
	{
		// Clamp without overflowing, including malformed texture metadata.
		uint32_t end = start < usage.totalBytes
				? start + (uint32_t)std::min<uint64_t>(length, usage.totalBytes - start) : start;
		auto previous = ranges.find(key);
		if (previous != ranges.end())
		{
			if (previous->second.start == start && previous->second.end == end)
				return;
			change(previous->second, false);
			ranges.erase(previous);
		}
		if (start < end)
		{
			Range range{start, end};
			ranges.emplace(key, range);
			change(range, true);
			usage.peakBytes = std::max(usage.peakBytes, usage.usedBytes);
		}
	}

	void remove(const void *key)
	{
		auto entry = ranges.find(key);
		if (entry != ranges.end())
		{
			change(entry->second, false);
			ranges.erase(entry);
		}
	}

	VramUsage snapshot() const { return usage; }

private:
	struct Range { uint32_t start, end; };
	std::unordered_map<const void *, Range> ranges;
	// Each boundary holds the reference count until the next boundary.
	std::map<uint32_t, uint32_t> coverage;
	VramUsage usage;

	auto split(uint32_t address)
	{
		auto next = coverage.lower_bound(address);
		if (next->first == address)
			return next;
		return coverage.emplace_hint(next, address, std::prev(next)->second);
	}

	void change(Range range, bool add)
	{
		auto end = split(range.end);
		auto begin = split(range.start);
		for (auto it = begin; it != end; ++it)
		{
			uint32_t bytes = std::next(it)->first - it->first;
			if (add)
			{
				if (it->second++ == 0)
					usage.usedBytes += bytes;
			}
			else if (--it->second == 0)
				usage.usedBytes -= bytes;
		}
		// Remove obsolete boundaries so repeated uploads don't grow this map.
		if (begin != coverage.begin())
			--begin;
		while (std::next(begin) != coverage.end())
		{
			auto next = std::next(begin);
			if (next->first > range.end || next->first == usage.totalBytes)
				break;
			if (begin->second == next->second)
				coverage.erase(next);
			else
				begin = next;
		}
	}
};
}
