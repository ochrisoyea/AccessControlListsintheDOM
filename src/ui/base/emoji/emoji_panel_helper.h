// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_BASE_EMOJI_EMOJI_PANEL_HELPER_H_
#define UI_BASE_EMOJI_EMOJI_PANEL_HELPER_H_

#include "ui/base/ui_base_export.h"

namespace ui {

// Returns whether showing the Emoji Panel is supported on this version of
// the operating system.
UI_BASE_EXPORT bool IsEmojiPanelSupported();

// Invokes the commands to show the Emoji Panel.
UI_BASE_EXPORT void ShowEmojiPanel();

}  // namespace ui

#endif  // UI_BASE_EMOJI_EMOJI_PANEL_HELPER_H_