// Copyright 2021 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cef/libcef/browser/browser_frame.h"

#include "cef/libcef/browser/browser_host_base.h"
#include "cef/libcef/browser/browser_info_manager.h"
#include "cef/libcef/browser/hodos_farbling_registry.h"
#include "cef/libcef/browser/thread_util.h"
#include "cef/libcef/common/frame_util.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

CefBrowserFrame::CefBrowserFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<cef::mojom::BrowserFrame> receiver)
    : CefFrameServiceBase(render_frame_host, std::move(receiver)) {
  DVLOG(1) << __func__ << ": frame "
           << frame_util::GetFrameDebugString(
                  render_frame_host->GetGlobalFrameToken())
           << " bound";
}

CefBrowserFrame::~CefBrowserFrame() = default;

// static
void CefBrowserFrame::RegisterBrowserInterfaceBindersForFrame(
    content::RenderFrameHost* render_frame_host,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  map->Add<cef::mojom::BrowserFrame>(base::BindRepeating(
      [](content::RenderFrameHost* frame_host,
         mojo::PendingReceiver<cef::mojom::BrowserFrame> receiver) {
        // This object is bound to the lifetime of |frame_host| and the mojo
        // connection. See DocumentServiceBase for details.
        new CefBrowserFrame(frame_host, std::move(receiver));
      }));
}

void CefBrowserFrame::SendMessage(const std::string& name,
                                  base::ListValue arguments) {
  // Always send to the newly created RFH, which may be speculative when
  // navigating cross-origin.
  if (auto host = GetFrameHost(/*prefer_speculative=*/true)) {
    host->SendMessage(name, std::move(arguments));
  }
}

std::string CefBrowserFrame::ResolveTopFrameHost() const {
  // P4e. Every frame on a page is keyed on the TOP frame's registrable domain --
  // Brave's model, and what PLAN_farbling_blink.md I4 specifies. Two reasons this is
  // resolved HERE and not in the renderer:
  //
  //   1. The renderer would have to derive the top-level site itself (e.g. from
  //      StorageKey().TopLevelSite()), which re-reduces the registrable domain through
  //      net::registry_controlled_domains. hodos_farbling_registry.h forbids that
  //      explicitly: the authoritative reduction is FarblingPolicy's hand-rolled one in
  //      the shell, and a disagreement between the two makes every lookup MISS -- i.e.
  //      fail closed, silently, which is indistinguishable from working correctly until
  //      someone measures it.
  //   2. A renderer must not be trusted about which site frames it.
  //
  // GetOutermostMainFrame(), not GetMainFrame(): the former escapes inner pages such as
  // fenced frames, so a fenced frame's children key on the embedding page rather than on
  // the fenced root. "Top frame" in the privacy sense means what the USER sees in the
  // omnibox.
  content::RenderFrameHost* rfh = render_frame_host();
  if (!rfh) {
    return std::string();
  }

  content::RenderFrameHost* top = rfh->GetOutermostMainFrame();
  if (!top) {
    return std::string();
  }

  // ⛔ ORIGIN-INHERITING TOP FRAMES ARE THE SECOND HALF OF THE BYPASS.
  //
  // `w = window.open()` produces a TOP frame whose committed URL is "about:blank". It
  // inherits its opener's origin and is fully scriptable from the opener, so a page reads
  // native canvas/WebGL/audio/navigator out of it exactly as it could through a
  // same-origin iframe. Resolving only the frame tree would leave that wide open, and the
  // iframe fix alone would have shipped as "bypass closed" while it stayed live --
  // measured on Windows 2026-08-13, both vectors native.
  //
  // Walking the opener chain covers precisely the exploitable set, because the bypass
  // needs a SCRIPTABLE handle and that is what an opener relationship is. A `noopener`
  // popup has no opener here and window.open() returned null there, so nobody can read it
  // either -- it correctly falls through to "no key".
  //
  // The depth cap is a cycle/abuse guard, not a semantic limit: opener chains are
  // attacker-controllable in length, and this runs on a blocking sync call.
  constexpr int kMaxOpenerDepth = 8;
  for (int depth = 0; depth < kMaxOpenerDepth; ++depth) {
    const GURL url = top->GetLastCommittedURL();
    if (url.SchemeIsHTTPOrHTTPS()) {
      return std::string(url.host());
    }

    content::WebContents* contents =
        content::WebContents::FromRenderFrameHost(top);
    if (!contents) {
      break;
    }
    content::RenderFrameHost* opener = contents->GetOpener();
    if (!opener) {
      break;
    }
    content::RenderFrameHost* opener_top = opener->GetOutermostMainFrame();
    if (!opener_top || opener_top == top) {
      break;
    }
    top = opener_top;
  }

  // No committed HTTP(S) first party anywhere up the chain: devtools://, file://, a
  // data: top frame, or the initial empty document of a fresh tab. Nothing to key on,
  // and the registry would miss anyway.
  return std::string();
}

void CefBrowserFrame::GetHodosFarblingKey(
    const std::string& host,
    cef::mojom::BrowserFrame::GetHodosFarblingKeyCallback callback) {
  const std::string top_host = ResolveTopFrameHost();

  if (top_host.empty()) {
    std::move(callback).Run(std::string(), false);
    return;
  }

  // |host| is what the renderer believes it is; |top_host| is what the browser knows.
  // They differ legitimately for every subframe, so only log when a MAIN frame disagrees
  // with itself -- that would mean the renderer's document URL and the browser's
  // last-committed URL have diverged, which is the one case where P4e could change
  // main-frame behaviour. Cheap standing tripwire for the T2 regression.
  if (!host.empty() && host != top_host && render_frame_host() &&
      !render_frame_host()->GetParentOrOuterDocument()) {
    LOG(WARNING) << "Hodos: main-frame host mismatch; renderer said '" << host
                 << "', browser resolved '" << top_host << "' -- using the latter";
  }

  std::string key_hex;
  bool enabled = false;
  if (!hodos::FarblingRegistry::GetInstance().Lookup(top_host, &key_hex,
                                                     &enabled)) {
    // Nothing filed for this host. Reply with an empty key rather than dropping
    // the callback: the renderer is blocked on this sync call, and it needs a
    // definite "no key" to fail closed on. Never invent a key here -- a constant
    // or zero key is a WORSE fingerprint than not farbling at all.
    std::move(callback).Run(std::string(), false);
    return;
  }
  std::move(callback).Run(key_hex, enabled);
}

void CefBrowserFrame::SendSharedMemoryRegion(
    const std::string& name,
    base::WritableSharedMemoryRegion region) {
  // Always send to the newly created RFH, which may be speculative when
  // navigating cross-origin.
  if (auto host = GetFrameHost(/*prefer_speculative=*/true)) {
    host->SendSharedMemoryRegion(name, std::move(region));
  }
}

void CefBrowserFrame::FrameAttached(
    mojo::PendingRemote<cef::mojom::RenderFrame> render_frame,
    bool reattached) {
  // Always send to the newly created RFH, which may be speculative when
  // navigating cross-origin.
  bool is_excluded;
  if (auto host = GetFrameHost(/*prefer_speculative=*/true, &is_excluded)) {
    host->FrameAttached(std::move(render_frame), reattached);
  } else if (is_excluded) {
    DVLOG(1) << __func__ << ": frame "
             << frame_util::GetFrameDebugString(
                    render_frame_host()->GetGlobalFrameToken())
             << " attach denied";
    mojo::Remote<cef::mojom::RenderFrame> render_frame_remote;
    render_frame_remote.Bind(std::move(render_frame));
    render_frame_remote->FrameAttachedAck(/*allow=*/false);
    render_frame_remote.ResetWithReason(
        static_cast<uint32_t>(frame_util::ResetReason::kExcluded), "Excluded");
  }
}

void CefBrowserFrame::UpdateDraggableRegions(
    std::optional<std::vector<cef::mojom::DraggableRegionEntryPtr>> regions) {
  if (auto host = GetFrameHost(/*prefer_speculative=*/false)) {
    host->UpdateDraggableRegions(std::move(regions));
  }
}

CefRefPtr<CefFrameHostImpl> CefBrowserFrame::GetFrameHost(
    bool prefer_speculative,
    bool* is_excluded) const {
  return CefBrowserInfoManager::GetFrameHost(
      render_frame_host(), prefer_speculative,
      /*browser_info=*/nullptr, is_excluded);
}
