// Copyright 2026 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cef/libcef/browser/hodos_farbling_registry.h"

#include "base/no_destructor.h"
#include "base/strings/string_util.h"

namespace hodos {

namespace {

// The key is 32 bytes, lowercase hex, no separators -- the format
// FarblingPolicy::EncodeHex produces.
constexpr size_t kKeyHexLength = 64;

bool IsLowercaseHex(const std::string& s) {
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace

// static
FarblingRegistry& FarblingRegistry::GetInstance() {
  static base::NoDestructor<FarblingRegistry> instance;
  return *instance;
}

FarblingRegistry::FarblingRegistry() = default;
FarblingRegistry::~FarblingRegistry() = default;

void FarblingRegistry::Set(const std::string& registrable_domain,
                           const std::string& key_hex,
                           bool enabled) {
  if (registrable_domain.empty()) {
    return;
  }

  // Validate the key here rather than at pull time. Rejecting a malformed key
  // wholesale is deliberate: a half-decoded key is not a weaker secret, it is a
  // DIFFERENT and unpredictable fingerprint, and storing one would quietly defeat
  // the fail-closed contract the whole design rests on.
  if (key_hex.size() != kKeyHexLength || !IsLowercaseHex(key_hex)) {
    return;
  }

  // Hosts are compared case-sensitively on the lookup path, so normalise once on
  // the way in. The shell lowercases already; this makes it not matter.
  const std::string domain = base::ToLowerASCII(registrable_domain);

  base::AutoLock lock(lock_);
  entries_[domain] = Entry{key_hex, enabled};
}

bool FarblingRegistry::Lookup(const std::string& host,
                              std::string* out_key_hex,
                              bool* out_enabled) const {
  if (host.empty() || !out_key_hex || !out_enabled) {
    return false;
  }

  const std::string needle = base::ToLowerASCII(host);

  base::AutoLock lock(lock_);

  // Longest dot-suffix wins. The tie-break matters for public-suffix entries: if
  // both "github.io" and "user.github.io" are registrable domains in their own
  // right and both have been visited, "user.github.io" must not be served
  // "github.io"'s key -- they are separate first parties.
  const Entry* best = nullptr;
  size_t best_len = 0;

  for (const auto& [domain, entry] : entries_) {
    bool matches = false;
    if (needle == domain) {
      matches = true;
    } else if (needle.size() > domain.size() + 1 &&
               needle.compare(needle.size() - domain.size(), domain.size(),
                              domain) == 0 &&
               needle[needle.size() - domain.size() - 1] == '.') {
      matches = true;
    }

    if (matches && domain.size() > best_len) {
      best = &entry;
      best_len = domain.size();
    }
  }

  if (!best) {
    return false;
  }

  *out_key_hex = best->key_hex;
  *out_enabled = best->enabled;
  return true;
}

}  // namespace hodos
