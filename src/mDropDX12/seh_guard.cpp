// seh_guard.cpp — the shared SEH crash report. See seh_guard.h for why a
// window procedure needs one at all.
//
// Lifted out of engine_presets.cpp, which had the only copy and used it for
// preset loading alone. It is the same report either way -- registers and a
// stack walk are not preset-specific -- and the EEL-specific half stayed
// behind, because g_eelCompileCtx is that module's business.
#include "seh_guard.h"

#include "utility.h"

#include <dbghelp.h>
#include <stdio.h>

namespace mdrop {

namespace {
// Reports written so far. Not atomic: it is only read and written from inside a
// crash filter, and two threads faulting in the same instant is not a case
// worth a lock -- the worst outcome is one report more or fewer than the cap.
int g_reports = 0;

// Beyond this, count but do not write.
//
// A window procedure that faults on every WM_PAINT would otherwise produce a
// stack walk per frame. The number is small on purpose: if the first twenty do
// not say what is wrong, the twenty-first will not either.
const int kMaxReports = 20;
}  // namespace

int SehCrashCount() { return g_reports; }

LONG SehCrashFilter(EXCEPTION_POINTERS* ep, const wchar_t* context) {
  if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
    return EXCEPTION_EXECUTE_HANDLER;

  g_reports++;
  if (g_reports > kMaxReports) {
    // Still say something, once per fault, at a cost of one line.
    DLOG_ERROR("SEH fault #%d (report suppressed past %d)", g_reports,
               kMaxReports);
    return EXCEPTION_EXECUTE_HANDLER;
  }

  FILE* f = DebugLogDiagOpen(L"diag_seh_crash.txt", L"a");   // accumulates
  if (!f) return EXCEPTION_EXECUTE_HANDLER;

  EXCEPTION_RECORD* er = ep->ExceptionRecord;
  CONTEXT* ctx = ep->ContextRecord;

  SYSTEMTIME st;
  GetLocalTime(&st);
  fwprintf(f, L"\n========== SEH CRASH %04d-%02d-%02d %02d:%02d:%02d ==========\n",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  fwprintf(f, L"Context: %s\n", context ? context : L"<unknown>");
  fwprintf(f, L"Exception Code:    0x%08X\n", er->ExceptionCode);
  fwprintf(f, L"Exception Flags:   0x%08X\n", er->ExceptionFlags);
  fwprintf(f, L"Exception Address: 0x%016llX\n", (DWORD64)er->ExceptionAddress);

  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      er->NumberParameters >= 2) {
    const wchar_t* op = er->ExceptionInformation[0] == 0 ? L"READ"
                      : er->ExceptionInformation[0] == 1 ? L"WRITE"
                                                         : L"DEP";
    fwprintf(f, L"Access Type:       %s\n", op);
    fwprintf(f, L"Target Address:    0x%016llX\n",
             (DWORD64)er->ExceptionInformation[1]);
  }

  fwprintf(f, L"\nRegisters:\n");
  fwprintf(f, L"  RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX\n",
           ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
  fwprintf(f, L"  RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX\n",
           ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
  fwprintf(f, L"  R8 =%016llX  R9 =%016llX  R10=%016llX  R11=%016llX\n",
           ctx->R8, ctx->R9, ctx->R10, ctx->R11);
  fwprintf(f, L"  R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX\n",
           ctx->R12, ctx->R13, ctx->R14, ctx->R15);
  fwprintf(f, L"  RIP=%016llX  EFLAGS=%08X\n", ctx->Rip, ctx->EFlags);

  fwprintf(f, L"\nStack Trace:\n");
  HANDLE process = GetCurrentProcess();
  SymInitialize(process, NULL, TRUE);

  CONTEXT ctxCopy = *ctx;      // StackWalk64 may modify the context
  STACKFRAME64 sf = {};
  sf.AddrPC.Offset    = ctx->Rip;  sf.AddrPC.Mode    = AddrModeFlat;
  sf.AddrFrame.Offset = ctx->Rbp;  sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = ctx->Rsp;  sf.AddrStack.Mode = AddrModeFlat;

  char symBuf[sizeof(SYMBOL_INFO) + 256];
  SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen   = 255;

  for (int i = 0; i < 32; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
                     &sf, &ctxCopy, NULL, SymFunctionTableAccess64,
                     SymGetModuleBase64, NULL))
      break;
    const DWORD64 addr = sf.AddrPC.Offset;
    if (addr == 0) break;

    HMODULE hMod = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)addr, &hMod);
    wchar_t modName[MAX_PATH] = L"<jit>";
    if (hMod) {
      GetModuleFileNameW(hMod, modName, MAX_PATH);
      wchar_t* slash = wcsrchr(modName, L'\\');
      if (slash) wmemmove(modName, slash + 1, wcslen(slash + 1) + 1);
    }

    DWORD64 displacement = 0;
    if (SymFromAddr(process, addr, &displacement, sym))
      fwprintf(f, L"  [%2d] 0x%016llX  %s!%hs +0x%llX\n", i, addr, modName,
               sym->Name, displacement);
    else
      fwprintf(f, L"  [%2d] 0x%016llX  %s+0x%llX\n", i, addr, modName,
               hMod ? (addr - (DWORD64)hMod) : addr);
  }

  SymCleanup(process);
  fwprintf(f, L"\n");
  fclose(f);

  DLOG_ERROR("SEH crash diagnostics written to diag_seh_crash.txt");
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace mdrop
