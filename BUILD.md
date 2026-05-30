# Building TranscribeAI

This produces a single self-contained `TranscribeAI.exe` (no external DLLs).

## Prerequisites

- Visual Studio 2022 or VS 2022 Build Tools with the "Desktop development with C++"
  workload (MSVC 19.4x, which has full `std::expected` support).
- CMake 3.25+ (the copy bundled with VS works; this project was verified with 3.31).
- A git clone of vcpkg (manifest mode needs a git-based ports tree for the
  `builtin-baseline` pinned in `vcpkg.json`).

## One-time vcpkg setup

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "C:\dev\vcpkg"
```

> The vcpkg instance bundled inside VS Build Tools is not a git repository, so it
> cannot resolve a manifest baseline. Use a real `git clone` as above.

The dependencies (`imgui[glfw-binding,opengl3-binding]`, `glfw3`, `curl`,
`nlohmann-json`, `miniaudio`, plus `zlib`) are installed automatically from
`vcpkg.json` during the CMake configure step. No manual `vcpkg install` is needed.

## Configure and build (Release)

```powershell
$env:VCPKG_ROOT = "C:\dev\vcpkg"
cd C:\dev\TranscribeAI

cmake --preset windows-msvc-release
cmake --build --preset release
```

Output: `build/release/Release/TranscribeAI.exe` (verified ~2 MB, links only
Windows system DLLs).

### Debug build

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset debug
# Output: build/debug/Debug/TranscribeAI.exe
```

## Usage

1. Launch `TranscribeAI.exe`. The Settings panel slides open on startup.
2. Paste your OpenAI API key, then click **Save key** (encrypted with Windows
   DPAPI under your user account).
3. Pick a model (default `gpt-4o-mini-transcribe`).
4. Click **Start** and play any audio on the PC. Finalized transcript lines
   appear and auto-scroll.
5. Click **Stop** to end the session.

The key is stored encrypted at `%APPDATA%\TranscribeAI\key.dat` and is decrypted
and prefilled automatically on the next launch.
