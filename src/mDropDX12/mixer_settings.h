// mixer_settings.h — every audio-mixer setting, in resources\mixer.json.
//
// These used to live in the [AudioMixer] section of settings.ini, one flat key
// per value plus composed ones like "Route.sonar_monitoring.3.Name". Shane
// asked for them in one JSON file instead, and two things make that the right
// shape rather than a preference:
//
//   * The failover allowlist and the fader order are ORDERED LISTS. An INI
//     holds those as Order.0..Order.63 and Route.<id>.0.Id..15.Name -- sixty
//     lines of padding to express a list of six, and every read has to stop at
//     the first gap.
//   * A route id contains a colon ("sonar:monitoring"), which is not wanted in
//     an INI key, so it was sanitised to an underscore on the way in. That is
//     lossy: nothing in the file says where the colon was. JSON keys have no
//     such restriction and the id is stored as it really is.
//
// It also holds something settings.ini never could: our own NAMES for audio
// devices. Shane has four identical Sony WF-1000XM5 sets -- "2 black, one rose
// gold, and one off white" -- that Windows calls WF-1000XM5-1 through -4 with
// nothing to tell them apart, and renaming them in Windows does not stick; he
// has tried "registry rescans, reboots, etc." and an IPolicyConfig write is
// refused outright. So the name is ours to keep.
//
// Writes respect the testing-mode write shield exactly as ConfigStore does. A
// test run must not rewrite the real file, and this one is outside settings.ini
// so it would not have been covered by accident.
//
// There is no migration from the old [AudioMixer] INI section, deliberately:
// this file arrived in the same release the settings did, so no installation
// has ever held them anywhere else.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace mdrop {

// A set of faders that one hotkey moves together.
//
// This replaced two named slots -- "personal" and "streaming" -- that each
// pointed at a single fader. Those were an invention of the implementation
// rather than a request: the commit that added them said so outright, that
// two slots "keeps the action list at eight however many channels a provider
// publishes". Shane's account of the result was "I intended the hotkeys to
// control volume ... they never did anything for me", so the shape he asked
// for is the one here -- a key that moves as many faders as you tick.
//
// A group of one member is exactly the old slot, so nothing the slots could
// do is lost.
struct MixerGroupCfg {
  std::wstring name;                    // shown in the Hotkeys window
  std::vector<std::wstring> members;    // "channel|fader" keys
};

// One entry of a route's ordered failover allowlist. Either field may be
// empty: an id alone is exact, a name alone is the only way to list a device
// that is not connected right now -- and a pair of Bluetooth headphones is
// switched off far more often than it is on.
struct MixerAllowEntry {
  std::wstring id;
  std::wstring name;
};

struct MixerRouteCfg {
  bool armed = false;
  std::vector<MixerAllowEntry> allow;
};

// Our name for a device, and what Windows was calling it when the name was
// given. The Windows name is kept for RE-ASSOCIATION: a re-paired Bluetooth
// device comes back under a brand new endpoint id, and matching the name we
// saw is the only way to recognise it as the same earbuds.
struct MixerDeviceName {
  std::wstring id;
  std::wstring windowsName;
  std::wstring alias;
};

// One fader's user preferences, keyed the way `order` keys its entries --
// "<channel>|<faderId>", e.g. "sonar:aux|streaming". Same key space, so a
// client that has parsed one has parsed all of them (forgejo#52).
//
// Deliberately NOT folded into `order`, tempting as one list carrying position
// and attributes together is. Absence from `order` already MEANS something --
// "unordered, show after, in provider order" -- so listing a fader there just
// to record its short name would silently give it a position it never had.
//
// `hidden` is a view preference and nothing more: a hidden fader keeps its
// level, still answers every write verb, and keeps its place in `order` so
// revealing it puts it back where it was rather than at the end (forgejo#50).
struct MixerFaderPref {
  std::wstring key;
  bool         hidden = false;
  std::wstring shortName;

  // A row carrying neither is not worth writing, and dropping it on save is
  // what keeps clearing both from leaving litter in the file.
  bool Empty() const { return !hidden && shortName.empty(); }
};

struct MixerSettings {
  int  version = 1;

  bool enabled = false;          // the subsystem is off until asked for
  bool sonarEnabled = true;      // the Sonar provider, when Sonar is present

  // Confirmation, per surface. Local surfaces default to off -- a bound key
  // and a button in your own window are both things you reached deliberately.
  // The remote default is on and cannot be relaxed until a PIN is configured.
  int  confirmHotkeySeconds = 0;
  bool confirmToolWindow = false;
  bool confirmRemote = true;

  int  volumeStepPercent = 5;    // what one press of a volume hotkey moves
  bool sortUnmutedFirst = false; // float channels in use above muted ones

  // Hold the failover devices at the top of the list while they are here.
  //
  // Without it, sortUnmutedFirst files a device by its mute state, and the one
  // device you reach for most is the one whose mute you toggle most: Shane's
  // headphones dropped down among the muted rows next to a mic that is muted
  // permanently. A device good enough to be on a failover allowlist is one you
  // want to find in the same place every time, muted or not.
  //
  // Membership of ANY route's allowlist counts, matched by endpoint id or --
  // for an entry added while its device was switched off, which has no id --
  // by the Windows name it stored. Only faders that exist are affected, so
  // "while connected" needs no separate test: the endpoint provider publishes
  // a channel only for an active endpoint.
  bool pinFailoverDevices = false;

  // Show the Windows endpoints a provider owns. Off, because they are decoys:
  // named like the channels they shadow, stuck at 1.000, and inert. On for a
  // machine where a null-container endpoint is real and wanted -- NVIDIA
  // Broadcast and the GS Wavetable Synth are caught by the same test and
  // THEIR volumes work. See forgejo#51.
  bool showVirtualEndpoints = false;

  // Draw the faders the user has hidden anyway, so they can be revealed.
  //
  // Off, like showVirtualEndpoints, and for the same reason: the point of
  // hiding is that the row is gone. This is the way BACK -- without it a fader
  // hidden from the phone could only be recovered by editing mixer.json, which
  // is not a way back at all (forgejo#50).
  bool showHiddenFaders = false;

  // How the failover allowlist is ORDERED ON SCREEN, and nothing more.
  //
  //   0  the preferred order -- the rule itself, and the default
  //   1  device name
  //   2  battery
  //   3  last seen
  //
  // A view rule, in the same sense as sortUnmutedFirst: the stored order IS
  // the failover preference -- the watcher takes the first entry that is
  // present -- so a sorted view must never be written back. The Move Up/Down
  // buttons are hidden while this is anything but 0, because a move edits the
  // rule and, seen through a sort, would move a row somewhere the user cannot
  // see while the visible list sat still (forgejo#69).
  //
  // Sorting exists because the order that makes the RULE right is a poor way
  // to FIND a row: with several near-identical pairings the question is
  // usually "which of these did I last use", and that answer is in a column.
  int allowSort = 0;
  bool spinBoxes = false;        // type or step by 1, instead of dragging

  // The hotkey groups. One ships, because Shane asked for three actions "for
  // now" -- but the count is NOT capped here. Capping it to keep the action
  // list short is the reasoning that produced the slots this replaced, so a
  // second group is a file edit and three more HK_DEFs, not a redesign.
  //
  // Empty members by default: a volume key that arrived pointing at something
  // would move a fader nobody chose, and the actions ship unbound anyway.
  std::vector<MixerGroupCfg> groups { { L"Group 1", {} } };

  // Display order, as "channel|fader" keys, most used first. Anything not
  // listed is shown after, in provider order.
  std::vector<std::wstring> order;

  int stabilitySeconds = 3;      // how long a replacement must be steady
  int minDwellSeconds = 10;      // the least time between two switches

  // Keyed by route id, kept in insertion order so the file is stable between
  // saves rather than reshuffling on every write.
  std::vector<std::pair<std::wstring, MixerRouteCfg>> routes;

  std::vector<MixerDeviceName> deviceNames;

  // Per-fader preferences. Insertion-ordered like `routes`, for the same
  // reason: a stable file between saves rather than one that reshuffles on
  // every write.
  std::vector<MixerFaderPref> faderPrefs;
};

class MixerSettingsStore {
 public:
  // Directory holding mixer.json. Set once at startup, after the base
  // directory is known -- same contract as AudioProfileStore.
  void SetResourceDir(const wchar_t* dir);
  std::wstring StorePath() const;

  // Redirect the store at another file, for tests.
  //
  // Testing mode alone makes Save() a no-op, which protects the real file but
  // leaves the saving and loading themselves untested -- and those are exactly
  // the parts worth testing. Point this at a throwaway path and the round trip
  // runs for real. An empty string restores the normal file.
  void SetTestPath(const std::wstring& path);
  bool UsingTestPath() const { return !m_testPath.empty(); }

  MixerSettings& Get() { return m_s; }
  const MixerSettings& Get() const { return m_s; }

  // Reads the file. A missing file is not an error -- the defaults stand and
  // are written out, which is how a fresh install gets a mixer.json.
  bool Load();

  // Writes the file. Does nothing while the config write shield is up, so a
  // testing-mode session cannot rewrite the real one; the in-memory settings
  // still change, which is what a test then observes.
  bool Save();

  // The rule for a route, created empty if it has none yet.
  MixerRouteCfg& Route(const std::wstring& routeId);
  const MixerRouteCfg* FindRoute(const std::wstring& routeId) const;

  // ── Device names ──

  // Our name for a device, or an empty string if it has none.
  //
  // Matches on the endpoint id first. Failing that it matches on the Windows
  // name we recorded, and when THAT hits it rewrites the stored id: a
  // re-paired device has come back under a new one, and this is the moment we
  // can tell that it is the same earbuds.
  std::wstring AliasFor(const std::wstring& id, const std::wstring& windowsName);

  void SetAlias(const std::wstring& id, const std::wstring& windowsName,
                const std::wstring& alias);
  bool ClearAlias(const std::wstring& idOrName);

  // ── Per-fader preferences ──
  //
  // `key` is "<channel>|<faderId>", exactly as MIXER_ORDER reports it.

  bool IsHidden(const std::wstring& key) const;
  void SetHidden(const std::wstring& key, bool hidden);

  // The user's own short label for one fader, or empty if it has none. It does
  // NOT replace the reported chname/label -- those stay, so a client can still
  // show "Aux - Monitoring" in a tooltip when a short name is cryptic.
  std::wstring ShortNameFor(const std::wstring& key) const;

  // An empty name CLEARS rather than storing an empty label (forgejo#52).
  void SetShortName(const std::wstring& key, const std::wstring& name);

 private:
  // The row for `key`, or null. Non-const callers use the mutating form, which
  // creates one.
  const MixerFaderPref* FindPref(const std::wstring& key) const;
  MixerFaderPref& PrefFor(const std::wstring& key);
  void DropIfEmpty(const std::wstring& key);

  std::wstring  m_resourceDir;
  std::wstring  m_testPath;
  MixerSettings m_s;
  bool          m_loaded = false;
};

MixerSettingsStore& MixerCfg();

}  // namespace mdrop
