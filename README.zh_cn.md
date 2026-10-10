# ykkz000的DLL 插件加载器

[English](README.md)

用代理加载器替换《风云变幻》（Expansion 2）的 GameCore DLL。加载器会加载原版 `GameCore_XP2_FinalRelease.dll`，安装共享的 Hook 服务，然后从所有**已启用**模组中发现并初始化 ykkz000 DLL 插件。

可识别的插件需导出 `GetPlugin`、`DestroyPlugin` 与 `GetPluginManifest`；加载器根据插件清单构建依赖图，并按依赖顺序初始化插件。插件目录从加载器自身模组、文档 `Mods` 目录与 Steam 创意工坊内容（AppID 289070）中发现，但仅限当前已启用的模组。

## 依赖

- 需《席德·梅尔的文明 VI》的**风云变幻（Expansion 2）**规则集。本模组仅替换 `Expansion2` 的 GameCore 行（见 `Data/YKKZ000_GameCores.sql`）。
- 请将本模组与需要加载的插件模组**同时启用**（例如 `ykkz000的DLL 插件 API` 与 `ykkz000的苏联（重做版）`）。只扫描已启用模组的插件目录。

## 构建环境配置

- Windows x64。
- PowerShell 5.1 或更高版本（`.tools/build.ps1` 以 `#Requires -Version 5.1` 开头）。
- CMake 3.21 或更高版本（`natives/CMakeLists.txt` 中的 `cmake_minimum_required(VERSION 3.21)`）。
- MSVC 的 v143 工具集、C++20、x64。可使用 `natives/CMakePresets.json` 提供的 `x64-v143`、`x64-ninja` preset 手动进行 CMake 配置。
- vcpkg（清单模式），用于获取第三方库（MinHook 与 Apache log4cxx 及其传递依赖）。克隆并引导 vcpkg，然后设置 `VCPKG_ROOT`：
  ```powershell
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  & C:\vcpkg\bootstrap-vcpkg.bat
  [Environment]::SetEnvironmentVariable('VCPKG_ROOT', 'C:\vcpkg', 'User')
  ```
  清单 `natives/vcpkg.json` 会在 CMake 配置时自动安装依赖（到 `natives/build/vcpkg_installed`）。默认 triplet 为 `x64-windows-static`（静态 CRT，与 `/MT` 一致）。可用 `--vcpkg-root <path>` / `--vcpkg-triplet <triplet>` 覆盖默认值。

本模组无美术资源，因此完整构建不需要《文明 VI》SDK/Assets 与资源 cooker；传入 `--skip=art`（或在无美术时自动跳过美术）即可。

## 从源码构建

使用 `.tools/build.ps1` 构建。项目文件路径包含空格与撇号，因此需要加引号：

```powershell
# 构建 loader DLL 并生成 .modinfo
& ".\.tools\build.ps1" "ykkz000's DLL Plugin Loader.civ6proj" --skip=art

# 未改 native 代码，仅重新生成模组包
& ".\.tools\build.ps1" "ykkz000's DLL Plugin Loader.civ6proj" --skip="dll,art"
```

- 默认输出：`<Documents>\My Games\Sid Meier's Civilization VI\Mods\ykkz000's DLL Plugin Loader`；可用 `-o <path>` / `--output <path>` 覆盖。
- 部署产物：`Binaries/Win64/GameCore_YKKZ000_Loader_XP2_FinalRelease.dll`。`--dll-prefix` 必须与 `Data/YKKZ000_GameCores.sql` 中的 `DllPrefix` 一致。

## 许可证

本项目在 MIT 许可证下授权（见 `LICENSE`）。该许可证仅适用于源代码；静态链接进 loader 的第三方组件见 `NOTICE` 与 `THIRD_PARTY_NOTICES.txt`。
