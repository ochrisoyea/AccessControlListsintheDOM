// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_SKIA_PAINT_CANVAS_H_
#define CC_PAINT_SKIA_PAINT_CANVAS_H_

#include <memory>

#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/macros.h"
#include "build/build_config.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_record.h"
#include "cc/paint/paint_text_blob.h"
#include "third_party/skia/include/core/SkCanvas.h"

namespace cc {
class ImageProvider;
class PaintFlags;

// A PaintCanvas derived class that passes PaintCanvas APIs through to
// an SkCanvas.  This is more efficient than recording to a PaintRecord
// and then playing back to an SkCanvas.
class CC_PAINT_EXPORT SkiaPaintCanvas final : public PaintCanvas {
 public:
  struct CC_PAINT_EXPORT ContextFlushes {
    ContextFlushes();

    bool enable;
    int max_draws_before_flush;
  };

  explicit SkiaPaintCanvas(SkCanvas* canvas,
                           ImageProvider* image_provider = nullptr,
                           ContextFlushes context_flushes = ContextFlushes());
  explicit SkiaPaintCanvas(const SkBitmap& bitmap);
  explicit SkiaPaintCanvas(const SkBitmap& bitmap, const SkSurfaceProps& props);
  // If |target_color_space| is non-nullptr, then this will wrap |canvas| in a
  // SkColorSpaceXformCanvas.
  SkiaPaintCanvas(SkCanvas* canvas,
                  sk_sp<SkColorSpace> target_color_space,
                  ImageProvider* image_provider = nullptr,
                  ContextFlushes context_flushes = ContextFlushes());
  ~SkiaPaintCanvas() override;

  void reset_image_provider() { image_provider_ = nullptr; }

  SkMetaData& getMetaData() override;
  SkImageInfo imageInfo() const override;

  void flush() override;

  int save() override;
  int saveLayer(const SkRect* bounds, const PaintFlags* flags) override;
  int saveLayerAlpha(const SkRect* bounds,
                     uint8_t alpha,
                     bool preserve_lcd_text_requests) override;

  void restore() override;
  int getSaveCount() const override;
  void restoreToCount(int save_count) override;
  void translate(SkScalar dx, SkScalar dy) override;
  void scale(SkScalar sx, SkScalar sy) override;
  void rotate(SkScalar degrees) override;
  void concat(const SkMatrix& matrix) override;
  void setMatrix(const SkMatrix& matrix) override;

  void clipRect(const SkRect& rect, SkClipOp op, bool do_anti_alias) override;
  void clipRRect(const SkRRect& rrect,
                 SkClipOp op,
                 bool do_anti_alias) override;
  void clipPath(const SkPath& path, SkClipOp op, bool do_anti_alias) override;
  SkRect getLocalClipBounds() const override;
  bool getLocalClipBounds(SkRect* bounds) const override;
  SkIRect getDeviceClipBounds() const override;
  bool getDeviceClipBounds(SkIRect* bounds) const override;
  void drawColor(SkColor color, SkBlendMode mode) override;
  void clear(SkColor color) override;

  void drawLine(SkScalar x0,
                SkScalar y0,
                SkScalar x1,
                SkScalar y1,
                const PaintFlags& flags) override;
  void drawRect(const SkRect& rect, const PaintFlags& flags) override;
  void drawIRect(const SkIRect& rect, const PaintFlags& flags) override;
  void drawOval(const SkRect& oval, const PaintFlags& flags) override;
  void drawRRect(const SkRRect& rrect, const PaintFlags& flags) override;
  void drawDRRect(const SkRRect& outer,
                  const SkRRect& inner,
                  const PaintFlags& flags) override;
  void drawRoundRect(const SkRect& rect,
                     SkScalar rx,
                     SkScalar ry,
                     const PaintFlags& flags) override;
  void drawPath(const SkPath& path, const PaintFlags& flags) override;
  void drawImage(const PaintImage& image,
                 SkScalar left,
                 SkScalar top,
                 const PaintFlags* flags) override;
  void drawImageRect(const PaintImage& image,
                     const SkRect& src,
                     const SkRect& dst,
                     const PaintFlags* flags,
                     SrcRectConstraint constraint) override;
  void drawBitmap(const SkBitmap& bitmap,
                  SkScalar left,
                  SkScalar top,
                  const PaintFlags* flags) override;

  void drawTextBlob(scoped_refptr<PaintTextBlob> blob,
                    SkScalar x,
                    SkScalar y,
                    const PaintFlags& flags) override;

  void drawPicture(sk_sp<const PaintRecord> record) override;

  bool isClipEmpty() const override;
  bool isClipRect() const override;
  const SkMatrix& getTotalMatrix() const override;

  void Annotate(AnnotationType type,
                const SkRect& rect,
                sk_sp<SkData> data) override;

  // Don't shadow non-virtual helper functions.
  using PaintCanvas::clipRect;
  using PaintCanvas::clipRRect;
  using PaintCanvas::clipPath;
  using PaintCanvas::drawBitmap;
  using PaintCanvas::drawColor;
  using PaintCanvas::drawImage;
  using PaintCanvas::drawPicture;

  // Same as the above drawPicture() except using the given custom data
  // raster callback.
  void drawPicture(
      sk_sp<const PaintRecord> record,
      PlaybackParams::CustomDataRasterCallback custom_raster_callback);

 private:
  // We always need skia shaders since the ops will be played on an SkCanvas.
  static const bool kIsRasterizing;

  void WrapCanvasInColorSpaceXformCanvas(
      sk_sp<SkColorSpace> target_color_space);
  void FlushAfterDrawIfNeeded();

  SkCanvas* canvas_;
  std::unique_ptr<SkCanvas> owned_;
  std::unique_ptr<SkCanvas> color_space_xform_canvas_;
  ImageProvider* image_provider_ = nullptr;

  const ContextFlushes context_flushes_;
  int num_of_ops_ = 0;

  DISALLOW_COPY_AND_ASSIGN(SkiaPaintCanvas);
};

}  // namespace cc

#endif  // CC_PAINT_SKIA_PAINT_CANVAS_H_
