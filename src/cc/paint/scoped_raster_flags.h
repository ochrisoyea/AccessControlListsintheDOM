// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_SCOPED_RASTER_FLAGS_H_
#define CC_PAINT_SCOPED_RASTER_FLAGS_H_

#include "base/containers/stack_container.h"
#include "base/macros.h"
#include "cc/paint/decode_stashing_image_provider.h"
#include "cc/paint/paint_export.h"
#include "cc/paint/paint_flags.h"

namespace cc {

// A helper class to modify the flags for raster. This includes alpha folding
// from SaveLayers and decoding images.
class CC_PAINT_EXPORT ScopedRasterFlags {
 public:
  // |flags| and |image_provider| must outlive this class.
  // |is_rasterizing| is true if these flags are for direct rasterization
  // and false if these flags are being used for serialization.
  ScopedRasterFlags(const PaintFlags* flags,
                    ImageProvider* image_provider,
                    const SkMatrix& ctm,
                    uint8_t alpha,
                    bool is_rasterizing);
  ~ScopedRasterFlags();

  // The usage of these flags should not extend beyond the lifetime of this
  // object.
  const PaintFlags* flags() const {
    if (decode_failed_)
      return nullptr;

    return modified_flags_ ? &*modified_flags_ : original_flags_;
  }

 private:
  void DecodeImageShader(const SkMatrix& ctm);
  void DecodeRecordShader(const SkMatrix& ctm, bool is_rasterizing);
  void DecodeFilter();

  void AdjustStrokeIfNeeded(const SkMatrix& ctm);

  PaintFlags* MutableFlags() {
    if (!modified_flags_)
      modified_flags_.emplace(*original_flags_);
    return &*modified_flags_;
  }

  const PaintFlags* original_flags_;
  base::Optional<PaintFlags> modified_flags_;
  base::Optional<DecodeStashingImageProvider> decode_stashing_image_provider_;
  bool decode_failed_ = false;

  DISALLOW_COPY_AND_ASSIGN(ScopedRasterFlags);
};

}  // namespace cc

#endif  // CC_PAINT_SCOPED_RASTER_FLAGS_H_
