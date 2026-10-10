#pragma once

/// @file log.h
/// @brief Log levels shared by the Loader and every plugin.
/// @note The numeric values travel across the bridge::LogFn ABI (an int argument); they must stay
///       stable and both sides must be deployed together.
namespace ykkz000::bridge {

/// @brief Log4j-style levels. The numeric values are part of the writer's filter; keep them stable.
/// @note TRACE is provided for completeness but is never used in this project.
enum class LogLevel : int {
  kTrace = 0,   ///< Finest granularity; reserved, never used.
  kDebug = 1,   ///< Addresses, pointers, descriptors, size/capacity/data dumps.
  kInfo = 2,    ///< Lifecycle, registration, state changes.
  kWarning = 3, ///< Recovered anomalies, degraded behavior.
  kError = 4,   ///< Failures that break a feature (hooks, registration, plugin load).
  kFatal = 5,   ///< Failures that make the mod unusable (MinHook init, core resolution).
};

/// @brief Numeric value of a level (the bridge ABI keeps an int).
/// @param[in] level Log level.
/// @return The level as an int.
constexpr int ToInt(LogLevel level) { return static_cast<int>(level); }

/// @brief Uppercase name written into the log-line prefix.
/// @param[in] level Log level.
/// @return A static string; "UNKNOWN" for an out-of-range value.
constexpr const char* LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace:
      return "TRACE";
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarning:
      return "WARNING";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kFatal:
      return "FATAL";
  }
  return "UNKNOWN";
}

// Compile-time gate: without _DEBUG only INFO and above survive; DEBUG/TRACE calls are
// eliminated entirely so their arguments are never evaluated (zero cost in Release).
#if defined(_DEBUG)
constexpr LogLevel kCompileMinLevel = LogLevel::kDebug;
#else
constexpr LogLevel kCompileMinLevel = LogLevel::kInfo;
#endif

} // namespace ykkz000::bridge
