# ykkz000's DLL Plugin Loader

[简体中文](README.zh_cn.md)

Replaces the *Gathering Storm* (Expansion 2) GameCore DLL with a proxy loader. The loader loads the original `GameCore_XP2_FinalRelease.dll`, installs the shared hooking service, and then discovers and initializes the ykkz000 DLL plugins shipped by every **enabled** mod.

Recognized plugins export `GetPlugin`, `DestroyPlugin`, and `GetPluginManifest`; the loader builds the dependency graph from the manifests and initializes the plugins in dependency order. Plugin directories are discovered under the loader's own mod, the Documents `Mods` directory, and Steam Workshop content (AppID 289070), but only for mods that are currently enabled.

## Requirements

- *Sid Meier's Civilization VI* with the **Gathering Storm (Expansion 2)** ruleset. The mod only replaces the `Expansion2` GameCore row (see `Data/YKKZ000_GameCores.sql`).
- Enable this mod **together with** the plugin mods it must load (for example `ykkz000's DLL Plugin API` and `ykkz000's Soviet Union (Rework)`). Only enabled mods' plugin directories are scanned.

## Build Environment

- Windows x64.
- PowerShell 5.1 or later (`.tools/build.ps1` starts with `#Requires -Version 5.1`).
- CMake 3.21 or later (`cmake_minimum_required(VERSION 3.21)` in `natives/CMakeLists.txt`).
- MSVC with the v143 toolset, C++20, x64. The provided `natives/CMakePresets.json` presets (`x64-v143`, `x64-ninja`) can be used for a manual CMake configure.
- vcpkg (manifest mode), used to obtain the third-party libraries (MinHook and Apache log4cxx with its transitive dependencies). Clone and bootstrap vcpkg, then set `VCPKG_ROOT`:
  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  & C:\vcpkg\bootstrap-vcpkg.bat
  [Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\vcpkg', 'User')
  ```
  The manifest `natives/vcpkg.json` installs the dependencies automatically during the CMake configure (into `natives/build/vcpkg_installed`). The default triplet is `x64-windows-static` (static CRT, matching `/MT`). Use `--vcpkg-root <path>` / `--vcpkg-triplet <triplet>` to override the defaults.

The loader mod has no art assets, so a full build does not require the Civilization VI SDK/Assets or the asset cooker; pass `--skip=art` (or rely on the build skipping art automatically when none is present).

## Building from Source

Build with `.tools/build.ps1`. Quote the project file because its path contains spaces and an apostrophe:

```powershell
# Build the loader DLL and generate the .modinfo
& ".\.tools\build.ps1" "ykkz000's DLL Plugin Loader.civ6proj" --skip=art

# Native code unchanged, only regenerate the mod package
& ".\.tools\build.ps1" "ykkz000's DLL Plugin Loader.civ6proj" --skip="dll,art"
```

- Default output: `<Documents>\My Games\Sid Meier's Civilization VI\Mods\ykkz000's DLL Plugin Loader`; override with `-o <path>` / `--output <path>`.
- Deployed artifact: `Binaries/Win64/GameCore_YKKZ000_Loader_XP2_FinalRelease.dll`. The `--dll-prefix` must match the `DllPrefix` in `Data/YKKZ000_GameCores.sql`.

## License

Licensed under the MIT License (see `LICENSE`). The license applies to the source code only; third-party components statically linked into the loader are covered by `NOTICE` and `THIRD_PARTY_NOTICES.txt`.
