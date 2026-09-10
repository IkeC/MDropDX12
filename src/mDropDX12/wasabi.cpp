#include "wasabi.h"
#include <Windows.h>

extern HINSTANCE api_orig_hinstance;

wchar_t* wasabi_detail::LangStringInto(int id, wchar_t* dest, int cchDest) {
  // LoadStringW returns 0 both for a missing resource and for an empty one,
  // and is not documented to write anything in the first case. The old
  // one-argument form discarded this and handed back the shared buffer, so a
  // bad id rendered the PREVIOUS caller's text; write the terminator instead
  // so a bad id renders nothing.
  if (LoadStringW(api_orig_hinstance, id, dest, cchDest) == 0)
    dest[0] = L'\0';
  return dest;
}
