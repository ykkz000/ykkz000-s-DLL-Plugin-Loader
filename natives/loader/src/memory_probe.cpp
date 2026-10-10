#include <windows.h>

#include <cstddef>
#include <cstdint>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {

// Determine whether [address, address+bytes) falls within committed, readable memory.
// Every pointer coming from the engine must pass this before being dereferenced (a pseudo-pointer
// like 0x14000000BE may still be "4-byte readable", so this must also be combined with the
// alignment check below).
bool isReadableRegion(const void* address, std::size_t bytes) {
  if (address == nullptr || bytes == 0) {
    return false;
  }
  MEMORY_BASIC_INFORMATION info = {};
  if (VirtualQuery(address, &info, sizeof(info)) == 0) {
    return false;
  }
  if (info.State != MEM_COMMIT) {
    return false;
  }
  const DWORD protection = info.Protect & 0xFF;
  const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                        protection == PAGE_EXECUTE_READWRITE ||
                        protection == PAGE_EXECUTE_WRITECOPY;
  if (!readable) {
    return false;
  }
  const auto start = reinterpret_cast<std::uintptr_t>(address);
  const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
  return start + bytes <= regionEnd;
}

// Whether a pointer "looks like a dereferenceable object": not a low address, 8-byte aligned, and
// its first pointer readable. 8-byte alignment is enough to filter out pseudo-pointers like
// 0x14000000BE that are stitched together from two int32s.
bool isCandidateObject(const void* pointer) {
  const auto value = reinterpret_cast<std::uintptr_t>(pointer);
  return value > 0x10000 && (value & 0x7) == 0 && isReadableRegion(pointer, sizeof(void*));
}

} // namespace ykkz000::loader
