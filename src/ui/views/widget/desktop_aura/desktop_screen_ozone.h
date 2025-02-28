// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_VIEWS_WIDGET_DESKTOP_AURA_DESKTOP_SCREEN_OZONE_H_
#define UI_VIEWS_WIDGET_DESKTOP_AURA_DESKTOP_SCREEN_OZONE_H_

#include "ui/display/screen_base.h"

namespace views {

class DesktopScreenOzone : public display::ScreenBase {
 public:
  DesktopScreenOzone();
  ~DesktopScreenOzone() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(DesktopScreenOzone);
};

}  // namespace views

#endif  // UI_VIEWS_WIDGET_DESKTOP_AURA_DESKTOP_SCREEN_OZONE_H_
