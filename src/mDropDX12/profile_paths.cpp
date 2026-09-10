// profile_paths.cpp — the half of the profile layout that touches the disk.
//
// Directory-centric on purpose: these take the profile directory that DirFor
// or DirForRes built, so a store reached by its resources dir and an engine
// reached by its base dir share one implementation instead of two.
//
// The string logic lives in the header, where the native suite can reach it
// without a filesystem.

#include "profile_paths.h"

#include <windows.h>

#include <algorithm>

#include "format_to.h"

namespace mdrop {
namespace profiles {

bool EnsureDirPath(const std::wstring& dir) {
  // Every level, one at a time. CreateDirectory does not build a chain, and
  // resources\ commonly exists while profiles\ does not -- the display profile
  // snapshot learned this the same way.
  if (dir.empty()) return false;
  std::wstring built;
  for (size_t i = 0; i < dir.size(); i++) {
    built += dir[i];
    if (dir[i] != L'\\' && dir[i] != L'/') continue;
    // Skip a drive root ("C:\") and the leading slashes of a UNC path, which
    // CreateDirectory refuses and which are never ours to make.
    if (built.size() <= 3) continue;
    CreateDirectoryW(built.c_str(), NULL);
  }
  CreateDirectoryW(dir.c_str(), NULL);

  const DWORD attr = GetFileAttributesW(dir.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::vector<std::wstring> ListDir(const std::wstring& dir) {
  std::vector<std::wstring> out;
  if (dir.empty()) return out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((WithSlash(dir) + L"*.json").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    // A leading underscore is store bookkeeping, not a profile: _store.json
    // carries the flags that used to be top-level members of the single file.
    if (fd.cFileName[0] == L'_') continue;
    out.push_back(fd.cFileName);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  // By name, which for a timestamped profile is by age -- see TimestampName.
  std::sort(out.begin(), out.end(),
            [](const std::wstring& a, const std::wstring& b) {
              return _wcsicmp(a.c_str(), b.c_str()) < 0;
            });
  return out;
}

bool ExistsIn(const std::wstring& dir, const std::wstring& leaf) {
  const std::wstring path = WithSlash(dir) + WithJson(leaf);
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring UniqueLeafIn(const std::wstring& dir, const std::wstring& desired) {
  // Saving a second profile under a name already used must not silently
  // overwrite the first: two profiles a person called the same thing are two
  // profiles, and the display name inside the file keeps them both readable.
  const std::wstring base = SanitizeFilename(desired);
  if (!ExistsIn(dir, base)) return base;
  for (int n = 2; n < 1000; n++) {
    wchar_t suffix[16];
    FormatTo(suffix, L"-%d", n);
    const std::wstring candidate = base + suffix;
    if (!ExistsIn(dir, candidate)) return candidate;
  }
  return base;   // a thousand collisions is not a case worth a better answer
}

std::wstring TimestampNow() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  return TimestampName(st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond);
}

}  // namespace profiles
}  // namespace mdrop
