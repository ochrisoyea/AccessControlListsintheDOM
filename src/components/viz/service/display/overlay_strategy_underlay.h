// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_VIZ_SERVICE_DISPLAY_OVERLAY_STRATEGY_UNDERLAY_H_
#define COMPONENTS_VIZ_SERVICE_DISPLAY_OVERLAY_STRATEGY_UNDERLAY_H_

#include "base/macros.h"
#include "components/viz/service/display/overlay_processor.h"
#include "components/viz/service/viz_service_export.h"

namespace viz {

class OverlayCandidateValidator;

// The underlay strategy looks for a video quad without regard to quads above
// it. The video is "underlaid" through a black transparent quad substituted
// for the video quad. The overlay content can then be blended in by the
// hardware under the the scene. This is only valid for overlay contents that
// are fully opaque.
class VIZ_SERVICE_EXPORT OverlayStrategyUnderlay
    : public OverlayProcessor::Strategy {
 public:
  explicit OverlayStrategyUnderlay(
      OverlayCandidateValidator* capability_checker);
  ~OverlayStrategyUnderlay() override;

  bool Attempt(const SkMatrix44& output_color_matrix,
               cc::DisplayResourceProvider* resource_provider,
               RenderPass* render_pass,
               cc::OverlayCandidateList* candidate_list,
               std::vector<gfx::Rect>* content_bounds) override;

  OverlayProcessor::StrategyType GetUMAEnum() const override;

 private:
  OverlayCandidateValidator* capability_checker_;  // Weak.

  DISALLOW_COPY_AND_ASSIGN(OverlayStrategyUnderlay);
};

}  // namespace viz

#endif  // COMPONENTS_VIZ_SERVICE_DISPLAY_OVERLAY_STRATEGY_UNDERLAY_H_
