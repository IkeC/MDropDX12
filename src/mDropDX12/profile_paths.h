// profile_paths.h — the one place that knows where a profile lives.
//
//     resources/profiles/<type>/<name>.json
//
// Four kinds of profile grew up separately and stored themselves four
// different ways: display profiles as timestamped files in
// resources/displayprofiles/, VFX and audio profiles as named entries inside a
// single resources/<kind>profiles.json, and the mixer had none at all. Adding a
// fifth store shaped like none of the others is how that becomes permanent, so
// the layout is stated once here and every store resolves through it.
//
// FILE PER PROFILE, not one file holding many. The deciding argument is this
// repository's own history: these files live in a separate config repo because
// the data has been destroyed twice. A single-file store rewrites every profile
// on every save, so one bad write costs the whole set; per-file it costs one.
// Per-file also gives a real git history per profile instead of one churning
// blob, makes a profile something you can copy or send on its own, and is the
// only shape in which a timestamped name means anything.
//
// The FILENAME and the profile's NAME are different things. The name a person
// typed can contain characters no filesystem will take, and renaming a profile
// must not have to move a file. So the filename is a sanitised, unique leaf and
// the display name is stored INSIDE the json as "name"; a listing shows the
// name from the file and falls back to the leaf when a file has none.
//
// The string half is header-only and dependency-free so the native suite can
// drive it -- same reasoning as order_group.h. The handful of functions that
// touch the disk are declared at the bottom and defined in profile_paths.cpp,
// which is the only part that needs Windows.
#pragma once

#include <cwchar>
#include <string>
#include <vector>

#include "format_to.h"

namespace mdrop {
namespace profiles {

// One directory per kind, under resources/profiles/.
enum class Kind { Display, Vfx, Audio, Mixer };

inline const wchar_t* KindDir(Kind k) {
  switch (k) {
    case Kind::Display: return L"display";
    case Kind::Vfx:     return L"vfx";
    case Kind::Audio:   return L"audio";
    case Kind::Mixer:   return L"mixer";
  }
  return L"";
}

// Where the old files sat, so a migration can find them without four separate
// tables of legacy paths. Empty for the mixer, which never had a store.
inline const wchar_t* LegacyLeaf(Kind k) {
  switch (k) {
    case Kind::Display: return L"displayprofiles";       // a directory
    case Kind::Vfx:     return L"vfxprofiles.json";      // one file, many
    case Kind::Audio:   return L"audioprofiles.json";    // one file, many
    case Kind::Mixer:   return L"";
  }
  return L"";
}

inline std::wstring WithSlash(const std::wstring& dir) {
  if (dir.empty()) return dir;
  if (dir.back() == L'\\' || dir.back() == L'/') return dir;
  return dir + L'\\';
}

// <base>/resources/profiles/ and <base>/resources/profiles/<type>/
inline std::wstring Root(const std::wstring& baseDir) {
  return WithSlash(baseDir) + L"resources\\profiles\\";
}
inline std::wstring DirFor(const std::wstring& baseDir, Kind k) {
  return Root(baseDir) + KindDir(k) + L"\\";
}

// The same directory, reached from the RESOURCES dir instead of the base.
//
// The profile stores are handed <base>esources\ and never see the base --
// one call site sets them all, and it passes m_szMilkdrop2Path. Rebuilding the
// base by chopping "resources" off the end would be a second, worse way of
// knowing the same thing.
inline std::wstring DirForRes(const std::wstring& resourceDir, Kind k) {
  return WithSlash(resourceDir) + L"profiles\\" + KindDir(k) + L"\\";
}

// The single-file store each kind used to keep, reached from the resources dir.
inline std::wstring LegacyPathRes(const std::wstring& resourceDir, Kind k) {
  const wchar_t* leaf = LegacyLeaf(k);
  if (!*leaf) return std::wstring();
  return WithSlash(resourceDir) + leaf;
}
inline std::wstring LegacyPath(const std::wstring& baseDir, Kind k) {
  const wchar_t* leaf = LegacyLeaf(k);
  if (!*leaf) return std::wstring();
  return WithSlash(baseDir) + L"resources\\" + leaf;
}

// A leaf that a filesystem will accept, carrying no directory component.
//
// Everything Windows forbids becomes '-', runs collapse, and the result is
// trimmed of leading and trailing dots and spaces -- a trailing dot is silently
// dropped by the API, which would make two profiles collide on one file.
// Reserved device names (CON, PRN, COM1 ...) are suffixed rather than rejected,
// since a person may reasonably call a profile "AUX".
inline std::wstring SanitizeFilename(const std::wstring& name) {
  static const wchar_t* kBad = L"<>:\"/\\|?*";
  std::wstring out;
  out.reserve(name.size());
  for (wchar_t c : name) {
    const bool bad = c < 32 || wcschr(kBad, c) != nullptr;
    const wchar_t ch = bad ? L'-' : c;
    if (ch == L'-' && !out.empty() && out.back() == L'-') continue;
    out += ch;
  }
  while (!out.empty() && (out.back() == L'.' || out.back() == L' ')) out.pop_back();
  size_t start = 0;
  while (start < out.size() && (out[start] == L'.' || out[start] == L' ')) start++;
  out = out.substr(start);
  if (out.empty()) out = L"profile";

  static const wchar_t* kReserved[] = {
      L"CON", L"PRN", L"AUX", L"NUL",
      L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
      L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
  for (const wchar_t* r : kReserved) {
    if (_wcsicmp(out.c_str(), r) == 0) { out += L"_"; break; }
  }
  if (out.size() > 120) out.resize(120);
  return out;
}

// "<leaf>.json", adding the extension only when it is not already there.
inline std::wstring WithJson(const std::wstring& leaf) {
  if (leaf.size() >= 5 && _wcsicmp(leaf.c_str() + leaf.size() - 5, L".json") == 0)
    return leaf;
  return leaf + L".json";
}

// Anything that looks like a path is one, and is returned untouched: profiles
// saved before this layout existed are absolute, and they must keep loading.
inline bool LooksLikePath(const std::wstring& s) {
  return s.find(L'\\') != std::wstring::npos ||
         s.find(L'/') != std::wstring::npos ||
         (s.size() > 1 && s[1] == L':');
}

inline std::wstring PathFor(const std::wstring& baseDir, Kind k,
                            const std::wstring& nameOrPath) {
  if (nameOrPath.empty()) return std::wstring();
  if (LooksLikePath(nameOrPath)) return nameOrPath;
  return DirFor(baseDir, k) + WithJson(nameOrPath);
}

// yyyy-MM-dd_HH-mm-ss, from caller-supplied parts so the string half stays
// testable. Sorting these by name sorts them by age, which is what makes a
// listing of timestamped profiles read in a sensible order with no date parsing.
inline std::wstring TimestampName(int y, int mo, int d, int h, int mi, int s) {
  wchar_t buf[64];
  FormatTo(buf, L"%04d-%02d-%02d_%02d-%02d-%02d", y, mo, d, h, mi, s);
  return buf;
}

// ── The half that touches the disk (profile_paths.cpp) ──────────────

// These take the profile DIRECTORY, from DirFor or DirForRes. Splitting the
// path building from the disk work is what lets a store reached by resources
// dir and an engine reached by base dir share one implementation.

// Creates the directory and every level above it. True if it is there after.
bool EnsureDirPath(const std::wstring& dir);

// The *.json leaves in `dir`, sorted by name -- which for a timestamped
// profile is by age.
std::vector<std::wstring> ListDir(const std::wstring& dir);

// Whether that leaf is already a file in `dir`.
bool ExistsIn(const std::wstring& dir, const std::wstring& leaf);

// `desired` sanitised, then suffixed -2, -3 ... until it names no existing
// file. Saving twice under one name must not silently overwrite.
std::wstring UniqueLeafIn(const std::wstring& dir, const std::wstring& desired);

// TimestampName() for the local clock now.
std::wstring TimestampNow();

}  // namespace profiles
}  // namespace mdrop
