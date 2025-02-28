/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_PUBLIC_PLATFORM_WEB_LAYER_TREE_VIEW_H_
#define THIRD_PARTY_BLINK_PUBLIC_PLATFORM_WEB_LAYER_TREE_VIEW_H_

#include "base/callback.h"
#include "cc/input/overscroll_behavior.h"
#include "cc/trees/layer_tree_mutator.h"
#include "components/viz/common/surfaces/frame_sink_id.h"
#include "third_party/blink/public/platform/web_browser_controls_state.h"
#include "third_party/blink/public/platform/web_common.h"
#include "third_party/blink/public/platform/web_event_listener_properties.h"
#include "third_party/blink/public/platform/web_float_point.h"
#include "third_party/blink/public/platform/web_layer.h"
#include "third_party/blink/public/platform/web_size.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkRefCnt.h"

class SkBitmap;

namespace cc {
class AnimationHost;
class PaintImage;
}

namespace blink {

struct WebPoint;
class WebSelection;

class WebLayerTreeView {
 public:
  // SwapResult mirrors the values of cc::SwapPromise::DidNotSwapReason, and
  // should be kept consistent with it. SwapResult additionally adds a success
  // value (kDidSwap).
  // These values are written to logs. New enum values can be added, but
  // existing enums must never be renumbered, deleted or reused.
  enum SwapResult {
    kDidSwap = 0,
    kDidNotSwapSwapFails = 1,
    kDidNotSwapCommitFails = 2,
    kDidNotSwapCommitNoUpdate = 3,
    kDidNotSwapActivationFails = 4,
    kSwapResultMax,
  };
  using ReportTimeCallback =
      base::OnceCallback<void(SwapResult, base::TimeTicks)>;

  virtual ~WebLayerTreeView() = default;

  // Initialization and lifecycle --------------------------------------

  // Sets the root of the tree. The root is set by way of the constructor.
  // TODO(danakj): This should be a scoped_refptr<cc::Layer>.
  virtual void SetRootLayer(WebLayer*) {}
  virtual void ClearRootLayer() {}

  // TODO(loyso): This should use CompositorAnimationHost. crbug.com/584551
  virtual cc::AnimationHost* CompositorAnimationHost() { return nullptr; }

  // View properties ---------------------------------------------------

  // Viewport size is given in physical pixels.
  virtual WebSize GetViewportSize() const { return WebSize(); }

  // Sets the background color for the viewport.
  virtual void SetBackgroundColor(SkColor) {}

  // Sets whether this view is visible. In threaded mode, a view that is not
  // visible will not composite or trigger UpdateAnimations() or Layout() calls
  // until it becomes visible.
  virtual void SetVisible(bool) {}

  // Sets the current page scale factor and minimum / maximum limits. Both
  // limits are initially 1 (no page scale allowed).
  virtual void SetPageScaleFactorAndLimits(float page_scale_factor,
                                           float minimum,
                                           float maximum) {}

  // Starts an animation of the page scale to a target scale factor and scroll
  // offset.
  // If useAnchor is true, destination is a point on the screen that will remain
  // fixed for the duration of the animation.
  // If useAnchor is false, destination is the final top-left scroll position.
  virtual void StartPageScaleAnimation(const WebPoint& destination,
                                       bool use_anchor,
                                       float new_page_scale,
                                       double duration_sec) {}

  // Returns true if the page scale animation had started.
  virtual bool HasPendingPageScaleAnimation() const { return false; }

  virtual void HeuristicsForGpuRasterizationUpdated(bool) {}

  // Sets the amount that the browser controls are showing, from 0 (hidden) to 1
  // (fully shown).
  virtual void SetBrowserControlsShownRatio(float) {}

  // Update browser controls permitted and current states
  virtual void UpdateBrowserControlsState(WebBrowserControlsState constraints,
                                          WebBrowserControlsState current,
                                          bool animate) {}

  // Set browser controls height. If |shrink_viewport| is set to true, then
  // Blink shrunk the viewport clip layers by the top and bottom browser
  // controls height. Top controls will translate the web page down and do not
  // immediately scroll when hiding. The bottom controls scroll immediately and
  // never translate the content (only clip it).
  virtual void SetBrowserControlsHeight(float top_height,
                                        float bottom_height,
                                        bool shrink_viewport) {}

  // Set the browser's behavior when overscroll happens, e.g. whether to glow
  // or navigate.
  virtual void SetOverscrollBehavior(const cc::OverscrollBehavior&) {}

  // Flow control and scheduling ---------------------------------------

  // Indicates that blink needs a BeginFrame, but that nothing might actually be
  // dirty.
  virtual void SetNeedsBeginFrame() {}

  // Relays the end of a fling animation.
  virtual void DidStopFlinging() {}

  // Run layout and paint of all pending document changes asynchronously.
  virtual void LayoutAndPaintAsync(base::OnceClosure callback) {}

  virtual void CompositeAndReadbackAsync(
      base::OnceCallback<void(const SkBitmap&)> callback) {}

  // Synchronously run all lifecycle phases and compositor update with no
  // raster. Should only be called by layout tests running in synchronous
  // single-threaded mode.
  virtual void SynchronouslyCompositeNoRasterForTesting() {}

  // Synchronously rasterizes and composites a frame.
  virtual void CompositeWithRasterForTesting() {}

  // Prevents updates to layer tree from becoming visible.
  virtual void SetDeferCommits(bool defer_commits) {}

  struct ViewportLayers {
    // TODO(danakj): These should be a scoped_refptr<cc::Layer>.
    WebLayer* overscroll_elasticity = nullptr;
    WebLayer* page_scale = nullptr;
    WebLayer* inner_viewport_container = nullptr;
    WebLayer* outer_viewport_container = nullptr;
    WebLayer* inner_viewport_scroll = nullptr;
    WebLayer* outer_viewport_scroll = nullptr;
  };

  // Identify key viewport layers to the compositor.
  virtual void RegisterViewportLayers(const ViewportLayers& viewport_layers) {}
  virtual void ClearViewportLayers() {}

  // Used to update the active selection bounds.
  virtual void RegisterSelection(const WebSelection&) {}
  virtual void ClearSelection() {}

  // Mutations are plumbed back to the layer tree via the mutator client.
  virtual void SetMutatorClient(std::unique_ptr<cc::LayerTreeMutator>) {}

  // For when the embedder itself change scales on the page (e.g. devtools)
  // and wants all of the content at the new scale to be crisp.
  virtual void ForceRecalculateRasterScales() {}

  // Input properties ---------------------------------------------------
  virtual void SetEventListenerProperties(WebEventListenerClass,
                                          WebEventListenerProperties) {}
  virtual void UpdateEventRectsForSubframeIfNecessary() {}
  virtual void SetHaveScrollEventHandlers(bool) {}

  // Returns the FrameSinkId of the widget associated with this layer tree view.
  virtual viz::FrameSinkId GetFrameSinkId() { return viz::FrameSinkId(); }

  // Debugging / dangerous ---------------------------------------------

  virtual WebEventListenerProperties EventListenerProperties(
      WebEventListenerClass) const {
    return WebEventListenerProperties::kNothing;
  }
  virtual bool HaveScrollEventHandlers() const { return false; }

  virtual int LayerTreeId() const { return 0; }

  // Toggles the FPS counter in the HUD layer
  virtual void SetShowFPSCounter(bool) {}

  // Toggles the paint rects in the HUD layer
  virtual void SetShowPaintRects(bool) {}

  // Toggles the debug borders on layers
  virtual void SetShowDebugBorders(bool) {}

  // Toggles scroll bottleneck rects on the HUD layer
  virtual void SetShowScrollBottleneckRects(bool) {}

  // ReportTimeCallback is a callback that should be fired when the
  // corresponding Swap completes (either with DidSwap or DidNotSwap).
  virtual void NotifySwapTime(ReportTimeCallback callback) {}

  virtual void RequestBeginMainFrameNotExpected(bool new_state) {}

  virtual void RequestDecode(const cc::PaintImage& image,
                             base::OnceCallback<void(bool)> callback) {}
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_PLATFORM_WEB_LAYER_TREE_VIEW_H_
