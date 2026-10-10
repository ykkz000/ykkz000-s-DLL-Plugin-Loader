#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {
namespace {

// Registration and lookup may come from different threads (plugin registration vs. engine
// DatabaseWriter reads), hence the lock on the name table. #unordered_map stores nodes, so element
// pointers remain valid after a rehash; rememberTypeName does not overwrite the value for an
// existing hash, keeping an already-returned c_str() pointer valid for the process lifetime (the
// engine may hold the GetTypeName return value for a long time).
std::mutex g_typeNamesMutex;
std::unordered_map<std::uint32_t, std::string> g_typeNames;

} // namespace

// Replace the factory vtable's GetTypeName slot: the signature is equivalent to
//   const char* GetTypeName() const  (this in RCX, return value in RAX)
extern "C" const char* ykkz000_GetTypeName(void* self) {
  if (self == nullptr) {
    return "";
  }
  const auto hash = TryReadOr(self, &civ6::ModifierEffectFactory::type_hash, std::uint32_t{0});
  static std::atomic<bool> s_logged{false};
  if (!s_logged.exchange(true)) {
    logDebugF("getname: first call self=%p hash=0x%08X", self, hash);
  }

  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  const auto it = g_typeNames.find(hash);
  return it != g_typeNames.end() ? it->second.c_str() : "";
}

void rememberTypeName(std::uint32_t typeHash, const char* typeName) {
  if (typeName == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  g_typeNames.emplace(typeHash, std::string(typeName));
}

void forgetTypeName(std::uint32_t typeHash) {
  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  g_typeNames.erase(typeHash);
}

void* cloneFactoryVTable(void* templateFactory) {
  if (templateFactory == nullptr) {
    return nullptr;
  }
  auto* source = *reinterpret_cast<void***>(templateFactory);
  if (source == nullptr) {
    return nullptr;
  }
  logDebugF("clone: template=%p source vtable=%p slots=%zu", templateFactory, source,
            civ6::kFactoryVTableSlots);
  const std::size_t slots = civ6::kFactoryVTableSlots;
  auto* block = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * (slots + civ6::kVTableRttiPrefixSlots)));
  if (block == nullptr) {
    return nullptr;
  }
  block[0] = source[-1]; // MSVC's RTTI/COL pointer, must be preserved as well
  std::memcpy(block + civ6::kVTableRttiPrefixSlots, source, sizeof(void*) * slots);

  void** vtable = block + civ6::kVTableRttiPrefixSlots; // the vptr handed back to the engine and callers
  vtable[civ6::kFactoryTypeNameSlot] = reinterpret_cast<void*>(&ykkz000_GetTypeName);
  logDebugF("clone: block=%p vtable=%p nameSlot=%zX rtti=%p", block, vtable,
            civ6::kFactoryTypeNameSlot, block[0]);
  return vtable;
}

} // namespace ykkz000::loader
