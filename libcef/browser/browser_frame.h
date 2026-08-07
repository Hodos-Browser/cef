// Copyright 2021 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CEF_LIBCEF_BROWSER_BROWSER_FRAME_H_
#define CEF_LIBCEF_BROWSER_BROWSER_FRAME_H_
#pragma once

#include "cef/libcef/browser/frame_host_impl.h"
#include "cef/libcef/browser/frame_service_base.h"
#include "cef/libcef/common/mojom/cef.mojom.h"
#include "mojo/public/cpp/bindings/binder_map.h"

// Implementation of the BrowserFrame mojo interface.
// This is implemented separately from CefFrameHostImpl to better manage the
// association with the RenderFrameHost (which may be speculative, etc.), and so
// that messages are always routed to the most appropriate CefFrameHostImpl
// instance. Lifespan is tied to the RFH via FrameServiceBase.
class CefBrowserFrame : public CefFrameServiceBase<cef::mojom::BrowserFrame> {
 public:
  CefBrowserFrame(content::RenderFrameHost* render_frame_host,
                  mojo::PendingReceiver<cef::mojom::BrowserFrame> receiver);

  CefBrowserFrame(const CefBrowserFrame&) = delete;
  CefBrowserFrame& operator=(const CefBrowserFrame&) = delete;

  ~CefBrowserFrame() override;

  // Called from the ContentBrowserClient method of the same name.
  static void RegisterBrowserInterfaceBindersForFrame(
      content::RenderFrameHost* render_frame_host,
      mojo::BinderMapWithContext<content::RenderFrameHost*>* map);

 private:
  // cef::mojom::BrowserFrame methods:
  void SendMessage(const std::string& name, base::ListValue arguments) override;
  void SendSharedMemoryRegion(const std::string& name,
                              base::WritableSharedMemoryRegion region) override;
  void FrameAttached(mojo::PendingRemote<cef::mojom::RenderFrame> render_frame,
                     bool reattached) override;
  void UpdateDraggableRegions(
      std::optional<std::vector<cef::mojom::DraggableRegionEntryPtr>> regions)
      override;
  // Hodos: serves the renderer's pull of this document's farbling material out of
  // hodos::FarblingRegistry. Answers from browser-process state only -- it does not
  // touch the CefFrameHostImpl association, so it is safe to service before
  // FrameAttached has been acked, which is precisely when the renderer needs it.
  void GetHodosFarblingKey(
      const std::string& host,
      cef::mojom::BrowserFrame::GetHodosFarblingKeyCallback callback) override;

  CefRefPtr<CefFrameHostImpl> GetFrameHost(bool prefer_speculative,
                                           bool* is_excluded = nullptr) const;
};

#endif  // CEF_LIBCEF_BROWSER_BROWSER_FRAME_H_
