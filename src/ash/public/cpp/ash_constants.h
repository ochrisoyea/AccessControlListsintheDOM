// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_PUBLIC_CPP_ASH_CONSTANTS_H_
#define ASH_PUBLIC_CPP_ASH_CONSTANTS_H_

#include "third_party/skia/include/core/SkColor.h"
#include "ui/gfx/color_palette.h"

typedef unsigned int SkColor;

namespace ash {

// In the window corners, the resize areas don't actually expand bigger, but the
// 16 px at the end of each edge triggers diagonal resizing.
constexpr int kResizeAreaCornerSize = 16;

// Ash windows do not have a traditional visible window frame. Window content
// extends to the edge of the window. We consider a small region outside the
// window bounds and an even smaller region overlapping the window to be the
// "non-client" area and use it for resizing.
constexpr int kResizeOutsideBoundsSize = 6;
constexpr int kResizeOutsideBoundsScaleForTouch = 5;
constexpr int kResizeInsideBoundsSize = 1;

// Background color used for the Chrome OS boot splash screen.
constexpr SkColor kChromeOsBootColor = SkColorSetRGB(0xfe, 0xfe, 0xfe);

// The border color of keyboard focus for launcher items and system tray.
constexpr SkColor kFocusBorderColor = SkColorSetA(gfx::kGoogleBlue500, 0x99);
constexpr int kFocusBorderThickness = 2;

constexpr int kDefaultLargeCursorSize = 64;

// The default wait time between last mouse movement and sending autoclick.
constexpr int kDefaultAutoclickDelayMs = 1000;

}  // namespace ash

#endif  // ASH_PUBLIC_CPP_ASH_CONSTANTS_H_
