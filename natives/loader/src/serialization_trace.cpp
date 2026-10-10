#include "serialization_trace.h"

// Serialization read trace (diagnostics).
//
// Purpose: during a savegame load, record the values the engine's AutoVariable serialization
// helpers transfer into a fixed-size static ring buffer, so the crash VEH can dump the last values
// read before a crash and locate the stream-derailment point.
//
// Reverse-engineering record (GameCore_XP2_FinalRelease.dll, build 0x667C6F5B, base +0x180000000):
//   * 0x260A10 int-vector block serializer: transfers a count through stream vtable slot +0x28,
//     then (key, value) pairs, writing each value into the data buffer (arg 2). The key list (arg 3)
//     is the global yield-key list; its length bounds how many values the helper reads.
//   * 0x4C46E0 int descriptor value, stream explicit: transfers 4 bytes at variable+0x10.
//   * 0x35F3A0 int descriptor value, stream from variable+0x08: transfers 4 bytes at variable+0x10.
//   * 0x140930 int-array descriptor value, stream explicit: forwards to 0x260A10 with the array at
//     variable+0x10.
//   * 0x1931A0 int-array descriptor value, stream from variable+0x08: forwards to 0x260A10.
//   * 0x0296F0 City yield int-vector loader (load-only): reads the yield vector at arg 2 through the
//     stream (arg 1).
//
// Direction: the value helpers above are shared between save and load (the stream object decides
// whether each slot read or writes), so a phase flag is kept. The helpers that recover the stream
// implicitly (0x35F3A0 / 0x1931A0) and the load-only City yield loader (0x0296F0) raise a
// thread-local depth, marking the records they produce (and any shared block helper they call) as
// load-phase. Recording itself is not gated so the dump still shows the save-side baseline for
// comparison; each record carries its load-phase flag and its caller.
//
// Constraints: the ring buffer is a static array, written with a relaxed atomic index. The VEH dump
// does not allocate and takes no locks.
#if defined(_DEBUG)

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {
namespace {

constexpr std::size_t kTraceCapacity = 8192;
constexpr std::size_t kTraceSampleValues = 6;

enum TraceHelper : std::uint32_t {
  kHelperIntArrayBlock = 0,
  kHelperIntValueExplicit = 1,
  kHelperIntValueImplicit = 2,
  kHelperIntArrayValueExplicit = 3,
  kHelperIntArrayValueImplicit = 4,
  kHelperCityYieldLoad = 5,
};

const char* helperName(std::uint32_t helper) {
  switch (helper) {
    case kHelperIntArrayBlock:
      return "int-array-block";
    case kHelperIntValueExplicit:
      return "int-explicit";
    case kHelperIntValueImplicit:
      return "int-implicit";
    case kHelperIntArrayValueExplicit:
      return "array-explicit";
    case kHelperIntArrayValueImplicit:
      return "array-implicit";
    case kHelperCityYieldLoad:
      return "city-yield-load";
    default:
      return "unknown";
  }
}

// One trace record. Kept POD and small; the VEH reads it without locks.
struct TraceRecord {
  std::uint64_t seq;
  std::uint64_t tickMs;
  std::uint32_t thread;
  std::uint32_t helper;
  std::uint32_t loadPhase;
  std::uint32_t reserved;
  void* stream;
  void* variable;
  std::uint64_t size;
  std::uint64_t values[kTraceSampleValues];
  void* caller;
};

TraceRecord g_records[kTraceCapacity] = {};
std::atomic<std::uint64_t> g_written{0};
std::atomic<bool> g_active{false};
// Non-zero while a load-phase-only helper (or a shared helper it calls) is running.
thread_local int g_loadDepth = 0;

// Hook targets and trampolines.
void* g_blockTarget = nullptr;
void* g_intExplicitTarget = nullptr;
void* g_intImplicitTarget = nullptr;
void* g_arrayExplicitTarget = nullptr;
void* g_arrayImplicitTarget = nullptr;
void* g_cityYieldTarget = nullptr;

using BlockFn = std::uint64_t (*)(void*, void*, void*);
using IntExplicitFn = void (*)(void*, void*);
using IntImplicitFn = std::uint64_t (*)(void*);
using ArrayExplicitFn = void (*)(void*, void*);
using ArrayImplicitFn = std::uint64_t (*)(void*);
using CityYieldFn = void (*)(void*, void*, char);

BlockFn g_blockOriginal = nullptr;
IntExplicitFn g_intExplicitOriginal = nullptr;
IntImplicitFn g_intImplicitOriginal = nullptr;
ArrayExplicitFn g_arrayExplicitOriginal = nullptr;
ArrayImplicitFn g_arrayImplicitOriginal = nullptr;
CityYieldFn g_cityYieldOriginal = nullptr;

template <typename T>
bool traceRead(const void* base, std::size_t offset, T& out) {
  return tryReadField(base, offset, out);
}

void recordTrace(std::uint32_t helper, void* stream, void* variable, std::uint64_t size,
                 const std::uint64_t* values, void* caller) {
  if (!g_active.load(std::memory_order_relaxed)) {
    return;
  }
  const std::uint64_t seq = g_written.fetch_add(1, std::memory_order_relaxed);
  TraceRecord& record = g_records[static_cast<std::size_t>(seq % kTraceCapacity)];
  record.seq = seq;
  record.tickMs = GetTickCount64();
  record.thread = GetCurrentThreadId();
  record.helper = helper;
  record.loadPhase = g_loadDepth > 0 ? 1u : 0u;
  record.reserved = 0;
  record.stream = stream;
  record.variable = variable;
  record.size = size;
  for (std::size_t i = 0; i < kTraceSampleValues; ++i) {
    record.values[i] = values != nullptr ? values[i] : 0;
  }
  record.caller = caller;
}

// Samples up to kTraceSampleValues int entries from a variable's data buffer (data pointer at
// variable+0x10, count field at variable+0x20). Returns the count field as read.
std::uint64_t sampleVariableArray(void* variable, std::uint64_t* values) {
  if (variable == nullptr) {
    return 0;
  }
  void* data = nullptr;
  std::uint64_t count = 0;
  traceRead(variable, 0x10, data);
  traceRead(variable, 0x20, count);
  if (data != nullptr) {
    const std::size_t sample =
        count < kTraceSampleValues ? static_cast<std::size_t>(count) : kTraceSampleValues;
    for (std::size_t i = 0; i < sample; ++i) {
      std::uint32_t value = 0;
      if (traceRead(data, i * sizeof(std::uint32_t), value)) {
        values[i] = value;
      }
    }
  }
  return count;
}

// 0x260A10: called by both the save and load array helpers. The list (arg 3) is a
// {begin, end} vector of 8-byte entries; its length bounds the values written into arg 2.
std::uint64_t BlockDetour(void* stream, void* data, void* list) {
  void* caller = _ReturnAddress();
  const std::uint64_t result =
      g_blockOriginal != nullptr ? g_blockOriginal(stream, data, list) : 0;

  std::uint64_t values[kTraceSampleValues] = {};
  std::uint64_t count = 0;
  void* begin = nullptr;
  void* end = nullptr;
  if (list != nullptr && traceRead(list, 0, begin) &&
      traceRead(list, sizeof(void*), end) && begin != nullptr && end != nullptr) {
    const auto start = reinterpret_cast<std::uintptr_t>(begin);
    const auto stop = reinterpret_cast<std::uintptr_t>(end);
    if (stop >= start) {
      count = (stop - start) >> 3;
    }
  }
  if (data != nullptr) {
    const std::size_t sample =
        count < kTraceSampleValues ? static_cast<std::size_t>(count) : kTraceSampleValues;
    for (std::size_t i = 0; i < sample; ++i) {
      std::uint32_t value = 0;
      if (traceRead(data, i * sizeof(std::uint32_t), value)) {
        values[i] = value;
      }
    }
  }
  recordTrace(kHelperIntArrayBlock, stream, data, count, values, caller);
  return result;
}

void IntExplicitDetour(void* variable, void* stream) {
  void* caller = _ReturnAddress();
  if (g_intExplicitOriginal != nullptr) {
    g_intExplicitOriginal(variable, stream);
  }
  std::uint64_t values[kTraceSampleValues] = {};
  std::uint32_t value = 0;
  if (variable != nullptr && traceRead(variable, 0x10, value)) {
    values[0] = value;
  }
  recordTrace(kHelperIntValueExplicit, stream, variable, 1, values, caller);
}

std::uint64_t IntImplicitDetour(void* variable) {
  void* caller = _ReturnAddress();
  ++g_loadDepth;
  const std::uint64_t result =
      g_intImplicitOriginal != nullptr ? g_intImplicitOriginal(variable) : 0;
  std::uint64_t values[kTraceSampleValues] = {};
  std::uint32_t value = 0;
  void* archive = nullptr;
  if (variable != nullptr) {
    traceRead(variable, 0x08, archive);
    if (traceRead(variable, 0x10, value)) {
      values[0] = value;
    }
  }
  recordTrace(kHelperIntValueImplicit, archive, variable, 1, values, caller);
  --g_loadDepth;
  return result;
}

void ArrayExplicitDetour(void* variable, void* stream) {
  void* caller = _ReturnAddress();
  if (g_arrayExplicitOriginal != nullptr) {
    g_arrayExplicitOriginal(variable, stream);
  }
  std::uint64_t values[kTraceSampleValues] = {};
  const std::uint64_t count = sampleVariableArray(variable, values);
  recordTrace(kHelperIntArrayValueExplicit, stream, variable, count, values, caller);
}

std::uint64_t ArrayImplicitDetour(void* variable) {
  void* caller = _ReturnAddress();
  ++g_loadDepth;
  const std::uint64_t result =
      g_arrayImplicitOriginal != nullptr ? g_arrayImplicitOriginal(variable) : 0;
  std::uint64_t values[kTraceSampleValues] = {};
  const std::uint64_t count = sampleVariableArray(variable, values);
  recordTrace(kHelperIntArrayValueImplicit, nullptr, variable, count, values, caller);
  --g_loadDepth;
  return result;
}

// 0x0296F0: load-only City yield vector. The vector (arg 2) is {data, begin, size}; the loaded
// values are sampled from its data buffer.
void CityYieldDetour(void* stream, void* vector, char flag) {
  void* caller = _ReturnAddress();
  ++g_loadDepth;
  if (g_cityYieldOriginal != nullptr) {
    g_cityYieldOriginal(stream, vector, flag);
  }
  std::uint64_t values[kTraceSampleValues] = {};
  void* data = nullptr;
  std::uint32_t count = 0;
  if (vector != nullptr) {
    traceRead(vector, 0x00, data);
    traceRead(vector, 0x10, count);
    if (data != nullptr) {
      const std::size_t sample =
          count < kTraceSampleValues ? static_cast<std::size_t>(count) : kTraceSampleValues;
      for (std::size_t i = 0; i < sample; ++i) {
        std::uint32_t value = 0;
        if (traceRead(data, i * sizeof(std::uint32_t), value)) {
          values[i] = value;
        }
      }
    }
  }
  recordTrace(kHelperCityYieldLoad, stream, vector, count, values, caller);
  --g_loadDepth;
}

void installOne(const char* name, void* target, void* detour, void** originalOut) {
  if (target == nullptr) {
    logWarnF("trace: %s target unavailable; hook skipped", name);
    return;
  }
  void* original = nullptr;
  const int status = installHookRaw(target, detour, &original);
  if (status == 0 && original != nullptr) {
    *originalOut = original;
    logDebugF("trace: %s hook active target=%p original=%p", name, target, original);
  } else {
    logWarnF("trace: %s hook install failed target=%p status=%d", name, target, status);
  }
}

void removeOne(const char* name, void* target) {
  if (target == nullptr) {
    return;
  }
  const int status = removeHookRaw(target);
  logDebugF("trace: %s hook removed target=%p status=%d", name, target, status);
}

// -- Crash dump --
// Writes the ring buffer (oldest first) through a chunked buffer, so the crash log sees the last
// values read before the fault. No allocation and no locks.
void dumpImpl(SerializationTraceWriter write) {
  if (write == nullptr) {
    return;
  }
  const std::uint64_t total = g_written.load(std::memory_order_acquire);
  const HMODULE gameCoreModule = gameCore().module;
  char buffer[8192];
  std::size_t used = 0;
  auto flush = [&]() {
    if (used > 0) {
      write(buffer, used);
      used = 0;
    }
  };
  auto append = [&](const char* text, int length) {
    if (length <= 0) {
      return;
    }
    if (static_cast<std::size_t>(length) >= sizeof(buffer)) {
      flush();
      write(text, static_cast<std::size_t>(length));
      return;
    }
    if (used + static_cast<std::size_t>(length) > sizeof(buffer)) {
      flush();
    }
    std::memcpy(buffer + used, text, static_cast<std::size_t>(length));
    used += static_cast<std::size_t>(length);
  };

  {
    char line[160];
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                              "!! serialization trace: records=%llu capacity=%zu "
                              "gamecore=%p active=%d\n",
                              static_cast<unsigned long long>(total), kTraceCapacity,
                              gameCoreModule, g_active.load(std::memory_order_relaxed) ? 1 : 0);
    append(line, n);
  }
  if (total == 0) {
    const char* empty = "!! serialization trace: no records captured\n";
    append(empty, static_cast<int>(std::strlen(empty)));
    flush();
    return;
  }

  const std::uint64_t start = total > kTraceCapacity ? total - kTraceCapacity : 0;
  for (std::uint64_t i = start; i < total; ++i) {
    const TraceRecord& record = g_records[static_cast<std::size_t>(i % kTraceCapacity)];
    char line[320];
    const int n = _snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "!! trace[%llu] t=%llu tid=%u helper=%s load=%u stream=%p variable=%p size=%llu "
        "v=%llu,%llu,%llu,%llu,%llu,%llu caller=%p\n",
        static_cast<unsigned long long>(record.seq),
        static_cast<unsigned long long>(record.tickMs), record.thread, helperName(record.helper),
        record.loadPhase, record.stream, record.variable,
        static_cast<unsigned long long>(record.size),
        static_cast<unsigned long long>(record.values[0]),
        static_cast<unsigned long long>(record.values[1]),
        static_cast<unsigned long long>(record.values[2]),
        static_cast<unsigned long long>(record.values[3]),
        static_cast<unsigned long long>(record.values[4]),
        static_cast<unsigned long long>(record.values[5]), record.caller);
    append(line, n);
  }
  const char* tail = "!! serialization trace end (newest last)\n";
  append(tail, static_cast<int>(std::strlen(tail)));
  flush();
}

} // namespace

void installSerializationTrace() {
  const GameCoreApi& api = gameCore();
  if (api.module == nullptr) {
    logWarn("trace: GameCore not resolved; serialization trace not installed");
    return;
  }
  g_active.store(false, std::memory_order_relaxed);
  installOne("int-array-block", api.autoVarIntArrayBlock,
             reinterpret_cast<void*>(&BlockDetour), reinterpret_cast<void**>(&g_blockOriginal));
  installOne("int-explicit", api.autoVarIntValueExplicit,
             reinterpret_cast<void*>(&IntExplicitDetour),
             reinterpret_cast<void**>(&g_intExplicitOriginal));
  installOne("int-implicit", api.autoVarIntValueImplicit,
             reinterpret_cast<void*>(&IntImplicitDetour),
             reinterpret_cast<void**>(&g_intImplicitOriginal));
  installOne("array-explicit", api.autoVarIntArrayValueExplicit,
             reinterpret_cast<void*>(&ArrayExplicitDetour),
             reinterpret_cast<void**>(&g_arrayExplicitOriginal));
  installOne("array-implicit", api.autoVarIntArrayValueImplicit,
             reinterpret_cast<void*>(&ArrayImplicitDetour),
             reinterpret_cast<void**>(&g_arrayImplicitOriginal));
  installOne("city-yield-load", api.cityYieldIntVectorLoad,
             reinterpret_cast<void*>(&CityYieldDetour),
             reinterpret_cast<void**>(&g_cityYieldOriginal));

  g_blockTarget = api.autoVarIntArrayBlock;
  g_intExplicitTarget = api.autoVarIntValueExplicit;
  g_intImplicitTarget = api.autoVarIntValueImplicit;
  g_arrayExplicitTarget = api.autoVarIntArrayValueExplicit;
  g_arrayImplicitTarget = api.autoVarIntArrayValueImplicit;
  g_cityYieldTarget = api.cityYieldIntVectorLoad;

  g_written.store(0, std::memory_order_relaxed);
  g_active.store(true, std::memory_order_relaxed);
  logInfoF("trace: serialization read trace active (capacity=%zu)", kTraceCapacity);
}

void uninstallSerializationTrace() {
  g_active.store(false, std::memory_order_relaxed);
  removeOne("int-array-block", g_blockTarget);
  removeOne("int-explicit", g_intExplicitTarget);
  removeOne("int-implicit", g_intImplicitTarget);
  removeOne("array-explicit", g_arrayExplicitTarget);
  removeOne("array-implicit", g_arrayImplicitTarget);
  removeOne("city-yield-load", g_cityYieldTarget);
  g_blockTarget = nullptr;
  g_intExplicitTarget = nullptr;
  g_intImplicitTarget = nullptr;
  g_arrayExplicitTarget = nullptr;
  g_arrayImplicitTarget = nullptr;
  g_cityYieldTarget = nullptr;
  g_blockOriginal = nullptr;
  g_intExplicitOriginal = nullptr;
  g_intImplicitOriginal = nullptr;
  g_arrayExplicitOriginal = nullptr;
  g_arrayImplicitOriginal = nullptr;
  g_cityYieldOriginal = nullptr;
}

void dumpSerializationTrace(SerializationTraceWriter write) {
  dumpImpl(write);
}

} // namespace ykkz000::loader

#else // !defined(_DEBUG)

namespace ykkz000::loader {

void installSerializationTrace() {}
void uninstallSerializationTrace() {}
void dumpSerializationTrace(SerializationTraceWriter) {}

} // namespace ykkz000::loader

#endif // defined(_DEBUG)
