// kv_store.cpp — see kv_store.h for why this exists.

#include "kv_store.h"

#include <windows.h>

#include <cstdlib>
#include <cwchar>

#include "config_store.h"
#include "utility.h"
#include "format_to.h"

namespace mdrop {

namespace {

// json_utils has no get-or-create helper; vfx_profile_store keeps its own for
// the same reason. Small enough to have twice.
JsonValue& Member(JsonValue& obj, const wchar_t* key) {
    obj.type = JsonValue::Object;
    for (auto& kv : obj.members)
        if (kv.first == key) return kv.second;
    obj.members.push_back({ key, JsonValue() });
    return obj.members.back().second;
}

// Whether the whole string is an integer, so it can be stored as a number
// rather than a quoted one. Deliberately strict: a leading zero, a sign on its
// own, or any trailing character means "string", because a device name that
// happens to read as a number must come back out as what went in.
bool LooksLikeInt(const wchar_t* s) {
    if (!s || !*s) return false;
    const wchar_t* p = s;
    if (*p == L'-') p++;
    if (!*p) return false;
    for (; *p; p++)
        if (*p < L'0' || *p > L'9') return false;
    return true;
}

// Deliberately no float counterpart.
//
// A float does not round-trip through a json number: "-1.000" is read back as
// "-1". The displays migration compares what it wrote against what it reads to
// decide whether it may delete the INI section, and it correctly refused to.
// Anything that is not plainly an integer is kept as a string, which does
// round-trip exactly; GetFloat parses a stored string, so nothing downstream
// notices the difference.

}  // namespace

void KvStore::SetPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_path == path) return;
    m_path = path;
    m_loaded = false;
}

std::wstring KvStore::Path() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_path;
}

void KvStore::Load() {
    if (m_loaded) return;
    m_loaded = true;
    m_root = JsonValue();
    m_root.type = JsonValue::Object;
    if (m_path.empty()) return;
    try {
        JsonValue v = JsonLoadFile(m_path.c_str());
        if (v.isObject()) m_root = v;
    } catch (...) {
        DLOG_ERROR("kv: %ls is malformed; starting empty", m_path.c_str());
    }
}

const JsonValue* KvStore::Find(const wchar_t* section, const wchar_t* key) {
    Load();
    if (!section || !key) return nullptr;
    const JsonValue& sec = m_root[section];
    if (!sec.isObject() || !sec.has(key)) return nullptr;
    return &sec[key];
}

bool KvStore::Has(const wchar_t* section, const wchar_t* key) {
    std::lock_guard<std::mutex> lk(m_mx);
    return Find(section, key) != nullptr;
}

int KvStore::GetInt(const wchar_t* section, const wchar_t* key, int fallback) {
    std::lock_guard<std::mutex> lk(m_mx);
    const JsonValue* v = Find(section, key);
    if (!v) return fallback;
    // A value stored as a string still has to read as a number: a file written
    // by an older build, or edited by hand, is not wrong.
    if (v->isString()) return _wtoi(v->asString().c_str());
    return v->asInt(fallback);
}

bool KvStore::GetBool(const wchar_t* section, const wchar_t* key, bool fallback) {
    std::lock_guard<std::mutex> lk(m_mx);
    const JsonValue* v = Find(section, key);
    if (!v) return fallback;
    if (v->isBool()) return v->asBool(fallback);
    if (v->isString()) return _wtoi(v->asString().c_str()) != 0;
    return v->asInt(fallback ? 1 : 0) != 0;
}

float KvStore::GetFloat(const wchar_t* section, const wchar_t* key, float fallback) {
    std::lock_guard<std::mutex> lk(m_mx);
    const JsonValue* v = Find(section, key);
    if (!v) return fallback;
    if (v->isString()) return (float)_wtof(v->asString().c_str());
    return v->asFloat(fallback);
}

void KvStore::GetStringTo(const wchar_t* section, const wchar_t* key,
                          const wchar_t* fallback, wchar_t* out, size_t cch) {
    if (!out || cch == 0) return;
    std::lock_guard<std::mutex> lk(m_mx);
    const JsonValue* v = Find(section, key);
    std::wstring text;
    if (!v) {
        text = fallback ? fallback : L"";
    } else if (v->isString()) {
        text = v->asString();
    } else {
        // Stored as a number because it looked like one. Render it back, so a
        // caller that asked for a string gets exactly what it wrote.
        wchar_t buf[64];
        const double d = v->asFloat(0.0f);
        if (d == (double)(int)d) FormatTo(buf, L"%d", (int)d);
        else                     FormatTo(buf, L"%g", d);
        text = buf;
    }
    wcsncpy_s(out, cch, text.c_str(), _TRUNCATE);
}

void KvStore::SetString(const wchar_t* section, const wchar_t* key,
                        const wchar_t* value) {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        Load();
        JsonValue& sec = Member(m_root, section);
        sec.type = JsonValue::Object;
        const wchar_t* v = value ? value : L"";
        if (LooksLikeInt(v)) Member(sec, key) = JsonValue(_wtoi(v));
        else                 Member(sec, key) = JsonValue(std::wstring(v));
    }
    Flush();
}

void KvStore::SetInt(const wchar_t* section, const wchar_t* key, int value) {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        Load();
        JsonValue& sec = Member(m_root, section);
        sec.type = JsonValue::Object;
        Member(sec, key) = JsonValue(value);
    }
    Flush();
}

std::vector<std::wstring> KvStore::SectionNames() {
    std::lock_guard<std::mutex> lk(m_mx);
    Load();
    std::vector<std::wstring> out;
    for (const auto& kv : m_root.members)
        if (kv.second.isObject()) out.push_back(kv.first);
    return out;
}

void KvStore::RemoveSection(const wchar_t* section) {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        Load();
        for (size_t i = 0; i < m_root.members.size(); i++) {
            if (_wcsicmp(m_root.members[i].first.c_str(), section) != 0) continue;
            m_root.members.erase(m_root.members.begin() + (ptrdiff_t)i);
            break;
        }
    }
    Flush();
}

bool KvStore::SaveLocked() {
    if (m_path.empty()) return false;
    JsonWriter w;
    w.BeginObject();
    for (const auto& kv : m_root.members) w.Value(kv.first.c_str(), kv.second);
    w.EndObject();
    return w.SaveToFile(m_path.c_str());
}

bool KvStore::Flush() {
    // Testing mode leaves no trace on disk, exactly as it does for settings.ini
    // and mixer.json. This one matters more than most: a sweep must not be able
    // to write display state back over the user's real monitor arrangement.
    if (IsConfigWriteShielded()) return true;
    std::lock_guard<std::mutex> lk(m_mx);
    return SaveLocked();
}

KvStore& DisplayCfg() {
    static KvStore s;
    return s;
}

}  // namespace mdrop
