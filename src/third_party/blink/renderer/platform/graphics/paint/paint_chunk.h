// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_PAINT_PAINT_CHUNK_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_PAINT_PAINT_CHUNK_H_

#include <iosfwd>
#include "third_party/blink/renderer/platform/geometry/float_rect.h"
#include "third_party/blink/renderer/platform/graphics/paint/display_item.h"
#include "third_party/blink/renderer/platform/graphics/paint/raster_invalidation_tracking.h"
#include "third_party/blink/renderer/platform/graphics/paint/ref_counted_property_tree_state.h"
#include "third_party/blink/renderer/platform/platform_export.h"
#include "third_party/blink/renderer/platform/wtf/allocator.h"
#include "third_party/blink/renderer/platform/wtf/forward.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

// A contiguous sequence of drawings with common paint properties.
//
// This is expected to be owned by the paint artifact which also owns the
// related drawings.
//
// This is a Slimming Paint v175+ class.
struct PLATFORM_EXPORT PaintChunk {
  DISALLOW_NEW_EXCEPT_PLACEMENT_NEW();

  using Id = DisplayItem::Id;

  enum Cacheable {
    kCacheable,
    kUncacheable,
  };

  PaintChunk(size_t begin,
             size_t end,
             const Id& id,
             const PropertyTreeState& props,
             Cacheable cacheable = kCacheable)
      : begin_index(begin),
        end_index(end),
        id(id),
        properties(props),
        outset_for_raster_effects(0),
        known_to_be_opaque(false),
        is_cacheable(cacheable == kCacheable),
        client_is_just_created(id.client.IsJustCreated()) {}

  size_t size() const {
    DCHECK_GE(end_index, begin_index);
    return end_index - begin_index;
  }

  // Check if a new PaintChunk (this) created in the latest paint matches an old
  // PaintChunk created in the previous paint.
  bool Matches(const PaintChunk& old) const {
    return old.is_cacheable && Matches(old.id);
  }

  bool Matches(const Id& other_id) const {
    if (!is_cacheable || id != other_id)
      return false;
#if DCHECK_IS_ON()
    DCHECK(id.client.IsAlive());
#endif
    // A chunk whose client is just created should not match any cached chunk,
    // even if it's id equals the old chunk's id (which may happen if this
    // chunk's client is just created at the same address of the old chunk's
    // deleted client).
    return !client_is_just_created;
  }

  // Index of the first drawing in this chunk.
  size_t begin_index;

  // Index of the first drawing not in this chunk, so that there are
  // |endIndex - beginIndex| drawings in the chunk.
  size_t end_index;

  // Identifier of this chunk. It should be unique if |is_cacheable| is true.
  // This is used to match a new chunk to a cached old chunk to track changes
  // of chunk contents, so the id should be stable across document cycles.
  Id id;

  // The paint properties which apply to this chunk.
  RefCountedPropertyTreeState properties;

  // The total bounds of this paint chunk's contents, in the coordinate space of
  // the containing transform node.
  FloatRect bounds;

  // Some raster effects can exceed |bounds| in the rasterization space. This
  // is the maximum DisplayItemClient::VisualRectOutsetForRasterEffects() of
  // all clients of items in this chunk.
  float outset_for_raster_effects;

  // True if the bounds are filled entirely with opaque contents.
  bool known_to_be_opaque : 1;

  bool is_cacheable : 1;

  bool client_is_just_created : 1;

  String ToString() const;
};

inline bool ChunkLessThanIndex(const PaintChunk& chunk, size_t index) {
  return chunk.end_index <= index;
}

inline Vector<PaintChunk>::iterator FindChunkInVectorByDisplayItemIndex(
    Vector<PaintChunk>& chunks,
    size_t index) {
  auto* chunk =
      std::lower_bound(chunks.begin(), chunks.end(), index, ChunkLessThanIndex);
  DCHECK(chunk == chunks.end() ||
         (index >= chunk->begin_index && index < chunk->end_index));
  return chunk;
}

inline Vector<PaintChunk>::const_iterator FindChunkInVectorByDisplayItemIndex(
    const Vector<PaintChunk>& chunks,
    size_t index) {
  return FindChunkInVectorByDisplayItemIndex(
      const_cast<Vector<PaintChunk>&>(chunks), index);
}

inline bool operator==(const PaintChunk& a, const PaintChunk& b) {
  return a.begin_index == b.begin_index && a.end_index == b.end_index &&
         a.id == b.id && a.properties == b.properties &&
         a.is_cacheable == b.is_cacheable;
}

inline bool operator!=(const PaintChunk& a, const PaintChunk& b) {
  return !(a == b);
}

PLATFORM_EXPORT std::ostream& operator<<(std::ostream&, const PaintChunk&);

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_GRAPHICS_PAINT_PAINT_CHUNK_H_
