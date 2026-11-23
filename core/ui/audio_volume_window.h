#pragma once

namespace audioVolumeWindow
{
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
constexpr bool supported = true;
// Main UI thread only. False means the user closed the window.
bool update(bool enabled, bool running, int volume);
void term();
bool hasFocus();
#else
constexpr bool supported = false;
inline bool update(bool, bool, int) { return true; }
inline void term() {}
inline bool hasFocus() { return false; }
#endif
}
