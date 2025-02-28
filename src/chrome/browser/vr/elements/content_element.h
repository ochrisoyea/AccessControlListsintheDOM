// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_VR_ELEMENTS_CONTENT_ELEMENT_H_
#define CHROME_BROWSER_VR_ELEMENTS_CONTENT_ELEMENT_H_

#include "base/macros.h"
#include "chrome/browser/vr/elements/platform_ui_element.h"
#include "chrome/browser/vr/ui_element_renderer.h"

namespace vr {

class ContentInputDelegate;
class TextInputDelegate;

// This element hosts the texture of a web page and surfaces all frames of the
// web page to VR. It also dispatches events to the page it hosts.
// It may also surface frames of overlays on a web page which usually displayed
// by Chrome (such as infobar after download a file).
// If quad layer is set, it stops surface frames of web page but instead draws
// transparent in UI. So the web page in quad layer can be later composited to
// the UI.
class ContentElement : public PlatformUiElement {
 public:
  typedef typename base::RepeatingCallback<void(const gfx::SizeF&)>
      ScreenBoundsChangedCallback;

  ContentElement(ContentInputDelegate* delegate, ScreenBoundsChangedCallback);
  ~ContentElement() override;

  bool OnBeginFrame(const gfx::Transform& head_pose) override;
  void Render(UiElementRenderer* renderer,
              const CameraModel& model) const final;
  void OnFocusChanged(bool focused) override;
  void OnInputEdited(const EditedText& info) override;
  void OnInputCommitted(const EditedText& info) override;
  void RequestFocus() override;
  void RequestUnfocus() override;
  void UpdateInput(const EditedText& info) override;

  void SetOverlayTextureId(unsigned int texture_id);
  void SetOverlayTextureLocation(UiElementRenderer::TextureLocation location);
  void SetOverlayTextureEmpty(bool empty);
  bool GetOverlayTextureEmpty();
  void SetProjectionMatrix(const gfx::Transform& matrix);
  void SetTextInputDelegate(TextInputDelegate* text_input_delegate);
  void SetUsesQuadLayer(bool uses_quad_layer);

 private:
  TextInputDelegate* text_input_delegate_ = nullptr;
  ScreenBoundsChangedCallback bounds_changed_callback_;
  unsigned int overlay_texture_id_ = 0;
  bool overlay_texture_non_empty_ = false;
  UiElementRenderer::TextureLocation overlay_texture_location_ =
      UiElementRenderer::kTextureLocationExternal;
  gfx::SizeF last_content_screen_bounds_;
  float last_content_aspect_ratio_ = 0.0f;
  gfx::Transform projection_matrix_;
  bool focused_ = false;
  bool uses_quad_layer_ = false;
  ContentInputDelegate* content_delegate_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(ContentElement);
};

}  // namespace vr

#endif  // CHROME_BROWSER_VR_ELEMENTS_CONTENT_ELEMENT_H_
