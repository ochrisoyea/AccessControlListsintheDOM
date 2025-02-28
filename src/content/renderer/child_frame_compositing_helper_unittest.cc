// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/child_frame_compositing_helper.h"

#include "content/renderer/child_frame_compositor.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/web_layer.h"

namespace content {

namespace {

class MockChildFrameCompositor : public ChildFrameCompositor {
 public:
  MockChildFrameCompositor() {
    constexpr int width = 32;
    constexpr int height = 32;
    sad_page_bitmap_.allocN32Pixels(width, height);
  }

  blink::WebLayer* GetLayer() override { return web_layer_.get(); }

  void SetLayer(scoped_refptr<cc::Layer> layer,
                bool prevent_contents_opaque_changes) override {
    layer_ = std::move(layer);
    web_layer_ = std::make_unique<blink::WebLayer>(layer_.get());
  }

  SkBitmap* GetSadPageBitmap() override { return &sad_page_bitmap_; }

 private:
  scoped_refptr<cc::Layer> layer_;
  std::unique_ptr<blink::WebLayer> web_layer_;
  SkBitmap sad_page_bitmap_;

  DISALLOW_COPY_AND_ASSIGN(MockChildFrameCompositor);
};

viz::SurfaceId MakeSurfaceId(const viz::FrameSinkId& frame_sink_id,
                             uint32_t parent_sequence_number,
                             uint32_t child_sequence_number = 1u) {
  return viz::SurfaceId(
      frame_sink_id,
      viz::LocalSurfaceId(parent_sequence_number, child_sequence_number,
                          base::UnguessableToken::Deserialize(0, 1u)));
}

}  // namespace

class ChildFrameCompositingHelperTest : public testing::Test {
 public:
  ChildFrameCompositingHelperTest() : compositing_helper_(&compositor_) {}

  ~ChildFrameCompositingHelperTest() override {}

  ChildFrameCompositingHelper* compositing_helper() {
    return &compositing_helper_;
  }

 private:
  MockChildFrameCompositor compositor_;
  ChildFrameCompositingHelper compositing_helper_;
  DISALLOW_COPY_AND_ASSIGN(ChildFrameCompositingHelperTest);
};

// This test verifies that the fallback surfaceId is cleared when the child
// frame is reported as being gone and a sad page is displayed.
TEST_F(ChildFrameCompositingHelperTest, ChildFrameGoneClearsFallback) {
  // The primary and fallback surface IDs should start out as invalid.
  EXPECT_FALSE(compositing_helper()->primary_surface_id().is_valid());
  EXPECT_FALSE(compositing_helper()->fallback_surface_id().is_valid());

  const viz::SurfaceId fallback_surface_id =
      MakeSurfaceId(viz::FrameSinkId(1, 1), 1);
  const gfx::Size frame_size_in_dip(100, 100);
  compositing_helper()->SetFallbackSurfaceId(fallback_surface_id,
                                             frame_size_in_dip);

  // Since the fallback surface ID was set before the primary surface ID then
  // the primary is set to the same value as the fallback. Verify this is so.
  EXPECT_EQ(fallback_surface_id, compositing_helper()->primary_surface_id());
  EXPECT_EQ(fallback_surface_id, compositing_helper()->fallback_surface_id());

  // Reporting that the child frame is gone should clear both the primary and
  // fallback surface Ids.
  compositing_helper()->ChildFrameGone(frame_size_in_dip, 1.f);
  EXPECT_FALSE(compositing_helper()->primary_surface_id().is_valid());
  EXPECT_FALSE(compositing_helper()->fallback_surface_id().is_valid());
}

}  // namespace content
