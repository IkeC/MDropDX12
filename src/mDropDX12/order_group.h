// order_group.h — fader-order arithmetic: moving a channel, and keeping the
// place of a fader that is not here right now.
//
// Header-only and dependency-free so the native suite can drive it: the real
// caller lives in engine_mixer_ui.cpp, which cannot be compiled alone without
// the whole tool-window layer. See forgejo#51's neighbours.
#pragma once

#include <string>

#include <vector>

namespace mdrop {

// The channel half of a "<channel>|<faderId>" order key.
inline std::wstring ChannelOfKey(const std::wstring& key) {
  const size_t bar = key.rfind(L'|');
  return bar == std::wstring::npos ? key : key.substr(0, bar);
}

// Move the channel owning keys[sel] by one place, taking every fader of that
// channel with it. Returns the new index of keys[sel], or -1 if it could not
// move.
//
// It GATHERS first. A channel's faders need not be adjacent -- Shane's order
// had Chat at position 6 and again at 11, which is why the list showed two
// "Chat" headings -- and moving a block that is not a block is meaningless. So
// the channel's rows are first collected at the position of the topmost one,
// in the order they already had, and the whole run then steps over its
// neighbouring channel.
//
// Gathering changes rows the user did not select, which is worth being
// deliberate about: it is the entire point here, since a split channel is the
// thing being repaired.
inline int MoveOrderGroupKeys(std::vector<std::wstring>& keys, int sel,
                              int delta) {
  if (sel < 0 || sel >= (int)keys.size() || delta == 0) return -1;
  const std::wstring channel = ChannelOfKey(keys[sel]);

  std::vector<std::wstring> mine, rest;
  int at = -1;
  for (int i = 0; i < (int)keys.size(); i++) {
    if (ChannelOfKey(keys[i]) == channel) {
      if (at < 0) at = (int)rest.size();     // where the block belongs
      mine.push_back(keys[i]);
    } else {
      rest.push_back(keys[i]);
    }
  }
  if (mine.empty()) return -1;

  // Where the block lands: over the whole of the neighbouring channel, not one
  // row of it, or a move would only ever swap with half of the channel above.
  int to = at;
  if (delta < 0) {
    if (at == 0) { to = 0; }
    else {
      const std::wstring above = ChannelOfKey(rest[at - 1]);
      to = at - 1;
      while (to > 0 && ChannelOfKey(rest[to - 1]) == above) to--;
    }
  } else {
    if (at >= (int)rest.size()) { to = at; }
    else {
      const std::wstring below = ChannelOfKey(rest[at]);
      to = at;
      while (to < (int)rest.size() && ChannelOfKey(rest[to]) == below) to++;
    }
  }

  std::vector<std::wstring> out;
  out.reserve(keys.size());
  for (int i = 0; i < (int)rest.size(); i++) {
    if (i == to) for (const std::wstring& k : mine) out.push_back(k);
    out.push_back(rest[i]);
  }
  if (to >= (int)rest.size()) for (const std::wstring& k : mine) out.push_back(k);

  const std::wstring want = keys[sel];
  keys.swap(out);
  for (int i = 0; i < (int)keys.size(); i++)
    if (keys[i] == want) return i;
  return -1;
}

// How far up the list a missing fader is worth holding a place for.
//
// Shane's rule, and the reasoning is his: "if a device was in the ordered list
// top 10 keep the devices in the ordered list as disconnected even if goes
// away; if they move it down then who cares". The top of the list is the
// deliberate arrangement -- the handful you actually reach for -- and a headset
// that is merely switched off should still be sitting where you put it when it
// comes back. Below that the order is not something anyone curated, so letting
// it fall out costs nothing.
//
// Counted in ROWS, not in devices: "a group's members count against the 10".
// So a Sonar channel, which contributes a monitoring and a streaming fader,
// spends two of the ten. Asked and answered, because the other reading is
// tempting and wrong -- do not "fix" this to count distinct channels.
inline constexpr int kKeepPlaceTop = 10;

// Fold the faders present now into the order that was stored, keeping the
// place of any that have gone away from near the top.
//
// `present` is the new arrangement of the faders that exist; `stored` is what
// was written last time. Anything in `stored` that is missing from `present`
// AND sat above kKeepPlaceTop is re-inserted at the index it held.
//
// Without this, saving the order silently forgot a device that happened to be
// unplugged at that moment: one move took the stored list from 25 entries to
// 24, and the entry it dropped was a real headset with a place in the list --
// not a view rule hiding it, just hardware that was switched off.
inline std::vector<std::wstring> MergeAbsentFaderOrder(
    const std::vector<std::wstring>& present,
    const std::vector<std::wstring>& stored) {
  std::vector<std::wstring> out = present;
  for (size_t i = 0; i < stored.size() && (int)i < kKeepPlaceTop; i++) {
    const std::wstring& key = stored[i];
    bool here = false;
    for (const std::wstring& k : out)
      if (k == key) { here = true; break; }
    if (here) continue;
    // At the index it held, or the end if the list has since grown shorter.
    const size_t at = (i < out.size()) ? i : out.size();
    out.insert(out.begin() + (ptrdiff_t)at, key);
  }
  return out;
}

}  // namespace mdrop
