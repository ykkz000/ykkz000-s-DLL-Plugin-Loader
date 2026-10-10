#pragma once

#include <cstddef>

/// @file serialization_trace.h
/// @brief Loader-internal serialization read trace (diagnostics).
/// @note The trace is compiled out in non-_DEBUG builds: install/uninstall become no-ops and the
///       crash dump writes nothing. It records the values the engine's AutoVariable serialization
///       helpers transfer into a fixed-size static ring buffer, which the crash VEH dumps to
///       YKKZ000_crash.log so a stream-derailment point can be located.
namespace ykkz000::loader {

/// @brief Writer callback used by the crash dump: receives a text chunk and its byte length.
/// @note The callback must not allocate or throw; see crash_capture.cpp's crashWrite.
using SerializationTraceWriter = void (*)(const char* text, std::size_t length);

/// @brief Install the serialization read-trace hooks (idempotent; no-op in release builds).
/// @note Requires the resolved GameCore entry points (call after ensureGameCoreLoaded).
void installSerializationTrace();
/// @brief Remove the serialization read-trace hooks (no-op in release builds).
void uninstallSerializationTrace();
/// @brief Dump the trace ring buffer through @p write, oldest record first (no-op in release).
/// @note Called from the crash VEH: it must not allocate or take locks.
void dumpSerializationTrace(SerializationTraceWriter write);

} // namespace ykkz000::loader
