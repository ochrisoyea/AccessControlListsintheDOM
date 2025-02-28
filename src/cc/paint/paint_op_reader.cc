// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/paint/paint_op_reader.h"

#include <stddef.h>
#include <algorithm>

#include "base/stl_util.h"
#include "cc/paint/image_transfer_cache_entry.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image_builder.h"
#include "cc/paint/paint_op_buffer.h"
#include "cc/paint/paint_shader.h"
#include "cc/paint/paint_typeface_transfer_cache_entry.h"
#include "cc/paint/path_transfer_cache_entry.h"
#include "cc/paint/shader_transfer_cache_entry.h"
#include "cc/paint/transfer_cache_deserialize_helper.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkRRect.h"
#include "third_party/skia/include/core/SkSerialProcs.h"
#include "third_party/skia/include/core/SkTextBlob.h"
#include "third_party/skia/src/core/SkRemoteGlyphCache.h"

namespace cc {
namespace {

// If we have more than this many colors, abort deserialization.
const size_t kMaxShaderColorsSupported = 10000;
const size_t kMaxMergeFilterCount = 10000;
const size_t kMaxKernelSize = 1000;
const size_t kMaxRegionByteSize = 10 * 1024;

bool IsValidPaintShaderType(PaintShader::Type type) {
  return static_cast<uint8_t>(type) <
         static_cast<uint8_t>(PaintShader::Type::kShaderCount);
}

bool IsValidSkShaderTileMode(SkShader::TileMode mode) {
  // When Skia adds Decal, update this (skbug.com/7638)
  return mode <= SkShader::kMirror_TileMode;
}

bool IsValidPaintShaderScalingBehavior(PaintShader::ScalingBehavior behavior) {
  return behavior == PaintShader::ScalingBehavior::kRasterAtScale ||
         behavior == PaintShader::ScalingBehavior::kFixedScale;
}

struct TypefaceCtx {
  explicit TypefaceCtx(SkStrikeClient* client) : client(client) {}
  bool invalid_typeface = false;
  SkStrikeClient* client = nullptr;
};

sk_sp<SkTypeface> DeserializeTypeface(const void* data,
                                      size_t length,
                                      void* ctx) {
  auto* typeface_ctx = static_cast<TypefaceCtx*>(ctx);
  auto tf = typeface_ctx->client->deserializeTypeface(data, length);
  if (tf)
    return tf;

  typeface_ctx->invalid_typeface = true;
  return nullptr;
}

}  // namespace

// static
void PaintOpReader::FixupMatrixPostSerialization(SkMatrix* matrix) {
  // Can't trust malicious clients to provide the correct derived matrix type.
  // However, if a matrix thinks that it's identity, then make it so.
  if (matrix->isIdentity())
    matrix->setIdentity();
  else
    matrix->dirtyMatrixTypeCache();
}

// static
bool PaintOpReader::ReadAndValidateOpHeader(const volatile void* input,
                                            size_t input_size,
                                            uint8_t* type,
                                            uint32_t* skip) {
  uint32_t first_word = reinterpret_cast<const volatile uint32_t*>(input)[0];
  *type = static_cast<uint8_t>(first_word & 0xFF);
  *skip = first_word >> 8;

  if (input_size < *skip)
    return false;
  if (*skip % PaintOpBuffer::PaintOpAlign != 0)
    return false;
  if (*type > static_cast<uint8_t>(PaintOpType::LastPaintOpType))
    return false;
  return true;
}

template <typename T>
void PaintOpReader::ReadSimple(T* val) {
  static_assert(base::is_trivially_copyable<T>::value,
                "Not trivially copyable");
  if (remaining_bytes_ < sizeof(T))
    SetInvalid();
  if (!valid_)
    return;

  // Most of the time this is used for primitives, but this function is also
  // used for SkRect/SkIRect/SkMatrix whose implicit operator= can't use a
  // volatile.  TOCTOU violations don't matter for these simple types so
  // use assignment.
  *val = *reinterpret_cast<const T*>(const_cast<const char*>(memory_));

  memory_ += sizeof(T);
  remaining_bytes_ -= sizeof(T);
}

template <typename T>
void PaintOpReader::ReadFlattenable(sk_sp<T>* val) {
  size_t bytes = 0;
  ReadSimple(&bytes);
  if (remaining_bytes_ < bytes)
    SetInvalid();
  if (!valid_)
    return;
  if (bytes == 0)
    return;

  // This is assumed safe from TOCTOU violations as the flattenable
  // deserializing function uses an SkReadBuffer which reads each piece of
  // memory once much like PaintOpReader does.
  DCHECK(SkIsAlign4(reinterpret_cast<uintptr_t>(memory_)));
  val->reset(static_cast<T*>(
      SkFlattenable::Deserialize(T::GetFlattenableType(),
                                 const_cast<const char*>(memory_), bytes)
          .release()));
  if (!val)
    SetInvalid();

  memory_ += bytes;
  remaining_bytes_ -= bytes;
}

void PaintOpReader::ReadData(size_t bytes, void* data) {
  if (remaining_bytes_ < bytes)
    SetInvalid();
  if (!valid_)
    return;
  if (bytes == 0)
    return;

  memcpy(data, const_cast<const char*>(memory_), bytes);
  memory_ += bytes;
  remaining_bytes_ -= bytes;
}

void PaintOpReader::ReadArray(size_t count, SkPoint* array) {
  size_t bytes = count * sizeof(SkPoint);
  if (remaining_bytes_ < bytes)
    SetInvalid();
  // Overflow?
  if (count > static_cast<size_t>(~0) / sizeof(SkPoint))
    SetInvalid();
  if (!valid_)
    return;
  if (count == 0)
    return;

  memcpy(array, const_cast<const char*>(memory_), bytes);
  memory_ += bytes;
  remaining_bytes_ -= bytes;
}

void PaintOpReader::ReadSize(size_t* size) {
  ReadSimple(size);
}

void PaintOpReader::Read(SkScalar* data) {
  ReadSimple(data);
}

void PaintOpReader::Read(uint8_t* data) {
  ReadSimple(data);
}

void PaintOpReader::Read(uint32_t* data) {
  ReadSimple(data);
}

void PaintOpReader::Read(uint64_t* data) {
  ReadSimple(data);
}

void PaintOpReader::Read(int32_t* data) {
  ReadSimple(data);
}

void PaintOpReader::Read(SkRect* rect) {
  ReadSimple(rect);
}

void PaintOpReader::Read(SkIRect* rect) {
  ReadSimple(rect);
}

void PaintOpReader::Read(SkRRect* rect) {
  ReadSimple(rect);
}

void PaintOpReader::Read(SkPath* path) {
  uint32_t transfer_cache_entry_id;
  ReadSimple(&transfer_cache_entry_id);
  if (!valid_)
    return;
  auto* entry =
      options_.transfer_cache->GetEntryAs<ServicePathTransferCacheEntry>(
          transfer_cache_entry_id);
  if (entry) {
    *path = entry->path();
  } else {
    valid_ = false;
  }
}

void PaintOpReader::Read(PaintFlags* flags) {
  Read(&flags->text_size_);
  ReadSimple(&flags->color_);
  Read(&flags->width_);
  Read(&flags->miter_limit_);
  ReadSimple(&flags->blend_mode_);
  ReadSimple(&flags->bitfields_uint_);

  // Flattenables must be read at 4-byte boundary, which should be the case
  // here.
  AlignMemory(4);
  ReadFlattenable(&flags->path_effect_);
  AlignMemory(4);
  ReadFlattenable(&flags->mask_filter_);
  AlignMemory(4);
  ReadFlattenable(&flags->color_filter_);

  AlignMemory(4);
  if (enable_security_constraints_) {
    size_t bytes = 0;
    ReadSimple(&bytes);
    if (bytes != 0u) {
      SetInvalid();
      return;
    }
  } else {
    ReadFlattenable(&flags->draw_looper_);
  }

  Read(&flags->image_filter_);
  Read(&flags->shader_);
}

void PaintOpReader::Read(PaintImage* image) {
  uint8_t serialized_type_int = 0u;
  Read(&serialized_type_int);
  if (serialized_type_int >
      static_cast<uint8_t>(PaintOp::SerializedImageType::kLastType)) {
    SetInvalid();
    return;
  }

  auto serialized_type =
      static_cast<PaintOp::SerializedImageType>(serialized_type_int);
  if (serialized_type == PaintOp::SerializedImageType::kNoImage)
    return;

  if (enable_security_constraints_) {
    switch (serialized_type) {
      case PaintOp::SerializedImageType::kNoImage:
        NOTREACHED();
        return;
      case PaintOp::SerializedImageType::kImageData: {
        SkColorType color_type;
        Read(&color_type);
        uint32_t width;
        Read(&width);
        uint32_t height;
        Read(&height);
        size_t pixel_size;
        ReadSize(&pixel_size);
        if (!valid_)
          return;

        SkImageInfo image_info =
            SkImageInfo::Make(width, height, color_type, kPremul_SkAlphaType);
        const volatile void* pixel_data = ExtractReadableMemory(pixel_size);
        if (!valid_)
          return;

        SkPixmap pixmap(image_info, const_cast<const void*>(pixel_data),
                        image_info.minRowBytes());

        *image = PaintImageBuilder::WithDefault()
                     .set_id(PaintImage::GetNextId())
                     .set_image(SkImage::MakeRasterCopy(pixmap),
                                PaintImage::kNonLazyStableId)
                     .TakePaintImage();
      }
        return;
      case PaintOp::SerializedImageType::kTransferCacheEntry:
        SetInvalid();
        return;
    }

    NOTREACHED();
    return;
  }

  if (serialized_type != PaintOp::SerializedImageType::kTransferCacheEntry) {
    SetInvalid();
    return;
  }

  uint32_t transfer_cache_entry_id;
  ReadSimple(&transfer_cache_entry_id);
  if (!valid_)
    return;

  // If we encountered a decode failure, we may write an invalid id for the
  // image. In these cases, just return, leaving the image as nullptr.
  if (transfer_cache_entry_id == kInvalidImageTransferCacheEntryId)
    return;

  if (auto* entry =
          options_.transfer_cache->GetEntryAs<ServiceImageTransferCacheEntry>(
              transfer_cache_entry_id)) {
    *image = PaintImageBuilder::WithDefault()
                 .set_id(PaintImage::GetNextId())
                 .set_image(entry->image(), PaintImage::kNonLazyStableId)
                 .TakePaintImage();
  }
}

void PaintOpReader::Read(sk_sp<SkData>* data) {
  size_t bytes = 0;
  ReadSimple(&bytes);
  if (remaining_bytes_ < bytes)
    SetInvalid();
  if (!valid_)
    return;

  // Separate out empty vs not valid cases.
  if (bytes == 0) {
    bool has_data = false;
    Read(&has_data);
    if (has_data)
      *data = SkData::MakeEmpty();
    return;
  }

  // This is safe to cast away the volatile as it is just a memcpy internally.
  *data = SkData::MakeWithCopy(const_cast<const char*>(memory_), bytes);

  memory_ += bytes;
  remaining_bytes_ -= bytes;
}

void PaintOpReader::Read(sk_sp<SkColorSpace>* color_space) {
  size_t size = 0;
  ReadSize(&size);
  if (remaining_bytes_ < size)
    valid_ = false;
  if (!valid_ || size == 0)
    return;

  // To avoid TOCTOU issues, make a copy of this prior to turning it
  // into an SkColorSpace.  SkColorSpace::Deserialize reads header
  // fields multiple times, so is not safe to pass memory_ to directly.
  std::unique_ptr<char[]> data(new char[size]);
  memcpy(data.get(), const_cast<const char*>(memory_), size);

  *color_space = SkColorSpace::Deserialize(data.get(), size);
  // If this had non-zero bytes, it should be a valid color space.
  if (!color_space)
    SetInvalid();

  memory_ += size;
  remaining_bytes_ -= size;
}

void PaintOpReader::Read(scoped_refptr<PaintTextBlob>* paint_blob) {
  size_t data_bytes = 0u;
  ReadSimple(&data_bytes);
  if (remaining_bytes_ < data_bytes || data_bytes == 0u)
    SetInvalid();
  if (!valid_)
    return;

  SkDeserialProcs procs;
  TypefaceCtx typeface_ctx(options_.strike_client);
  procs.fTypefaceProc = &DeserializeTypeface;
  procs.fTypefaceCtx = &typeface_ctx;
  sk_sp<SkTextBlob> blob = SkTextBlob::Deserialize(
      const_cast<const char*>(memory_), data_bytes, procs);
  if (typeface_ctx.invalid_typeface) {
    SetInvalid();
    return;
  }

  *paint_blob = base::MakeRefCounted<PaintTextBlob>(
      std::move(blob), std::vector<PaintTypeface>());
  memory_ += data_bytes;
  remaining_bytes_ -= data_bytes;
}

void PaintOpReader::Read(sk_sp<PaintShader>* shader) {
  bool has_shader = false;
  ReadSimple(&has_shader);
  if (!has_shader) {
    *shader = nullptr;
    return;
  }
  PaintShader::Type shader_type;
  ReadSimple(&shader_type);
  // Avoid creating a shader if something is invalid.
  if (!valid_ || !IsValidPaintShaderType(shader_type)) {
    SetInvalid();
    return;
  }

  *shader = sk_sp<PaintShader>(new PaintShader(shader_type));
  PaintShader& ref = **shader;
  ReadSimple(&ref.flags_);
  ReadSimple(&ref.end_radius_);
  ReadSimple(&ref.start_radius_);
  ReadSimple(&ref.tx_);
  ReadSimple(&ref.ty_);
  if (!IsValidSkShaderTileMode(ref.tx_) || !IsValidSkShaderTileMode(ref.ty_))
    SetInvalid();
  ReadSimple(&ref.fallback_color_);
  ReadSimple(&ref.scaling_behavior_);
  if (!IsValidPaintShaderScalingBehavior(ref.scaling_behavior_))
    SetInvalid();
  bool has_local_matrix = false;
  ReadSimple(&has_local_matrix);
  if (has_local_matrix) {
    ref.local_matrix_.emplace();
    Read(&*ref.local_matrix_);
  }
  ReadSimple(&ref.center_);
  ReadSimple(&ref.tile_);
  ReadSimple(&ref.start_point_);
  ReadSimple(&ref.end_point_);
  ReadSimple(&ref.start_degrees_);
  ReadSimple(&ref.end_degrees_);
  Read(&ref.image_);
  bool has_record = false;
  ReadSimple(&has_record);
  uint32_t shader_id = PaintShader::kInvalidRecordShaderId;
  size_t shader_size = 0;
  if (has_record) {
    Read(&shader_id);

    // Track dependent transfer cache entries to make cached shader size
    // more realistic.
    size_t pre_size = options_.transfer_cache->GetTotalEntrySizes();
    size_t record_size = Read(&ref.record_);
    size_t post_size = options_.transfer_cache->GetTotalEntrySizes();
    shader_size = post_size - pre_size + record_size;
  }
  decltype(ref.colors_)::size_type colors_size = 0;
  ReadSimple(&colors_size);

  // If there are too many colors, abort.
  if (colors_size > kMaxShaderColorsSupported) {
    SetInvalid();
    return;
  }
  size_t colors_bytes = colors_size * sizeof(SkColor);
  if (colors_bytes > remaining_bytes_) {
    SetInvalid();
    return;
  }
  ref.colors_.resize(colors_size);
  ReadData(colors_bytes, ref.colors_.data());

  decltype(ref.positions_)::size_type positions_size = 0;
  ReadSimple(&positions_size);
  // Positions are optional. If they exist, they have the same count as colors.
  if (positions_size > 0 && positions_size != colors_size) {
    SetInvalid();
    return;
  }
  size_t positions_bytes = positions_size * sizeof(SkScalar);
  if (positions_bytes > remaining_bytes_) {
    SetInvalid();
    return;
  }
  ref.positions_.resize(positions_size);
  ReadData(positions_size * sizeof(SkScalar), ref.positions_.data());

  // We don't write the cached shader, so don't attempt to read it either.

  if (!(*shader)->IsValid()) {
    SetInvalid();
    return;
  }

  if (shader_id == PaintShader::kInvalidRecordShaderId) {
    // Paint record shaders must have ids.
    if (shader_type == PaintShader::Type::kPaintRecord) {
      SetInvalid();
      return;
    }
    (*shader)->CreateSkShader();
    return;
  }

  // Record shaders have shader ids.  Attempt to use cached versions of
  // these so that Skia can cache based on SkPictureShader::fUniqueId.
  // These shaders are always serialized (and assumed to not be large
  // records).  Handling this edge case in this roundabout way prevents
  // transfer cache entries from needing to depend on other transfer cache
  // entries.
  auto* entry =
      options_.transfer_cache->GetEntryAs<ServiceShaderTransferCacheEntry>(
          shader_id);
  // Only consider entries that use the same scale and color space.
  // This limits the service side transfer cache to only having one entry
  // per shader but this will hit the common case of enabling Skia reuse.
  if (entry && entry->shader()->tile_ == ref.tile_ &&
      entry->raster_color_space_id() == options_.raster_color_space_id) {
    DCHECK(!ref.cached_shader_);
    ref.cached_shader_ = entry->shader()->GetSkShader();
  } else {
    ref.CreateSkShader();
    std::unique_ptr<ServiceShaderTransferCacheEntry> entry(
        new ServiceShaderTransferCacheEntry(
            *shader, options_.raster_color_space_id, shader_size));
    options_.transfer_cache->CreateLocalEntry(shader_id, std::move(entry));
  }
}

void PaintOpReader::Read(SkMatrix* matrix) {
  ReadSimple(matrix);
  FixupMatrixPostSerialization(matrix);
}

void PaintOpReader::Read(SkColorType* color_type) {
  uint32_t raw_color_type = kUnknown_SkColorType;
  ReadSimple(&raw_color_type);

  if (raw_color_type > kLastEnum_SkColorType) {
    SetInvalid();
    return;
  }

  *color_type = static_cast<SkColorType>(raw_color_type);
}

void PaintOpReader::AlignMemory(size_t alignment) {
  // Due to the math below, alignment must be a power of two.
  DCHECK_GT(alignment, 0u);
  DCHECK_EQ(alignment & (alignment - 1), 0u);

  uintptr_t memory = reinterpret_cast<uintptr_t>(memory_);
  // The following is equivalent to:
  //   padding = (alignment - memory % alignment) % alignment;
  // because alignment is a power of two. This doesn't use modulo operator
  // however, since it can be slow.
  size_t padding = ((memory + alignment - 1) & ~(alignment - 1)) - memory;
  if (padding > remaining_bytes_)
    SetInvalid();

  memory_ += padding;
  remaining_bytes_ -= padding;
}

inline void PaintOpReader::SetInvalid() {
  valid_ = false;
}

const volatile void* PaintOpReader::ExtractReadableMemory(size_t bytes) {
  if (remaining_bytes_ < bytes)
    SetInvalid();
  if (!valid_)
    return nullptr;
  if (bytes == 0)
    return nullptr;

  const volatile void* extracted_memory = memory_;
  memory_ += bytes;
  remaining_bytes_ -= bytes;
  return extracted_memory;
}

void PaintOpReader::Read(sk_sp<PaintFilter>* filter) {
  uint32_t type_int = 0;
  ReadSimple(&type_int);
  if (type_int > static_cast<uint32_t>(PaintFilter::Type::kMaxFilterType))
    SetInvalid();
  if (!valid_)
    return;

  auto type = static_cast<PaintFilter::Type>(type_int);
  if (type == PaintFilter::Type::kNullFilter) {
    *filter = nullptr;
    return;
  }

  uint32_t has_crop_rect = 0;
  base::Optional<PaintFilter::CropRect> crop_rect;
  ReadSimple(&has_crop_rect);
  if (has_crop_rect) {
    uint32_t flags = 0;
    SkRect rect = SkRect::MakeEmpty();
    ReadSimple(&flags);
    ReadSimple(&rect);
    crop_rect.emplace(rect, flags);
  }

  AlignMemory(4);
  switch (type) {
    case PaintFilter::Type::kNullFilter:
      NOTREACHED();
      break;
    case PaintFilter::Type::kColorFilter:
      ReadColorFilterPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kBlur:
      ReadBlurPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kDropShadow:
      ReadDropShadowPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kMagnifier:
      ReadMagnifierPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kCompose:
      ReadComposePaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kAlphaThreshold:
      ReadAlphaThresholdPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kXfermode:
      ReadXfermodePaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kArithmetic:
      ReadArithmeticPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kMatrixConvolution:
      ReadMatrixConvolutionPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kDisplacementMapEffect:
      ReadDisplacementMapEffectPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kImage:
      ReadImagePaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kPaintRecord:
      ReadRecordPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kMerge:
      ReadMergePaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kMorphology:
      ReadMorphologyPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kOffset:
      ReadOffsetPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kTile:
      ReadTilePaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kTurbulence:
      ReadTurbulencePaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kPaintFlags:
      ReadPaintFlagsPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kMatrix:
      ReadMatrixPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kLightingDistant:
      ReadLightingDistantPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kLightingPoint:
      ReadLightingPointPaintFilter(filter, crop_rect);
      break;
    case PaintFilter::Type::kLightingSpot:
      ReadLightingSpotPaintFilter(filter, crop_rect);
      break;
  }
}

void PaintOpReader::ReadColorFilterPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  sk_sp<SkColorFilter> color_filter;
  sk_sp<PaintFilter> input;

  ReadFlattenable(&color_filter);
  Read(&input);
  if (!color_filter)
    SetInvalid();
  if (!valid_)
    return;
  filter->reset(new ColorFilterPaintFilter(std::move(color_filter),
                                           std::move(input),
                                           base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadBlurPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkScalar sigma_x = 0.f;
  SkScalar sigma_y = 0.f;
  BlurPaintFilter::TileMode tile_mode = SkBlurImageFilter::kClamp_TileMode;
  sk_sp<PaintFilter> input;

  Read(&sigma_x);
  Read(&sigma_y);
  ReadSimple(&tile_mode);
  Read(&input);
  if (!valid_)
    return;
  filter->reset(new BlurPaintFilter(sigma_x, sigma_y, tile_mode,
                                    std::move(input),
                                    base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadDropShadowPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkScalar dx = 0.f;
  SkScalar dy = 0.f;
  SkScalar sigma_x = 0.f;
  SkScalar sigma_y = 0.f;
  SkColor color = SK_ColorBLACK;
  DropShadowPaintFilter::ShadowMode shadow_mode =
      SkDropShadowImageFilter::kDrawShadowAndForeground_ShadowMode;
  sk_sp<PaintFilter> input;

  Read(&dx);
  Read(&dy);
  Read(&sigma_x);
  Read(&sigma_y);
  Read(&color);
  ReadSimple(&shadow_mode);
  Read(&input);

  if (shadow_mode > SkDropShadowImageFilter::kLast_ShadowMode)
    SetInvalid();
  if (!valid_)
    return;
  filter->reset(new DropShadowPaintFilter(dx, dy, sigma_x, sigma_y, color,
                                          shadow_mode, std::move(input),
                                          base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadMagnifierPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkRect src_rect = SkRect::MakeEmpty();
  SkScalar inset = 0.f;
  sk_sp<PaintFilter> input;

  Read(&src_rect);
  Read(&inset);
  Read(&input);
  if (!valid_)
    return;
  filter->reset(new MagnifierPaintFilter(src_rect, inset, std::move(input),
                                         base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadComposePaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  sk_sp<PaintFilter> outer;
  sk_sp<PaintFilter> inner;

  Read(&outer);
  Read(&inner);
  if (!valid_)
    return;
  filter->reset(new ComposePaintFilter(std::move(outer), std::move(inner)));
}

void PaintOpReader::ReadAlphaThresholdPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkRegion region;
  SkScalar inner_min = 0.f;
  SkScalar outer_max = 0.f;
  sk_sp<PaintFilter> input;

  Read(&region);
  ReadSimple(&inner_min);
  ReadSimple(&outer_max);
  Read(&input);
  if (!valid_)
    return;
  filter->reset(new AlphaThresholdPaintFilter(
      region, inner_min, outer_max, std::move(input),
      base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadXfermodePaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  uint32_t blend_mode_int = 0;
  sk_sp<PaintFilter> background;
  sk_sp<PaintFilter> foreground;

  Read(&blend_mode_int);
  Read(&background);
  Read(&foreground);
  SkBlendMode blend_mode = SkBlendMode::kClear;
  if (blend_mode_int > static_cast<uint32_t>(SkBlendMode::kLastMode))
    SetInvalid();
  if (!valid_)
    return;
  blend_mode = static_cast<SkBlendMode>(blend_mode_int);

  filter->reset(new XfermodePaintFilter(blend_mode, std::move(background),
                                        std::move(foreground),
                                        base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadArithmeticPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  float k1 = 0.f;
  float k2 = 0.f;
  float k3 = 0.f;
  float k4 = 0.f;
  bool enforce_pm_color = false;
  sk_sp<PaintFilter> background;
  sk_sp<PaintFilter> foreground;
  Read(&k1);
  Read(&k2);
  Read(&k3);
  Read(&k4);
  Read(&enforce_pm_color);
  Read(&background);
  Read(&foreground);
  if (!valid_)
    return;
  filter->reset(new ArithmeticPaintFilter(
      k1, k2, k3, k4, enforce_pm_color, std::move(background),
      std::move(foreground), base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadMatrixConvolutionPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkISize kernel_size = SkISize::MakeEmpty();
  SkScalar gain = 0.f;
  SkScalar bias = 0.f;
  SkIPoint kernel_offset = SkIPoint::Make(0, 0);
  uint32_t tile_mode_int = 0;
  bool convolve_alpha = false;
  sk_sp<PaintFilter> input;

  ReadSimple(&kernel_size);
  if (!valid_)
    return;
  auto size =
      static_cast<size_t>(sk_64_mul(kernel_size.width(), kernel_size.height()));
  if (size > kMaxKernelSize) {
    SetInvalid();
    return;
  }
  std::vector<SkScalar> kernel(size);
  for (size_t i = 0; i < size; ++i)
    Read(&kernel[i]);
  Read(&gain);
  Read(&bias);
  ReadSimple(&kernel_offset);
  Read(&tile_mode_int);
  Read(&convolve_alpha);
  Read(&input);
  if (tile_mode_int > SkMatrixConvolutionImageFilter::kMax_TileMode)
    SetInvalid();
  if (!valid_)
    return;
  MatrixConvolutionPaintFilter::TileMode tile_mode =
      static_cast<MatrixConvolutionPaintFilter::TileMode>(tile_mode_int);
  filter->reset(new MatrixConvolutionPaintFilter(
      kernel_size, kernel.data(), gain, bias, kernel_offset, tile_mode,
      convolve_alpha, std::move(input), base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadDisplacementMapEffectPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  // Unknown, R, G, B, A: max type is 4.
  static const int kMaxChannelSelectorType = 4;

  uint32_t channel_x_int = 0;
  uint32_t channel_y_int = 0;
  SkScalar scale = 0.f;
  sk_sp<PaintFilter> displacement;
  sk_sp<PaintFilter> color;

  Read(&channel_x_int);
  Read(&channel_y_int);
  Read(&scale);
  Read(&displacement);
  Read(&color);

  if (channel_x_int > kMaxChannelSelectorType ||
      channel_y_int > kMaxChannelSelectorType) {
    SetInvalid();
  }
  if (!valid_)
    return;
  DisplacementMapEffectPaintFilter::ChannelSelectorType channel_x =
      static_cast<DisplacementMapEffectPaintFilter::ChannelSelectorType>(
          channel_x_int);
  DisplacementMapEffectPaintFilter::ChannelSelectorType channel_y =
      static_cast<DisplacementMapEffectPaintFilter::ChannelSelectorType>(
          channel_y_int);
  filter->reset(new DisplacementMapEffectPaintFilter(
      channel_x, channel_y, scale, std::move(displacement), std::move(color),
      base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadImagePaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  PaintImage image;
  Read(&image);
  if (!image) {
    SetInvalid();
    return;
  }

  SkRect src_rect;
  Read(&src_rect);
  SkRect dst_rect;
  Read(&dst_rect);
  SkFilterQuality quality;
  Read(&quality);

  if (!valid_)
    return;
  filter->reset(
      new ImagePaintFilter(std::move(image), src_rect, dst_rect, quality));
}

void PaintOpReader::ReadRecordPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkRect record_bounds;
  sk_sp<PaintRecord> record;
  Read(&record_bounds);
  Read(&record);
  if (!valid_)
    return;
  filter->reset(new RecordPaintFilter(std::move(record), record_bounds));
}

void PaintOpReader::ReadMergePaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  size_t input_count = 0;
  ReadSimple(&input_count);
  if (input_count > kMaxMergeFilterCount)
    SetInvalid();
  if (!valid_)
    return;
  std::vector<sk_sp<PaintFilter>> inputs(input_count);
  for (auto& input : inputs)
    Read(&input);
  if (!valid_)
    return;
  filter->reset(new MergePaintFilter(inputs.data(),
                                     static_cast<int>(input_count),
                                     base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadMorphologyPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  uint32_t morph_type_int = 0;
  int radius_x = 0;
  int radius_y = 0;
  sk_sp<PaintFilter> input;
  Read(&morph_type_int);
  Read(&radius_x);
  Read(&radius_y);
  Read(&input);
  if (morph_type_int >
      static_cast<uint32_t>(MorphologyPaintFilter::MorphType::kMaxMorphType)) {
    SetInvalid();
  }
  if (!valid_)
    return;
  MorphologyPaintFilter::MorphType morph_type =
      static_cast<MorphologyPaintFilter::MorphType>(morph_type_int);
  filter->reset(new MorphologyPaintFilter(morph_type, radius_x, radius_y,
                                          std::move(input),
                                          base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadOffsetPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkScalar dx = 0.f;
  SkScalar dy = 0.f;
  sk_sp<PaintFilter> input;

  Read(&dx);
  Read(&dy);
  Read(&input);
  if (!valid_)
    return;
  filter->reset(new OffsetPaintFilter(dx, dy, std::move(input),
                                      base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadTilePaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkRect src = SkRect::MakeEmpty();
  SkRect dst = SkRect::MakeEmpty();
  sk_sp<PaintFilter> input;

  Read(&src);
  Read(&dst);
  Read(&input);
  if (!valid_)
    return;
  filter->reset(new TilePaintFilter(src, dst, std::move(input)));
}

void PaintOpReader::ReadTurbulencePaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  uint32_t turbulence_type_int = 0;
  SkScalar base_frequency_x = 0.f;
  SkScalar base_frequency_y = 0.f;
  int num_octaves = 0;
  SkScalar seed = 0.f;
  SkISize tile_size = SkISize::MakeEmpty();

  Read(&turbulence_type_int);
  Read(&base_frequency_x);
  Read(&base_frequency_y);
  Read(&num_octaves);
  Read(&seed);
  ReadSimple(&tile_size);
  if (turbulence_type_int >
      static_cast<uint32_t>(
          TurbulencePaintFilter::TurbulenceType::kMaxTurbulenceType)) {
    SetInvalid();
  }
  if (!valid_)
    return;
  TurbulencePaintFilter::TurbulenceType turbulence_type =
      static_cast<TurbulencePaintFilter::TurbulenceType>(turbulence_type_int);
  filter->reset(new TurbulencePaintFilter(
      turbulence_type, base_frequency_x, base_frequency_y, num_octaves, seed,
      &tile_size, base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadPaintFlagsPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  AlignMemory(4);
  PaintFlags flags;
  Read(&flags);
  if (!valid_)
    return;
  filter->reset(
      new PaintFlagsPaintFilter(flags, base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadMatrixPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  SkMatrix matrix = SkMatrix::I();
  SkFilterQuality filter_quality = kNone_SkFilterQuality;
  sk_sp<PaintFilter> input;

  Read(&matrix);
  ReadSimple(&filter_quality);
  Read(&input);
  if (filter_quality > kLast_SkFilterQuality)
    SetInvalid();
  if (!valid_)
    return;
  filter->reset(
      new MatrixPaintFilter(matrix, filter_quality, std::move(input)));
}

void PaintOpReader::ReadLightingDistantPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  uint32_t lighting_type_int = 0;
  SkPoint3 direction = SkPoint3::Make(0.f, 0.f, 0.f);
  SkColor light_color = SK_ColorBLACK;
  SkScalar surface_scale = 0.f;
  SkScalar kconstant = 0.f;
  SkScalar shininess = 0.f;
  sk_sp<PaintFilter> input;

  Read(&lighting_type_int);
  ReadSimple(&direction);
  Read(&light_color);
  Read(&surface_scale);
  Read(&kconstant);
  Read(&shininess);
  Read(&input);
  if (lighting_type_int >
      static_cast<uint32_t>(PaintFilter::LightingType::kMaxLightingType)) {
    SetInvalid();
  }
  if (!valid_)
    return;
  PaintFilter::LightingType lighting_type =
      static_cast<PaintFilter::LightingType>(lighting_type_int);
  filter->reset(new LightingDistantPaintFilter(
      lighting_type, direction, light_color, surface_scale, kconstant,
      shininess, std::move(input), base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadLightingPointPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  uint32_t lighting_type_int = 0;
  SkPoint3 location = SkPoint3::Make(0.f, 0.f, 0.f);
  SkColor light_color = SK_ColorBLACK;
  SkScalar surface_scale = 0.f;
  SkScalar kconstant = 0.f;
  SkScalar shininess = 0.f;
  sk_sp<PaintFilter> input;

  Read(&lighting_type_int);
  ReadSimple(&location);
  Read(&light_color);
  Read(&surface_scale);
  Read(&kconstant);
  Read(&shininess);
  Read(&input);
  if (lighting_type_int >
      static_cast<uint32_t>(PaintFilter::LightingType::kMaxLightingType)) {
    SetInvalid();
  }
  if (!valid_)
    return;
  PaintFilter::LightingType lighting_type =
      static_cast<PaintFilter::LightingType>(lighting_type_int);
  filter->reset(new LightingPointPaintFilter(
      lighting_type, location, light_color, surface_scale, kconstant, shininess,
      std::move(input), base::OptionalOrNullptr(crop_rect)));
}

void PaintOpReader::ReadLightingSpotPaintFilter(
    sk_sp<PaintFilter>* filter,
    const base::Optional<PaintFilter::CropRect>& crop_rect) {
  uint32_t lighting_type_int = 0;
  SkPoint3 location = SkPoint3::Make(0.f, 0.f, 0.f);
  SkPoint3 target = SkPoint3::Make(0.f, 0.f, 0.f);
  SkScalar specular_exponent = 0.f;
  SkScalar cutoff_angle = 0.f;
  SkColor light_color = SK_ColorBLACK;
  SkScalar surface_scale = 0.f;
  SkScalar kconstant = 0.f;
  SkScalar shininess = 0.f;
  sk_sp<PaintFilter> input;

  Read(&lighting_type_int);
  ReadSimple(&location);
  ReadSimple(&target);
  Read(&specular_exponent);
  Read(&cutoff_angle);
  Read(&light_color);
  Read(&surface_scale);
  Read(&kconstant);
  Read(&shininess);
  Read(&input);

  if (lighting_type_int >
      static_cast<uint32_t>(PaintFilter::LightingType::kMaxLightingType)) {
    SetInvalid();
  }
  if (!valid_)
    return;
  PaintFilter::LightingType lighting_type =
      static_cast<PaintFilter::LightingType>(lighting_type_int);
  filter->reset(new LightingSpotPaintFilter(
      lighting_type, location, target, specular_exponent, cutoff_angle,
      light_color, surface_scale, kconstant, shininess, std::move(input),
      base::OptionalOrNullptr(crop_rect)));
}

size_t PaintOpReader::Read(sk_sp<PaintRecord>* record) {
  size_t size_bytes = 0;
  ReadSimple(&size_bytes);
  AlignMemory(PaintOpBuffer::PaintOpAlign);

  if (enable_security_constraints_) {
    // Validate that the record was not serialized if security constraints are
    // enabled.
    if (size_bytes != 0) {
      SetInvalid();
      return 0;
    }
    *record = sk_make_sp<PaintOpBuffer>();
    return 0;
  }

  if (size_bytes > remaining_bytes_)
    SetInvalid();
  if (!valid_)
    return 0;

  *record = PaintOpBuffer::MakeFromMemory(memory_, size_bytes, options_);
  if (!*record) {
    SetInvalid();
    return 0;
  }
  memory_ += size_bytes;
  remaining_bytes_ -= size_bytes;
  return size_bytes;
}

void PaintOpReader::Read(SkRegion* region) {
  size_t region_bytes = 0;
  ReadSimple(&region_bytes);
  if (region_bytes == 0 || region_bytes > kMaxRegionByteSize)
    SetInvalid();
  if (!valid_)
    return;
  std::unique_ptr<char[]> data(new char[region_bytes]);
  ReadData(region_bytes, data.get());
  if (!valid_)
    return;
  size_t result = region->readFromMemory(data.get(), region_bytes);
  if (!result)
    SetInvalid();
}

}  // namespace cc
