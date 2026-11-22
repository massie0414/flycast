# Flycast

[![Android CI](https://github.com/flyinghead/flycast/actions/workflows/android.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/android.yml)
[![C/C++ CI](https://github.com/flyinghead/flycast/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/c-cpp.yml)
[![Nintendo Switch CI](https://github.com/flyinghead/flycast/actions/workflows/switch.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/switch.yml)
[![Windows UWP CI](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml)
[![BSD CI](https://github.com/flyinghead/flycast/actions/workflows/bsd.yml/badge.svg)](https://github.com/flyinghead/flycast/actions/workflows/bsd.yml)

<img src="shell/linux/flycast.png" alt="flycast logo" width="150"/>

**Flycast** is a multi-platform Sega Dreamcast, Naomi, Naomi 2, and Atomiswave emulator derived from [**reicast**](https://github.com/skmp/reicast-emulator).

Information about configuration and supported features can be found on [**TheArcadeStriker's flycast wiki**](https://github.com/TheArcadeStriker/flycast-wiki/wiki).

Join us on our [**Discord server**](https://discord.gg/X8YWP8w) for a chat.

## Downloads ![android](https://flyinghead.github.io/flycast-builds/android.jpg) ![windows](https://flyinghead.github.io/flycast-builds/windows.png) ![linux](https://flyinghead.github.io/flycast-builds/ubuntu.png) ![apple](https://flyinghead.github.io/flycast-builds/apple.png) ![switch](https://flyinghead.github.io/flycast-builds/switch.png) ![xbox](https://flyinghead.github.io/flycast-builds/xbox.png)

Get builds for your system from the [**builds page**](https://flyinghead.github.io/flycast-builds/) or [**GitHub Releases**](https://github.com/flyinghead/flycast/releases).

- **Latest master builds:** regular builds from the `master` branch with recent fixes and updates.
- **Nightly dev builds:** experimental builds with the latest features and changes.
- **Stable tagged releases:** versioned release builds published on GitHub Releases.

Automated test results are available from the builds page as well.

## Install

### Android ![android](https://flyinghead.github.io/flycast-builds/android.jpg)

&emsp;Install Flycast from [**Google Play**](https://play.google.com/store/apps/details?id=com.flycast.emulator).

### Flatpak (Linux ![ubuntu logo](https://flyinghead.github.io/flycast-builds/ubuntu.png))

1. [Set up Flatpak](https://www.flatpak.org/setup/).

2. Install Flycast from [Flathub](https://flathub.org/apps/details/org.flycast.Flycast):

&emsp;`flatpak install -y org.flycast.Flycast`

3. Run Flycast:

&emsp;`flatpak run org.flycast.Flycast`

### Homebrew (macOS ![apple logo](https://flyinghead.github.io/flycast-builds/apple.png))

1. [Set up Homebrew](https://brew.sh) or run `brew update` if already installed.

2. Choose one channel:

| Channel              | Install command                                         |
| -------------------- | ------------------------------------------------------- |
| Master (recommended) | `brew install --cask flyinghead/flycast/flycast@master` |
| Stable               | `brew install --cask flyinghead/flycast/flycast`        |
| Nightly dev          | `brew install --cask flyinghead/flycast/flycast@dev`    |

3. Run Flycast from your Application folder

&emsp;See the <a href="https://github.com/flyinghead/homebrew-flycast#readme">Flycast tap</a> for updating, uninstalling, and switching channels.

### iOS

&emsp;Due to persistent harassment from an iOS user, support for this platform has been dropped.

### Xbox One/Series ![xbox logo](https://flyinghead.github.io/flycast-builds/xbox.png)

&emsp;Grab the latest build from [**the builds page**](https://flyinghead.github.io/flycast-builds/), or the [**GitHub Actions**](https://github.com/flyinghead/flycast/actions/workflows/uwp.yml). Then install it using the **Xbox Device Portal**.

## Build from source

### macOS

&emsp;Right-click the bootstrap script and choose **Open**:

&emsp;`shell/apple/generate_xcode_project.command`

### Windows

&emsp;Double-click the bootstrap script:

&emsp;`shell\windows\generate_vs_project.bat`

The script prefers Visual Studio 2026 and falls back to Visual Studio 2022.
Install the **Desktop development with C++** workload, including a Windows SDK
and **C++ CMake tools for Windows**. Visual Studio 2026 requires
[CMake 4.2 or newer](https://cmake.org/cmake/help/v4.2/generator/Visual%20Studio%2018%202026.html).

For Visual Studio 2026, the generated solution is `build\vs2026\flycast.slnx`
(or `flycast.sln`, depending on the CMake version).
Open it, select **Release | x64** (or **Debug | x64**), and build the solution.
The executable is written to `build\vs2026\Release\flycast.exe` (or `Debug`).
Direct3D 9 is disabled to avoid requiring the legacy DirectX SDK; Direct3D 11,
OpenGL, and Vulkan remain enabled.

To generate without opening the IDE, run
`shell\windows\generate_vs_project.bat --no-open`.
Alternatively, from a Developer PowerShell with CMake on `PATH`:

```powershell
git submodule update --init --recursive
cmake --preset vs2026-x64
cmake --build --preset vs2026-release
# For a debug build: cmake --build --preset vs2026-debug
```

### Linux

#### Dependencies

- **C/C++ compiler toolchain** (e.g. `gcc`/`g++`)
- **CMake**
- **make**
- **libcurl** (development headers)
- **libudev** (development headers)
- **SDL2** (development headers)
- **Graphics API**: Vulkan, OpenGL

#### Build

```
$ git clone --recursive https://github.com/flyinghead/flycast.git
$ cd flycast
$ mkdir build && cd build
$ cmake ..
$ make
```

## Packaging status

[![Packaging status](https://repology.org/badge/vertical-allrepos/flycast.svg)](https://repology.org/project/flycast/versions)
