#pragma once

/// @file export.h
/// @brief Plugin export macro definitions.
/// @note The loader's exports are implemented in proxy.cpp and given ordinals by loader.def
///       (155/156/157/158), so no corresponding dllexport branch is needed.

/// @def YKKZ000_PLUGIN_API
/// @brief Declares a plugin symbol exported with the C ABI (extern "C" __declspec(dllexport)).
#define YKKZ000_PLUGIN_API extern "C" __declspec(dllexport)
