// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/paint/skia_paint_canvas.h"

#include "cc/paint/display_item_list.h"
#include "cc/paint/paint_recorder.h"
#include "cc/paint/scoped_raster_flags.h"
#include "third_party/skia/include/core/SkAnnotation.h"
#include "third_party/skia/include/core/SkColorSpaceXformCanvas.h"
#include "third_party/skia/include/core/SkMetaData.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "third_party/skia/include/gpu/GrContext.h"
#include "third_party/skia/include/utils/SkNWayCanvas.h"

namespace cc {

const bool SkiaPaintCanvas::kIsRasterizing = true;

SkiaPaintCanvas::ContextFlushes::ContextFlushes()
    : enable(false), max_draws_before_flush(-1) {}

SkiaPaintCanvas::SkiaPaintCanvas(SkCanvas* canvas,
                                 ImageProvider* image_provider,
                                 ContextFlushes context_flushes)
    : canvas_(canvas),
      image_provider_(image_provider),
      context_flushes_(context_flushes) {}

SkiaPaintCanvas::SkiaPaintCanvas(const SkBitmap& bitmap)
    : canvas_(new SkCanvas(bitmap)), owned_(canvas_) {}

SkiaPaintCanvas::SkiaPaintCanvas(const SkBitmap& bitmap,
                                 const SkSurfaceProps& props)
    : canvas_(new SkCanvas(bitmap, props)), owned_(canvas_) {}

SkiaPaintCanvas::SkiaPaintCanvas(SkCanvas* canvas,
                                 sk_sp<SkColorSpace> target_color_space,
                                 ImageProvider* image_provider,
                                 ContextFlushes context_flushes)
    : canvas_(canvas),
      image_provider_(image_provider),
      context_flushes_(context_flushes) {
  WrapCanvasInColorSpaceXformCanvas(target_color_space);
}

SkiaPaintCanvas::~SkiaPaintCanvas() = default;

void SkiaPaintCanvas::WrapCanvasInColorSpaceXformCanvas(
    sk_sp<SkColorSpace> target_color_space) {
  if (target_color_space) {
    color_space_xform_canvas_ =
        SkCreateColorSpaceXformCanvas(canvas_, target_color_space);
    canvas_ = color_space_xform_canvas_.get();
  }
}

SkMetaData& SkiaPaintCanvas::getMetaData() {
  return canvas_->getMetaData();
}

SkImageInfo SkiaPaintCanvas::imageInfo() const {
  return canvas_->imageInfo();
}

void SkiaPaintCanvas::flush() {
  canvas_->flush();
}

int SkiaPaintCanvas::save() {
  return canvas_->save();
}

int SkiaPaintCanvas::saveLayer(const SkRect* bounds, const PaintFlags* flags) {
  if (!flags)
    return canvas_->saveLayer(bounds, nullptr);

  SkPaint paint = flags->ToSkPaint();
  return canvas_->saveLayer(bounds, &paint);
}

int SkiaPaintCanvas::saveLayerAlpha(const SkRect* bounds,
                                    uint8_t alpha,
                                    bool preserve_lcd_text_requests) {
  if (preserve_lcd_text_requests) {
    SkPaint paint;
    paint.setAlpha(alpha);
    return canvas_->saveLayerPreserveLCDTextRequests(bounds, &paint);
  }
  return canvas_->saveLayerAlpha(bounds, alpha);
}

void SkiaPaintCanvas::restore() {
  canvas_->restore();
}

int SkiaPaintCanvas::getSaveCount() const {
  return canvas_->getSaveCount();
}

void SkiaPaintCanvas::restoreToCount(int save_count) {
  canvas_->restoreToCount(save_count);
}

void SkiaPaintCanvas::translate(SkScalar dx, SkScalar dy) {
  canvas_->translate(dx, dy);
}

void SkiaPaintCanvas::scale(SkScalar sx, SkScalar sy) {
  canvas_->scale(sx, sy);
}

void SkiaPaintCanvas::rotate(SkScalar degrees) {
  canvas_->rotate(degrees);
}

void SkiaPaintCanvas::concat(const SkMatrix& matrix) {
  canvas_->concat(matrix);
}

void SkiaPaintCanvas::setMatrix(const SkMatrix& matrix) {
  canvas_->setMatrix(matrix);
}

void SkiaPaintCanvas::clipRect(const SkRect& rect,
                               SkClipOp op,
                               bool do_anti_alias) {
  canvas_->clipRect(rect, op, do_anti_alias);
}

void SkiaPaintCanvas::clipRRect(const SkRRect& rrect,
                                SkClipOp op,
                                bool do_anti_alias) {
  canvas_->clipRRect(rrect, op, do_anti_alias);
}

void SkiaPaintCanvas::clipPath(const SkPath& path,
                               SkClipOp op,
                               bool do_anti_alias) {
  canvas_->clipPath(path, op, do_anti_alias);
}

SkRect SkiaPaintCanvas::getLocalClipBounds() const {
  return canvas_->getLocalClipBounds();
}

bool SkiaPaintCanvas::getLocalClipBounds(SkRect* bounds) const {
  return canvas_->getLocalClipBounds(bounds);
}

SkIRect SkiaPaintCanvas::getDeviceClipBounds() const {
  return canvas_->getDeviceClipBounds();
}

bool SkiaPaintCanvas::getDeviceClipBounds(SkIRect* bounds) const {
  return canvas_->getDeviceClipBounds(bounds);
}

void SkiaPaintCanvas::drawColor(SkColor color, SkBlendMode mode) {
  canvas_->drawColor(color, mode);
}

void SkiaPaintCanvas::clear(SkColor color) {
  canvas_->clear(color);
}

void SkiaPaintCanvas::drawLine(SkScalar x0,
                               SkScalar y0,
                               SkScalar x1,
                               SkScalar y1,
                               const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  SkiaPaintCanvas::canvas_->drawLine(x0, y0, x1, y1, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawRect(const SkRect& rect, const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawRect(rect, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawIRect(const SkIRect& rect, const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawIRect(rect, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawOval(const SkRect& oval, const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawOval(oval, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawRRect(const SkRRect& rrect, const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawRRect(rrect, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawDRRect(const SkRRect& outer,
                                 const SkRRect& inner,
                                 const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawDRRect(outer, inner, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawRoundRect(const SkRect& rect,
                                    SkScalar rx,
                                    SkScalar ry,
                                    const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawRoundRect(rect, rx, ry, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawPath(const SkPath& path, const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();

  canvas_->drawPath(path, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawImage(const PaintImage& image,
                                SkScalar left,
                                SkScalar top,
                                const PaintFlags* flags) {
  base::Optional<ScopedRasterFlags> scoped_flags;
  if (flags) {
    scoped_flags.emplace(flags, image_provider_, canvas_->getTotalMatrix(),
                         255u, kIsRasterizing);
    if (!scoped_flags->flags())
      return;
  }

  const PaintFlags* raster_flags = scoped_flags ? scoped_flags->flags() : flags;
  PlaybackParams params(image_provider_, canvas_->getTotalMatrix());
  DrawImageOp draw_image_op(image, left, top, nullptr);
  DrawImageOp::RasterWithFlags(&draw_image_op, raster_flags, canvas_, params);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawImageRect(const PaintImage& image,
                                    const SkRect& src,
                                    const SkRect& dst,
                                    const PaintFlags* flags,
                                    SrcRectConstraint constraint) {
  base::Optional<ScopedRasterFlags> scoped_flags;
  if (flags) {
    scoped_flags.emplace(flags, image_provider_, canvas_->getTotalMatrix(),
                         255u, kIsRasterizing);
    if (!scoped_flags->flags())
      return;
  }

  const PaintFlags* raster_flags = scoped_flags ? scoped_flags->flags() : flags;
  PlaybackParams params(image_provider_, canvas_->getTotalMatrix());
  DrawImageRectOp draw_image_rect_op(image, src, dst, flags, constraint);
  DrawImageRectOp::RasterWithFlags(&draw_image_rect_op, raster_flags, canvas_,
                                   params);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawBitmap(const SkBitmap& bitmap,
                                 SkScalar left,
                                 SkScalar top,
                                 const PaintFlags* flags) {
  if (flags) {
    ScopedRasterFlags raster_flags(flags, image_provider_,
                                   canvas_->getTotalMatrix(), 255u,
                                   kIsRasterizing);
    if (!raster_flags.flags())
      return;
    SkPaint paint = raster_flags.flags()->ToSkPaint();
    canvas_->drawBitmap(bitmap, left, top, &paint);
  } else {
    canvas_->drawBitmap(bitmap, left, top, nullptr);
  }
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawTextBlob(scoped_refptr<PaintTextBlob> blob,
                                   SkScalar x,
                                   SkScalar y,
                                   const PaintFlags& flags) {
  ScopedRasterFlags raster_flags(
      &flags, image_provider_, canvas_->getTotalMatrix(), 255u, kIsRasterizing);
  if (!raster_flags.flags())
    return;
  SkPaint paint = raster_flags.flags()->ToSkPaint();
  canvas_->drawTextBlob(blob->ToSkTextBlob(), x, y, paint);
  FlushAfterDrawIfNeeded();
}

void SkiaPaintCanvas::drawPicture(sk_sp<const PaintRecord> record) {
  drawPicture(record, PlaybackParams::CustomDataRasterCallback());
}

bool SkiaPaintCanvas::isClipEmpty() const {
  return canvas_->isClipEmpty();
}

bool SkiaPaintCanvas::isClipRect() const {
  return canvas_->isClipRect();
}

const SkMatrix& SkiaPaintCanvas::getTotalMatrix() const {
  return canvas_->getTotalMatrix();
}

void SkiaPaintCanvas::Annotate(AnnotationType type,
                               const SkRect& rect,
                               sk_sp<SkData> data) {
  switch (type) {
    case AnnotationType::URL:
      SkAnnotateRectWithURL(canvas_, rect, data.get());
      break;
    case AnnotationType::LINK_TO_DESTINATION:
      SkAnnotateLinkToDestination(canvas_, rect, data.get());
      break;
    case AnnotationType::NAMED_DESTINATION: {
      SkPoint point = SkPoint::Make(rect.x(), rect.y());
      SkAnnotateNamedDestination(canvas_, point, data.get());
      break;
    }
  }
}

void SkiaPaintCanvas::drawPicture(
    sk_sp<const PaintRecord> record,
    PlaybackParams::CustomDataRasterCallback custom_raster_callback) {
  auto did_draw_op_cb =
      context_flushes_.enable
          ? base::BindRepeating(&SkiaPaintCanvas::FlushAfterDrawIfNeeded,
                                base::Unretained(this))
          : PlaybackParams::DidDrawOpCallback();
  PlaybackParams params(image_provider_, canvas_->getTotalMatrix(),
                        custom_raster_callback, did_draw_op_cb);
  record->Playback(canvas_, params);
}

void SkiaPaintCanvas::FlushAfterDrawIfNeeded() {
  if (!context_flushes_.enable)
    return;

  if (++num_of_ops_ > context_flushes_.max_draws_before_flush) {
    num_of_ops_ = 0;
    if (auto* context = canvas_->getGrContext()) {
      TRACE_EVENT0("cc",
                   "SkiaPaintCanvas::FlushAfterDrawIfNeeded::FlushGrContext");
      context->flush();
    }
  }
}

}  // namespace cc
