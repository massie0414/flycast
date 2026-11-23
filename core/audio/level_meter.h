#pragma once

#include <array>
#include <cstdint>

namespace audioMeter
{
enum Source { Output, Aica, Cdda, Dsp, Vmu, SourceCount };
using Levels = std::array<std::array<float, 2>, SourceCount>;

#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
// The UI enables collection; only the emulation thread calls beginSample/record.
// A generation token prevents samples from a previous session being published.
void setEnabled(bool enabled);
std::uint32_t beginSample();
void record(std::uint32_t token, const Levels& samples);
// Consume the peaks since the last read, normalized to 16-bit full scale.
Levels consume();
#else
inline void setEnabled(bool) {}
inline std::uint32_t beginSample() { return 0; }
inline void record(std::uint32_t, const Levels&) {}
inline Levels consume() { return {}; }
#endif
}
