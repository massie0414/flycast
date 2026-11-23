#pragma once

namespace resourceUsageWindow
{
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
constexpr bool supported = true;
// Called on the main UI thread. False means the user closed the window.
bool update(bool enabled, bool running);
void term();
bool hasFocus();
#else
constexpr bool supported = false;
inline bool update(bool, bool) { return true; }
inline void term() {}
inline bool hasFocus() { return false; }
#endif
}
