#include <windows.h>

#include <shlobj.h>
#include <knownfolders.h>
#include <winreg.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {
namespace {

// One host view per plugin (a copy of bridge::Host): pluginHandle points at this structure and the
// plugin hands the ownership back through host->pluginHandle; the plugin sets host->onGameContext
// to its own context callback in GetPlugin, and the Loader then broadcasts lifecycle events
// through it.
struct PluginSlot {
  HMODULE module = nullptr;
  bridge::DestroyPluginFn destroy = nullptr;
  bridge::ContextListenerFn listener = nullptr;
  std::string name;
  bridge::Host host = {};
};

std::vector<PluginSlot*> g_plugins;
bool g_pluginsLoaded = false;
bool g_crossModLoaded = false;
// Stashed by loadPlugins (phase A) for the deferred cross-mod phase (phase B), so the cross-mod
// discovery can initialize its plugins against the same host table without exposing g_host globally.
bridge::Host* g_hostTemplate = nullptr;
// Enabled-mod ids of the Mods.sqlite selected group (the ModGroups row with Selected set), seeded
// by the proxy before the real DllCreateGameContext. Consumed by loadPlugins, which then scans the
// cross-mod directories in phase A. Kept afterwards: a set seeded after phase A (a Mods.sqlite
// re-read) still feeds the deferred cross-mod phase.
std::vector<std::string> g_groupEnabledModIds;
// The .modinfo paths belonging to g_groupEnabledModIds, as reported by the seeding source. Plugin
// discovery probes the mod root (the .modinfo file's directory) of every seeded id in addition to
// the Documents Mods and Workshop scans, covering enabled mods outside those roots (for example
// base-game/DLC content).
std::vector<std::pair<std::string, std::wstring>> g_groupEnabledModInfoPaths;

std::mutex g_activeMutex;
void* g_activePluginHandle = nullptr;

// Directories added to the process DLL search path for the lifetime of the loaded plugins, so a
// consumer in one mod's plugin directory can import an API DLL from another mod's directory.
std::vector<DLL_DIRECTORY_COOKIE> g_dllDirectoryCookies;

void addDllSearchDirectories(const std::vector<std::wstring>& directories) {
  for (const std::wstring& directory : directories) {
    if (directory.empty() || GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(directory.c_str());
    if (cookie != nullptr) {
      g_dllDirectoryCookies.push_back(cookie);
    } else {
      logWarn(L"AddDllDirectory failed; cross-directory imports may not resolve: " + directory);
    }
  }
}

void removeDllSearchDirectories() {
  for (DLL_DIRECTORY_COOKIE cookie : g_dllDirectoryCookies) {
    RemoveDllDirectory(cookie);
  }
  g_dllDirectoryCookies.clear();
}

std::wstring normalized(const std::wstring& path) {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
  if (length == 0 || length >= MAX_PATH) {
    return path;
  }
  return std::wstring(buffer, length);
}

void addUnique(std::vector<std::wstring>& out, const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  const std::wstring candidate = normalized(path);
  for (const std::wstring& existing : out) {
    // Windows paths are case-insensitive: the same directory spelled with different casing (for
    // example a registry Steam path versus its canonical form in libraryfolders.vdf) must collapse
    // into one entry, or the directory is scanned and registered twice.
    if (_wcsicmp(existing.c_str(), candidate.c_str()) == 0) {
      return;
    }
  }
  out.push_back(candidate);
}

// -- Cross-mod plugin-directory discovery --
// Besides the loader's own directory, discover the plugin directories carried by other mods: in the
// Documents Mods directory and in Steam Workshop content (AppID 289070). Two views are derived from
// the same layout:
//   * the load view (addModPluginDir) keeps only *enabled* mods: only directories literally named
//     ykkz000_civ6_plugin are ever loaded, and only when the owning mod is a member of the
//     Mods.sqlite selected group; a mod whose id cannot be read or is not a member is skipped.
//   * the dependency-search view (discoverAllModPluginDirs) keeps every mod's plugin directory
//     regardless of the enabled state; those directories are registered on the DLL search path so a
//     consumer can import the API DLLs another mod ships, but they are never scanned or loaded.

bool isDirectory(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string toLowerAscii(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// Read up to maxBytes of a file into a string.
bool readFileToString(const std::wstring& path, std::string& out, std::size_t maxBytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
    CloseHandle(file);
    return false;
  }
  std::size_t bytes = static_cast<std::size_t>(size.QuadPart);
  if (bytes > maxBytes) {
    bytes = maxBytes;
  }
  out.resize(bytes);
  DWORD read = 0;
  const BOOL ok = ReadFile(file, out.data(), static_cast<DWORD>(bytes), &read, nullptr);
  CloseHandle(file);
  if (!ok) {
    return false;
  }
  out.resize(read);
  return true;
}

// Read the `<Mod id="...">` UUID from a .modinfo file (the first Mod element).
bool readModInfoId(const std::wstring& modinfoPath, std::string& out) {
  std::string text;
  if (!readFileToString(modinfoPath, text, 1024 * 1024)) {
    return false;
  }
  const std::size_t mod = text.find("<Mod");
  if (mod == std::string::npos) {
    return false;
  }
  const std::size_t tagEnd = text.find('>', mod);
  const std::size_t id = text.find("id=\"", mod);
  if (id == std::string::npos || (tagEnd != std::string::npos && id > tagEnd)) {
    return false;
  }
  const std::size_t valueBegin = id + 4;
  const std::size_t valueEnd = text.find('"', valueBegin);
  if (valueEnd == std::string::npos) {
    return false;
  }
  out.assign(text, valueBegin, valueEnd - valueBegin);
  return !out.empty();
}

// Find the first *.modinfo directly inside a mod root directory.
bool findTopLevelModInfo(const std::wstring& modRoot, std::wstring& out) {
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = modRoot + L"\\*.modinfo";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool found = false;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    out = modRoot + L"\\" + data.cFileName;
    found = true;
    break;
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
  return found;
}

// Probe one mod root for its plugin directory (regardless of the mod's enabled state) and add it to
// out. The plugin directory lives either under Binaries\Win64 (a normal deployment) or directly at
// the mod root (some Workshop packages keep no Binaries layer).
bool findModPluginDir(const std::wstring& modRoot, std::vector<std::wstring>& out) {
  const std::wstring underBinaries = modRoot + L"\\Binaries\\Win64\\ykkz000_civ6_plugin";
  if (isDirectory(underBinaries)) {
    addUnique(out, underBinaries);
    return true;
  }
  const std::wstring direct = modRoot + L"\\ykkz000_civ6_plugin";
  if (isDirectory(direct)) {
    addUnique(out, direct);
    return true;
  }
  return false;
}

// Append the plugin directory of one mod root when its .modinfo id is in the enabled set. This
// filtered view decides what the Loader *loads*; the unfiltered discovery below decides only which
// directories the DLL loader may *resolve dependencies* from.
void addModPluginDir(const std::wstring& modRoot, const std::unordered_set<std::string>& enabled,
                     std::vector<std::wstring>& out) {
  std::wstring modinfo;
  if (!findTopLevelModInfo(modRoot, modinfo)) {
    return; // not a mod root
  }
  std::string id;
  if (!readModInfoId(modinfo, id)) {
    logWarn(L"Mod has no readable id; skipping: " + modRoot);
    return;
  }
  if (enabled.count(toLowerAscii(id)) == 0) {
    logDebug(L"Mod is not enabled; skipping its plugin directory: " + modRoot);
    return;
  }
  (void)findModPluginDir(modRoot, out);
}

// Enumerate immediate subdirectories of a mods container and add the enabled mods' plugin
// directories.
void discoverModPluginDirs(const std::wstring& modsRoot,
                           const std::unordered_set<std::string>& enabled,
                           std::vector<std::wstring>& out) {
  if (!isDirectory(modsRoot)) {
    return;
  }
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = modsRoot + L"\\*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;
    }
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
      continue;
    }
    addModPluginDir(modsRoot + L"\\" + data.cFileName, enabled, out);
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
}

// Enumerate immediate subdirectories of a mods container and add every mod's plugin directory,
// regardless of the owning mod's enabled state. These directories feed only the process DLL search
// path, so a disabled mod's plugin DLLs become resolvable as dependencies of an enabled plugin;
// they are never scanned, loaded or initialized by the Loader.
void discoverAllModPluginDirs(const std::wstring& modsRoot, std::vector<std::wstring>& out) {
  if (!isDirectory(modsRoot)) {
    return;
  }
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = modsRoot + L"\\*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      continue;
    }
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
      continue;
    }
    const std::wstring modRoot = modsRoot + L"\\" + data.cFileName;
    std::wstring modinfo;
    if (!findTopLevelModInfo(modRoot, modinfo)) {
      continue; // not a mod root
    }
    std::string id;
    if (!readModInfoId(modinfo, id)) {
      logDebug(L"Mod has no readable id; not used as a dependency search root: " + modRoot);
      continue;
    }
    (void)findModPluginDir(modRoot, out);
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
}

// <Documents>\My Games\Sid Meier's Civilization VI\Mods.
std::wstring documentsModsRoot() {
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &path)) ||
      path == nullptr) {
    return {};
  }
  const std::wstring root(path);
  CoTaskMemFree(path);
  return root + L"\\My Games\\Sid Meier's Civilization VI\\Mods";
}

// HKCU\Software\Valve\Steam\SteamPath.
std::wstring steamInstallPath() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return {};
  }
  wchar_t buffer[1024] = {};
  DWORD size = sizeof(buffer);
  DWORD type = 0;
  const LONG result = RegQueryValueExW(key, L"SteamPath", nullptr, &type,
                                       reinterpret_cast<LPBYTE>(buffer), &size);
  RegCloseKey(key);
  if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return {};
  }
  return std::wstring(buffer);
}

// Parse the `"path"` values from steamapps\libraryfolders.vdf and append them to out.
void appendSteamLibraryPaths(const std::wstring& vdfPath, std::vector<std::wstring>& out) {
  std::string text;
  if (!readFileToString(vdfPath, text, 1024 * 1024)) {
    return;
  }
  const std::string needle = "\"path\"";
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    pos += needle.size();
    const std::size_t open = text.find('"', pos);
    if (open == std::string::npos) {
      break;
    }
    const std::size_t close = text.find('"', open + 1);
    if (close == std::string::npos) {
      break;
    }
    // VDF escapes each backslash as "\\".
    std::string value;
    value.reserve(close - open - 1);
    for (std::size_t i = open + 1; i < close; ++i) {
      if (text[i] == '\\' && i + 1 < close && text[i + 1] == '\\') {
        value.push_back('\\');
        ++i;
      } else {
        value.push_back(text[i]);
      }
    }
    pos = close + 1;
    if (value.empty()) {
      continue;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) {
      continue;
    }
    std::wstring wide(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), needed);
    addUnique(out, wide);
  }
}

// Steam library roots: the install itself plus every `"path"` in libraryfolders.vdf.
std::vector<std::wstring> steamLibraryRoots() {
  std::vector<std::wstring> roots;
  const std::wstring steam = steamInstallPath();
  if (steam.empty()) {
    return roots;
  }
  addUnique(roots, steam);
  appendSteamLibraryPaths(steam + L"\\steamapps\\libraryfolders.vdf", roots);
  return roots;
}

// Steam Workshop content for Civ6 (AppID 289070) across every library.
void discoverWorkshopPluginDirs(const std::unordered_set<std::string>& enabled,
                                std::vector<std::wstring>& out) {
  const std::vector<std::wstring> roots = steamLibraryRoots();
  if (roots.empty()) {
    logDebug(L"Steam installation not found; skipping Workshop discovery");
    return;
  }
  for (const std::wstring& root : roots) {
    discoverModPluginDirs(root + L"\\steamapps\\workshop\\content\\289070", enabled, out);
  }
}

// Every mod's plugin directory in Steam Workshop content (dependency search roots: the mod's
// enabled state is irrelevant here).
void discoverAllWorkshopPluginDirs(std::vector<std::wstring>& out) {
  const std::vector<std::wstring> roots = steamLibraryRoots();
  if (roots.empty()) {
    logDebug(L"Steam installation not found; skipping Workshop discovery");
    return;
  }
  for (const std::wstring& root : roots) {
    discoverAllModPluginDirs(root + L"\\steamapps\\workshop\\content\\289070", out);
  }
}

// The Loader's own plugin directories (phase A): no enabled-mod-list dependency, so they are always
// scanned. The loader's own mod is by definition enabled.
std::vector<std::wstring> loaderOwnPluginDirs() {
  std::vector<std::wstring> dirs;
  const std::wstring base = moduleDirectory();
  if (base.empty()) {
    return dirs;
  }
  // Scan only the dedicated plugin directories; never load arbitrary DLLs from the game Binaries
  // directory.
  addUnique(dirs, base + L"\\ykkz000_civ6_plugin");
  addUnique(dirs, base + L"\\..\\..\\..\\..\\ykkz000_civ6_plugin");
  return dirs;
}

// Cross-mod plugin directories (phase B): the plugin directories carried by other enabled mods, in
// the Documents Mods directory and in Steam Workshop content.
std::vector<std::wstring> crossModPluginDirs(const std::unordered_set<std::string>& enabled) {
  std::vector<std::wstring> dirs;
  const std::wstring documents = documentsModsRoot();
  if (!documents.empty()) {
    discoverModPluginDirs(documents, enabled, dirs);
  }
  discoverWorkshopPluginDirs(enabled, dirs);
  return dirs;
}

// The game executable's directory: the base for the relative .modinfo paths the engine stores for
// base-game and DLC content ("../../../DLC/Expansion2/Expansion2.modinfo").
std::wstring hostExecutableDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(GetModuleHandleW(nullptr), buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  const std::wstring path(buffer, length);
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

// Add the plugin directory of the mod a scanned .modinfo path belongs to. The path may be absolute
// (either separator) or relative to the game executable's directory, as the engine stores both
// forms; a mod root without a plugin directory is a cheap no-op.
void addModPluginDirByModinfo(const std::wstring& modinfoPath, std::vector<std::wstring>& out) {
  if (modinfoPath.empty()) {
    return;
  }
  const bool absolute = modinfoPath.size() >= 2 && (modinfoPath[1] == L':' ||
                       (modinfoPath[0] == L'\\' && modinfoPath[1] == L'\\'));
  std::wstring candidate = modinfoPath;
  if (!absolute) {
    const std::wstring base = hostExecutableDirectory();
    if (base.empty()) {
      return;
    }
    candidate = base + L"\\" + candidate;
  }
  const std::wstring full = normalized(candidate);
  const std::size_t slash = full.find_last_of(L"\\/");
  if (slash == std::wstring::npos || slash == 0) {
    return;
  }
  (void)findModPluginDir(full.substr(0, slash), out);
}

// Plugin directories derived from the seeded .modinfo paths, restricted to the seeded enabled set.
std::vector<std::wstring> seededModPluginDirs(
    const std::unordered_set<std::string>& enabled,
    const std::vector<std::pair<std::string, std::wstring>>& modinfoPaths) {
  std::vector<std::wstring> dirs;
  for (const std::pair<std::string, std::wstring>& entry : modinfoPaths) {
    if (enabled.count(toLowerAscii(entry.first)) == 0) {
      continue;
    }
    addModPluginDirByModinfo(entry.second, dirs);
  }
  return dirs;
}

// Dependency search roots: the plugin directories of every mod (enabled or not) in the Documents
// Mods directory and in Steam Workshop content. Registered on the process DLL search path only, so
// a consumer can import an API DLL another mod ships, while plugin discovery stays restricted to
// the enabled set.
std::vector<std::wstring> allModPluginDirs() {
  std::vector<std::wstring> dirs;
  const std::wstring documents = documentsModsRoot();
  if (!documents.empty()) {
    discoverAllModPluginDirs(documents, dirs);
  }
  discoverAllWorkshopPluginDirs(dirs);
  return dirs;
}

bool isSelfOrGameCore(HMODULE module) {
  if (module == g_selfModule) {
    return true;
  }
  return module == gameCore().module;
}

// Narrow (ASCII) view of a wide path, used for diagnostic/logical plugin names.
std::string narrowPath(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
  return out;
}

bool endsWithDll(const std::string& name) {
  if (name.size() < 4) {
    return false;
  }
  const std::string tail = name.substr(name.size() - 4);
  return _stricmp(tail.c_str(), ".dll") == 0;
}

// A loaded candidate plugin DLL plus the manifest data read from it. Modules stay loaded from
// discovery through initialization; failed/skipped candidates are released by the caller.
struct Candidate {
  HMODULE module = nullptr;
  bridge::GetPluginFn getPlugin = nullptr;
  bridge::DestroyPluginFn destroy = nullptr;
  const bridge::PluginManifest* manifest = nullptr;
  std::string name;                  // resolved logical plugin id
  std::vector<std::string> deps;     // resolved dependency names
  bool ok = true;                    // survives dependency/version validation
  std::string reason;                // why it was rejected (diagnostics)
};

// POD-only SEH wrapper: a single plugin callback failure does not take down the game. When the VEH
// sees g_guardedCallActive it passes through to __except and writes no crash log.
void callListenerGuarded(bridge::ContextListenerFn listener, bridge::GameContextEvent event,
                         void* context) {
  g_guardedCallActive = true;
  __try {
    listener(event, context);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Ignored: a single plugin callback exception must not interrupt other plugins or the engine.
  }
  g_guardedCallActive = false;
}

// Load one plugin DLL with its own directory on the search path, so the consumer can resolve the
// API DLLs it imports from the same directory. Falls back to AddDllDirectory for systems where the
// LOAD_LIBRARY_SEARCH_* flags are unavailable.
HMODULE loadPluginModule(const std::wstring& file, const std::wstring& directory) {
  HMODULE module = LoadLibraryExW(file.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (module != nullptr) {
    return module;
  }
  const DWORD firstError = GetLastError();
  DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(directory.c_str());
  module = LoadLibraryW(file.c_str());
  const DWORD secondError = GetLastError();
  if (cookie != nullptr) {
    RemoveDllDirectory(cookie);
  }
  if (module == nullptr) {
    wchar_t detail[512] = {};
    _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                 L"Plugin LoadLibrary failed (GetLastError=%lu/%lu): %s", firstError, secondError,
                 file.c_str());
    logError(detail);
  }
  return module;
}

// Phase 1: enumerate *.dll in one directory, load each candidate, and read its manifest.
void discoverPlugins(const std::wstring& directory, std::vector<Candidate>& out) {
  LogScope scope("scan plugin directory");
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = directory + L"\\*.dll";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    logDebug(L"Plugin directory missing or has no readable DLL: " + directory);
    return;
  }
  logInfo(L"Scanning plugin directory: " + directory);
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    const std::wstring file = directory + L"\\" + data.cFileName;
    logDebug(L"Trying plugin DLL: " + file);
    HMODULE module = loadPluginModule(file, directory);
    if (module == nullptr) {
      continue;
    }
    if (isSelfOrGameCore(module)) {
      FreeLibrary(module); // self or GameCore; skip silently
      continue;
    }

    auto* getPlugin = reinterpret_cast<bridge::GetPluginFn>(
        GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_GETPLUGIN));
    if (getPlugin == nullptr) {
      logWarn(L"Skipped: GetPlugin not exported: " + file);
      FreeLibrary(module);
      continue;
    }
    auto* destroy = reinterpret_cast<bridge::DestroyPluginFn>(
        GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_DESTROY));
    auto* getManifest = reinterpret_cast<bridge::GetPluginManifestFn>(
        GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_GETPLUGINMANIFEST));

    Candidate candidate;
    candidate.module = module;
    candidate.getPlugin = getPlugin;
    candidate.destroy = destroy;
    candidate.manifest = getManifest != nullptr ? getManifest() : nullptr;
    // Prefix safety: only read fields when the plugin declares a struct at least as large as this
    // build's PluginManifest. A smaller manifest is rejected during ordering.
    if (candidate.manifest != nullptr &&
        candidate.manifest->structSize >= sizeof(bridge::PluginManifest) &&
        candidate.manifest->name != nullptr && candidate.manifest->name[0] != '\0') {
      candidate.name = candidate.manifest->name;
    }
    if (candidate.name.empty()) {
      // Compatibility transition: a plugin that does not export a usable manifest keeps loading
      // with no dependencies, identified by its file name.
      const std::string file_name = narrowPath(data.cFileName);
      candidate.name = file_name.empty() ? "unknown-plugin" : file_name;
      if (candidate.manifest == nullptr) {
        logWarn(L"No GetPluginManifest: loaded without dependency ordering: " + file);
      } else {
        logWarn(L"Plugin manifest has no name; using the file name: " + file);
      }
    }
    logDebugF("plugin: LoadLibrary -> %p name=%s GetPlugin=%p DestroyPlugin=%p", module,
              candidate.name.c_str(), reinterpret_cast<void*>(getPlugin),
              reinterpret_cast<void*>(destroy));
    out.push_back(std::move(candidate));
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
}

// Phase 2: validate manifests and compute the dependency-topological initialization order.
// Rejected candidates keep ok=false; the caller releases their modules. `satisfied` holds plugin
// names already loaded by an earlier phase (phase A); such names satisfy dependencies and must not be
// duplicated by a new candidate.
void orderCandidates(std::vector<Candidate>& candidates, std::vector<std::size_t>& order,
                     const std::unordered_set<std::string>& satisfied) {
  LogScope scope("resolve plugin order");

  // Name uniqueness: a duplicated logical name invalidates every candidate that uses it. A name that
  // matches an already-loaded plugin is a duplicate too and is rejected.
  std::unordered_map<std::string, std::size_t> byName;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    Candidate& candidate = candidates[i];
    if (!candidate.ok) {
      continue;
    }
    if (satisfied.count(candidate.name) != 0) {
      candidate.ok = false;
      candidate.reason = "duplicate plugin name (already loaded)";
      logErrorF("plugin: '%s' duplicates an already-loaded plugin; rejected",
                candidate.name.c_str());
      continue;
    }
    const auto it = byName.find(candidate.name);
    if (it == byName.end()) {
      byName.emplace(candidate.name, i);
    } else {
      candidate.ok = false;
      candidate.reason = "duplicate plugin name";
      Candidate& first = candidates[it->second];
      if (first.ok) {
        first.ok = false;
        first.reason = "duplicate plugin name";
      }
      logErrorF("plugin: duplicate name '%s'; both candidates rejected", candidate.name.c_str());
    }
  }

  // Manifest field validation (prefix safety) plus dependency extraction.
  for (Candidate& candidate : candidates) {
    if (!candidate.ok) {
      continue;
    }
    const bridge::PluginManifest* manifest = candidate.manifest;
    if (manifest != nullptr) {
      if (manifest->structSize < sizeof(bridge::PluginManifest)) {
        candidate.ok = false;
        candidate.reason = "manifest structSize too small";
      } else if (manifest->requiredHostApi > bridge::kHostApiVersion) {
        candidate.ok = false;
        candidate.reason = "requiredHostApi is newer than the host";
      }
    }
    if (!candidate.ok) {
      logErrorF("plugin: rejected '%s': %s", candidate.name.c_str(), candidate.reason.c_str());
      continue;
    }
    if (manifest != nullptr && manifest->dependencies != nullptr) {
      candidate.deps.reserve(manifest->dependencyCount);
      for (std::uint32_t d = 0; d < manifest->dependencyCount; ++d) {
        const char* dep = manifest->dependencies[d];
        if (dep == nullptr || dep[0] == '\0') {
          candidate.ok = false;
          candidate.reason = "manifest has an empty dependency name";
          break;
        }
        candidate.deps.emplace_back(dep);
      }
    }
    if (!candidate.ok) {
      logErrorF("plugin: rejected '%s': %s", candidate.name.c_str(), candidate.reason.c_str());
    }
  }

  // Missing dependencies propagate transitively to dependents.
  bool changed = true;
  while (changed) {
    changed = false;
    for (Candidate& candidate : candidates) {
      if (!candidate.ok) {
        continue;
      }
      for (const std::string& dep : candidate.deps) {
        if (satisfied.count(dep) != 0) {
          continue; // provided by an already-loaded plugin
        }
        const auto it = byName.find(dep);
        if (it == byName.end() || !candidates[it->second].ok) {
          candidate.ok = false;
          candidate.reason = "missing dependency: " + dep;
          changed = true;
          break;
        }
      }
      if (!candidate.ok) {
        logErrorF("plugin: rejected '%s': %s", candidate.name.c_str(), candidate.reason.c_str());
      }
    }
  }

  // Kahn topological sort over the surviving candidates. Any node left unprocessed takes part in
  // (or depends on) a dependency cycle and is rejected.
  const std::size_t count = candidates.size();
  std::vector<std::size_t> indegree(count, 0);
  std::vector<std::vector<std::size_t>> dependents(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (!candidates[i].ok) {
      continue;
    }
    for (const std::string& dep : candidates[i].deps) {
      if (satisfied.count(dep) != 0) {
        continue; // an already-loaded plugin provides this dependency: no graph edge
      }
      const std::size_t j = byName[dep];
      if (j == i) {
        ++indegree[i]; // self-dependency: unsatisfiable, detected as a cycle below
        continue;
      }
      dependents[j].push_back(i);
      ++indegree[i];
    }
  }

  std::vector<std::size_t> queue;
  for (std::size_t i = 0; i < count; ++i) {
    if (candidates[i].ok && indegree[i] == 0) {
      queue.push_back(i);
    }
  }
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const std::size_t node = queue[head];
    order.push_back(node);
    for (const std::size_t next : dependents[node]) {
      if (indegree[next] > 0 && --indegree[next] == 0) {
        queue.push_back(next);
      }
    }
  }

  if (order.size() != static_cast<std::size_t>(
                           std::count_if(candidates.begin(), candidates.end(),
                                         [](const Candidate& c) { return c.ok; }))) {
    for (std::size_t i = 0; i < count; ++i) {
      if (candidates[i].ok && indegree[i] != 0) {
        candidates[i].ok = false;
        candidates[i].reason = "dependency cycle";
        logErrorF("plugin: rejected '%s': dependency cycle", candidates[i].name.c_str());
      }
    }
  }

  for (const std::size_t index : order) {
    logInfoF("plugin: init order -> %s", candidates[index].name.c_str());
  }
}

// Phase 3: allocate a slot per ordered candidate and call GetPlugin. A failed dependency causes its
// dependents to be skipped rather than initialized against an absent service.
void initializeCandidates(std::vector<Candidate>& candidates,
                          const std::vector<std::size_t>& order,
                          const bridge::Host& hostTemplate) {
  LogScope scope("initialize plugins");
  std::unordered_set<std::string> failed;
  for (const std::size_t index : order) {
    Candidate& candidate = candidates[index];
    bool dependency_failed = false;
    for (const std::string& dep : candidate.deps) {
      if (failed.count(dep) != 0) {
        dependency_failed = true;
        break;
      }
    }
    if (dependency_failed) {
      logErrorF("plugin: skipped '%s' because a dependency failed to initialize",
                candidate.name.c_str());
      failed.insert(candidate.name);
      continue;
    }

    auto* slot = new (std::nothrow) PluginSlot();
    if (slot == nullptr) {
      logError("plugin: failed to allocate plugin slot");
      failed.insert(candidate.name);
      continue;
    }
    slot->module = candidate.module;
    slot->destroy = candidate.destroy;
    slot->name = candidate.name;
    slot->host = hostTemplate;
    slot->host.pluginHandle = slot; // per-plugin ownership handle

    setActivePluginHandle(slot);
    const int result = candidate.getPlugin(&slot->host);
    setActivePluginHandle(nullptr);
    logInfoF("plugin: GetPlugin(host) -> %d (%s)", result, candidate.name.c_str());
    if (result <= 0) {
      // GetPlugin failed: the plugin may already have registered hooks/slots/implementation
      // callbacks, so the Loader must first restore as a fallback (revoke the still-registered
      // plugin-owned hooks and effect slots, clear implementation callbacks), and only then let the
      // plugin clean up and unload; otherwise hooks/slots pointing at plugin code become dangling
      // after FreeLibrary.
      removeHooksForPlugin(slot);
      teardownPluginEffects(slot);
      if (candidate.destroy != nullptr) {
        candidate.destroy();
      }
      FreeLibrary(candidate.module);
      candidate.module = nullptr;
      delete slot;
      failed.insert(candidate.name);
      logErrorF("plugin: initialization failed (GetPlugin returned %d): %s", result,
                candidate.name.c_str());
      continue;
    }

    slot->listener = slot->host.onGameContext; // the plugin registers this inside GetPlugin
    candidate.module = nullptr;                // ownership transferred to the slot
    g_plugins.push_back(slot);
    logInfoF("plugin: loaded '%s'", candidate.name.c_str());
  }
}

} // namespace

void loadPlugins(bridge::Host* host) {
  if (host == nullptr || g_pluginsLoaded) {
    return;
  }
  LogScope scope("load plugins (core)");
  g_pluginsLoaded = true;
  g_hostTemplate = host; // stashed for the deferred cross-mod phase

  std::vector<Candidate> candidates;
  std::vector<std::wstring> directories = loaderOwnPluginDirs();
  // Mods.sqlite selected-group enabled set (seeded by the proxy before the engine published its own
  // settings): when present, the other enabled mods' plugin directories are discovered and loaded
  // in this same phase, so their plugins register effect types before the real
  // DllCreateGameContext builds and validates the effect database.
  if (!g_groupEnabledModIds.empty()) {
    std::unordered_set<std::string> enabled;
    enabled.reserve(g_groupEnabledModIds.size());
    for (const std::string& id : g_groupEnabledModIds) {
      enabled.insert(toLowerAscii(id));
    }
    const std::vector<std::wstring> crossDirs = crossModPluginDirs(enabled);
    for (const std::wstring& directory : crossDirs) {
      addUnique(directories, directory);
    }
    // Seeded .modinfo paths reach enabled mods outside the Documents Mods and Workshop roots;
    // probing a mod root without a plugin directory is a cheap no-op.
    for (const std::wstring& directory : seededModPluginDirs(enabled, g_groupEnabledModInfoPaths)) {
      addUnique(directories, directory);
    }
    // The seeded set is fixed for the process lifetime unless a later Mods.sqlite re-read replaces
    // it: the deferred phase has nothing left to load, so mark it complete. Its triggers then stay
    // no-ops (already-loaded plugins would otherwise be rejected as duplicates on a re-scan).
    g_crossModLoaded = true;
    logInfoF("cross-mod: Mods.sqlite group enabled-mod set (%zu ids); loading now", enabled.size());
  }
  // Dependency search path: the load directories plus every mod's plugin directory (enabled or
  // not). Registering them lets a consumer in one directory import the API DLLs it declares as
  // dependencies from another mod's directory; the discovery loop below still runs only over the
  // load directories, so a disabled mod's plugin DLLs are resolvable but never loaded.
  std::vector<std::wstring> searchDirectories = directories;
  for (const std::wstring& directory : allModPluginDirs()) {
    addUnique(searchDirectories, directory);
  }
  addDllSearchDirectories(searchDirectories);
  logInfo(L"Plugin search directory count: " + std::to_wstring(searchDirectories.size()));
#if defined(_DEBUG)
  for (const std::wstring& directory : searchDirectories) {
    logDebug(L"Plugin search path: " + directory);
  }
#endif
  for (const std::wstring& directory : directories) {
    discoverPlugins(directory, candidates);
  }

  std::vector<std::size_t> order;
  const std::unordered_set<std::string> satisfied;
  orderCandidates(candidates, order, satisfied);

  // Release rejected candidates (invalid manifest, duplicate name, missing dependency, cycle).
  for (Candidate& candidate : candidates) {
    if (!candidate.ok && candidate.module != nullptr) {
      FreeLibrary(candidate.module);
      candidate.module = nullptr;
    }
  }

  initializeCandidates(candidates, order, *host);

  // Release any module not claimed by a slot (skipped candidates and failures).
  for (Candidate& candidate : candidates) {
    if (candidate.module != nullptr) {
      FreeLibrary(candidate.module);
      candidate.module = nullptr;
    }
  }

  logInfo(L"Plugin loading finished; loaded count: " + std::to_wstring(g_plugins.size()));
}

void setGroupEnabledModIds(std::vector<std::string> ids) {
  g_groupEnabledModIds = std::move(ids);
}

void setGroupEnabledModInfoPaths(
    std::vector<std::pair<std::string, std::wstring>> idToModinfoPath) {
  g_groupEnabledModInfoPaths = std::move(idToModinfoPath);
}

bool loadCrossModPluginsDeferred() {
  if (g_crossModLoaded) {
    return true;
  }
  if (g_hostTemplate == nullptr) {
    return false; // phase A has not run yet
  }

  // The Mods.sqlite selected group is the only enabled-mod source: without a seeded set there is
  // nothing to scan, so the phase stays pending and a later Mods.sqlite re-read can still seed it.
  if (g_groupEnabledModIds.empty()) {
    logDebug("cross-mod: the Mods.sqlite selected group has not been seeded; deferring");
    return false;
  }
  std::unordered_set<std::string> enabled;
  enabled.reserve(g_groupEnabledModIds.size());
  for (const std::string& id : g_groupEnabledModIds) {
    enabled.insert(toLowerAscii(id));
  }
  logInfoF("cross-mod: Mods.sqlite group enabled-mod count: %zu", enabled.size());

  // Phase-A plugins already satisfy dependencies of cross-mod plugins.
  std::unordered_set<std::string> satisfied;
  for (PluginSlot* slot : g_plugins) {
    satisfied.insert(slot->name);
  }

  LogScope scope("load plugins (cross-mod)");
  std::vector<std::wstring> dirs = crossModPluginDirs(enabled);
  for (const std::wstring& dir : seededModPluginDirs(enabled, g_groupEnabledModInfoPaths)) {
    addUnique(dirs, dir);
  }
  std::vector<Candidate> candidates;
  // Same split as phase A: the load dirs decide what is scanned; every mod's plugin directory is
  // additionally registered for DLL dependency resolution.
  std::vector<std::wstring> searchDirs = dirs;
  for (const std::wstring& dir : allModPluginDirs()) {
    addUnique(searchDirs, dir);
  }
  addDllSearchDirectories(searchDirs);
  logInfo(L"cross-mod plugin search directory count: " + std::to_wstring(searchDirs.size()));
  for (const std::wstring& d : dirs) {
    discoverPlugins(d, candidates);
  }

  std::vector<std::size_t> order;
  orderCandidates(candidates, order, satisfied);

  // Release rejected candidates (invalid manifest, duplicate name, missing dependency, cycle).
  for (Candidate& c : candidates) {
    if (!c.ok && c.module != nullptr) {
      FreeLibrary(c.module);
      c.module = nullptr;
    }
  }

  const std::size_t before = g_plugins.size();
  initializeCandidates(candidates, order, *g_hostTemplate);

  // Release any module not claimed by a slot (skipped candidates and failures).
  for (Candidate& c : candidates) {
    if (c.module != nullptr) {
      FreeLibrary(c.module);
      c.module = nullptr;
    }
  }

  // Newly loaded plugins missed the kCreated broadcast (this runs after createGameContext): notify
  // only the slots added in this call, never the already-loaded ones.
  for (std::size_t i = before; i < g_plugins.size(); ++i) {
    if (g_plugins[i]->listener != nullptr) {
      callListenerGuarded(g_plugins[i]->listener, bridge::GameContextEvent::kCreated, nullptr);
    }
  }

  g_crossModLoaded = true;
  logInfo(L"cross-mod: plugin loading finished; total loaded=" +
          std::to_wstring(g_plugins.size()));
  return true;
}

void setActivePluginHandle(void* pluginHandle) {
  std::lock_guard<std::mutex> guard(g_activeMutex);
  g_activePluginHandle = pluginHandle;
}

void* activePluginHandle() {
  std::lock_guard<std::mutex> guard(g_activeMutex);
  return g_activePluginHandle;
}

void notifyPluginsGameContext(bridge::GameContextEvent event, void* context) {
  for (PluginSlot* slot : g_plugins) {
    if (slot->listener == nullptr) {
      continue;
    }
    callListenerGuarded(slot->listener, event, context);
  }
}

void unloadPlugins() {
  LogScope scope("unload plugins");
  // Unload in reverse order: opposite to the topological initialization order, so a consumer is torn
  // down before the API plugins it depends on.
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    PluginSlot* slot = *it;
    if (slot == nullptr) {
      continue;
    }
    // Let the plugin clean up first (revoke hooks, clear caches), then do the Loader fallback.
    if (slot->listener != nullptr) {
      callListenerGuarded(slot->listener, bridge::GameContextEvent::kDestroyed, nullptr);
    }
    // Fallback: remove/restore any hooks and vtable slots still pointing at this plugin's code
    // before unloading is allowed.
    removeHooksForPlugin(slot);
    teardownPluginEffects(slot);
    if (slot->destroy != nullptr) {
      slot->destroy();
    }
    if (slot->module != nullptr) {
      FreeLibrary(slot->module);
    }
    delete slot;
  }
  g_plugins.clear();
  g_pluginsLoaded = false;
  g_crossModLoaded = false;
  removeDllSearchDirectories();
}

} // namespace ykkz000::loader
