#include "enabled_mods_build.h"

// Enabled-mod construction/read (reusable capability).
//
// Builds an ENABLED_MODS section from the caller's entries and inserts it into the engine's live
// configuration manager, and reads the published ENABLED_MODS collection back through the engine's
// own accessor. The construction mirrors the engine's own object graph:
//
//   * Data_TypedVariant layout (0x18 bytes, confirmed against FUN_1809a1320 / FUN_180164870): int16
//     tag at +0, payload at +8, size at +0x10, flags at +0x14. The reader only inspects tag and
//     payload, but the flags gate ownership on destruction, so the engine's own values are
//     reproduced:
//       string   (tag 5) flags 0x20000000 (bit 29: opaque/shared view; the destructor does not free)
//       tag 0xA  map     flags 0x05000000
//       tag 0xB  vector-map flags 0x11000000
//     These match the flag constants FUN_1809a1320 writes when it builds the same variants.
//   * The section is a tag 0xB vector-map (0x48-byte object) whose element vector lives at +0x28
//     (begin) / +0x30 (end) with 0x18-byte elements; each element is a tag 0xA map referencing a
//     property bag (0x58-byte object) that carries MOD_ID and the title key as tag-5 strings.
//   * The objects themselves are allocated with the engine's temp allocator, and inserted with the
//     engine's own move-insert primitives, so the inserted graph is indistinguishable from the
//     engine's own population.
//
// All engine calls are SEH-guarded with no C++ unwinding, so a faulting call degrades to "failed"
// instead of taking down the process (the crash VEH passes through while g_guardedCallActive is
// set).
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {
namespace {

// Engine std::vector header for the {id, title} enabled-mod entries.
struct EngineEnabledModsVector {
  void* begin;
  void* end;
  void* cap;
};

constexpr std::size_t kEntryStride = 0x10;
constexpr std::size_t kWalkSanityCap = 1u << 20;

// SEH-guarded engine calls with no C++ unwinding, so a faulting call degrades to "unavailable"
// instead of taking down the process (the crash VEH passes through while g_guardedCallActive is
// set).
bool callGetEnabledMods(void* function, void* container, void* out) {
  using GetEnabledModsFn = void* (*)(void*, void*);
  g_guardedCallActive = true;
  __try {
    reinterpret_cast<GetEnabledModsFn>(function)(container, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

bool callVectorDeallocate(void* function, void* vector, void* data, std::size_t count) {
  using DeallocateFn = void (*)(void*, void*, std::size_t);
  g_guardedCallActive = true;
  __try {
    reinterpret_cast<DeallocateFn>(function)(vector, data, count);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

// Copy the {id, title} entries of an accessor-returned vector into owned strings. A malformed
// header or unreadable entry stops the walk; entries without an id are skipped, because the engine
// filters them out as well. Read-only; the vector stays caller-owned.
std::size_t appendEnabledMods(const EngineEnabledModsVector& vector,
                              std::vector<EnabledModEntry>& out) {
  if (vector.begin == nullptr || vector.end == nullptr || vector.end < vector.begin) {
    return 0;
  }
  const auto start = reinterpret_cast<std::uintptr_t>(vector.begin);
  const auto stop = reinterpret_cast<std::uintptr_t>(vector.end);
  const std::size_t bytes = stop - start;
  if (bytes % kEntryStride != 0 || bytes > kWalkSanityCap) {
    return 0;
  }
  const std::size_t count = bytes / kEntryStride;
  std::size_t appended = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto entry = start + i * kEntryStride;
    if (!isReadableRegion(reinterpret_cast<const void*>(entry), kEntryStride)) {
      break;
    }
    const char* id = *reinterpret_cast<const char* const*>(entry);
    if (id == nullptr || !isReadableRegion(id, 1) || id[0] == '\0') {
      continue;
    }
    const char* title = *reinterpret_cast<const char* const*>(entry + sizeof(void*));
    EnabledModEntry copy;
    copy.id = id;
    if (title != nullptr && isReadableRegion(title, 1)) {
      copy.title = title;
    }
    out.push_back(std::move(copy));
    ++appended;
  }
  return appended;
}

// Release a vector allocated by the accessor, using the engine's own deallocator with the capacity
// count (not the size).
void releaseEnabledModsVector(EngineEnabledModsVector& vector) {
  const GameCoreApi& api = gameCore();
  if (vector.begin == nullptr || api.vectorDeallocate == nullptr || vector.cap < vector.begin) {
    return;
  }
  const std::size_t capacity =
      (static_cast<const char*>(vector.cap) - static_cast<const char*>(vector.begin)) /
      kEntryStride;
  if (capacity > kWalkSanityCap / kEntryStride) {
    return;
  }
  (void)callVectorDeallocate(api.vectorDeallocate, &vector, vector.begin, capacity);
}

// Data_TypedVariant tags and flags (see the file header).
constexpr std::int16_t kStringVariantTag = 5;
constexpr std::int16_t kMapVariantTag = 0xA;
constexpr std::int16_t kVectorMapVariantTag = 0xB;
constexpr std::uint32_t kStringVariantFlags = 0x20000000;
constexpr std::uint32_t kMapVariantFlags = 0x05000000;
constexpr std::uint32_t kVectorMapVariantFlags = 0x11000000;

// Object sizes the engine's own constructors expect (FUN_1809a1320 allocates exactly these).
constexpr std::size_t kMapObjectSize = 0x58;
constexpr std::size_t kVectorMapObjectSize = 0x48;

// The vector-map's embedded element vector: begin at +0x28, end at +0x30.
constexpr std::size_t kListBeginOffset = 0x28;
constexpr std::size_t kListEndOffset = 0x30;

// The 0x18-byte DataVariant as seen on the stack; only the fields the reader uses plus the flags are
// meaningful. Passed by pointer to the engine move/copy primitives, which read the whole 0x18 bytes.
struct DataVariant {
  std::int16_t tag;        // +0x00
  std::int16_t reserved0;  // +0x02
  std::uint32_t reserved1; // +0x04
  void* payload;           // +0x08
  std::uint32_t size;      // +0x10
  std::uint32_t flags;     // +0x14
};
static_assert(sizeof(DataVariant) == 0x18, "DataVariant must be 0x18 bytes");
static_assert(offsetof(DataVariant, payload) == 0x08, "payload offset");
static_assert(offsetof(DataVariant, flags) == 0x14, "flags offset");

using EngineAllocFn = void* (*)(std::size_t, const char*, int, int, int);
using MapCtorFn = void* (*)(void*);
using MapInsertFn = void* (*)(void*, std::uint32_t, void*);
using MapFindFn = void* (*)(void*, std::uint32_t);
using VectorMapInsertFn = void (*)(void*, void*, void*);

// Process-lifetime storage for the section's id/title strings. The strings are tag-5 shared views
// (flags 0x20000000) that the engine never frees, so the buffers must outlive every context; they
// are intentionally kept for the process lifetime. Interning makes repeated builds of the same
// entries leak nothing extra. The allocation is non-throwing so it can never unwind through an
// engine frame.
constexpr std::size_t kInternCap = 1024;
std::mutex g_internMutex;
const char* g_interned[kInternCap] = {};
std::size_t g_internedCount = 0;

const char* internString(const char* text) {
  if (text == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> guard(g_internMutex);
  for (std::size_t i = 0; i < g_internedCount; ++i) {
    if (std::strcmp(g_interned[i], text) == 0) {
      return g_interned[i];
    }
  }
  const std::size_t length = std::strlen(text) + 1;
  char* copy = new (std::nothrow) char[length];
  if (copy == nullptr) {
    return nullptr;
  }
  std::memcpy(copy, text, length);
  if (g_internedCount < kInternCap) {
    g_interned[g_internedCount++] = copy; // process-lifetime view; never freed by design
  }
  return copy;
}

// SEH-guarded engine write/alloc calls with no C++ unwinding (mirrors the helpers above).
bool callEngineAlloc(void* function, std::size_t size, void*& out) {
  g_guardedCallActive = true;
  __try {
    out = reinterpret_cast<EngineAllocFn>(function)(size, nullptr, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

bool callMapCtor(void* function, void* object, void*& out) {
  g_guardedCallActive = true;
  __try {
    out = reinterpret_cast<MapCtorFn>(function)(object);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

bool callMapInsert(void* function, void* map, std::uint32_t key, void* value, void*& out) {
  g_guardedCallActive = true;
  __try {
    out = reinterpret_cast<MapInsertFn>(function)(map, key, value);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

bool callMapFind(void* function, void* map, std::uint32_t key, void*& out) {
  g_guardedCallActive = true;
  __try {
    out = reinterpret_cast<MapFindFn>(function)(map, key);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

bool callVectorMapInsert(void* function, void* vector, void* position, void* value) {
  g_guardedCallActive = true;
  __try {
    reinterpret_cast<VectorMapInsertFn>(function)(vector, position, value);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_guardedCallActive = false;
    return false;
  }
  g_guardedCallActive = false;
  return true;
}

// Insert one tag-5 string variant under `key` into the property bag `bag`.
bool insertStringField(const GameCoreApi& api, void* bag, std::uint32_t key,
                       const char* text) {
  DataVariant value{};
  value.tag = kStringVariantTag;
  value.payload = const_cast<char*>(text);
  value.size = static_cast<std::uint32_t>(std::strlen(text));
  value.flags = kStringVariantFlags;
  void* inserted = nullptr;
  return callMapInsert(api.variantMapInsert, bag, key, &value, inserted) && inserted != nullptr;
}

} // namespace

bool buildAndInsertEnabledMods(void* manager, const EnabledMod* mods, std::size_t count) {
  const GameCoreApi& api = gameCore();
  if (manager == nullptr || !isCandidateObject(manager) || (count != 0 && mods == nullptr) ||
      api.variantMapCtor == nullptr || api.vectorMapCtor == nullptr ||
      api.variantMapInsert == nullptr || api.variantMapFind == nullptr ||
      api.vectorMapInsert == nullptr || api.mallocTemp == nullptr ||
      api.enabledModsHash == nullptr || api.modIdHash == nullptr ||
      api.modTitleKeyHash == nullptr) {
    return false;
  }

  std::uint32_t enabledHash = 0;
  std::uint32_t modIdKey = 0;
  std::uint32_t titleKey = 0;
  if (!tryReadField(api.enabledModsHash, 0, enabledHash) ||
      !tryReadField(api.modIdHash, 0, modIdKey) ||
      !tryReadField(api.modTitleKeyHash, 0, titleKey)) {
    return false;
  }

  void* existing = nullptr;
  if (callMapFind(api.variantMapFind, manager, enabledHash, existing) && existing != nullptr) {
    return false; // never overwrite an existing section (the engine's own population)
  }

  // 1) vector-map (tag 0xB payload).
  void* vmStorage = nullptr;
  if (!callEngineAlloc(api.mallocTemp, kVectorMapObjectSize, vmStorage) || vmStorage == nullptr) {
    return false;
  }
  void* vm = nullptr;
  if (!callMapCtor(api.vectorMapCtor, vmStorage, vm) || vm == nullptr) {
    return false;
  }

  std::size_t appended = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char* id = internString(mods[i].id);
    if (id == nullptr || id[0] == '\0') {
      continue; // an entry without an id cannot be identified by the engine's readers
    }
    const char* title = internString(mods[i].title);

    // 2) property bag (tag 0xA payload) carrying MOD_ID and the title key.
    void* bagStorage = nullptr;
    if (!callEngineAlloc(api.mallocTemp, kMapObjectSize, bagStorage) || bagStorage == nullptr) {
      return false;
    }
    void* bag = nullptr;
    if (!callMapCtor(api.variantMapCtor, bagStorage, bag) || bag == nullptr) {
      return false;
    }
    if (!insertStringField(api, bag, modIdKey, id)) {
      return false;
    }
    if (title != nullptr && !insertStringField(api, bag, titleKey, title)) {
      return false;
    }

    // 3) append a tag 0xA element referencing the bag into the vector-map's element vector. The
    //    element vector object begins at vm+0x28; insertion at the current end appends.
    void* vectorEnd = nullptr;
    if (!tryReadField(vm, kListEndOffset, vectorEnd)) {
      return false;
    }
    void* elementVector = reinterpret_cast<char*>(vm) + kListBeginOffset;
    DataVariant element{};
    element.tag = kMapVariantTag;
    element.payload = bag;
    element.flags = kMapVariantFlags;
    if (!callVectorMapInsert(api.vectorMapInsert, elementVector, vectorEnd, &element)) {
      return false;
    }
    ++appended;
  }

  // An empty section would pre-empt the engine's own population with no usable data, so report
  // failure instead of inserting one.
  if (appended == 0) {
    return false;
  }

  // 4) insert the tag 0xB section into the manager under the ENABLED_MODS key.
  DataVariant section{};
  section.tag = kVectorMapVariantTag;
  section.payload = vm;
  section.flags = kVectorMapVariantFlags;
  void* sectionResult = nullptr;
  if (!callMapInsert(api.variantMapInsert, manager, enabledHash, &section, sectionResult) ||
      sectionResult == nullptr) {
    return false;
  }
  return true;
}

bool enumerateEnabledMods(std::vector<EnabledModEntry>& out) {
  const GameCoreApi& api = gameCore();
  if (api.enabledModsContainer == nullptr || api.getEnabledMods == nullptr) {
    return false;
  }
  void* container = nullptr;
  if (!tryReadField(api.enabledModsContainer, 0, container) || container == nullptr ||
      !isCandidateObject(container)) {
    return false; // the engine has not published the modding settings yet
  }
  EngineEnabledModsVector vector{};
  if (!callGetEnabledMods(api.getEnabledMods, container, &vector)) {
    return false;
  }
  std::vector<EnabledModEntry> entries;
  (void)appendEnabledMods(vector, entries);
  releaseEnabledModsVector(vector);
  out = std::move(entries);
  return true;
}

} // namespace ykkz000::loader
