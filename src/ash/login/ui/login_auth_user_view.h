// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_LOGIN_UI_LOGIN_AUTH_USER_VIEW_H_
#define ASH_LOGIN_UI_LOGIN_AUTH_USER_VIEW_H_

#include <stdint.h>
#include <memory>

#include "ash/ash_export.h"
#include "ash/login/ui/login_password_view.h"
#include "ash/login/ui/login_user_view.h"
#include "ash/login/ui/non_accessible_view.h"
#include "ash/public/interfaces/user_info.mojom.h"
#include "base/memory/weak_ptr.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/view.h"

namespace views {
class LabelButton;
}

namespace ash {

class LoginPasswordView;
class LoginPinView;

// Wraps a UserView which also has authentication available. Adds additional
// views below the UserView instance which show authentication UIs.
//
// This class will make call mojo authentication APIs directly. The embedder can
// receive some events about the results of those mojo
// authentication attempts (ie, success/failure).
class ASH_EXPORT LoginAuthUserView : public NonAccessibleView,
                                     public views::ButtonListener {
 public:
  // TestApi is used for tests to get internal implementation details.
  class ASH_EXPORT TestApi {
   public:
    explicit TestApi(LoginAuthUserView* view);
    ~TestApi();

    LoginUserView* user_view() const;
    LoginPasswordView* password_view() const;
    LoginPinView* pin_view() const;
    views::Button* online_sign_in_message() const;

   private:
    LoginAuthUserView* const view_;
  };

  using OnAuthCallback = base::RepeatingCallback<void(bool auth_success)>;
  using OnEasyUnlockIconTapped = base::RepeatingClosure;
  using OnEasyUnlockIconHovered = base::RepeatingClosure;

  struct Callbacks {
    Callbacks();
    Callbacks(const Callbacks& other);
    ~Callbacks();

    // Executed whenever an authentication result is available, such as when the
    // user submits a password or taps the user icon when AUTH_TAP is enabled.
    OnAuthCallback on_auth;
    // Called when the user taps the user view and AUTH_TAP is not enabled.
    LoginUserView::OnTap on_tap;
    // Called when the remove user warning message has been shown.
    LoginUserView::OnRemoveWarningShown on_remove_warning_shown;
    // Called when the user should be removed. The callback should do the actual
    // removal.
    LoginUserView::OnRemove on_remove;
    // Called when the easy unlock icon is hovered.
    OnEasyUnlockIconHovered on_easy_unlock_icon_hovered;
    // Called when the easy unlock icon is tapped.
    OnEasyUnlockIconTapped on_easy_unlock_icon_tapped;
  };

  // Flags which describe the set of currently visible auth methods.
  enum AuthMethods {
    AUTH_NONE = 0,                 // No extra auth methods.
    AUTH_PASSWORD = 1 << 0,        // Display password.
    AUTH_PIN = 1 << 1,             // Display PIN keyboard.
    AUTH_TAP = 1 << 2,             // Tap to unlock.
    AUTH_ONLINE_SIGN_IN = 1 << 3,  // Force online sign-in.
    AUTH_FINGERPRINT = 1 << 4,     // Use fingerprint to unlock.
  };

  LoginAuthUserView(const mojom::LoginUserInfoPtr& user,
                    const Callbacks& callbacks);
  ~LoginAuthUserView() override;

  // Set the displayed set of auth methods. |auth_methods| contains or-ed
  // together AuthMethod values.
  void SetAuthMethods(uint32_t auth_methods);
  AuthMethods auth_methods() const { return auth_methods_; }

  // Add an easy unlock icon.
  void SetEasyUnlockIcon(mojom::EasyUnlockIconId id,
                         const base::string16& accessibility_label);

  // Captures any metadata about the current view state that will be used for
  // animation.
  void CaptureStateForAnimationPreLayout();
  // Applies animation based on current layout state compared to the most
  // recently captured state.
  void ApplyAnimationPostLayout();

  // Update the displayed name, icon, etc to that of |user|.
  void UpdateForUser(const mojom::LoginUserInfoPtr& user);

  void SetFingerprintState(mojom::FingerprintUnlockState state);

  const mojom::LoginUserInfoPtr& current_user() const;

  LoginPasswordView* password_view() { return password_view_; }
  LoginUserView* user_view() { return user_view_; }

  // views::View:
  gfx::Size CalculatePreferredSize() const override;
  void RequestFocus() override;

  // views::ButtonListener:
  void ButtonPressed(views::Button* sender, const ui::Event& event) override;

 private:
  struct AnimationState;
  class FingerprintView;

  // Called when the user submits an auth method. Runs mojo call.
  void OnAuthSubmit(const base::string16& password);
  // Called with the result of the request started in |OnAuthSubmit|.
  void OnAuthComplete(base::Optional<bool> auth_success);

  // Called when the user view has been tapped. This will run |on_auth_| if tap
  // to unlock is enabled, or run |OnOnlineSignInMessageTap| if the online
  // sign-in message is shown, otherwise it will run |on_tap_|.
  void OnUserViewTap();

  // Called when the online sign-in message is tapped. It opens the Gaia screen.
  void OnOnlineSignInMessageTap();

  // Helper method to check if an auth method is enable. Use it like this:
  // bool has_tap = HasAuthMethod(AUTH_TAP).
  bool HasAuthMethod(AuthMethods auth_method) const;

  // Update UI for the online sign-in message.
  void DecorateOnlineSignInMessage();

  AuthMethods auth_methods_ = AUTH_NONE;
  LoginUserView* user_view_ = nullptr;
  LoginPasswordView* password_view_ = nullptr;
  LoginPinView* pin_view_ = nullptr;
  views::LabelButton* online_sign_in_message_ = nullptr;
  FingerprintView* fingerprint_view_ = nullptr;
  const OnAuthCallback on_auth_;
  const LoginUserView::OnTap on_tap_;

  // Animation state that was cached from before a layout. Generated by
  // |CaptureStateForAnimationPreLayout| and consumed by
  // |ApplyAnimationPostLayout|.
  std::unique_ptr<AnimationState> cached_animation_state_;

  base::WeakPtrFactory<LoginAuthUserView> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(LoginAuthUserView);
};

}  // namespace ash

#endif  // ASH_LOGIN_UI_LOGIN_AUTH_USER_VIEW_H_
