// Copyright 2026 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CEF_LIBCEF_BROWSER_HODOS_FARBLING_REGISTRY_H_
#define CEF_LIBCEF_BROWSER_HODOS_FARBLING_REGISTRY_H_
#pragma once

#include <map>
#include <string>

// Needed in the HEADER, not just the .cc: the friend declaration below names
// base::NoDestructor<>, so the template must be visible here.
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"

namespace hodos {

// Browser-process store for per-site fingerprint-farbling material, sitting
// between the shell (which computes it) and the renderer (which pulls it).
//
// WHY THIS EXISTS. The shell computes {key, enabled} in OnBeforeBrowse, i.e.
// pre-commit. Delivering it to the renderer by PUSH is wrong by construction: the
// target document does not exist yet, and each document gets a fresh
// CefFrameImpl, so the renderer has nowhere durable to park it. The push instead
// becomes a cache FILL here, and the renderer PULLS at OnContextCreated -- the one
// moment that is both after the right document exists and before page script runs.
// This also removes the cross-process failure that broke the legacy seed path: the
// browser always holds the data no matter which renderer process the document
// lands in.
//
// KEYING. Entries are filed under the REGISTRABLE DOMAIN (eTLD+1) exactly as the
// shell computed it, and looked up by a committed document's host via longest
// dot-suffix match. Two reasons this is not a plain origin map:
//   * the key is HMAC(profile_seed, eTLD+1), so every host under a site shares one
//     key by design -- filing per origin would store redundant duplicates;
//   * it makes the lookup survive host-changing redirects within a site
//     (example.com -> www.example.com), which an origin key would miss and
//     silently fail closed on.
//
// ⚠️ DO NOT re-derive the registrable domain here with net::registry_controlled_
// domains. The authoritative reduction is FarblingPolicy::RegistrableDomain in the
// shell, which is a deliberately separate hand-rolled implementation (see its
// header for why it must not be de-duplicated against the cookie helper). If this
// side reduced independently the two could disagree and the lookup would miss.
// That is why the shell sends the registrable domain explicitly and this class
// only ever does suffix matching on a string it was given.
//
// ⚠️ SINGLE ACTIVE PROFILE. Entries are not profile-scoped, so this assumes one
// active profile per browser process. That assumption is already load-bearing in
// the shell -- FarblingPolicy::InitializeForProfile caches exactly one profile
// seed in-process -- so this inherits it rather than introducing it. If Hodos ever
// runs two profiles in one browser process, key these entries by profile first or
// profile A's key will be served to profile B's renderer.
class FarblingRegistry {
 public:
  static FarblingRegistry& GetInstance();

  FarblingRegistry(const FarblingRegistry&) = delete;
  FarblingRegistry& operator=(const FarblingRegistry&) = delete;

  // Files |key_hex| / |enabled| under |registrable_domain|. Overwrites any prior
  // entry: the newest verdict wins, which is what makes a mid-session Privacy
  // Shield toggle take effect on the next navigation. A malformed or empty
  // |registrable_domain| is dropped.
  void Set(const std::string& registrable_domain,
           const std::string& key_hex,
           bool enabled);

  // Resolves |host| to its entry by longest dot-suffix match, so
  // "accounts.google.com" finds the entry filed under "google.com". Returns false
  // when nothing matches, in which case the caller MUST NOT farble.
  bool Lookup(const std::string& host,
              std::string* out_key_hex,
              bool* out_enabled) const;

 private:
  // base::NoDestructor placement-news T from inside its own ctor, so it needs
  // access to ours. Standard Chromium idiom for a private-ctor singleton.
  friend class base::NoDestructor<FarblingRegistry>;

  FarblingRegistry();
  ~FarblingRegistry();

  struct Entry {
    std::string key_hex;
    bool enabled = false;
  };

  // Guards |entries_|. The fill runs on the UI thread (OnBeforeBrowse) and so
  // does the [Sync] mojo read, so a lock is not strictly required today -- it is
  // here so that a future caller on another thread cannot turn this into a data
  // race that only shows up as an occasionally-unfarbled page.
  mutable base::Lock lock_;

  // Keyed by registrable domain. Grows by one short entry per distinct site
  // visited in a session (tens of bytes each), so it is deliberately uncapped:
  // evicting would mean silently serving "no key" for a site the user is still
  // browsing, which reads as farbling randomly switching off.
  std::map<std::string, Entry> entries_;
};

}  // namespace hodos

#endif  // CEF_LIBCEF_BROWSER_HODOS_FARBLING_REGISTRY_H_
