// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_FRAME_VISUAL_PROPERTIES_H_
#define CONTENT_COMMON_FRAME_VISUAL_PROPERTIES_H_

#include "content/common/content_export.h"
#include "content/public/common/screen_info.h"
#include "third_party/blink/public/platform/web_display_mode.h"
#include "ui/gfx/geometry/size.h"

namespace content {

// TODO(fsamuel): We might want to unify this with content::ResizeParams.
struct CONTENT_EXPORT FrameVisualProperties {
  FrameVisualProperties();
  FrameVisualProperties(const FrameVisualProperties& other);
  ~FrameVisualProperties();

  FrameVisualProperties& operator=(const FrameVisualProperties& other);

  // Information about the screen (dpi, depth, etc..).
  ScreenInfo screen_info;

  // Whether or not blink should be in auto-resize mode.
  bool auto_resize_enabled = false;

  // The minimum size for Blink if auto-resize is enabled.
  gfx::Size min_size_for_auto_resize;

  // The maximum size for Blink if auto-resize is enabled.
  gfx::Size max_size_for_auto_resize;

  gfx::Rect screen_space_rect;

  gfx::Size local_frame_size;

  uint32_t capture_sequence_number = 0u;
};

}  // namespace content

#endif  // CONTENT_COMMON_FRAME_VISUAL_PROPERTIES_H_
