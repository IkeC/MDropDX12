// preset_replacements.cpp — see preset_replacements.h.

#include "preset_replacements.h"
#include "json_utils.h"
#include "config_store.h"
#include "utility.h"

#include <windows.h>
#include <shlwapi.h>
#include <cstdio>

namespace mdrop {

// ─── the header probe ───────────────────────────────────────────────────

// How much of a preset to read looking for the key. MD3 PRO writes MD31=/MD32=
// into the header block, well inside the first few hundred bytes; 8 KB is
// generous enough to survive a reordered header and small enough that doing it
// once per preset change is free.
static const size_t kHeaderProbeBytes = 8192;

// A .milk is byte-oriented ASCII with real line breaks in the HEADER -- the
// LINEFEED_CONTROL_CHAR (0x01) convention applies inside code blocks, which sit
// below anything we look at here. So a plain line scan is correct.
static bool LineValueAt(const char* buf, size_t len, const char* key,
                        std::wstring* out) {
  const size_t klen = strlen(key);
  for (size_t i = 0; i + klen + 1 < len; i++) {
    // Must be at the start of a line, and followed by '='.
    if (i != 0 && buf[i - 1] != '\n' && buf[i - 1] != '\r') continue;
    if (_strnicmp(buf + i, key, klen) != 0) continue;
    if (buf[i + klen] != '=') continue;
    size_t v = i + klen + 1;
    size_t e = v;
    while (e < len && buf[e] != '\r' && buf[e] != '\n' && buf[e] != 0) e++;
    while (e > v && (buf[e - 1] == ' ' || buf[e - 1] == '\t')) e--;
    if (e == v) return false;                 // present but empty: not a key
    out->assign(buf + v, buf + e);
    return true;
  }
  return false;
}

std::wstring PresetReplacementStore::KeyForPreset(const wchar_t* presetPath) {
  if (!presetPath || !presetPath[0]) return std::wstring();

  FILE* f = _wfopen(presetPath, L"rb");
  if (!f) return std::wstring();
  std::vector<char> buf(kHeaderProbeBytes);
  const size_t got = fread(buf.data(), 1, buf.size(), f);
  fclose(f);
  if (got == 0) return std::wstring();

  // MD31 first: a file carrying both is an MD31 preset that also has a second
  // cached stage, and the corpus keys its replacements on MD31 where both exist.
  std::wstring val;
  if (LineValueAt(buf.data(), got, "MD31", &val)) return L"MD31:" + val;
  if (LineValueAt(buf.data(), got, "MD32", &val)) return L"MD32:" + val;
  return std::wstring();
}

// ─── paths ──────────────────────────────────────────────────────────────

std::wstring PresetReplacementStore::StorePath() const {
  return m_resourceDir + L"preset_replacements.json";
}

static bool FileExists(const std::wstring& p) {
  if (p.empty()) return false;
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring BareName(const wchar_t* path) {
  if (!path) return std::wstring();
  const wchar_t* slash = wcsrchr(path, L'\\');
  const wchar_t* fwd = wcsrchr(path, L'/');
  if (fwd && (!slash || fwd > slash)) slash = fwd;
  return slash ? std::wstring(slash + 1) : std::wstring(path);
}

// ─── resolution ─────────────────────────────────────────────────────────

std::wstring PresetReplacementStore::ExplicitFor(const std::wstring& key) const {
  for (const auto& [k, p] : m_explicit)
    if (_wcsicmp(k.c_str(), key.c_str()) == 0) return p;
  return std::wstring();
}

void PresetReplacementStore::SetExplicit(const std::wstring& key,
                                         const std::wstring& path) {
  for (size_t i = 0; i < m_explicit.size(); i++) {
    if (_wcsicmp(m_explicit[i].first.c_str(), key.c_str()) != 0) continue;
    if (path.empty()) m_explicit.erase(m_explicit.begin() + i);
    else              m_explicit[i].second = path;
    return;
  }
  if (!path.empty()) m_explicit.emplace_back(key, path);
}

std::wstring PresetReplacementStore::Resolve(const wchar_t* presetPath,
                                             const std::wstring& key,
                                             std::wstring* whyNot) const {
  if (whyNot) whyNot->clear();
  if (!m_bEnabled || key.empty() || !presetPath) return std::wstring();

  // 1. An explicit path for this key value.
  const std::wstring ex = ExplicitFor(key);
  if (!ex.empty()) {
    if (FileExists(ex)) return ex;
    // Named but absent. Say so: a designated replacement that has gone missing
    // is a configuration error the user wants to hear about, not a silent
    // fallthrough to a directory that happens to hold a same-named file.
    if (whyNot) *whyNot = ex;
    return std::wstring();
  }

  // 2. Same bare name on a search path, in order.
  const std::wstring bare = BareName(presetPath);
  if (bare.empty()) return std::wstring();
  for (const auto& dir : m_searchPaths) {
    if (dir.empty()) continue;
    std::wstring cand = dir;
    if (cand.back() != L'\\' && cand.back() != L'/') cand += L'\\';
    cand += bare;
    if (FileExists(cand)) {
      // Never replace a file with itself: a search path pointed at the preset's
      // own directory would otherwise loop the load through a "replacement"
      // that is the same bytes.
      wchar_t a[MAX_PATH * 2] = {0}, b[MAX_PATH * 2] = {0};
      GetFullPathNameW(cand.c_str(), MAX_PATH * 2, a, NULL);
      GetFullPathNameW(presetPath, MAX_PATH * 2, b, NULL);
      if (_wcsicmp(a, b) == 0) continue;
      return cand;
    }
  }
  return std::wstring();
}

// ─── load / save ────────────────────────────────────────────────────────

bool PresetReplacementStore::Load(const wchar_t* resourceDir) {
  m_resourceDir = resourceDir ? resourceDir : L"";
  m_searchPaths.clear();
  m_explicit.clear();
  m_unknownRoot.clear();
  m_bEnabled = true;
  m_bLoaded = true;

  JsonValue root;
  try {
    root = JsonLoadFile(StorePath().c_str());
  } catch (...) {
    // /EHa with a static CRT: an escaping exception aborts with nothing in the
    // log, which is how a malformed store would look like a crash on startup.
    DLOG_WARN("PresetReplacements: preset_replacements.json failed to parse; "
              "starting empty");
    return false;
  }
  if (root.isNull()) {
    DLOG_INFO("PresetReplacements: no preset_replacements.json; starting empty");
    return true;   // absence is normal, not an error
  }

  if (root.has(L"enabled")) m_bEnabled = root[L"enabled"].asBool(true);

  const JsonValue& paths = root[L"searchPaths"];
  if (paths.isArray()) {
    for (size_t i = 0; i < paths.size(); i++) {
      std::wstring p = paths.at(i).asString();
      if (!p.empty()) m_searchPaths.push_back(std::move(p));
    }
  }

  const JsonValue& reps = root[L"replacements"];
  if (reps.isObject()) {
    for (const auto& [key, val] : reps.members) {
      const std::wstring path = val.asString();
      if (!key.empty() && !path.empty()) m_explicit.emplace_back(key, path);
    }
  }

  for (const auto& [key, val] : root.members) {
    if (key == L"version" || key == L"enabled" ||
        key == L"searchPaths" || key == L"replacements") continue;
    m_unknownRoot.emplace_back(key, val);
  }

  DLOG_INFO("PresetReplacements: %d search paths, %d explicit, enabled=%d",
            (int)m_searchPaths.size(), (int)m_explicit.size(),
            m_bEnabled ? 1 : 0);
  return true;
}

bool PresetReplacementStore::Save() const {
  // The rule settings.ini follows: testing mode diverts writes so a run leaves
  // no trace. shaderoverrides.json does NOT do this and should; a new store is
  // not the place to repeat that.
  if (IsConfigWriteShielded()) return true;
  if (m_resourceDir.empty()) return false;

  JsonWriter w;
  w.BeginObject();
  w.Int(L"version", 1);
  w.Bool(L"enabled", m_bEnabled);

  JsonValue paths;
  paths.type = JsonValue::Array;
  for (const auto& p : m_searchPaths) paths.elements.push_back(JsonValue(p));
  w.Value(L"searchPaths", paths);

  w.BeginObject(L"replacements");
  for (const auto& [key, path] : m_explicit)
    w.String(key.c_str(), path);
  w.EndObject();

  for (const auto& [key, val] : m_unknownRoot)
    w.Value(key.c_str(), val);

  w.EndObject();
  return w.SaveToFile(StorePath().c_str());
}

PresetReplacementStore& PresetReplacements() {
  static PresetReplacementStore s;
  return s;
}

}  // namespace mdrop
