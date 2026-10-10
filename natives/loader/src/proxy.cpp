#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <ykkz000/loader/internal.h>

#include "serialization_trace.h"

namespace ykkz000::loader {
namespace {

using CreateGameContextFn = void* (*)();
using DestroyGameContextFn = void (*)(void*);
using TelemetryHashFn = std::uint64_t (*)();

std::once_flag g_initOnce;
bool g_initialized = false;
CreateGameContextFn g_realCreate = nullptr;
DestroyGameContextFn g_realDestroy = nullptr;
TelemetryHashFn g_realTelemetryHash = nullptr;
bridge::Host g_host = {};

// -- Validated reads/writes exposed to plugins (bridge::Host services) --
int hostReadField(const void* base, std::size_t offset, std::size_t bytes, void* out) {
  if (base == nullptr || out == nullptr || bytes == 0) {
    return 0;
  }
  const auto* address = static_cast<const std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, bytes)) {
    return 0;
  }
  std::memcpy(out, address, bytes);
  return 1;
}

int hostWriteField(void* base, std::size_t offset, std::size_t bytes, const void* in) {
  if (base == nullptr || in == nullptr || bytes == 0) {
    return 0;
  }
  auto* address = static_cast<std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, bytes)) {
    return 0;
  }
  std::memcpy(address, in, bytes);
  return 1;
}

int hostIsCandidateObject(const void* pointer) {
  return isCandidateObject(pointer) ? 1 : 0;
}

int hostIsReadableRegion(const void* address, std::size_t bytes) {
  return isReadableRegion(address, bytes) ? 1 : 0;
}

bool initialize() {
  installCrashCapture();  // install as early as possible so any later crash is recorded
  LogScope scope("initialize");
  {
    LogScope s("resolve real GameCore");
    if (!ensureGameCoreLoaded()) {
      logFatal("Initialization failed: real GameCore unavailable");
      return false;
    }
  }

  const GameCoreApi& api = gameCore();
  {
    LogScope s("resolve GameCore exports");
    g_realCreate = reinterpret_cast<CreateGameContextFn>(
        GetProcAddress(api.module, "DllCreateGameContext"));
    g_realDestroy = reinterpret_cast<DestroyGameContextFn>(
        GetProcAddress(api.module, "DllDestroyGameContext"));
    g_realTelemetryHash = reinterpret_cast<TelemetryHashFn>(
        GetProcAddress(api.module, "EXP_GetTelemetrySessionHash"));
  }
  if (g_realCreate == nullptr) {
    logFatal("Initialization failed: could not resolve real DllCreateGameContext");
    return false;
  }

  // Effect handler registration: the hooks must be installed before the real DllCreateGameContext,
  // so that the built-in handler tables constructed while the game context is created are captured
  // and custom effects are re-registered.
  const bool handlerHookReady = installEffectHandlerHook();
  logInfoF("effect handler hook ready=%d", handlerHookReady ? 1 : 0);

  // Serialization read trace (diagnostics, _DEBUG only): install after the GameCore entry points
  // are resolved. The hooks are Loader-owned and never collide with plugin hooks.
  installSerializationTrace();

  {
    LogScope s("build host interface");
    g_host.apiVersion = bridge::kHostApiVersion;
    g_host.makeHash = &makeHash;
    g_host.registerEffectType = &registerEffectType;
    g_host.log = &logMessage;
    g_host.gameCoreModule = api.module;
    g_host.getEffectRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
    g_host.engine = &engineApi();
    g_host.installHook = &serviceInstallHook;
    g_host.removeHook = &serviceRemoveHook;
    g_host.readField = &hostReadField;
    g_host.writeField = &hostWriteField;
    g_host.isCandidateObject = &hostIsCandidateObject;
    g_host.isReadableRegion = &hostIsReadableRegion;
    g_host.onGameContext = nullptr; // registered by each plugin in its own Host copy
    g_host.pluginHandle = nullptr;  // filled in by each plugin's Host copy
  }

  // Registration must complete before the real DllCreateGameContext: it subsequently triggers
  // ModifierLibrary::Initialize and DatabaseWriter::WriteEffectData.
  {
    LogScope s("load plugins");
    loadPlugins(&g_host);
  }
  return true;
}

} // namespace

void initializeLoaderOnce() {
  // Configure the log4cxx backend before any LogScope so the startup lines reach the log file.
  // logMessage() also initializes lazily, covering exported entry points whose LogScope runs
  // before this function (for example createGameContext).
  initializeLogging();
  LogScope scope("initialize loader");
  std::call_once(g_initOnce, []() { g_initialized = initialize(); });
}

bool loaderInitialized() {
  return g_initialized;
}

void* createGameContext() {
  LogScope scope("create game context");
  logDebugF("context: created t0");
  initializeLoaderOnce();
  // Notify plugins: a new context is being created. Plugins reinstall/re-enable their own hooks
  // here and clear caches that become invalid with the context; the Loader no longer recognizes any
  // concrete behavior, so there is no install*IfNeeded() anymore.
  notifyPluginsGameContext(bridge::GameContextEvent::kCreated, nullptr);
  (void)installEffectHandlerHook();
  if (g_realCreate == nullptr) {
    return nullptr;
  }
  LogScope call("call real DllCreateGameContext");
  return g_realCreate();
}

void destroyGameContext(void* context) {
  LogScope scope("destroy game context");
  // Must precede the real destroy: the handler root is still valid here, so our registered nodes
  // can be safely detached; otherwise the engine shutdown phase would call a handler that no longer
  // exists and crash (see the exit-crash analysis).
  removeCustomEffectHandlers();
  if (g_realDestroy != nullptr) {
    LogScope call("call real DllDestroyGameContext");
    g_realDestroy(context);
  }
  // The context has been destroyed: notify plugins to disable hooks and clear caches.
  //
  // Note: the plugin DLLs stay loaded and are not unloaded here. The game repeatedly
  // creates/destroys contexts between the main menu and a match, whereas effect registration
  // happens only once (registration must precede ModifierLibrary::Initialize), so a plugin could
  // not re-register in the next context after being unloaded. Plugins live for the whole process;
  // when a real unload is needed (process exit), unloadPlugins() handles it in the order
  // "notify first, then fallback restore, then FreeLibrary".
  notifyPluginsGameContext(bridge::GameContextEvent::kDestroyed, context);
  uninstallEffectHandlerHook();
}

std::uint64_t telemetrySessionHash() {
  LogScope scope("telemetry session hash");
  initializeLoaderOnce();
  if (g_realTelemetryHash == nullptr && ensureGameCoreLoaded()) {
    g_realTelemetryHash = reinterpret_cast<TelemetryHashFn>(
        GetProcAddress(gameCore().module, "EXP_GetTelemetrySessionHash"));
  }
  return g_realTelemetryHash != nullptr ? g_realTelemetryHash() : 0;
}

} // namespace ykkz000::loader

extern "C" void* DllCreateGameContext() {
  return ykkz000::loader::createGameContext();
}

extern "C" void DllDestroyGameContext(void* context) {
  ykkz000::loader::destroyGameContext(context);
}

extern "C" int InitLoader() {
  ykkz000::loader::initializeLoaderOnce();
  return ykkz000::loader::loaderInitialized() ? 1 : 0;
}

extern "C" std::uint64_t EXP_GetTelemetrySessionHash() {
  return ykkz000::loader::telemetrySessionHash();
}
