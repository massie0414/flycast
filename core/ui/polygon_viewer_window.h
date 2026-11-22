#pragma once

namespace polygonViewerWindow
{
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
constexpr bool supported = true;
bool update(bool enabled);
void term();
bool hasFocus();
#else
constexpr bool supported = false;
inline bool update(bool) { return true; }
inline void term() {}
inline bool hasFocus() { return false; }
#endif
}
