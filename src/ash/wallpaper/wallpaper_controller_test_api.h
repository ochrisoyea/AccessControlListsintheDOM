// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_WALLPAPER_WALLPAPER_CONTROLLER_TEST_API_H_
#define ASH_WALLPAPER_WALLPAPER_CONTROLLER_TEST_API_H_

#include "ash/ash_export.h"
#include "base/macros.h"
#include "third_party/skia/include/core/SkColor.h"

namespace ash {

class WallpaperController;

class ASH_EXPORT WallpaperControllerTestApi {
 public:
  explicit WallpaperControllerTestApi(WallpaperController* controller);
  virtual ~WallpaperControllerTestApi();

  // Creates and sets a new wallpaper that cause the prominent color of the
  // |controller_| to be a valid (i.e. not kInvalidWallpaperColor) color. The
  // WallpaperControllerObservers should be notified as well. This assumes the
  // default DARK-MUTED luma-saturation ranges are in effect.
  //
  // The expected prominent color is returned.
  SkColor ApplyColorProducingWallpaper();

  // Simulates the fullscreen wallpaper preview mode.
  void StartWallpaperPreview();

 private:
  WallpaperController* controller_;

  DISALLOW_COPY_AND_ASSIGN(WallpaperControllerTestApi);
};

}  // namespace ash

#endif  // ASH_WALLPAPER_WALLPAPER_CONTROLLER_TEST_API_H_
