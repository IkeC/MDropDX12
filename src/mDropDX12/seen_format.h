// seen_format.h — "last seen" as a person reads it.
//
// Header-only and dependency-free on purpose: engine_mixer_ui.cpp cannot be
// compiled on its own without dragging in the whole tool-window layer, and a
// date helper with two boundary cases in it is exactly the thing that should
// be tested rather than eyeballed. The native suite includes this directly.
#pragma once

#include <windows.h>

#include <string>

#include "format_to.h"

namespace mdrop {

// "2026-08-31 13:04" as "Today 13:04", and yesterday's as "Ystrdy 13:04".
//
// Shane asked for this on the failover allowlist: a date is the wrong unit for
// something seen an hour ago, and the two most recent are the two anyone is
// actually choosing between.
//
// The TIME is kept, never just the day. Three of his four Sony pairings were
// last seen on the same date and are told apart only by the hour, so "Today"
// alone would merge exactly the rows this column exists to separate.
//
// Display only. The value on the wire stays the absolute, sortable
// "YYYY-MM-DD HH:MM" that FormatLastSeen produces, because the device dropdown
// orders by string comparison on it (`a.seen > b.seen`) -- rewriting the
// stored form would put Today below Ystrdy and both above every real date. A
// client rendering this list can make the same substitution itself, and in its
// own timezone, which it could not do from a pre-formatted label.
inline std::wstring FriendlySeen(const std::wstring& seen) {
  if (seen.empty() || seen == L"-") return L"unknown";
  if (seen.size() < 10) return seen;

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  wchar_t today[16], yesterday[16];
  FormatTo(today, L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);

  // Yesterday via FILETIME arithmetic rather than by decrementing the day,
  // which would have to know about month lengths and leap years.
  FILETIME ft = {};
  SystemTimeToFileTime(&now, &ft);
  ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  t -= 24ull * 60ull * 60ull * 10000000ull;
  ft.dwHighDateTime = (DWORD)(t >> 32);
  ft.dwLowDateTime  = (DWORD)(t & 0xFFFFFFFF);
  SYSTEMTIME prev = {};
  if (!FileTimeToSystemTime(&ft, &prev)) return seen;
  FormatTo(yesterday, L"%04d-%02d-%02d", prev.wYear, prev.wMonth, prev.wDay);

  const std::wstring day = seen.substr(0, 10);
  const std::wstring rest = seen.substr(10);   // " HH:MM", space included
  if (day == today)     return L"Today" + rest;
  if (day == yesterday) return L"Ystrdy" + rest;

  // A sighting from THIS year drops the year: "08-23 21:03".
  //
  // Four digits and a dash on every row, all saying the same thing, in a
  // column already competing for width with the device name -- and the year is
  // only ever news when it is not the current one. Shane asked for it directly.
  // An older sighting keeps its year, which is the case where the year is the
  // entire point: two identically-named pairings a year apart.
  //
  // The displayed form still sorts correctly by string within the current
  // year, and the STORED value is untouched either way -- the device dropdown
  // orders on that, not on this. Same rule the Today/Ystrdy substitution
  // above already follows.
  if (seen.compare(0, 4, today, 4) == 0) return seen.substr(5);
  return seen;
}

}  // namespace mdrop
