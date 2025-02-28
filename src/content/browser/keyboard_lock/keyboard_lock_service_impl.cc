// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/keyboard_lock/keyboard_lock_service_impl.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/containers/flat_set.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/optional.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/keyboard_lock/keyboard_lock_metrics.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/common/content_features.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/keycode_converter.h"

using blink::mojom::GetKeyboardLayoutMapResult;
using blink::mojom::KeyboardLockRequestResult;

namespace content {

namespace {

void LogKeyboardLockMethodCalled(KeyboardLockMethods method) {
  UMA_HISTOGRAM_ENUMERATION(kKeyboardLockMethodCalledHistogramName, method,
                            KeyboardLockMethods::kCount);
}

}  // namespace

KeyboardLockServiceImpl::KeyboardLockServiceImpl(
    RenderFrameHost* render_frame_host)
    : render_frame_host_(static_cast<RenderFrameHostImpl*>(render_frame_host)) {
  DCHECK(render_frame_host_);
}

KeyboardLockServiceImpl::~KeyboardLockServiceImpl() = default;

// static
void KeyboardLockServiceImpl::CreateMojoService(
    RenderFrameHost* render_frame_host,
    blink::mojom::KeyboardLockServiceRequest request) {
  mojo::MakeStrongBinding(
      std::make_unique<KeyboardLockServiceImpl>(render_frame_host),
      std::move(request));
}

void KeyboardLockServiceImpl::RequestKeyboardLock(
    const std::vector<std::string>& key_codes,
    RequestKeyboardLockCallback callback) {
  if (key_codes.empty())
    LogKeyboardLockMethodCalled(KeyboardLockMethods::kRequestAllKeys);
  else
    LogKeyboardLockMethodCalled(KeyboardLockMethods::kRequestSomeKeys);

  if (!base::FeatureList::IsEnabled(features::kKeyboardLockAPI)) {
    std::move(callback).Run(KeyboardLockRequestResult::kSuccess);
    return;
  }

  if (!render_frame_host_->IsCurrent()) {
    std::move(callback).Run(KeyboardLockRequestResult::kFrameDetachedError);
    return;
  }

  if (render_frame_host_->GetParent()) {
    std::move(callback).Run(KeyboardLockRequestResult::kChildFrameError);
    return;
  }

  // Per base::flat_set usage notes, the proper way to init a flat_set is
  // inserting into a vector and using that to init the flat_set.
  std::vector<ui::DomCode> dom_codes;
  for (const std::string& code : key_codes) {
    ui::DomCode dom_code = ui::KeycodeConverter::CodeStringToDomCode(code);
    if (dom_code != ui::DomCode::NONE)
      dom_codes.push_back(dom_code);
  }

  // If we are provided with a vector containing only invalid keycodes, then
  // exit without enabling keyboard lock.  An empty vector is treated as
  // 'capture all keys' which is not what the caller intended.
  if (!key_codes.empty() && dom_codes.empty()) {
    std::move(callback).Run(KeyboardLockRequestResult::kNoValidKeyCodesError);
    return;
  }

  base::Optional<base::flat_set<ui::DomCode>> dom_code_set;
  if (!dom_codes.empty())
    dom_code_set = std::move(dom_codes);

  render_frame_host_->GetRenderWidgetHost()->RequestKeyboardLock(
      std::move(dom_code_set));

  std::move(callback).Run(KeyboardLockRequestResult::kSuccess);
}

void KeyboardLockServiceImpl::CancelKeyboardLock() {
  LogKeyboardLockMethodCalled(KeyboardLockMethods::kCancelLock);

  if (base::FeatureList::IsEnabled(features::kKeyboardLockAPI))
    render_frame_host_->GetRenderWidgetHost()->CancelKeyboardLock();
}

void KeyboardLockServiceImpl::GetKeyboardLayoutMap(
    GetKeyboardLayoutMapCallback callback) {
  auto response = GetKeyboardLayoutMapResult::New();
  response->status = blink::mojom::GetKeyboardLayoutMapStatus::kSuccess;
  response->layout_map =
      render_frame_host_->GetRenderWidgetHost()->GetKeyboardLayoutMap();

  std::move(callback).Run(std::move(response));
}

}  // namespace content
