#include "wasabi.h"
#include <Windows.h>

extern HINSTANCE api_orig_hinstance;

static wchar_t buffer[4096];

wchar_t* wasabiApiLangString(int id, wchar_t* dest, int len) {
  LoadStringW(api_orig_hinstance, id, dest, len);
  return (wchar_t*)dest;
}

wchar_t* wasabiApiLangString(int id) {
  LoadStringW(api_orig_hinstance, id, buffer, 4096);
  return (wchar_t*)buffer;
}

HWND wasabiApiCreateDialogParam(int templateName, HWND parent, DLGPROC proc, LPARAM initParam) {
  return CreateDialogParamW(api_orig_hinstance, wasabiApiLangString(templateName), parent, proc, initParam);
}

HMENU wasabiApiLoadMenu(int menuId) {
  return LoadMenuW(api_orig_hinstance, MAKEINTRESOURCEW(menuId));
}