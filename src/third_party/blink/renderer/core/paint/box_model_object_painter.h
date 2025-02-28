// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_PAINT_BOX_MODEL_OBJECT_PAINTER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_PAINT_BOX_MODEL_OBJECT_PAINTER_H_

#include "third_party/blink/renderer/core/layout/background_bleed_avoidance.h"
#include "third_party/blink/renderer/core/paint/box_painter_base.h"
#include "third_party/blink/renderer/platform/geometry/layout_size.h"
#include "third_party/blink/renderer/platform/wtf/allocator.h"

namespace blink {

class FillLayer;
class InlineFlowBox;
class LayoutRect;
struct PaintInfo;
class LayoutBoxModelObject;

// BoxModelObjectPainter is a class that can paint either a LayoutBox or a
// LayoutInline and allows code sharing between block and inline block painting.
class BoxModelObjectPainter : public BoxPainterBase {
  STACK_ALLOCATED();

 public:
  BoxModelObjectPainter(const LayoutBoxModelObject&,
                        const InlineFlowBox* = nullptr);

  static bool IsPaintingBackgroundOfPaintContainerIntoScrollingContentsLayer(
      const LayoutBoxModelObject*,
      const PaintInfo&);

 protected:
  BoxPainterBase::FillLayerInfo GetFillLayerInfo(
      const Color&,
      const FillLayer&,
      BackgroundBleedAvoidance) const override;

  void PaintTextClipMask(GraphicsContext&,
                         const IntRect& mask_rect,
                         const LayoutPoint& paint_offset,
                         bool flow_box_has_multiple_fragments) override;
  LayoutRect AdjustForScrolledContent(const PaintInfo&,
                                      const BoxPainterBase::FillLayerInfo&,
                                      const LayoutRect&) override;

 private:
  const LayoutBoxModelObject& box_model_;
  const InlineFlowBox* flow_box_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_PAINT_BOX_MODEL_OBJECT_PAINTER_H_
