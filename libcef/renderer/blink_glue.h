// Copyright (c) 2012 The Chromium Embedded Framework Authors.
// Portions copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CEF_LIBCEF_RENDERER_BLINK_GLUE_H_
#define CEF_LIBCEF_RENDERER_BLINK_GLUE_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "cef/include/internal/cef_types.h"
#include "third_party/blink/public/mojom/frame/lifecycle.mojom-blink-forward.h"
#include "third_party/blink/public/platform/web_common.h"
#include "v8/include/v8.h"

namespace blink {
class WebElement;
class WebLocalFrame;
class WebNode;
class WebString;
class WebURLRequest;
class WebURLResponse;
class WebView;
}  // namespace blink

namespace blink_glue {

BLINK_EXPORT bool CanGoBack(blink::WebView* view);
BLINK_EXPORT bool CanGoForward(blink::WebView* view);
BLINK_EXPORT void GoBack(blink::WebView* view);
BLINK_EXPORT void GoForward(blink::WebView* view);

BLINK_EXPORT bool IsInBackForwardCache(blink::WebLocalFrame* frame);

// Returns the text of the document element.
BLINK_EXPORT blink::WebString DumpDocumentText(blink::WebLocalFrame* frame);
// Returns the markup of the document element.
BLINK_EXPORT blink::WebString DumpDocumentMarkup(blink::WebLocalFrame* frame);

// Expose additional actions on WebNode.
BLINK_EXPORT cef_dom_node_type_t GetNodeType(const blink::WebNode& node);
BLINK_EXPORT blink::WebString GetNodeName(const blink::WebNode& node);
BLINK_EXPORT blink::WebString CreateNodeMarkup(const blink::WebNode& node);
BLINK_EXPORT bool SetNodeValue(blink::WebNode& node,
                               const blink::WebString& value);

BLINK_EXPORT bool IsTextControlElement(const blink::WebElement& element);

BLINK_EXPORT v8::MaybeLocal<v8::Value> CallV8Function(
    v8::Local<v8::Context> context,
    v8::Local<v8::Function> function,
    v8::Local<v8::Object> receiver,
    int argc,
    v8::Local<v8::Value> args[],
    v8::Isolate* isolate);

BLINK_EXPORT v8::Local<v8::Value> ExecuteV8ScriptAndReturnValue(
    const blink::WebString& source,
    const blink::WebString& source_url,
    int start_line,
    v8::Local<v8::Context> context,
    v8::TryCatch& tryCatch,
    v8::Isolate* isolate);

BLINK_EXPORT v8::MicrotaskQueue* GetMicrotaskQueue(
    v8::Local<v8::Context> context);

BLINK_EXPORT bool IsScriptForbidden();

class BLINK_EXPORT CefObserverRegistration {
 public:
  CefObserverRegistration() = default;

  CefObserverRegistration(const CefObserverRegistration&) = delete;
  CefObserverRegistration& operator=(const CefObserverRegistration&) = delete;

  virtual ~CefObserverRegistration() = default;
};

class BLINK_EXPORT CefExecutionContextLifecycleStateObserver {
 public:
  virtual void ContextLifecycleStateChanged(
      blink::mojom::blink::FrameLifecycleState state) {}

 protected:
  virtual ~CefExecutionContextLifecycleStateObserver() = default;
};

// Register an ExecutionContextLifecycleStateObserver. Remains registered until
// the returned object is destroyed.
BLINK_EXPORT std::unique_ptr<CefObserverRegistration>
RegisterExecutionContextLifecycleStateObserver(
    v8::Local<v8::Context> context,
    CefExecutionContextLifecycleStateObserver* observer);

BLINK_EXPORT void RegisterURLSchemeAsSupportingFetchAPI(
    const blink::WebString& scheme);

// Wrapper for blink::ScriptForbiddenScope.
class BLINK_EXPORT CefScriptForbiddenScope final {
 public:
  CefScriptForbiddenScope();

  CefScriptForbiddenScope(const CefScriptForbiddenScope&) = delete;
  CefScriptForbiddenScope& operator=(const CefScriptForbiddenScope&) = delete;

  ~CefScriptForbiddenScope();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

BLINK_EXPORT bool ResponseWasCached(const blink::WebURLResponse& response);

// Returns true if the frame owner is a plugin.
BLINK_EXPORT bool HasPluginFrameOwner(blink::WebLocalFrame* frame);

BLINK_EXPORT void StartNavigation(blink::WebLocalFrame* frame,
                                  const blink::WebURLRequest& request);

// Sets whether select popup menus should be rendered by the browser.
BLINK_EXPORT void SetUseExternalPopupMenus(blink::WebView* view, bool value);

// Hodos C2: installs this document's fingerprint-farbling material.
//
// |key32| is exactly 32 bytes: HMAC-SHA256(profile_seed, first-party eTLD+1),
// computed in the BROWSER process. The master profile seed never enters a renderer
// and never appears on any command line, so a compromised renderer learns its own
// site's key and nothing about any other site. |farbling_enabled| is the browser's
// single already-collapsed verdict (global toggle AND auth-domain allowlist AND the
// user's per-site Privacy Shield toggle); the renderer does not re-decide.
//
// Lands on the frame's LocalDOMWindow via HodosSessionCache. Until this is called
// the cache holds no key, so every patched API returns the native value: a missed
// or late delivery degrades to "not farbled" rather than to a degenerate
// constant-seeded farble, which would be a worse fingerprint than none.
//
// Safe to call on a detached or document-less frame; it no-ops.
BLINK_EXPORT void SetHodosFarblingKey(blink::WebLocalFrame* frame,
                                      const uint8_t* key32,
                                      bool farbling_enabled);

}  // namespace blink_glue

#endif  // CEF_LIBCEF_RENDERER_BLINK_GLUE_H_
