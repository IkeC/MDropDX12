// mixer_sonar_restart.h — stop and restart the SteelSeries GG process tree.
//
// The user reports SteelSeries "gets hung all the time and I can't kill it
// easily", and it is several cooperating processes rather than one, which is
// exactly what makes it awkward by hand.
//
// This NEVER runs on its own. A watchdog may notice GG has stopped answering
// and offer, but only a person starts it: a restart takes the whole audio path
// down for several seconds, and if it goes wrong the machine is left with no
// audio at all. That cost belongs to the user, not to a heuristic.
//
// Measured on this machine: every GG process RUNS as the ordinary user and the
// only service, SteelSeriesGGUpdateServiceProxy, is stopped. Killing therefore
// needs no elevation.
//
// STARTING does. SteelSeriesGGEZ.exe, the launcher, asks for administrator, so
// CreateProcess cannot start it at all -- it fails with ERROR_ELEVATION_REQUIRED
// (740) and the machine is left with the whole tree killed and nothing brought
// back. That is how a restart here ended in no audio until GG was started by
// hand. The relaunch goes through ShellExecuteEx, which is what Explorer and
// PowerShell's Start-Process use, and which performs the elevation.
#pragma once

#include <string>
#include <vector>

namespace mdrop {

struct SonarRestartPlan {
  // Image names in kill order. The GG parent is LAST: killed first it simply
  // restarts the children and the tree never goes down.
  std::vector<std::wstring> order;

  // Nothing is launched: see kOrder in the .cpp.
};

// Reads the running processes and works out what a restart would do. Changes
// nothing, which is what makes the order and the relaunch path testable
// without performing one.
SonarRestartPlan PlanSonarRestart();

// Executes a plan: terminate in order, wait for exit, relaunch the parent.
// Returns false and fills `detail` when something would not die or the
// relaunch failed. A process that refuses to terminate is reported, never
// retried indefinitely.
bool RestartSonar(std::wstring& detail);

}  // namespace mdrop
