// Per-process GPU utilisation, read from the same performance counters Task
// Manager shows.
//
// WHY IN THE APP RATHER THAN IN THE BENCH
//
// The obvious external route -- PowerShell's Get-Counter -- does not work here.
// `Get-Counter '\GPU Engine(*)\Utilization Percentage'` fails outright with
// "The data in one of the performance counter samples is not valid", because a
// handful of the ~1000 instances on this machine carry a bad status and
// Get-Counter refuses the whole set rather than the bad entries. Reading the
// counters directly through PDH and skipping items whose CStatus is not
// ERROR_SUCCESS returns the rest perfectly well, which is what this does.
//
// It also puts the number where it is useful: on the HUD while something is
// actually running, and over IPC so a bench can read it from every instance --
// including a -child, which has its own pid and its own GPU work and is exactly
// what the parent-plus-N-children measurement needs to see separately.
//
// COST AND WHEN IT RUNS
//
// PdhCollectQueryData walks every GPU engine instance on the machine, so this
// is polled about once a second and never per frame, and only while something
// has asked for it -- the debug overlay being up, or a DIAG_GPU request. It
// stays closed otherwise, so an ordinary run pays nothing.
//
// A rate counter needs two collects to produce a value, so the first Poll()
// after opening deliberately reports nothing.

#pragma once

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace mdrop {

class GpuUsageSampler {
public:
  // -1 until two polls have happened, or if the counters are unavailable.
  float Percent3D()  const { return m_pct3D; }
  float PercentAll() const { return m_pctAll; }
  bool  Available()  const { return m_pct3D >= 0.f; }

  // Call at most about once a second, and not from the render thread's hot
  // path. Safe to call when the counters are missing: it gives up once and
  // then does nothing.
  void Poll() {
    if (m_failed) return;
    if (!m_query && !Open()) return;

    if (PdhCollectQueryData(m_query) != ERROR_SUCCESS)
      return;
    if (!m_primed) {            // a rate counter has no value from one sample
      m_primed = true;
      return;
    }

    DWORD bufSize = 0, itemCount = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE,
                                                 &bufSize, &itemCount, nullptr);
    if (st != PDH_MORE_DATA || bufSize == 0)
      return;

    m_buf.resize(bufSize);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(m_buf.data());
    st = PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE,
                                      &bufSize, &itemCount, items);
    if (st != ERROR_SUCCESS)
      return;

    double all = 0.0, threeD = 0.0;
    for (DWORD i = 0; i < itemCount; i++) {
      // The reason Get-Counter cannot read these: some instances are always
      // bad. Skip them rather than abandoning the sample.
      if (items[i].FmtValue.CStatus != ERROR_SUCCESS || !items[i].szName)
        continue;
      const wchar_t* name = items[i].szName;
      if (!wcsstr(name, m_pidTag.c_str()))
        continue;
      const double v = items[i].FmtValue.doubleValue;
      if (v < 0.0)
        continue;
      all += v;
      // Instance names end "..._engtype_3D", "..._engtype_Copy", and so on.
      const wchar_t* eng = wcsstr(name, L"engtype_");
      if (eng && _wcsicmp(eng + 8, L"3D") == 0)
        threeD += v;
    }
    m_pct3D  = (float)threeD;
    m_pctAll = (float)all;
  }

  void Close() {
    if (m_query) {
      PdhCloseQuery(m_query);
      m_query = nullptr;
      m_counter = nullptr;
    }
    m_primed = false;
    m_pct3D = m_pctAll = -1.f;
  }

  ~GpuUsageSampler() { Close(); }

private:
  bool Open() {
    // "pid_1234_" -- the instance-name prefix for this process. Matching the
    // underscore matters: without it pid_123 also matches pid_1234.
    wchar_t tag[32];
    swprintf_s(tag, L"pid_%lu_", GetCurrentProcessId());
    m_pidTag = tag;

    if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS) {
      m_query = nullptr;
      m_failed = true;
      return false;
    }
    if (PdhAddEnglishCounterW(m_query, L"\\GPU Engine(*)\\Utilization Percentage",
                              0, &m_counter) != ERROR_SUCCESS) {
      // English-named lookup so a localised Windows still resolves the path.
      PdhCloseQuery(m_query);
      m_query = nullptr;
      m_counter = nullptr;
      m_failed = true;
      return false;
    }
    return true;
  }

  PDH_HQUERY        m_query = nullptr;
  PDH_HCOUNTER      m_counter = nullptr;
  bool              m_primed = false;
  bool              m_failed = false;   // counters absent: give up, stay quiet
  float             m_pct3D = -1.f;
  float             m_pctAll = -1.f;
  std::wstring      m_pidTag;
  std::vector<BYTE> m_buf;
};

} // namespace mdrop
