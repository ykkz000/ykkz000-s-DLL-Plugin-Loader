#include "mods_sqlite.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ykkz000/loader/internal.h>

// Read-only Mods.sqlite access. The snapshot parser walks the file's SQLite b-trees directly (no
// external dependency) and reassembles payloads across overflow pages; the system's winsqlite3.dll
// serves the same queries when the database is in WAL mode or the snapshot parse fails. Everything
// here is best-effort: any failure leaves the Loader's behavior unchanged and cross-mod plugin
// discovery then stays limited to the Loader's own directory. Nothing is written and no engine
// state is touched.
namespace ykkz000::loader {
namespace {

using Bytes = std::vector<std::uint8_t>;

// -- SQLite file format constants (see the public SQLite file-format description) --
constexpr char kMagic[16] = "SQLite format 3";
constexpr std::size_t kFileHeaderSize = 100;
constexpr std::size_t kMinPageSize = 512;
constexpr std::size_t kMaxPageSize = 65536;
constexpr std::uint32_t kTextEncodingUtf8 = 1;
constexpr std::uint32_t kTextEncodingUtf16Le = 2;
constexpr std::uint32_t kTextEncodingUtf16Be = 3;
constexpr std::uint8_t kPageTypeInteriorTable = 0x05;
constexpr std::uint8_t kPageTypeLeafTable = 0x0D;
// Bounds for a single row payload. The target tables hold tiny rows; sqlite_master carries the
// CREATE TABLE statements, which stay far below this even after localization-heavy migrations.
constexpr std::uint64_t kMaxPayloadBytes = 1 << 20;
// The target tables have at most five columns; the cap only bounds storage for malformed headers.
constexpr std::size_t kMaxColumns = 32;
// The database is about 4.6 MB in a real profile; refuse to buffer more than this.
constexpr std::uint64_t kMaxFileBytes = 64 << 20;

// -- Byte-level helpers ------------------------------------------------------

std::uint16_t readBe16(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint32_t>(data[0]) << 8) | data[1]);
}

std::uint32_t readBe32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

// SQLite variable-length integer: big-endian, seven bits per byte, up to nine bytes (the ninth
// contributes all eight bits).
bool readVarint(const std::uint8_t*& cursor, const std::uint8_t* end, std::uint64_t& value) {
  std::uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    if (cursor >= end) {
      return false;
    }
    const std::uint8_t byte = *cursor++;
    result = (result << 7) | (byte & 0x7F);
    if ((byte & 0x80) == 0) {
      value = result;
      return true;
    }
  }
  if (cursor >= end) {
    return false;
  }
  value = (result << 8) | *cursor++;
  return true;
}

// Big-endian signed integer of 1..8 bytes (the record format's integer serial types).
std::int64_t decodeSigned(const std::uint8_t* data, std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i) {
    value = (value << 8) | data[i];
  }
  if (size != 0 && size < 8 && (data[0] & 0x80) != 0) {
    value |= ~((static_cast<std::uint64_t>(1) << (size * 8)) - 1);
  }
  return static_cast<std::int64_t>(value);
}

std::string toLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return text;
}

std::string wideToUtf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

// Convert a text column to UTF-8 according to the database text encoding.
bool textToUtf8(const std::uint8_t* data, std::size_t size, std::uint32_t encoding,
                std::string& out) {
  if (encoding == kTextEncodingUtf8) {
    out.assign(reinterpret_cast<const char*>(data), size);
    return true;
  }
  if (encoding != kTextEncodingUtf16Le && encoding != kTextEncodingUtf16Be) {
    return false;
  }
  std::wstring wide;
  wide.reserve(size / 2);
  for (std::size_t i = 0; i + 1 < size; i += 2) {
    const std::uint16_t unit = encoding == kTextEncodingUtf16Le
                                   ? static_cast<std::uint16_t>(data[i] | (data[i + 1] << 8))
                                   : static_cast<std::uint16_t>((data[i] << 8) | data[i + 1]);
    wide.push_back(static_cast<wchar_t>(unit));
  }
  if (wide.empty()) {
    out.clear();
    return true;
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
  if (needed <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(needed));
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), out.data(), needed,
                      nullptr, nullptr);
  return true;
}

// -- One decoded record ------------------------------------------------------
// The record header carries a serial type per column; the value bytes follow in column order. Only
// the value classes the target tables use are decoded (integers and text); blobs and floats are
// recognized for size accounting but never requested.
struct ColumnRef {
  bool isNull = true;
  bool isInteger = false;
  bool isText = false;
  std::int64_t integer = 0;
  const std::uint8_t* text = nullptr;
  std::size_t textSize = 0;
};

class RecordView {
 public:
  /// @brief Decode one record payload (header plus values).
  /// @note The view borrows the payload buffer: it stays valid only until the next row is read.
  [[nodiscard]] bool parse(const std::uint8_t* payload, std::size_t size) {
    const std::uint8_t* end = payload + size;
    const std::uint8_t* cursor = payload;
    std::uint64_t headerSize = 0;
    if (!readVarint(cursor, end, headerSize)) {
      return false;
    }
    if (headerSize < static_cast<std::uint64_t>(cursor - payload) || headerSize > size) {
      return false;
    }
    const std::uint8_t* headerEnd = payload + headerSize;
    const std::uint8_t* body = headerEnd;
    count_ = 0;
    while (cursor < headerEnd) {
      std::uint64_t serial = 0;
      if (!readVarint(cursor, headerEnd, serial)) {
        return false;
      }
      ColumnRef column;
      column.isNull = false;
      std::size_t length = 0;
      if (serial == 0 || serial == 10 || serial == 11) {
        column.isNull = true; // NULL (10/11 are reserved and never appear in a table row)
      } else if (serial <= 6) {
        static constexpr std::size_t kIntegerBytes[6] = {1, 2, 3, 4, 6, 8};
        column.isInteger = true;
        length = kIntegerBytes[serial - 1];
      } else if (serial == 7) {
        length = 8; // float; kept only for the body walk
      } else if (serial == 8 || serial == 9) {
        column.isInteger = true;
        column.integer = serial == 9 ? 1 : 0;
      } else if ((serial & 1) == 0) {
        length = static_cast<std::size_t>((serial - 12) / 2); // blob
      } else {
        column.isText = true;
        length = static_cast<std::size_t>((serial - 13) / 2);
      }
      if (static_cast<std::size_t>(end - body) < length) {
        return false;
      }
      if (column.isInteger && length != 0) {
        column.integer = decodeSigned(body, length);
      }
      if (column.isText) {
        column.text = body;
        column.textSize = length;
      }
      body += length;
      if (count_ < kMaxColumns) {
        columns_[count_++] = column;
      }
    }
    return true;
  }

  /// @brief Read an integer column.
  /// @return true when the column exists and holds an integer (a missing or NULL column fails).
  [[nodiscard]] bool integer(std::size_t index, std::int64_t& out) const {
    if (index >= count_ || columns_[index].isNull || !columns_[index].isInteger) {
      return false;
    }
    out = columns_[index].integer;
    return true;
  }

  /// @brief Read a text column as UTF-8.
  /// @return true when the column exists and holds text.
  [[nodiscard]] bool text(std::size_t index, std::uint32_t encoding, std::string& out) const {
    if (index >= count_ || !columns_[index].isText) {
      return false;
    }
    return textToUtf8(columns_[index].text, columns_[index].textSize, encoding, out);
  }

 private:
  std::array<ColumnRef, kMaxColumns> columns_{};
  std::size_t count_ = 0;
};

using RowCallback = std::function<void(std::int64_t rowid, const RecordView& record)>;

// -- Snapshot parser ---------------------------------------------------------
// Validates the 100-byte file header, resolves the target tables through sqlite_master, and walks
// their table b-trees. Every structural violation fails the read; the caller then falls back.
class DatabaseReader {
 public:
  explicit DatabaseReader(const Bytes& data) : data_(data) {}

  [[nodiscard]] bool initialize() {
    if (data_.size() < kFileHeaderSize || std::memcmp(data_.data(), kMagic, sizeof(kMagic)) != 0) {
      return false;
    }
    std::uint32_t pageSize = readBe16(data_.data() + 16);
    if (pageSize == 1) {
      pageSize = 65536;
    }
    if (pageSize < kMinPageSize || pageSize > kMaxPageSize || (pageSize & (pageSize - 1)) != 0) {
      return false;
    }
    pageSize_ = pageSize;
    if (data_[20] > pageSize_ - 480) { // the usable size must stay at least 480 bytes
      return false;
    }
    usableSize_ = pageSize_ - data_[20];
    textEncoding_ = readBe32(data_.data() + 56);
    if (textEncoding_ != kTextEncodingUtf8 && textEncoding_ != kTextEncodingUtf16Le &&
        textEncoding_ != kTextEncodingUtf16Be) {
      return false;
    }
    fileChangeCounter_ = readBe32(data_.data() + 24);
    const std::size_t filePages = data_.size() / pageSize_;
    if (filePages == 0) {
      return false;
    }
    const std::uint32_t headerPages = readBe32(data_.data() + 28);
    if (headerPages > filePages) {
      return false; // the file is shorter than the header claims: a torn read
    }
    pageCount_ = headerPages != 0 ? headerPages : filePages;
    return true;
  }

  [[nodiscard]] std::uint32_t textEncoding() const { return textEncoding_; }
  [[nodiscard]] std::uint32_t fileChangeCounter() const { return fileChangeCounter_; }
  [[nodiscard]] std::size_t pageCount() const { return pageCount_; }

  /// @brief Walk a table b-tree and invoke onRow for every row.
  /// @return true when the whole tree was read; false on any structural violation.
  [[nodiscard]] bool walkTable(std::uint32_t rootPage, const RowCallback& onRow) {
    if (rootPage < 1 || rootPage > pageCount_) {
      return false;
    }
    std::vector<std::uint32_t> pending{rootPage};
    std::unordered_set<std::uint32_t> visited;
    while (!pending.empty()) {
      const std::uint32_t page = pending.back();
      pending.pop_back();
      if (page < 1 || page > pageCount_ || !visited.insert(page).second) {
        return false;
      }
      const std::uint8_t* start = pageStart(page);
      const std::uint8_t* header = page == 1 ? start + kFileHeaderSize : start;
      const std::uint8_t type = header[0];
      if (type == kPageTypeInteriorTable) {
        if (!walkInteriorPage(page, start, header, pending)) {
          return false;
        }
      } else if (type == kPageTypeLeafTable) {
        if (!walkLeafPage(page, start, header, onRow)) {
          return false;
        }
      } else {
        return false; // an index page (or a free page): not part of this table
      }
    }
    return true;
  }

 private:
  [[nodiscard]] const std::uint8_t* pageStart(std::uint32_t page) const {
    return data_.data() + static_cast<std::size_t>(page - 1) * pageSize_;
  }

  [[nodiscard]] bool walkInteriorPage(std::uint32_t page, const std::uint8_t* start,
                                      const std::uint8_t* header,
                                      std::vector<std::uint32_t>& pending) const {
    const std::uint32_t cellCount = readBe16(header + 3);
    if (cellCount > (usableSize_ - 12) / 2) {
      return false;
    }
    const std::uint8_t* pageEnd = start + usableSize_;
    for (std::uint32_t i = 0; i < cellCount; ++i) {
      const std::uint32_t offset = readBe16(header + 12 + i * 2);
      if (offset < (page == 1 ? kFileHeaderSize : 0) || offset >= usableSize_) {
        return false;
      }
      const std::uint8_t* cell = start + offset;
      if (cell + 4 > pageEnd) {
        return false;
      }
      const std::uint32_t child = readBe32(cell);
      if (child < 1 || child > pageCount_) {
        return false;
      }
      pending.push_back(child);
    }
    const std::uint32_t rightmost = readBe32(header + 8);
    if (rightmost < 1 || rightmost > pageCount_) {
      return false;
    }
    pending.push_back(rightmost);
    return true;
  }

  [[nodiscard]] bool walkLeafPage(std::uint32_t page, const std::uint8_t* start,
                                  const std::uint8_t* header, const RowCallback& onRow) {
    const std::uint32_t cellCount = readBe16(header + 3);
    if (cellCount > (usableSize_ - 8) / 2) {
      return false;
    }
    const std::uint8_t* pageEnd = start + usableSize_;
    for (std::uint32_t i = 0; i < cellCount; ++i) {
      const std::uint32_t offset = readBe16(header + 8 + i * 2);
      if (offset < (page == 1 ? kFileHeaderSize : 0) || offset >= usableSize_) {
        return false;
      }
      const std::uint8_t* cell = start + offset;
      const std::uint8_t* cursor = cell;
      std::uint64_t payloadSize = 0;
      std::uint64_t rowid = 0;
      if (!readVarint(cursor, pageEnd, payloadSize) || !readVarint(cursor, pageEnd, rowid)) {
        return false;
      }
      if (payloadSize > kMaxPayloadBytes) {
        logDebugF("mods-sqlite: skipping oversized row (%llu bytes)",
                  static_cast<unsigned long long>(payloadSize));
        continue;
      }
      if (!assemblePayload(cursor, pageEnd, payloadSize)) {
        return false;
      }
      RecordView record;
      if (!record.parse(payload_.data(), payload_.size())) {
        return false;
      }
      onRow(static_cast<std::int64_t>(rowid), record);
    }
    return true;
  }

  // Copy a row payload into payload_: the local part (size from the SQLite formula) plus the
  // overflow-page chain, each overflow page starting with a 4-byte next-page pointer.
  [[nodiscard]] bool assemblePayload(const std::uint8_t* cursor, const std::uint8_t* limit,
                                     std::uint64_t payloadSize) {
    if (cursor > limit) {
      return false;
    }
    const std::uint64_t usable = usableSize_;
    std::uint64_t local = payloadSize;
    if (payloadSize > usable - 35) {
      const std::uint64_t maxLocal = ((usable - 12) * 32) / 255 - 23;
      const std::uint64_t minLocal = maxLocal + (payloadSize - maxLocal) % (usable - 4);
      local = minLocal <= usable - 35 ? minLocal : maxLocal;
    }
    if (local > payloadSize) {
      local = payloadSize;
    }
    if (static_cast<std::size_t>(limit - cursor) < local) {
      return false;
    }
    payload_.assign(cursor, cursor + local);
    cursor += local;
    std::uint64_t remaining = payloadSize - local;
    std::uint32_t next = 0;
    if (remaining > 0) {
      if (static_cast<std::size_t>(limit - cursor) < 4) {
        return false;
      }
      next = readBe32(cursor);
    }
    while (remaining > 0) {
      if (next < 1 || next > pageCount_) {
        return false;
      }
      const std::uint8_t* overflow = pageStart(next);
      const std::size_t chunk =
          static_cast<std::size_t>(std::min<std::uint64_t>(remaining, usable - 4));
      if (overflow + 4 + chunk > data_.data() + data_.size()) {
        return false;
      }
      payload_.insert(payload_.end(), overflow + 4, overflow + 4 + chunk);
      remaining -= chunk;
      if (remaining > 0) {
        next = readBe32(overflow);
      }
    }
    return true;
  }

  const Bytes& data_;
  std::size_t pageSize_ = 0;
  std::size_t usableSize_ = 0;
  std::size_t pageCount_ = 0;
  std::uint32_t textEncoding_ = 0;
  std::uint32_t fileChangeCounter_ = 0;
  Bytes payload_;
};

// Master scan plus the four target tables. Mods.ModRowId and ScannedFiles.ScannedFileRowId are
// INTEGER PRIMARY KEY aliases of the cell rowid, so the row identity comes from the cell, not the
// record (whose first column is then NULL).
[[nodiscard]] bool parseSnapshot(const Bytes& data, ModsSqliteSnapshot& snapshot) {
  DatabaseReader reader(data);
  if (!reader.initialize()) {
    logDebug("mods-sqlite: the file header is not a usable SQLite database");
    return false;
  }
  logDebugF("mods-sqlite: snapshot pages=%zu counter=%u", reader.pageCount(),
            reader.fileChangeCounter());
  const std::uint32_t encoding = reader.textEncoding();

  std::unordered_map<std::string, std::uint32_t> roots;
  const RowCallback masterRow = [&roots, &reader](std::int64_t, const RecordView& record) {
    const std::uint32_t encoding = reader.textEncoding();
    std::string type;
    std::string name;
    std::int64_t rootPage = 0;
    if (!record.text(0, encoding, type) || !record.text(1, encoding, name) ||
        !record.integer(3, rootPage)) {
      return; // malformed master row; the required tables are validated afterwards
    }
    if (type != "table" || rootPage <= 0 ||
        rootPage > static_cast<std::int64_t>(reader.pageCount())) {
      return;
    }
    roots.emplace(std::move(name), static_cast<std::uint32_t>(rootPage));
  };
  if (!reader.walkTable(1, masterRow)) {
    logDebug("mods-sqlite: sqlite_master could not be read");
    return false;
  }
  const auto groupRoot = roots.find("ModGroups");
  const auto itemsRoot = roots.find("ModGroupItems");
  const auto modsRoot = roots.find("Mods");
  const auto filesRoot = roots.find("ScannedFiles");
  if (groupRoot == roots.end() || itemsRoot == roots.end() || modsRoot == roots.end()) {
    logDebug("mods-sqlite: the expected tables are missing");
    return false;
  }

  // ModGroups: the selected group (the lowest rowid wins when several rows are marked selected).
  std::int64_t selectedGroup = 0;
  std::string selectedName;
  bool foundSelected = false;
  const RowCallback groupRow = [&](std::int64_t rowid, const RecordView& record) {
    std::int64_t selected = 0;
    std::string name;
    if (!record.integer(3, selected) || selected == 0 ||
        !record.text(1, static_cast<std::uint32_t>(encoding), name)) {
      return;
    }
    if (!foundSelected || rowid < selectedGroup) {
      foundSelected = true;
      selectedGroup = rowid;
      selectedName = std::move(name);
    }
  };
  if (!reader.walkTable(groupRoot->second, groupRow) || !foundSelected) {
    logDebug("mods-sqlite: no selected mod group");
    return false;
  }

  // ModGroupItems: the selected group's rows, split into all members (diagnostics) and the
  // non-disabled members (the enabled set).
  std::size_t memberCount = 0;
  std::unordered_set<std::int64_t> memberRows;
  const RowCallback itemRow = [&](std::int64_t, const RecordView& record) {
    std::int64_t group = 0;
    std::int64_t mod = 0;
    std::int64_t disabled = 0;
    if (!record.integer(0, group) || !record.integer(1, mod) || group != selectedGroup) {
      return;
    }
    ++memberCount;
    if (record.integer(2, disabled) && disabled != 0) {
      return;
    }
    memberRows.insert(mod);
  };
  if (!reader.walkTable(itemsRoot->second, itemRow)) {
    logDebug("mods-sqlite: ModGroupItems could not be read");
    return false;
  }

  // Mods: the ids of the member rows.
  struct ModRow {
    std::string id;
    std::int64_t scannedFile = 0;
  };
  std::unordered_map<std::int64_t, ModRow> memberMods;
  const RowCallback modRow = [&](std::int64_t rowid, const RecordView& record) {
    if (memberRows.count(rowid) == 0) {
      return;
    }
    std::int64_t scannedFile = 0;
    std::string id;
    if (!record.integer(1, scannedFile) || !record.text(2, encoding, id)) {
      return;
    }
    id = toLowerAscii(std::move(id));
    if (!id.empty()) {
      memberMods.emplace(rowid, ModRow{std::move(id), scannedFile});
    }
  };
  if (!reader.walkTable(modsRoot->second, modRow)) {
    logDebug("mods-sqlite: Mods could not be read");
    return false;
  }

  // ScannedFiles: the .modinfo path per scanned file row. Missing paths only reduce the plugin
  // discovery reach, never the enabled set itself, so this table degrades to "no paths".
  std::unordered_map<std::int64_t, std::wstring> scannedPaths;
  bool pathsRead = false;
  if (filesRoot != roots.end()) {
    const RowCallback fileRow = [&](std::int64_t rowid, const RecordView& record) {
      std::string path;
      if (!record.text(1, encoding, path) || path.empty()) {
        return;
      }
      scannedPaths.emplace(rowid, utf8ToWide(path));
    };
    pathsRead = reader.walkTable(filesRoot->second, fileRow);
    if (!pathsRead) {
      logWarn("mods-sqlite: ScannedFiles could not be read; .modinfo paths are unavailable");
    }
  }

  // Deduplicate by id (a mod id never repeats in practice) and sort for deterministic logs.
  std::vector<std::pair<std::string, std::wstring>> byId;
  std::unordered_set<std::string> seen;
  for (const std::pair<const std::int64_t, ModRow>& entry : memberMods) {
    if (!seen.insert(entry.second.id).second) {
      continue;
    }
    std::wstring path;
    if (pathsRead) {
      const auto it = scannedPaths.find(entry.second.scannedFile);
      if (it != scannedPaths.end()) {
        path = it->second;
      }
    }
    byId.emplace_back(entry.second.id, std::move(path));
  }
  std::sort(byId.begin(), byId.end(),
            [](const std::pair<std::string, std::wstring>& left,
               const std::pair<std::string, std::wstring>& right) {
              return left.first < right.first;
            });

  snapshot.enabledModIds.reserve(byId.size());
  for (std::pair<std::string, std::wstring>& entry : byId) {
    snapshot.enabledModIds.push_back(std::move(entry.first));
    if (!entry.second.empty()) {
      snapshot.idToModinfoPath.emplace_back(snapshot.enabledModIds.back(),
                                            std::move(entry.second));
    }
  }
  snapshot.selectedGroupRowId = selectedGroup;
  snapshot.selectedGroupName = std::move(selectedName);
  snapshot.groupMemberCount = memberCount;
  snapshot.fileChangeCounter = reader.fileChangeCounter();
  return true;
}

// -- System SQLite fallback (winsqlite3.dll) ---------------------------------
// The WAL case and any snapshot-parse failure are served by the system's own SQLite, read-only.
// Every entry point is resolved dynamically and is optional: when one is missing or a call fails,
// the read is abandoned.

struct SqliteDb;
struct SqliteStmt;

using SqliteOpenV2Fn = int (*)(const char*, SqliteDb**, int, const char*);
using SqliteCloseFn = int (*)(SqliteDb*);
using SqlitePrepareFn = int (*)(SqliteDb*, const char*, int, SqliteStmt**, const char**);
using SqliteBindInt64Fn = int (*)(SqliteStmt*, int, std::int64_t);
using SqliteStepFn = int (*)(SqliteStmt*);
using SqliteFinalizeFn = int (*)(SqliteStmt*);
using SqliteColumnTextFn = const unsigned char* (*)(SqliteStmt*, int);
using SqliteColumnBytesFn = int (*)(SqliteStmt*, int);
using SqliteColumnInt64Fn = std::int64_t (*)(SqliteStmt*, int);
using SqliteBusyTimeoutFn = int (*)(SqliteDb*, int);

constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadonly = 0x00000001;

struct SqliteApi {
  SqliteOpenV2Fn openV2 = nullptr;
  SqliteCloseFn close = nullptr;
  SqlitePrepareFn prepare = nullptr;
  SqliteBindInt64Fn bindInt64 = nullptr;
  SqliteStepFn step = nullptr;
  SqliteFinalizeFn finalize = nullptr;
  SqliteColumnTextFn columnText = nullptr;
  SqliteColumnBytesFn columnBytes = nullptr;
  SqliteColumnInt64Fn columnInt64 = nullptr;
  SqliteBusyTimeoutFn busyTimeout = nullptr;
};

bool loadSqliteApi(HMODULE module, SqliteApi& api) {
  api.openV2 = reinterpret_cast<SqliteOpenV2Fn>(GetProcAddress(module, "sqlite3_open_v2"));
  api.close = reinterpret_cast<SqliteCloseFn>(GetProcAddress(module, "sqlite3_close"));
  api.prepare = reinterpret_cast<SqlitePrepareFn>(GetProcAddress(module, "sqlite3_prepare_v2"));
  api.bindInt64 = reinterpret_cast<SqliteBindInt64Fn>(GetProcAddress(module, "sqlite3_bind_int64"));
  api.step = reinterpret_cast<SqliteStepFn>(GetProcAddress(module, "sqlite3_step"));
  api.finalize = reinterpret_cast<SqliteFinalizeFn>(GetProcAddress(module, "sqlite3_finalize"));
  api.columnText =
      reinterpret_cast<SqliteColumnTextFn>(GetProcAddress(module, "sqlite3_column_text"));
  api.columnBytes =
      reinterpret_cast<SqliteColumnBytesFn>(GetProcAddress(module, "sqlite3_column_bytes"));
  api.columnInt64 =
      reinterpret_cast<SqliteColumnInt64Fn>(GetProcAddress(module, "sqlite3_column_int64"));
  api.busyTimeout =
      reinterpret_cast<SqliteBusyTimeoutFn>(GetProcAddress(module, "sqlite3_busy_timeout"));
  return api.openV2 != nullptr && api.close != nullptr && api.prepare != nullptr &&
         api.bindInt64 != nullptr && api.step != nullptr && api.finalize != nullptr &&
         api.columnText != nullptr && api.columnBytes != nullptr && api.columnInt64 != nullptr;
}

// RAII for a prepared statement: finalized on every exit path.
class StatementGuard {
 public:
  StatementGuard(const SqliteApi& api, SqliteStmt* statement)
      : api_(&api), statement_(statement) {}
  ~StatementGuard() {
    if (statement_ != nullptr && api_->finalize(statement_) != kSqliteOk) {
      logDebug("mods-sqlite: sqlite3_finalize returned non-OK; a statement may stay unfinalized");
    }
  }
  StatementGuard(const StatementGuard&) = delete;
  StatementGuard& operator=(const StatementGuard&) = delete;

 private:
  const SqliteApi* api_;
  SqliteStmt* statement_;
};

// Close a connection opened through winsqlite3. sqlite3_close returns SQLITE_BUSY while an
// unfinalized statement still holds the connection; the statement guards finalize every statement,
// so this branch should stay rare, but a failure is logged instead of silently leaking.
void closeSqliteConnection(const SqliteApi& api, SqliteDb* db) {
  const int closeResult = api.close(db);
  if (closeResult != kSqliteOk) {
    logWarnF("mods-sqlite: sqlite3_close returned %d; the connection may stay open", closeResult);
  }
}

[[nodiscard]] bool readByWinSqlite(const std::wstring& path, ModsSqliteSnapshot& snapshot) {
  HMODULE module = LoadLibraryW(L"winsqlite3.dll");
  if (module == nullptr) {
    logDebug("mods-sqlite: winsqlite3.dll is unavailable");
    return false;
  }
  SqliteApi api;
  if (!loadSqliteApi(module, api)) {
    logDebug("mods-sqlite: winsqlite3.dll does not export the expected entry points");
    FreeLibrary(module);
    return false;
  }
  const std::string narrowPath = wideToUtf8(path);
  SqliteDb* db = nullptr;
  if (narrowPath.empty() ||
      api.openV2(narrowPath.c_str(), &db, kSqliteOpenReadonly, nullptr) != kSqliteOk ||
      db == nullptr) {
    if (db != nullptr) {
      closeSqliteConnection(api, db);
    }
    logDebug("mods-sqlite: opening the database read-only through winsqlite3 failed");
    FreeLibrary(module);
    return false;
  }
  if (api.busyTimeout != nullptr) {
    api.busyTimeout(db, 250); // best effort: a concurrent writer must not fail the read instantly
  }

  bool read = false;
  SqliteStmt* statement = nullptr;
  if (api.prepare(db,
                  "SELECT ModGroupRowId, Name FROM ModGroups WHERE Selected=1 "
                  "ORDER BY ModGroupRowId LIMIT 1",
                  -1, &statement, nullptr) == kSqliteOk &&
      statement != nullptr) {
    StatementGuard guard(api, statement);
    if (api.step(statement) == kSqliteRow) {
      snapshot.selectedGroupRowId = api.columnInt64(statement, 0);
      const unsigned char* name = api.columnText(statement, 1);
      const int nameBytes = api.columnBytes(statement, 1);
      if (name != nullptr && nameBytes > 0) {
        snapshot.selectedGroupName.assign(reinterpret_cast<const char*>(name),
                                          static_cast<std::size_t>(nameBytes));
      }
      read = true;
    }
  }
  if (!read) {
    closeSqliteConnection(api, db);
    FreeLibrary(module);
    logDebug("mods-sqlite: no selected mod group through winsqlite3");
    return false;
  }

  // Members: join through ModGroupItems to the Mods rows and their ScannedFiles path.
  bool membersComplete = false;
  statement = nullptr;
  if (api.prepare(db,
                  "SELECT m.ModId, s.Path FROM ModGroupItems i "
                  "JOIN Mods m ON m.ModRowId=i.ModRowId "
                  "LEFT JOIN ScannedFiles s ON s.ScannedFileRowId=m.ScannedFileRowId "
                  "WHERE i.ModGroupRowId=?1 AND (i.Disabled IS NULL OR i.Disabled=0)",
                  -1, &statement, nullptr) == kSqliteOk &&
      statement != nullptr) {
    {
      StatementGuard guard(api, statement);
      api.bindInt64(statement, 1, snapshot.selectedGroupRowId);
      std::unordered_set<std::string> seen;
      int stepResult = kSqliteRow;
      while ((stepResult = api.step(statement)) == kSqliteRow) {
        const unsigned char* id = api.columnText(statement, 0);
        const int idBytes = api.columnBytes(statement, 0);
        if (id == nullptr || idBytes <= 0) {
          continue;
        }
        std::string modId = toLowerAscii(
            std::string(reinterpret_cast<const char*>(id), static_cast<std::size_t>(idBytes)));
        if (!seen.insert(modId).second) {
          continue;
        }
        const unsigned char* filePath = api.columnText(statement, 1);
        const int pathBytes = api.columnBytes(statement, 1);
        std::wstring widePath;
        if (filePath != nullptr && pathBytes > 0) {
          widePath = utf8ToWide(std::string(reinterpret_cast<const char*>(filePath),
                                            static_cast<std::size_t>(pathBytes)));
        }
        snapshot.enabledModIds.push_back(modId);
        if (!widePath.empty()) {
          snapshot.idToModinfoPath.emplace_back(modId, std::move(widePath));
        }
      }
      // A partial member list must never pass as a complete enabled set: an interrupted query
      // abandons the read.
      membersComplete = stepResult == kSqliteDone;
    }
  }
  if (!membersComplete) {
    closeSqliteConnection(api, db);
    FreeLibrary(module);
    logWarn("mods-sqlite: the member query did not complete through winsqlite3");
    return false;
  }
  std::sort(snapshot.enabledModIds.begin(), snapshot.enabledModIds.end());
  std::sort(snapshot.idToModinfoPath.begin(), snapshot.idToModinfoPath.end(),
            [](const std::pair<std::string, std::wstring>& left,
               const std::pair<std::string, std::wstring>& right) {
              return left.first < right.first;
            });

  // Group member count including disabled rows (diagnostics only).
  statement = nullptr;
  if (api.prepare(db, "SELECT COUNT(*) FROM ModGroupItems WHERE ModGroupRowId=?1", -1, &statement,
                  nullptr) == kSqliteOk &&
      statement != nullptr) {
    StatementGuard guard(api, statement);
    api.bindInt64(statement, 1, snapshot.selectedGroupRowId);
    if (api.step(statement) == kSqliteRow) {
      snapshot.groupMemberCount = static_cast<std::size_t>(api.columnInt64(statement, 0));
    }
  }

  closeSqliteConnection(api, db);
  FreeLibrary(module);
  return true;
}

// -- File helpers ------------------------------------------------------------

bool readWholeFile(const std::wstring& path, Bytes& out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaxFileBytes) {
    CloseHandle(file);
    return false;
  }
  out.resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t total = 0;
  while (total < out.size()) {
    DWORD read = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - total, 1u << 20));
    if (!ReadFile(file, out.data() + total, want, &read, nullptr) || read == 0) {
      CloseHandle(file);
      return false;
    }
    total += read;
  }
  CloseHandle(file);
  return true;
}

// A non-empty -wal sidecar means the main file alone may not carry the latest commits.
bool hasWalSidecar(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (!GetFileAttributesExW((path + L"-wal").c_str(), GetFileExInfoStandard, &attributes)) {
    return false;
  }
  const std::uint64_t size =
      (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
  return size > 0;
}

std::uint32_t readFileChangeCounter(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return 0;
  }
  std::uint8_t header[kFileHeaderSize] = {};
  DWORD read = 0;
  const BOOL ok = ReadFile(file, header, sizeof(header), &read, nullptr);
  CloseHandle(file);
  if (!ok || read < kFileHeaderSize || std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
    return 0;
  }
  return readBe32(header + 24);
}

} // namespace

std::wstring modsSqlitePath() {
  PWSTR base = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &base)) ||
      base == nullptr) {
    return {};
  }
  std::wstring path(base);
  CoTaskMemFree(base);
  path += L"\\Firaxis Games\\Sid Meier's Civilization VI\\Mods.sqlite";
  return path;
}

bool readModsSqlite(ModsSqliteSnapshot& out) {
  const std::wstring path = modsSqlitePath();
  if (path.empty()) {
    logError("mods-sqlite: cannot resolve the database path");
    return false;
  }

  ModsSqliteSnapshot snapshot;
  bool parsed = false;
  const bool walPresent = hasWalSidecar(path);
  if (walPresent) {
    // The snapshot parser does not replay the WAL; the system SQLite reads the log correctly.
    logDebug("mods-sqlite: a WAL sidecar is present; using the system SQLite");
  } else {
    // Whole-file snapshot. The read is retried once: the front-end may rewrite the database while
    // the Loader starts, and a torn first attempt must not abort the source.
    for (int attempt = 0; attempt < 2 && !parsed; ++attempt) {
      Bytes data;
      if (!readWholeFile(path, data)) {
        logDebug("mods-sqlite: the database file could not be read");
        break;
      }
      if (data.size() < kFileHeaderSize || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        logWarn("mods-sqlite: the file is not an SQLite database");
        break;
      }
      parsed = parseSnapshot(data, snapshot);
      if (!parsed && attempt == 0) {
        logDebug("mods-sqlite: the snapshot parse failed; retrying once");
      }
    }
  }

  if (!parsed && readByWinSqlite(path, snapshot)) {
    parsed = true;
    snapshot.usedWinSqlite = true;
    snapshot.fileChangeCounter = readFileChangeCounter(path);
  }
  if (!parsed) {
    logWarn("mods-sqlite: no usable enabled-mod data");
    return false;
  }

  logInfoF("mods-sqlite: selected group '%s' (rowid %lld) members=%zu enabled=%zu%s",
           snapshot.selectedGroupName.c_str(), static_cast<long long>(snapshot.selectedGroupRowId),
           snapshot.groupMemberCount, snapshot.enabledModIds.size(),
           snapshot.usedWinSqlite ? " via winsqlite3" : "");
  snapshot.ok = true;
  out = std::move(snapshot);
  return true;
}

} // namespace ykkz000::loader
