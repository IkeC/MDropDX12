// kv_store.h — a section/key store backed by json instead of an INI.
//
// Exists to move a family of sections out of settings.ini WITHOUT rewriting the
// code that reads and writes them. It offers the handful of ConfigStore methods
// that code actually uses -- GetInt, GetBool, GetFloat, GetStringTo, SetString,
// RemoveSection -- with the same signatures and the same meaning, so a
// migration is a swap of which object is called and nothing else.
//
// That mattered for the displays. Their loader maps some sixty fields across
// two output types and decides whether a child process takes over a real
// monitor; rewriting it field by field to speak json would have put a
// misread flag between the user and his screens, and that exact fault has
// already happened twice here. Swapping the store leaves the mapping untouched.
//
// Values are stored as json numbers when they look like numbers and strings
// otherwise, so the file reads as a person would write it -- "Enabled": 1, not
// "Enabled": "1" -- while SetString stays the only writer the callers need.
//
// THREAD SAFE. Tool windows and the render thread both reach these stores.
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "json_utils.h"

namespace mdrop {

class KvStore {
public:
    // The file this store lives in. Setting a different path drops whatever is
    // cached, so the next read comes from the new file.
    void SetPath(const std::wstring& path);
    std::wstring Path() const;

    bool  Has(const wchar_t* section, const wchar_t* key);
    int   GetInt(const wchar_t* section, const wchar_t* key, int fallback);
    bool  GetBool(const wchar_t* section, const wchar_t* key, bool fallback);
    float GetFloat(const wchar_t* section, const wchar_t* key, float fallback);
    // Same contract as ConfigStore::GetStringTo: writes at most cch characters
    // including the terminator, and falls back when the key is absent.
    void  GetStringTo(const wchar_t* section, const wchar_t* key,
                      const wchar_t* fallback, wchar_t* out, size_t cch);

    void  SetString(const wchar_t* section, const wchar_t* key, const wchar_t* value);
    void  SetInt(const wchar_t* section, const wchar_t* key, int value);

    std::vector<std::wstring> SectionNames();
    void RemoveSection(const wchar_t* section);

    // Writes now. Every setter already flushes, so this is for a caller that
    // wants to be sure after a batch.
    bool Flush();

private:
    void Load();                 // call with m_mx held
    bool SaveLocked();           // call with m_mx held
    const JsonValue* Find(const wchar_t* section, const wchar_t* key);   // m_mx held

    mutable std::mutex m_mx;
    std::wstring m_path;
    JsonValue    m_root;
    bool         m_loaded = false;
};

// Display outputs and the global mirror flags: resources/displays.json.
// Was [DisplayOutputs] plus [DisplayOutput_0..N] in settings.ini -- 62 keys.
KvStore& DisplayCfg();

}  // namespace mdrop
