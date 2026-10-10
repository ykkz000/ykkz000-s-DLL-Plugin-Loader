#include <windows.h>

#include <cstdio>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {

HMODULE g_selfModule = nullptr;

} // namespace ykkz000::loader

namespace {

// DllMain holds the loader lock, so file I/O is forbidden; output to the debugger only.
void logDebugOnly(const char* text) {
  char buffer[160] = {};
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "[YKKZ000] %s\n", text);
  OutputDebugStringA(buffer);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    ykkz000::loader::g_selfModule = instance;
    // Pin this module: keep cloned vtables / handlers / VEH valid for the whole process lifetime,
    // so the engine shutdown phase (Gameplay DLL unload/reload) cannot crash by calling into
    // unloaded code.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&ykkz000::loader::g_selfModule), &pinned);
    logDebugOnly("BEGIN: dll process attach");
    logDebugOnly("END: dll process attach");
  } else if (reason == DLL_PROCESS_DETACH) {
    logDebugOnly("BEGIN: dll process detach");
    // Under the loader lock: only remove the VEH so it cannot point at unloaded code (no
    // logging/file I/O).
    ykkz000::loader::uninstallCrashCapture();
    logDebugOnly("END: dll process detach");
  }
  return TRUE;
}
