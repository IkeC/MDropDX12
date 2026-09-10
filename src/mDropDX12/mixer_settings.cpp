#include "mixer_settings.h"

#include "config_store.h"
#include "json_utils.h"
#include "utility.h"

#include <windows.h>

#include <initializer_list>

namespace mdrop {

MixerSettingsStore& MixerCfg() {
  static MixerSettingsStore store;
  return store;
}

void MixerSettingsStore::SetResourceDir(const wchar_t* dir) {
  m_resourceDir = dir ? dir : L"";
}

std::wstring MixerSettingsStore::StorePath() const {
  return m_testPath.empty() ? m_resourceDir + L"mixer.json" : m_testPath;
}

void MixerSettingsStore::SetTestPath(const std::wstring& path) {
  // A test gets a COPY of the real settings, not an empty store.
  //
  // This is the same bargain the settings.ini write shield strikes -- reads
  // see the user's real configuration, writes never reach his disk -- and the
  // redirect exists only because a shield makes saving a no-op, so persistence
  // itself cannot be tested through one. Redirecting must not also change what
  // is being tested.
  //
  // It briefly did. An earlier fix here made a redirect reset to defaults,
  // because two tests asserting an empty route were finding six real devices
  // in it. That got the direction backwards: an empty store makes every suite
  // test an empty machine, where presence, battery and ordering have nothing
  // to look at and quietly skip. A test that wants nothing allowed should say
  // so, rather than depending on the store having been blanked underneath it.
  //
  // Seeding happens only when the scratch file is not there yet. An existing
  // one is read as it stands, so a test may lay out a fixture in advance and
  // a re-run continues from it.
  const bool seeding =
      !path.empty() &&
      GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;

  m_testPath = path;
  if (seeding) {
    // m_s already holds the real settings, so writing them out IS the copy.
    Save();
    return;
  }
  Load();
}

// ── Reading ───────────────────────────────────────────────────────────────

bool MixerSettingsStore::Load() {
  m_loaded = true;
  const std::wstring path = StorePath();

  JsonValue root;
  try {
    root = JsonLoadFile(path.c_str());
  } catch (...) {
    // JsonParse throws on malformed input, and this build is /EHa with a
    // static CRT: an escaping exception calls abort() with no log and no
    // dialog. A settings file someone has hand-edited must not take the
    // visualiser down, so a broken one reads as "no file" and the defaults
    // stand until it is saved over.
    DebugLogW(L"mixer.json could not be parsed; using defaults", LOG_ERROR);
    root = JsonValue();
  }

  if (!root.isObject()) {
    // Back to DEFAULTS, whichever way it failed.
    //
    // This used to leave m_s holding whatever was loaded last, which was wrong
    // in a way that only showed under test: pointing the store at a scratch
    // file kept the real settings in memory and then WROTE THEM INTO the
    // scratch file, so two tests that assume an empty route found six real
    // devices in it.
    m_s = MixerSettings();

    // A file that is simply not there yet gets written, so the defaults are
    // visible and editable rather than implied. One that exists but could not
    // be read is LEFT ALONE: overwriting a file somebody hand-edited into
    // invalidity would destroy the very thing they need to fix.
    const bool exists =
        GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists) {
      Save();
      DebugLogW(L"mixer.json created with defaults", LOG_INFO);
    } else {
      DebugLogW(L"mixer.json could not be read; running on defaults and "
                L"leaving the file untouched", LOG_ERROR);
    }
    return false;
  }

  const MixerSettings d;   // member initialisers are the defaults
  m_s.version              = root[L"version"].asInt(d.version);
  m_s.enabled              = root[L"enabled"].asBool(d.enabled);
  m_s.sonarEnabled         = root[L"sonarEnabled"].asBool(d.sonarEnabled);

  const JsonValue& confirm = root[L"confirm"];
  m_s.confirmHotkeySeconds = confirm[L"hotkeySeconds"].asInt(d.confirmHotkeySeconds);
  m_s.confirmToolWindow    = confirm[L"toolWindow"].asBool(d.confirmToolWindow);
  m_s.confirmRemote        = confirm[L"remote"].asBool(d.confirmRemote);

  m_s.volumeStepPercent    = root[L"volumeStepPercent"].asInt(d.volumeStepPercent);
  m_s.sortUnmutedFirst     = root[L"sortUnmutedFirst"].asBool(d.sortUnmutedFirst);
  m_s.pinFailoverDevices   = root[L"pinFailoverDevices"].asBool(d.pinFailoverDevices);
  m_s.showVirtualEndpoints = root[L"showVirtualEndpoints"].asBool(d.showVirtualEndpoints);
  m_s.showHiddenFaders = root[L"showHiddenFaders"].asBool(d.showHiddenFaders);
  m_s.allowSort            = root[L"allowSort"].asInt(d.allowSort);
  // A file written by a later version, or edited by hand, must not select a
  // column that does not exist -- the sort would fall through to no ordering
  // at all and look like the feature had broken.
  if (m_s.allowSort < 0 || m_s.allowSort > 3) m_s.allowSort = d.allowSort;
  m_s.spinBoxes            = root[L"spinBoxes"].asBool(d.spinBoxes);

  // Hotkey groups. A file from before groups existed has no "groups" key and
  // keeps the default single empty group -- the old "slots" object is read by
  // nothing and simply drops out on the next save.
  const JsonValue& groups = root[L"groups"];
  if (groups.size() > 0) {
    m_s.groups.clear();
    for (size_t i = 0; i < groups.size(); i++) {
      const JsonValue& g = groups.at(i);
      MixerGroupCfg cfg;
      cfg.name = g[L"name"].asString(L"");
      const JsonValue& members = g[L"members"];
      for (size_t j = 0; j < members.size(); j++) {
        const std::wstring key = members.at(j).asString();
        if (!key.empty()) cfg.members.push_back(key);
      }
      // Unnamed groups are numbered rather than shown blank: the name is what
      // the Hotkeys window lists, and a blank row there names nothing.
      if (cfg.name.empty()) cfg.name = L"Group " + std::to_wstring(i + 1);
      m_s.groups.push_back(cfg);
    }
  }
  // Never zero groups. The hotkey actions are compiled in and have to have
  // something to point at, and "groups": [] in a hand-edited file would
  // otherwise make every press a no-op with nothing on screen to explain it.
  if (m_s.groups.empty()) m_s.groups = d.groups;

  m_s.order.clear();
  const JsonValue& order = root[L"order"];
  for (size_t i = 0; i < order.size(); i++) {
    const std::wstring key = order.at(i).asString();
    if (!key.empty()) m_s.order.push_back(key);
  }

  m_s.faderPrefs.clear();
  const JsonValue& prefs = root[L"faderPrefs"];
  for (const auto& member : prefs.members) {
    MixerFaderPref fp;
    fp.key       = member.first;
    fp.hidden    = member.second[L"hidden"].asBool(false);
    fp.shortName = member.second[L"short"].asString(L"");
    // A row that says nothing is dropped on the way in as well as out, so a
    // hand-edited file cannot accumulate them.
    if (!fp.key.empty() && !fp.Empty()) m_s.faderPrefs.push_back(fp);
  }

  const JsonValue& fo = root[L"failover"];
  m_s.stabilitySeconds = fo[L"stabilitySeconds"].asInt(d.stabilitySeconds);
  m_s.minDwellSeconds  = fo[L"minDwellSeconds"].asInt(d.minDwellSeconds);

  m_s.routes.clear();
  const JsonValue& routes = fo[L"routes"];
  for (const auto& member : routes.members) {
    MixerRouteCfg rule;
    rule.armed = member.second[L"armed"].asBool(false);
    const JsonValue& allow = member.second[L"allow"];
    for (size_t i = 0; i < allow.size(); i++) {
      MixerAllowEntry e;
      e.id   = allow.at(i)[L"id"].asString();
      e.name = allow.at(i)[L"name"].asString();
      if (!e.id.empty() || !e.name.empty()) rule.allow.push_back(e);
    }
    m_s.routes.push_back({ member.first, rule });
  }

  m_s.deviceNames.clear();
  const JsonValue& names = root[L"deviceNames"];
  for (size_t i = 0; i < names.size(); i++) {
    MixerDeviceName n;
    n.id          = names.at(i)[L"id"].asString();
    n.windowsName = names.at(i)[L"windowsName"].asString();
    n.alias       = names.at(i)[L"alias"].asString();
    if (!n.alias.empty()) m_s.deviceNames.push_back(n);
  }
  return true;
}

// ── Writing ───────────────────────────────────────────────────────────────

bool MixerSettingsStore::Save() {
  // The same rule settings.ini follows: testing mode diverts writes so a run
  // leaves no trace, and this file is not part of that store, so it has to
  // ask. A test that WANTS to exercise saving points the store at its own file
  // first, and then the write is real -- against a path the test owns.
  if (m_testPath.empty() && IsConfigWriteShielded()) return true;
  if (m_testPath.empty() && m_resourceDir.empty()) return false;

  // Records go through JsonValue rather than JsonWriter: the writer has no
  // way to open an anonymous object inside an array, and an allowlist is a
  // list of {id, name} pairs. ValueAnon/Value emit a built tree verbatim.
  auto obj = [](std::initializer_list<std::pair<const wchar_t*, std::wstring>> f) {
    JsonValue v;
    v.type = JsonValue::Object;
    for (const auto& kv : f) v.members.push_back({ kv.first, JsonValue(kv.second) });
    return v;
  };

  JsonValue order;
  order.type = JsonValue::Array;
  for (const std::wstring& key : m_s.order) order.elements.push_back(JsonValue(key));

  JsonValue groups;
  groups.type = JsonValue::Array;
  for (const MixerGroupCfg& g : m_s.groups) {
    JsonValue members;
    members.type = JsonValue::Array;
    for (const std::wstring& key : g.members)
      members.elements.push_back(JsonValue(key));

    JsonValue entry;
    entry.type = JsonValue::Object;
    entry.members.push_back({ L"name", JsonValue(g.name) });
    entry.members.push_back({ L"members", members });
    groups.elements.push_back(entry);
  }

  JsonValue routes;
  routes.type = JsonValue::Object;
  for (const auto& r : m_s.routes) {
    JsonValue allow;
    allow.type = JsonValue::Array;
    for (const MixerAllowEntry& e : r.second.allow)
      allow.elements.push_back(obj({ { L"id", e.id }, { L"name", e.name } }));

    JsonValue rule;
    rule.type = JsonValue::Object;
    rule.members.push_back({ L"armed", JsonValue(r.second.armed) });
    rule.members.push_back({ L"allow", allow });
    routes.members.push_back({ r.first, rule });
  }

  JsonValue failover;
  failover.type = JsonValue::Object;
  failover.members.push_back({ L"stabilitySeconds", JsonValue(m_s.stabilitySeconds) });
  failover.members.push_back({ L"minDwellSeconds", JsonValue(m_s.minDwellSeconds) });
  failover.members.push_back({ L"routes", routes });

  JsonValue names;
  names.type = JsonValue::Array;
  for (const MixerDeviceName& n : m_s.deviceNames)
    names.elements.push_back(obj({ { L"id", n.id },
                                   { L"windowsName", n.windowsName },
                                   { L"alias", n.alias } }));

  JsonValue faderPrefs;
  faderPrefs.type = JsonValue::Object;
  for (const MixerFaderPref& fp : m_s.faderPrefs) {
    if (fp.Empty()) continue;         // never write a row that says nothing
    JsonValue one;
    one.type = JsonValue::Object;
    if (fp.hidden) one.members.push_back({ L"hidden", JsonValue(true) });
    if (!fp.shortName.empty())
      one.members.push_back({ L"short", JsonValue(fp.shortName) });
    faderPrefs.members.push_back({ fp.key, one });
  }

  JsonWriter w;
  w.BeginObject();
  w.Int(L"version", m_s.version);
  w.Bool(L"enabled", m_s.enabled);
  w.Bool(L"sonarEnabled", m_s.sonarEnabled);

  w.BeginObject(L"confirm");
  w.Int(L"hotkeySeconds", m_s.confirmHotkeySeconds);
  w.Bool(L"toolWindow", m_s.confirmToolWindow);
  w.Bool(L"remote", m_s.confirmRemote);
  w.EndObject();

  w.Int(L"volumeStepPercent", m_s.volumeStepPercent);
  w.Bool(L"sortUnmutedFirst", m_s.sortUnmutedFirst);
  w.Bool(L"pinFailoverDevices", m_s.pinFailoverDevices);
  w.Bool(L"showVirtualEndpoints", m_s.showVirtualEndpoints);
  w.Bool(L"showHiddenFaders", m_s.showHiddenFaders);
  w.Int(L"allowSort", m_s.allowSort);
  w.Bool(L"spinBoxes", m_s.spinBoxes);

  w.Value(L"groups", groups);

  w.Value(L"order", order);
  w.Value(L"faderPrefs", faderPrefs);
  w.Value(L"failover", failover);
  w.Value(L"deviceNames", names);
  w.EndObject();

  return w.SaveToFile(StorePath().c_str());

}

// ── Routes ────────────────────────────────────────────────────────────────

MixerRouteCfg& MixerSettingsStore::Route(const std::wstring& routeId) {
  for (auto& r : m_s.routes)
    if (r.first == routeId) return r.second;
  m_s.routes.push_back({ routeId, MixerRouteCfg() });
  return m_s.routes.back().second;
}

const MixerRouteCfg* MixerSettingsStore::FindRoute(const std::wstring& routeId) const {
  for (const auto& r : m_s.routes)
    if (r.first == routeId) return &r.second;
  return nullptr;
}

// ── Device names ──────────────────────────────────────────────────────────

std::wstring MixerSettingsStore::AliasFor(const std::wstring& id,
                                          const std::wstring& windowsName) {
  for (const MixerDeviceName& n : m_s.deviceNames)
    if (!n.id.empty() && n.id == id) return n.alias;

  // No id match. The same earbuds re-paired come back under a new endpoint id,
  // so the Windows name we recorded is the only thread back to them. Rewrite
  // the id while we can see both halves; the next lookup is then exact.
  if (!windowsName.empty()) {
    for (MixerDeviceName& n : m_s.deviceNames) {
      if (n.windowsName != windowsName) continue;
      if (n.id != id) {
        n.id = id;
        Save();
      }
      return n.alias;
    }
  }
  return std::wstring();
}

void MixerSettingsStore::SetAlias(const std::wstring& id,
                                  const std::wstring& windowsName,
                                  const std::wstring& alias) {
  for (MixerDeviceName& n : m_s.deviceNames) {
    // Matched by id when there is one, by Windows name when there is not --
    // a device that is switched off has no endpoint id to be keyed on, and
    // naming one you cannot currently see is the whole point of naming it in
    // a list of replacements.
    const bool same = !id.empty() ? (n.id == id)
                                  : (!windowsName.empty() && n.windowsName == windowsName);
    if (!same) continue;
    n.windowsName = windowsName;
    n.alias = alias;
    Save();
    return;
  }
  MixerDeviceName n;
  n.id = id;
  n.windowsName = windowsName;
  n.alias = alias;
  m_s.deviceNames.push_back(n);
  Save();
}

bool MixerSettingsStore::ClearAlias(const std::wstring& idOrName) {
  for (size_t i = 0; i < m_s.deviceNames.size(); i++) {
    const MixerDeviceName& n = m_s.deviceNames[i];
    if (n.id != idOrName && n.windowsName != idOrName && n.alias != idOrName)
      continue;
    m_s.deviceNames.erase(m_s.deviceNames.begin() + i);
    Save();
    return true;
  }
  return false;
}

// ── Per-fader preferences ─────────────────────────────────────────────────
//
// A linear scan, like AliasFor's. The list is one row per fader the user has
// expressed an opinion about -- a couple of dozen at the very most on a machine
// with nineteen channels -- and it is read while building a row, not per frame.

const MixerFaderPref* MixerSettingsStore::FindPref(const std::wstring& key) const {
  for (const MixerFaderPref& fp : m_s.faderPrefs)
    if (fp.key == key) return &fp;
  return nullptr;
}

MixerFaderPref& MixerSettingsStore::PrefFor(const std::wstring& key) {
  for (MixerFaderPref& fp : m_s.faderPrefs)
    if (fp.key == key) return fp;
  MixerFaderPref fp;
  fp.key = key;
  m_s.faderPrefs.push_back(fp);
  return m_s.faderPrefs.back();
}

// Clearing the last opinion about a fader removes the row rather than leaving
// {"hidden":false,"short":""} behind. Save() skips empty rows too, so this is
// belt and braces -- but it also keeps the in-memory list honest for anything
// that enumerates it, such as MIXER_HIDDEN.
void MixerSettingsStore::DropIfEmpty(const std::wstring& key) {
  for (size_t i = 0; i < m_s.faderPrefs.size(); i++) {
    if (m_s.faderPrefs[i].key == key) {
      if (m_s.faderPrefs[i].Empty())
        m_s.faderPrefs.erase(m_s.faderPrefs.begin() + i);
      return;
    }
  }
}

bool MixerSettingsStore::IsHidden(const std::wstring& key) const {
  const MixerFaderPref* fp = FindPref(key);
  return fp && fp->hidden;
}

void MixerSettingsStore::SetHidden(const std::wstring& key, bool hidden) {
  if (key.empty()) return;
  PrefFor(key).hidden = hidden;
  DropIfEmpty(key);
}

std::wstring MixerSettingsStore::ShortNameFor(const std::wstring& key) const {
  const MixerFaderPref* fp = FindPref(key);
  return fp ? fp->shortName : std::wstring();
}

void MixerSettingsStore::SetShortName(const std::wstring& key,
                                      const std::wstring& name) {
  if (key.empty()) return;
  PrefFor(key).shortName = name;
  DropIfEmpty(key);
}

}  // namespace mdrop
