// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_ACCESSIBILITY_ACCESSIBILITY_MANAGER_H_
#define CHROME_BROWSER_CHROMEOS_ACCESSIBILITY_ACCESSIBILITY_MANAGER_H_

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ash/public/interfaces/accessibility_controller.mojom.h"
#include "ash/public/interfaces/accessibility_focus_ring_controller.mojom.h"
#include "base/callback_forward.h"
#include "base/callback_list.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observer.h"
#include "base/time/time.h"
#include "chrome/browser/chromeos/accessibility/chromevox_panel.h"
#include "chrome/browser/extensions/api/braille_display_private/braille_controller.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/session_manager/core/session_manager_observer.h"
#include "components/user_manager/user_manager.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "extensions/browser/event_router.h"
#include "extensions/browser/extension_system.h"
#include "ui/accessibility/ax_enums.mojom.h"
#include "ui/base/ime/chromeos/input_method_manager.h"

class Browser;
class DictationChromeos;
class Profile;

namespace gfx {
class Rect;
}  // namespace gfx

namespace chromeos {

class AccessibilityExtensionLoader;
class SelectToSpeakEventHandler;
class SwitchAccessEventHandler;

enum AccessibilityNotificationType {
  ACCESSIBILITY_MANAGER_SHUTDOWN,
  ACCESSIBILITY_TOGGLE_HIGH_CONTRAST_MODE,
  ACCESSIBILITY_TOGGLE_LARGE_CURSOR,
  ACCESSIBILITY_TOGGLE_STICKY_KEYS,
  ACCESSIBILITY_TOGGLE_SCREEN_MAGNIFIER,
  ACCESSIBILITY_TOGGLE_SPOKEN_FEEDBACK,
  ACCESSIBILITY_TOGGLE_SELECT_TO_SPEAK,
  ACCESSIBILITY_TOGGLE_VIRTUAL_KEYBOARD,
  ACCESSIBILITY_TOGGLE_MONO_AUDIO,
  ACCESSIBILITY_TOGGLE_CARET_HIGHLIGHT,
  ACCESSIBILITY_TOGGLE_CURSOR_HIGHLIGHT,
  ACCESSIBILITY_TOGGLE_FOCUS_HIGHLIGHT,
  ACCESSIBILITY_TOGGLE_DICTATION,
};

struct AccessibilityStatusEventDetails {
  AccessibilityStatusEventDetails(
      AccessibilityNotificationType notification_type,
      bool enabled);

  AccessibilityNotificationType notification_type;
  bool enabled;
};

typedef base::Callback<void(const AccessibilityStatusEventDetails&)>
    AccessibilityStatusCallback;

typedef base::CallbackList<void(const AccessibilityStatusEventDetails&)>
    AccessibilityStatusCallbackList;

typedef AccessibilityStatusCallbackList::Subscription
    AccessibilityStatusSubscription;

class ChromeVoxPanelWidgetObserver;

enum class PlaySoundOption {
  // The sound is always played.
  ALWAYS = 0,

  // The sound is played only if spoken feedback is enabled, and
  // --ash-disable-system-sounds is not set.
  ONLY_IF_SPOKEN_FEEDBACK_ENABLED,
};

// AccessibilityManager changes the statuses of accessibility features
// watching profile notifications and pref-changes.
// TODO(yoshiki): merge MagnificationManager with AccessibilityManager.
class AccessibilityManager
    : public content::NotificationObserver,
      public extensions::api::braille_display_private::BrailleObserver,
      public extensions::ExtensionRegistryObserver,
      public user_manager::UserManager::UserSessionStateObserver,
      public session_manager::SessionManagerObserver,
      public input_method::InputMethodManager::Observer {
 public:
  // Creates an instance of AccessibilityManager, this should be called once,
  // because only one instance should exist at the same time.
  static void Initialize();
  // Deletes the existing instance of AccessibilityManager.
  static void Shutdown();
  // Returns the existing instance. If there is no instance, returns NULL.
  static AccessibilityManager* Get();

  // Show the accessibility help as a tab in the browser.
  static void ShowAccessibilityHelp(Browser* browser);

  // Returns true when the accessibility menu should be shown.
  bool ShouldShowAccessibilityMenu();

  // Enables or disables the large cursor.
  void EnableLargeCursor(bool enabled);

  // Returns true if the large cursor is enabled, or false if not.
  bool IsLargeCursorEnabled() const;

  // Enables or disable Sticky Keys.
  void EnableStickyKeys(bool enabled);

  // Returns true if the Sticky Keys is enabled, or false if not.
  bool IsStickyKeysEnabled() const;

  // Enables or disables spoken feedback. Enabling spoken feedback installs the
  // ChromeVox component extension.
  void EnableSpokenFeedback(bool enabled);

  // Returns true if spoken feedback is enabled, or false if not.
  bool IsSpokenFeedbackEnabled() const;

  // Enables or disables the high contrast mode for Chrome.
  void EnableHighContrast(bool enabled);

  // Returns true if High Contrast is enabled, or false if not.
  bool IsHighContrastEnabled() const;

  // Enables or disables autoclick.
  void EnableAutoclick(bool enabled);

  // Returns true if autoclick is enabled.
  bool IsAutoclickEnabled() const;

  // Enables or disables the virtual keyboard.
  void EnableVirtualKeyboard(bool enabled);
  // Returns true if the virtual keyboard is enabled, otherwise false.
  bool IsVirtualKeyboardEnabled() const;

  // Enables or disables mono audio output.
  void EnableMonoAudio(bool enabled);
  // Returns true if mono audio output is enabled, otherwise false.
  bool IsMonoAudioEnabled() const;

  // Starts or stops darkening the screen.
  void SetDarkenScreen(bool darken);

  // Invoked to enable or disable caret highlighting.
  void SetCaretHighlightEnabled(bool enabled);

  // Returns if caret highlighting is enabled.
  bool IsCaretHighlightEnabled() const;

  // Invoked to enable or disable cursor highlighting.
  void SetCursorHighlightEnabled(bool enabled);

  // Returns if cursor highlighting is enabled.
  bool IsCursorHighlightEnabled() const;

  // Invoked to enable or disable focus highlighting.
  void SetFocusHighlightEnabled(bool enabled);

  // Returns if focus highlighting is enabled.
  bool IsFocusHighlightEnabled() const;

  // Enables or disables tap dragging.
  void EnableTapDragging(bool enabled);

  // Returns true if the tap dragging is enabled, or false if not.
  bool IsTapDraggingEnabled() const;

  // Invoked to enable or disable select-to-speak.
  void SetSelectToSpeakEnabled(bool enabled);

  // Returns if select-to-speak is enabled.
  bool IsSelectToSpeakEnabled() const;

  // Requests that the Select-to-Speak extension change its state.
  void RequestSelectToSpeakStateChange();

  // Called when the Select-to-Speak extension state has changed.
  void OnSelectToSpeakStateChanged(ash::mojom::SelectToSpeakState state);

  // Invoked to enable or disable switch access.
  void SetSwitchAccessEnabled(bool enabled);

  // Returns if switch access is enabled.
  bool IsSwitchAccessEnabled() const;

  // Returns true if a braille display is connected to the system, otherwise
  // false.
  bool IsBrailleDisplayConnected() const;

  // user_manager::UserManager::UserSessionStateObserver overrides:
  void ActiveUserChanged(const user_manager::User* active_user) override;

  // Initiates play of shutdown sound and returns it's duration.
  base::TimeDelta PlayShutdownSound();

  // Register a callback to be notified when the status of an accessibility
  // option changes.
  std::unique_ptr<AccessibilityStatusSubscription> RegisterCallback(
      const AccessibilityStatusCallback& cb);

  // Notify registered callbacks of a status change in an accessibility setting.
  void NotifyAccessibilityStatusChanged(
      const AccessibilityStatusEventDetails& details);

  // Notify accessibility when locale changes occur.
  void OnLocaleChanged();

  // Called when we first detect two fingers are held down, which can be
  // used to toggle spoken feedback on some touch-only devices.
  void OnTwoFingerTouchStart();

  // Called when the user is no longer holding down two fingers (including
  // releasing one, holding down three, or moving them).
  void OnTwoFingerTouchStop();

  // Whether or not to enable toggling spoken feedback via holding down
  // two fingers on the screen.
  bool ShouldToggleSpokenFeedbackViaTouch();

  // Play tick sound indicating spoken feedback will be toggled after countdown.
  bool PlaySpokenFeedbackToggleCountdown(int tick_count);

  // Notify that a view is focused in arc.
  void OnViewFocusedInArc(const gfx::Rect& bounds_in_screen);

  // Plays an earcon. Earcons are brief and distinctive sounds that indicate
  // the their mapped event has occurred. The |sound_key| enums can be found in
  // chromeos/audio/chromeos_sounds.h.
  bool PlayEarcon(int sound_key, PlaySoundOption option);

  // Forward an accessibility gesture from the touch exploration controller
  // to ChromeVox.
  void HandleAccessibilityGesture(ax::mojom::Gesture gesture);

  // Update the touch exploration controller so that synthesized
  // touch events are anchored at this point.
  void SetTouchAccessibilityAnchorPoint(const gfx::Point& anchor_point);

  // Called by our widget observer when the ChromeVoxPanel is closing.
  void OnChromeVoxPanelClosing();
  void OnChromeVoxPanelDestroying();

  // Profile having the a11y context.
  Profile* profile() { return profile_; }

  // Extension id of extension receiving keyboard events.
  void SetKeyboardListenerExtensionId(const std::string& id,
                                      content::BrowserContext* context);
  const std::string& keyboard_listener_extension_id() {
    return keyboard_listener_extension_id_;
  }

  // Whether keyboard listener extension gets to capture keys.
  void set_keyboard_listener_capture(bool val) {
    keyboard_listener_capture_ = val;
  }
  bool keyboard_listener_capture() { return keyboard_listener_capture_; }

  // Set the keys to be captured by Switch Access.
  void SetSwitchAccessKeys(const std::set<int>& key_codes);

  // Starts or stops dictation (type what you speak).
  bool ToggleDictation();

  // Sets the focus ring color.
  void SetFocusRingColor(SkColor color);

  // Resets the focus ring color back to the default.
  void ResetFocusRingColor();

  // Draws a focus ring around the given set of rects in screen coordinates. Use
  // |focus_ring_behavior| to specify whether the focus ring should persist or
  // fade out.
  void SetFocusRing(const std::vector<gfx::Rect>& rects_in_screen,
                    ash::mojom::FocusRingBehavior focus_ring_behavior);

  // Hides focus ring on screen.
  void HideFocusRing();

  // Draws a highlight at the given rects in screen coordinates. Rects may be
  // overlapping and will be merged into one layer. This looks similar to
  // selecting a region with the cursor, except it is drawn in the foreground
  // rather than behind a text layer.
  void SetHighlights(const std::vector<gfx::Rect>& rects_in_screen,
                     SkColor color);

  // Hides highlight on screen.
  void HideHighlights();

  // Test helpers:
  void SetProfileForTest(Profile* profile);
  static void SetBrailleControllerForTest(
      extensions::api::braille_display_private::BrailleController* controller);
  void FlushForTesting();
  void SetFocusRingObserverForTest(base::RepeatingCallback<void()> observer);
  void SetSelectToSpeakStateObserverForTest(
      base::RepeatingCallback<void()> observer);

 protected:
  AccessibilityManager();
  ~AccessibilityManager() override;

 private:
  void PostLoadChromeVox();
  void PostUnloadChromeVox();
  void PostSwitchChromeVoxProfile();
  void ReloadChromeVoxPanel();

  void PostUnloadSelectToSpeak();
  void UpdateAlwaysShowMenuFromPref();
  void OnLargeCursorChanged();
  void OnStickyKeysChanged();
  void OnSpokenFeedbackChanged();
  void OnHighContrastChanged();
  void OnVirtualKeyboardChanged();
  void OnMonoAudioChanged();
  void OnCaretHighlightChanged();
  void OnCursorHighlightChanged();
  void OnFocusHighlightChanged();
  void OnTapDraggingChanged();
  void OnSelectToSpeakChanged();
  void UpdateSwitchAccessFromPref();

  void CheckBrailleState();
  void ReceiveBrailleDisplayState(
      std::unique_ptr<extensions::api::braille_display_private::DisplayState>
          state);
  void UpdateBrailleImeState();

  void SetProfile(Profile* profile);

  void UpdateChromeOSAccessibilityHistograms();

  // content::NotificationObserver
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

  // extensions::api::braille_display_private::BrailleObserver implementation.
  // Enables spoken feedback if a braille display becomes available.
  void OnBrailleDisplayStateChanged(
      const extensions::api::braille_display_private::DisplayState&
          display_state) override;
  void OnBrailleKeyEvent(
      const extensions::api::braille_display_private::KeyEvent& event) override;

  // ExtensionRegistryObserver implementation.
  void OnExtensionUnloaded(content::BrowserContext* browser_context,
                           const extensions::Extension* extension,
                           extensions::UnloadedExtensionReason reason) override;
  void OnShutdown(extensions::ExtensionRegistry* registry) override;

  // InputMethodManager::Observer
  void InputMethodChanged(input_method::InputMethodManager* manager,
                          Profile* profile,
                          bool show_message) override;

  // session_manager::SessionManagerObserver
  void OnSessionStateChanged() override;

  // Profile which has the current a11y context.
  Profile* profile_;

  content::NotificationRegistrar notification_registrar_;
  std::unique_ptr<PrefChangeRegistrar> pref_change_registrar_;
  std::unique_ptr<PrefChangeRegistrar> local_state_pref_change_registrar_;
  std::unique_ptr<user_manager::ScopedUserSessionStateObserver>
      session_state_observer_;

  bool spoken_feedback_enabled_;
  bool select_to_speak_enabled_;
  bool switch_access_enabled_;

  AccessibilityStatusCallbackList callback_list_;

  bool braille_display_connected_;
  ScopedObserver<extensions::api::braille_display_private::BrailleController,
                 AccessibilityManager>
      scoped_braille_observer_;

  bool braille_ime_current_;

  ChromeVoxPanel* chromevox_panel_;
  std::unique_ptr<ChromeVoxPanelWidgetObserver>
      chromevox_panel_widget_observer_;

  std::string keyboard_listener_extension_id_;
  bool keyboard_listener_capture_;

  // Listen to extension unloaded notifications.
  ScopedObserver<extensions::ExtensionRegistry,
                 extensions::ExtensionRegistryObserver>
      extension_registry_observer_;

  std::unique_ptr<AccessibilityExtensionLoader> chromevox_loader_;

  std::unique_ptr<AccessibilityExtensionLoader> select_to_speak_loader_;

  std::unique_ptr<chromeos::SelectToSpeakEventHandler>
      select_to_speak_event_handler_;

  std::unique_ptr<AccessibilityExtensionLoader> switch_access_loader_;

  std::unique_ptr<chromeos::SwitchAccessEventHandler>
      switch_access_event_handler_;

  // Ash's mojom::AccessibilityController used to request Ash's a11y feature.
  ash::mojom::AccessibilityControllerPtr accessibility_controller_;

  // Ash's mojom::AccessibilityFocusRingController used to request Ash's a11y
  // focus ring feature.
  ash::mojom::AccessibilityFocusRingControllerPtr
      accessibility_focus_ring_controller_;

  bool app_terminating_ = false;

  std::unique_ptr<DictationChromeos> dictation_;

  base::RepeatingCallback<void()> focus_ring_observer_for_test_;

  base::RepeatingCallback<void()> select_to_speak_state_observer_for_test_;

  base::WeakPtrFactory<AccessibilityManager> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(AccessibilityManager);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_ACCESSIBILITY_ACCESSIBILITY_MANAGER_H_
