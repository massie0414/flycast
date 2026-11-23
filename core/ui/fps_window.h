#pragma once

namespace fpsWindow
{
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
constexpr bool detached = true;
// Main UI thread only. False means the user closed the window.
bool update(bool enabled, bool running, bool fastForward);
void frameRendered();
void term();
bool hasFocus();
#else
constexpr bool detached = false;
inline bool update(bool, bool, bool) { return true; }
inline void frameRendered() {}
inline void term() {}
inline bool hasFocus() { return false; }
#endif
}
