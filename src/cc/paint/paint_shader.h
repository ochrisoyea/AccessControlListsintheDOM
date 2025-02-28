// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_PAINT_SHADER_H_
#define CC_PAINT_PAINT_SHADER_H_

#include <memory>
#include <vector>

#include "base/optional.h"
#include "base/stl_util.h"
#include "cc/paint/image_analysis_state.h"
#include "cc/paint/paint_export.h"
#include "cc/paint/paint_image.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkScalar.h"
#include "third_party/skia/include/core/SkShader.h"
#include "ui/gfx/color_space.h"
#include "ui/gfx/geometry/size_f.h"

namespace cc {
class ImageProvider;
class PaintOpBuffer;
using PaintRecord = PaintOpBuffer;

class CC_PAINT_EXPORT PaintShader : public SkRefCnt {
 public:
  enum class Type : uint8_t {
    kColor,
    kLinearGradient,
    kRadialGradient,
    kTwoPointConicalGradient,
    kSweepGradient,
    kImage,
    kPaintRecord,
    kShaderCount
  };

  using RecordShaderId = uint32_t;
  static const RecordShaderId kInvalidRecordShaderId;

  // Scaling behavior dictates how a PaintRecord shader will behave. Use
  // RasterAtScale to create a picture shader. Use FixedScale to create an image
  // shader that is backed by the paint record.
  enum class ScalingBehavior : uint8_t { kRasterAtScale, kFixedScale };

  static sk_sp<PaintShader> MakeColor(SkColor color);

  static sk_sp<PaintShader> MakeLinearGradient(
      const SkPoint* points,
      const SkColor* colors,
      const SkScalar* pos,
      int count,
      SkShader::TileMode mode,
      uint32_t flags = 0,
      const SkMatrix* local_matrix = nullptr,
      SkColor fallback_color = SK_ColorTRANSPARENT);

  static sk_sp<PaintShader> MakeRadialGradient(
      const SkPoint& center,
      SkScalar radius,
      const SkColor colors[],
      const SkScalar pos[],
      int color_count,
      SkShader::TileMode mode,
      uint32_t flags = 0,
      const SkMatrix* local_matrix = nullptr,
      SkColor fallback_color = SK_ColorTRANSPARENT);

  static sk_sp<PaintShader> MakeTwoPointConicalGradient(
      const SkPoint& start,
      SkScalar start_radius,
      const SkPoint& end,
      SkScalar end_radius,
      const SkColor colors[],
      const SkScalar pos[],
      int color_count,
      SkShader::TileMode mode,
      uint32_t flags = 0,
      const SkMatrix* local_matrix = nullptr,
      SkColor fallback_color = SK_ColorTRANSPARENT);

  static sk_sp<PaintShader> MakeSweepGradient(
      SkScalar cx,
      SkScalar cy,
      const SkColor colors[],
      const SkScalar pos[],
      int color_count,
      SkShader::TileMode mode,
      SkScalar start_degrees,
      SkScalar end_degrees,
      uint32_t flags = 0,
      const SkMatrix* local_matrix = nullptr,
      SkColor fallback_color = SK_ColorTRANSPARENT);

  static sk_sp<PaintShader> MakeImage(const PaintImage& image,
                                      SkShader::TileMode tx,
                                      SkShader::TileMode ty,
                                      const SkMatrix* local_matrix);

  static sk_sp<PaintShader> MakePaintRecord(
      sk_sp<PaintRecord> record,
      const SkRect& tile,
      SkShader::TileMode tx,
      SkShader::TileMode ty,
      const SkMatrix* local_matrix,
      ScalingBehavior scaling_behavior = ScalingBehavior::kRasterAtScale);

  static size_t GetSerializedSize(const PaintShader* shader);

  ~PaintShader() override;

  void set_has_animated_images(bool has_animated_images) {
    image_analysis_state_ = has_animated_images
                                ? ImageAnalysisState::kAnimatedImages
                                : ImageAnalysisState::kNoAnimatedImages;
  }
  ImageAnalysisState image_analysis_state() const {
    return image_analysis_state_;
  }

  bool has_discardable_images() const;

  SkMatrix GetLocalMatrix() const {
    return local_matrix_ ? *local_matrix_ : SkMatrix::I();
  }
  Type shader_type() const { return shader_type_; }
  const PaintImage& paint_image() const {
    DCHECK_EQ(Type::kImage, shader_type_);
    return image_;
  }

  const gfx::SizeF* tile_scale() const {
    return base::OptionalOrNullptr(tile_scale_);
  }
  const sk_sp<PaintRecord>& paint_record() const { return record_; }
  bool GetRasterizationTileRect(const SkMatrix& ctm, SkRect* tile_rect) const;

  SkShader::TileMode tx() const { return tx_; }
  SkShader::TileMode ty() const { return ty_; }
  SkRect tile() const { return tile_; }

  bool IsOpaque() const;

  // Returns true if the shader looks like it is valid (ie the members required
  // for this shader type all look reasonable. Returns false otherwise. Note
  // that this is a best effort function since truly validating whether the
  // shader is correct is hard.
  bool IsValid() const;

  bool operator==(const PaintShader& other) const;
  bool operator!=(const PaintShader& other) const { return !(*this == other); }

  RecordShaderId paint_record_shader_id() const {
    DCHECK(id_ == kInvalidRecordShaderId || shader_type_ == Type::kPaintRecord);
    return id_;
  }

 private:
  friend class PaintFlags;
  friend class PaintOpReader;
  friend class PaintOpSerializationTestUtils;
  friend class PaintOpWriter;
  friend class ScopedRasterFlags;
  FRIEND_TEST_ALL_PREFIXES(PaintShaderTest, DecodePaintRecord);
  FRIEND_TEST_ALL_PREFIXES(PaintOpBufferTest, PaintRecordShaderSerialization);
  FRIEND_TEST_ALL_PREFIXES(PaintOpBufferTest, RecordShadersCached);

  explicit PaintShader(Type type);

  sk_sp<SkShader> GetSkShader() const;
  void CreateSkShader(ImageProvider* image_provider = nullptr);

  sk_sp<PaintShader> CreateDecodedPaintRecord(
      const SkMatrix& ctm,
      ImageProvider* image_provider) const;

  void SetColorsAndPositions(const SkColor* colors,
                             const SkScalar* positions,
                             int count);
  void SetMatrixAndTiling(const SkMatrix* matrix,
                          SkShader::TileMode tx,
                          SkShader::TileMode ty);
  void SetFlagsAndFallback(uint32_t flags, SkColor fallback_color);

  Type shader_type_ = Type::kShaderCount;

  uint32_t flags_ = 0;
  SkScalar end_radius_ = 0;
  SkScalar start_radius_ = 0;
  SkShader::TileMode tx_ = SkShader::kClamp_TileMode;
  SkShader::TileMode ty_ = SkShader::kClamp_TileMode;
  SkColor fallback_color_ = SK_ColorTRANSPARENT;
  ScalingBehavior scaling_behavior_ = ScalingBehavior::kRasterAtScale;

  base::Optional<SkMatrix> local_matrix_;
  SkPoint center_ = SkPoint::Make(0, 0);
  SkRect tile_ = SkRect::MakeEmpty();

  SkPoint start_point_ = SkPoint::Make(0, 0);
  SkPoint end_point_ = SkPoint::Make(0, 0);

  SkScalar start_degrees_ = 0;
  SkScalar end_degrees_ = 0;

  PaintImage image_;
  sk_sp<PaintRecord> record_;
  RecordShaderId id_ = kInvalidRecordShaderId;

  // For decoded PaintRecord shaders, specifies the scale at which the record
  // will be rasterized.
  base::Optional<gfx::SizeF> tile_scale_;

  std::vector<SkColor> colors_;
  std::vector<SkScalar> positions_;

  // The |cached_shader_| can be derived/creates from other inputs present in
  // the PaintShader but we always construct it at creation time to ensure that
  // accesses to it are thread-safe.
  sk_sp<SkShader> cached_shader_;

  ImageAnalysisState image_analysis_state_ = ImageAnalysisState::kNoAnalysis;

  DISALLOW_COPY_AND_ASSIGN(PaintShader);
};

}  // namespace cc

#endif  // CC_PAINT_PAINT_SHADER_H_
