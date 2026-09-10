// pin_auth.h — hashing and verification for the TCP remote's PIN.
//
// The PIN gates the TCP remote: the Android client and any raw IPC caller. It
// used to be stored verbatim under a key named PinHash and never checked at
// all. That was a deliberate deferral while the TCP stack was still being
// stabilised, not an oversight -- see forgejo#44.
//
// Stored form is "v1$<salt-hex>$<sha256-hex>": a 16-byte random salt, and
// SHA-256 over salt || utf8(pin). A stored value that does not begin with
// "v1$" is a legacy plaintext PIN. Those still verify, so upgrading locks
// nobody out, and the caller rewrites the value as a hash on the first
// successful check.
//
// Every comparison here runs over fixed-length digests in constant time, so
// neither the length nor the content of the stored PIN leaks through timing.
#pragma once

#include <string>

namespace mdrop {

// "v1$<salt-hex>$<sha256-hex>" for a PIN. Returns empty for an empty PIN, so
// clearing the field in the Remote window still clears the setting, and empty
// on failure of the system RNG or hash, which the caller must treat as "do not
// store" rather than "store nothing".
std::wstring PinHashCreate(const std::wstring& pin);

// True when stored holds a pre-hash plaintext PIN rather than a v1 hash.
bool PinHashIsLegacy(const std::wstring& stored);

// Constant-time check of a submitted PIN against either form. Returns false
// for an empty or malformed stored value, so corruption and misuse both deny
// rather than admit; callers gate on the PIN being configured at all.
bool PinVerify(const std::wstring& submitted, const std::wstring& stored);

}  // namespace mdrop
