// Copyright (c) 2012 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.

#include "cef/libcef/renderer/frame_impl.h"

#include <array>
#include <cstdio>
#include <map>

#include "build/build_config.h"

// Enable deprecation warnings on Windows. See http://crbug.com/585142.
#if BUILDFLAG(IS_WIN)
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wdeprecated-declarations"
#else
#pragma warning(push)
#pragma warning(default : 4996)
#endif
#endif

#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/unguessable_token.h"
#include "cef/include/cef_urlrequest.h"
#include "cef/libcef/common/app_manager.h"
#include "cef/libcef/common/frame_util.h"
#include "cef/libcef/common/net/http_header_utils.h"
#include "cef/libcef/common/process_message_impl.h"
#include "cef/libcef/common/process_message_smr_impl.h"
#include "cef/libcef/common/request_impl.h"
#include "cef/libcef/common/string_util.h"
#include "cef/libcef/renderer/blink_glue.h"
#include "cef/libcef/renderer/browser_impl.h"
#include "cef/libcef/renderer/dom_document_impl.h"
#include "cef/libcef/renderer/render_frame_util.h"
#include "cef/libcef/renderer/thread_util.h"
#include "cef/libcef/renderer/v8_impl.h"
#include "content/renderer/render_frame_impl.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/platform/web_data.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "third_party/blink/public/web/blink.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_document_loader.h"
#include "third_party/blink/public/web/web_frame.h"
#include "third_party/blink/public/web/web_frame_content_dumper.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_navigation_control.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

namespace {

// Hodos C2: browser -> renderer farbling key. Consumed inside libcef and never
// surfaced to the client; see CefFrameImpl::SendMessage.
constexpr char kHodosFarblingKeyMessage[] = "hodos_farble_key";

// Hodos P4e: one renderer-process-wide memo entry, keyed by the top frame's token.
// |has_key| false is a real, memoisable answer ("the browser has nothing for this top
// frame"), NOT an error -- errors are deliberately never memoised, so a dropped pipe
// cannot silently unfarble a whole document's worth of frames.
struct HodosFarblingMemoEntry {
  bool has_key = false;
  bool enabled = false;
  std::array<uint8_t, 32> key{};
};

// Maximum number of times to retry the browser connection.
constexpr size_t kConnectionRetryMaxCt = 3U;

// Length of time to wait before initiating a browser connection retry. The
// short value is optimized for navigation-related disconnects (time delta
// between CefFrameImpl::OnDisconnect and CefFrameHostImpl::MaybeReAttach) which
// should take << 10ms in normal circumstances (reasonably fast machine, limited
// redirects). The long value is optimized for slower machines or navigations
// with many redirects to reduce overall failure rates. See related comments in
// CefFrameImpl::OnDisconnect.
constexpr auto kConnectionRetryDelayShort = base::Milliseconds(25);
constexpr auto kConnectionRetryDelayLong = base::Seconds(3);

std::string GetDebugString(blink::WebLocalFrame* frame) {
  return "frame " + render_frame_util::GetIdentifier(frame);
}

v8::Isolate* GetFrameIsolate(blink::WebLocalFrame* frame) {
  return frame->GetAgentGroupScheduler()->Isolate();
}

}  // namespace

CefFrameImpl::CefFrameImpl(CefBrowserImpl* browser, blink::WebLocalFrame* frame)
    : browser_(browser),
      frame_(frame),
      frame_debug_str_(GetDebugString(frame)) {}

CefFrameImpl::~CefFrameImpl() = default;

bool CefFrameImpl::IsValid() {
  CEF_REQUIRE_RT_RETURN(false);

  return (frame_ != nullptr);
}

void CefFrameImpl::Undo() {
  SendCommand("Undo");
}

void CefFrameImpl::Redo() {
  SendCommand("Redo");
}

void CefFrameImpl::Cut() {
  SendCommand("Cut");
}

void CefFrameImpl::Copy() {
  SendCommand("Copy");
}

void CefFrameImpl::Paste() {
  SendCommand("Paste");
}

void CefFrameImpl::PasteAndMatchStyle() {
  SendCommand("PasteAndMatchStyle");
}

void CefFrameImpl::Delete() {
  SendCommand("Delete");
}

void CefFrameImpl::SelectAll() {
  SendCommand("SelectAll");
}

void CefFrameImpl::ViewSource() {
  DCHECK(false) << "ViewSource cannot be called from the renderer process";
}

void CefFrameImpl::GetSource(CefRefPtr<CefStringVisitor> visitor) {
  CEF_REQUIRE_RT_RETURN_VOID();
  if (frame_) {
    CefString content;
    string_util::GetCefString(blink_glue::DumpDocumentMarkup(frame_), content);
    visitor->Visit(content);
  }
}

void CefFrameImpl::GetText(CefRefPtr<CefStringVisitor> visitor) {
  CEF_REQUIRE_RT_RETURN_VOID();
  if (frame_) {
    CefString content;
    string_util::GetCefString(blink_glue::DumpDocumentText(frame_), content);
    visitor->Visit(content);
  }
}

void CefFrameImpl::LoadRequest(CefRefPtr<CefRequest> request) {
  CEF_REQUIRE_RT_RETURN_VOID();

  if (!frame_) {
    return;
  }

  auto params = cef::mojom::RequestParams::New();
  static_cast<CefRequestImpl*>(request.get())->Get(params);
  LoadRequest(std::move(params));
}

void CefFrameImpl::LoadURL(const CefString& url) {
  CEF_REQUIRE_RT_RETURN_VOID();

  if (!frame_) {
    return;
  }

  auto params = cef::mojom::RequestParams::New();
  params->url = GURL(url.ToString());
  params->method = "GET";
  LoadRequest(std::move(params));
}

void CefFrameImpl::ExecuteJavaScript(const CefString& jsCode,
                                     const CefString& scriptUrl,
                                     int startLine) {
  SendJavaScript(jsCode, scriptUrl, startLine);
}

bool CefFrameImpl::IsMain() {
  CEF_REQUIRE_RT_RETURN(false);

  if (frame_) {
    return (frame_->Parent() == nullptr);
  }
  return false;
}

bool CefFrameImpl::IsFocused() {
  CEF_REQUIRE_RT_RETURN(false);

  if (frame_ && frame_->View()) {
    return (frame_->View()->FocusedFrame() == frame_);
  }
  return false;
}

CefString CefFrameImpl::GetName() {
  CefString name;
  CEF_REQUIRE_RT_RETURN(name);

  if (frame_) {
    name = render_frame_util::GetName(frame_);
  }
  return name;
}

CefString CefFrameImpl::GetIdentifier() {
  CefString identifier;
  CEF_REQUIRE_RT_RETURN(identifier);

  if (frame_) {
    identifier = render_frame_util::GetIdentifier(frame_);
  }
  return identifier;
}

CefRefPtr<CefFrame> CefFrameImpl::GetParent() {
  CEF_REQUIRE_RT_RETURN(nullptr);

  if (frame_) {
    blink::WebFrame* parent = frame_->Parent();
    if (parent && parent->IsWebLocalFrame()) {
      return browser_->GetWebFrameImpl(parent->ToWebLocalFrame()).get();
    }
  }

  return nullptr;
}

CefString CefFrameImpl::GetURL() {
  CefString url;
  CEF_REQUIRE_RT_RETURN(url);

  if (frame_) {
    GURL gurl = frame_->GetDocument().Url();
    url = gurl.spec();
  }
  return url;
}

CefRefPtr<CefBrowser> CefFrameImpl::GetBrowser() {
  CEF_REQUIRE_RT_RETURN(nullptr);

  return browser_;
}

CefRefPtr<CefV8Context> CefFrameImpl::GetV8Context() {
  CEF_REQUIRE_RT_RETURN(nullptr);

  if (frame_) {
    v8::Isolate* isolate = GetFrameIsolate(frame_);
    v8::HandleScope handle_scope(isolate);
    return new CefV8ContextImpl(isolate, frame_->MainWorldScriptContext());
  } else {
    return nullptr;
  }
}

void CefFrameImpl::VisitDOM(CefRefPtr<CefDOMVisitor> visitor) {
  CEF_REQUIRE_RT_RETURN_VOID();

  if (!frame_) {
    return;
  }

  // Create a CefDOMDocumentImpl object that is valid only for the scope of this
  // method.
  CefRefPtr<CefDOMDocumentImpl> documentImpl;
  const blink::WebDocument& document = frame_->GetDocument();
  if (!document.IsNull()) {
    documentImpl = new CefDOMDocumentImpl(browser_, frame_);
  }

  visitor->Visit(documentImpl.get());

  if (documentImpl.get()) {
    documentImpl->Detach();
  }
}

CefRefPtr<CefURLRequest> CefFrameImpl::CreateURLRequest(
    CefRefPtr<CefRequest> request,
    CefRefPtr<CefURLRequestClient> client) {
  DCHECK(false) << "CreateURLRequest cannot be called from the render process";
  return nullptr;
}

void CefFrameImpl::SendProcessMessage(CefProcessId target_process,
                                      CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_RT_RETURN_VOID();
  DCHECK_EQ(PID_BROWSER, target_process);
  DCHECK(message && message->IsValid());
  if (!message || !message->IsValid()) {
    return;
  }

  if (message->GetArgumentList() != nullptr) {
    // Invalidate the message object immediately by taking the argument list.
    auto argument_list =
        static_cast<CefProcessMessageImpl*>(message.get())->TakeArgumentList();
    SendToBrowserFrame(
        __FUNCTION__,
        base::BindOnce(
            [](const CefString& name, base::ListValue argument_list,
               const BrowserFrameType& render_frame) {
              render_frame->SendMessage(name, std::move(argument_list));
            },
            message->GetName(), std::move(argument_list)));
  } else {
    auto region =
        static_cast<CefProcessMessageSMRImpl*>(message.get())->TakeRegion();
    SendToBrowserFrame(
        __FUNCTION__,
        base::BindOnce(
            [](const CefString& name, base::WritableSharedMemoryRegion region,
               const BrowserFrameType& render_frame) {
              render_frame->SendSharedMemoryRegion(name, std::move(region));
            },
            message->GetName(), std::move(region)));
  }
}

void CefFrameImpl::OnWasShown() {
  if (browser_connection_state_ == ConnectionState::DISCONNECTED &&
      did_commit_provisional_load_) {
    // Reconnect a frame that has exited the bfcache. We ignore temporary
    // frames that have never called DidCommitProvisionalLoad.
    ConnectBrowserFrame(ConnectReason::WAS_SHOWN);
  }
}

void CefFrameImpl::OnDidCommitProvisionalLoad() {
  did_commit_provisional_load_ = true;
  if (browser_connection_state_ == ConnectionState::DISCONNECTED) {
    // Connect after RenderFrameImpl::DidCommitNavigation has potentially
    // reset the BrowserInterfaceBroker in the browser process. See related
    // comments in OnDisconnect.
    ConnectBrowserFrame(ConnectReason::DID_COMMIT);
  }
  MaybeInitializeScriptContext();
}

void CefFrameImpl::OnDidFinishLoad() {
  // Ignore notifications from the embedded frame hosting a mime-type plugin.
  // We'll eventually receive a notification from the owner frame.
  if (blink_glue::HasPluginFrameOwner(frame_)) {
    return;
  }

  if (!blink::RuntimeEnabledFeatures::BackForwardCacheEnabled() && IsMain()) {
    // Refresh draggable regions. Otherwise, we may not receive updated regions
    // after navigation because LocalFrameView::UpdateDocumentAnnotatedRegion
    // lacks sufficient context. When bfcache is disabled we use this method
    // instead of DidStopLoading() because it provides more accurate timing.
    OnDraggableRegionsChanged();
  }

  blink::WebDocumentLoader* dl = frame_->GetDocumentLoader();
  const int http_status_code = dl->GetWebResponse().HttpStatusCode();

  CefRefPtr<CefApp> app = CefAppManager::Get()->GetApplication();
  if (app) {
    CefRefPtr<CefRenderProcessHandler> handler = app->GetRenderProcessHandler();
    if (handler) {
      CefRefPtr<CefLoadHandler> load_handler = handler->GetLoadHandler();
      if (load_handler) {
        load_handler->OnLoadEnd(browser_, this, http_status_code);
      }
    }
  }
}

void CefFrameImpl::OnDraggableRegionsChanged() {
  // Only the main frame is allowed to control draggable regions, to avoid other
  // frames trying to manipulate the regions in the browser process.
  if (frame_->Parent() != nullptr) {
    return;
  }

  auto webregions = frame_->GetDocument().DraggableRegions();
  std::vector<cef::mojom::DraggableRegionEntryPtr> regions;
  if (!webregions.empty()) {
    auto render_frame = content::RenderFrameImpl::FromWebFrame(frame_);

    regions.reserve(webregions.size());
    for (const auto& webregion : webregions) {
      auto region = cef::mojom::DraggableRegionEntry::New(
          render_frame->ConvertViewportToWindow(webregion.bounds),
          webregion.draggable);
      regions.emplace_back(std::move(region));
    }
  }

  using RegionsArg =
      std::optional<std::vector<cef::mojom::DraggableRegionEntryPtr>>;
  RegionsArg regions_arg =
      regions.empty() ? std::nullopt : std::make_optional(std::move(regions));

  SendToBrowserFrame(
      __FUNCTION__,
      base::BindOnce(
          [](RegionsArg regions_arg, const BrowserFrameType& browser_frame) {
            browser_frame->UpdateDraggableRegions(std::move(regions_arg));
          },
          std::move(regions_arg)));
}

void CefFrameImpl::OnContextCreated(v8::Local<v8::Context> context) {
  context_created_ = true;

  CHECK(frame_);

  // Hodos C2: install this document's farbling key before any page script runs.
  // This is a PULL, and it has to be -- see MaybeApplyHodosFarblingKey().
  MaybeApplyHodosFarblingKey();

  while (!queued_context_actions_.empty()) {
    auto& action = queued_context_actions_.front();
    std::move(action.second).Run(frame_);
    queued_context_actions_.pop();
  }

  execution_context_lifecycle_state_observer_ =
      blink_glue::RegisterExecutionContextLifecycleStateObserver(context, this);
}

void CefFrameImpl::OnContextReleased() {
  execution_context_lifecycle_state_observer_.reset();
}

void CefFrameImpl::OnDetached() {
  // Called when this frame has been detached from the view. This *will* be
  // called for child frames when a parent frame is detached.
  // The browser may hold the last reference to |this|. Take a reference here to
  // keep |this| alive until after this method returns.
  CefRefPtr<CefFrameImpl> self = this;

  browser_->FrameDetached(frame_);
  frame_ = nullptr;

  OnDisconnect(DisconnectReason::DETACHED, 0, std::string(), MOJO_RESULT_OK);

  browser_ = nullptr;

  // In case we never attached.
  while (!queued_browser_actions_.empty()) {
    auto& action = queued_browser_actions_.front();
    LOG(WARNING) << action.first << " sent to detached " << frame_debug_str_
                 << " will be ignored";
    queued_browser_actions_.pop();
  }

  // In case we're destroyed without the context being created.
  while (!queued_context_actions_.empty()) {
    auto& action = queued_context_actions_.front();
    LOG(WARNING) << action.first << " sent to detached " << frame_debug_str_
                 << " will be ignored";
    queued_context_actions_.pop();
  }
}

void CefFrameImpl::MaybeApplyHodosFarblingKey() {
  // Ask the BROWSER for this document's farbling key, synchronously, at the one
  // moment that works.
  //
  // WHY A PULL AND NOT A PUSH -- both push directions are broken by construction,
  // and both were tried:
  //   * PRE-COMMIT push (the shell's OnBeforeBrowse send): the document that needs
  //     the key does not exist yet, so it lands on the OUTGOING document. And it
  //     cannot be parked for the next one either, because each document gets a NEW
  //     CefFrameImpl -- proven by frame tokens changing per document.
  //   * POST-COMMIT push: SendToBrowserFrame queues everything until the
  //     FrameAttached ack round-trip completes, which is strictly after
  //     OnContextCreated. A key arriving then has already lost the race against
  //     the document's first inline script.
  // A [Sync] pull here is after the right document exists and before page script
  // runs. That is the entire requirement, and only this satisfies both halves.
  //
  // FAIL CLOSED everywhere below: every early return leaves the document with no
  // key, so HodosSessionCache reports FarblingEnabled() == false and every patched
  // API returns its native value. Never substitute a default or constant key -- a
  // degenerate constant-seeded farble is a WORSE fingerprint than none, and it
  // would silently defeat the auth-domain exemption, which depends on the bypass
  // being a true native pass-through.
  if (!frame_ || attach_denied_) {
    return;
  }

  const bool is_main_frame = (frame_->Parent() == nullptr);

  // P4e: SUBFRAMES ARE IN SCOPE. The old `if (Parent() != nullptr) return;` here made
  // every subframe -- same-origin ones included -- fail closed to native values, which
  // is a BYPASS and not merely a coverage gap: a same-origin child frame is fully
  // scriptable from its parent, so three lines of JS read the machine's real canvas /
  // WebGL / audio / navigator out of an about:blank iframe on the very page farbling is
  // supposed to protect. Measured on macOS (f910e19) and again on Windows 2026-08-13.
  //
  // The browser now answers every frame with its TOP frame's key (see
  // CefBrowserFrame::ResolveTopFrameHost), so a subframe has an entry to find and the
  // exemption bit is inherited from the top frame for free.

  // --- memo -------------------------------------------------------------------------
  //
  // ⛔ THIS IS A SAFETY MECHANISM, NOT AN OPTIMISATION, and it must not be removed as
  // "premature". A subframe's OnContextCreated fires INSIDE the parent's JS call stack
  // when the iframe is appended synchronously, so without a memo
  //
  //     for (let i = 0; i < 10000; i++) document.body.appendChild(document.createElement('iframe'));
  //
  // becomes 10,000 BLOCKING browser round-trips on the renderer main thread -- roughly a
  // second and a half of jank that the page fully controls. Page-controlled amplification
  // of a blocking IPC is an availability bug. Keyed by the top frame's token, every frame
  // sharing a top document costs exactly one round trip.
  //
  // ⚠️ INVALIDATION IS EXPLICIT AND MUST STAY THAT WAY. It is tempting to rely on the
  // token changing per document -- RenderDocument is enabled by default at the
  // `all-frames` level on Chromium 150, so each navigation does get a fresh
  // RenderFrameHost and hence a fresh token. But that is a FeatureParam default, not an
  // invariant: a field trial or --disable-features flips it, the top frame then reuses
  // its LocalFrame across a same-site navigation, and the memo would serve the PREVIOUS
  // document's verdict -- i.e. a Privacy Shield toggle silently fails to take effect.
  // So a main frame drops its own entry before pulling, which is safe because the top
  // frame's OnContextCreated always precedes its subframes'.
  //
  // ⚠️ Renderer `Top()` and browser `GetOutermostMainFrame()` partition differently for
  // fenced frames: Top() stops at the fenced root, the browser escapes it. That can only
  // make the memo key FINER than the answer's partition, costing an extra round trip and
  // never yielding a wrong key. The dangerous direction -- a coarser key than the answer
  // -- is impossible. Do not "simplify" this to the browser's notion of top.
  static base::NoDestructor<std::map<base::UnguessableToken, HodosFarblingMemoEntry>>
      memo;
  // Bounded so a session that visits many top documents in one renderer cannot grow it
  // without limit. Clearing wholesale rather than evicting one entry is deliberate: the
  // only cost of a miss is one round trip, so the simplest correct policy wins.
  constexpr size_t kMemoMaxEntries = 32;

  blink::WebFrame* top_frame = frame_->Top();
  const base::UnguessableToken memo_key =
      top_frame ? top_frame->GetFrameToken().value() : base::UnguessableToken();

  if (is_main_frame) {
    // ⚠️ The erase happens BEFORE the main-frame URL gating below, deliberately. If it
    // sat after, a main frame navigating from a keyed site to internal UI would take an
    // early return without invalidating, and (when frame tokens are reused, i.e.
    // RenderDocument below `all-frames`) its subframes would then be served the PREVIOUS
    // document's key. Invalidating first is free -- the only cost of an unnecessary
    // erase is one round trip.
    memo->erase(memo_key);
  } else {
    auto it = memo->find(memo_key);
    if (it != memo->end()) {
      if (!it->second.has_key) {
        return;  // memoised "no key" -- fail closed, no IPC.
      }
      blink_glue::SetHodosFarblingKey(frame_, it->second.key.data(),
                                      it->second.enabled);
      return;
    }
  }

  const GURL url = frame_->GetDocument().Url();
  std::string host;

  if (is_main_frame) {
    // Main-frame fast paths, kept EXACTLY as they were so that this change cannot
    // regress startup. Hodos serves the header and ~15 overlays from
    // http://127.0.0.1:5137; each is a main frame, and each would otherwise fire a
    // BLOCKING sync round-trip on the first-paint critical path only to be told there
    // is no key. The shell never files an entry for those hosts, so not asking is both
    // faster and semantically right -- internal UI is never farbled.
    if (url.SchemeIsHTTPOrHTTPS()) {
      // Parenthesised, not `=`: GURL::host() returns std::string_view on Chromium 150,
      // and std::string's string_view constructor is explicit, so copy-initialisation
      // does not compile. We need an owning std::string anyway to pass by const ref.
      host = std::string(url.host());
      if (host.empty()) {
        return;
      }
      if (host == "127.0.0.1" || host == "localhost" || host == "[::1]") {
        return;
      }
    } else if (frame_->Opener() == nullptr) {
      // A non-HTTP(S) main frame with no opener is the initial empty document of a
      // fresh tab, devtools://, or a file:// page. Nothing to key on.
      //
      // ⛔ The `Opener()` clause is load-bearing, not defensive tidiness. `window.open()`
      // yields a main frame sitting on about:blank that INHERITS the opener's origin and
      // is scriptable from it -- the same bypass in a different container. Drop this
      // clause and the popup vector survives the fix while every iframe test goes green.
      // Measured live on Windows 2026-08-13: popup child == native on all five fields.
      return;
    }
    // else: origin-inheriting main frame WITH an opener -- fall through and pull. The
    // browser walks the opener chain to find the real first party.
  }

  // Bind if needed but do not disturb the connection state machine: pass the
  // CURRENT acked-ness rather than asserting it. The remote is normally already
  // bound here (OnDidCommitProvisionalLoad -> ConnectBrowserFrame precedes
  // OnContextCreated), and a sync call does NOT require FrameAttached to have been
  // acked -- CefBrowserFrame::GetHodosFarblingKey answers purely from
  // browser-process state and never touches the CefFrameHostImpl association.
  auto& browser_frame = GetBrowserFrame(
      /*expect_acked=*/browser_connection_state_ ==
      ConnectionState::CONNECTION_ACKED);
  if (!browser_frame) {
    return;
  }

  std::string key_hex;
  bool enabled = false;
  // |host| is advisory and is empty for subframes; the browser keys on the top frame it
  // resolves for itself. It is still sent so the browser can log a main-frame mismatch.
  if (!browser_frame->GetHodosFarblingKey(host, &key_hex, &enabled)) {
    // Sync call failed outright (pipe error / browser going away). Do NOT memoise a
    // transport failure as "no key" -- that would turn one dropped pipe into a whole
    // top document's worth of silently unfarbled frames.
    return;
  }

  if (memo->size() >= kMemoMaxEntries) {
    memo->clear();
  }

  if (key_hex.empty()) {
    // A definite "the browser has nothing for this top frame". Distinct from a failure,
    // and handled identically on purpose -- but memoisable, unlike a failure, so the
    // subframes of an unkeyed page cost one round trip between them rather than one each.
    (*memo)[memo_key] = HodosFarblingMemoEntry{};
    return;
  }

  std::array<uint8_t, 32> key{};
  if (key_hex.size() != key.size() * 2) {
    LOG(WARNING) << "Hodos: farbling key wrong length; not farbling "
                 << frame_debug_str_;
    (*memo)[memo_key] = HodosFarblingMemoEntry{};
    return;
  }
  for (size_t i = 0; i < key.size(); ++i) {
    unsigned int byte = 0;
    if (std::sscanf(key_hex.c_str() + i * 2, "%2x", &byte) != 1) {
      // Reject wholesale rather than farbling with a partially decoded key: a
      // half-random key is not a weaker secret, it is a different fingerprint.
      LOG(WARNING) << "Hodos: farbling key not valid hex; not farbling "
                   << frame_debug_str_;
      (*memo)[memo_key] = HodosFarblingMemoEntry{};
      return;
    }
    key[i] = static_cast<uint8_t>(byte);
  }

  HodosFarblingMemoEntry entry;
  entry.has_key = true;
  entry.enabled = enabled;
  entry.key = key;
  (*memo)[memo_key] = entry;

  blink_glue::SetHodosFarblingKey(frame_, key.data(), enabled);
}

void CefFrameImpl::ExecuteOnLocalFrame(const std::string& function_name,
                                       LocalFrameAction action) {
  CEF_REQUIRE_RT_RETURN_VOID();

  if (!context_created_) {
    queued_context_actions_.emplace(function_name, std::move(action));
    MaybeInitializeScriptContext();
    return;
  }

  if (frame_) {
    std::move(action).Run(frame_);
  } else {
    LOG(WARNING) << function_name << " sent to detached " << frame_debug_str_
                 << " will be ignored";
  }
}

void CefFrameImpl::ConnectBrowserFrame(ConnectReason reason) {
  DCHECK(browser_connection_state_ == ConnectionState::DISCONNECTED ||
         browser_connection_state_ == ConnectionState::RECONNECT_PENDING);

  if (VLOG_IS_ON(1)) {
    std::string reason_str;
    switch (reason) {
      case ConnectReason::DID_COMMIT:
        reason_str = "DID_COMMIT";
        break;
      case ConnectReason::WAS_SHOWN:
        reason_str = "WAS_SHOWN";
        break;
      case ConnectReason::RETRY:
        reason_str = base::StringPrintf(
            "RETRY %zu/%zu", browser_connect_retry_ct_, kConnectionRetryMaxCt);
        break;
    }
    DVLOG(1) << __func__ << ": " << frame_debug_str_
             << " connection request (reason=" << reason_str << ")";
  }

  browser_connect_timer_.Stop();

  // Don't attempt to connect an invalid or bfcache'd frame. If a bfcache'd
  // frame returns to active status a reconnect will be triggered via
  // OnWasShown().
  if (!frame_ || attach_denied_ || blink_glue::IsInBackForwardCache(frame_)) {
    browser_connection_state_ = ConnectionState::DISCONNECTED;
    DVLOG(1) << __func__ << ": " << frame_debug_str_
             << " connection retry canceled (reason="
             << (frame_ ? (attach_denied_ ? "ATTACH_DENIED" : "BFCACHED")
                        : "INVALID")
             << ")";
    return;
  }

  browser_connection_state_ = ConnectionState::CONNECTION_PENDING;

  auto& browser_frame = GetBrowserFrame(/*expect_acked=*/false);
  CHECK(browser_frame);

  // True if this connection is a retry or if the frame just exited the
  // BackForwardCache.
  const bool reattached =
      browser_connect_retry_ct_ > 0 || reason == ConnectReason::WAS_SHOWN;

  // If the channel is working we should get a call to FrameAttachedAck().
  // Otherwise, OnDisconnect() should be called to retry the
  // connection.
  browser_frame->FrameAttached(receiver_.BindNewPipeAndPassRemote(),
                               reattached);
  receiver_.set_disconnect_with_reason_and_result_handler(
      base::BindOnce(&CefFrameImpl::OnRenderFrameDisconnect, this));
}

const mojo::Remote<cef::mojom::BrowserFrame>& CefFrameImpl::GetBrowserFrame(
    bool expect_acked) {
  DCHECK_EQ(expect_acked,
            browser_connection_state_ == ConnectionState::CONNECTION_ACKED);

  if (!browser_frame_.is_bound()) {
    auto render_frame = content::RenderFrameImpl::FromWebFrame(frame_);
    if (render_frame) {
      // Triggers creation of a CefBrowserFrame in the browser process.
      render_frame->GetBrowserInterfaceBroker().GetInterface(
          browser_frame_.BindNewPipeAndPassReceiver());
      browser_frame_.set_disconnect_with_reason_and_result_handler(
          base::BindOnce(&CefFrameImpl::OnBrowserFrameDisconnect, this));
    }
  }
  return browser_frame_;
}

void CefFrameImpl::OnBrowserFrameDisconnect(uint32_t custom_reason,
                                            const std::string& description,
                                            MojoResult error_result) {
  OnDisconnect(DisconnectReason::BROWSER_FRAME_DISCONNECT, custom_reason,
               description, error_result);
}

void CefFrameImpl::OnRenderFrameDisconnect(uint32_t custom_reason,
                                           const std::string& description,
                                           MojoResult error_result) {
  OnDisconnect(DisconnectReason::RENDER_FRAME_DISCONNECT, custom_reason,
               description, error_result);
}

// static
std::string CefFrameImpl::GetDisconnectDebugString(
    ConnectionState connection_state,
    bool frame_is_valid,
    bool frame_is_main,
    DisconnectReason reason,
    uint32_t custom_reason,
    const std::string& description,
    MojoResult error_result) {
  std::string reason_str;
  switch (reason) {
    case DisconnectReason::DETACHED:
      reason_str = "DETACHED";
      break;
    case DisconnectReason::RENDER_FRAME_DISCONNECT:
      reason_str = "RENDER_FRAME_DISCONNECT";
      break;
    case DisconnectReason::BROWSER_FRAME_DISCONNECT:
      reason_str = "BROWSER_FRAME_DISCONNECT";
      break;
  };

  std::string state_str;
  switch (connection_state) {
    case ConnectionState::DISCONNECTED:
      state_str = "DISCONNECTED";
      break;
    case ConnectionState::CONNECTION_PENDING:
      state_str = "CONNECTION_PENDING";
      break;
    case ConnectionState::CONNECTION_ACKED:
      state_str = "CONNECTION_ACKED";
      break;
    case ConnectionState::RECONNECT_PENDING:
      state_str = "RECONNECT_PENDING";
      break;
  }

  if (!frame_is_valid) {
    state_str += ", FRAME_INVALID";
  } else if (frame_is_main) {
    state_str += ", MAIN_FRAME";
  } else {
    state_str += ", SUB_FRAME";
  }

  if (custom_reason !=
      static_cast<uint32_t>(frame_util::ResetReason::kNoReason)) {
    state_str += ", custom_reason=" + base::NumberToString(custom_reason);
  }

  if (!description.empty()) {
    state_str += ", description=" + description;
  }

  if (error_result != MOJO_RESULT_OK) {
    state_str += ", error_result=" + base::NumberToString(error_result);
  }

  return "(reason=" + reason_str + ", current_state=" + state_str + ")";
}

void CefFrameImpl::OnDisconnect(DisconnectReason reason,
                                uint32_t custom_reason,
                                const std::string& description,
                                MojoResult error_result) {
  // Ignore multiple calls in close proximity (which may occur if both
  // |browser_frame_| and |receiver_| disconnect). |frame_| will be nullptr
  // when called from/after OnDetached().
  if (frame_ &&
      browser_connection_state_ == ConnectionState::RECONNECT_PENDING) {
    return;
  }

  // Ignore additional calls if we're already disconnected. DETACHED,
  // RENDER_FRAME_DISCONNECT and/or BROWSER_FRAME_DISCONNECT may arrive in any
  // order.
  if (browser_connection_state_ == ConnectionState::DISCONNECTED) {
    return;
  }

  const auto connection_state = browser_connection_state_;
  const bool frame_is_valid = !!frame_;
  const bool frame_is_main = frame_ && frame_->IsOutermostMainFrame();
  DVLOG(1) << __func__ << ": " << frame_debug_str_ << " disconnected "
           << GetDisconnectDebugString(connection_state, frame_is_valid,
                                       frame_is_main, reason, custom_reason,
                                       description, error_result);

  browser_frame_.reset();
  receiver_.reset();
  browser_connection_state_ = ConnectionState::DISCONNECTED;

  // True if the frame was previously bound/connected and then intentionally
  // detached (Receiver::ResetWithReason called) from the browser process side.
  const bool connected_and_intentionally_detached =
      (reason == DisconnectReason::BROWSER_FRAME_DISCONNECT ||
       reason == DisconnectReason::RENDER_FRAME_DISCONNECT) &&
      custom_reason !=
          static_cast<uint32_t>(frame_util::ResetReason::kNoReason);

  // Don't retry if the frame is invalid or if the browser process has
  // intentionally detached.
  if (!frame_ || attach_denied_ || connected_and_intentionally_detached) {
    return;
  }

  // True if the connection was closed (binding declined) from the browser
  // process side. This can occur during navigation or if a matching
  // RenderFrameHost is not currently available (like for bfcache'd frames).
  // When navigating there is a race in the browser process between
  // BrowserInterfaceBrokerImpl::GetInterface and RenderFrameHostImpl::
  // DidCommitNavigation. The connection will be closed if the GetInterface call
  // from the renderer is still in-flight when DidCommitNavigation calls
  // |broker_receiver_.reset()|. If, however, the GetInterface call arrives
  // first (BrowserInterfaceBrokerImpl::GetInterface called and the
  // PendingReceiver bound) then the binding will be successful and remain
  // connected until the connection is closed for some other reason (like the
  // Receiver being reset or the renderer process terminating).
  const bool connection_binding_declined =
      (reason == DisconnectReason::BROWSER_FRAME_DISCONNECT ||
       reason == DisconnectReason::RENDER_FRAME_DISCONNECT) &&
      error_result == MOJO_RESULT_FAILED_PRECONDITION;

  if (browser_connect_retry_ct_++ < kConnectionRetryMaxCt) {
    DVLOG(1) << __func__ << ": " << frame_debug_str_
             << " connection retry scheduled (" << browser_connect_retry_ct_
             << "/" << kConnectionRetryMaxCt << ")";
    if (!browser_connect_retry_log_.empty()) {
      browser_connect_retry_log_ += "; ";
    }
    browser_connect_retry_log_ += GetDisconnectDebugString(
        connection_state, frame_is_valid, frame_is_main, reason, custom_reason,
        description, error_result);

    // Use a shorter delay for the first retry attempt after the browser process
    // intentionally declines the connection. This will improve load performance
    // in normal circumstances (reasonably fast machine and navigations with
    // limited redirects).
    const auto retry_delay =
        connection_binding_declined && browser_connect_retry_ct_ == 1
            ? kConnectionRetryDelayShort
            : kConnectionRetryDelayLong;

    // Retry after a delay in case the frame is currently navigating or entering
    // the bfcache. In the navigation case the retry will likely succeed. In the
    // bfcache case the status may not be updated immediately, so we allow the
    // reconnect timer to trigger and then check the status in
    // ConnectBrowserFrame() instead.
    browser_connection_state_ = ConnectionState::RECONNECT_PENDING;
    browser_connect_timer_.Start(
        FROM_HERE, retry_delay,
        base::BindOnce(&CefFrameImpl::ConnectBrowserFrame, this,
                       ConnectReason::RETRY));
    return;
  }

  DVLOG(1) << __func__ << ": " << frame_debug_str_
           << " connection retry limit exceeded";

  // Don't crash on retry failures in cases where the browser process has
  // intentionally declined the connection and we have never been previously
  // connected. Also don't crash for sub-frame connection failures as those are
  // less likely to be important functionally. We still crash for other main
  // frame connection errors or in cases where a previously connected main frame
  // was disconnected without first being intentionally deleted or detached.
  const bool ignore_retry_failure =
      (connection_binding_declined && !ever_connected_) || !frame_is_main;

  // Trigger a crash in official builds.
  LOG_IF(FATAL, !ignore_retry_failure)
      << frame_debug_str_ << " connection retry failed "
      << GetDisconnectDebugString(connection_state, frame_is_valid,
                                  frame_is_main, reason, custom_reason,
                                  description, error_result)
      << ", prior disconnects: " << browser_connect_retry_log_;
}

void CefFrameImpl::SendToBrowserFrame(const std::string& function_name,
                                      BrowserFrameAction action) {
  if (!frame_ || attach_denied_) {
    // We're detached.
    LOG(WARNING) << function_name << " sent to detached " << frame_debug_str_
                 << " will be ignored";
    return;
  }

  if (browser_connection_state_ != ConnectionState::CONNECTION_ACKED) {
    // Queue actions until we're notified by the browser that it's ready to
    // handle them.
    queued_browser_actions_.emplace(function_name, std::move(action));
    return;
  }

  auto& browser_frame = GetBrowserFrame();
  CHECK(browser_frame);

  std::move(action).Run(browser_frame);
}

void CefFrameImpl::MaybeInitializeScriptContext() {
  if (did_initialize_script_context_) {
    return;
  }

  if (!did_commit_provisional_load_) {
    // Too soon for context initialization.
    return;
  }

  if (queued_context_actions_.empty()) {
    // Don't need early context initialization. Avoid it due to performance
    // consequences.
    return;
  }

  did_initialize_script_context_ = true;

  // Explicitly force creation of the script context. This occurred implicitly
  // via DidCommitProvisionalLoad prior to https://crrev.com/5150754880a.
  // Otherwise, a script context may never be created for a frame that doesn't
  // contain JS code.
  v8::HandleScope handle_scope(GetFrameIsolate(frame_));
  frame_->MainWorldScriptContext();
}

void CefFrameImpl::FrameAttachedAck(bool allow) {
  // Sent from the browser process in response to ConnectBrowserFrame() sending
  // FrameAttached().
  CHECK_EQ(ConnectionState::CONNECTION_PENDING, browser_connection_state_);
  browser_connection_state_ = ConnectionState::CONNECTION_ACKED;
  browser_connect_retry_ct_ = 0;
  browser_connect_retry_log_.clear();

  DVLOG(1) << __func__ << ": " << frame_debug_str_
           << " connection acked allow=" << allow;

  if (!allow) {
    // This will be followed by a connection disconnect from the browser side.
    attach_denied_ = true;
    while (!queued_browser_actions_.empty()) {
      queued_browser_actions_.pop();
    }
    return;
  }

  ever_connected_ = true;

  auto& browser_frame = GetBrowserFrame();
  CHECK(browser_frame);

  while (!queued_browser_actions_.empty()) {
    std::move(queued_browser_actions_.front().second).Run(browser_frame);
    queued_browser_actions_.pop();
  }
}

void CefFrameImpl::SendMessage(const std::string& name,
                               base::ListValue arguments) {
  // Hodos C2: the farbling key must never surface in the client's
  // OnProcessMessageReceived -- it is deliberately not public CEF API, because the
  // key has to end up inside Blink (which an embedder cannot reach) and no embedder
  // should ever hold it.
  //
  // This arm should be unreachable: the browser side now consumes the message in
  // CefFrameHostImpl::SendProcessMessage and turns it into a registry fill, so it
  // is never put on the wire. Kept as a belt-and-braces drop so that if a future
  // change starts forwarding it again, the failure is "no farbling" rather than
  // "the key leaked to the client".
  if (name == kHodosFarblingKeyMessage) {
    return;
  }

  if (auto app = CefAppManager::Get()->GetApplication()) {
    if (auto handler = app->GetRenderProcessHandler()) {
      CefRefPtr<CefProcessMessageImpl> message(
          new CefProcessMessageImpl(name, std::move(arguments),
                                    /*read_only=*/true));
      handler->OnProcessMessageReceived(browser_, this, PID_BROWSER,
                                        message.get());
    }
  }
}

void CefFrameImpl::SendSharedMemoryRegion(
    const std::string& name,
    base::WritableSharedMemoryRegion region) {
  if (auto app = CefAppManager::Get()->GetApplication()) {
    if (auto handler = app->GetRenderProcessHandler()) {
      CefRefPtr<CefProcessMessage> message(
          new CefProcessMessageSMRImpl(name, std::move(region)));
      handler->OnProcessMessageReceived(browser_, this, PID_BROWSER, message);
    }
  }
}

void CefFrameImpl::SendCommand(const std::string& command) {
  ExecuteOnLocalFrame(
      __FUNCTION__,
      base::BindOnce(
          [](const std::string& command, blink::WebLocalFrame* frame) {
            frame->ExecuteCommand(blink::WebString::FromUtf8(command));
          },
          command));
}

void CefFrameImpl::SendCommandWithResponse(
    const std::string& command,
    cef::mojom::RenderFrame::SendCommandWithResponseCallback callback) {
  ExecuteOnLocalFrame(
      __FUNCTION__,
      base::BindOnce(
          [](const std::string& command,
             cef::mojom::RenderFrame::SendCommandWithResponseCallback callback,
             blink::WebLocalFrame* frame) {
            blink::WebString response;

            if (base::EqualsCaseInsensitiveASCII(command, "getsource")) {
              response = blink_glue::DumpDocumentMarkup(frame);
            } else if (base::EqualsCaseInsensitiveASCII(command, "gettext")) {
              response = blink_glue::DumpDocumentText(frame);
            }

            std::move(callback).Run(
                string_util::CreateSharedMemoryRegion(response));
          },
          command, std::move(callback)));
}

void CefFrameImpl::SendJavaScript(const std::u16string& jsCode,
                                  const std::string& scriptUrl,
                                  int32_t startLine) {
  ExecuteOnLocalFrame(
      __FUNCTION__,
      base::BindOnce(
          [](const std::u16string& jsCode, const std::string& scriptUrl,
             blink::WebLocalFrame* frame) {
            frame->ExecuteScript(blink::WebScriptSource(
                blink::WebString::FromUtf16(jsCode), GURL(scriptUrl)));
          },
          jsCode, scriptUrl));
}

void CefFrameImpl::LoadRequest(cef::mojom::RequestParamsPtr params) {
  ExecuteOnLocalFrame(
      __FUNCTION__,
      base::BindOnce(
          [](cef::mojom::RequestParamsPtr params, blink::WebLocalFrame* frame) {
            blink::WebURLRequest request;
            CefRequestImpl::Get(params, request);
            blink_glue::StartNavigation(frame, request);
          },
          std::move(params)));
}

void CefFrameImpl::DidStopLoading() {
  // We should only receive this notification for the highest-level LocalFrame
  // in this frame's in-process subtree. If there are multiple of these for
  // the same browser then the other occurrences will be discarded in
  // OnLoadingStateChange.
  browser_->OnLoadingStateChange(false);

  if (blink::RuntimeEnabledFeatures::BackForwardCacheEnabled()) {
    // Refresh draggable regions. Otherwise, we may not receive updated regions
    // after navigation because LocalFrameView::UpdateDocumentAnnotatedRegion
    // lacks sufficient context. When bfcache is enabled we can't rely on
    // OnDidFinishLoad() as the frame may not actually be reloaded.
    OnDraggableRegionsChanged();
  }
}

void CefFrameImpl::MoveOrResizeStarted() {
  if (frame_) {
    auto web_view = frame_->View();
    if (web_view) {
      web_view->CancelPagePopup();
    }
  }
}

void CefFrameImpl::ContextLifecycleStateChanged(
    blink::mojom::blink::FrameLifecycleState state) {
  if (state == blink::mojom::FrameLifecycleState::kFrozen && IsMain() &&
      blink_glue::IsInBackForwardCache(frame_)) {
    browser_->OnEnterBFCache();
  }
}

// Enable deprecation warnings on Windows. See http://crbug.com/585142.
#if BUILDFLAG(IS_WIN)
#if defined(__clang__)
#pragma GCC diagnostic pop
#else
#pragma warning(pop)
#endif
#endif
