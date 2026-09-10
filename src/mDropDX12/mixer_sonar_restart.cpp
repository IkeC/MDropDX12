#include "mixer_sonar_restart.h"

#include <windows.h>
#include <tlhelp32.h>

namespace mdrop {

namespace {

// ONLY the Electron GUI, and nothing else.
//
// This used to take the whole SteelSeries tree down and start it again. It
// cannot: SteelSeriesGGEZ.exe, the launcher, REQUIRES ELEVATION to start --
// CreateProcess from this process fails with ERROR_ELEVATION_REQUIRED (740),
// measured. At logon a service starts it with the user's token, which is how it
// comes to be running non-elevated and killable; but once killed, nothing here
// can put it back. ShellExecuteEx can, by elevating, and then GGEZ is running
// ELEVATED and the next reset cannot kill it at all -- access denied. Relaunching
// SteelSeriesGG.exe instead does not rebuild the tree; it starts and exits.
//
// So the old design ended with the suite down and no way to restore it, which
// is exactly what it did: twice, on Shane's machine, until GG was started by
// hand.
//
// What actually wedges is the GUI. Shane: "it's just the steel series windows
// gui layers ... those are what get hung as written in electron", and later,
// after watching a restart: "you didn't affect my sound in a negative way, the
// windows gui for sonar was broken." The seven SteelSeriesGGClient processes
// ARE that Electron GUI. Killing them leaves the launcher, the engine and the
// audio path alone, and SteelSeriesGG.exe -- still running, because it is not
// touched -- brings its renderers back by itself.
//
// Nothing is relaunched here any more. That is the point: there is nothing to
// relaunch, so there is nothing that can fail to relaunch.
const wchar_t* const kOrder[] = {
    L"SteelSeriesGGClient.exe",
};

struct Running {
  std::wstring name;
  DWORD pid = 0;
};

std::vector<Running> SnapshotProcesses() {
  std::vector<Running> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;
  PROCESSENTRY32W pe;
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsnicmp(pe.szExeFile, L"SteelSeries", 11) == 0) {
        Running r;
        r.name = pe.szExeFile;
        r.pid = pe.th32ProcessID;
        out.push_back(r);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return out;
}

bool KillByName(const std::wstring& name, std::wstring& detail) {
  bool allGone = true;
  for (const Running& r : SnapshotProcesses()) {
    if (_wcsicmp(r.name.c_str(), name.c_str()) != 0) continue;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, r.pid);
    if (!h) {
      // A process that has gone between the snapshot and here is the NORMAL
      // case, not a failure: killing one GG client takes its siblings with it,
      // so most of the seven are already dead by the time the loop reaches
      // them. OpenProcess reports a pid that no longer exists as
      // ERROR_INVALID_PARAMETER, which is "it is gone" -- exactly what was
      // wanted. Only a real refusal, such as ACCESS_DENIED on a process that
      // is still running, is worth reporting.
      if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
      allGone = false;
      detail += name + L"(open) ";
      continue;
    }
    if (!TerminateProcess(h, 0)) {
      // Same again, one step later. TerminateProcess fails with ACCESS_DENIED
      // on a process that has already begun to exit, so the call's return says
      // nothing on its own. Ask whether the process is actually still there.
      if (WaitForSingleObject(h, 2000) != WAIT_OBJECT_0) {
        allGone = false;
        detail += name + L"(term) ";
      }
    } else {
      WaitForSingleObject(h, 4000);
    }
    CloseHandle(h);
  }
  return allGone;
}

}  // namespace

SonarRestartPlan PlanSonarRestart() {
  SonarRestartPlan plan;
  const std::vector<Running> running = SnapshotProcesses();
  // NOT an early return when nothing is running. That is the case where a
  // relaunch matters most -- the tree is already down -- and bailing here is
  // what produced "no way to relaunch SteelSeries GG was found" from a machine
  // whose Run key names the command perfectly well.

  for (const wchar_t* name : kOrder) {
    for (const Running& r : running) {
      if (_wcsicmp(r.name.c_str(), name) == 0) {
        plan.order.push_back(name);
        break;                      // one entry per image name, not per process
      }
    }
  }


  return plan;
}

bool RestartSonar(std::wstring& detail) {
  detail.clear();
  const SonarRestartPlan plan = PlanSonarRestart();
  if (plan.order.empty()) {
    detail = L"the SteelSeries GUI is not running";
    return false;
  }

  bool clean = true;
  for (const std::wstring& name : plan.order)
    if (!KillByName(name, detail)) clean = false;

  // No relaunch, deliberately. SteelSeriesGG.exe is left running and rebuilds
  // its own renderers; see the note on kOrder for why starting anything from
  // here cannot work.
  return clean;
}

}  // namespace mdrop
