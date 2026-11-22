#pragma once

namespace textureViewerWindow
{
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
constexpr bool detached = true;
// Called on the main UI thread. False means the user closed the viewer.
bool update(bool enabled);
void term();
bool hasFocus();
#else
constexpr bool detached = false;
inline bool update(bool) { return true; }
inline void term() {}
inline bool hasFocus() { return false; }
#endif
}
