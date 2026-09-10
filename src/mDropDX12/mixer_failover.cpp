#include "mixer_failover.h"

#include <algorithm>

namespace mdrop {

void FailoverWatcher::NoteRouteDevice(const std::wstring& routeId,
                                      const std::wstring& deviceId) {
  RouteState& st = m_routes[routeId];
  if (st.current == deviceId) return;
  // A move re-baselines and abandons any pending arm: whether it came from a
  // person, from Sonar, or from our own commit, the question the watcher was
  // about to answer has just been answered.
  st.current = deviceId;
  st.candidate.clear();
  st.state = FailoverState::Idle;
  st.armedAt = 0;
  st.reason.clear();
}

std::wstring FailoverWatcher::Resolve(const AllowEntry& entry) const {
  if (Present(entry.id)) return entry.id;
  if (entry.name.empty() || !m_nameOf) return std::wstring();
  // No id match. The device may have come back under a new id carrying the
  // same name, which is exactly what a re-paired Bluetooth headset does, so
  // ask what id holds that name now.
  return m_nameOf(entry.name);
}

std::wstring FailoverWatcher::BestCandidate(const RouteRule& rule) const {
  for (const AllowEntry& entry : rule.allow) {
    const std::wstring resolved = Resolve(entry);
    if (!resolved.empty() && Present(resolved)) return resolved;
  }
  return std::wstring();
}

void FailoverWatcher::Tick() {
  const unsigned now = Now();

  for (const auto& pair : m_rules) {
    const RouteRule& rule = pair.second;
    RouteState& st = m_routes[rule.routeId];

    if (!rule.armed) {
      st.state = FailoverState::Idle;
      st.candidate.clear();
      st.reason = L"not armed";
      continue;
    }

    if (Present(st.current)) {
      // Cancel on return: whatever was being armed is dropped the moment the
      // device we are actually on is back.
      st.state = FailoverState::Idle;
      st.candidate.clear();
      st.armedAt = 0;
      st.reason.clear();
      continue;
    }

    // The current device is gone.
    if (st.committedAt != 0 && m_dwellMs != 0 &&
        (now - st.committedAt) < m_dwellMs) {
      st.state = FailoverState::Searching;
      st.reason = L"waiting out the minimum dwell";
      continue;
    }

    const std::wstring candidate = BestCandidate(rule);
    if (candidate.empty()) {
      st.state = FailoverState::Searching;
      st.candidate.clear();
      st.armedAt = 0;
      st.reason = L"no allowed replacement is present";
      continue;
    }

    if (st.state != FailoverState::Arming || st.candidate != candidate) {
      // First sight of this candidate, or a different one than we were
      // arming: start its window from now.
      st.state = FailoverState::Arming;
      st.candidate = candidate;
      st.armedAt = now;
      st.reason = L"waiting for the replacement to be stable";
      continue;
    }

    if ((now - st.armedAt) >= m_stabilityMs) {
      const std::wstring target = st.candidate;
      st.current = target;
      st.candidate.clear();
      st.state = FailoverState::Idle;
      st.armedAt = 0;
      st.committedAt = now;
      st.reason.clear();
      if (m_onCommit) m_onCommit(rule.routeId, target);
    }
  }
}

FailoverState FailoverWatcher::StateOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? FailoverState::Idle : it->second.state;
}

std::wstring FailoverWatcher::ReasonOf(const std::wstring& routeId) const {
  const auto it = m_routes.find(routeId);
  return it == m_routes.end() ? std::wstring() : it->second.reason;
}

bool AllowEntryMatches(const AllowEntry& e, const std::wstring& key) {
  if (key.empty()) return false;
  return (!e.id.empty()   && e.id   == key) ||
         (!e.name.empty() && e.name == key);
}

int MoveAllowEntry(RouteRule& rule, const std::wstring& key, int delta) {
  int from = -1;
  for (size_t i = 0; i < rule.allow.size(); i++) {
    if (AllowEntryMatches(rule.allow[i], key)) { from = (int)i; break; }
  }
  if (from < 0) return -1;

  int to = from + delta;
  if (to < 0) to = 0;
  if (to >= (int)rule.allow.size()) to = (int)rule.allow.size() - 1;

  // Clamping rather than refusing. Up from the top is what a user pressing the
  // button repeatedly does, and answering that with an error would make the
  // control feel broken at exactly the moment it is doing the right thing.
  if (to != from) std::swap(rule.allow[from], rule.allow[to]);
  return to;
}

}  // namespace mdrop
