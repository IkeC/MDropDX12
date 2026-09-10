// mixer_failover.h — re-home a route when the device it is on disappears.
//
// The rules, all chosen explicitly:
//
//   * Fail over, NEVER back. After a commit the new device is the current one.
//     Reconnecting the old one does nothing.
//   * Cancel on return. If the original comes back during the stability
//     window, nothing moves -- so a brief Bluetooth dropout costs nothing.
//     That is the point of the debounce rather than a side effect of it.
//   * Opt-in is absolute. With no allowlisted device present the watcher does
//     nothing, forever, and says why.
//   * A minimum dwell after each commit stops one bad unplug cascading down
//     the priority list.
//
// Clock and presence are injected, so every sequence is testable without a
// device: disconnect, flap, re-pair under a new id, nothing available.
#pragma once

#include "mixer_provider.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mdrop {

enum class FailoverState { Idle, Searching, Arming };

// Both halves are stored. A re-paired Bluetooth headset can return with a new
// endpoint id and the same friendly name, so an allowlist keyed on id alone
// silently stops matching -- which is already true of the user's own Sonar
// configuration, where a monitoring route points at a WF-1000XM5-3 while
// Windows reports WF-1000XM5-4.
struct AllowEntry {
  std::wstring id;
  std::wstring name;
};

struct RouteRule {
  std::wstring routeId;
  bool armed = false;
  std::vector<AllowEntry> allow;   // ordered: first present entry wins
};

// Does this entry answer to `key`? By id OR by name.
//
// Both, because a device that is not connected is listed by NAME with no id at
// all -- and a pair of Bluetooth headphones is switched off far more often
// than it is on. Matching on the id alone made exactly those entries
// unreachable: they could not be removed, and they piled up. Shared so the
// remove and the reorder cannot drift apart on it.
bool AllowEntryMatches(const AllowEntry& e, const std::wstring& key);

// Move the entry answering to `key` by `delta` places, swapping with its
// neighbour and clamping at the ends. Returns the new index, or -1 when
// nothing matches.
//
// The order is the preference: `allow` is "first present entry wins", so this
// is what decides which replacement failover reaches for. Kept here, as a pure
// function on the rule, so that decision is testable without an audio device
// or a running app.
int MoveAllowEntry(RouteRule& rule, const std::wstring& key, int delta);

class FailoverWatcher {
 public:
  void SetRule(const RouteRule& rule) { m_rules[rule.routeId] = rule; }
  void ClearRules() { m_rules.clear(); m_routes.clear(); }

  void SetStabilitySeconds(int s) { m_stabilityMs = (unsigned)(s * 1000); }
  void SetMinDwellSeconds(int s)  { m_dwellMs = (unsigned)(s * 1000); }

  void SetPresence(std::function<bool(const std::wstring&)> fn) {
    m_present = std::move(fn);
  }
  // NAME -> the id currently carrying it. That direction is the Bluetooth
  // fallback; getting it backwards makes the re-pair case silently dead.
  void SetNames(std::function<std::wstring(const std::wstring&)> fn) {
    m_nameOf = std::move(fn);
  }
  void SetClock(std::function<unsigned()> fn) { m_now = std::move(fn); }
  void SetOnCommit(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    m_onCommit = std::move(fn);
  }

  // Tell the watcher where a route currently points. Called at startup, after
  // a device event, and after anything -- including a person -- moves it. A
  // change abandons any pending arm: the question has just been answered.
  void NoteRouteDevice(const std::wstring& routeId,
                       const std::wstring& deviceId);

  // Advance the machine. MUST be called periodically, not only on device
  // events: arming and committing are deliberately separate ticks, so a route
  // that arms and then sees no further event would never commit. The mixer
  // worker wakes at least every 100ms, which is where this belongs. It is
  // cheap and does no I/O.
  void Tick();

  FailoverState StateOf(const std::wstring& routeId) const;
  std::wstring ReasonOf(const std::wstring& routeId) const;

 private:
  struct RouteState {
    std::wstring current;      // device the route is on
    std::wstring candidate;    // device being armed
    FailoverState state = FailoverState::Idle;
    unsigned armedAt = 0;
    unsigned committedAt = 0;
    std::wstring reason;
  };

  unsigned Now() const { return m_now ? m_now() : 0; }
  bool Present(const std::wstring& id) const {
    return m_present && !id.empty() && m_present(id);
  }
  // Matches by id, then by an exact friendly-name match when no id matches.
  // The fallback can only ever resolve to an entry the user added, so opt-in
  // is preserved: it never enrols a device.
  std::wstring Resolve(const AllowEntry& entry) const;
  std::wstring BestCandidate(const RouteRule& rule) const;

  std::map<std::wstring, RouteRule> m_rules;
  std::map<std::wstring, RouteState> m_routes;

  unsigned m_stabilityMs = 3000;
  unsigned m_dwellMs = 10000;

  std::function<bool(const std::wstring&)> m_present;
  std::function<std::wstring(const std::wstring&)> m_nameOf;
  std::function<unsigned()> m_now;
  std::function<void(const std::wstring&, const std::wstring&)> m_onCommit;
};

}  // namespace mdrop
