#include "pin_auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace mdrop {

namespace {

const wchar_t* const kPrefix = L"v1$";
const size_t kPrefixLen = 3;
const size_t kSaltBytes = 16;
const size_t kHashBytes = 32;

std::wstring ToHex(const unsigned char* p, size_t n) {
  static const wchar_t* const kDigits = L"0123456789abcdef";
  std::wstring out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    out.push_back(kDigits[p[i] >> 4]);
    out.push_back(kDigits[p[i] & 0x0f]);
  }
  return out;
}

int HexVal(wchar_t c) {
  if (c >= L'0' && c <= L'9') return c - L'0';
  if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
  if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
  return -1;
}

bool FromHex(const std::wstring& s, std::vector<unsigned char>& out) {
  if (s.empty() || (s.size() % 2) != 0) return false;
  out.clear();
  out.reserve(s.size() / 2);
  for (size_t i = 0; i < s.size(); i += 2) {
    const int hi = HexVal(s[i]);
    const int lo = HexVal(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((unsigned char)((hi << 4) | lo));
  }
  return true;
}

std::vector<unsigned char> Utf8Bytes(const std::wstring& s) {
  std::vector<unsigned char> out;
  if (s.empty()) return out;
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return out;
  out.resize((size_t)n);
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                      (char*)out.data(), n, nullptr, nullptr);
  return out;
}

bool Sha256(const std::vector<unsigned char>& data, unsigned char out[kHashBytes]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM,
                                                  nullptr, 0)))
    return false;
  // An empty input is legitimate here; BCryptHash accepts a null pointer only
  // with a zero length, so hand it a valid one either way.
  unsigned char dummy = 0;
  const NTSTATUS st = BCryptHash(alg, nullptr, 0,
                                 data.empty() ? &dummy : (PUCHAR)data.data(),
                                 (ULONG)data.size(), out, (ULONG)kHashBytes);
  BCryptCloseAlgorithmProvider(alg, 0);
  return BCRYPT_SUCCESS(st);
}

// Fixed-length compare with no early exit, so a near-miss costs the same as a
// wild miss.
bool EqualConstantTime(const unsigned char* a, const unsigned char* b, size_t n) {
  unsigned char diff = 0;
  for (size_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

bool HashSalted(const std::vector<unsigned char>& salt, const std::wstring& pin,
                unsigned char out[kHashBytes]) {
  std::vector<unsigned char> buf(salt);
  const std::vector<unsigned char> pinBytes = Utf8Bytes(pin);
  buf.insert(buf.end(), pinBytes.begin(), pinBytes.end());
  return Sha256(buf, out);
}

}  // namespace

std::wstring PinHashCreate(const std::wstring& pin) {
  if (pin.empty()) return std::wstring();

  std::vector<unsigned char> salt(kSaltBytes, 0);
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, salt.data(), (ULONG)kSaltBytes,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
    return std::wstring();

  unsigned char hash[kHashBytes] = {};
  if (!HashSalted(salt, pin, hash)) return std::wstring();

  return std::wstring(kPrefix) + ToHex(salt.data(), kSaltBytes) + L"$" +
         ToHex(hash, kHashBytes);
}

bool PinHashIsLegacy(const std::wstring& stored) {
  if (stored.empty()) return false;
  return stored.compare(0, kPrefixLen, kPrefix) != 0;
}

bool PinVerify(const std::wstring& submitted, const std::wstring& stored) {
  // Fail closed. Callers decide whether a PIN is configured at all; reaching
  // here with nothing stored is misuse, and denying is the safe answer.
  if (stored.empty()) return false;

  if (PinHashIsLegacy(stored)) {
    // Pre-hash value, held verbatim. Compare digests rather than the strings
    // so the stored PIN's length does not leak through the comparison.
    unsigned char got[kHashBytes] = {};
    unsigned char want[kHashBytes] = {};
    if (!Sha256(Utf8Bytes(submitted), got)) return false;
    if (!Sha256(Utf8Bytes(stored), want)) return false;
    return EqualConstantTime(got, want, kHashBytes);
  }

  const size_t sep = stored.find(L'$', kPrefixLen);
  if (sep == std::wstring::npos) return false;

  std::vector<unsigned char> salt;
  std::vector<unsigned char> want;
  if (!FromHex(stored.substr(kPrefixLen, sep - kPrefixLen), salt)) return false;
  if (!FromHex(stored.substr(sep + 1), want)) return false;
  if (salt.size() != kSaltBytes || want.size() != kHashBytes) return false;

  unsigned char got[kHashBytes] = {};
  if (!HashSalted(salt, submitted, got)) return false;
  return EqualConstantTime(got, want.data(), kHashBytes);
}

}  // namespace mdrop
