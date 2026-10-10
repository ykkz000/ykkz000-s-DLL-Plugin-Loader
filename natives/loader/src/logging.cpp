#include <windows.h>

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

#include <log4cxx/fileappender.h>
#include <log4cxx/helpers/transcoder.h>
#include <log4cxx/level.h>
#include <log4cxx/logger.h>
#include <log4cxx/nt/outputdebugstringappender.h>
#include <log4cxx/patternlayout.h>

#include <ykkz000/loader/internal.h>

// Logging backend: log4cxx owns the file appender (append + immediate flush) and the Windows
// debugger appender. The layout and level mapping reproduce the previous self-implemented writer:
//   * line format "[YKKZ000:<LEVEL>] message"
//   * the file path and the YKKZ000_LOG_LEVEL semantics are unchanged
//   * level names match the bridge enum (notably WARNING, whereas log4cxx's built-in is WARN)
// The Loader is the only holder of a log4cxx logger; plugins keep forwarding through host->log.
namespace ykkz000::loader {
namespace {

std::once_flag g_loggingOnce;
log4cxx::LoggerPtr g_logger;
bool g_loggerReady = false;

std::wstring gameLogDirectory() {
  // Prefer the Known Folder API; fall back to the environment variable on failure.
  std::wstring local;
  PWSTR wide = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &wide))) {
    local.assign(wide);
    CoTaskMemFree(wide);
  } else {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      local.assign(buffer, length);
    }
  }
  if (local.empty()) {
    return {};
  }
  const std::wstring dir =
      local + L"\\Firaxis Games\\Sid Meier's Civilization VI\\Logs";
  CreateDirectoryW(dir.c_str(), nullptr); // usually already exists; failure is not fatal
  return dir;
}

// log4cxx names its warning level "WARN"; the bridge/writer ABI and the historical log format use
// "WARNING". A custom level with the same priority (30000) keeps the level order and filter
// behavior while preserving the emitted name.
log4cxx::LevelPtr warningLevel() {
  static const log4cxx::LevelPtr level =
      std::make_shared<log4cxx::Level>(30000, LOG4CXX_STR("WARNING"), 4);
  return level;
}

// Convert UTF-8 text and UTF-16 text to log4cxx's configured LogString (UTF-8 by default on
// Windows, wide for wide-character builds) without relying on the concrete LogString type.
log4cxx::LogString toLogString(const char* value) {
  LOG4CXX_DECODE_CHAR(converted, value);
  return converted;
}

log4cxx::LogString toLogString(const std::wstring& value) {
  LOG4CXX_DECODE_WCHAR(converted, value);
  return converted;
}

log4cxx::LevelPtr toLog4cxxLevel(int level) {
  switch (level) {
    case 0:
      return log4cxx::Level::getTrace();
    case 1:
      return log4cxx::Level::getDebug();
    case 2:
      return log4cxx::Level::getInfo();
    case 3:
      return warningLevel();
    case 4:
      return log4cxx::Level::getError();
    case 5:
      return log4cxx::Level::getFatal();
    default:
      return log4cxx::Level::getTrace();
  }
}

// Parses YKKZ000_LOG_LEVEL ("TRACE"/"DEBUG"/"INFO"/"WARNING"/"ERROR"/"FATAL", case-insensitive, or
// "0".."5"); unknown text yields kTrace so the caller clamps it to kCompileMinLevel.
bridge::LogLevel parseLogLevelName(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    return bridge::LogLevel::kTrace;
  }
  if (text[1] == '\0' && text[0] >= '0' && text[0] <= '5') {
    return static_cast<bridge::LogLevel>(text[0] - '0');
  }
  constexpr const char* kNames[] = {"TRACE", "DEBUG",   "INFO",
                                    "WARNING", "ERROR", "FATAL"};
  for (int level = 0; level <= 5; ++level) {
    const char* name = kNames[level];
    int i = 0;
    for (;; ++i) {
      if (name[i] == '\0' || text[i] == '\0') {
        if (name[i] == '\0' && text[i] == '\0') {
          return static_cast<bridge::LogLevel>(level);
        }
        break;
      }
      char c = text[i];
      if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
      }
      if (c != name[i]) {
        break;
      }
    }
  }
  return bridge::LogLevel::kTrace;
}

// Runtime minimum level, resolved once per process and clamped to the compile-time gate.
log4cxx::LevelPtr runtimeLevel() {
  char value[32] = {};
  bridge::LogLevel parsed = bridge::LogLevel::kTrace;
  if (GetEnvironmentVariableA("YKKZ000_LOG_LEVEL", value, sizeof(value)) > 0) {
    parsed = parseLogLevelName(value);
  }
  const bridge::LogLevel compile_min = bridge::kCompileMinLevel;
  const int clamped = bridge::ToInt(parsed < compile_min ? compile_min : parsed);
  return toLog4cxxLevel(clamped);
}

void setupLogging() {
  try {
    log4cxx::LoggerPtr logger = log4cxx::Logger::getLogger("ykkz000");
    // Do not inherit appenders from the root logger: this is the only writer in the process.
    logger->setAdditivity(false);
    log4cxx::LayoutPtr layout =
        std::make_shared<log4cxx::PatternLayout>(LOG4CXX_STR("[YKKZ000:%p] %m%n"));

    const std::wstring path = logFilePath();
    if (!path.empty()) {
      // FileAppender's (layout, filename, append) constructor opens the file immediately.
      auto fileAppender = std::make_shared<log4cxx::FileAppender>(layout, toLogString(path), true);
      fileAppender->setImmediateFlush(true);
      logger->addAppender(fileAppender);
    }

    auto debugAppender = std::make_shared<log4cxx::nt::OutputDebugStringAppender>();
    debugAppender->setLayout(layout);
    logger->addAppender(debugAppender);

    logger->setLevel(runtimeLevel());
    g_logger = logger;
    g_loggerReady = true;
  } catch (...) {
    // Logging must never prevent the Loader from starting: fall back to the debugger only.
    g_logger = nullptr;
    g_loggerReady = false;
  }
}

// Fallback used before setupLogging succeeds (or when it fails): debugger output only.
void fallbackLog(int level, const char* message) {
  if (message == nullptr) {
    return;
  }
  const auto named = (level >= 0 && level <= 5) ? static_cast<bridge::LogLevel>(level)
                                                : bridge::LogLevel::kTrace;
  char buffer[1024] = {};
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "[YKKZ000:%s] %s\n",
              bridge::LogLevelName(named), message);
  OutputDebugStringA(buffer);
}

} // namespace

std::wstring logFilePath() {
  const std::wstring dir = gameLogDirectory();
  if (!dir.empty()) {
    return dir + L"\\YKKZ000_loader.log";
  }
  const std::wstring fallback = moduleDirectory(); // fallback: the Loader directory
  return fallback.empty() ? std::wstring{} : fallback + L"\\YKKZ000_loader.log";
}

void initializeLogging() {
  std::call_once(g_loggingOnce, []() { setupLogging(); });
}

void logMessage(int level, const char* message) {
  if (message == nullptr) {
    return;
  }
  initializeLogging();
  if (!g_loggerReady || g_logger == nullptr) {
    fallbackLog(level, message);
    return;
  }
  g_logger->log(toLog4cxxLevel(level), toLogString(message), LOG4CXX_LOCATION);
}

void logMessage(int level, const std::wstring& message) {
  if (message.empty()) {
    return;
  }
  initializeLogging();
  if (!g_loggerReady || g_logger == nullptr) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, message.c_str(),
                                          static_cast<int>(message.size()), nullptr, 0, nullptr,
                                          nullptr);
    if (bytes <= 0) {
      return;
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
                        utf8.data(), bytes, nullptr, nullptr);
    fallbackLog(level, utf8.c_str());
    return;
  }
  g_logger->log(toLog4cxxLevel(level), toLogString(message), LOG4CXX_LOCATION);
}

void logMessageF(int level, const char* format, ...) {
  if (format == nullptr) {
    return;
  }
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
  va_end(args);
  logMessage(level, buffer);
}

} // namespace ykkz000::loader
