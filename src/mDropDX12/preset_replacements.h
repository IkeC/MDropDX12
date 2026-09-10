// preset_replacements.h — run a different preset file than the one asked for.
//
// MilkDrop2077's presets carry an `MD31=<hash>` or `MD32=<hash>` line in their
// header. It is not a shader and not a pointer to another file: it is a key into
// MD3 PRO's own cache of precompiled proprietary shaders, and PRO renders that
// cached shader INSTEAD of compiling the shader text in the file. The file
// therefore ships a stub -- `Intestines of the Beast2.milk` has 11 warp_ lines
// where the real shader has 96 -- and any engine without that cache compiles the
// stub and draws a black frame. Nothing is wrong with the renderer; the preset
// is only half present.
//
// Hand-written replacements exist for the whole affected corpus. This store is
// what makes one of them load in place of the original, so a preset that cannot
// work renders the version that does.
//
// Resolution order, applied only to a preset that actually carries an MD3x key:
//
//   1. The preset's own `replacementPreset` annotation, if it has one. That is
//      the per-preset designation made from the Annotations window.
//   2. An explicit path recorded here for that KEY VALUE. Keyed on the value
//      rather than the filename so it survives a rename or a re-download --
//      the key travels inside the file.
//   3. The first configured search path holding a file of the same bare name.
//      This is what covers a whole corpus with one directory and no table,
//      because the replacements were built alongside the originals and share
//      their names.
//   4. Nothing. The original loads and the user is told why it may look wrong.
//
// A preset with no MD3x key never reaches any of this.

#pragma once

#include <string>
#include <vector>

namespace mdrop {

// Defined in json_utils.h, which this header deliberately does not include:
// the tag must match that definition exactly (struct, not class) or every TU
// seeing both warns C4099. Same rule as shader_overrides.h.
struct JsonValue;

class PresetReplacementStore {
public:
  // resourceDir is the directory holding preset_replacements.json
  // (m_szMilkdrop2Path). It ALREADY ends in a backslash -- do not add one.
  bool Load(const wchar_t* resourceDir);
  bool Save() const;

  bool IsEnabled() const { return m_bEnabled; }
  void SetEnabled(bool b) { m_bEnabled = b; }
  bool IsLoaded() const { return m_bLoaded; }

  // The MD3x key a preset carries, as "MD31:<value>" / "MD32:<value>", or empty
  // when it carries neither.
  //
  // Reads the head of the file only. CState::Import already parses these keys
  // (state.cpp) but throws the value away and keeps a bool, and it runs on the
  // background load thread AFTER the type routing has committed -- far too late
  // to choose a different file. Hence a standalone probe.
  //
  // The two spaces are namespaced because they are not interchangeable: one
  // value is already shared between two files in the shipped corpus.
  static std::wstring KeyForPreset(const wchar_t* presetPath);

  // The file that should render instead of presetPath, or empty for "load the
  // original". `key` is what KeyForPreset returned; pass it in rather than
  // re-probing, since the caller has already paid for it.
  //
  // Only returns a path that exists on disk: a dangling replacement is reported
  // as no replacement, and the caller says so, rather than failing the load.
  std::wstring Resolve(const wchar_t* presetPath, const std::wstring& key,
                       std::wstring* whyNot = nullptr) const;

  const std::vector<std::wstring>& SearchPaths() const { return m_searchPaths; }
  void SetSearchPaths(std::vector<std::wstring> paths) {
    m_searchPaths = std::move(paths);
  }

  // The explicit per-key entries. Empty path removes the entry.
  std::wstring ExplicitFor(const std::wstring& key) const;
  void SetExplicit(const std::wstring& key, const std::wstring& path);
  const std::vector<std::pair<std::wstring, std::wstring>>& Explicits() const {
    return m_explicit;
  }

private:
  std::wstring StorePath() const;

  std::wstring m_resourceDir;
  std::vector<std::wstring> m_searchPaths;                        // ORDER IS PRIORITY
  std::vector<std::pair<std::wstring, std::wstring>> m_explicit;  // key -> full path
  bool m_bEnabled = true;
  bool m_bLoaded = false;

  // Members this build did not recognise, re-emitted on save so a newer format
  // is not erased by an older build. Same preservation shaderoverrides.json uses.
  std::vector<std::pair<std::wstring, JsonValue>> m_unknownRoot;
};

// The process-wide store. Reached through a function rather than a member on
// Engine so that engine.h needs neither this header nor any JSON type.
PresetReplacementStore& PresetReplacements();

}  // namespace mdrop
