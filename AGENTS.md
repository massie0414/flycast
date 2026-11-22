# Windows development

- Use one Visual Studio 2026 build tree: `build/vs2026` with the
  `vs2026-x64` CMake preset. The Flycast solution is
  `build/vs2026/flycast.slnx` (or `.sln` with older CMake).
- Do not create feature-specific build directories, duplicate Flycast
  solutions, or copied application executables for smoke tests. All features
  belong to the existing Flycast target and solution.
- Build and deliver the current application at
  `build/vs2026/Release/flycast.exe`. Before finishing a code change, run
  `shell/windows/generate_vs_project.bat --no-open` or configure and build with
  the `vs2026-x64` / `vs2026-release` presets. Never report an older executable
  as updated when the build failed.
- The Windows application preset explicitly disables the configuration that
  replaces Flycast with the unit-test runner. If using a temporary test
  configuration, restore the application preset and rebuild Release before
  delivering the result.
- Preserve `emu.cfg`, `data`, mappings, BIOS files, and save data when cleaning
  generated build files. Refresh a relocated CMake cache in the same build
  directory rather than starting another solution.
- `shell/windows/generate_vs_project.bat --run` builds the current sources
  successfully before launching the canonical Release executable.
