#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include <ykkz000/loader/internal.h>

#include "serialization_trace.h"

// -- Crash-context capture (VEH) --
// Requirements: no heap dependency, no debugger dependency; only append to a small file.
namespace ykkz000::loader {

// Whether this thread is currently executing an SEH-guarded engine call. The VEH uses this to
// silently pass through recoverable exceptions, handing them to that call's __except and avoiding
// writing probing access violations into YKKZ000_crash.log as fatal crashes.
// Currently unused (the suzerain count was changed to a pure in-memory walk); kept for reuse if
// untrusted engine calls are added later.
thread_local bool g_guardedCallActive = false;

namespace {

constexpr std::size_t kMaxPathChars = 512;
constexpr int kCrashStackQwords = 64;      // print the first N qwords starting at Rsp
constexpr int kCrashBackTraceFrames = 48;
constexpr long kMaxCrashRecords = 8;       // cap how many exceptions are recorded, to avoid spam

std::atomic<long> s_crashRecords{0};
PVOID g_vehHandle = nullptr;

// Determine whether an address "looks readable" (the lightest protection in a crash context)
bool crashReadable(const void* address, std::size_t size) {
  if (address == nullptr) {
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD protection = mbi.Protect & 0xFF;
  const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                        protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
  if (!readable) {
    return false;
  }
  const auto start = reinterpret_cast<std::uintptr_t>(address);
  const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return start + size <= regionEnd;
}

template <typename T>
T crashRead(const void* base, std::size_t offset, T fallback = T{}) {
  const auto* address = static_cast<const std::uint8_t*>(base) + offset;
  if (!crashReadable(address, sizeof(T))) {
    return fallback;
  }
  T value = {};
  std::memcpy(&value, address, sizeof(T));
  return value;
}

// Crash log file: <Loader directory>\YKKZ000_crash.log (kept separate from the regular log)
const wchar_t* crashLogPath() {
  static wchar_t path[kMaxPathChars] = {};
  static bool resolved = false;
  if (resolved) {
    return path;
  }
  resolved = true;
  wchar_t modulePath[kMaxPathChars] = {};
  const DWORD length = GetModuleFileNameW(g_selfModule, modulePath, kMaxPathChars);
  if (length == 0 || length >= kMaxPathChars) {
    return path;
  }
  std::size_t slash = 0;
  for (std::size_t i = 0; i < length; ++i) {
    if (modulePath[i] == L'\\' || modulePath[i] == L'/') {
      slash = i;
    }
  }
  const wchar_t* suffix = L"\\YKKZ000_crash.log";
  if (slash + 16 >= kMaxPathChars) {
    return path;
  }
  std::memcpy(path, modulePath, slash * sizeof(wchar_t));
  std::memcpy(path + slash, suffix, (std::wcslen(suffix) + 1) * sizeof(wchar_t));
  return path;
}

// Use only stack buffers + CreateFileW/WriteFile, to avoid touching the CRT heap during a crash
void crashWrite(const char* text, std::size_t length) {
  if (text == nullptr || length == 0) {
    return;
  }
  const wchar_t* path = crashLogPath();
  if (path[0] == L'\0') {
    return;
  }
  HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
  CloseHandle(file);
}

void crashWriteF(const char* format, ...) {
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
  va_end(args);
  crashWrite(buffer, std::strlen(buffer));
}

// Print every module's base/size (used to convert stack addresses into RVAs)
void crashLogModules() {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                    GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }
  MODULEENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Module32FirstW(snapshot, &entry)) {
    do {
      crashWriteF("!! module base=%p size=0x%zX name=%ls\n", entry.modBaseAddr,
                  static_cast<std::size_t>(entry.modBaseSize), entry.szModule);
      entry.dwSize = sizeof(entry);
    } while (Module32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
}

void crashLogStack(const CONTEXT* context) {
  // 1) Raw stack (each value is readability-checked first)
  const auto* sp = reinterpret_cast<const std::uint64_t*>(context->Rsp);
  for (int i = 0; i < kCrashStackQwords; ++i) {
    const auto* address = sp + i;
    if (!crashReadable(address, sizeof(std::uint64_t))) {
      crashWriteF("!! stack[%02d]=<unreadable>\n", i);
      continue;
    }
    crashWriteF("!! stack[%02d]=%p\n", i, reinterpret_cast<void*>(*address));
  }
  // 2) Return-address chain (the VEH runs on the faulting thread, so the fault context is just
  //    below this stack)
  void* frames[kCrashBackTraceFrames] = {};
  const USHORT captured = CaptureStackBackTrace(2, kCrashBackTraceFrames, frames, nullptr);
  for (USHORT i = 0; i < captured; ++i) {
    crashWriteF("!! backtrace[%02d]=%p\n", i, frames[i]);
  }
}

LONG CALLBACK CrashCapture_Handler(PEXCEPTION_POINTERS info) {
  static thread_local bool inHandler = false;
  if (inHandler || info == nullptr || info->ExceptionRecord == nullptr ||
      info->ContextRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // SEH-guarded probing call: do not record and do not intercept; pass it to that call's __except.
  if (g_guardedCallActive) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // Only care about "true crash" classes of exception, to avoid flooding the log with normal
  // SEH/breakpoints
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  const bool interesting =
      (code == 0xC0000005) ||      // ACCESS_VIOLATION
      (code == 0xC000001D) ||      // ILLEGAL_INSTRUCTION
      (code == 0xC0000094) ||      // INTEGER_DIVIDE_BY_ZERO
      (code == 0xC0000096) ||      // PRIVILEGED_INSTRUCTION
      (code == 0xC00000FD) ||      // STACK_OVERFLOW
      (code & 0x80000000u) != 0;   // other severe exceptions
  const bool nonContinuable =
      (info->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0;
  if (!interesting && !nonContinuable) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const long record = ++s_crashRecords;
  if (record > kMaxCrashRecords) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  inHandler = true;

  const EXCEPTION_RECORD* rec = info->ExceptionRecord;
  const CONTEXT* ctx = info->ContextRecord;

  crashWriteF("\n==== YKKZ000 crash capture #%ld ====\n", record);
  crashWriteF("!! code=0x%08X flags=0x%08X addr=%p\n", rec->ExceptionCode, rec->ExceptionFlags,
              rec->ExceptionAddress);
  if (rec->NumberParameters >= 2) {
    crashWriteF("!! %s address=%p\n", rec->ExceptionInformation[0] ? "writing" : "reading",
                reinterpret_cast<void*>(rec->ExceptionInformation[1]));
  }
  crashWriteF("!! rip=%p rsp=%p rbp=%p\n", reinterpret_cast<void*>(ctx->Rip),
              reinterpret_cast<void*>(ctx->Rsp), reinterpret_cast<void*>(ctx->Rbp));
  crashWriteF("!! rax=%p rcx=%p rdx=%p r8=%p r9=%p\n", reinterpret_cast<void*>(ctx->Rax),
              reinterpret_cast<void*>(ctx->Rcx), reinterpret_cast<void*>(ctx->Rdx),
              reinterpret_cast<void*>(ctx->R8), reinterpret_cast<void*>(ctx->R9));

  // If rcx is that "node/object", decode the hash and handler directly (the most critical info for
  // this crash)
  const void* node = reinterpret_cast<const void*>(ctx->Rcx);
  if (crashReadable(node, 0x20)) {
    crashWriteF("!! node=%p hash=0x%08X handler=%p\n", node,
                crashRead<std::uint32_t>(node, 0x10), crashRead<void*>(node, 0x18));
    const void* handler = crashRead<void*>(node, 0x18);
    if (crashReadable(handler, sizeof(void*))) {
      const void* table = crashRead<void*>(handler, 0);
      crashWriteF("!! handler=%p table=%p\n", handler, table);
      if (crashReadable(table, 0x38)) {
        crashWriteF("!! table[4]=%p table[6]=%p\n", crashRead<void*>(table, 0x20),
                    crashRead<void*>(table, 0x30));
      }
    }
  }
  // If rax is an "object/vtable", decode it too (the faulting instruction is jmp [rax+0x30])
  const void* rax = reinterpret_cast<const void*>(ctx->Rax);
  if (crashReadable(rax, 0x38)) {
    crashWriteF("!! rax=%p [rax]=%p [rax+0x30]=%p\n", rax, crashRead<void*>(rax, 0),
                crashRead<void*>(rax, 0x30));
  }

  crashLogStack(ctx);
  crashLogModules();

  // Dump the serialization read trace (the last values the AutoVariable helpers transferred), which
  // shows where the load stream derailed. No-op in release builds.
  dumpSerializationTrace(&crashWrite);

  // Optional: keep the process alive to ease "Task Manager -> Create dump file".
  //   Usage: set the environment variable YKKZ000_CRASH_HOLD_MS (milliseconds); for example,
  //   300000 holds for 5 minutes.
  wchar_t hold[32] = {};
  if (GetEnvironmentVariableW(L"YKKZ000_CRASH_HOLD_MS", hold, 32) > 0) {
    const DWORD milliseconds = static_cast<DWORD>(_wtoi(hold));
    if (milliseconds > 0) {
      crashWriteF("!! holding process for %lu ms (create a dump now)\n", milliseconds);
      Sleep(milliseconds);
    }
  }

  inHandler = false;
  return EXCEPTION_CONTINUE_SEARCH;  // observe only; do not change crash behavior
}

} // namespace

void installCrashCapture() {
  if (g_vehHandle != nullptr) {
    return; // idempotent
  }
  // Truncate the crash log once per process, before the handler can append: each run starts empty.
  // crashWrite keeps using FILE_APPEND_DATA so several crashes within one run still accumulate.
  const wchar_t* path = crashLogPath();
  if (path[0] != L'\0') {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
  }
  // 1 = called first (captures the exception before the engine's SEH)
  g_vehHandle = AddVectoredExceptionHandler(
      1, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(&CrashCapture_Handler));
  if (g_vehHandle != nullptr) {
    logInfo("Crash capture: VEH installed");
  } else {
    logError("Crash capture: AddVectoredExceptionHandler failed");
  }
}

void uninstallCrashCapture() {
  // Note: may be called under the loader lock in DllMain(DLL_PROCESS_DETACH); logging is
  // forbidden there.
  if (g_vehHandle != nullptr) {
    RemoveVectoredExceptionHandler(g_vehHandle);
    g_vehHandle = nullptr;
  }
}

} // namespace ykkz000::loader
