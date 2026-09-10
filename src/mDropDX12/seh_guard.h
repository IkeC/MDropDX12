// seh_guard.h — catching a fault inside a window procedure, where nothing else
// can.
//
// A window procedure runs as a kernel-to-user callback. Windows treats an
// exception escaping one as FATAL AND NON-CONTINUABLE: the process is
// terminated with STATUS_FATAL_USER_CALLBACK_EXCEPTION (0xC000041D) without
// taking the ordinary unhandled-exception route. So:
//
//   * SetUnhandledExceptionFilter is never called for it;
//   * Windows Error Reporting never sees a fault, and writes no minidump;
//   * nothing reaches the Application event log;
//   * debug.log simply stops mid-line.
//
// Measured, not assumed -- the process exits 0xC000041D and
// %LOCALAPPDATA%\CrashDumps gains nothing, on a machine where WER demonstrably
// does collect dumps for this image. See forgejo#75.
//
// That is the worst shape a failure can take: a GUI app does a great deal of
// work inside window procedures -- every tool window's DoCommand, DoNotify and
// paint path is one -- so a bug in any of them makes the app vanish with no
// artefact at all. The standing rule is that the visualizer should never crash,
// and it cannot handle what it is never told about.
//
// The ONLY place such a fault can still be caught is inside the callback,
// before it unwinds back into the kernel. Hence a thin __try/__except wrapper
// around each window procedure, using SehCrashFilter below as its filter.
#pragma once

#include <windows.h>

namespace mdrop {

// An __except FILTER. Writes registers and a stack walk to
// log/diag_seh_crash.txt, notes the crash in debug.log, and returns
// EXCEPTION_EXECUTE_HANDLER so the guarded handler runs.
//
// Must be called from the filter expression, because GetExceptionInformation()
// is only valid there.
//
// `context` says WHERE, and is written into the report -- "ToolWindow" and the
// window's title, say. A crash report that cannot tell you which of thirty tool
// windows faulted is most of a report short of useful.
LONG SehCrashFilter(EXCEPTION_POINTERS* ep, const wchar_t* context);

// How many faults this filter has reported since the process started.
//
// The guarded handlers keep going rather than exiting, so a window whose paint
// path faults every frame would otherwise write a stack walk every frame and
// fill the disk. After a cap the report is skipped and only the count is kept,
// which stays honest without being ruinous.
int SehCrashCount();

}  // namespace mdrop
