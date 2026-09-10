#pragma once

#include <windows.h>

// Loading a localized string used to be spellable three ways, two of them
// wrong (issue 19).
//
//   wasabiApiLangString(id)                 returned a pointer to ONE shared
//                                           4096-wchar buffer
//   wasabiApiLangString(id, dest, len)      trusted the caller to state len
//
// Both are gone. The shared buffer is gone with them, so a result can no
// longer be clobbered by the next call -- the hazard the issue names, which
// making the buffer thread_local narrowed but did not remove: within one
// thread, holding a result across any call that loads another string still
// silently swapped the text. CMilkMenu::AddItem holds its name across the
// CMilkMenuItem constructor, which loads a string; 120 call sites reach it.
//
// The length is now DEDUCED from the destination array, which is what makes
// the second mistake unspellable rather than merely fixed. LoadStringW counts
// its buffer in CHARACTERS, and ten sites stated bytes -- nine as sizeof() of
// a wchar_t array, and engineshell.cpp as a literal 2048 for a wchar_t[1024]
// holding a string already 861 characters long. Each authorised LoadStringW to
// write twice the array. None of them can be written now: there is no overload
// that accepts a length, and no overload that accepts a bare pointer.
namespace wasabi_detail {
  wchar_t* LangStringInto(int id, wchar_t* dest, int cchDest);
}

// Loads string resource `id` into `dest`, truncating to fit, always
// null-terminated. Returns `dest` so it can be used in an expression.
template <size_t N>
inline wchar_t* wasabiApiLangString(int id, wchar_t (&dest)[N]) {
  static_assert(N > 1, "wasabiApiLangString: destination must hold a character and a terminator");
  static_assert(N <= 0x7fffffff, "wasabiApiLangString: destination exceeds LoadStringW's int count");
  return wasabi_detail::LangStringInto(id, dest, (int)N);
}
